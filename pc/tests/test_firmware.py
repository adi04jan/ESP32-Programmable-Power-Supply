"""Firmware behaviour tests, run against the real firmware logic in vb_sim.

Each test here corresponds to a gate in the spec's §8 build order. They are the
part of bring-up that does NOT need the v2.1 board to exist — which means when
the hardware does arrive, a failure is evidence about the hardware rather than
one more suspect in a list of two.

Time is injected, so every debounce boundary is tested exactly: "did not fire
at 60 ms, did fire at 140 ms" is a real assertion here, not a flaky one.
"""
import pytest

from voltbench import Trip
from voltbench.transport import CommandRejected


# =============================================================================
#   Phase 1 — one channel: DAC, EN, measurement
# =============================================================================
def test_boots_with_outputs_off_and_dac_at_minimum_volts(psu):
    """The inverted CV mapping makes code 0 = 20.18 V, so a supply that boots
    without writing the DAC is a 20 V supply. Code must start at the maximum."""
    st = psu.status()
    assert all(not c.en for c in st.ch)
    cv, cc = psu.t.dac_codes(1)
    assert cv == 4095, f"CV code at boot is {cv}; anything lower is volts on the terminal"
    assert cc == 0

def test_output_follows_setpoint_in_cv(psu):
    psu.t.load(1, 100)
    psu.ch[1].set(volts=5.0, amps=1.0)
    psu.ch[1].on()
    psu.t.advance(0.4)
    v, i, w, mode = psu.ch[1].measure()
    assert mode == "CV"
    assert 4.9 <= v <= 5.1, f"expected ~5 V, got {v}"
    assert 0.045 <= i <= 0.055, f"5 V into 100 ohm should be ~50 mA, got {i}"
    assert w == pytest.approx(v * i, abs=0.01)

def test_disabled_output_produces_nothing(psu):
    psu.t.load(1, 100)
    psu.ch[1].set(volts=12.0)
    psu.t.advance(0.4)
    v, i, _, _ = psu.ch[1].measure()
    assert v < 0.1 and i < 0.01, "a DAC write with EN low must not produce output"

@pytest.mark.parametrize("volts", [1.0, 3.3, 5.0, 12.0, 19.0, 20.0])
def test_setpoint_accuracy_across_range(psu, volts):
    psu.t.load(1, 1000)
    psu.ch[1].set(volts=volts, amps=1.0)
    psu.ch[1].on()
    psu.t.advance(0.4)
    v = psu.ch[1].measure()[0]
    # One DAC step is 4.76 mV; 50 mV is the spec's Phase 2 calibration gate.
    assert abs(v - volts) < 0.05, f"set {volts} V, measured {v} V"


# =============================================================================
#   Clamping — the chain that stands between a typo and a dead DUT
# =============================================================================
def test_setpoint_above_maximum_is_clamped_not_obeyed(psu):
    psu.t.load(1, 1000)
    psu.ch[1].set(volts=25.0)
    psu.ch[1].on()
    psu.t.advance(0.4)
    assert psu.ch[1].measure()[0] <= 20.1

def test_ovp_acts_as_the_setpoint_ceiling(psu):
    """One concept, two jobs: set OVP to 3.6 V before probing an ESP32 board and
    no coarse-digit fumble can smoke it."""
    psu.t.load(1, 1000)
    psu.ch[1].protection(ovp=3.6)
    psu.ch[1].set(volts=12.0)
    psu.ch[1].on()
    psu.t.advance(0.4)
    v = psu.ch[1].measure()[0]
    assert v <= 3.7, f"OVP ceiling ignored — rail reached {v} V"

def test_below_spec_dac_region_is_never_commanded(psu):
    psu.ch[1].set(volts=0.5)          # below the 1.0 V floor
    psu.ch[1].on()
    psu.t.advance(0.2)
    cv, _ = psu.t.dac_codes(1)
    assert cv <= 4069, f"code {cv} is in the below-spec 4070-4095 region"


# =============================================================================
#   Phase 3 — CC detection
# =============================================================================
def test_short_circuit_enters_cc_and_holds_the_limit(psu):
    psu.ch[1].set(volts=10.0, amps=0.5)
    psu.ch[1].on()
    psu.t.advance(0.4)
    assert psu.ch[1].measure()[3] == "CV"

    psu.t.load(1, 1.0)                # 10 V / 1 ohm would be 10 A
    psu.t.advance(0.3)
    v, i, _, mode = psu.ch[1].measure()
    assert mode == "CC"
    assert 0.45 <= i <= 0.55, f"current limit not held: {i} A"
    assert v < 1.0, f"rail should fold back in CC, sat at {v} V"

def test_cc_entry_needs_both_current_and_collapsed_voltage(psu):
    """A rail happily delivering exactly its limit is NOT in CC. Only the
    collapsed voltage proves the analog loop took control."""
    psu.t.load(1, 10.0)               # 5 V / 10 ohm = 500 mA, exactly the limit
    psu.ch[1].set(volts=5.0, amps=0.5)
    psu.ch[1].on()
    psu.t.advance(0.5)
    v, i, _, mode = psu.ch[1].measure()
    assert v > 4.5, "voltage should not have collapsed"
    assert mode != "CC", "current at the limit alone must not read as CC"

def test_cc_exit_is_sticky_so_the_badge_cannot_flicker(psu):
    psu.ch[1].set(volts=5.0, amps=0.5)
    psu.ch[1].on()
    psu.t.load(1, 1.0)
    # 250 ms grace + 60 ms CC debounce = 310 ms before CC can possibly latch.
    psu.t.advance(0.4)
    assert psu.ch[1].measure()[3] == "CC"

    psu.t.load(1, 1000)               # load removed
    psu.t.advance(0.10)
    assert psu.ch[1].measure()[3] == "CC", "CC exited before its 200 ms debounce"
    psu.t.advance(0.20)
    assert psu.ch[1].measure()[3] == "CV"

def test_changing_channel_1_does_not_disturb_channel_2(psu):
    """The Phase 3 gate. Two channels sharing one DAC must stay independent."""
    psu.t.load(1, 100); psu.t.load(2, 100)
    psu.ch[1].set(volts=5.0, amps=1.0); psu.ch[1].on()
    psu.ch[2].set(volts=12.0, amps=1.0); psu.ch[2].on()
    psu.t.advance(0.5)
    before = psu.ch[2].measure()[0]

    for v in (3.0, 15.0, 1.0, 20.0):
        psu.ch[1].set(volts=v)
        psu.t.advance(0.1)
        assert abs(psu.ch[2].measure()[0] - before) < 0.05, "ch1 edit glitched ch2"


# =============================================================================
#   Phase 4 — protection FSM
# =============================================================================
def test_ocp_latches_and_survives_the_condition_clearing(psu):
    psu.ch[1].set(volts=5.0, amps=2.0)
    psu.ch[1].protection(ocp=0.6)
    psu.ch[1].on()
    psu.t.advance(0.4)
    psu.t.short(1)
    psu.t.advance(0.3)

    st = psu.ch[1].state()
    assert st.tripped and st.reason == "ocp", f"expected an OCP trip, got {st}"

    psu.t.load(1, 1000)               # fault removed
    psu.t.advance(1.0)
    assert psu.ch[1].state().tripped, "a latch that clears itself is not a latch"

def test_trip_drops_enable_and_forces_the_dac_safe(psu):
    """Belt and braces: a stuck EN net must not be able to hold 20 V."""
    psu.ch[1].set(volts=15.0, amps=2.0)
    psu.ch[1].protection(ocp=0.5)
    psu.ch[1].on()
    psu.t.advance(0.4)
    psu.t.short(1)
    psu.t.advance(0.3)

    assert psu.ch[1].state().tripped
    cv, cc = psu.t.dac_codes(1)
    assert cv == 4095 and cc == 0, f"trip left the DAC at cv={cv} cc={cc}"

def test_ocp_debounce_rides_out_integrator_windup(psu):
    """OCP detects that the ANALOG LOOP FAILED — it is not the limiter. Its
    100 ms debounce must clear the CC band and the windup overshoot, or every
    normal CC entry would nuisance-trip."""
    psu.ch[1].set(volts=10.0, amps=1.0)
    psu.ch[1].protection(ocp=2.048)   # default: limit + 150 mA governs
    psu.ch[1].on()
    psu.t.advance(0.4)
    psu.t.short(1)                    # legitimate CC entry, with overshoot
    psu.t.advance(0.5)
    st = psu.ch[1].state()
    assert not st.tripped, f"normal CC entry nuisance-tripped: {st.reason}"
    assert st.mode == "cc"

def test_cc_mode_trip_cuts_a_short_quickly(psu):
    """The Phase 4 gate: cc_mode=trip must cut a short in under 200 ms."""
    psu.ch[1].set(volts=10.0, amps=0.5)
    psu.ch[1].cc_mode("trip")
    psu.ch[1].on()
    psu.t.advance(0.4)

    psu.t.short(1)
    psu.t.advance(0.06)
    assert not psu.ch[1].state().tripped, "tripped before the CC debounce"
    psu.t.advance(0.15)
    st = psu.ch[1].state()
    assert st.tripped and st.reason == "cc_trip", f"expected cc_trip, got {st}"

def test_cc_mode_limit_does_not_trip_on_the_same_short(psu):
    """limit and trip differ by EXACTLY one edge. Same stimulus, other outcome."""
    psu.ch[1].set(volts=10.0, amps=0.5)
    psu.ch[1].cc_mode("limit")
    psu.ch[1].on()
    psu.t.advance(0.4)
    psu.t.short(1)
    psu.t.advance(1.0)
    st = psu.ch[1].state()
    assert not st.tripped and st.mode == "cc"

def test_clear_is_refused_while_the_fault_persists(psu):
    """The Phase 4 gate. A clear that relatches is just noise, so it is refused
    while the CONDITION is still true.

    Uses a hardware fault rather than OVP, because OVP is self-clearing by
    physics: once EN drops, the rail decays through the load within a few
    milliseconds, so 'v > ovp' stops being true almost immediately. See
    test_ovp_clears_once_the_rail_has_come_down for that path.
    """
    psu.ch[1].set(volts=12.0, amps=1.0)
    psu.ch[1].on()
    psu.t.advance(0.4)

    psu.t.hw_fault(True)
    psu.t.advance(0.2)
    assert psu.ch[1].state().reason == "hw"

    psu.ch[1].clear_trip()            # fault still asserted
    psu.t.advance(0.3)
    assert psu.ch[1].state().tripped, "cleared while the hardware fault persisted"

    psu.t.hw_fault(False)
    psu.ch[1].clear_trip()
    psu.t.advance(0.3)
    assert not psu.ch[1].state().tripped


def test_ovp_trips_and_clears_once_the_rail_has_come_down(psu):
    """OVP is ABSOLUTE, not tracking — a step-down legitimately sits high for
    seconds into a capacitive load and must not self-trip. It is also
    unreachable through a legal setpoint, so the rail has to be forced there."""
    psu.ch[1].set(volts=12.0, amps=1.0)
    psu.ch[1].on()
    psu.t.advance(0.4)

    psu.t.force_volts(1, 22.0)
    psu.t.advance(0.1)
    st = psu.ch[1].state()
    assert st.tripped and st.reason == "ovp"
    assert not st.en, "an OVP trip must drop the enable"

    psu.ch[1].clear_trip()            # rail has decayed through the load by now
    psu.t.advance(0.3)
    assert not psu.ch[1].state().tripped

def test_clearing_never_auto_re_enables(psu):
    """A latch that re-arms itself is not a latch. After a clear the output must
    stay OFF until someone asks for it again."""
    psu.ch[1].set(volts=5.0, amps=0.5)
    psu.ch[1].protection(ocp=0.3)
    psu.ch[1].on()
    psu.t.advance(0.4)
    psu.t.short(1)
    psu.t.advance(0.3)
    assert psu.ch[1].state().tripped

    psu.t.load(1, 1000)
    psu.ch[1].clear_trip()
    psu.t.advance(0.5)
    st = psu.ch[1].state()
    assert not st.tripped
    assert not st.en, "clearing a trip silently re-energised the output"
    assert psu.ch[1].measure()[0] < 0.1

def test_grace_window_suppresses_inrush(psu):
    """The Phase 4 gate: a capacitive load's inrush must not trip the supply."""
    psu.ch[1].set(volts=5.0, amps=0.5)
    psu.ch[1].protection(ocp=0.6)
    psu.t.short(1)                    # worst case: enable straight into a short
    psu.ch[1].on()
    psu.t.advance(0.15)               # inside the 250 ms grace window
    assert not psu.ch[1].state().tripped, "grace window failed to suppress inrush"

def test_otp_trips_and_respects_hysteresis(psu):
    psu.ch[1].set(volts=5.0, amps=0.5)
    psu.ch[1].on()
    psu.t.advance(0.4)
    psu.t.temperature(85)
    psu.t.advance(0.5)
    assert not psu.ch[1].state().tripped, "OTP fired before its 1 s debounce"
    psu.t.advance(0.7)
    assert psu.ch[1].state().reason == "otp"

    psu.t.temperature(78)             # inside the 5 C hysteresis band
    psu.ch[1].clear_trip()
    psu.t.advance(0.3)
    assert psu.ch[1].state().tripped, "OTP cleared while still too hot"

    psu.t.temperature(70)
    psu.ch[1].clear_trip()
    psu.t.advance(0.3)
    assert not psu.ch[1].state().tripped

def test_dead_sensor_latches_rather_than_running_blind(psu):
    """OVP and OCP both depend on the INA219. A channel that cannot measure
    itself must not stay enabled."""
    psu.ch[1].set(volts=5.0, amps=0.5)
    psu.ch[1].on()
    psu.t.advance(0.4)
    psu.t.sense_fail(1)
    psu.t.advance(0.3)
    st = psu.ch[1].state()
    assert st.tripped and st.reason == "sense"

def test_hardware_fault_trips_after_debounce(psu):
    psu.ch[1].set(volts=5.0, amps=0.5)
    psu.ch[1].on()
    psu.t.advance(0.4)
    psu.t.hw_fault(True)
    psu.t.advance(0.2)
    assert psu.ch[1].state().reason == "hw"


# =============================================================================
#   Protocol robustness (spec §4.3)
# =============================================================================
def test_bad_channel_is_rejected(psu):
    with pytest.raises(CommandRejected) as e:
        psu.t.ask("VOLT 3,5.0")
    assert e.value.code == "badch"

def test_overlong_line_is_rejected_whole_not_truncated(psu):
    """A clipped 'VOLT 1,20.0' is a different and possibly dangerous command."""
    with pytest.raises(CommandRejected) as e:
        psu.t.ask("VOLT 1,5.0" + " " * 120)
    assert e.value.code == "overflow"

def test_unknown_command_says_so(psu):
    with pytest.raises(CommandRejected) as e:
        psu.t.ask("FLUX 1,5")
    assert e.value.code == "unknown"

def test_commands_are_case_insensitive(psu):
    psu.t.ask("volt 1,7.5")
    assert psu.ch[1].state().vset == pytest.approx(7.5)


# =============================================================================
#   Phase 7 — logging
# =============================================================================
def test_log_records_at_the_configured_rate(psu):
    psu.log_clear()
    psu.log_start(hz=10)
    psu.ch[1].set(volts=5.0, amps=0.5)
    psu.ch[1].on()
    psu.t.advance(1.0)
    n = psu.status().log.n
    assert 8 <= n <= 12, f"10 Hz for 1 s gave {n} records"

def test_trip_is_always_logged_even_with_logging_stopped(psu):
    """Guarantees the CSV contains the trip point — far cheaper than a
    pre-trigger ring, and the one record you always want."""
    psu.log_clear()
    psu.log_stop()
    psu.ch[1].set(volts=5.0, amps=0.5)
    psu.ch[1].protection(ocp=0.3)
    psu.ch[1].on()
    psu.t.advance(0.4)
    assert psu.status().log.n == 0, "logging is stopped; nothing should be recorded"

    psu.t.short(1)
    psu.t.advance(0.3)
    assert psu.ch[1].state().tripped
    assert psu.status().log.n >= 1, "the trip point was not forced into the log"

def test_csv_dump_is_wellformed(psu):
    psu.log_clear()
    psu.log_start(hz=10)
    psu.t.load(1, 100)
    psu.ch[1].set(volts=5.0, amps=1.0)
    psu.ch[1].on()
    psu.t.advance(0.5)
    rows = psu.log_rows()
    assert rows[0].startswith("seq,t_ms,")
    assert len(rows) >= 4
    body = rows[1].split(",")
    assert len(body) == len(rows[0].split(",")), "row/header column mismatch"
    assert float(body[2]) == pytest.approx(5.0, abs=0.2)     # v1


# =============================================================================
#   Phase 8 — sequencer
# =============================================================================
def _push(psu, op, ch=0, a=0, b=0, timeout=0):
    psu.t.ask(f"SIM:SEQ {op},{ch},{a},{b},{timeout}")

SET, RAMP, WAIT, WAIT_UNTIL, OUTPUT, END = 0, 1, 2, 3, 4, 5

def test_sequence_runs_to_completion_and_safes_outputs(psu):
    """End of sequence turns outputs off BY DEFAULT — a sequence that ends
    silently leaving 20 V on the bench is how you kill a DUT."""
    psu.t.ask("SIM:SEQCLR")
    _push(psu, SET, 1, 5000, 500)
    _push(psu, OUTPUT, 1, 1)
    _push(psu, WAIT, 0, 200)
    _push(psu, END)
    psu.t.ask("SEQ RUN")
    psu.t.advance(0.1)
    assert psu.ch[1].state().en

    psu.t.advance(0.5)
    st = psu.status()
    assert st.seq.state == "done"
    assert not st.ch[0].en, "sequence ended without safing the outputs"

def test_trip_mid_sequence_kills_both_channels(psu):
    """In a two-rail test, leaving one rail up after the other dies back-feeds
    the DUT."""
    psu.t.ask("SIM:SEQCLR")
    _push(psu, SET, 1, 5000, 300)
    _push(psu, OUTPUT, 1, 1)
    _push(psu, WAIT, 0, 5000)
    _push(psu, END)

    psu.ch[2].set(volts=12.0, amps=1.0)
    psu.ch[2].on()
    # cc_mode=trip, not a low OCP: the analog loop holds current AT the limit,
    # so OCP (which sits above it) correctly never fires on a healthy short.
    psu.ch[1].cc_mode("trip")
    psu.t.ask("SEQ RUN")
    psu.t.advance(0.4)
    psu.t.short(1)
    psu.t.advance(0.5)

    st = psu.status()
    assert st.seq.state == "aborted"
    assert not st.ch[0].en and not st.ch[1].en, "the healthy channel was left live"

def test_ramp_is_monotonic_and_reaches_its_target(psu):
    psu.t.ask("SIM:SEQCLR")
    _push(psu, SET, 1, 2000, 1000)
    _push(psu, OUTPUT, 1, 1)
    _push(psu, RAMP, 1, 10000, 1000)
    _push(psu, WAIT, 0, 100)
    _push(psu, END)
    psu.t.load(1, 1000)
    psu.t.ask("SEQ RUN")

    psu.t.advance(0.2)
    seen = []
    for _ in range(12):
        psu.t.advance(0.1)
        # Stop at the END step, or the samples include the deliberate all-off.
        if psu.status().seq.state != "running":
            break
        seen.append(psu.ch[1].measure()[0])
    assert len(seen) >= 6, f"too few samples during the ramp: {seen}"
    assert seen == sorted(seen), f"ramp was not monotonic: {seen}"
    assert seen[-1] > 9.0, f"ramp did not reach its target: {seen[-1]}"
