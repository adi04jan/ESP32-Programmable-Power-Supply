// =============================================================================
//   Host tests for the Voltbench v2.1 PURE headers.
//
//   Build:  g++ -std=c++17 -Wall -Wextra -Werror -o tpv2 test_pure_v2.cpp && ./tpv2
//
//   -Werror is deliberate: it is what enforces the PURE constraint in practice.
//   If someone drags Arduino.h or a String into one of these headers, this
//   translation unit stops compiling and the protection FSM stops being
//   testable off-hardware — which is exactly when it would start rotting.
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../Voltbench_v2/vb_cal.h"
#include "../../Voltbench_v2/vb_protect.h"
#include "../../Voltbench_v2/vb_cmd.h"
#include "../../Voltbench_v2/vb_log.h"
#include "../../Voltbench_v2/vb_seq.h"
#include "../../Voltbench_v2/vb_ui_logic.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

// -----------------------------------------------------------------------------
static void test_cal() {
  ChanCal c; cal_defaults(&c);
  CHECK(cal_plausible(&c), "nominal cal must pass its own bounds check");

  // The inverted mapping: code 0 is TWENTY VOLTS. If this ever reads otherwise,
  // stop and re-read the hardware doc before touching firmware.
  CHECK(cal_cv_mV(&c, 0) == 20180, "code 0 = %d mV, expected 20180", cal_cv_mV(&c, 0));
  CHECK(cal_cv_mV(&c, 4069) < 900, "code 4069 should be near the floor, got %d", cal_cv_mV(&c, 4069));

  uint16_t code; int32_t mV;
  CHECK(cal_cv_code(&c, 5000, 1000, 20000, &code, &mV) == CVC_OK, "5 V should be exact");
  CHECK(mV > 4995 && mV < 5005, "5 V round-trip landed at %d mV", mV);

  // Round-trip every 100 mV across the range: quantisation must stay inside
  // half a step (4.763 mV), never accumulate.
  for (int32_t want = 1000; want <= 20000; want += 100) {
    cal_cv_code(&c, want, 1000, 20000, &code, &mV);
    int32_t err = mV - want; if (err < 0) err = -err;
    CHECK(err <= 3, "round-trip at %d mV was off by %d", want, err);
  }

  // Clamping, in the order the spec fixes it.
  CHECK(cal_cv_code(&c, 25000, 1000, 20000, &code, &mV) == CVC_CLAMPED, "over vmax must clamp");
  CHECK(mV <= 20005, "clamped high landed at %d mV", mV);
  CHECK(cal_cv_code(&c, 100, 1000, 20000, &code, &mV) == CVC_CLAMPED, "under vmin must clamp");
  CHECK(mV >= 995 && mV <= 1010, "clamped low landed at %d mV", mV);
  CHECK(code <= 4069, "code %u escaped the below-spec guard", code);

  // A user soft ceiling (OVP doubling as the setpoint ceiling) must bind.
  cal_cv_code(&c, 12000, 1000, 3600, &code, &mV);
  CHECK(mV <= 3610, "soft ceiling ignored: got %d mV", mV);

  // Assert-back: a broken slope must be REFUSED, not silently obeyed. This is
  // the check that stands between a corrupt NVS blob and 20 V on a DUT.
  ChanCal bad; cal_defaults(&bad);
  bad.cv_slope_uV = 4763.0f * 4;     // pretends each code moves 4x as much
  CvCodeResult r = cal_cv_code(&bad, 2000, 1000, 20000, &code, &mV);
  CHECK(r == CVC_REFUSED || mV <= 2250, "implausible slope produced %d mV (r=%d)", mV, (int)r);

  // CC mapping: 0.5 mA/LSB.
  CHECK(cal_cc_code(&c, 1000, 2048, &code, &mV) == CVC_OK && code == 2000,
        "1 A should be code 2000, got %u", code);
  CHECK(cal_cc_code(&c, 5000, 2048, &code, &mV) == CVC_CLAMPED, "over imax must clamp");

  // Two-point fit: accept a good pair...
  ChanCal fit; cal_defaults(&fit);
  CHECK(cal_fit_cv(3800, 2100, 1700, 12100, &fit) == 1, "plausible fit was rejected");
  CHECK(fit.cv_slope_uV > 4700 && fit.cv_slope_uV < 4820, "fit slope %f", (double)fit.cv_slope_uV);
  CHECK(fit.cv_v0_mV   > 20100 && fit.cv_v0_mV   < 20300, "fit v0 %f", (double)fit.cv_v0_mV);

  // ...and reject the ways a cal run goes wrong.
  ChanCal keep; cal_defaults(&keep); ChanCal before = keep;
  CHECK(cal_fit_cv(3800, 2100, 3700, 2600, &keep) == 0, "too-narrow span must be rejected");
  CHECK(cal_fit_cv(3800, 2100, 1700, 2200, &keep) == 0, "absurd slope must be rejected");
  CHECK(cal_fit_cv(3800, 9000, 1700, 12100, &keep) == 0, "out-of-bounds v0 must be rejected");
  CHECK(memcmp(&keep, &before, sizeof(keep)) == 0, "a rejected fit must not mutate the cal");

  // Phantom divider subtraction (spec §9.4).
  CHECK(cal_subtract_phantom(500, 20000) == 500 - 20000 / 107, "phantom subtraction wrong");
  CHECK(cal_subtract_phantom(10, 20000) == 0, "phantom must not drive current negative");
}

// -----------------------------------------------------------------------------
static void prot_defaults(ProtIn *in) {
  memset(in, 0, sizeof(*in));
  in->vset_mV = 5000; in->iset_mA = 1000;
  in->v_mV    = 5000; in->i_mA    = 100;
  in->ovp_mV  = 21000; in->ocp_mA = 2048;
  in->otp_c   = 80;    in->temp_c10 = 250;
  in->en_cmd  = 1;
}

static void run(ProtCtx *c, ProtIn *in, ProtOut *out, int ms) {
  for (int t = 0; t < ms; t += 20) prot_tick(c, in, 20, out);
}

static void test_protect() {
  ProtCtx c; ProtOut o; ProtIn in;
  prot_init(&c); prot_defaults(&in);

  // OFF -> STARTING -> CV, with the grace window actually elapsing.
  prot_tick(&c, &in, 20, &o);
  CHECK(c.state == PS_STARTING, "en_cmd should start the channel, state=%d", (int)c.state);
  CHECK(o.en_out == 1, "STARTING must drive EN high");
  run(&c, &in, &o, 200);
  CHECK(c.state == PS_STARTING, "grace window ended early");
  run(&c, &in, &o, 100);
  CHECK(c.state == PS_CV, "should reach CV after 250 ms, state=%d", (int)c.state);

  // Inrush during the grace window must NOT trip.
  prot_init(&c); prot_defaults(&in);
  in.i_mA = 3000;                                  // way over OCP
  run(&c, &in, &o, 200);
  CHECK(c.state == PS_STARTING, "grace window failed to suppress inrush");

  // OCP: debounced at 100 ms, and it is a LATCH.
  prot_init(&c); prot_defaults(&in);
  run(&c, &in, &o, 300);
  in.i_mA = 3000;
  run(&c, &in, &o, 60);
  CHECK(c.state == PS_CV, "OCP fired before its 100 ms debounce");
  run(&c, &in, &o, 80);
  CHECK(c.state == PS_TRIPPED && c.reason == PR_OCP, "OCP should have latched");
  CHECK(o.en_out == 0, "a trip must drop EN");
  CHECK(o.force_dac_safe == 1, "a trip must also force the DAC safe");

  // The latch must not re-arm itself while en_cmd is still asserted.
  CHECK(o.clear_en_cmd == 1, "entering TRIPPED must clear the caller's en_cmd");
  in.i_mA = 100;
  run(&c, &in, &o, 1000);
  CHECK(c.state == PS_TRIPPED, "TRIPPED must be sticky without an explicit clear");

  // Clear -> CLEARING -> OFF, and OFF must not silently restart.
  in.clear_req = 1;
  prot_tick(&c, &in, 20, &o);
  CHECK(c.state == PS_CLEARING, "clear_req should start CLEARING");
  in.clear_req = 0;
  in.en_cmd    = 0;                                // caller honoured clear_en_cmd
  run(&c, &in, &o, 250);
  CHECK(c.state == PS_OFF, "CLEARING should reach OFF, state=%d", (int)c.state);

  // OVP is absolute, debounced 20 ms.
  prot_init(&c); prot_defaults(&in);
  run(&c, &in, &o, 300);
  in.v_mV = 21500;
  prot_tick(&c, &in, 10, &o);
  CHECK(c.state == PS_CV, "OVP fired before its debounce");
  prot_tick(&c, &in, 20, &o);
  CHECK(c.state == PS_TRIPPED && c.reason == PR_OVP, "OVP should have latched");
  // ...and clearing is refused while the rail is still high.
  in.clear_req = 1;
  prot_tick(&c, &in, 20, &o);
  CHECK(c.state == PS_TRIPPED, "clear must be refused while OVP condition persists");
  in.v_mV = 5000;
  prot_tick(&c, &in, 20, &o);
  CHECK(c.state == PS_CLEARING, "clear should succeed once the rail came down");

  // CC entry needs BOTH halves of the predicate.
  prot_init(&c); prot_defaults(&in);
  run(&c, &in, &o, 300);
  in.i_mA = 1000;                                  // at the limit...
  in.v_mV = 5000;                                  // ...but voltage has not collapsed
  run(&c, &in, &o, 200);
  CHECK(c.state == PS_CV, "current alone must not be read as CC");
  in.v_mV = 4000;                                  // now it has
  run(&c, &in, &o, 40);
  CHECK(c.state == PS_CV, "CC entry fired before its 60 ms debounce");
  run(&c, &in, &o, 60);
  CHECK(c.state == PS_CC && c.cc_badge == 1, "CC entry failed, state=%d", (int)c.state);

  // CC exit is asymmetric and sticky-on, so the badge cannot flicker.
  in.i_mA = 100; in.v_mV = 5000;
  run(&c, &in, &o, 100);
  CHECK(c.state == PS_CC, "CC exited too eagerly (badge would flicker)");
  run(&c, &in, &o, 150);
  CHECK(c.state == PS_CV && c.cc_badge == 0, "CC should have exited after 200 ms");

  // cc_mode=trip differs from limit by exactly one edge.
  prot_init(&c); prot_defaults(&in);
  in.cc_mode = CCM_TRIP;
  run(&c, &in, &o, 300);
  in.i_mA = 1000; in.v_mV = 4000;
  run(&c, &in, &o, 300);
  CHECK(c.state == PS_TRIPPED && c.reason == PR_CC_TRIP, "cc_mode=trip should latch");

  // OTP: slow, 1 s, and clearing respects the 5 C hysteresis.
  prot_init(&c); prot_defaults(&in);
  run(&c, &in, &o, 300);
  in.temp_c10 = 850;
  run(&c, &in, &o, 500);
  CHECK(c.state == PS_CV, "OTP fired before its 1 s debounce");
  run(&c, &in, &o, 600);
  CHECK(c.state == PS_TRIPPED && c.reason == PR_OTP, "OTP should have latched");
  in.temp_c10 = 780; in.clear_req = 1;             // 78 C — inside the hysteresis band
  prot_tick(&c, &in, 20, &o);
  CHECK(c.state == PS_TRIPPED, "OTP clear must respect the 5 C hysteresis");
  in.temp_c10 = 700;
  prot_tick(&c, &in, 20, &o);
  CHECK(c.state == PS_CLEARING, "OTP should clear once cool enough");

  // HW fault needs 3 consecutive ticks and outranks everything.
  prot_init(&c); prot_defaults(&in);
  run(&c, &in, &o, 300);
  in.hw_fault = 1;
  prot_tick(&c, &in, 20, &o); prot_tick(&c, &in, 20, &o);
  CHECK(c.state == PS_CV, "HW fault tripped before 3 ticks");
  prot_tick(&c, &in, 20, &o);
  CHECK(c.state == PS_TRIPPED && c.reason == PR_HW, "HW fault should have latched");

  // Dropping en_cmd from a live state is immediate.
  prot_init(&c); prot_defaults(&in);
  run(&c, &in, &o, 300);
  in.en_cmd = 0;
  prot_tick(&c, &in, 20, &o);
  CHECK(c.state == PS_OFF && o.en_out == 0, "en_cmd=0 must stop the channel at once");

  // Regulation warning is log-only — it must never become a trip.
  prot_init(&c); prot_defaults(&in);
  run(&c, &in, &o, 300);
  in.v_mV = 5000 + 2000;
  run(&c, &in, &o, 4000);
  CHECK(c.state != PS_TRIPPED, "regulation warning must not trip");

  // OCP threshold is min(ocp, limit+150).
  ProtIn t; prot_defaults(&t);
  t.iset_mA = 500; t.ocp_mA = 2048;
  CHECK(prot_ocp_threshold(&t) == 650, "OCP threshold = %d, expected 650", prot_ocp_threshold(&t));
  t.ocp_mA = 600;
  CHECK(prot_ocp_threshold(&t) == 600, "explicit OCP below limit+150 must win");
}

// -----------------------------------------------------------------------------
static void test_cmd() {
  // The two PURE headers keep their CC-mode encodings in sync by assertion,
  // not by inclusion.
  CHECK((int)CCM_LIMIT == CCM_LIMIT_VAL && (int)CCM_TRIP == CCM_TRIP_VAL,
        "CcMode and CCM_*_VAL have drifted apart");

  int32_t v;
  CHECK(vb_parse_milli("5", &v)      && v == 5000,  "parse '5' -> %d", v);
  CHECK(vb_parse_milli("5.001", &v)  && v == 5001,  "parse '5.001' -> %d", v);
  CHECK(vb_parse_milli("0.5", &v)    && v == 500,   "parse '0.5' -> %d", v);
  CHECK(vb_parse_milli("-1.25", &v)  && v == -1250, "parse '-1.25' -> %d", v);
  // Truncation, not rounding: a setpoint that rounds UP on parse can exceed a
  // ceiling the operator typed.
  CHECK(vb_parse_milli("5.0009", &v) && v == 5000,  "parse '5.0009' -> %d", v);
  CHECK(!vb_parse_milli("5.0abc", &v), "trailing garbage must be rejected");
  CHECK(!vb_parse_milli("", &v),       "empty must be rejected");
  CHECK(!vb_parse_milli("abc", &v),    "non-numeric must be rejected");
  CHECK(!vb_parse_milli("-", &v),      "lone sign must be rejected");

  ParsedLine p;
  vb_parse_line("VOLT 1,5.0", SRC_SERIAL, &p);
  CHECK(p.kind == CK_APPLY && p.cmd.op == OP_SET_V && p.cmd.ch == 1 && p.cmd.arg == 5000,
        "VOLT parse failed (kind=%d arg=%d)", (int)p.kind, p.cmd.arg);
  CHECK(p.cmd.src == SRC_SERIAL, "source stamp lost");

  vb_parse_line("volt 2,12.345", SRC_TELNET, &p);
  CHECK(p.kind == CK_APPLY && p.cmd.ch == 2 && p.cmd.arg == 12345, "case-insensitivity broken");

  vb_parse_line("CURR 1,0.5", SRC_SERIAL, &p);
  CHECK(p.kind == CK_APPLY && p.cmd.op == OP_SET_I && p.cmd.arg == 500, "CURR parse failed");

  vb_parse_line("OUT 1,1", SRC_SERIAL, &p);
  CHECK(p.kind == CK_APPLY && p.cmd.op == OP_ENABLE && p.cmd.arg == 1, "OUT parse failed");

  vb_parse_line("OUT 1,2", SRC_SERIAL, &p);
  CHECK(p.kind == CK_ERR, "OUT with a non-boolean must be rejected");

  vb_parse_line("VOLT 3,5.0", SRC_SERIAL, &p);
  CHECK(p.kind == CK_ERR && !strcmp(p.err, "badch"), "channel 3 must be rejected");

  vb_parse_line("MEAS?", SRC_SERIAL, &p);
  CHECK(p.kind == CK_MEAS && p.ch == 0, "bare MEAS? should mean all channels");
  vb_parse_line("MEAS? 2", SRC_SERIAL, &p);
  CHECK(p.kind == CK_MEAS && p.ch == 2, "MEAS? 2 failed");

  vb_parse_line("*IDN?", SRC_SERIAL, &p);   CHECK(p.kind == CK_IDN,   "*IDN? failed");
  vb_parse_line("STATE?", SRC_SERIAL, &p);  CHECK(p.kind == CK_STATE, "STATE? failed");
  vb_parse_line("", SRC_SERIAL, &p);        CHECK(p.kind == CK_NONE,  "blank should be silent");
  vb_parse_line("   ", SRC_SERIAL, &p);     CHECK(p.kind == CK_NONE,  "whitespace should be silent");

  vb_parse_line("CCMODE 1,TRIP", SRC_SERIAL, &p);
  CHECK(p.kind == CK_APPLY && p.cmd.op == OP_CC_MODE && p.cmd.arg == CCM_TRIP_VAL, "CCMODE failed");

  vb_parse_line("PROT:CLR 2", SRC_SERIAL, &p);
  CHECK(p.kind == CK_APPLY && p.cmd.op == OP_CLEAR_TRIP && p.cmd.ch == 2, "PROT:CLR failed");
  vb_parse_line("PROT:OVP 1,21.0", SRC_SERIAL, &p);
  CHECK(p.kind == CK_APPLY && p.cmd.op == OP_SET_OVP && p.cmd.arg == 21000, "PROT:OVP failed");

  vb_parse_line("LOG START", SRC_SERIAL, &p);
  CHECK(p.kind == CK_APPLY && p.cmd.op == OP_LOG && p.cmd.arg == LOGOP_START, "LOG START failed");
  vb_parse_line("LOG:RATE 0.1", SRC_SERIAL, &p);
  CHECK(p.kind == CK_APPLY && p.cmd.arg == LOGOP_RATE && p.cmd.arg2 == 1, "LOG:RATE 0.1 failed");
  vb_parse_line("LOG:RATE 50", SRC_SERIAL, &p);
  CHECK(p.kind == CK_ERR, "50 Hz must be rejected — it aliases the ripple into garbage");

  vb_parse_line("CAL:PT 1,CV,5.002", SRC_SERIAL, &p);
  CHECK(p.kind == CK_APPLY && p.cmd.op == OP_CAL_PT && p.cmd.arg == CALPT_CV
        && p.cmd.arg2 == 5002, "CAL:PT failed");

  vb_parse_line("NONSENSE 1", SRC_SERIAL, &p);
  CHECK(p.kind == CK_ERR && !strcmp(p.err, "unknown"), "unknown verb should say so");

  // A line over 96 bytes must be rejected WHOLE — never truncated into a
  // different, valid, and possibly dangerous command.
  char big[200];
  memset(big, 'A', sizeof(big)); big[sizeof(big) - 1] = 0;
  vb_parse_line(big, SRC_SERIAL, &p);
  CHECK(p.kind == CK_ERR && !strcmp(p.err, "overflow"), "over-long line must report overflow");

  char sneaky[120];
  snprintf(sneaky, sizeof(sneaky), "VOLT 1,20.0%*s", 100, "");
  vb_parse_line(sneaky, SRC_SERIAL, &p);
  CHECK(p.kind == CK_ERR, "an over-long line must not be truncated into a valid command");
}

// -----------------------------------------------------------------------------
static void test_log() {
  static LogRec storage[64];
  LogRing L; log_init(&L, storage, 64);

  uint16_t v[2] = {5000, 12000};
  uint16_t i[2] = {log_pack_i(250, 1, 0, 0), log_pack_i(100, 1, 1, 0)};

  CHECK(log_append(&L, 1000, v, i, 0) == 0, "must not log while stopped");
  L.running = 1;
  CHECK(log_append(&L, 1000, v, i, 0) == 1, "first record should be accepted");
  CHECK(log_append(&L, 1100, v, i, 0) == 0, "1 Hz rate limiter should reject +100 ms");
  CHECK(log_append(&L, 2000, v, i, 0) == 1, "+1000 ms should be accepted");

  // The trip record bypasses both the rate limiter and the running flag —
  // that is what guarantees the CSV always contains the trip point.
  L.running = 0;
  CHECK(log_append(&L, 2010, v, i, 1) == 1, "forced record must ignore rate and run state");

  // Wrap: oldest records are dropped, count saturates at cap. Step the clock by
  // the full 10 Hz period, or the rate limiter (correctly) drops most of these.
  L.running = 1; L.rate_hz10 = 100;              // 10.0 Hz -> 100 ms period
  for (int k = 0; k < 200; k++) log_append(&L, 3000 + k * 100, v, i, 0);
  CHECK(log_count(&L) == 64, "count should saturate at cap, got %lu", (unsigned long)log_count(&L));
  CHECK(log_at(&L, 0) == 0, "an overwritten index must return null, not stale data");
  CHECK(log_at(&L, L.head - 1) != 0, "newest record must be readable");

  char line[160];
  int n = log_csv_row(&L, L.head - 1, line, sizeof(line));
  CHECK(n > 0 && n < (int)sizeof(line), "csv row length %d", n);
  CHECK(strchr(line, '\n') != 0, "csv row must end with a newline");
  // Power derived, not stored: 5.000 V * 0.250 A = 1.250 W
  CHECK(strstr(line, "1.250") != 0, "derived power missing from row: %s", line);

  n = log_csv_header(line, sizeof(line));
  CHECK(n > 0 && strstr(line, "seq,") == line, "csv header must start with the seq column");
}

// -----------------------------------------------------------------------------
static void test_seq() {
  SeqStep s[8];
  memset(s, 0, sizeof(s));
  uint16_t bad = 0;

  s[0].op = SEQ_SET;    s[0].ch = 1; s[0].a = 5000; s[0].b = 500;
  s[1].op = SEQ_OUTPUT; s[1].ch = 1; s[1].a = 1;
  s[2].op = SEQ_WAIT;   s[2].a = 100;
  s[3].op = SEQ_END;
  CHECK(seq_validate(s, 4, 1000, 20000, 2048, &bad) == 0, "valid sequence rejected");

  s[0].a = 25000;
  const char *why = seq_validate(s, 4, 1000, 20000, 2048, &bad);
  CHECK(why != 0 && bad == 0, "out-of-range v should fail at index 0 (got idx %u)", bad);
  s[0].a = 5000;

  s[2].op = SEQ_WAIT_UNTIL; s[2].ch = 1; s[2].cond = SEQC_I_BELOW; s[2].b = 50; s[2].timeout_ms = 0;
  why = seq_validate(s, 4, 1000, 20000, 2048, &bad);
  CHECK(why != 0 && bad == 2, "WAIT_UNTIL without a timeout must be rejected");
  s[2].timeout_ms = 1000;
  CHECK(seq_validate(s, 4, 1000, 20000, 2048, &bad) == 0, "WAIT_UNTIL with timeout should pass");

  // Execution: SET then OUTPUT then wait-until, satisfied.
  SeqCtx c; SeqAction a; SeqWorld w;
  memset(&w, 0, sizeof(w));
  w.v_mV[0] = 5000; w.i_mA[0] = 500;
  seq_init(&c); seq_start(&c, 4);

  seq_tick(&c, s, &w, 20, &a);
  CHECK(a.do_set && a.ch == 1 && a.v_mV == 5000 && a.i_mA == 500, "SET step did not emit");
  seq_tick(&c, s, &w, 20, &a);
  CHECK(a.do_enable && a.en == 1, "OUTPUT step did not emit");

  for (int k = 0; k < 10; k++) seq_tick(&c, s, &w, 20, &a);
  CHECK(c.state == SQ_RUNNING, "WAIT_UNTIL satisfied early (i is still 500)");
  w.i_mA[0] = 10;
  seq_tick(&c, s, &w, 20, &a);
  seq_tick(&c, s, &w, 20, &a);
  CHECK(c.state == SQ_DONE, "sequence should finish, state=%d", (int)c.state);
  CHECK(a.do_all_off == 1, "END OF SEQUENCE MUST TURN OUTPUTS OFF");

  // WAIT_UNTIL that never satisfies must time out, not hang forever.
  // do_all_off is a ONE-SHOT on the finishing tick — the real caller acts on it
  // immediately — so capture it there rather than reading a later empty tick.
  seq_init(&c); seq_start(&c, 4);
  w.i_mA[0] = 500;
  seq_tick(&c, s, &w, 20, &a); seq_tick(&c, s, &w, 20, &a);
  int saw_all_off = 0;
  for (int k = 0; k < 100; k++) {
    seq_tick(&c, s, &w, 20, &a);
    if (a.finished) { saw_all_off = a.do_all_off; break; }
  }
  CHECK(c.state == SQ_ABORTED && c.abort_timeout, "WAIT_UNTIL must abort on timeout");
  CHECK(saw_all_off == 1, "a timed-out sequence must still safe the outputs");

  // A trip on EITHER channel aborts and kills BOTH.
  seq_init(&c); seq_start(&c, 4);
  w.tripped[1] = 1;
  seq_tick(&c, s, &w, 20, &a);
  CHECK(c.state == SQ_ABORTED && c.abort_trip, "a trip must abort the sequence");
  CHECK(a.do_all_off == 1, "a trip mid-sequence must disable BOTH channels");

  // Ramp interpolates from the measured start toward the target.
  SeqStep r[2];
  memset(r, 0, sizeof(r));
  r[0].op = SEQ_RAMP; r[0].ch = 1; r[0].a = 10000; r[0].b = 1000;
  r[1].op = SEQ_END;
  memset(&w, 0, sizeof(w)); w.v_mV[0] = 5000;
  seq_init(&c); seq_start(&c, 2);
  seq_tick(&c, r, &w, 20, &a);
  CHECK(a.do_set && a.v_mV >= 5000 && a.v_mV < 5300, "ramp start wrong: %d", a.v_mV);
  CHECK(a.i_mA == -1, "ramp must leave the current limit alone");
  for (int k = 0; k < 25; k++) seq_tick(&c, r, &w, 20, &a);
  CHECK(a.v_mV >= 7000 && a.v_mV <= 10000, "ramp midpoint wrong: %d", a.v_mV);
  for (int k = 0; k < 40; k++) seq_tick(&c, r, &w, 20, &a);
  CHECK(c.state == SQ_DONE, "ramp should complete");
}

// -----------------------------------------------------------------------------
static void test_ui_logic() {
  UiStepCtx u; ui_step_init(&u);
  CHECK(u.step == UI_STEP_FINE, "step must default to the finest");

  ui_step_cycle(&u, 0); CHECK(u.step == UI_STEP_MED,    "cycle 1");
  ui_step_cycle(&u, 0); CHECK(u.step == UI_STEP_COARSE, "cycle 2");
  ui_step_cycle(&u, 0); CHECK(u.step == UI_STEP_FINE,   "cycle must wrap");

  // Decay: you can never walk up to the panel and get +1.00 V from a step size
  // chosen ten minutes ago.
  ui_step_cycle(&u, 1000);
  ui_step_cycle(&u, 1000);
  CHECK(u.step == UI_STEP_COARSE, "setup for decay test");
  CHECK(ui_step_decay(&u, 1000 + 14000) == 0, "decayed too early");
  CHECK(u.step == UI_STEP_COARSE, "step changed before the 15 s deadline");
  CHECK(ui_step_decay(&u, 1000 + 15001) == 1, "should have decayed at 15 s");
  CHECK(u.step == UI_STEP_FINE, "decay must land on the finest step");

  // Velocity ramp bands.
  ui_step_init(&u);
  CHECK(ui_velocity_delta(&u, 100, 20) == 0, "no auto-repeat below 400 ms");
  ui_velocity_reset(&u);
  int32_t moved = 0;
  for (int t = 400; t < 1200; t += 20) moved += ui_velocity_delta(&u, t, 20);
  CHECK(moved > 280 && moved < 340, "0.4 V/s band moved %d mV over 800 ms", moved);

  ui_velocity_reset(&u);
  moved = 0;
  for (int t = 2500; t < 3500; t += 20) moved += ui_velocity_delta(&u, t, 20);
  CHECK(moved > 4800 && moved < 5200, "5 V/s band moved %d mV over 1 s", moved);

  // Full-range traverse should land near the ~5.6 s the design claims.
  ui_velocity_reset(&u);
  int32_t total = 0; int t = 0;
  while (total < 19000 && t < 20000) { total += ui_velocity_delta(&u, (uint32_t)t, 20); t += 20; }
  CHECK(t > 4500 && t < 7000, "full-range traverse took %d ms, expected ~5600", t);

  // Formatters must be byte-stable — that is what makes the dirty check work.
  char b1[16], b2[16];
  ui_fmt_milli(5002, 2, b1, sizeof(b1));
  ui_fmt_milli(5002, 2, b2, sizeof(b2));
  CHECK(!strcmp(b1, "5.00"), "2-decimal format gave '%s'", b1);
  CHECK(!strcmp(b1, b2), "formatter is not deterministic");
  ui_fmt_milli(5002, 3, b1, sizeof(b1));
  CHECK(!strcmp(b1, "5.002"), "3-decimal format gave '%s'", b1);
  ui_fmt_milli(-1250, 2, b1, sizeof(b1));
  CHECK(!strcmp(b1, "-1.25"), "negative format gave '%s'", b1);

  // Dashes never mean zero.
  ui_fmt_dashes(2, b1, sizeof(b1));
  CHECK(!strcmp(b1, "--.--"), "dash placeholder gave '%s'", b1);

  // OVP as the setpoint ceiling.
  CHECK(ui_v_ceiling(20000, 3600) == 3600, "OVP must bind the ceiling");
  CHECK(ui_v_ceiling(20000, 21000) == 20000, "vmax must bind when OVP is higher");
}

// -----------------------------------------------------------------------------
int main() {
  test_cal();
  test_protect();
  test_cmd();
  test_log();
  test_seq();
  test_ui_logic();

  printf("LogRec=%zu bytes, SeqStep=%zu bytes, ChanCal=%zu bytes\n",
         sizeof(LogRec), sizeof(SeqStep), sizeof(ChanCal));
  if (sizeof(LogRec)  != 12) { printf("FAIL LogRec must be exactly 12 bytes\n");  g_fail++; }
  if (sizeof(SeqStep) != 16) { printf("FAIL SeqStep must be exactly 16 bytes\n"); g_fail++; }

  if (g_fail) { printf("\n%d CHECK(s) FAILED\n", g_fail); return 1; }
  printf("vb v2.1 pure headers: all checks passed\n");
  return 0;
}
