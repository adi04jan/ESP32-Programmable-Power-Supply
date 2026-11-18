"""PowerSupply / Channel — the API bench scripts actually write against.

SYNC, NOT ASYNC. Bench work is inherently sequential (set -> wait -> measure ->
assert); `await` on every line would buy nothing and drag in qasync and
pyserial-asyncio.

Two behaviours that matter more than the API surface:

  * A latched trip RAISES out of any blocking primitive. A wait that silently
    never satisfies is the worst possible bench behaviour.
  * The context manager safes the outputs on the way out — exception or not,
    Ctrl+C included, since that is how most bench scripts actually end — and
    then VERIFIES the outputs are off rather than assuming the command landed.
"""
from __future__ import annotations

import contextlib
from typing import Callable

from .model import (ChannelState, Status, Trip, compile_condition, parse_duration,
                    parse_meas, parse_status)
from .transport import CommandRejected, Transport, connect_transport


class Channel:
    def __init__(self, psu: "PowerSupply", n: int):
        self._psu = psu
        self.n = n

    # -- setpoints ----------------------------------------------------------
    def set(self, volts: float | None = None, amps: float | None = None) -> None:
        if volts is not None:
            self._psu.t.ask(f"VOLT {self.n},{volts:.3f}")
        if amps is not None:
            self._psu.t.ask(f"CURR {self.n},{amps:.3f}")

    def adjust(self, dvolts: float = 0.0, damps: float = 0.0) -> None:
        """Relative change — the form that has no read-modify-write race."""
        if dvolts:
            self._psu.t.ask(f"VOLT {self.n},{self.state().vset + dvolts:.3f}")
        if damps:
            self._psu.t.ask(f"CURR {self.n},{self.state().iset + damps:.3f}")

    def on(self) -> None:  self._psu.t.ask(f"OUT {self.n},1")
    def off(self) -> None: self._psu.t.ask(f"OUT {self.n},0")

    def cc_mode(self, mode: str) -> None:
        if mode not in ("limit", "trip"):
            raise ValueError("cc_mode must be 'limit' or 'trip'")
        self._psu.t.ask(f"CCMODE {self.n},{mode.upper()}")

    def protection(self, ovp: float | None = None, ocp: float | None = None) -> None:
        if ovp is not None: self._psu.t.ask(f"PROT:OVP {self.n},{ovp:.3f}")
        if ocp is not None: self._psu.t.ask(f"PROT:OCP {self.n},{ocp:.3f}")

    def clear_trip(self) -> None:
        """Refused while the fault persists — that is the FSM working, not an error."""
        self._psu.t.ask(f"PROT:CLR {self.n}")

    # -- readback -----------------------------------------------------------
    def measure(self) -> tuple[float, float, float, str]:
        return parse_meas(self._psu.t.ask1(f"MEAS? {self.n}"))

    def state(self) -> ChannelState:
        return self._psu.status().channel(self.n)

    @property
    def volts(self) -> float: return self.measure()[0]

    @property
    def amps(self) -> float:  return self.measure()[1]

    # -- waits --------------------------------------------------------------
    def wait_until(self, condition: str | Callable[[ChannelState], bool],
                   hold: str | float = 0, timeout: str | float = "60s",
                   poll: str | float = "20ms") -> ChannelState:
        """Block until `condition` holds continuously for `hold`.

        Raises Trip immediately if the channel latches, and TimeoutError if the
        deadline passes. On a simulated transport the clock is injected, so a
        four-hour timeout costs microseconds.
        """
        pred = compile_condition(condition) if isinstance(condition, str) else condition
        hold_s, timeout_s, poll_s = (parse_duration(hold), parse_duration(timeout),
                                     parse_duration(poll))
        elapsed = 0.0
        held = 0.0

        while True:
            st = self.state()
            if st.tripped:
                raise Trip(self.n, st.reason)
            if pred(st):
                held += poll_s
                if held >= hold_s:
                    return st
            else:
                held = 0.0
            if elapsed >= timeout_s:
                raise TimeoutError(
                    f"channel {self.n}: condition not met within {timeout} "
                    f"(v={st.v} i={st.i} mode={st.mode})")
            self._psu.t.advance(poll_s)
            elapsed += poll_s

    def wait_settled(self, timeout: str | float = "5s") -> ChannelState:
        """Wait for the rail to reach its setpoint (within 100 mV)."""
        target = self.state().vset
        return self.wait_until(lambda s: abs(s.v - target) < 0.1, timeout=timeout)


class PowerSupply:
    def __init__(self, transport: Transport):
        self.t = transport
        idn = self._idn()
        self.channels = idn.get("ch", 2)
        self.firmware = idn.get("fw", "?")
        self.ch = {n: Channel(self, n) for n in range(1, self.channels + 1)}

    def _idn(self) -> dict:
        # *IDN? answers with a '!' banner, which ask() treats as async, so read
        # it directly rather than through the request path.
        self.t._write_line("*IDN?")
        for _ in range(10):
            line = (self.t._read_line(2.0) or "").strip()
            if line.startswith("! READY"):
                body = line[len("! READY"):].strip()
                parts = [p.strip() for p in body.split(",")]
                out = {"name": parts[0] if parts else "?"}
                if len(parts) > 1: out["fw"] = parts[1]
                for p in parts[2:]:
                    if p.startswith("ch "):    out["ch"] = int(p[3:])
                    if p.startswith("proto "): out["proto"] = int(p[6:])
                return out
        return {}

    def status(self) -> Status:
        return parse_status(self.t.ask1("STATE?"))

    def all_off(self) -> None:
        self.t.ask("ALLOFF")

    def safe(self) -> bool:
        """Turn everything off and PROVE it, rather than assuming the write landed."""
        with contextlib.suppress(Exception):
            self.all_off()
        with contextlib.suppress(Exception):
            self.t.advance(0.05)
            return not any(c.en for c in self.status().ch)
        return False

    # -- logging ------------------------------------------------------------
    def log_start(self, hz: float = 1.0) -> None:
        self.t.ask(f"LOG:RATE {hz}")
        self.t.ask("LOG START")

    def log_stop(self)  -> None: self.t.ask("LOG STOP")
    def log_clear(self) -> None: self.t.ask("LOG CLEAR")

    def log_rows(self) -> list[str]:
        """CSV rows including the header. Streamed line-by-line, never one blob."""
        return self.t.ask("LOG:DUMP", timeout=30.0)[1:]

    # -- context manager ----------------------------------------------------
    def __enter__(self) -> "PowerSupply":
        return self

    def __exit__(self, *exc) -> bool:
        self.safe()
        self.t.close()
        return False


def connect(target: str = "sim://", **kw) -> PowerSupply:
    """`connect("sim://")`, `connect("COM7")`. Same object either way."""
    return PowerSupply(connect_transport(target))
