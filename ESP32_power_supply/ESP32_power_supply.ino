// =============================================================================
//   Voltbench — ESP32-C3 Programmable Power Supply
//   Hardware: LM2596-ADJ (variable), Mini360 5V, LM1117 3.3V
//             MCP4017T-103E/LT (10K, 128-step) I2C digital pot @ 0x2F
//             Voltage divider: VOUT — 10K — FB — MCP4017 — GND
//             ADC divider:     VOUT — 47K:1K → VOLTAGE_READ_PIN_VV (×48 scale)
//
//   This file is entry points ONLY. Everything else lives in vb_*.{h,cpp},
//   each a separate translation unit — so nothing depends on being textually
//   pasted after anything else, which is what the old mid-file
//   `#include "firmware-additions.h"` at line 457 required.
//
//   Module map:
//     vb_config.h   constants, pins, version   (include FIRST in every TU)
//     vb_state      globals shared across modules
//     vb_debug      DbgPrint ring -> Serial + telnet + /logws
//     vb_control    digipot, ADC, deadbeat controller   (sole I2C+ADC owner)
//     vb_console    bench console (USB + telnet), log-ring drain
//     vb_nvs        Preferences / settings / calibration persistence
//     vb_ota        pull-OTA from GitHub release assets
//     vb_net        HTTP routes, WebSockets, WiFi manager, captive AP, MQTT
// =============================================================================
#include "vb_config.h"
#include "vb_state.h"
#include "vb_debug.h"
#include "vb_control.h"
#include "vb_console.h"
#include "vb_nvs.h"
#include "vb_ota.h"
#include "vb_net.h"
#include "credential.h"     // ssids[], passwords[] (initial NVS seed only)

#include <Wire.h>

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
  vb_console_begin();          // remote debug console on :23
  Dbg.println("HTTP server started");
  if (!vb_in_captive)
    Dbg.printf("URL: http://%s.local/  (telnet %s.local:23)\n", vb.mdns.c_str(), vb.mdns.c_str());

  // Auto-OTA check runs AFTER the server is up, in its own task, so a slow
  // HTTPS round-trip to GitHub never delays boot / the web UI.
  if (!vb_in_captive && vb.ota_auto)
    perform_ota_tasked(false);
}

// Phase 0 instrumentation: free heap and the largest allocatable block set the
// budget for v2.1's static log ring (~24 KB) and the OLED framebuffer. The
// worst moment is mid pull-OTA, when mbedTLS is holding ~50 KB. Watch this
// under load before sizing anything.
static void heap_report() {
  static uint32_t t_heap = 0;
  if (millis() - t_heap < 10000) return;
  t_heap = millis();
  Dbg.printf("[heap] free=%u largest=%u minfree=%u | stack loop=%u ctrl=%u\n",
             (unsigned)ESP.getFreeHeap(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)ESP.getMinFreeHeap(),
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("VoltCtrl")));
}

void loop() {
  vb_loop();            // DNS captive, WebSocket push, MQTT
  vb_console_pump();    // USB + telnet console, log stream, periodic telemetry
  heap_report();        // Phase 0: baseline for the v2.1 memory budget
  delay(2);
}
