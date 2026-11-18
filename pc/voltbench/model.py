"""Data model and parsers — THE contract between firmware and every client.

No I/O lives here, deliberately: this module is what the GUI, the CLI, the
tests, and the simulator all agree on, so it must be importable and testable
without a device, a socket, or a serial port in sight.

The status frame shape is frozen in spec §2.4. `api` is checked on parse so a
version skew surfaces as a clear error rather than as silently missing fields.
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from typing import Any

API_VERSION = 2


class ProtocolError(RuntimeError):
    """The device said something we cannot parse or did not expect."""


class Trip(RuntimeError):
    """A channel latched a protection trip.

    Raised out of any blocking primitive, because a wait that silently never
    satisfies is the worst possible bench behaviour.
    """

    def __init__(self, ch: int, reason: str):
        super().__init__(f"channel {ch} tripped: {reason}")
        self.ch = ch
        self.reason = reason


@dataclass
class ChannelState:
    en: bool = False
    v: float = 0.0
    i: float = 0.0
    w: float = 0.0
    vset: float = 0.0
    iset: float = 0.0
    mode: str = "cv"          # cv | cc
    prot: str = "ok"          # ok | trip
    reason: str = ""
    cc_mode: str = "limit"    # limit | trip
    ovp: float = 0.0
    ocp: float = 0.0
    sense: bool = True

    @property
    def tripped(self) -> bool:
        return self.prot == "trip"

    @property
    def in_cc(self) -> bool:
        return self.mode == "cc"


@dataclass
class SeqState:
    state: str = "idle"
    step: int = 0
    n: int = 0
    el: int = 0


@dataclass
class LogState:
    on: bool = False
    n: int = 0
    cap: int = 0


@dataclass
class Status:
    ts: int = 0
    up: int = 0
    api: int = API_VERSION
    ch: list[ChannelState] = field(default_factory=list)
    temp: float = 0.0
    temp_src: str = ""
    seq: SeqState = field(default_factory=SeqState)
    log: LogState = field(default_factory=LogState)
    cal: list[bool] = field(default_factory=list)

    def channel(self, n: int) -> ChannelState:
        """1-based, matching every other surface. Channel 0 does not exist."""
        if not 1 <= n <= len(self.ch):
            raise IndexError(f"channel {n} out of range (device has {len(self.ch)})")
        return self.ch[n - 1]


def parse_status(payload: str | dict[str, Any]) -> Status:
    """Parse a status frame from JSON text or an already-decoded dict."""
    if isinstance(payload, str):
        try:
            payload = json.loads(payload)
        except json.JSONDecodeError as e:
            raise ProtocolError(f"status frame is not JSON: {e}") from e
    if not isinstance(payload, dict):
        raise ProtocolError(f"status frame is {type(payload).__name__}, expected object")

    api = int(payload.get("api", 0))
    if api != API_VERSION:
        raise ProtocolError(
            f"device speaks status api {api}, this client speaks {API_VERSION}"
        )

    st = Status(ts=int(payload.get("ts", 0)), up=int(payload.get("up", 0)), api=api)

    for raw in payload.get("ch", []):
        st.ch.append(ChannelState(
            en=bool(raw.get("en", False)),
            v=float(raw.get("v", 0.0)),
            i=float(raw.get("i", 0.0)),
            w=float(raw.get("w", 0.0)),
            vset=float(raw.get("vset", 0.0)),
            iset=float(raw.get("iset", 0.0)),
            mode=str(raw.get("mode", "cv")),
            prot=str(raw.get("prot", "ok")),
            reason=str(raw.get("reason", "")),
            cc_mode=str(raw.get("cc_mode", "limit")),
            ovp=float(raw.get("ovp", 0.0)),
            ocp=float(raw.get("ocp", 0.0)),
            sense=bool(raw.get("sense", True)),
        ))

    st.temp = float(payload.get("temp", 0.0))
    st.temp_src = str(payload.get("temp_src", ""))

    sq = payload.get("seq", {})
    st.seq = SeqState(str(sq.get("state", "idle")), int(sq.get("step", 0)),
                      int(sq.get("n", 0)), int(sq.get("el", 0)))
    lg = payload.get("log", {})
    st.log = LogState(bool(lg.get("on", False)), int(lg.get("n", 0)), int(lg.get("cap", 0)))
    st.cal = [bool(x) for x in payload.get("cal", [])]
    return st


def parse_meas(line: str) -> tuple[float, float, float, str]:
    """`MEAS?` reply: v,i,w,mode as positional CSV."""
    parts = line.split(",")
    if len(parts) != 4:
        raise ProtocolError(f"MEAS? reply has {len(parts)} fields, expected 4: {line!r}")
    try:
        return float(parts[0]), float(parts[1]), float(parts[2]), parts[3].strip()
    except ValueError as e:
        raise ProtocolError(f"MEAS? reply not numeric: {line!r}") from e


_DURATION = re.compile(r"^\s*([0-9]*\.?[0-9]+)\s*(ms|s|m|h)?\s*$", re.I)
_UNITS = {"ms": 0.001, "s": 1.0, "m": 60.0, "h": 3600.0, None: 1.0}


def parse_duration(spec: str | float | int) -> float:
    """'30s' / '4h' / '250ms' / 2.5 -> seconds.

    Bench scripts are full of durations; making them strings keeps call sites
    readable and keeps '4h' from being mistaken for 4 seconds.
    """
    if isinstance(spec, (int, float)):
        return float(spec)
    m = _DURATION.match(spec)
    if not m:
        raise ValueError(f"cannot parse duration {spec!r}")
    return float(m.group(1)) * _UNITS[(m.group(2) or "").lower() or None]


_COND = re.compile(r"^\s*([viw])\s*(<=|>=|<|>)\s*(-?[0-9]*\.?[0-9]+)\s*$", re.I)


def compile_condition(expr: str):
    """'i < 0.05' -> predicate over a ChannelState.

    Deliberately tiny and closed: no eval, no arbitrary attribute access. A
    bench script's wait condition is not a place to execute strings.
    """
    m = _COND.match(expr)
    if not m:
        raise ValueError(f"cannot parse condition {expr!r} (want e.g. 'i < 0.05')")
    field_name, op, rhs_s = m.group(1).lower(), m.group(2), float(m.group(3))
    ops = {"<": lambda a, b: a < b, ">": lambda a, b: a > b,
           "<=": lambda a, b: a <= b, ">=": lambda a, b: a >= b}
    fn = ops[op]

    def pred(ch: ChannelState) -> bool:
        return fn(getattr(ch, field_name), rhs_s)

    pred.__doc__ = f"{field_name} {op} {rhs_s}"
    return pred
