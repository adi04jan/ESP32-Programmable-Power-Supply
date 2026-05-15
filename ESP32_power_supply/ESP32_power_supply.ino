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
volatile float   g_pid_kp       = 0.50f;
volatile float   g_pid_ki       = 0.10f;
volatile float   g_pid_kd       = 0.02f;
volatile uint8_t g_vctrl_state  = 0;    // 0=idle 1=settling 2=pid 3=converged 4=tuning
volatile bool    g_autotune_req = false;

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
static uint32_t vctrl_sample8() {
  uint32_t sum = 0;
  for (int s = 0; s < 8; s++) {
    sum += analogReadMilliVolts(VOLTAGE_READ_PIN_VV);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return (sum / 8) * 48;
}

// pid_autotune() is defined after #include "firmware-additions.h"
bool pid_autotune(uint32_t target);

void voltageControlTask(void* pvParameters) {
  float    integral   = 0.0f;
  float    prev_err   = 0.0f;
  uint32_t lastTarget = g_target_mV;
  uint32_t settle_t   = 0;
  int      conv_count = 0;
  uint8_t  state      = 0;

  for (;;) {
    if (g_autotune_req) {
      g_autotune_req = false;
      g_vctrl_state  = 4;
      pid_autotune(g_target_mV);
      integral = 0; prev_err = 0; conv_count = 0;
      lastTarget = 0;   // force formula re-set next iteration
      state = 0; g_vctrl_state = 0;
      continue;
    }

    uint32_t target = g_target_mV;
    if (target != lastTarget) {
      lastTarget = target;
      integral = 0; prev_err = 0; conv_count = 0;
      if (target > (uint32_t)DC_V_REF) {
        long r_calc = (long)((DC_R2_REF * DC_V_REF) / (target - DC_V_REF)) - MCPWIPEROHMS;
        uint32_t r_val = (r_calc > 0) ? (uint32_t)r_calc : 0;
        i2cDP.setResistance(r_val);
        Serial.printf("[VCtrl] target=%umV R=%u\n", target, r_val);
      }
      settle_t = millis();
      state = 1; g_vctrl_state = 1;
    }

    // ~80 ms ADC sample — runs every loop regardless of state
    g_measured_mV = vctrl_sample8();

    if (state == 1 && millis() - settle_t >= 300) {
      state = 2; g_vctrl_state = 2;
    }

    if (state == 2) {
      if (target > (uint32_t)DC_V_REF) {
        float error = (float)target - (float)g_measured_mV;
        float r2    = (float)i2cDP.calcResistance();

        // Integral with anti-windup (clamp to ±3V equivalent contribution)
        integral += error * 0.29f;
        if (g_pid_ki > 0.0f) {
          float lim = 3000.0f / g_pid_ki;
          integral  = constrain(integral, -lim, lim);
        }

        float deriv  = (error - prev_err) / 0.29f;
        prev_err = error;

        // PID output in mV correction → linearise to ΔR
        // dVOUT/dR = −Vref×Rtop/R² → dR = −dV × R²/(Vref×Rtop)
        float v_corr = g_pid_kp * error + g_pid_ki * integral + g_pid_kd * deriv;
        float new_r  = constrain(r2 - v_corr * r2 * r2 / ((float)DC_V_REF * (float)DC_R2_REF),
                                 0.0f, (float)DC_R2_REF);
        i2cDP.setResistance((uint32_t)new_r);

        if (fabsf(error) < 100.0f) {
          if (++conv_count >= 3) { state = 3; g_vctrl_state = 3; }
        } else { conv_count = 0; }
      } else {
        state = 3; g_vctrl_state = 3;  // below Vref — formula can't drive here
      }
    }

    vTaskDelay(pdMS_TO_TICKS(200));
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
