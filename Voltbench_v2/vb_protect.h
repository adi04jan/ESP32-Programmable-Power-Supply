// =============================================================================
//   vb_protect.h — PURE: per-channel protection + CC state machine (spec §3.5).
//
//        OFF ──en_cmd&&!hw──► STARTING ──250ms──► CV ◄──────┐
//         ▲                                        │        │ !cc 200ms
//         │ en_cmd=0                       cc 60ms │        │
//         │                                        ▼        │
//         │                                       CC ───────┘
//         │      ovp|ocp|otp|cc_trip|hw|interlock  │
//         │  ┌─────────────────────────────────────┘
//         │  ▼
//         │ TRIPPED(reason)   EN low AND DAC forced to CV=4095/CC=0
//         │  │ clear_trip AND fault absent
//         │  ▼
//         └ CLEARING (200 ms, rewrite DACs) ──► OFF
//
//   Two rules that look like details and are not:
//
//   1. A TRIP DROPS EN *AND* FORCES THE DAC SAFE. A stuck EN net must not be
//      able to hold 20 V on a tripped output.
//   2. CLEARING NEVER AUTO-RE-ENABLES. A latch that re-arms itself is not a
//      latch, so entering TRIPPED also clears the caller's enable request
//      (out->clear_en_cmd) — otherwise OFF would immediately re-enter STARTING
//      while en_cmd was still asserted.
//
//   Latches are RAM-only on purpose: a power cycle clears them, so a headless
//   unit cannot strand itself.
// =============================================================================
#pragma once
#include <stdint.h>
#include <string.h>

typedef enum { PS_OFF = 0, PS_STARTING, PS_CV, PS_CC, PS_TRIPPED, PS_CLEARING } ProtState;
typedef enum { PR_NONE = 0, PR_OVP, PR_OCP, PR_OTP, PR_CC_TRIP,
               PR_HW, PR_INTERLOCK, PR_SENSE } ProtReason;
typedef enum { CCM_LIMIT = 0, CCM_TRIP } CcMode;

typedef struct {
  ProtState  state;
  ProtReason reason;
  uint32_t   t_state;                       // ms in the current state
  uint32_t   t_ovp, t_ocp, t_otp;           // fault debounce accumulators
  uint32_t   t_cc_on, t_cc_off, t_regwarn;
  uint8_t    hw_ticks;
  uint8_t    cc_badge;                      // sticky-on CC indication
} ProtCtx;

typedef struct {
  int32_t v_mV, i_mA;                       // measured
  int32_t vset_mV, iset_mA;                 // commanded
  int32_t ovp_mV, ocp_mA;                   // per-channel limits
  int32_t temp_c10;                         // tenths of a degree C
  int32_t otp_c;                            // trip threshold, whole degrees
  CcMode  cc_mode;
  uint8_t en_cmd;                           // user wants this channel on
  uint8_t hw_fault;                         // I2C NACK / DAC readback mismatch
  uint8_t sense_fault;                      // INA219 not answering
  uint8_t interlock;                        // self-cal load engaged, etc.
  uint8_t clear_req;                        // one-shot clear request
} ProtIn;

typedef struct {
  uint8_t en_out;                           // drive the EN GPIO
  uint8_t force_dac_safe;                   // write CV=4095 / CC=0 this tick
  uint8_t clear_en_cmd;                     // caller must zero its en_cmd cell
  uint8_t log_now;                          // force one log record (trip point)
  uint8_t push_now;                         // WebSocket edge push
  uint8_t banner_cc;                        // CC *entry* only — 3 s banner
  uint8_t reg_warn;                         // log-only regulation warning
} ProtOut;

static inline void prot_init(ProtCtx *c) {
  memset(c, 0, sizeof(*c));
  c->state = PS_OFF;
}

static inline const char *prot_state_str(ProtState s) {
  switch (s) {
    case PS_OFF:      return "off";
    case PS_STARTING: return "starting";
    case PS_CV:       return "cv";
    case PS_CC:       return "cc";
    case PS_TRIPPED:  return "tripped";
    case PS_CLEARING: return "clearing";
  }
  return "?";
}

static inline const char *prot_reason_str(ProtReason r) {
  switch (r) {
    case PR_NONE:      return "";
    case PR_OVP:       return "ovp";
    case PR_OCP:       return "ocp";
    case PR_OTP:       return "otp";
    case PR_CC_TRIP:   return "cc_trip";
    case PR_HW:        return "hw";
    case PR_INTERLOCK: return "interlock";
    case PR_SENSE:     return "sense";
  }
  return "?";
}

// The CC predicate. Both halves are required: current at the limit alone also
// describes a happy CV rail feeding exactly its limit, and only the collapsed
// voltage proves the analog loop has actually taken control.
static inline int prot_cc_predicate(const ProtIn *in) {
  int32_t margin = in->iset_mA / 20;              // 5%
  if (margin < 50) margin = 50;                   // ...or 50 mA, whichever is larger
  return in->i_mA >= in->iset_mA - margin
      && in->v_mV <  in->vset_mV - 250;           // clears ripple + regulation error
}

// OCP is NOT the limiter — the analog loop is. OCP detects that the analog loop
// FAILED, so it sits above the CC band and above integrator windup overshoot.
static inline int32_t prot_ocp_threshold(const ProtIn *in) {
  int32_t t = in->iset_mA + 150;
  return (in->ocp_mA < t) ? in->ocp_mA : t;
}

// Is the condition that caused the latch still present? Clearing while the
// fault persists just relatches, so it is refused with a distinct error.
// OCP and CC_TRIP have no persistent condition and clear immediately.
static inline int prot_can_clear(const ProtCtx *c, const ProtIn *in) {
  if (c->state != PS_TRIPPED) return 0;
  switch (c->reason) {
    case PR_OVP:       return in->v_mV < in->ovp_mV;
    case PR_OTP:       return in->temp_c10 < (in->otp_c - 5) * 10;   // 5 C hysteresis
    case PR_HW:        return !in->hw_fault;
    case PR_SENSE:     return !in->sense_fault;
    case PR_INTERLOCK: return !in->interlock;
    default:           return 1;
  }
}

static inline void prot__enter(ProtCtx *c, ProtState s) {
  c->state   = s;
  c->t_state = 0;
  c->t_ovp = c->t_ocp = c->t_otp = 0;
  c->t_cc_on = c->t_cc_off = 0;
}

static inline void prot__trip(ProtCtx *c, ProtReason r, ProtOut *out) {
  prot__enter(c, PS_TRIPPED);
  c->reason         = r;
  c->cc_badge       = 0;
  out->force_dac_safe = 1;
  out->clear_en_cmd = 1;      // the latch must not re-arm itself
  out->log_now      = 1;      // guarantees the CSV contains the trip point
  out->push_now     = 1;
}

// One tick. dt_ms is the elapsed time since the previous call — pass the real
// measured delta, not the nominal period, or every debounce drifts under load.
static inline void prot_tick(ProtCtx *c, const ProtIn *in, uint32_t dt_ms, ProtOut *out) {
  memset(out, 0, sizeof(*out));
  c->t_state += dt_ms;

  // ---- hardware faults: debounced over ticks, and they win everywhere -------
  if (in->hw_fault) { if (c->hw_ticks < 255) c->hw_ticks++; }
  else                c->hw_ticks = 0;

  // ---- user dropped the enable: leave immediately, from any live state -----
  if (!in->en_cmd && c->state != PS_TRIPPED && c->state != PS_CLEARING) {
    if (c->state != PS_OFF) { prot__enter(c, PS_OFF); out->push_now = 1; }
    c->cc_badge = 0;
    out->en_out = 0;
    return;
  }

  switch (c->state) {

    case PS_OFF:
      out->en_out = 0;
      c->reason   = PR_NONE;
      if (in->en_cmd && !in->hw_fault && !in->interlock) {
        prot__enter(c, PS_STARTING);
        out->en_out   = 1;      // assert on the transition tick, not 20 ms later
        out->push_now = 1;
      }
      break;

    case PS_STARTING:
      // Protection is suppressed through inrush — but a hard fault is not
      // inrush, so HW and INTERLOCK still bite here.
      out->en_out = 1;
      if (c->hw_ticks >= 3)  { prot__trip(c, PR_HW, out);        break; }
      if (in->interlock)     { prot__trip(c, PR_INTERLOCK, out); break; }
      if (c->t_state >= 250) { prot__enter(c, PS_CV); out->push_now = 1; }
      break;

    case PS_CV:
    case PS_CC: {
      out->en_out = 1;

      // ---- latching faults, highest severity first -------------------------
      if (c->hw_ticks >= 3)   { prot__trip(c, PR_HW, out);        break; }
      if (in->sense_fault)    { prot__trip(c, PR_SENSE, out);     break; }
      if (in->interlock)      { prot__trip(c, PR_INTERLOCK, out); break; }

      c->t_otp = (in->temp_c10 > in->otp_c * 10) ? c->t_otp + dt_ms : 0;
      if (c->t_otp >= 1000)   { prot__trip(c, PR_OTP, out);       break; }

      c->t_ovp = (in->v_mV > in->ovp_mV) ? c->t_ovp + dt_ms : 0;
      if (c->t_ovp >= 20)     { prot__trip(c, PR_OVP, out);       break; }

      c->t_ocp = (in->i_mA > prot_ocp_threshold(in)) ? c->t_ocp + dt_ms : 0;
      if (c->t_ocp >= 100)    { prot__trip(c, PR_OCP, out);       break; }

      // ---- CV <-> CC, asymmetric and sticky-on so the badge cannot flicker --
      int pred = prot_cc_predicate(in);
      if (c->state == PS_CV) {
        c->t_cc_on = pred ? c->t_cc_on + dt_ms : 0;
        if (c->t_cc_on >= 60) {
          prot__enter(c, PS_CC);
          c->cc_badge     = 1;
          out->banner_cc  = 1;        // entry only; a permanent banner stops being read
          out->push_now   = 1;
        }
      } else {
        c->t_cc_off = pred ? 0 : c->t_cc_off + dt_ms;
        if (c->t_cc_off >= 200) {
          prot__enter(c, PS_CV);
          c->cc_badge   = 0;
          out->push_now = 1;
          break;
        }
        // cc_mode=trip differs from cc_mode=limit by EXACTLY this edge. In both
        // modes the analog CC DAC stays at the user's limit — it is what bounds
        // fault current during the ~120 ms before firmware latches.
        if (in->cc_mode == CCM_TRIP && c->t_state >= 60) {
          prot__trip(c, PR_CC_TRIP, out);
          break;
        }
      }

      // ---- log-only: catches a blown FB resistor without nuisance trips -----
      int32_t err = in->v_mV - in->vset_mV;
      if (err < 0) err = -err;
      c->t_regwarn = (err > 1000 && c->state == PS_CV) ? c->t_regwarn + dt_ms : 0;
      if (c->t_regwarn >= 3000) { out->reg_warn = 1; c->t_regwarn = 0; }
      break;
    }

    case PS_TRIPPED:
      out->en_out         = 0;
      out->force_dac_safe = 1;
      // Re-asserted every tick, not just on the trip edge: a caller that misses
      // the one-shot must not be able to leave en_cmd set, or CLEARING->OFF
      // would walk straight back into STARTING and re-energise the fault.
      out->clear_en_cmd   = 1;
      if (in->clear_req && prot_can_clear(c, in)) {
        prot__enter(c, PS_CLEARING);
        out->push_now = 1;
      }
      break;

    case PS_CLEARING:
      out->en_out         = 0;
      out->force_dac_safe = 1;          // DACs are rewritten across this window
      if (c->t_state >= 200) {
        prot__enter(c, PS_OFF);
        c->reason     = PR_NONE;
        out->push_now = 1;
      }
      break;
  }
}
