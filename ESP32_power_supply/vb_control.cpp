#include "vb_control.h"
#include "vb_debug.h"
#include "vb_nvs.h"          // cal_save() — persists the swept map
#include "control_math.h"    // pure control math, host-tested (tools/host_test/)
#include <Wire.h>
#include <esp_system.h>      // esp_reset_reason()

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
//   Change log — last VB_LOG_N output/setpoint changes with their source.
//   RAM only: it answers "who switched it?" for the current boot.
// =============================================================================
enum : uint8_t { LG_BOOT, LG_SET, LG_ON, LG_OFF, LG_ALLOFF, LG_CYCLE, LG_SWEEP,
                 LG_SWEEP_ABORT, LG_AUTO_ON, LG_AUTO_FAIL, LG_RESTORE };
static const char *const LG_NAME[]  = {"boot","set","on","off","all_off","cycle","sweep",
                                       "sweep_abort","auto_on","auto_on_fail","restore"};
static const char *const SRC_NAME[] = {"sys","usb","telnet","http","ws","mqtt"};

#define VB_LOG_N 32
struct LogEnt { uint32_t ms; int32_t val; uint8_t src, act, ch; };
static LogEnt   g_lg[VB_LOG_N];
static uint32_t g_lg_n = 0;                       // total ever written
static portMUX_TYPE g_lg_mux = portMUX_INITIALIZER_UNLOCKED;

static void lg(uint8_t src, uint8_t act, uint8_t ch, int32_t val) {
  portENTER_CRITICAL(&g_lg_mux);
  g_lg[g_lg_n % VB_LOG_N] = { millis(), val, src, act, ch };
  g_lg_n++;
  portEXIT_CRITICAL(&g_lg_mux);
  Dbg.printf("[chg] %s %s ch=%u val=%ld\n", SRC_NAME[src % 6], LG_NAME[act], ch, (long)val);
}

int vb_log_count() { return g_lg_n < VB_LOG_N ? (int)g_lg_n : VB_LOG_N; }

bool vb_log_line(int i, char *buf, size_t n) {
  int cnt = vb_log_count();
  if (i < 0 || i >= cnt) return false;
  portENTER_CRITICAL(&g_lg_mux);
  LogEnt e = g_lg[(g_lg_n - cnt + i) % VB_LOG_N];
  portEXIT_CRITICAL(&g_lg_mux);
  snprintf(buf, n, "%lu.%03lu %s %s ch=%u val=%ld",
           (unsigned long)(e.ms / 1000), (unsigned long)(e.ms % 1000),
           SRC_NAME[e.src % 6], LG_NAME[e.act], e.ch, (long)e.val);
  return true;
}

const char *vb_ctl_name(uint8_t s) {
  static const char *const N[] = {"idle","settling","verifying","frozen","busy"};
  return N[s > 4 ? 4 : s];
}

const char *vb_reset_reason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "poweron";
    case ESP_RST_SW:       return "sw";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP:return "deepsleep";
    case ESP_RST_EXT:      return "ext";
    case ESP_RST_USB:      return "usb";
    case ESP_RST_JTAG:     return "jtag";
    default:               return "other";
  }
}

// =============================================================================
//   Output switching + deferred re-enable
//
//   cycle, the post-sweep re-enable and restore-after-reboot all queue a
//   channel here; the control task (out_service) switches it on once the rail
//   is proven right. An explicit toggle/all_off cancels the queue under the same
//   lock as the switch itself, so the host's last word always wins.
// =============================================================================
static portMUX_TYPE g_out_mux   = portMUX_INITIALIZER_UNLOCKED;
static uint8_t      g_pend_mask = 0;              // bit (ch-1)
static uint32_t     g_pend_at[3];                 // millis() not-before
static uint8_t      g_pend_src[3];
volatile bool       g_sweeping  = false;

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

// Switch + cancel any queued re-enable, atomically.
static void out_set(uint8_t ch, bool on) {
  portENTER_CRITICAL(&g_out_mux);
  g_pend_mask &= ~(1u << (ch - 1));
  setOutput(ch, on);
  portEXIT_CRITICAL(&g_out_mux);
}

static void out_pend(uint8_t ch, uint32_t delay_ms, uint8_t src) {
  portENTER_CRITICAL(&g_out_mux);
  g_pend_mask |= 1u << (ch - 1);
  g_pend_at[ch - 1]  = millis() + delay_ms;
  g_pend_src[ch - 1] = src;
  portEXIT_CRITICAL(&g_out_mux);
}

uint8_t vb_pending_mask() { return g_pend_mask; }

// Rail proven right? CH1: controller settled on the CURRENT target and the
// (pre-switch) sense within 500 mV of it. CH2/CH3: fixed rails in tolerance.
static bool out_ready(uint8_t ch) {
  if (ch == 1) {
    uint32_t t = g_target_mV, m = g_measured_mV;
    return g_vctrl_state == 3 && !g_sweeping && (m > t ? m - t : t - m) <= 500;
  }
  if (ch == 2) return g_5v_mV  >= 4500 && g_5v_mV  <= 5500;
  return              g_3v3_mV >= 3000 && g_3v3_mV <= 3600;
}

static void out_service() {
  for (uint8_t ch = 1; ch <= 3; ch++) {
    uint8_t bit = 1u << (ch - 1);
    if (!(g_pend_mask & bit)) continue;
    int32_t late = (int32_t)(millis() - g_pend_at[ch - 1]);
    if (late < 0) continue;
    bool ready = out_ready(ch), fired = false, failed = false;
    uint8_t src = g_pend_src[ch - 1];
    portENTER_CRITICAL(&g_out_mux);
    if (g_pend_mask & bit) {                      // re-check: a host may have cancelled
      if (ready)              { g_pend_mask &= ~bit; setOutput(ch, true); fired = true; }
      else if (late > 10000)  { g_pend_mask &= ~bit; failed = true; }
    }
    portEXIT_CRITICAL(&g_out_mux);
    uint32_t rail = ch == 1 ? g_measured_mV : ch == 2 ? g_5v_mV : g_3v3_mV;
    if (fired)  lg(src, LG_AUTO_ON,   ch, (int32_t)rail);
    if (failed) lg(src, LG_AUTO_FAIL, ch, (int32_t)rail);   // left OFF: never switch onto a wrong rail
  }
}

// Persist setpoint + outputs for restore-after-reboot, 2 s after they stop
// changing (a 0.2 V/s ramp costs one write at the end, not one per step).
static void out_persist() {
  static bool     init = false;
  static uint32_t seen_mv, seen_at, saved_mv;
  static uint8_t  seen_mask, saved_mask;
  if (!init) { saved_mv = seen_mv = vb.r_mv; saved_mask = seen_mask = vb.r_out; seen_at = millis(); init = true; }
  if (g_pend_mask || g_sweeping) return;          // mid-cycle/restore is not a state worth keeping
  uint8_t  mask = (psState.output1 ? 1 : 0) | (psState.output2 ? 2 : 0) | (psState.output3 ? 4 : 0);
  uint32_t mv   = g_target_mV;
  if (mv != seen_mv || mask != seen_mask) { seen_mv = mv; seen_mask = mask; seen_at = millis(); return; }
  if (mv == saved_mv && mask == saved_mask) return;
  if (millis() - seen_at < 2000) return;
  vb_save_outstate(mv, mask);
  saved_mv = mv; saved_mask = mask;
}

// Setpoint only. Writes the target BEFORE flagging "settling", so the control
// task can never re-publish "frozen" for the previous target after a host saw
// the new one accepted (the documented "poll until frozen" recipe relies on it).
bool setVoltageMv(uint32_t mv) {
  if (mv < VB_VMIN_MV || mv > VB_VMAX_MV) return false;
  bool changed  = (mv != g_target_mV);
  g_target_mV   = mv;
  g_setpoint_mV = mv;
  psState.voltage1 = mv / 1000.0f;
  if (changed && !g_sweeping) g_vctrl_state = 1;
  return true;
}

bool setVoltage(float volts) {
  if (!(volts >= 0.0f) || volts > 100.0f) return false;     // NaN / negative / absurd
  return setVoltageMv((uint32_t)lroundf(volts * 1000.0f));
}

// Single command dispatcher shared by WS / HTTP / USB+telnet console / MQTT.
bool apply_command(const String &action, uint8_t ch, float val, uint8_t src) {
  if (isnan(val) && action != "all_off") return false;   // unparseable value from a transport
  if (action == "toggle") {
    if (ch < 1 || ch > 3) return false;
    bool on = val >= 0.5f;
    if (on && ch == 1 && g_sweeping) return false;   // the sweep owns output 1
    out_set(ch, on);
    lg(src, on ? LG_ON : LG_OFF, ch, 0);
    return true;
  }
  if (action == "set_voltage") {
    if (ch != 1 || !setVoltage(val)) return false;
    lg(src, LG_SET, 1, (int32_t)g_target_mV);
    return true;
  }
  if (action == "all_off") {
    for (uint8_t c = 1; c <= 3; c++) out_set(c, false);
    lg(src, LG_ALLOFF, 0, 0);
    return true;
  }
  if (action == "cycle") {                 // off now, back on after val seconds (rail-checked)
    if (ch < 1 || ch > 3 || !(val >= 1.0f && val <= 3600.0f)) return false;
    if (ch == 1 && g_sweeping) return false;
    out_set(ch, false);
    out_pend(ch, (uint32_t)lroundf(val * 1000.0f), src);
    lg(src, LG_CYCLE, ch, (int32_t)lroundf(val));
    return true;
  }
  return false;
}

void out_boot() {
  lg(SRC_SYS, LG_BOOT, 0, (int32_t)esp_reset_reason());
  if (!vb.restore || !vb.r_out) return;
  if (!setVoltageMv(vb.r_mv)) { Dbg.printf("[restore] saved setpoint %lu mV invalid — not restoring\n", (unsigned long)vb.r_mv); return; }
  for (uint8_t ch = 1; ch <= 3; ch++)
    if (vb.r_out & (1u << (ch - 1))) out_pend(ch, 0, SRC_SYS);
  lg(SRC_SYS, LG_RESTORE, 0, (int32_t)vb.r_mv);
  Dbg.printf("[restore] set=%lumV, outputs mask=%u queued (switch on only once the rail checks out)\n",
             (unsigned long)vb.r_mv, vb.r_out);
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

// Build the step->mV calibration map by sweeping the wiper.
//
// Output 1 is switched OFF for the whole sweep. All three ADC dividers tap the
// regulator outputs BEFORE the P-FET switches (schematic net N$1; confirmed live:
// CH3 switched off still reads 3.35 V), so the sweep measures an unloaded rail
// and nothing downstream ever sees the ~2.5 -> 16.5 V ramp. (It used to force
// output 1 ON — straight into whatever was attached — and save a loaded map.)
//
// Sweeps into a scratch map and only commits it if it looks like a real rail,
// so a dead divider or a miswired board can never overwrite a good map.
void run_cal_sweep() {
  static uint16_t tmp[128];
  bool was_on = psState.output1;
  g_sweeping = true;
  out_set(1, false);
  g_vctrl_state = 4;                          // "busy" for the UI
  lg(SRC_SYS, LG_SWEEP, 1, was_on ? 1 : 0);
  Dbg.println("[Cal] sweep start (output 1 OFF)");
  memset(tmp, 0, sizeof(tmp));
  dpSetStep(dpMaxSteps - 1);                  // max R -> lowest V
  vTaskDelay(pdMS_TO_TICKS(400));
  bool ok = true;
  uint32_t top = 0;
  for (int s = dpMaxSteps - 1; s >= 0; s--) {
    dpSetStep(s);
    vTaskDelay(pdMS_TO_TICKS(120));           // settle (rising V is fast, no load)
    uint32_t mv = (vctrl_sample() + vctrl_sample()) / 2;   // ~400 ms average for a cleaner map
    tmp[s] = (mv > 65000u) ? 65000u : (uint16_t)mv;
    if (mv > top) top = mv;
    if (s == dpMaxSteps - 1 && mv < 1000) {   // not seeing the regulator at all
      ok = false; break;
    }
    if (mv > 16500u) {                        // railed near Vin ceiling — fill rest & stop
      for (int j = s - 1; j >= 0; j--) tmp[j] = tmp[s];
      break;
    }
  }
  if (ok && top < (uint32_t)tmp[dpMaxSteps - 1] + 3000u) ok = false;   // never rose: not a rail
  dpSetStep(dpMaxSteps - 1);                  // park at lowest V; the task re-acquires the target
  if (ok) {
    memcpy(g_cal_mv, tmp, sizeof(g_cal_mv));
    cal_smooth_monotonic();                   // isotonic clean-up before use/save
    g_cal_valid = true;
    cal_save();
    Dbg.printf("[Cal] sweep done + saved: %u..%u mV\n", g_cal_mv[dpMaxSteps - 1], g_cal_mv[0]);
  } else {
    lg(SRC_SYS, LG_SWEEP_ABORT, 1, (int32_t)tmp[dpMaxSteps - 1]);
    Dbg.printf("[Cal] sweep ABORTED, map unchanged (low=%u top=%lu mV)\n",
               tmp[dpMaxSteps - 1], (unsigned long)top);
  }
  disp_reset(0);
  g_vctrl_state = 0;
  g_sweeping = false;
  if (was_on) out_pend(1, 0, SRC_SYS);       // back on once re-acquired and checked
}

// Closest calibrated step to target that doesn't exceed the +300 mV ceiling.
static int cal_best_step(uint32_t target) { return cm_best_step(g_cal_mv, target); }

void voltageControlTask(void* pvParameters) {
  // 0 forces an acquire on the first pass. After a SOFTWARE reboot (OTA,
  // /api/reboot) the MCP4017 keeps its old wiper, so the regulator would sit at
  // the pre-reboot voltage while status reported the boot default.
  uint32_t lastTarget  = 0;
  uint32_t settle_t    = 0;
  uint8_t  state       = 0;   // 0=idle 1=settling 2=verifying 3=frozen 4=busy
  bool     discharging = false;

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
      lastTarget = target;
      int cstep      = g_cal_valid ? cal_best_step(target) : -1;
      int start_step = (cstep >= 0) ? cstep : cm_formula_step(target);
      int floor_step = cm_floor_step(target);
      if (start_step < floor_step) start_step = floor_step;
      // Straight to the predicted step, also when going DOWN. The old
      // "pre-discharge" parked the pot at its lowest step first; the LM2596
      // can't sink current, so that bought nothing and let a loaded output
      // collapse (2.9 V seen on a 15.0 -> 13.5 V move — gateway brownout).
      dpSetStep(start_step);
      discharging = g_measured_mV > target + 300u;     // falling: wait before verifying
      Dbg.printf("[VCtrl] target=%umV step=%d (%s)%s\n", target, start_step,
                 g_cal_valid ? "map" : "formula", discharging ? " falling" : "");
      settle_t = millis();
      state = 1; g_vctrl_state = 1;
    }

    if (state == 1) {                                 // settling
      if (discharging) {
        if (g_measured_mV <= target + 300u) { discharging = false; settle_t = millis(); }
        else if (millis() - settle_t > 5000) {
          // Unloaded output still bleeding down through the divider: the step
          // is the predicted one; "correcting" against a stale high reading
          // would only move it the wrong way. Freeze as-is.
          state = 3;
          if (g_target_mV == target) g_vctrl_state = 3;
          Dbg.printf("[VCtrl] frozen (still falling): tgt=%umV meas=%umV\n", target, g_measured_mV);
        }
      } else if (millis() - settle_t >= 300) {
        state = 2; g_vctrl_state = 2;
      }
    }

    if (state == 2) {                                 // verify + correct once, then freeze
      // The map is NOT updated here any more. These readings are taken with
      // whatever load is attached, and the high-Z divider under-reads under
      // load; "self-healing" the map with them made the next setpoint land
      // a full step high. The map changes only by sweep.
      int      s  = g_wiper_step;
      uint32_t mv = g_measured_mV;                    // sample from the top of this pass
      int32_t err = (int32_t)mv - (int32_t)target;
      int cand = cm_correction_step(g_cal_mv, s, cm_floor_step(target), err, target);
      if (cand != s) {
        dpSetStep(cand);
        vTaskDelay(pdMS_TO_TICKS(300));
        uint32_t mv2 = vctrl_sample();
        uint32_t e1 = (err < 0) ? (uint32_t)(-err) : (uint32_t)err;
        uint32_t e2 = (mv2 > target) ? mv2 - target : target - mv2;
        bool ov1 = mv > target + 300u, ov2 = mv2 > target + 300u;
        if ((!ov2 && (ov1 || e2 <= e1)) || (ov1 && ov2)) { mv = mv2; }  // keep correction; if both overvolt, cand is the lower-V step
        else {                                             // correction made it worse: step back
          dpSetStep(s); vTaskDelay(pdMS_TO_TICKS(300));
          mv = vctrl_sample();
        }
      }
      g_measured_mV = mv;
      disp_reset(mv);                                 // snap display to the new level
      Dbg.printf("[VCtrl] frozen: tgt=%umV meas=%umV step=%d R=%d\n",
                 target, mv, (int)g_wiper_step, (int)dpCalcR());
      state = 3;
      if (g_target_mV == target) g_vctrl_state = 3;  // a newer target is already "settling"
    }

    out_service();
    out_persist();
    vTaskDelay(pdMS_TO_TICKS(50));   // ~250 ms loop incl. the 200 ms sample
  }
}
