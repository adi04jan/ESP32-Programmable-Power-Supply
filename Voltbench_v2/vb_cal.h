// =============================================================================
//   vb_cal.h — PURE: 2-point calibration fit, code<->mV conversion, clamping.
//
//   PURE means <stdint.h>/<string.h>/<math.h> ONLY — no Arduino.h, no String,
//   no FreeRTOS. That constraint is what makes tools/host_test possible. Do not
//   relax it; anything needing Arduino headers belongs in vb_hal or vb_control.
//
//   The CV mapping is INVERTED: code 0 = 20.18 V, code 4095 = minimum volts.
//   Every clamp in here exists because an unwritten or wrongly-computed CV code
//   is a 20 V event on someone's DUT.
// =============================================================================
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

// Stored per channel in NVS as ONE 24-byte blob (spec §3.6 — one key, not six).
// v0/slope rather than gain-and-offset because both are physically meaningful,
// directly invertible, and independently bounds-checkable.
typedef struct {
  float cv_v0_mV;      // output mV at DAC code 0
  float cv_slope_uV;   // uV of output lost per DAC code (positive; mapping falls)
  float cc_gain;       // DAC codes per mA
  float cc_off_mA;     // current offset, mA
  float ina_v_gain;    // INA219 bus-voltage trim (1.0 = untrimmed)
  float ina_i_gain;    // INA219 current trim
} ChanCal;

typedef enum {
  CVC_OK = 0,        // exact request honoured
  CVC_CLAMPED,       // limits reduced it; out_code/out_mV say what was applied
  CVC_REFUSED        // assert-back failed -> caller must latch HW_FAULT
} CvCodeResult;

static inline void cal_defaults(ChanCal *c) {
  c->cv_v0_mV    = 20180.0f;
  c->cv_slope_uV = 4763.0f;
  c->cc_gain     = 2.0f;
  c->cc_off_mA   = 0.0f;
  c->ina_v_gain  = 1.0f;
  c->ina_i_gain  = 1.0f;
}

// A fit that survived bounds checking. Nominal values also pass, so this is
// safe to call on a default-initialised ChanCal.
static inline int cal_plausible(const ChanCal *c) {
  return c->cv_v0_mV    >= 18500.0f && c->cv_v0_mV    <= 22000.0f
      && c->cv_slope_uV >= 4300.0f  && c->cv_slope_uV <= 5300.0f
      && c->cc_gain     >  0.1f     && c->cc_gain     <  20.0f;
}

static inline int32_t cal_cv_mV(const ChanCal *c, uint16_t code) {
  return (int32_t)lrintf(c->cv_v0_mV - (float)code * (c->cv_slope_uV / 1000.0f));
}

static inline int32_t cal_cc_mA(const ChanCal *c, uint16_t code) {
  return (int32_t)lrintf((float)code / c->cc_gain - c->cc_off_mA);
}

// Full clamp chain, in the order the spec fixes it (§3.4):
//   absolute limits -> user soft ceiling -> code conversion -> code clamp to
//   [cv_code(vmax), 4069] -> assert-back.
//
// The assert-back is the load-bearing step: if the code we are about to write
// would produce more than 250 mV above what was asked, the calibration is lying
// and we refuse the write rather than trust it. 4070-4095 are below-spec and
// never commanded.
static inline CvCodeResult cal_cv_code(const ChanCal *c,
                                       int32_t want_mV,
                                       int32_t vmin_mV, int32_t vmax_mV,
                                       uint16_t *out_code, int32_t *out_mV) {
  int clamped = 0;
  if (want_mV < vmin_mV) { want_mV = vmin_mV; clamped = 1; }
  if (want_mV > vmax_mV) { want_mV = vmax_mV; clamped = 1; }

  float f = (c->cv_v0_mV - (float)want_mV) / (c->cv_slope_uV / 1000.0f);
  long  n = lrintf(f);
  if (n < 0)    { n = 0;    clamped = 1; }
  if (n > 4069) { n = 4069; clamped = 1; }

  uint16_t code   = (uint16_t)n;
  int32_t  actual = cal_cv_mV(c, code);

  *out_code = code;
  *out_mV   = actual;

  // Overshoot beyond a quantisation step means the fit is not describing the
  // hardware. Refuse — do not "clamp harder" and hope.
  if (actual > want_mV + 250) return CVC_REFUSED;
  return clamped ? CVC_CLAMPED : CVC_OK;
}

static inline CvCodeResult cal_cc_code(const ChanCal *c,
                                       int32_t want_mA, int32_t imax_mA,
                                       uint16_t *out_code, int32_t *out_mA) {
  int clamped = 0;
  if (want_mA < 0)       { want_mA = 0;       clamped = 1; }
  if (want_mA > imax_mA) { want_mA = imax_mA; clamped = 1; }

  long n = lrintf(((float)want_mA + c->cc_off_mA) * c->cc_gain);
  if (n < 0)    { n = 0;    clamped = 1; }
  if (n > 4095) { n = 4095; clamped = 1; }

  *out_code = (uint16_t)n;
  *out_mA   = cal_cc_mA(c, (uint16_t)n);
  return clamped ? CVC_CLAMPED : CVC_OK;
}

// Two-point CV fit. Points are (DAC code, measured mV) — order does not matter,
// but they must be at least CAL_CODE_MIN_SPAN apart or the slope is noise.
// Returns 0 and leaves *out untouched if the fit is implausible, so a bad
// calibration run degrades to "keep nominal, flag invalid" rather than to a
// 20 V surprise.
static inline int cal_fit_cv(uint16_t code_a, int32_t mV_a,
                             uint16_t code_b, int32_t mV_b,
                             ChanCal *out) {
  // Positive when `a` is the higher code (and therefore the lower voltage).
  int32_t dcode = (int32_t)code_a - (int32_t)code_b;
  int32_t span  = dcode < 0 ? -dcode : dcode;
  if (span < 1500) return 0;

  // Higher code = lower voltage, so a sane pair yields a positive slope. Written
  // so the result is independent of which point the caller passed first.
  float slope_uV = ((float)(mV_b - mV_a) * 1000.0f) / (float)dcode;
  if (!(slope_uV > 0.0f)) return 0;                       // also catches NaN
  float v0 = (float)mV_a + (float)code_a * (slope_uV / 1000.0f);

  if (v0       < 18500.0f || v0       > 22000.0f) return 0;
  if (slope_uV < 4300.0f  || slope_uV > 5300.0f)  return 0;

  out->cv_v0_mV    = v0;
  out->cv_slope_uV = slope_uV;
  return 1;
}

// Single-point CC trim: hold the gain, move the offset so a measured current
// matches what was commanded.
static inline int cal_fit_cc_point(int32_t commanded_mA, int32_t measured_mA,
                                   ChanCal *out) {
  float err = (float)(measured_mA - commanded_mA);
  if (err < -500.0f || err > 500.0f) return 0;   // that is not a trim, that is a fault
  out->cc_off_mA += err;
  return 1;
}

// Phantom divider current (spec §9.4): the 100 kOhm CV divider taps AFTER the
// shunt, so its own draw reads as load current — ~187 uA at 20 V. Free to
// remove, and it is the difference between "0 mA" and "0.2 mA" on an idle rail.
static inline int32_t cal_subtract_phantom(int32_t i_uA, int32_t v_mV) {
  int32_t phantom_uA = v_mV / 107;
  int32_t out = i_uA - phantom_uA;
  return out < 0 ? 0 : out;
}
