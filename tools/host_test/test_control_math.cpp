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
  { // same err but tgt=8900: step 39 no longer overvolts (9075<=9200) -> moves
    assert(cm_correction_step(map, 40, 0, -400, 8900) == 39); }

  puts("control_math: all checks passed");
  return 0;
}
