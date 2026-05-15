// =============================================================================
//   Voltbench — ESP32-C3 Programmable Power Supply
//   Hardware: LM2596-ADJ (variable), Mini360 5V, LM1117 3.3V
//             MCP4017T-103E/LT (10K, 128-step) I2C digital pot @ 0x2F
//             Voltage divider: VOUT — 10K — FB — MCP4017 — GND
//             ADC divider:     VOUT — 10K:1K → VOLTAGE_READ_PIN_VV (×11 scale)
// =============================================================================
#include <Wire.h>
#include <SW_MCP4017.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include "HttpsOTAUpdate.h"
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
  return read_any_volt(VOLTAGE_READ_PIN_VV, s, &init, sc) * 11;
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
//   voltageControlTask — owns all I2C pot writes and ADC reads
//   Runs on core 0, priority 2. Web handlers return immediately.
// =============================================================================
void voltageControlTask(void* pvParameters) {
  const int      FINE_TUNE_R2_MIN  = 3000;  // below this ohms each step > 100 mV tolerance
  const int      FINE_TUNE_MAX     = 3;
  const uint32_t VOLTAGE_TOL_MV    = 100;
  const uint32_t MONITOR_MS        = 200;

  uint32_t lastTarget = g_target_mV;

  for (;;) {
    uint32_t target = g_target_mV;

    if (target != lastTarget) {
      lastTarget = target;

      // Formula-based initial resistance set
      if (target > (uint32_t)DC_V_REF) {
        long r_calc = (long)((DC_R2_REF * DC_V_REF) / (target - DC_V_REF)) - MCPWIPEROHMS;
        uint32_t r_val = (r_calc > 0) ? (uint32_t)r_calc : 0;
        i2cDP.setResistance(r_val);
        Serial.printf("[VCtrl] target=%umV R=%u\n", target, r_val);
      }
      vTaskDelay(pdMS_TO_TICKS(150));

      // Fresh 3-sample read
      uint32_t sum = 0;
      for (int s = 0; s < 3; s++) {
        sum += analogReadMilliVolts(VOLTAGE_READ_PIN_VV);
        if (s < 2) vTaskDelay(pdMS_TO_TICKS(10));
      }
      g_measured_mV = (sum / 3) * 11;

      // Fine-tune only in low-voltage range (R2 large enough that each step ≤ tolerance)
      if (g_ctrl_output1) {
        int r_now = (int)i2cDP.calcResistance();
        for (int i = 0; i < FINE_TUNE_MAX; i++) {
          if (g_target_mV != lastTarget) break;   // new target arrived — abort
          if (r_now < FINE_TUNE_R2_MIN)  break;   // step too coarse at high V

          sum = 0;
          for (int s = 0; s < 3; s++) {
            sum += analogReadMilliVolts(VOLTAGE_READ_PIN_VV);
            if (s < 2) vTaskDelay(pdMS_TO_TICKS(10));
          }
          uint32_t measured = (sum / 3) * 11;
          g_measured_mV = measured;

          long diff = (long)target - (long)measured;
          if (abs(diff) <= (long)VOLTAGE_TOL_MV) break;

          if (diff > 0) r_now = max(r_now - 79, 0);
          else          r_now = min(r_now + 79, DC_R2_REF);
          i2cDP.setResistance((uint32_t)r_now);
          Serial.printf("[VCtrl] fine-tune #%d R=%d meas=%umV\n", i+1, r_now, measured);
          vTaskDelay(pdMS_TO_TICKS(80));
        }
      }
    } else {
      // Idle — keep measured voltage fresh for WebSocket push
      vTaskDelay(pdMS_TO_TICKS(MONITOR_MS));
      uint32_t sum = 0;
      for (int s = 0; s < 3; s++) {
        sum += analogReadMilliVolts(VOLTAGE_READ_PIN_VV);
        if (s < 2) vTaskDelay(pdMS_TO_TICKS(10));
      }
      g_measured_mV = (sum / 3) * 11;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// =============================================================================
//   OTA
// =============================================================================
static const char *server_certificate =
  "-----BEGIN CERTIFICATE-----\n"
  "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\n"
  "-----END CERTIFICATE-----";

static void HttpEvent(HttpEvent_t *e) { (void)e; }

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
  HttpsOTA.onHttpEvent(HttpEvent);
  HttpsOTA.begin(firmware_url.c_str(), verify_ssl ? server_certificate : nullptr, false);

  uint32_t spent = 0;
  while (true) {
    HttpsOTAStatus_t st = HttpsOTA.status();
    if (st == HTTPS_OTA_SUCCESS) { Serial.println("OTA: success, rebooting"); delay(500); ESP.restart(); }
    if (st == HTTPS_OTA_FAIL)    { Serial.println("OTA: failed"); return; }
    if (++spent > 120)           { Serial.println("OTA: timeout"); return; }
    delay(1000);
  }
}

// =============================================================================
//   Voltbench: NVS, gzip serving, WebSocket, captive portal, guards, MQTT
//   Include AFTER all shared symbols are defined above.
// =============================================================================
#include "firmware-additions.h"

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
