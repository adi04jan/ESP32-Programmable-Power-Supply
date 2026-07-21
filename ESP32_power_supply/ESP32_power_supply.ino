// =============================================================================
//   Voltbench — ESP32-C3 Programmable Power Supply
//   Hardware: LM2596-ADJ (variable), Mini360 5V, LM1117 3.3V
//             MCP4017T-103E/LT (10K, 128-step) I2C digital pot @ 0x2F
//             Voltage divider: VOUT — 10K — FB — MCP4017 — GND
//             ADC divider:     VOUT — 47K:1K → VOLTAGE_READ_PIN_VV (×48 scale)
// =============================================================================
#pragma GCC optimize("Os")   // optimize for size — recovers ~50-80 KB
#include <Wire.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h> // HTTPS pull-OTA (GitHub release assets)
#include <Update.h>          // web-upload OTA (Update.write/end)
#include "driver/rtc_io.h"
#include <ESPmDNS.h>

#include "web_page.h"       // index_html_gz, index_html_gz_len
#include "credential.h"     // ssids[], passwords[], base_url (initial seed only)
#include "control_math.h"   // pure control math, host-tested (tools/host_test/)

#define CURRENT_FIRMWARE_VERSION "2.1.0"

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
static float dpCalcR() {
  return ((float)g_wiper_step / dpMaxSteps) * (float)maxRangeOhms + (float)MCPWIPEROHMS;
}

// ---- Output-1 calibration map: measured mV at each wiper step (0 = uncalibrated)
// Built by a one-time sweep (run_cal_sweep), persisted to NVS. Lets set-voltage
// land on the genuinely-closest step instead of trusting the (inaccurate) formula.
uint16_t          g_cal_mv[128]   = {0};
volatile bool     g_cal_valid     = false;
volatile bool     g_cal_sweep_req = false;   // set via API/console to (re)build the map
volatile uint32_t g_display_mV    = 0;       // median-of-5 reading for the UI
void cal_save();                              // defined in firmware-additions.h (needs NVS)
void cal_load();

volatile uint32_t g_5v_mV  = 0;   // 5 V / 3.3 V rails, sampled by the control task
volatile uint32_t g_3v3_mV = 0;   // so web handlers never touch the ADC

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
//   Remote debug log — fans out to USB Serial + telnet (:23) + browser WS,
//   backed by a ring buffer so a late-connecting client still sees recent
//   output. Producers (any task) only touch Serial + the ring; the telnet/WS
//   consumers are pumped from loop() to respect AsyncTCP's single-task rule.
// =============================================================================
static const int         LOGN     = 3072;
static char              g_log[LOGN];
static volatile uint32_t g_logw   = 0;            // monotonic write counter
static portMUX_TYPE      g_logmux = portMUX_INITIALIZER_UNLOCKED;

// NOTE: `if (Serial)` is NOT enough — after an esptool flash the CDC session is
// left stale-"connected" (port enumerated, nobody reading). Once the TX ring
// fills, HWCDC::write's retry counter underflows with txTimeout=0 (core 3.2.1
// HWCDC.cpp:448-479: `tries--` from 0) and blocks loop() ~forever. Writing no
// more than availableForWrite() keeps every write on the non-blocking fast path.
class DbgPrint : public Print {
 public:
  size_t write(uint8_t c) override {
    if (Serial && Serial.availableForWrite() > 0) Serial.write(c);
    portENTER_CRITICAL(&g_logmux);
    g_log[g_logw % LOGN] = (char)c; g_logw++;
    portEXIT_CRITICAL(&g_logmux);
    return 1;
  }
  size_t write(const uint8_t *b, size_t n) override {
    if (Serial) {
      size_t a = (size_t)Serial.availableForWrite();
      if (a > 0) Serial.write(b, n < a ? n : a);   // truncate to free space, never block
    }
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

// Voltage-control state — written by voltageControlTask, read by firmware-additions.h
volatile uint8_t  g_vctrl_state     = 0;    // 0=idle 1=settling 2=verifying 3=frozen 4=busy

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


// Execute one bench-console command, replying on `out`. Shared by USB + telnet.
void console_exec(const String &lineIn, Print &out) {
  String l = lineIn; l.trim();
  if (!l.length()) return;
  char c = l[0];
  if      (c == 'v') { float v = l.substring(1).toFloat();
                       apply_command("set_voltage", 1, v); setOutput(1, true);
                       out.printf("[con] set=%dmV out1=on\n", (int)(psState.voltage1 * 1000)); }
  else if (l == "o1") { apply_command("toggle", 1, 1.0f); out.println("[con] out1=on"); }
  else if (l == "o0") { apply_command("toggle", 1, 0.0f); out.println("[con] out1=off"); }
  else if (c == 'm')  { g_cal_sweep_req = true; out.println("[con] calibration sweep requested"); }
  else if (c == 'c')  { out.printf("[con] cal valid=%d\n", g_cal_valid ? 1 : 0); }
  else if (c == '?')  { out.printf("[con] set=%umV meas=%umV disp=%umV state=%u R=%dohm cal=%d\n",
                          g_setpoint_mV, g_measured_mV, g_display_mV, g_vctrl_state, (int)dpCalcR(), g_cal_valid ? 1 : 0); }
  else out.println("[con] cmds: v<volts> o1 o0 m c ?");
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

// =============================================================================
//   OTA — uses ESP32 core HTTPUpdate (HTTP or HTTPS, no extra library needed)
// =============================================================================
volatile bool     ota_busy       = false;   // one OTA operation at a time (pull or upload)
String            ota_latest     = "";      // cached /api/ota/check result
volatile uint32_t ota_checked_at = 0;       // millis of last successful check (0 = never)

// Fetch <base>version.txt (follows GitHub's 302). "" on any failure.
String ota_fetch_latest(const String &ota_url, bool verify_ssl) {
  bool   https       = ota_url.startsWith("https");
  String version_url = ota_url + "version.txt";
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  bool begun;
  WiFiClientSecure sec; WiFiClient plain;
  if (https) { if (!verify_ssl) sec.setInsecure(); begun = http.begin(sec, version_url); }
  else       { begun = http.begin(plain, version_url); }
  if (!begun) { Dbg.println("OTA: version begin failed"); return ""; }
  int code = http.GET();
  if (code != 200) { http.end(); Dbg.printf("OTA: version.txt HTTP %d\n", code); return ""; }
  String latest = http.getString(); latest.trim(); http.end();
  return latest;
}

void perform_ota(bool force, bool verify_ssl, const String &ota_url) {
  bool https = ota_url.startsWith("https");

  String latest = ota_fetch_latest(ota_url, verify_ssl);

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
//   Voltbench: NVS, gzip serving, WebSocket, captive portal, MQTT
//   Include AFTER all shared symbols are defined above.
// =============================================================================
#include "firmware-additions.h"

static bool ota_force_flag = false;
static void ota_task_fn(void *pv) {
  perform_ota(ota_force_flag, vb.ota_ssl, vb.ota_url);
  ota_force_flag = false;
  ota_busy = false;
  vTaskDelete(NULL);
}
// 16 KB stack — TLS (WiFiClientSecure) handshake/buffers are stack-hungry.
void perform_ota_tasked(bool force) {
  if (ota_busy) { Dbg.println("OTA: busy — request ignored"); return; }
  ota_busy = true;
  ota_force_flag = force;
  xTaskCreatePinnedToCore(ota_task_fn, "otaTask", 16384, NULL, 5, NULL, 0);
}

// =============================================================================
//   Remote debug pump (run from loop(): single-task, AsyncTCP-safe)
// =============================================================================
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
    // Budget the flush to the socket's free send space — a stalled peer must
    // never block loop() (same failure class as the WS queue backlog).
    int budget = telnetCli.availableForWrite();
    uint32_t w = g_logw;
    if (w - g_telnetRead > (uint32_t)LOGN) g_telnetRead = w - LOGN;
    while (g_telnetRead < w && budget-- > 0) {
      telnetCli.write((uint8_t)g_log[g_telnetRead % LOGN]); g_telnetRead++;
    }
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
    // Same rule as vb_push_status: never queue onto a backed-up client — the
    // ring cursor just lags (and skips ahead, above) until the client drains.
    if (w > g_wsLogRead && logws.availableForWriteAll()) {
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
  g_nvs_mux = xSemaphoreCreateMutex();
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);   // never block on USB-CDC writes when no host is reading
#endif
  Dbg.println();
  Dbg.println("Voltbench v" CURRENT_FIRMWARE_VERSION " booting...");

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
  Dbg.println(Wire.endTransmission() == 0 ? "MCP4017 OK @ 0x2F" : "WARNING: MCP4017 not found!");

  // Start non-blocking voltage control task on core 0 before anything else
  xTaskCreatePinnedToCore(voltageControlTask, "VoltCtrl", 4096, nullptr, 2, nullptr, 0);

  // Load NVS settings first so we can seed WiFi credentials
  vb_load();
  cal_load();   // restore the output-1 calibration map if one was saved

  // Seed credential.h entries only on first boot / after factory reset —
  // otherwise the UI's "Forget network" would revert on every reboot.
  bool have_saved = false;
  for (uint8_t i = 0; i < VBSettings::MAX_SSID; i++)
    if (!vb.ssid[i].isEmpty()) { have_saved = true; break; }
  if (!have_saved)
    for (uint8_t i = 0; i < min((int)total_ssid_count, (int)VBSettings::MAX_SSID); i++)
      vb_remember_wifi(ssids[i], passwords[i]);

  // Pull-OTA source = this repo's GitHub latest-release assets (HTTPS, insecure
  // TLS — no CA bundle embedded; pin a cert later for tamper-proofing).
  if (vb.ota_url != OTA_GITHUB_URL || vb.ota_ssl) {
    vb.ota_url = OTA_GITHUB_URL;
    vb.ota_ssl = false;
    vb_save();
  }

  // Voltbench: try WiFi, register all /api/* routes
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
  vb_loop();                      // DNS captive, WebSocket push, MQTT
  remote_debug_pump();            // telnet + browser log stream

  // Bench console over USB (same commands as telnet): v<volts> o1 o0 m c ?
  // Non-blocking accumulator (readStringUntil would stall loop() up to 1 s
  // per partially-typed line).
  static String usbin;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (usbin.length()) { console_exec(usbin, Dbg); usbin = ""; }
    } else if (usbin.length() < 64) usbin += ch;
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
