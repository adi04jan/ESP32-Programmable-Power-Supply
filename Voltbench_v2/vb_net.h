// =============================================================================
//   vb_net.h — HTTP routes, both WebSockets, the WiFi manager, captive portal,
//   token auth, OTA upload.
//
//   The comments inside the WiFi manager encode real IDF bugs (scan/connect
//   mutual exclusion) and a real field failure (arduino-esp32 permanently
//   giving up on some disconnect reasons, which stranded a v1 unit until it was
//   power-cycled). Do not condense them.
//
//   NOTHING HERE MAY TOUCH I2C OR PREFERENCES. Async handlers read the seqlock
//   snapshot and write setpoint cells or the command queue; that is all.
// =============================================================================
#pragma once
#include "vb_config.h"
#include <ESPAsyncWebServer.h>

extern AsyncWebServer server;
extern AsyncWebSocket vb_ws;      // "/ws"    — status push + command channel
extern AsyncWebSocket logws;      // "/logws" — debug-log stream (view-only)
extern bool vb_in_captive;

void   vb_setup();
void   vb_loop();
void   vb_start_captive_ap();
String vb_make_token();
bool   vb_authed(AsyncWebServerRequest *req);
void   vb_push_status();

// WiFi drop post-mortem, served from RAM (never an NVS read on the async task).
extern volatile uint8_t  g_wifi_last_reason;
extern volatile uint32_t g_wifi_drops;
extern volatile uint32_t g_wifi_last_drop_s;
extern volatile uint8_t  g_wifi_boot_reason;
