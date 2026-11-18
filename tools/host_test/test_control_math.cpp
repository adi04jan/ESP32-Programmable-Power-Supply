// Host self-check for control_math.h — compiles with plain g++, no Arduino.
#include <assert.h>
#include <stdio.h>
#include "../../ESP32_power_supply/control_math.h"

int main() {
  // --- median5 ---
  uint32_t v[5] = {5000, 4990, 9000, 5010, 5005};
  assert(cm_median5(v) == 5005);

  // --- floor step: higher target -> lower (or equal) floor step (more V allowed)
  assert(cm_floor_step(3300) >= cm_floor_step(12000));

  // --- synthetic non-increasing map: 12.0 V at step 0, -75 mV per step
  static uint16_t map[128];
  for (int s = 0; s < 128; s++) map[s] = (uint16_t)(12000 - s * 75);

  // best step never overvolts (> target+300) and is nearest
  for (uint32_t t = 3000; t <= 12000; t += 500) {
    int b = cm_best_step(map, t);
    assert(b >= 0 && (uint32_t)map[b] <= t + 300u);
    uint32_t e = map[b] > t ? map[b] - t : t - map[b];
    assert(e <= 75);                     // within one step of target
  }

  // --- correction: err (+80 mV) > half local spacing (37 mV) -> move +1 step (lower V)
  { uint32_t tgt = map[40] - 80;
    assert(cm_correction_step(map, 40, 0, +80, tgt) == 41);
    assert(cm_correction_step(map, 40, 0, +20, tgt) == 40);      // small err: stay
    assert(cm_correction_step(map, 41, 41, -200, tgt) == 41); }  // floor blocks lower step: stay

  // --- correction never returns a predicted-overvolt step
  { // tgt=8700: step 39 (9075 mV) exceeds tgt+300=9000 -> must refuse the move
    assert(cm_correction_step(map, 40, 0, -400, 8700) == 40); }
  { // a one-step-sized low reading still corrects upward (meas 9000 at step 40, tgt 9100)
    assert(cm_correction_step(map, 40, 0, -100, 9100) == 39); }
  { // implausible under-read (> 3 steps low, e.g. the ADC sagging under load) must
    // never step UP, even when the map says the next step would not overvolt
    assert(cm_correction_step(map, 40, 0, -400, 8900) == 40);
    assert(cm_correction_step(map, 100, 0, -1300, 13500) == 100); }
  { // a large HIGH reading may still step down (the safe direction)
    assert(cm_correction_step(map, 40, 0, +400, map[41]) == 41); }

  // --- fixed map (replaces the sweep): this board's fitted curve
  { uint16_t f[CM_STEPS]; cm_fixed_map(f);
    for (int s = 0; s < 9; s++) assert(f[s] == CM_RAIL_MV);          // above the input rail
    for (int s = 10; s < CM_STEPS; s++) assert(f[s] < f[s - 1]);     // strictly falling: no plateaus
    assert(f[12] >= 13413 && f[12] <= 13433);   // measured unloaded at step 12: 13430 mV
    assert(f[127] >= 2580 && f[127] <= 2600);   // bottom of range (sweeps: 2535 / 2606)
    assert(cm_best_step(f, 13485) == 12);       // the build-box gateway setpoint: unchanged step
    // 13.7 V must not pick step 11 (~14.3 V real): the noisy swept map did (+0.6 V)
    assert(cm_best_step(f, 13700) == 12);
    for (uint32_t t = 2500; t <= 17000; t += 10) {  // every valid target (VB_VMAX_MV 17 V):
      int s = cm_best_step(f, t);                    // a step, never > +300 mV, never the
      assert(s >= 9 && f[s] <= t + CM_OV_MARGIN_MV); // unregulated dropout steps 0-8
    }
    assert(cm_best_step(f, 17000) == 9);         // top setpoint = step 9 (~16.6 V)
  }

  puts("control_math: all checks passed");
  return 0;
}
