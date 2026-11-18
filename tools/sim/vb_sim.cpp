// =============================================================================
//   vb_sim — a host-side Voltbench v2.1 that speaks the REAL serial protocol.
//
//   THE POINT: this links the actual firmware headers — vb_cal.h, vb_protect.h,
//   vb_seq.h, vb_log.h, vb_cmd.h — against a simulated analog plant. The
//   protection FSM, the calibration clamp chain, and the line tokeniser under
//   test here are the same code that ships. A behavioural simulator written
//   separately would drift from the firmware within a week; this one cannot,
//   because there is only one copy of the logic.
//
//   Time is INJECTED, never read from a clock: `SIM:TICK <ms>` advances the
//   model. That is what lets a four-hour soak test run in microseconds, and it
//   makes every debounce test exact instead of flaky.
//
//   Build:  g++ -std=c++17 -Wall -Wextra -O2 -o vb_sim vb_sim.cpp
//   Drive:  echo -e "VOLT 1,5\nOUT 1,1\nSIM:TICK 300\nMEAS? 1" | ./vb_sim
// =============================================================================
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <iostream>

#include "../../Voltbench_v2/vb_cal.h"
#include "../../Voltbench_v2/vb_protect.h"
#include "../../Voltbench_v2/vb_cmd.h"
#include "../../Voltbench_v2/vb_log.h"
#include "../../Voltbench_v2/vb_seq.h"

#define NCH 2
#define TICK_MS 20

// Kept local so the simulator does not need vb_config.h (which drags Arduino.h).
#define CV_CODE_SAFE_SIM 4095

// =============================================================================
//   The plant: MP1584 + FB injection + LM358 CC loop + a load.
// =============================================================================
struct Plant {
  double load_ohms   = 100.0;   // bench default: a light resistive load
  double v_mV        = 0.0;     // slewed output
  double i_mA        = 0.0;
  double tau_ms      = 5.0;     // output RC — why OVP must be absolute, not tracking
  double windup_ms   = 0.0;     // integrator overshoot remaining, on CC entry
  bool   was_cc      = false;
  bool   sense_ok    = true;    // INA219 answering
  double v_noise_mV  = 0.0;     // ripple, peak
};

struct Chan {
  ChanCal  cal;
  ProtCtx  prot;
  Plant    plant;
  int32_t  set_mV   = 5000, set_mA = 500;
  uint8_t  en       = 0;
  CcMode   cc_mode  = CCM_LIMIT;
  int32_t  ovp_mV   = 21000, ocp_mA = 2048;
  uint16_t cv_code  = CV_CODE_SAFE_SIM, cc_code = 0;
  bool     cal_valid = false;
  uint8_t  clear_req = 0;
};

static Chan     g_ch[NCH];
static uint32_t g_now_ms   = 0;
static bool     g_hw_fault = false;
static int32_t  g_temp_c10 = 250;
static int32_t  g_otp_c    = 80;
static SeqCtx   g_seqctx;
static SeqStep  g_seq[64];
static LogRec   g_logbuf[2048];
static LogRing  g_logring;
static uint8_t  g_stream_hz = 0;

// =============================================================================
//   Plant update — one tick
// =============================================================================
static void plant_tick(Chan &c, bool en_out, uint32_t dt_ms) {
  Plant &p = c.plant;

  if (!en_out) {
    // Decays through the load, it does not snap to zero — which is exactly why
    // "clear a trip while the rail is still high" has to be refused.
    double a = 1.0 - exp(-(double)dt_ms / p.tau_ms);
    p.v_mV += (0.0 - p.v_mV) * a;
    p.i_mA  = 0.0;
    p.was_cc = false;
    return;
  }

  // The CV loop commands this voltage; the CC loop caps the current.
  double v_cmd = (double)cal_cv_mV(&c.cal, c.cv_code);
  double ilim  = (double)cal_cc_mA(&c.cal, c.cc_code);
  double R     = p.load_ohms < 0.01 ? 0.01 : p.load_ohms;

  double i_want = v_cmd / R;                 // mV / ohm = mA
  double v_tgt, i_tgt;
  bool   cc;
  if (i_want > ilim) {                       // analog CC takes control
    cc    = true;
    i_tgt = ilim;
    v_tgt = ilim * R;                        // the rail folds back
  } else {
    cc    = false;
    i_tgt = i_want;
    v_tgt = v_cmd;
  }

  // Integrator windup: when CC first engages, the LM358's 10 ms tau lets
  // current overshoot for a few ms. OCP's 100 ms debounce exists to ride this
  // out, so the simulator must produce it or that test proves nothing.
  if (cc && !p.was_cc) p.windup_ms = 12.0;
  p.was_cc = cc;

  double overshoot = 0.0;
  if (p.windup_ms > 0.0) {
    overshoot   = i_tgt * 0.35 * (p.windup_ms / 12.0);
    p.windup_ms = p.windup_ms > dt_ms ? p.windup_ms - dt_ms : 0.0;
  }

  double a = 1.0 - exp(-(double)dt_ms / p.tau_ms);
  p.v_mV += (v_tgt - p.v_mV) * a;
  p.i_mA += (i_tgt - p.i_mA) * a;
  p.i_mA += overshoot;
  if (p.v_mV < 0) p.v_mV = 0;
  if (p.i_mA < 0) p.i_mA = 0;
}

// =============================================================================
//   One firmware tick — mirrors vb_control.cpp's ordering exactly
// =============================================================================
static void sim_tick(uint32_t dt_ms) {
  g_now_ms += dt_ms;

  // Sequencer, using the same executor the firmware runs.
  if (g_seqctx.state == SQ_RUNNING) {
    SeqWorld w; SeqAction a;
    for (int k = 0; k < NCH; k++) {
      w.v_mV[k]    = (int32_t)lround(g_ch[k].plant.v_mV);
      w.i_mA[k]    = (int32_t)lround(g_ch[k].plant.i_mA);
      w.tripped[k] = g_ch[k].prot.state == PS_TRIPPED;
    }
    seq_tick(&g_seqctx, g_seq, &w, dt_ms, &a);
    if (a.do_all_off) for (int k = 0; k < NCH; k++) g_ch[k].en = 0;
    if (a.do_set && a.ch) {
      g_ch[a.ch - 1].set_mV = a.v_mV;
      if (a.i_mA >= 0) g_ch[a.ch - 1].set_mA = a.i_mA;
    }
    if (a.do_enable && a.ch) g_ch[a.ch - 1].en = a.en;
  }

  ProtOut po[NCH];
  for (int k = 0; k < NCH; k++) {
    Chan &c = g_ch[k];

    ProtIn in;
    memset(&in, 0, sizeof(in));
    in.v_mV    = c.plant.sense_ok ? (int32_t)lround(c.plant.v_mV) : 0;
    in.i_mA    = c.plant.sense_ok ? (int32_t)lround(c.plant.i_mA) : 0;
    in.vset_mV = c.set_mV;   in.iset_mA = c.set_mA;
    in.ovp_mV  = c.ovp_mV;   in.ocp_mA  = c.ocp_mA;
    in.temp_c10 = g_temp_c10; in.otp_c  = g_otp_c;
    in.cc_mode  = c.cc_mode;
    in.en_cmd   = c.en;
    in.hw_fault = g_hw_fault;
    in.sense_fault = !c.plant.sense_ok;
    in.clear_req   = c.clear_req;

    prot_tick(&c.prot, &in, dt_ms, &po[k]);
    c.clear_req = 0;
    if (po[k].clear_en_cmd) c.en = 0;

    // Same clamp chain as the firmware, same functions.
    if (po[k].force_dac_safe) {
      c.cv_code = CV_CODE_SAFE_SIM;
      c.cc_code = 0;
    } else {
      int32_t ceiling = c.ovp_mV < 20000 ? c.ovp_mV : 20000;
      int32_t act;
      cal_cv_code(&c.cal, c.set_mV, 1000, ceiling, &c.cv_code, &act);
      cal_cc_code(&c.cal, c.set_mA, 2048, &c.cc_code, &act);
    }
    plant_tick(c, po[k].en_out, dt_ms);
  }

  uint16_t lv[2], li[2];
  for (int k = 0; k < NCH; k++) {
    lv[k] = (uint16_t)lround(g_ch[k].plant.v_mV);
    li[k] = log_pack_i((int32_t)lround(g_ch[k].plant.i_mA), po[k].en_out,
                       g_ch[k].prot.cc_badge, g_ch[k].prot.state == PS_TRIPPED);
  }
  bool trip_now = po[0].log_now || po[1].log_now;
  log_append(&g_logring, g_now_ms, lv, li, trip_now ? 1 : 0);
}

static void advance(uint32_t ms) {
  while (ms >= TICK_MS) { sim_tick(TICK_MS); ms -= TICK_MS; }
  if (ms) sim_tick(ms);
}

// =============================================================================
//   Replies — byte-identical shapes to vb_console.cpp
// =============================================================================
static void p_ok()                 { printf("OK\n"); }
static void p_err(const char *e)   { printf("ERR %s\n", e); }

static const char *mode_str(const Chan &c) {
  return c.prot.state == PS_TRIPPED ? "TRIP" : c.prot.state == PS_CC ? "CC" : "CV";
}

static void print_meas(int k) {
  const Chan &c = g_ch[k];
  long v = c.plant.sense_ok ? lround(c.plant.v_mV) : 0;
  long i = c.plant.sense_ok ? lround(c.plant.i_mA) : 0;
  long w = v * i / 1000;
  printf("OK %ld.%03ld,%ld.%03ld,%ld.%03ld,%s\n",
         v / 1000, v % 1000, i / 1000, i % 1000, w / 1000, w % 1000, mode_str(c));
}

static void print_meas_row(int k) {
  const Chan &c = g_ch[k];
  long v = c.plant.sense_ok ? lround(c.plant.v_mV) : 0;
  long i = c.plant.sense_ok ? lround(c.plant.i_mA) : 0;
  long w = v * i / 1000;
  printf("> %d,%ld.%03ld,%ld.%03ld,%ld.%03ld,%s\n", k + 1,
         v / 1000, v % 1000, i / 1000, i % 1000, w / 1000, w % 1000, mode_str(c));
}

static void print_state() {
  printf("OK {\"ts\":%u,\"up\":%u,\"api\":2,\"ch\":[", g_now_ms, g_now_ms / 1000);
  for (int k = 0; k < NCH; k++) {
    const Chan &c = g_ch[k];
    long v = c.plant.sense_ok ? lround(c.plant.v_mV) : 0;
    long i = c.plant.sense_ok ? lround(c.plant.i_mA) : 0;
    long w = v * i / 1000;
    printf("%s{\"en\":%s,\"v\":%ld.%03ld,\"i\":%ld.%03ld,\"w\":%ld.%03ld,"
           "\"vset\":%ld.%03ld,\"iset\":%ld.%03ld,\"mode\":\"%s\",\"prot\":\"%s\","
           "\"reason\":\"%s\",\"cc_mode\":\"%s\",\"ovp\":%ld.%03ld,\"ocp\":%ld.%03ld}",
      k ? "," : "", c.en ? "true" : "false",
      v / 1000, v % 1000, i / 1000, i % 1000, w / 1000, w % 1000,
      (long)(c.set_mV / 1000), (long)(c.set_mV % 1000),
      (long)(c.set_mA / 1000), (long)(c.set_mA % 1000),
      c.prot.state == PS_CC ? "cc" : "cv",
      c.prot.state == PS_TRIPPED ? "trip" : "ok",
      prot_reason_str(c.prot.reason),
      c.cc_mode == CCM_TRIP ? "trip" : "limit",
      (long)(c.ovp_mV / 1000), (long)(c.ovp_mV % 1000),
      (long)(c.ocp_mA / 1000), (long)(c.ocp_mA % 1000));
  }
  printf("],\"temp\":%ld.%01ld,\"temp_src\":\"mcp9700\","
         "\"seq\":{\"state\":\"%s\",\"step\":%u,\"n\":%u,\"el\":%u},"
         "\"log\":{\"on\":%s,\"n\":%u,\"cap\":%u},\"cal\":[%s,%s]}\n",
    (long)(g_temp_c10 / 10), (long)(g_temp_c10 % 10),
    seq_state_str(g_seqctx.state), g_seqctx.pc, g_seqctx.n, g_seqctx.t_total,
    g_logring.running ? "true" : "false",
    (unsigned)log_count(&g_logring), g_logring.cap,
    g_ch[0].cal_valid ? "true" : "false", g_ch[1].cal_valid ? "true" : "false");
}

// =============================================================================
//   SIM:* meta-commands — the injectable clock and the fault injector
// =============================================================================
static bool sim_meta(const char *line) {
  if (strncasecmp(line, "SIM:", 4) != 0) return false;
  const char *rest = line + 4;

  char verb[16] = {0};
  int  n = 0;
  while (rest[n] && rest[n] != ' ' && n < 15) { verb[n] = rest[n]; n++; }
  const char *args = rest[n] == ' ' ? rest + n + 1 : rest + n;

  if (!strcasecmp(verb, "TICK")) {
    long ms = atol(args);
    if (ms < 0 || ms > 86400000L) { p_err("range"); return true; }
    advance((uint32_t)ms);
    p_ok();
    return true;
  }
  if (!strcasecmp(verb, "TIME?")) { printf("OK %u\n", g_now_ms); return true; }

  if (!strcasecmp(verb, "LOAD")) {                 // SIM:LOAD <ch>,<ohms>
    int ch; double ohms;
    if (sscanf(args, "%d,%lf", &ch, &ohms) != 2 || ch < 1 || ch > NCH) { p_err("range"); return true; }
    g_ch[ch - 1].plant.load_ohms = ohms;
    p_ok();
    return true;
  }
  if (!strcasecmp(verb, "SHORT")) {                // dead short
    int ch = atoi(args);
    if (ch < 1 || ch > NCH) { p_err("range"); return true; }
    g_ch[ch - 1].plant.load_ohms = 0.05;
    p_ok();
    return true;
  }
  if (!strcasecmp(verb, "SENSE")) {                // SIM:SENSE <ch>,<0|1>
    int ch, ok;
    if (sscanf(args, "%d,%d", &ch, &ok) != 2 || ch < 1 || ch > NCH) { p_err("range"); return true; }
    g_ch[ch - 1].plant.sense_ok = ok != 0;
    p_ok();
    return true;
  }
  // Emit a line with NO sigil, then answer normally. Spec §4.3 requires the
  // host to discard unsigiled lines — that is what makes boot-ROM chatter and
  // mid-line truncation harmless instead of fatal.
  if (!strcasecmp(verb, "NOISE")) {
    printf("rst:0x15 (USB_UART_CHIP_RESET),boot:0x5 %s\n", args);
    p_ok();
    return true;
  }
  // A reboot in the middle of a request. The host must fail the pending call
  // rather than hang waiting for a reply that will never come.
  if (!strcasecmp(verb, "REBANNER")) {
    printf("! READY Voltbench,4.0.0-sim,proto 1,ch 2\n");
    return true;
  }
  if (!strcasecmp(verb, "TEMP")) { g_temp_c10 = (int32_t)(atof(args) * 10); p_ok(); return true; }
  if (!strcasecmp(verb, "HW"))   { g_hw_fault = atoi(args) != 0;            p_ok(); return true; }

  // Force the rail somewhere the control loop did not put it — the only way to
  // test OVP, which by construction cannot be reached through a legal setpoint.
  if (!strcasecmp(verb, "FORCEV")) {
    int ch; double mv;
    if (sscanf(args, "%d,%lf", &ch, &mv) != 2 || ch < 1 || ch > NCH) { p_err("range"); return true; }
    g_ch[ch - 1].plant.v_mV = mv;
    p_ok();
    return true;
  }
  if (!strcasecmp(verb, "CODES?")) {               // white-box: what got written
    int ch = atoi(args);
    if (ch < 1 || ch > NCH) { p_err("range"); return true; }
    printf("OK %u,%u\n", g_ch[ch - 1].cv_code, g_ch[ch - 1].cc_code);
    return true;
  }
  if (!strcasecmp(verb, "SEQ")) {                  // SIM:SEQ op,ch,a,b,timeout
    int op, ch; long a, b, to;
    if (sscanf(args, "%d,%d,%ld,%ld,%ld", &op, &ch, &a, &b, &to) != 5) { p_err("range"); return true; }
    if (g_seqctx.n >= 64) { p_err("full"); return true; }
    SeqStep &s = g_seq[g_seqctx.n];
    s.op = (uint8_t)op; s.ch = (uint8_t)ch;
    s.a = (int32_t)a;   s.b = (int32_t)b; s.timeout_ms = (int32_t)to;
    s.cond = 0;
    g_seqctx.n++;
    p_ok();
    return true;
  }
  if (!strcasecmp(verb, "SEQCLR")) { g_seqctx.n = 0; seq_init(&g_seqctx); p_ok(); return true; }
  if (!strcasecmp(verb, "RESET")) {
    for (int k = 0; k < NCH; k++) {
      g_ch[k] = Chan();
      cal_defaults(&g_ch[k].cal);
      prot_init(&g_ch[k].prot);
    }
    g_now_ms = 0; g_hw_fault = false; g_temp_c10 = 250;
    seq_init(&g_seqctx);
    log_init(&g_logring, g_logbuf, 2048);
    p_ok();
    return true;
  }
  p_err("unknown sim command");
  return true;
}

// =============================================================================
//   Dispatch — parsed by the FIRMWARE's tokeniser, so a protocol change cannot
//   pass here and fail on the device.
// =============================================================================
static void exec(const char *line) {
  if (sim_meta(line)) return;

  ParsedLine p;
  vb_parse_line(line, SRC_SERIAL, &p);
  if (p.kind == CK_NONE) return;
  if (p.kind == CK_ERR)  { p_err(p.err); return; }

  switch (p.kind) {
    case CK_APPLY: {
      const VbCommand &c = p.cmd;
      int k = c.ch ? c.ch - 1 : 0;
      switch (c.op) {
        case OP_SET_V:      g_ch[k].set_mV = c.arg; p_ok(); break;
        case OP_SET_I:      g_ch[k].set_mA = c.arg; p_ok(); break;
        case OP_ENABLE:     g_ch[k].en = (uint8_t)c.arg; p_ok(); break;
        case OP_ALL_OFF:    for (int j = 0; j < NCH; j++) g_ch[j].en = 0; p_ok(); break;
        case OP_CC_MODE:    g_ch[k].cc_mode = c.arg ? CCM_TRIP : CCM_LIMIT; p_ok(); break;
        case OP_CLEAR_TRIP: g_ch[k].clear_req = 1; p_ok(); break;
        case OP_SET_OVP:    g_ch[k].ovp_mV = c.arg; p_ok(); break;
        case OP_SET_OCP:    g_ch[k].ocp_mA = c.arg; p_ok(); break;
        case OP_SET_OTP:    g_otp_c = c.arg; p_ok(); break;
        case OP_ADJ_V:      g_ch[k].set_mV += c.arg; p_ok(); break;
        case OP_ADJ_I:      g_ch[k].set_mA += c.arg; p_ok(); break;
        case OP_LOG:
          if      (c.arg == LOGOP_START) g_logring.running = 1;
          else if (c.arg == LOGOP_STOP)  g_logring.running = 0;
          else if (c.arg == LOGOP_CLEAR) log_clear(&g_logring);
          else if (c.arg == LOGOP_RATE)  g_logring.rate_hz10 = (uint16_t)c.arg2;
          p_ok();
          break;
        case OP_SEQ:
          if      (c.arg == SEQOP_RUN)    seq_start(&g_seqctx, g_seqctx.n);
          else if (c.arg == SEQOP_STOP) { g_seqctx.state = SQ_IDLE;
                                          for (int j = 0; j < NCH; j++) g_ch[j].en = 0; }
          else if (c.arg == SEQOP_PAUSE)  g_seqctx.state = SQ_PAUSED;
          else if (c.arg == SEQOP_RESUME) g_seqctx.state = SQ_RUNNING;
          p_ok();
          break;
        case OP_CAL_PT: {
          // Two CV points complete a fit, exactly as the firmware does it.
          static int32_t  pend_mV[NCH]   = {0, 0};
          static uint16_t pend_code[NCH] = {0, 0};
          if (c.arg == CALPT_CV) {
            if (!pend_code[k]) { pend_code[k] = g_ch[k].cv_code; pend_mV[k] = c.arg2; p_ok(); }
            else {
              ChanCal t = g_ch[k].cal;
              if (cal_fit_cv(pend_code[k], pend_mV[k], g_ch[k].cv_code, c.arg2, &t)) {
                g_ch[k].cal = t; g_ch[k].cal_valid = true; p_ok();
              } else { g_ch[k].cal_valid = false; p_err("calfit"); }
              pend_code[k] = 0;
            }
          } else {
            cal_fit_cc_point(g_ch[k].set_mA, c.arg2, &g_ch[k].cal);
            p_ok();
          }
          break;
        }
        default: p_ok(); break;
      }
      break;
    }

    case CK_IDN:    printf("! READY Voltbench,4.0.0-sim,proto 1,ch 2\n"); break;
    case CK_STATE:  print_state(); break;
    case CK_MEAS:
      if (p.ch) { print_meas(p.ch - 1); }
      else {
        for (int k = 0; k < NCH; k++) print_meas_row(k);
        p_ok();
      }
      break;
    case CK_VOLT_Q: printf("OK %ld.%03ld\n", (long)(g_ch[p.ch-1].set_mV/1000), (long)(g_ch[p.ch-1].set_mV%1000)); break;
    case CK_CURR_Q: printf("OK %ld.%03ld\n", (long)(g_ch[p.ch-1].set_mA/1000), (long)(g_ch[p.ch-1].set_mA%1000)); break;
    case CK_OUT_Q:  printf("OK %d\n", g_ch[p.ch-1].en ? 1 : 0); break;
    case CK_PROT_Q: {
      const Chan &c = g_ch[p.ch - 1];
      printf("OK %s,%s,%ld.%03ld,%ld.%03ld,%ld\n",
             prot_state_str(c.prot.state), prot_reason_str(c.prot.reason),
             (long)(c.ovp_mV/1000), (long)(c.ovp_mV%1000),
             (long)(c.ocp_mA/1000), (long)(c.ocp_mA%1000), (long)g_otp_c);
      break;
    }
    case CK_CAL_Q:
      printf("OK %d,%.1f,%.1f,%.4f,%.1f\n", g_ch[p.ch-1].cal_valid ? 1 : 0,
             (double)g_ch[p.ch-1].cal.cv_v0_mV, (double)g_ch[p.ch-1].cal.cv_slope_uV,
             (double)g_ch[p.ch-1].cal.cc_gain,  (double)g_ch[p.ch-1].cal.cc_off_mA);
      break;
    case CK_LOG_Q:
      printf("OK %s,%u,%u,%u\n", g_logring.running ? "on" : "off",
             (unsigned)log_count(&g_logring), g_logring.cap, g_logring.rate_hz10);
      break;
    case CK_LOGDUMP: {
      // '>' rows FIRST, terminating OK LAST. A leading OK would let a host
      // consider the reply complete and treat the payload rows as the next
      // command's answer — every exchange after it shifts by one.
      char row[192];
      log_csv_header(row, sizeof(row));
      printf("> %s", row);
      for (uint32_t i = log_first(&g_logring); i < g_logring.head; i++)
        if (log_csv_row(&g_logring, i, row, sizeof(row)) > 0) printf("> %s", row);
      printf("OK %u\n", (unsigned)log_count(&g_logring));
      break;
    }
    case CK_SEQ_Q:
      printf("OK %s,%u,%u,%u\n", seq_state_str(g_seqctx.state),
             g_seqctx.pc, g_seqctx.n, g_seqctx.t_total);
      break;
    case CK_NET:    printf("OK sim,127.0.0.1,-40\n"); break;
    case CK_STREAM: g_stream_hz = (uint8_t)p.arg; p_ok(); break;
    case CK_HELP:   printf("> see spec 4.3; SIM:TICK/LOAD/SHORT/SENSE/TEMP/HW/FORCEV/CODES?/RESET\n");
                    p_ok(); break;
    case CK_REBOOT: printf("OK rebooting\n! READY Voltbench,4.0.0-sim,proto 1,ch 2\n"); break;
    default:        p_err("unsupported"); break;
  }
}

int main() {
  for (int k = 0; k < NCH; k++) { cal_defaults(&g_ch[k].cal); prot_init(&g_ch[k].prot); }
  log_init(&g_logring, g_logbuf, 2048);
  // UNBUFFERED, not line-buffered: MSVCRT does not implement _IOLBF and
  // silently treats it as fully-buffered, so a host driving this over a pipe
  // would block forever waiting for a reply that is sitting in libc.
  setvbuf(stdout, nullptr, _IONBF, 0);

  // The banner a host uses to detect a mid-stream reboot and flush its buffer.
  printf("! READY Voltbench,4.0.0-sim,proto 1,ch 2\n");

  std::string line;
  while (std::getline(std::cin, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    exec(line.c_str());
  }
  return 0;
}
