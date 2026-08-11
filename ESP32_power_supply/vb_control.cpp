#include "vb_control.h"
#include "vb_debug.h"
#include "vb_nvs.h"          // cal_save() — persists the swept map
#include "control_math.h"    // pure control math, host-tested (tools/host_test/)
#include <Wire.h>

// Direct MCP4017 wiper control over I2C. (The SW_MCP4017 library is avoided: its
// setSteps() prints to Serial 4x per call, flooding the port and adding jitter in
// the control hot loop.) Tracks the wiper step locally so dpCalcR() stays consistent.
static volatile uint16_t g_wiper_step = 0;
static int dpStepForR(float Rout) { return cm_step_for_r(Rout); }
static void dpSetStep(int s) {
  s = constrain(s, 0, dpMaxSteps - 1);
  g_wiper_step = (uint16_t)s;
  Wire.beginTransmission(MCP4017ADDRESS);
  Wire.write((uint8_t)s);
  Wire.endTransmission();
}
static void dpSetR(float Rout) { dpSetStep(dpStepForR(Rout)); }
float dpCalcR() {
  return ((float)g_wiper_step / dpMaxSteps) * (float)maxRangeOhms + (float)MCPWIPEROHMS;
}

// Median-of-5 display window: calm steady-state number, snapped on freeze.
static uint32_t g_disp_ring[5];
static uint8_t  g_disp_i = 0;
static void disp_reset(uint32_t mv) {
  for (int i = 0; i < 5; i++) g_disp_ring[i] = mv;
  g_disp_i = 0; g_display_mV = mv;
}
static void disp_push(uint32_t mv) {
  g_disp_ring[g_disp_i] = mv; g_disp_i = (g_disp_i + 1) % 5;
  g_display_mV = cm_median5(g_disp_ring);
}

// =============================================================================
//   ADC averaging (rolling buffer)
// =============================================================================
static int read_any_volt(int pin, uint32_t *sample, bool *firstrun, int sc) {
  for (int j = sc - 1; j > 0; j--) sample[j] = sample[j - 1];
  sample[0] = analogReadMilliVolts(pin);
  if (!*firstrun && sample[0] != 0) {
    for (int j = 1; j < sc; j++) sample[j] = sample[0];
    *firstrun = true;
  }
  long sum = 0;
  for (int j = 0; j < sc; j++) sum += sample[j];
  return sum / sc;
}

int read_5V_volt() {
  const int sc = 5;
  static uint32_t s[sc] = {0}; static bool init = false;
  return read_any_volt(VOLTAGE_READ_PIN_5V, s, &init, sc) * 2;
}
int read_3V3_volt() {
  const int sc = 5;
  static uint32_t s[sc] = {0}; static bool init = false;
  return read_any_volt(VOLTAGE_READ_PIN_3V3, s, &init, sc) * 2;
}

// =============================================================================
//   Output / voltage control
// =============================================================================
void setOutput(uint8_t output, bool state) {
  if (output < 1 || output > 3) return;   // single bounds guard for all callers
  uint8_t pin = (output == 1) ? ENABLE_VV_PIN
              : (output == 2) ? ENABLE_5V_PIN
                              : ENABLE_3V3_PIN;
  digitalWrite(pin, state ? HIGH : LOW);
  if (output == 1)   psState.output1 = state;
  if (output == 2)   psState.output2 = state;
  if (output == 3)   psState.output3 = state;
}

// Non-blocking: writes globals — voltageControlTask does the actual I2C work.
// Clamped [0, 16 V]: rejects NaN/negative/garbage from MQTT/HTTP/console.
void setVoltage(float voltage) {
  if (!(voltage >= 0.0f)) voltage = 0.0f;
  if (voltage > 16.0f)    voltage = 16.0f;
  uint32_t mv      = (uint32_t)(voltage * 1000.0f);
  g_target_mV      = mv;
  g_setpoint_mV    = mv;
  psState.voltage1 = voltage;
}

// Single command dispatcher shared by WS / HTTP / USB+telnet console / MQTT.
bool apply_command(const String &action, uint8_t ch, float val) {
  if (action == "toggle" && ch >= 1 && ch <= 3) { setOutput(ch, val >= 0.5f); return true; }
  if (action == "set_voltage" && ch == 1)       { setVoltage(val); return true; }
  if (action == "all_off") { setOutput(1, false); setOutput(2, false); setOutput(3, false); return true; }
  return false;
}

// =============================================================================
//   voltageControlTask — deterministic best-step voltage setter
//   g_vctrl_state: 0=idle 1=settling 2=verifying 3=frozen 4=busy
//   Runs on core 0, priority 2. Web handlers return immediately.
// =============================================================================
// Heavily filtered: mean of 50 reads over ~200 ms (10 mains cycles @50 Hz) to
// reject hum picked up on the high-Z 47K:1K divider node. The old 4-sample /
// 20 ms read let ±600 mV of ADC noise through.
static uint32_t vctrl_sample() {
  uint32_t sum = 0;
  for (int s = 0; s < 50; s++) {
    sum += analogReadMilliVolts(VOLTAGE_READ_PIN_VV);
    vTaskDelay(pdMS_TO_TICKS(4));
  }
  return (uint32_t)((sum / 50) * VV_ADC_SCALE);
}

// Isotonic regression (Pool-Adjacent-Violators) — enforce Vout non-increasing
// as step increases, pooling noisy violators. Cleans the swept map so lookups
// land in the right neighbourhood even at low V, where per-step spacing
// (~27 mV) is well under the ADC noise (~±150 mV) and raw samples are jumbled.
static void cal_smooth_monotonic() {
  int lo = 0;               while (lo < dpMaxSteps && g_cal_mv[lo] == 0) lo++;
  int hi = dpMaxSteps - 1;  while (hi >= 0 && g_cal_mv[hi] == 0) hi--;
  if (hi - lo < 3) return;
  static float bval[128]; static int bcnt[128];
  int nb = 0;
  for (int i = lo; i <= hi; i++) {
    bval[nb] = g_cal_mv[i]; bcnt[nb] = 1; nb++;
    while (nb >= 2 && bval[nb - 1] > bval[nb - 2]) {       // non-increasing violation -> pool
      float sum = bval[nb - 1] * bcnt[nb - 1] + bval[nb - 2] * bcnt[nb - 2];
      bcnt[nb - 2] += bcnt[nb - 1];
      bval[nb - 2]  = sum / bcnt[nb - 2];
      nb--;
    }
  }
  int idx = lo;
  for (int b = 0; b < nb; b++)
    for (int k = 0; k < bcnt[b]; k++) g_cal_mv[idx++] = (uint16_t)(bval[b] + 0.5f);
}

// Build the step->mV calibration map by sweeping the wiper across its range.
// Output 1 is driven from ~2.5 V up to its ceiling, so run only with nothing
// (or a voltage-tolerant load) on output 1. Persisted to NVS on completion.
void run_cal_sweep() {
  bool was_on = psState.output1;
  setOutput(1, true);
  g_vctrl_state = 4;                          // "busy" for the UI
  Dbg.println("[Cal] sweep start");
  dpSetStep(dpMaxSteps - 1);                  // max R -> lowest V
  vTaskDelay(pdMS_TO_TICKS(400));
  for (int s = dpMaxSteps - 1; s >= 0; s--) {
    dpSetStep(s);
    vTaskDelay(pdMS_TO_TICKS(120));           // settle (rising V is fast, no load)
    uint32_t mv = (vctrl_sample() + vctrl_sample()) / 2;   // ~400 ms average for a cleaner map
    g_cal_mv[s] = (mv > 65000u) ? 65000u : (uint16_t)mv;
    if (mv > 16500u) {                        // railed near Vin ceiling — fill rest & stop
      for (int j = s - 1; j >= 0; j--) g_cal_mv[j] = g_cal_mv[s];
      break;
    }
  }
  dpSetStep(dpMaxSteps - 1);                  // park at lowest V
  cal_smooth_monotonic();                     // isotonic clean-up before use/save
  g_cal_valid = true;
  cal_save();
  if (!was_on) setOutput(1, false);
  disp_reset(0);
  g_vctrl_state = 0;
  Dbg.println("[Cal] sweep done + saved");
}

// Closest calibrated step to target that doesn't exceed the +300 mV ceiling.
static int cal_best_step(uint32_t target) { return cm_best_step(g_cal_mv, target); }

void voltageControlTask(void* pvParameters) {
  uint32_t lastTarget    = g_target_mV;
  uint32_t settle_t      = 0;
  uint8_t  state         = 0;   // 0=idle 1=settling 2=verifying 3=frozen 4=busy
  bool     pre_discharge = false;
  int      pending_step  = 0;

  for (;;) {
    if (g_cal_sweep_req) { g_cal_sweep_req = false; run_cal_sweep(); lastTarget = 0; state = 0; continue; }

    uint32_t target = g_target_mV;

    // Fresh filtered reading (~200 ms) + rail housekeeping + display median.
    g_measured_mV = vctrl_sample();
    static bool disp_init = false;
    if (!disp_init) { disp_reset(g_measured_mV); disp_init = true; }
    else            { disp_push(g_measured_mV); }
    g_5v_mV  = (uint32_t)read_5V_volt();
    g_3v3_mV = (uint32_t)read_3V3_volt();

    if (target != lastTarget) {                       // new setpoint -> acquire
      lastTarget    = target;
      pre_discharge = false;
      if (target > (uint32_t)DC_V_REF) {
        int cstep      = g_cal_valid ? cal_best_step(target) : -1;
        int start_step = (cstep >= 0) ? cstep : cm_formula_step(target);
        int floor_step = cm_floor_step(target);
        if (start_step < floor_step) start_step = floor_step;
        pending_step   = start_step;
        if (g_measured_mV > target + 300u) {          // way high: discharge first
          dpSetStep(dpMaxSteps - 1);
          pre_discharge = true;
          Dbg.printf("[VCtrl] pre-discharge: meas=%umV tgt=%umV\n", g_measured_mV, target);
        } else {
          dpSetStep(start_step);
          Dbg.printf("[VCtrl] target=%umV start step=%d (%s)\n",
                     target, start_step, g_cal_valid ? "map" : "formula");
        }
      }
      settle_t = millis();
      state = 1; g_vctrl_state = 1;
    }

    if (state == 1) {                                 // settling / pre-discharging
      if (pre_discharge) {
        if (g_measured_mV <= target + 300u) {
          dpSetStep(pending_step);
          pre_discharge = false;
          settle_t = millis();
          Dbg.printf("[VCtrl] pre-discharge done: step=%d\n", pending_step);
        }
      } else if (millis() - settle_t >= 300) {
        state = 2; g_vctrl_state = 2;
      }
    }

    if (state == 2) {                                 // verify + correct once, then freeze
      if (target > (uint32_t)DC_V_REF) {
        int      s  = g_wiper_step;
        uint32_t mv = g_measured_mV;                  // sample from the top of this pass
        g_cal_mv[s] = (mv > 65000u) ? 65000u : (uint16_t)mv;   // map self-heals
        int32_t err = (int32_t)mv - (int32_t)target;
        int cand = cm_correction_step(g_cal_mv, s, cm_floor_step(target), err, target);
        if (cand != s) {
          dpSetStep(cand);
          vTaskDelay(pdMS_TO_TICKS(300));
          uint32_t mv2 = vctrl_sample();
          g_cal_mv[cand] = (mv2 > 65000u) ? 65000u : (uint16_t)mv2;
          uint32_t e1 = (err < 0) ? (uint32_t)(-err) : (uint32_t)err;
          uint32_t e2 = (mv2 > target) ? mv2 - target : target - mv2;
          bool ov1 = mv > target + 300u, ov2 = mv2 > target + 300u;
          if ((!ov2 && (ov1 || e2 <= e1)) || (ov1 && ov2)) { mv = mv2; }  // keep correction; if both overvolt, cand is the lower-V step
          else {                                             // correction made it worse: step back
            dpSetStep(s); vTaskDelay(pdMS_TO_TICKS(300));
            mv = vctrl_sample();
            g_cal_mv[s] = (mv > 65000u) ? 65000u : (uint16_t)mv;   // map self-heal on re-sample too
          }
        }
        g_measured_mV = mv;
        disp_reset(mv);                               // snap display to the new level
        Dbg.printf("[VCtrl] frozen: tgt=%umV meas=%umV step=%d R=%d\n",
                   target, mv, (int)g_wiper_step, (int)dpCalcR());
      }
      state = 3; g_vctrl_state = 3;
    }

    vTaskDelay(pdMS_TO_TICKS(50));   // ~250 ms loop incl. the 200 ms sample
  }
}
