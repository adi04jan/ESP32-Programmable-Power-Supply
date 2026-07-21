// =============================================================================
//   Pure control math for the deadbeat voltage controller.
//   NO Arduino dependencies — host-testable via tools/host_test/.
//   Convention: higher wiper step = more resistance = LOWER output voltage;
//   the calibration map (mV per step, 0 = unknown) is non-increasing in step.
// =============================================================================
#pragma once
#include <stdint.h>

#define CM_STEPS        128
#define CM_RANGE_OHMS   10000
#define CM_WIPER_OHMS   15
#define CM_VREF_MV      1235
#define CM_R2_REF       10000
#define CM_OV_MARGIN_MV 300u    // hard overvolt ceiling above target

static inline int cm_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Wiper step whose resistance is closest to r_ohms.
static inline int cm_step_for_r(float r_ohms) {
  int s = (int)(((float)CM_STEPS * (r_ohms - (float)CM_WIPER_OHMS)) / (float)CM_RANGE_OHMS);
  return cm_clampi(s, 0, CM_STEPS - 1);
}

// Formula-predicted step for a target (used when the calibration map is empty).
static inline int cm_formula_step(uint32_t tgt_mV) {
  uint32_t safe = (tgt_mV > (uint32_t)(CM_VREF_MV) + CM_OV_MARGIN_MV)
                    ? (tgt_mV - CM_OV_MARGIN_MV) : tgt_mV;
  if (safe <= (uint32_t)CM_VREF_MV) return CM_STEPS - 1;
  float r = (float)CM_R2_REF * (float)CM_VREF_MV / (float)(safe - CM_VREF_MV)
            - (float)CM_WIPER_OHMS;
  return cm_step_for_r(r > 0.0f ? r : 0.0f);
}

// Lowest step (= max V) permitted for a target; ceiling = target + 300 mV.
static inline int cm_floor_step(uint32_t tgt_mV) {
  uint32_t ceil_mV = tgt_mV + CM_OV_MARGIN_MV;
  if (ceil_mV <= (uint32_t)CM_VREF_MV) return CM_STEPS - 1;
  float r = (float)CM_R2_REF * (float)CM_VREF_MV / (float)(ceil_mV - CM_VREF_MV)
            - (float)CM_WIPER_OHMS;
  return cm_step_for_r(r > 0.0f ? r : 0.0f);
}

// Closest calibrated step to target that doesn't exceed target+300 mV. -1 if none.
static inline int cm_best_step(const uint16_t *map, uint32_t tgt_mV) {
  int best = -1; uint32_t besterr = 0xFFFFFFFFu;
  for (int s = 0; s < CM_STEPS; s++) {
    if (map[s] == 0) continue;
    if ((uint32_t)map[s] > tgt_mV + CM_OV_MARGIN_MV) continue;
    uint32_t e = (tgt_mV > map[s]) ? (tgt_mV - map[s]) : ((uint32_t)map[s] - tgt_mV);
    if (e < besterr) { besterr = e; best = s; }
  }
  return best;
}

// Median of the last 5 display samples.
static inline uint32_t cm_median5(const uint32_t v[5]) {
  uint32_t a[5]; for (int i = 0; i < 5; i++) a[i] = v[i];
  for (int i = 1; i < 5; i++) {
    uint32_t k = a[i]; int j = i - 1;
    while (j >= 0 && a[j] > k) { a[j + 1] = a[j]; j--; }
    a[j + 1] = k;
  }
  return a[2];
}

// Per-step voltage spacing (mV) around step s, from map neighbours.
static inline uint32_t cm_local_spacing(const uint16_t *map, int s) {
  int lo = s > 0 ? s - 1 : s, hi = s < CM_STEPS - 1 ? s + 1 : s;
  if (map[lo] && map[hi] && hi > lo && map[lo] >= map[hi])
    return (uint32_t)(map[lo] - map[hi]) / (uint32_t)(hi - lo);
  return 60;   // conservative fallback ~= 2x low-range spacing
}

// The single allowed correction. Returns the step to move to, or s to stay.
// err_mV = measured - target (signed). Never proposes an overvolt step, never
// crosses floor_step, never moves against the intended direction.
static inline int cm_correction_step(const uint16_t *map, int s, int floor_step,
                                     int32_t err_mV, uint32_t tgt_mV) {
  uint32_t half = cm_local_spacing(map, s) / 2;
  uint32_t mag  = err_mV < 0 ? (uint32_t)(-err_mV) : (uint32_t)err_mV;
  if (mag <= half) return s;                       // within half a step: stay
  int cand = err_mV > 0 ? s + 1 : s - 1;           // too high -> +1 (lower V)
  cand = cm_clampi(cand, floor_step, CM_STEPS - 1);
  if (cand == s || (err_mV > 0) != (cand > s)) return s;   // clamped away: stay
  if (map[cand]) {
    if ((uint32_t)map[cand] > tgt_mV + CM_OV_MARGIN_MV) return s;  // would overvolt
    uint32_t ce = ((uint32_t)map[cand] > tgt_mV)
                    ? (uint32_t)map[cand] - tgt_mV : tgt_mV - (uint32_t)map[cand];
    if (ce >= mag) return s;                       // not predicted closer
  }
  return cand;
}
