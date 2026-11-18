// =============================================================================
//   vb_seq.h — PURE: fixed-size test sequencer (spec §3.8).
//
//   Three safety defaults, each learned the hard way in this class of gear:
//     * END OF SEQUENCE TURNS OUTPUTS OFF by default. A sequence that ends
//       silently leaving 20 V on the bench is how you kill a DUT.
//     * A TRIP MID-SEQUENCE ABORTS AND DISABLES BOTH CHANNELS. In a two-rail
//       test, leaving one rail up after the other dies back-feeds the DUT.
//     * NEVER AUTO-RESUME AFTER A RESET. Resume-after-crash on a power supply
//       is how you discover your DUT's real absolute-maximum ratings.
//
//   Steps are a fixed 16 bytes in a static array — no allocation while running,
//   which is also what makes the PC-side TOML a field rename rather than a
//   lossy flatten/re-nest.
// =============================================================================
#pragma once
#include <stdint.h>
#include <string.h>

enum { SEQ_SET = 0, SEQ_RAMP, SEQ_WAIT, SEQ_WAIT_UNTIL, SEQ_OUTPUT, SEQ_END };
enum { SEQC_I_BELOW = 0, SEQC_I_ABOVE, SEQC_V_BELOW, SEQC_V_ABOVE };

typedef struct {            // exactly 16 bytes
  uint8_t  op;
  uint8_t  ch;              // 1..2, or 0 for both where meaningful
  uint16_t cond;            // WAIT_UNTIL predicate selector
  int32_t  a;               // SET: mV · RAMP: target mV · WAIT: ms · OUTPUT: 0/1
  int32_t  b;               // SET: mA · RAMP: duration ms · WAIT_UNTIL: threshold
  int32_t  timeout_ms;      // WAIT_UNTIL only; 0 = no timeout (validator rejects)
} SeqStep;

typedef enum { SQ_IDLE = 0, SQ_RUNNING, SQ_PAUSED, SQ_DONE, SQ_ABORTED } SeqState;

typedef struct {
  SeqState state;
  uint16_t pc, n;
  uint32_t t_step;          // ms inside the current step
  uint32_t t_total;
  int32_t  ramp_from_mV;
  uint8_t  abort_trip;      // aborted because a channel tripped
  uint8_t  abort_timeout;
} SeqCtx;

// What the executor wants done this tick. The caller turns these into typed
// apply_command calls — the sequencer itself touches no hardware.
typedef struct {
  uint8_t do_set;           // apply v_mV / i_mA to ch
  uint8_t do_enable;
  uint8_t do_all_off;
  uint8_t ch;
  int32_t v_mV, i_mA;
  uint8_t en;
  uint8_t finished;         // sequence just ended (any reason)
} SeqAction;

// Live measurements the WAIT_UNTIL predicate reads.
typedef struct {
  int32_t v_mV[2], i_mA[2];
  uint8_t tripped[2];
} SeqWorld;

static inline void seq_init(SeqCtx *c) { memset(c, 0, sizeof(*c)); }

static inline const char *seq_state_str(SeqState s) {
  switch (s) {
    case SQ_IDLE:    return "idle";
    case SQ_RUNNING: return "running";
    case SQ_PAUSED:  return "paused";
    case SQ_DONE:    return "done";
    case SQ_ABORTED: return "aborted";
  }
  return "?";
}

// Validation happens at PUT time and returns the failing INDEX, which is what
// makes the API usable: {"err":"step","idx":3,"why":"v out of range"}.
static inline const char *seq_validate(const SeqStep *s, uint16_t n,
                                       int32_t vmin, int32_t vmax, int32_t imax,
                                       uint16_t *bad_idx) {
  if (n == 0)  { *bad_idx = 0; return "empty"; }
  if (n > 64)  { *bad_idx = 64; return "too many steps"; }

  for (uint16_t k = 0; k < n; k++) {
    *bad_idx = k;
    const SeqStep *p = &s[k];
    if (p->op > SEQ_END) return "unknown op";

    if (p->op == SEQ_SET || p->op == SEQ_RAMP || p->op == SEQ_OUTPUT ||
        p->op == SEQ_WAIT_UNTIL) {
      if (p->ch < 1 || p->ch > 2) return "bad channel";
    }
    switch (p->op) {
      case SEQ_SET:
        if (p->a < vmin || p->a > vmax) return "v out of range";
        if (p->b < 0    || p->b > imax) return "i out of range";
        break;
      case SEQ_RAMP:
        if (p->a < vmin || p->a > vmax) return "v out of range";
        if (p->b < 10   || p->b > 3600000) return "ramp duration out of range";
        break;
      case SEQ_WAIT:
        if (p->a < 0 || p->a > 86400000) return "wait out of range";
        break;
      case SEQ_WAIT_UNTIL:
        if (p->cond > SEQC_V_ABOVE) return "bad condition";
        // A wait that can never satisfy is the worst possible bench behaviour,
        // so an explicit timeout is mandatory rather than optional.
        if (p->timeout_ms < 10 || p->timeout_ms > 86400000) return "timeout required";
        break;
      case SEQ_OUTPUT:
        if (p->a != 0 && p->a != 1) return "output must be 0 or 1";
        break;
      default: break;
    }
  }
  *bad_idx = 0;
  return 0;                 // valid
}

static inline int seq__cond_met(const SeqStep *p, const SeqWorld *w) {
  int k = p->ch - 1;
  switch (p->cond) {
    case SEQC_I_BELOW: return w->i_mA[k] <  p->b;
    case SEQC_I_ABOVE: return w->i_mA[k] >  p->b;
    case SEQC_V_BELOW: return w->v_mV[k] <  p->b;
    case SEQC_V_ABOVE: return w->v_mV[k] >  p->b;
  }
  return 0;
}

static inline void seq__finish(SeqCtx *c, SeqState st, SeqAction *out) {
  c->state       = st;
  out->do_all_off = 1;      // the default that keeps DUTs alive
  out->finished   = 1;
}

// One executor tick. `dt_ms` is the real elapsed time, not the nominal period.
static inline void seq_tick(SeqCtx *c, const SeqStep *s, const SeqWorld *w,
                            uint32_t dt_ms, SeqAction *out) {
  memset(out, 0, sizeof(*out));
  if (c->state != SQ_RUNNING) return;

  // A trip anywhere kills the whole run, including the healthy channel.
  if (w->tripped[0] || w->tripped[1]) {
    c->abort_trip = 1;
    seq__finish(c, SQ_ABORTED, out);
    return;
  }
  if (c->pc >= c->n) { seq__finish(c, SQ_DONE, out); return; }

  c->t_step  += dt_ms;
  c->t_total += dt_ms;
  const SeqStep *p = &s[c->pc];
  int first = (c->t_step <= dt_ms);          // this is the step's first tick

  switch (p->op) {
    case SEQ_SET:
      out->do_set = 1; out->ch = p->ch; out->v_mV = p->a; out->i_mA = p->b;
      c->pc++; c->t_step = 0;
      break;

    case SEQ_RAMP: {
      if (first) c->ramp_from_mV = w->v_mV[p->ch - 1];
      uint32_t dur = (uint32_t)p->b;
      int32_t  span = p->a - c->ramp_from_mV;
      int32_t  now  = (c->t_step >= dur)
                    ? p->a
                    : c->ramp_from_mV + (int32_t)((int64_t)span * (int32_t)c->t_step / (int32_t)dur);
      out->do_set = 1; out->ch = p->ch; out->v_mV = now;
      out->i_mA   = -1;                      // -1 = leave the current limit alone
      if (c->t_step >= dur) { c->pc++; c->t_step = 0; }
      break;
    }

    case SEQ_WAIT:
      if (c->t_step >= (uint32_t)p->a) { c->pc++; c->t_step = 0; }
      break;

    case SEQ_WAIT_UNTIL:
      if (seq__cond_met(p, w)) { c->pc++; c->t_step = 0; }
      else if (c->t_step >= (uint32_t)p->timeout_ms) {
        c->abort_timeout = 1;
        seq__finish(c, SQ_ABORTED, out);
      }
      break;

    case SEQ_OUTPUT:
      out->do_enable = 1; out->ch = p->ch; out->en = (uint8_t)p->a;
      c->pc++; c->t_step = 0;
      break;

    case SEQ_END:
    default:
      seq__finish(c, SQ_DONE, out);
      break;
  }

  if (c->state == SQ_RUNNING && c->pc >= c->n) seq__finish(c, SQ_DONE, out);
}

static inline int seq_start(SeqCtx *c, uint16_t n) {
  if (n == 0) return 0;
  c->state = SQ_RUNNING; c->pc = 0; c->n = n;
  c->t_step = c->t_total = 0;
  c->abort_trip = c->abort_timeout = 0;
  return 1;
}
