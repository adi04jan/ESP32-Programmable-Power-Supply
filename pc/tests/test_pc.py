"""Tests for the PC library itself: the contract, the transport discipline, and
the device-level guarantees bench scripts rely on."""
import json

import pytest

from voltbench import ProtocolError, Trip, parse_duration, parse_meas, parse_status
from voltbench.model import compile_condition
from voltbench.transport import CommandRejected, LinkLost

# The exact frame frozen in spec §2.4. If the firmware ever changes shape, this
# is the test that should fail first.
SPEC_FRAME = json.dumps({
    "ts": 123456, "up": 940, "api": 2,
    "ch": [{"en": True, "v": 5.002, "i": 0.312, "w": 1.561, "vset": 5.0, "iset": 1.0,
            "mode": "cv", "prot": "ok", "reason": "", "cc_mode": "limit",
            "ovp": 21.0, "ocp": 2.048}],
    "temp": 41.5, "temp_src": "mcp9700",
    "seq": {"state": "running", "step": 3, "n": 8, "el": 1240},
    "log": {"on": True, "n": 812, "cap": 2048},
    "cal": [True, True],
})


class TestModel:
    def test_parses_the_frozen_spec_frame(self):
        st = parse_status(SPEC_FRAME)
        assert st.ts == 123456 and st.up == 940 and st.api == 2
        c = st.channel(1)
        assert c.en and c.v == 5.002 and c.iset == 1.0
        assert c.mode == "cv" and not c.tripped and not c.in_cc
        assert st.temp == 41.5 and st.temp_src == "mcp9700"
        assert st.seq.state == "running" and st.seq.step == 3
        assert st.log.on and st.log.n == 812 and st.log.cap == 2048
        assert st.cal == [True, True]

    def test_version_skew_is_loud_not_silent(self):
        """A device speaking a different frame shape must fail clearly rather
        than silently parse into a Status full of defaults."""
        with pytest.raises(ProtocolError, match="api"):
            parse_status(json.dumps({"api": 3, "ch": []}))

    def test_garbage_raises_protocol_error(self):
        with pytest.raises(ProtocolError):
            parse_status("not json at all")
        with pytest.raises(ProtocolError):
            parse_status("[1,2,3]")

    def test_channels_are_one_based(self):
        st = parse_status(SPEC_FRAME)
        with pytest.raises(IndexError):
            st.channel(0)
        with pytest.raises(IndexError):
            st.channel(9)

    def test_tripped_and_cc_properties(self):
        raw = json.loads(SPEC_FRAME)
        raw["ch"][0].update(prot="trip", reason="ocp", mode="cc")
        c = parse_status(json.dumps(raw)).channel(1)
        assert c.tripped and c.in_cc and c.reason == "ocp"

    @pytest.mark.parametrize("spec,seconds", [
        ("250ms", 0.25), ("30s", 30), ("2m", 120), ("4h", 14400),
        ("1.5s", 1.5), ("0.5h", 1800), (2.5, 2.5), (3, 3),
    ])
    def test_duration_parsing(self, spec, seconds):
        assert parse_duration(spec) == pytest.approx(seconds)

    def test_bad_duration_rejected(self):
        with pytest.raises(ValueError):
            parse_duration("soon")

    def test_meas_parsing(self):
        assert parse_meas("5.001,0.213,1.059,CV") == (5.001, 0.213, 1.059, "CV")
        with pytest.raises(ProtocolError):
            parse_meas("5.001,0.213")

    def test_conditions_do_not_eval_arbitrary_code(self):
        """A wait condition is not a place to execute strings."""
        with pytest.raises(ValueError):
            compile_condition("__import__('os').system('echo pwned')")
        with pytest.raises(ValueError):
            compile_condition("i < 0.05 or True")

    def test_condition_predicates(self):
        c = parse_status(SPEC_FRAME).channel(1)
        assert compile_condition("i < 0.5")(c)
        assert not compile_condition("i > 0.5")(c)
        assert compile_condition("v >= 5.0")(c)


class TestTransportDiscipline:
    def test_unsigiled_noise_is_discarded(self, psu):
        """Boot-ROM chatter must not be mistaken for a reply. The C3's ROM
        prints exactly this when DTR is low through a reset, so it is a line
        this host WILL see in the field, not a hypothetical."""
        psu.t.ask("SIM:NOISE (DOWNLOAD(USB/UART0/1))")
        # The stream must still be aligned afterwards.
        psu.ch[1].set(volts=4.5)
        assert psu.ch[1].state().vset == pytest.approx(4.5)

    def test_reboot_mid_request_fails_loudly_instead_of_hanging(self, psu):
        """On '! READY' the host clears its buffer and fails pending requests —
        a hang here would strand a bench script forever."""
        with pytest.raises(LinkLost, match="rebooted"):
            psu.t.ask("SIM:REBANNER")

    def test_err_becomes_a_typed_exception(self, psu):
        with pytest.raises(CommandRejected) as e:
            psu.t.ask("VOLT 9,1.0")
        assert e.value.code == "badch"

    def test_multiline_payload_is_collected_whole(self, psu):
        lines = psu.t.ask("HELP")
        assert len(lines) >= 1
        # The next command must still line up — proof the terminator was consumed.
        assert psu.ch[1].state().vset >= 0

    def test_reply_streams_stay_aligned_over_many_commands(self, psu):
        """One desynchronised reply shifts every later one, so this is the test
        that catches a protocol regression anywhere in the console."""
        for v in (1.5, 7.25, 19.0, 3.3):
            psu.ch[1].set(volts=v)
            assert psu.ch[1].state().vset == pytest.approx(v)
            psu.t.ask("MEAS?")            # multi-row reply in between
            assert psu.ch[1].state().vset == pytest.approx(v)


class TestDevice:
    def test_idn_reports_channels_and_firmware(self, psu):
        assert psu.channels == 2
        assert psu.firmware

    def test_wait_until_returns_when_condition_holds(self, psu):
        psu.t.load(1, 100)
        psu.ch[1].set(volts=5.0, amps=1.0)
        psu.ch[1].on()
        st = psu.ch[1].wait_until("v > 4.5", timeout="2s")
        assert st.v > 4.5

    def test_wait_until_honours_hold(self, psu):
        psu.t.load(1, 100)
        psu.ch[1].set(volts=5.0, amps=1.0)
        psu.ch[1].on()
        st = psu.ch[1].wait_until("v > 4.5", hold="500ms", timeout="5s")
        assert st.v > 4.5

    def test_wait_until_times_out_rather_than_hanging(self, psu):
        psu.t.load(1, 100)
        psu.ch[1].set(volts=5.0, amps=1.0)
        psu.ch[1].on()
        with pytest.raises(TimeoutError):
            psu.ch[1].wait_until("v > 50", timeout="1s")

    def test_a_four_hour_timeout_costs_nothing_on_the_simulator(self, psu):
        """The injectable clock is what makes long bench waits testable at all."""
        import time
        psu.t.load(1, 100)
        psu.ch[1].set(volts=5.0, amps=1.0)
        psu.ch[1].on()
        t0 = time.monotonic()
        with pytest.raises(TimeoutError):
            psu.ch[1].wait_until("v > 50", timeout="4h", poll="30s")
        assert time.monotonic() - t0 < 10, "wall-clock time was not decoupled"

    def test_trip_raises_out_of_a_blocking_wait(self, psu):
        """A wait that silently never satisfies is the worst possible bench
        behaviour, so a latch must surface as an exception."""
        psu.ch[1].set(volts=5.0, amps=0.5)
        psu.ch[1].cc_mode("trip")
        psu.ch[1].on()
        psu.t.advance(0.4)
        psu.t.short(1)
        with pytest.raises(Trip) as e:
            psu.ch[1].wait_until("i < 0.01", timeout="10s")
        assert e.value.ch == 1 and e.value.reason == "cc_trip"

    def test_context_manager_safes_outputs_on_exception(self, sim_binary):
        from voltbench import connect
        psu = connect(f"sim://{sim_binary}")
        try:
            with psu:
                psu.t.load(1, 100)
                psu.ch[1].set(volts=5.0, amps=1.0)
                psu.ch[1].on()
                psu.t.advance(0.3)
                assert psu.ch[1].state().en
                raise RuntimeError("bench script blew up")
        except RuntimeError:
            pass
        # The transport is closed, so re-open and confirm the outputs really are
        # off rather than trusting that the command was sent.
        psu2 = connect(f"sim://{sim_binary}")
        assert not any(c.en for c in psu2.status().ch)
        psu2.t.close()

    def test_safe_verifies_rather_than_assumes(self, psu):
        psu.t.load(1, 100)
        psu.ch[1].set(volts=5.0, amps=1.0)
        psu.ch[1].on()
        psu.ch[2].set(volts=12.0, amps=1.0)
        psu.ch[2].on()
        psu.t.advance(0.3)
        assert psu.safe() is True
        assert not any(c.en for c in psu.status().ch)

    def test_adjust_is_relative_to_the_authoritative_value(self, psu):
        psu.ch[1].set(volts=5.0)
        psu.ch[1].adjust(dvolts=0.01)
        assert psu.ch[1].state().vset == pytest.approx(5.01)
        psu.ch[1].adjust(dvolts=-0.01)
        assert psu.ch[1].state().vset == pytest.approx(5.0)


class TestReconnect:
    def test_dead_transport_reports_link_lost(self, psu):
        psu.t.proc.kill()
        psu.t.proc.wait()
        with pytest.raises(LinkLost):
            psu.status()
