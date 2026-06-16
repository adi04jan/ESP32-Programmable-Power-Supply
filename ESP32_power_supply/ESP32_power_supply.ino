// =============================================================================
//   Voltbench — ESP32-C3 Programmable Power Supply
//   Hardware: LM2596-ADJ (variable), Mini360 5V, LM1117 3.3V
//             MCP4017T-103E/LT (10K, 128-step) I2C digital pot @ 0x2F
//             Voltage divider: VOUT — 10K — FB — MCP4017 — GND
//             ADC divider:     VOUT — 47K:1K → VOLTAGE_READ_PIN_VV (×48 scale)
// =============================================================================
#pragma GCC optimize("Os")   // optimize for size — recovers ~50-80 KB
#include <Wire.h>
#include <SW_MCP4017.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h> // HTTPS pull-OTA (GitHub release assets)
#include <Update.h>          // web-upload OTA (Update.write/end)
#include <ArduinoOTA.h>      // push OTA from arduino-cli / IDE network port
#include "driver/rtc_io.h"
#include <ESPmDNS.h>

#include "web_page.h"       // index_html_gz, index_html_gz_len
#include "credential.h"     // ssids[], passwords[], base_url (initial seed only)

#define CURRENT_FIRMWARE_VERSION "2.0.1"

// Pull-OTA source: this repo's GitHub "latest release" assets. The device fetches
// <base>version.txt, and if it differs from CURRENT_FIRMWARE_VERSION, downloads
// <base>firmware_<latest>.bin. /releases/latest/download/ always resolves to the
// newest release, so each new release is auto-discovered.
#define OTA_GITHUB_URL "https://github.com/adi04jan/ESP32-Programmable-Power-Supply/releases/latest/download/"

#define DC_R2_REF          10000
#define DC_V_REF           1235
#define MCPWIPEROHMS       15
#define VOLTAGE_READ_PIN_VV  0
#define VOLTAGE_READ_PIN_5V  1
#define VOLTAGE_READ_PIN_3V3 3

#define ENABLE_VV_PIN   5
#define ENABLE_5V_PIN   6
#define ENABLE_3V3_PIN  7

#define MCP4017ADDRESS  0x2F

static const uint8_t dpMaxSteps   = 128;
static const int     maxRangeOhms = 10000;

MCP4017 i2cDP(MCP4017ADDRESS, dpMaxSteps, maxRangeOhms);

// Direct MCP4017 wiper control — bypasses SW_MCP4017::setSteps(), which prints
// to Serial 4x on every call (flooding the port and adding jitter in the
// control hot loop). Tracks the wiper step locally so dpCalcR() stays consistent.
static volatile uint16_t g_wiper_step = 0;
static int dpStepForR(float Rout) {
  int s = (int)(((float)dpMaxSteps * (Rout - (float)MCPWIPEROHMS)) / (float)maxRangeOhms);
  return constrain(s, 0, dpMaxSteps - 1);
}
static void dpSetStep(int s) {
  s = constrain(s, 0, dpMaxSteps - 1);
  g_wiper_step = (uint16_t)s;
  Wire.beginTransmission(MCP4017ADDRESS);
  Wire.write((uint8_t)s);
  Wire.endTransmission();
}
static void dpSetR(float Rout) { dpSetStep(dpStepForR(Rout)); }
static float dpCalcR() {
  return ((float)g_wiper_step / dpMaxSteps) * (float)maxRangeOhms + (float)MCPWIPEROHMS;
}

// ---- Output-1 calibration map: measured mV at each wiper step (0 = uncalibrated)
// Built by a one-time sweep (run_cal_sweep), persisted to NVS. Lets set-voltage
// land on the genuinely-closest step instead of trusting the (inaccurate) formula.
uint16_t          g_cal_mv[128]   = {0};
volatile bool     g_cal_valid     = false;
volatile bool     g_cal_sweep_req = false;   // set via API/console to (re)build the map
volatile uint32_t g_display_mV    = 0;       // EMA-smoothed reading for the UI
void cal_save();                              // defined in firmware-additions.h (needs NVS)
void cal_load();

// =============================================================================
//   Remote debug log — fans out to USB Serial + telnet (:23) + browser WS,
//   backed by a ring buffer so a late-connecting client still sees recent
//   output. Producers (any task) only touch Serial + the ring; the telnet/WS
//   consumers are pumped from loop() to respect AsyncTCP's single-task rule.
// =============================================================================
static const int         LOGN     = 3072;
static char              g_log[LOGN];
static volatile uint32_t g_logw   = 0;            // monotonic write counter
static portMUX_TYPE      g_logmux = portMUX_INITIALIZER_UNLOCKED;

class DbgPrint : public Print {
 public:
  size_t write(uint8_t c) override {
    if (Serial) Serial.write(c);                  // guarded: never blocks with no USB host
    portENTER_CRITICAL(&g_logmux);
    g_log[g_logw % LOGN] = (char)c; g_logw++;
    portEXIT_CRITICAL(&g_logmux);
    return 1;
  }
  size_t write(const uint8_t *b, size_t n) override {
    if (Serial) Serial.write(b, n);
    portENTER_CRITICAL(&g_logmux);
    for (size_t i = 0; i < n; i++) { g_log[g_logw % LOGN] = (char)b[i]; g_logw++; }
    portEXIT_CRITICAL(&g_logmux);
    return n;
  }
};
DbgPrint   Dbg;

WiFiServer telnetSrv(23);
WiFiClient telnetCli;
bool       telnetAuthed = false;
uint32_t   g_telnetRead = 0;       // ring cursor for telnet client
uint32_t   g_wsLogRead  = 0;       // ring cursor for browser log WS

AsyncWebServer server(80);

struct State {
  bool  output1  = false;
  bool  output2  = false;
  bool  output3  = false;
  float voltage1 = 2.5f;   // commanded set-point (V)
} psState;

// Shared between web-handler task and voltageControlTask.
// 32-bit aligned reads/writes are atomic on ESP32-C3 RISC-V.
volatile uint32_t g_target_mV    = 2500;
volatile uint32_t g_setpoint_mV  = 2500;
volatile uint32_t g_measured_mV  = 0;
volatile bool     g_ctrl_output1 = false;

// PID state — written by voltageControlTask, read by firmware-additions.h
volatile float    g_pid_kp          = 0.50f;
volatile float    g_pid_ki          = 0.10f;
volatile float    g_pid_kd          = 0.02f;
volatile uint8_t  g_vctrl_state     = 0;    // 0=idle 1=settling 2=pid 3=converged 4=tuning
volatile bool     g_autotune_req    = false;
volatile uint8_t  g_ctrl_mode       = 0;    // 0=quick 1=continuous 2=timed
volatile uint32_t g_ctrl_timeout_ms = 5000;

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

int read_VV_volt() {
  const int sc = 5;
  static uint32_t s[sc] = {0}; static bool init = false;
  return read_any_volt(VOLTAGE_READ_PIN_VV, s, &init, sc) * 48;
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
  uint8_t pin = (output == 1) ? ENABLE_VV_PIN
              : (output == 2) ? ENABLE_5V_PIN
                              : ENABLE_3V3_PIN;
  digitalWrite(pin, state ? HIGH : LOW);
  if (output == 1) { psState.output1 = state; g_ctrl_output1 = state; }
  if (output == 2)   psState.output2 = state;
  if (output == 3)   psState.output3 = state;
}

// Non-blocking: writes globals — voltageControlTask does the actual I2C work
void setVoltage(float voltage) {
  uint32_t mv     = (uint32_t)(voltage * 1000.0f);
  g_target_mV     = mv;
  g_setpoint_mV   = mv;
  psState.voltage1 = voltage;
}

// Send any new ring-buffer bytes (since *cursor) to a stream; skip ahead if the
// consumer fell more than one ring behind.
static void flushLog(Print &out, uint32_t &cursor) {
  uint32_t w = g_logw;
  if (w - cursor > (uint32_t)LOGN) cursor = w - LOGN;
  while (cursor < w) { out.write((uint8_t)g_log[cursor % LOGN]); cursor++; }
}

// Execute one bench-console command, replying on `out`. Shared by USB + telnet.
void console_exec(const String &lineIn, Print &out) {
  String l = lineIn; l.trim();
  if (!l.length()) return;
  char c = l[0];
  if      (c == 'v') { float v = l.substring(1).toFloat(); setVoltage(v); setOutput(1, true);
                       out.printf("[con] set=%dmV out1=on\n", (int)(v * 1000)); }
  else if (l == "o1") { setOutput(1, true);  out.println("[con] out1=on"); }
  else if (l == "o0") { setOutput(1, false); out.println("[con] out1=off"); }
  else if (c == 'm')  { g_cal_sweep_req = true; out.println("[con] calibration sweep requested"); }
  else if (c == 'c')  { out.printf("[con] cal valid=%d\n", g_cal_valid ? 1 : 0); }
  else if (c == '?')  { out.printf("[con] set=%umV meas=%umV disp=%umV state=%u R=%dohm cal=%d\n",
                          g_setpoint_mV, g_measured_mV, g_display_mV, g_vctrl_state, (int)dpCalcR(), g_cal_valid ? 1 : 0); }
  else out.println("[con] cmds: v<volts> o1 o0 m c ?");
}

// =============================================================================
//   voltageControlTask — deterministic best-step voltage setter
//   g_vctrl_state: 0=idle 1=settling 2=acquiring 3=frozen 4=busy(sweep/tune)
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
  return (sum / 50) * 48;
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
  g_display_mV  = 0;
  g_vctrl_state = 0;
  Dbg.println("[Cal] sweep done + saved");
}

// Closest calibrated step to target that doesn't exceed the +300 mV ceiling.
static int cal_best_step(uint32_t target) {
  int best = -1; uint32_t besterr = 0xFFFFFFFFu;
  for (int s = 0; s < dpMaxSteps; s++) {
    if (g_cal_mv[s] == 0) continue;
    if ((uint32_t)g_cal_mv[s] > target + 300u) continue;
    uint32_t e = (target > g_cal_mv[s]) ? (target - g_cal_mv[s]) : (g_cal_mv[s] - target);
    if (e < besterr) { besterr = e; best = s; }
  }
  return best;
}

// pid_autotune() is defined after #include "firmware-additions.h"
bool pid_autotune(uint32_t target);

void voltageControlTask(void* pvParameters) {
  uint32_t lastTarget    = g_target_mV;
  uint32_t settle_t      = 0;
  uint8_t  state         = 0;
  bool     pre_discharge = false;  // waiting for Vout to fall before applying the target step
  int      pending_step  = 0;      // step to apply once pre-discharge completes

  // Lowest step (= max V) allowed for a target — hard ceiling of target+300 mV.
  auto overvolt_floor_step = [](uint32_t tgt_mV) -> int {
    uint32_t ceiling_mV = tgt_mV + 300u;
    if (ceiling_mV <= (uint32_t)DC_V_REF) return dpMaxSteps - 1;
    float r = (float)DC_R2_REF * (float)DC_V_REF / (float)(ceiling_mV - DC_V_REF) - (float)MCPWIPEROHMS;
    return dpStepForR(r > 0.0f ? r : 0.0f);
  };
  // Formula step for a target (fallback when the calibration map isn't built).
  auto formula_step = [](uint32_t tgt_mV) -> int {
    uint32_t safe = (tgt_mV > (uint32_t)(DC_V_REF + 300)) ? (tgt_mV - 300u) : tgt_mV;
    long r = (long)((DC_R2_REF * DC_V_REF) / (safe - DC_V_REF)) - MCPWIPEROHMS;
    return dpStepForR(r > 0 ? (float)r : 0.0f);
  };

  for (;;) {
    if (g_cal_sweep_req) { g_cal_sweep_req = false; run_cal_sweep(); lastTarget = 0; state = 0; continue; }

    if (g_autotune_req) {
      g_autotune_req = false;
      g_vctrl_state  = 4;
      pid_autotune(g_target_mV);
      pre_discharge = false;
      lastTarget = 0;
      state = 0; g_vctrl_state = 0;
      continue;
    }

    uint32_t target = g_target_mV;

    // Fresh filtered reading; EMA (a=1/4) feeds the smoothed display value.
    g_measured_mV = vctrl_sample();
    if (g_display_mV == 0) {
      g_display_mV = g_measured_mV;
    } else {
      int32_t d = (int32_t)g_display_mV;
      d += ((int32_t)g_measured_mV - d) / 4;
      g_display_mV = (uint32_t)d;
    }

    if (target != lastTarget) {
      lastTarget    = target;
      pre_discharge = false;

      if (target > (uint32_t)DC_V_REF) {
        // Centre step: calibration map if built, else formula. Clamp to ceiling.
        int cstep      = g_cal_valid ? cal_best_step(target) : -1;
        int start_step = (cstep >= 0) ? cstep : formula_step(target);
        int floor_step = overvolt_floor_step(target);
        if (start_step < floor_step) start_step = floor_step;
        pending_step   = start_step;

        if (g_measured_mV > target + 300u) {   // discharge first if currently too high
          dpSetStep(dpMaxSteps - 1);           // max R -> lowest V
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

    // State 1: settle (or wait for pre-discharge to complete)
    if (state == 1) {
      if (pre_discharge) {
        if (g_measured_mV <= target + 300u) {
          dpSetStep(pending_step);
          pre_discharge = false;
          settle_t = millis();  // restart settle timer from here
          Dbg.printf("[VCtrl] pre-discharge done: meas=%umV step=%d\n", g_measured_mV, pending_step);
        }
        // else keep sampling until voltage drops — don't advance state
      } else if (millis() - settle_t >= 300) {
        state = 2; g_vctrl_state = 2;
      }
    }

    if (state == 2) {
      if (target > (uint32_t)DC_V_REF) {
        // Trim ±2 steps (±1 without a map) around the centre; pick the closest
        // measured step that doesn't overvolt, then FREEZE. Each live read also
        // refreshes the map so it self-corrects for drift over time.
        int centre     = g_wiper_step;
        int floor_step = overvolt_floor_step(target);
        int span       = g_cal_valid ? 2 : 1;
        int lo = constrain(centre - span, floor_step, dpMaxSteps - 1);
        int hi = constrain(centre + span, 0, dpMaxSteps - 1);

        int      pick = hi;                       // default: lowest V in window (safe)
        uint32_t pick_e = 0xFFFFFFFFu; bool pick_set = false;
        for (int s = lo; s <= hi; s++) {
          dpSetStep(s);
          vTaskDelay(pdMS_TO_TICKS(200));         // settle the small move
          uint32_t mv = vctrl_sample();
          g_cal_mv[s] = (mv > 65000u) ? 65000u : (uint16_t)mv;   // keep the map fresh
          if (mv > target + 300u) continue;       // never freeze on an overvolt step
          uint32_t e = (target > mv) ? (target - mv) : (mv - target);
          if (!pick_set || e < pick_e) { pick_e = e; pick = s; pick_set = true; }
        }
        dpSetStep(pick);
        g_measured_mV = vctrl_sample();
        g_display_mV  = g_measured_mV;            // snap display to the new level
        Dbg.printf("[VCtrl] frozen: tgt=%umV meas=%umV step=%d R=%d\n",
                      target, g_measured_mV, pick, (int)dpCalcR());
        state = 3; g_vctrl_state = 3;
      } else {
        state = 3; g_vctrl_state = 3;
      }
    }

    // Short idle delay -> the live reading (200 ms filtered sample) refreshes
    // ~every 250 ms so the UI updates in real time.
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// =============================================================================
//   OTA — uses ESP32 core HTTPUpdate (HTTP or HTTPS, no extra library needed)
// =============================================================================
void perform_ota(bool force, bool verify_ssl, const String &ota_url) {
  bool   https       = ota_url.startsWith("https");
  String version_url = ota_url + "version.txt";

  // --- version check (GitHub 302-redirects assets to a different host) ---
  String latest;
  {
    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    bool begun;
    WiFiClientSecure sec; WiFiClient plain;
    if (https) { if (!verify_ssl) sec.setInsecure(); begun = http.begin(sec, version_url); }
    else       { begun = http.begin(plain, version_url); }
    if (!begun) { Dbg.println("OTA: version begin failed"); return; }
    int code = http.GET();
    if (code != 200) { http.end(); Dbg.printf("OTA: version.txt HTTP %d\n", code); return; }
    latest = http.getString(); latest.trim(); http.end();
  }

  Dbg.printf("OTA: current=%s latest=%s\n", CURRENT_FIRMWARE_VERSION, latest.c_str());
  if (latest.isEmpty()) { Dbg.println("OTA: empty version, abort"); return; }
  if (!force && latest == CURRENT_FIRMWARE_VERSION) { Dbg.println("OTA: already up-to-date"); return; }

  // --- download + flash firmware_<latest>.bin ---
  String firmware_url = ota_url + "firmware_" + latest + ".bin";
  Dbg.printf("OTA: downloading %s\n", firmware_url.c_str());
  httpUpdate.rebootOnUpdate(true);
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  t_httpUpdate_return ret;
  if (https) {
    WiFiClientSecure sec; if (!verify_ssl) sec.setInsecure();
    ret = httpUpdate.update(sec, firmware_url);
  } else {
    WiFiClient plain;
    ret = httpUpdate.update(plain, firmware_url);
  }
  if (ret == HTTP_UPDATE_FAILED)
    Dbg.printf("OTA: failed (%d) %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
  else if (ret == HTTP_UPDATE_NO_UPDATES)
    Dbg.println("OTA: server reports no update");
}

// =============================================================================
//   Voltbench: NVS, gzip serving, WebSocket, captive portal, guards, MQTT
//   Include AFTER all shared symbols are defined above.
// =============================================================================
#include "firmware-additions.h"

// =============================================================================
//   Relay-feedback auto-tune (Åström-Hägglund)
//   Called from voltageControlTask (core 0) when g_autotune_req is set.
//   Computes Ziegler-Nichols Kp/Ki/Kd and persists them to NVS.
// =============================================================================
bool pid_autotune(uint32_t target) {
  if (target <= (uint32_t)DC_V_REF) return false;

  long r_cl = (long)((DC_R2_REF * DC_V_REF) / (target - DC_V_REF)) - MCPWIPEROHMS;
  if (r_cl < 0) r_cl = 0;
  float rc = (float)r_cl;

  // Relay amplitude: aim for ~150 mV voltage swing, clamped 2–8 MCP4017 steps
  const float STEP = 79.0f;
  float relay_r = constrain(150.0f * rc * rc / ((float)DC_V_REF * (float)DC_R2_REF),
                            STEP * 2, STEP * 8);
  // Formula set + settle
  dpSetR((uint32_t)rc);
  vTaskDelay(pdMS_TO_TICKS(800));

  const int MAX_CROSS = 8;
  uint32_t  cross_t[MAX_CROSS];
  int       n_cross = 0;
  bool      relay_up = true;
  uint32_t  meas_max = 0, meas_min = 0xFFFFFFFFu;
  bool      tracking = false;
  uint32_t  t_start  = millis();

  dpSetR((uint32_t)constrain(rc - relay_r, 0.0f, (float)DC_R2_REF));

  while (n_cross < MAX_CROSS && millis() - t_start < 30000) {
    vTaskDelay(pdMS_TO_TICKS(10));
    uint32_t sum = 0;
    for (int s = 0; s < 4; s++) {
      sum += analogReadMilliVolts(VOLTAGE_READ_PIN_VV);
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    uint32_t meas = (sum / 4) * 48;
    g_measured_mV = meas;
    if (tracking) {
      if (meas > meas_max) meas_max = meas;
      if (meas < meas_min) meas_min = meas;
    }
    if (relay_up && meas >= target) {
      relay_up = false;
      dpSetR((uint32_t)constrain(rc + relay_r, 0.0f, (float)DC_R2_REF));
      cross_t[n_cross++] = millis(); tracking = true;
    } else if (!relay_up && meas < target) {
      relay_up = true;
      dpSetR((uint32_t)constrain(rc - relay_r, 0.0f, (float)DC_R2_REF));
      cross_t[n_cross++] = millis();
    }
  }

  dpSetR((uint32_t)rc);
  if (n_cross < 4 || meas_max <= meas_min) return false;

  float Tu_sum = 0; int Tu_n = 0;
  for (int i = 2; i < n_cross; i += 2) { Tu_sum += cross_t[i] - cross_t[i-2]; Tu_n++; }
  if (Tu_n == 0) return false;
  float Tu = Tu_sum / Tu_n / 1000.0f;
  float Au = (meas_max - meas_min) / 2.0f;
  if (Tu < 0.2f || Au < 30.0f) return false;

  float d_v = relay_r * (float)DC_V_REF * (float)DC_R2_REF / (rc * rc);
  float Ku  = 4.0f * d_v / (3.14159265f * Au);

  g_pid_kp = 0.6f * Ku;
  g_pid_ki = 1.2f * Ku / Tu;
  g_pid_kd = 0.075f * Ku * Tu;
  vb.pid_kp = g_pid_kp; vb.pid_ki = g_pid_ki; vb.pid_kd = g_pid_kd;
  vb.pid_tuned = true;
  vb_save();
  // Print gains as integers (×1000) to avoid pulling in float-printf
  Serial.printf("[AutoTune] Kp=%d Ki=%d Kd=%d (x1000)\n",
    (int)(g_pid_kp*1000), (int)(g_pid_ki*1000), (int)(g_pid_kd*1000));
  return true;
}

static bool ota_force_flag = false;
static void ota_task_fn(void *pv) {
  perform_ota(ota_force_flag, vb.ota_ssl, vb.ota_url);
  ota_force_flag = false;
  vTaskDelete(NULL);
}
// 16 KB stack — TLS (WiFiClientSecure) handshake/buffers are stack-hungry.
void perform_ota_tasked(bool force) {
  ota_force_flag = force;
  xTaskCreatePinnedToCore(ota_task_fn, "otaTask", 16384, NULL, 5, NULL, 0);
}

// =============================================================================
//   Remote debug + OTA pumps (run from loop(): single-task, AsyncTCP-safe)
// =============================================================================
static bool g_ota_begun = false;
void arduino_ota_begin_if_ready() {
  if (g_ota_begun || WiFi.status() != WL_CONNECTED) return;
  g_ota_begun = true;                         // set first: never retry if begin misbehaves
  ArduinoOTA.setMdnsEnabled(false);           // wifiMgrTask owns MDNS; ArduinoOTA re-entering it deadlocks
  ArduinoOTA.setHostname(vb.mdns.c_str());
  if (vb.password.length()) ArduinoOTA.setPassword(vb.password.c_str());
  ArduinoOTA.onStart([]()           { Dbg.println("[OTA] arduino-ota start"); });
  ArduinoOTA.onEnd([]()             { Dbg.println("[OTA] done -> reboot"); });
  ArduinoOTA.onError([](ota_error_t e){ Dbg.printf("[OTA] error %d\n", (int)e); });
  ArduinoOTA.onProgress([](unsigned p, unsigned t) {
    static int last = -1; int pct = t ? (int)(p * 100 / t) : 0;
    if (pct != last && pct % 20 == 0) { last = pct; Dbg.printf("[OTA] %d%%\n", pct); }
  });
  ArduinoOTA.begin();
  g_ota_begun = true;
  Dbg.printf("[OTA] ArduinoOTA ready (arduino-cli -p %s.local)\n", vb.mdns.c_str());
}

void remote_debug_pump() {
  // --- Telnet (:23): live log stream + password-gated console ---
  if (telnetSrv.hasClient()) {
    if (telnetCli && telnetCli.connected()) { telnetCli.println("\n[telnet] superseded"); telnetCli.stop(); }
    telnetCli = telnetSrv.available();
    telnetCli.setNoDelay(true);
    telnetAuthed = !vb.require_login;
    g_telnetRead = (g_logw > (uint32_t)LOGN) ? g_logw - LOGN : 0;   // begin with recent history
    telnetCli.printf("Voltbench v%s debug. ", CURRENT_FIRMWARE_VERSION);
    telnetCli.println(telnetAuthed ? "cmds: v<volts> o1 o0 m c ?" : "password:");
  }
  if (telnetCli && telnetCli.connected()) {
    flushLog(telnetCli, g_telnetRead);
    static String tin;
    while (telnetCli.available()) {
      char ch = (char)telnetCli.read();
      if (ch == '\n' || ch == '\r') {
        if (tin.length()) {
          if (!telnetAuthed) {
            telnetAuthed = (tin == vb.password);
            telnetCli.println(telnetAuthed ? "ok. cmds: v<volts> o1 o0 m c ?" : "bad password");
          } else console_exec(tin, telnetCli);
          tin = "";
        }
      } else if (tin.length() < 64) tin += ch;
    }
  }
  // --- Browser log WebSocket (view-only) ---
  if (logws.count()) {
    uint32_t w = g_logw;
    if (w - g_wsLogRead > (uint32_t)LOGN) g_wsLogRead = w - LOGN;
    if (w > g_wsLogRead) {
      String s; s.reserve(w - g_wsLogRead);
      while (g_wsLogRead < w) { s += g_log[g_wsLogRead % LOGN]; g_wsLogRead++; }
      logws.textAll(s);
    }
  } else {
    g_wsLogRead = g_logw;     // no viewers -> don't build a backlog
  }
}

// =============================================================================
//   Arduino entry points
// =============================================================================
static const uint8_t total_ssid_count = sizeof(ssids) / sizeof(ssids[0]);

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);   // never block on USB-CDC writes when no host is reading
#endif
  Serial.println();
  Serial.println("Voltbench v" CURRENT_FIRMWARE_VERSION " booting...");

  pinMode(ENABLE_VV_PIN,  OUTPUT);
  pinMode(ENABLE_5V_PIN,  OUTPUT);
  pinMode(ENABLE_3V3_PIN, OUTPUT);
  digitalWrite(ENABLE_VV_PIN,  LOW);
  digitalWrite(ENABLE_5V_PIN,  LOW);
  digitalWrite(ENABLE_3V3_PIN, LOW);

  analogSetAttenuation(ADC_11db);   // 0–2500 mV input → supports output up to ~27.5 V
  analogReadResolution(12);
  Wire.begin();
  Wire.beginTransmission(MCP4017ADDRESS);
  Serial.println(Wire.endTransmission() == 0 ? "MCP4017 OK @ 0x2F" : "WARNING: MCP4017 not found!");

  // Start non-blocking voltage control task on core 0 before anything else
  xTaskCreatePinnedToCore(voltageControlTask, "VoltCtrl", 4096, nullptr, 2, nullptr, 0);

  // Load NVS settings first so we can seed WiFi credentials
  vb_load();
  cal_load();   // restore the output-1 calibration map if one was saved

  // Always refresh credential.h entries so hardcoded credentials stay current
  for (uint8_t i = 0; i < min((int)total_ssid_count, (int)VBSettings::MAX_SSID); i++)
    vb_remember_wifi(ssids[i], passwords[i]);

  // Pull-OTA source = this repo's GitHub latest-release assets (HTTPS, insecure
  // TLS — no CA bundle embedded; pin a cert later for tamper-proofing).
  if (vb.ota_url != OTA_GITHUB_URL || vb.ota_ssl) {
    vb.ota_url = OTA_GITHUB_URL;
    vb.ota_ssl = false;
    vb_save();
  }

  // Voltbench: load NVS again (now has migrated creds), try WiFi, register all /api/* routes
  vb_setup();

  server.begin();
  telnetSrv.begin();           // remote debug console on :23
  telnetSrv.setNoDelay(true);
  Dbg.println("HTTP server started");
  if (!vb_in_captive)
    Dbg.printf("URL: http://%s.local/  (telnet %s.local:23)\n", vb.mdns.c_str(), vb.mdns.c_str());

  // Auto-OTA check runs AFTER the server is up, in its own task, so a slow
  // HTTPS round-trip to GitHub never delays boot / the web UI.
  if (!vb_in_captive && vb.ota_auto)
    perform_ota_tasked(false);
}

void loop() {
  vb_loop();                      // DNS captive, WebSocket push, MQTT, OCP/OTP guards
  // NOTE: ArduinoOTA disabled — its begin()/handle() hangs the loop alongside
  // AsyncTCP. Web-upload OTA (/api/ota/upload) is the working WiFi-flash path.
  // arduino_ota_begin_if_ready();
  // ArduinoOTA.handle();
  remote_debug_pump();            // telnet + browser log stream

  // Bench console over USB (same commands as telnet): v<volts> o1 o0 m c ?
  if (Serial.available()) {
    String l = Serial.readStringUntil('\n');
    console_exec(l, Dbg);
  }
  // Periodic telemetry while output 1 is on -> fans out to USB, telnet, web log.
  static uint32_t t_con = 0;
  if (psState.output1 && millis() - t_con > 1000) {
    t_con = millis();
    Dbg.printf("[con] set=%umV meas=%umV disp=%umV state=%u R=%dohm\n",
               g_setpoint_mV, g_measured_mV, g_display_mV, g_vctrl_state, (int)dpCalcR());
  }
  delay(2);
}
