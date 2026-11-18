// =============================================================================
//   Voltbench v2.1 — 2-channel bench supply, hardware CC + CV per channel
//
//   Hardware: ESP32-C3 Super Mini · MP1584 x2 (FB network removed) ·
//             MCP4728 quad DAC @0x60 · INA219 x2 @0x40/0x41 ·
//             SSD1306 @0x3C · MCP9700 on GPIO4 · 3 buttons
//   Reference: Hardware/HARDWARE-DESIGN-V2.md
//              docs/superpowers/specs/2026-08-11-v2.1-firmware-and-software-design.md
//
//   THE ONE THING TO KNOW BEFORE CHANGING ANYTHING HERE:
//   the CV mapping is INVERTED — Vout = 20.18 V - code * 4.763 mV — so DAC code
//   0 is TWENTY VOLTS. Any path that leaves a CV DAC unwritten is a 20 V path.
//   That is why hal_gpio_init() is the first statement in setup(), why the boot
//   sequence writes 4095 before it does anything else, and why it reads every
//   DAC register back twice before an output can ever be enabled.
//
//   This file is entry points ONLY. Every module is a separate translation
//   unit, so nothing depends on being textually pasted after anything else.
//
//     vb_config.h   pins, addresses, limits   (include FIRST in every TU)
//     vb_cal.h      PURE  2-point fit, code<->mV, clamping
//     vb_protect.h  PURE  protection + CC state machine
//     vb_seq.h      PURE  sequencer
//     vb_log.h      PURE  12-byte log ring + CSV formatter
//     vb_cmd.h      PURE  VbCommand + serial line tokeniser
//     vb_ui_logic.h PURE  step/velocity/formatters
//     vb_state      seqlock snapshot, setpoint cells, command queue, dispatcher
//     vb_hal        MCP4728 / INA219 / MCP9700 / GPIO   (ctrl task only)
//     vb_control    the ctrl task — SOLE owner of I2C
//     vb_ui         SSD1306 rendering + button task
//     vb_nvs        Preferences, cal blobs, presets, deferred flush
//     vb_net        HTTP routes, WebSockets, WiFi manager, OTA
//     vb_console    con task (USB) + telnet, one shared parser
// =============================================================================
#include "vb_config.h"
#include "vb_state.h"
#include "vb_debug.h"
#include "vb_hal.h"
#include "vb_control.h"
#include "vb_console.h"
#include "vb_nvs.h"
#include "vb_net.h"
#include "vb_ui.h"

#if __has_include("credential.h")
  #include "credential.h"        // gitignored; seeds NVS on first boot only
  #define HAVE_CREDENTIALS 1
#endif

void setup() {
  // 1. EN and CAL low FIRST. Before serial, before I2C, before anything that
  //    could throw. Each pin also has a 10k hardware pull-down, so even this
  //    failing leaves the outputs dead.
  hal_gpio_init();

  vb_state_init();

  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);      // never block on USB-CDC writes with no reader
#endif
  Dbg.println();
  Dbg.println("Voltbench v" CURRENT_FIRMWARE_VERSION " (v2.1 hardware) booting...");
  Dbg.printf("Reset reason: %d\n", (int)esp_reset_reason());

  // 2. NVS before hardware, so calibration and limits are in hand when the
  //    first setpoint is computed.
  vb_load();
  vb_cal_load();
  vb_presets_load();

  // 3. Prove the DAC is present, safe, and readable. Returns false if outputs
  //    can never be enabled this boot — which is the correct outcome, not a
  //    bug to route around.
  bool hw_ok = vb_control_boot();

  ui_begin();

#ifdef HAVE_CREDENTIALS
  // Seed only on first boot / after a factory reset, or the UI's "Forget
  // network" would revert on every reboot.
  bool have_saved = false;
  for (uint8_t i = 0; i < VBSettings::MAX_SSID; i++)
    if (!vb.ssid[i].isEmpty()) { have_saved = true; break; }
  if (!have_saved) {
    const uint8_t n = sizeof(ssids) / sizeof(ssids[0]);
    for (uint8_t i = 0; i < n && i < VBSettings::MAX_SSID; i++)
      vb_remember_wifi(ssids[i], passwords[i]);
  }
#endif

  // 4. Power-on policy. Staggered by 100 ms so two channels cannot present a
  //    double inrush to the same 24 V brick.
  if (hw_ok) {
    for (int k = 0; k < VB_CHANNELS; k++) {
      if (vb.po_mode[k] == PO_ON_LAST) {
        g_set_mV[k] = vb.last_v[k]; g_set_mA[k] = vb.last_i[k];
        g_en_cmd[k] = vb.last_en[k];
      } else if (vb.po_mode[k] == PO_ON_FIXED) {
        g_set_mV[k] = vb.po_v[k];   g_set_mA[k] = vb.po_i[k];
        g_en_cmd[k] = 1;
      }
    }
  }

  // 5. Tasks. ESP32-C3 is single-core, so priority ordering IS the concurrency
  //    design — ctrl above AsyncTCP so a trip never queues behind a WS burst.
  xTaskCreate(vb_control_task, "ctrl", STACK_CTRL, nullptr, PRIO_CTRL, nullptr);
  xTaskCreate(ui_button_task,  "btn",  STACK_BTN,  nullptr, PRIO_BTN,  nullptr);
  xTaskCreate(vb_console_task, "con",  STACK_CON,  nullptr, PRIO_CON,  nullptr);

  vb_setup();                    // WiFi manager + routes
  server.begin();
  vb_console_begin();            // telnet :23

  Dbg.printf("Ready. hw=%s  http://%s.local/\n", hw_ok ? "ok" : "FAULT", vb.mdns.c_str());
}

// Every 10 s: the number that decides whether the log ring can grow to 4096
// records, and the first thing to check when something starts misbehaving
// after hours of uptime.
static void heap_report() {
  static uint32_t t = 0;
  if (millis() - t < 10000) return;
  t = millis();
  Dbg.printf("[heap] free=%u largest=%u minfree=%u | stack loop=%u ctrl=%u con=%u\n",
             (unsigned)ESP.getFreeHeap(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)ESP.getMinFreeHeap(),
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("ctrl")),
             (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("con")));
}

void loop() {
  vb_loop();             // captive DNS, WebSocket push
  vb_console_pump();     // telnet + /logws drain
  vb_nvs_flush_tick();   // deferred NVS writes happen HERE and nowhere else
  heap_report();
  delay(2);
}
