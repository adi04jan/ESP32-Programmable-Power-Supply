// =============================================================================
//   vb_net.h — everything networked: HTTP routes, both WebSockets, the WiFi
//   manager task, the captive-portal fallback, token auth, and the MQTT bridge.
//
//   The comments inside vb_net.cpp's WiFi manager encode real IDF bugs
//   (scan/connect mutual exclusion, failed-join recovery). Do not condense them.
// =============================================================================
#pragma once
#include "vb_config.h"
#include <ESPAsyncWebServer.h>

extern AsyncWebSocket vb_ws;     // "/ws"    — status push + command channel
extern AsyncWebSocket logws;     // "/logws" — browser debug-log stream (view-only)

extern bool vb_in_captive;

// Call from setup(), AFTER vb_load(): joins WiFi (or starts the captive AP)
// and registers every /api/* route.
void vb_setup();

// Call once per loop(): captive DNS, WebSocket status push, MQTT.
void vb_loop();

void   vb_start_captive_ap();
String vb_make_token();
bool   vb_authed(AsyncWebServerRequest *req);
void   vb_push_status();
