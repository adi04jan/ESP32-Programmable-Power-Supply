"""Transports — how bytes reach a device, and nothing else.

Three implementations share one ABC:

  SimTransport      subprocess running tools/sim/vb_sim
  SerialTransport   pyserial, the §4.3 codec
  (net transport is a later phase; the API contract is identical)

THE INJECTABLE CLOCK. `Transport.advance(seconds)` is what a wait loop calls
instead of time.sleep. On real hardware it sleeps; on the simulator it sends
`SIM:TICK`, so a test written as timeout="4h" runs in microseconds and every
debounce boundary is exact rather than flaky. This is the single design choice
that makes the whole PC package testable before the hardware exists.

Line discipline (spec §4.3): the host DISCARDS any line without a leading
sigil, which makes boot-ROM chatter and mid-line truncation harmless, and on
`! READY` it clears its buffer and fails pending requests.
"""
from __future__ import annotations

import shutil
import subprocess
import time
from abc import ABC, abstractmethod
from pathlib import Path

from .model import ProtocolError

SIGILS = ("OK", "ERR", "!", "#", ">")


class LinkLost(RuntimeError):
    """The device stopped answering."""


class CommandRejected(RuntimeError):
    def __init__(self, code: str, text: str = ""):
        super().__init__(f"{code} {text}".strip())
        self.code = code
        self.text = text


class Transport(ABC):
    """One command in, its reply lines out."""

    simulated = False

    @abstractmethod
    def _write_line(self, line: str) -> None: ...

    @abstractmethod
    def _read_line(self, timeout: float) -> str | None: ...

    @abstractmethod
    def close(self) -> None: ...

    def advance(self, seconds: float) -> None:
        """Let `seconds` of device time pass. Overridden by the simulator."""
        time.sleep(seconds)

    # -- request/reply ------------------------------------------------------
    def ask(self, cmd: str, timeout: float = 5.0) -> list[str]:
        """Send one command; collect its reply.

        Returns payload lines: the text after `OK`, plus any `>` continuation
        rows. Raises CommandRejected on `ERR`.
        """
        self._write_line(cmd)
        payload: list[str] = []
        deadline = time.monotonic() + timeout

        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise LinkLost(f"no reply to {cmd!r} within {timeout}s")
            line = self._read_line(remaining)
            if line is None:
                continue
            line = line.rstrip("\r\n")
            if not line:
                continue

            head = line.split(" ", 1)[0]
            if head not in SIGILS and not line.startswith((">", "!", "#")):
                # Unsigiled noise: ROM chatter, a truncated line, terminal echo.
                continue

            if line.startswith("#"):
                continue                      # device log fan-out, not our reply
            if line.startswith("!"):
                if "READY" in line:
                    # A reboot mid-stream. Fail loudly rather than hang: any
                    # pending request is now unanswerable.
                    raise LinkLost("device rebooted mid-request (READY banner)")
                continue                      # async event, not our reply
            if line.startswith(">"):
                payload.append(line[1:].lstrip())
                continue
            if line.startswith("ERR"):
                parts = line.split(" ", 2)
                raise CommandRejected(parts[1] if len(parts) > 1 else "unknown",
                                      parts[2] if len(parts) > 2 else "")
            if line == "OK":
                return payload
            if line.startswith("OK "):
                payload.insert(0, line[3:])
                return payload
            raise ProtocolError(f"unclassifiable reply: {line!r}")

    def ask1(self, cmd: str, timeout: float = 5.0) -> str:
        lines = self.ask(cmd, timeout)
        if not lines:
            raise ProtocolError(f"{cmd!r} returned no payload")
        return lines[0]


class SimTransport(Transport):
    """The firmware's own logic, running as a subprocess.

    vb_sim links vb_cal.h / vb_protect.h / vb_seq.h / vb_log.h / vb_cmd.h — the
    real headers — so these tests exercise shipping code, not a Python
    re-description of it that would drift within a week.
    """

    simulated = True

    def __init__(self, binary: str | Path | None = None):
        self.binary = Path(binary) if binary else find_sim_binary()
        if not self.binary or not self.binary.exists():
            raise FileNotFoundError(
                "vb_sim not built — run: "
                "g++ -std=c++17 -O2 -o tools/sim/vb_sim tools/sim/vb_sim.cpp"
            )
        self.proc = subprocess.Popen(
            [str(self.binary)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            text=True, bufsize=1,
        )
        banner = self.proc.stdout.readline()
        if "READY" not in banner:
            raise ProtocolError(f"simulator did not greet: {banner!r}")

    def _write_line(self, line: str) -> None:
        if self.proc.poll() is not None:
            raise LinkLost("simulator exited")
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def _read_line(self, timeout: float) -> str | None:
        line = self.proc.stdout.readline()
        if line == "":
            raise LinkLost("simulator closed stdout")
        return line

    def advance(self, seconds: float) -> None:
        """Device time is injected, so this is exact and instant."""
        ms = int(round(seconds * 1000))
        if ms > 0:
            self.ask(f"SIM:TICK {ms}")

    # -- fault injection, simulator only ------------------------------------
    def load(self, ch: int, ohms: float) -> None: self.ask(f"SIM:LOAD {ch},{ohms}")
    def short(self, ch: int) -> None:             self.ask(f"SIM:SHORT {ch}")
    def sense_fail(self, ch: int, ok: bool = False) -> None:
        self.ask(f"SIM:SENSE {ch},{1 if ok else 0}")
    def temperature(self, celsius: float) -> None: self.ask(f"SIM:TEMP {celsius}")
    def hw_fault(self, on: bool = True) -> None:   self.ask(f"SIM:HW {1 if on else 0}")
    def force_volts(self, ch: int, volts: float) -> None:
        self.ask(f"SIM:FORCEV {ch},{volts * 1000}")
    def dac_codes(self, ch: int) -> tuple[int, int]:
        cv, cc = self.ask1(f"SIM:CODES? {ch}").split(",")
        return int(cv), int(cc)
    def reset(self) -> None: self.ask("SIM:RESET")

    def close(self) -> None:
        if self.proc.poll() is None:
            try:
                self.proc.stdin.close()
                self.proc.wait(timeout=2)
            except Exception:
                self.proc.kill()


class SerialTransport(Transport):
    """pyserial. Untested against hardware — the v2.1 board does not exist yet."""

    def __init__(self, port: str, baud: int = 115200, timeout: float = 1.0):
        import serial
        # NON-NEGOTIABLE: dtr/rts False BEFORE any write. Asserting either
        # RESETS the ESP32-C3 and drops the outputs mid-experiment.
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = timeout
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.ser.reset_input_buffer()
        self._write_line("")          # flush any partial line on the device side

    def _write_line(self, line: str) -> None:
        self.ser.write((line + "\n").encode("ascii", "replace"))
        self.ser.flush()

    def _read_line(self, timeout: float) -> str | None:
        self.ser.timeout = max(0.05, min(timeout, 1.0))
        raw = self.ser.readline()
        return raw.decode("ascii", "replace") if raw else None

    def close(self) -> None:
        try:
            self.ser.close()
        except Exception:
            pass


def find_sim_binary() -> Path | None:
    here = Path(__file__).resolve()
    for parent in here.parents:
        for name in ("vb_sim", "vb_sim.exe"):
            cand = parent / "tools" / "sim" / name
            if cand.exists():
                return cand
    found = shutil.which("vb_sim")
    return Path(found) if found else None


def connect_transport(target: str) -> Transport:
    if target.startswith("sim://"):
        return SimTransport(target[6:] or None)
    if target.upper().startswith("COM") or target.startswith("/dev/"):
        return SerialTransport(target)
    raise ValueError(f"don't know how to reach {target!r} (try 'sim://' or 'COM7')")
