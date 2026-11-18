// =============================================================================
//   vb_ui_logic.h — PURE: step sizing, velocity ramp, field formatting.
//
//   The rules here are the reason the panel is safe to use on a live rail:
//
//   * The step DECAYS back to the finest value after 15 s of inactivity, so you
//     can never walk up to the panel, press UP, and get +1.00 V because of a
//     step size you selected ten minutes ago.
//   * There is no edit mode and therefore nothing to abort: every press moves
//     the setpoint by a bounded, visible amount, and the inverse press undoes
//     it exactly.
//   * OVP doubles as the setpoint ceiling. Set OVP to 3.6 V before probing an
//     ESP32 board and no coarse-digit fumble can smoke it.
// =============================================================================
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define UI_STEP_FINE    10      // mV / mA
#define UI_STEP_MED    100
#define UI_STEP_COARSE 1000
#define UI_STEP_DECAY_MS 15000

typedef struct {
  uint16_t step;            // current step in mV (or mA)
  uint32_t last_touch_ms;
  int32_t  vel_rem;         // fractional carry so slow rates still advance
} UiStepCtx;

static inline void ui_step_init(UiStepCtx *c) {
  memset(c, 0, sizeof(*c));
  c->step = UI_STEP_FINE;   // always defaults to the finest
}

// UP+DOWN tap cycles coarser and wraps.
static inline void ui_step_cycle(UiStepCtx *c, uint32_t now_ms) {
  c->step = (c->step == UI_STEP_FINE) ? UI_STEP_MED
          : (c->step == UI_STEP_MED)  ? UI_STEP_COARSE : UI_STEP_FINE;
  c->last_touch_ms = now_ms;
}

static inline void ui_step_touch(UiStepCtx *c, uint32_t now_ms) {
  c->last_touch_ms = now_ms;
}

// Call every tick. Returns 1 if the step just decayed (so the hint line can be
// repainted rather than polled).
static inline int ui_step_decay(UiStepCtx *c, uint32_t now_ms) {
  if (c->step != UI_STEP_FINE && (now_ms - c->last_touch_ms) >= UI_STEP_DECAY_MS) {
    c->step = UI_STEP_FINE;
    return 1;
  }
  return 0;
}

// Velocity ramp for a held button (spec §5.3). `held_ms` is time since the
// press edge; the single step on the edge itself is applied by the caller, so
// this returns 0 below the 400 ms threshold.
//
//   < 400 ms   one step (caller)
//   0.4-1.2 s  0.4 V/s
//   1.2-2.5 s  1.5 V/s
//   >= 2.5 s   5.0 V/s
//
// Full-range traverse works out at ~5.8 s of hold, and a 10 mV nudge is still
// exactly one press.
static inline int32_t ui_velocity_delta(UiStepCtx *c, uint32_t held_ms, uint32_t dt_ms) {
  int32_t rate_mV_s;
  if      (held_ms <  400) return 0;
  else if (held_ms < 1200) rate_mV_s =  400;
  else if (held_ms < 2500) rate_mV_s = 1500;
  else                     rate_mV_s = 5000;

  int32_t num = rate_mV_s * (int32_t)dt_ms + c->vel_rem;
  int32_t out = num / 1000;
  c->vel_rem  = num % 1000;
  return out;
}

static inline void ui_velocity_reset(UiStepCtx *c) { c->vel_rem = 0; }

// ---------------------------------------------------------------------------
//   Formatters. Fixed width so the format-then-compare dirty check works: a
//   stable rail must produce byte-identical strings and therefore ZERO display
//   traffic, no matter how much the ADC dithers below the shown resolution.
// ---------------------------------------------------------------------------

// Home screen uses 2 decimals: the INA219's 4 mV LSB plus 20-30 mV of ripple
// makes a third decimal dance. Full precision lives on the detail line.
static inline void ui_fmt_milli(int32_t milli, int decimals, char *out, int cap) {
  int neg = milli < 0;
  if (neg) milli = -milli;
  int32_t whole = milli / 1000, frac = milli % 1000;
  if (decimals == 2) snprintf(out, (size_t)cap, "%s%ld.%02ld", neg ? "-" : "", (long)whole, (long)(frac / 10));
  else if (decimals == 1) snprintf(out, (size_t)cap, "%s%ld.%ld", neg ? "-" : "", (long)whole, (long)(frac / 100));
  else snprintf(out, (size_t)cap, "%s%ld.%03ld", neg ? "-" : "", (long)whole, (long)frac);
}

// Dashes never mean zero. A dead INA219 must be visually distinct from a rail
// sitting at 0 V, or the operator reads a fault as a valid measurement.
static inline void ui_fmt_dashes(int decimals, char *out, int cap) {
  snprintf(out, (size_t)cap, decimals == 2 ? "--.--" : "--.---");
}

static inline int ui_clamp32(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// The setpoint ceiling is min(vmax, ovp) — one concept doing two jobs, which is
// what removes any need for a confirmation dialog on a setpoint edit.
static inline int32_t ui_v_ceiling(int32_t vmax_mV, int32_t ovp_mV) {
  return ovp_mV < vmax_mV ? ovp_mV : vmax_mV;
}
