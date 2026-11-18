"""voltbench — control library for the Voltbench v2.1 bench supply.

    from voltbench import connect

    with connect("sim://") as psu:          # or "COM7"
        ch = psu.ch[1]
        ch.set(volts=5.00, amps=0.500)
        ch.on()
        ch.wait_until("i < 0.05", hold="30s", timeout="4h")
    # outputs safed here, exception or not

`sim://` runs tools/sim/vb_sim, which links the firmware's own PURE headers.
Tests against it exercise shipping logic, not a second description of it.
"""
from .device import Channel, PowerSupply, connect
from .model import (API_VERSION, ChannelState, ProtocolError, Status, Trip,
                    parse_duration, parse_meas, parse_status)
from .transport import (CommandRejected, LinkLost, SerialTransport, SimTransport,
                        Transport, connect_transport)

__all__ = [
    "connect", "PowerSupply", "Channel",
    "Status", "ChannelState", "Trip", "ProtocolError", "API_VERSION",
    "parse_status", "parse_meas", "parse_duration",
    "Transport", "SimTransport", "SerialTransport", "connect_transport",
    "CommandRejected", "LinkLost",
]
__version__ = "0.1.0"
