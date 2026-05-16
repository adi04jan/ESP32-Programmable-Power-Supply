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
#include "driver/rtc_io.h"
#include <ESPmDNS.h>

#include "web_page.h"       // index_html_gz, index_html_gz_len
#include "credential.h"     // ssids[], passwords[], base_url (initial seed only)

#define CURRENT_FIRMWARE_VERSION "0.1.0"

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

// =============================================================================
//   voltageControlTask — PID state machine
//   States: 0=idle  1=settling  2=pid-running  3=converged  4=auto-tuning
//   Runs on core 0, priority 2. Web handlers return immediately.
// =============================================================================
// 4 samples × 5 ms = 20 ms total — fast enough for <1s convergence
static uint32_t vctrl_sample() {
  uint32_t sum = 0;
  for (int s = 0; s < 4; s++) {
    sum += analogReadMilliVolts(VOLTAGE_READ_PIN_VV);
    if (s < 3) vTaskDelay(pdMS_TO_TICKS(5));
  }
  return (sum / 4) * 48;
}

// pid_autotune() is defined after #include "firmware-additions.h"
bool pid_autotune(uint32_t target);

void voltageControlTask(void* pvParameters) {
  static const float DT = 0.07f;  // PID dt: 20ms ADC + 50ms delay

  float    integral    = 0.0f;
  float    prev_err    = 0.0f;
  uint32_t lastTarget  = g_target_mV;
  uint32_t settle_t    = 0;
  int      conv_count  = 0;
  uint8_t  state       = 0;
  bool     pre_discharge = false;  // true while waiting for Vout to drop before applying target R
  uint32_t pending_r     = 0;      // R to apply once pre-discharge completes

  // Calibration cache: maps target_mV → converged R value (Quick + Timed modes)
  struct CalibEntry { uint32_t target_mV; uint32_t r_val; };
  static CalibEntry calib_cache[8] = {};
  static uint8_t   calib_next     = 0;

  // Timed-mode helpers
  uint32_t timed_start  = 0;
  uint32_t best_r       = 0;
  uint32_t best_err_abs = 0xFFFFFFFFu;

  // Minimum R allowed for a given target — enforces hard Vout ceiling of target+300mV
  auto r_overvolt_floor = [](uint32_t tgt_mV) -> float {
    uint32_t ceiling_mV = tgt_mV + 300u;
    if (ceiling_mV <= (uint32_t)DC_V_REF) return (float)DC_R2_REF;
    float r = (float)DC_R2_REF * (float)DC_V_REF / (float)(ceiling_mV - DC_V_REF) - (float)MCPWIPEROHMS;
    return (r > 0.0f) ? r : 0.0f;
  };

  for (;;) {
    if (g_autotune_req) {
      g_autotune_req = false;
      g_vctrl_state  = 4;
      pid_autotune(g_target_mV);
      integral = 0; prev_err = 0; conv_count = 0;
      best_r = 0; best_err_abs = 0xFFFFFFFFu;
      pre_discharge = false; pending_r = 0;
      lastTarget = 0;
      state = 0; g_vctrl_state = 0;
      continue;
    }

    uint8_t  mode   = g_ctrl_mode;
    uint32_t target = g_target_mV;

    // Sample ADC first so we have a fresh reading for pre-discharge decisions
    g_measured_mV = vctrl_sample();

    if (target != lastTarget) {
      lastTarget   = target;
      integral     = 0; prev_err = 0; conv_count = 0;
      best_r       = 0; best_err_abs = 0xFFFFFFFFu;
      pre_discharge = false; pending_r = 0;

      if (target > (uint32_t)DC_V_REF) {
        // Conservative starting R: use formula for (target-300mV) to guarantee undershoot on entry
        uint32_t safe_tgt = (target > (uint32_t)(DC_V_REF + 300)) ? (target - 300u) : target;
        long r_calc = (long)((DC_R2_REF * DC_V_REF) / (safe_tgt - DC_V_REF)) - MCPWIPEROHMS;
        uint32_t formula_r = (r_calc > 0) ? (uint32_t)r_calc : 0;

        // Check calibration cache — only use if it won't overshoot
        uint32_t r_val     = formula_r;
        bool     cache_hit = false;
        float    r_floor   = r_overvolt_floor(target);
        for (int i = 0; i < 8; i++) {
          if (calib_cache[i].target_mV == target && calib_cache[i].r_val > 0) {
            if ((float)calib_cache[i].r_val >= r_floor) {
              r_val     = calib_cache[i].r_val;
              cache_hit = true;
            }
            break;
          }
        }

        // If current Vout is already above target+300mV, pre-discharge to max R first
        if (g_measured_mV > target + 300u) {
          i2cDP.setResistance(DC_R2_REF);  // max R → Vout ~2.47V minimum
          pre_discharge = true;
          pending_r     = r_val;
          Serial.printf("[VCtrl] pre-discharge: meas=%umV tgt=%umV\n", g_measured_mV, target);
        } else {
          i2cDP.setResistance(r_val);
          Serial.printf("[VCtrl] target=%umV R=%u%s\n", target, r_val, cache_hit ? " (cached)" : "");
        }
      }
      settle_t = millis();
      state = 1; g_vctrl_state = 1;
    }

    // State 1: settle (or wait for pre-discharge to complete)
    if (state == 1) {
      if (pre_discharge) {
        if (g_measured_mV <= target + 300u) {
          i2cDP.setResistance(pending_r);
          pre_discharge = false;
          settle_t = millis();  // restart settle timer from here
          Serial.printf("[VCtrl] pre-discharge done: meas=%umV R=%u\n", g_measured_mV, pending_r);
        }
        // else keep sampling until voltage drops — don't advance state
      } else if (millis() - settle_t >= 300) {
        timed_start = millis();
        state = 2; g_vctrl_state = 2;
      }
    }

    if (state == 2) {
      if (target > (uint32_t)DC_V_REF) {
        float    error    = (float)target - (float)g_measured_mV;
        float    err_abs  = fabsf(error);
        uint32_t cur_r    = i2cDP.calcResistance();

        // Track best result — only when NOT in overvoltage (error >= -300 means Vout <= target+300)
        if (error >= -300.0f && (uint32_t)err_abs < best_err_abs) {
          best_err_abs = (uint32_t)err_abs;
          best_r       = cur_r;
        }

        auto pid_step = [&]() {
          float r2  = (float)cur_r;
          integral += error * DT;
          // Prevent upward overshoot: zero positive integral the moment Vout exceeds target.
          // Without this, the integral built while stuck at a sub-step value fires all at once
          // and overshoots by 1-2 MCP4017 steps when the wiper finally advances.
          if (error < 0.0f && integral > 0.0f) integral = 0.0f;
          // Tight anti-windup: ±300 mV × cycles budget (prevents slow build-up accumulation)
          integral = constrain(integral, -300.0f, 300.0f);
          float deriv  = (error - prev_err) / DT;
          prev_err     = error;
          float v_corr = g_pid_kp * error + g_pid_ki * integral + g_pid_kd * deriv;
          // Asymmetric clamp: fast downward (Vout too high), slow upward (≤100 mV/step)
          v_corr = constrain(v_corr, -2000.0f, 100.0f);
          float new_r  = constrain(r2 - v_corr * r2 * r2 / ((float)DC_V_REF * (float)DC_R2_REF),
                                   0.0f, (float)DC_R2_REF);
          // Hard floor: never allow R that would produce Vout > target+300mV
          // Add one MCP4017 step (79Ω) to absorb quantization rounding toward lower resistance
          float r_floor = r_overvolt_floor(target) + 79.0f;
          if (r_floor > 0.0f && new_r < r_floor) new_r = r_floor;
          i2cDP.setResistance((uint32_t)new_r);
        };

        auto cache_store = [&](uint32_t r) {
          // Only cache if within 500mV above and 300mV below target (no overvoltage allowed)
          if (error < -300.0f || error > 500.0f) return;
          for (int i = 0; i < 8; i++) {
            if (calib_cache[i].target_mV == target) { calib_cache[i].r_val = r; return; }
          }
          calib_cache[calib_next] = { target, r };
          calib_next = (calib_next + 1) & 7;
        };

        if (mode == 0) {
          // --- Quick: PID until <150 mV for 2 consecutive reads, then freeze ---
          if (err_abs < 150.0f) {
            if (++conv_count >= 2) { cache_store(cur_r); state = 3; g_vctrl_state = 3; }
          } else {
            conv_count = 0;
            pid_step();
          }
        } else if (mode == 1) {
          // --- Continuous: PID until <100 mV for 2 consecutive reads ---
          if (err_abs < 100.0f) {
            if (++conv_count >= 2) { state = 3; g_vctrl_state = 3; }
          } else {
            conv_count = 0;
            pid_step();
          }
        } else {
          // --- Timed: PID for ctrl_timeout_ms, then freeze at best R ---
          uint32_t elapsed = millis() - timed_start;
          bool     timeout = elapsed >= g_ctrl_timeout_ms;
          if (timeout || err_abs < 100.0f) {
            uint32_t freeze_r = (best_r > 0) ? best_r : cur_r;
            if (timeout && best_r > 0) i2cDP.setResistance(freeze_r);
            cache_store(freeze_r);
            state = 3; g_vctrl_state = 3;
          } else {
            pid_step();
          }
        }
      } else {
        state = 3; g_vctrl_state = 3;
      }
    }

    // Fast loop while PID active, slow while idle/converged/pre-discharge
    vTaskDelay(pdMS_TO_TICKS(state == 2 ? 50 : 200));
  }
}

// =============================================================================
//   OTA — uses ESP32 core HTTPUpdate (HTTP or HTTPS, no extra library needed)
// =============================================================================
void perform_ota(bool force, bool verify_ssl, const String &ota_url) {
  HTTPClient http;
  String version_url = ota_url + "version.txt";
  http.begin(version_url);
  int code = http.GET();
  if (code != 200) { http.end(); Serial.printf("OTA: version.txt HTTP %d\n", code); return; }
  String latest = http.getString(); latest.trim(); http.end();

  Serial.printf("OTA: current=%s latest=%s\n", CURRENT_FIRMWARE_VERSION, latest.c_str());
  if (!force && latest == CURRENT_FIRMWARE_VERSION) { Serial.println("OTA: already up-to-date"); return; }

  String firmware_url = ota_url + "firmware_" + latest + ".bin";
  WiFiClient c;
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return ret = httpUpdate.update(c, firmware_url);
  if (ret == HTTP_UPDATE_FAILED)
    Serial.printf("OTA: failed (%d) %s\n",
      httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
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
  i2cDP.setResistance((uint32_t)rc);
  vTaskDelay(pdMS_TO_TICKS(800));

  const int MAX_CROSS = 8;
  uint32_t  cross_t[MAX_CROSS];
  int       n_cross = 0;
  bool      relay_up = true;
  uint32_t  meas_max = 0, meas_min = 0xFFFFFFFFu;
  bool      tracking = false;
  uint32_t  t_start  = millis();

  i2cDP.setResistance((uint32_t)constrain(rc - relay_r, 0.0f, (float)DC_R2_REF));

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
      i2cDP.setResistance((uint32_t)constrain(rc + relay_r, 0.0f, (float)DC_R2_REF));
      cross_t[n_cross++] = millis(); tracking = true;
    } else if (!relay_up && meas < target) {
      relay_up = true;
      i2cDP.setResistance((uint32_t)constrain(rc - relay_r, 0.0f, (float)DC_R2_REF));
      cross_t[n_cross++] = millis();
    }
  }

  i2cDP.setResistance((uint32_t)rc);
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
void perform_ota_tasked() {
  xTaskCreatePinnedToCore(ota_task_fn, "otaTask", 8192, NULL, 5, NULL, 0);
}

// =============================================================================
//   Arduino entry points
// =============================================================================
static const uint8_t total_ssid_count = sizeof(ssids) / sizeof(ssids[0]);

void setup() {
  Serial.begin(115200);
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

  // One-time migration: seed NVS from credential.h if no SSIDs saved yet
  bool any_saved = false;
  for (uint8_t i = 0; i < VBSettings::MAX_SSID; i++) {
    if (!vb.ssid[i].isEmpty()) { any_saved = true; break; }
  }
  if (!any_saved) {
    Serial.println("First boot — seeding WiFi credentials from credential.h");
    for (uint8_t i = 0; i < min((int)total_ssid_count, (int)VBSettings::MAX_SSID); i++)
      vb_remember_wifi(ssids[i], passwords[i]);
  }

  // Set OTA URL from credential.h if NVS still has the placeholder
  if (vb.ota_url.startsWith("https://updates.example.com")) {
    vb.ota_url = String(base_url);
    vb_save();
  }

  // Voltbench: load NVS again (now has migrated creds), try WiFi, register all /api/* routes
  vb_setup();

  if (!vb_in_captive && vb.ota_auto)
    perform_ota(false, vb.ota_ssl, vb.ota_url);

  server.begin();
  Serial.println("HTTP server started");
  if (!vb_in_captive)
    Serial.printf("URL: http://%s.local/\n", vb.mdns.c_str());
}

void loop() {
  vb_loop();   // DNS captive, WebSocket push, MQTT, OCP/OTP guards
  delay(2);
}
