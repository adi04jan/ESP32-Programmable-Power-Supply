#include "vb_net.h"
#include "vb_state.h"
#include "vb_nvs.h"
#include "vb_debug.h"
#include "vb_control.h"
#include "vb_console.h"
#include "vb_protect.h"
#include "vb_seq.h"
#include "vb_log.h"

#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <ArduinoJson.h>

AsyncWebServer server(80);
AsyncWebSocket vb_ws("/ws");
AsyncWebSocket logws("/logws");
static DNSServer vb_dns;
bool vb_in_captive = false;

static volatile bool     g_scan_busy      = false;
static volatile int16_t  g_scan_result    = -2;
static volatile bool     g_wifi_conn_busy = false;
static SemaphoreHandle_t g_wifi_ready     = nullptr;

// ---- WiFi drop post-mortem --------------------------------------------------
// Set on the WiFi event task, persisted by wifiMgrTask (a Preferences write is
// a 10-40 ms flash erase and must not run on the event task), served to
// /api/info straight from RAM so the AsyncTCP task never blocks on g_nvs_mux.
volatile uint8_t  g_wifi_last_reason = 0;
volatile uint32_t g_wifi_drops       = 0;
volatile uint32_t g_wifi_last_drop_s = 0;
volatile uint8_t  g_wifi_boot_reason = 0;

// =============================================================================
//   Scan / connect helpers
// =============================================================================
static void _wfScanTask(void *) {
  WiFi.scanDelete();
  int16_t n = WiFi.scanNetworks(false, true);
  g_scan_result = (n < 0) ? -2 : n;
  g_scan_busy   = false;
  vTaskDelete(NULL);
}

// CRITICAL: esp_wifi_connect() during an active scan silently kills the scan
// and WIFI_EVENT_SCAN_DONE never fires — waitStatusBits then blocks for 60 s.
static void vb_trigger_scan() {
  if (g_scan_busy || g_wifi_conn_busy) return;
  g_scan_busy   = true;
  g_scan_result = -1;
  xTaskCreate(_wfScanTask, "wfscan", 4096, nullptr, 1, nullptr);
}

static bool _trySSID(const char *ssid, const char *pass, uint32_t timeoutMs) {
  WiFi.begin(ssid, pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs)
    vTaskDelay(pdMS_TO_TICKS(100));
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.disconnect(false, false);
  vTaskDelay(pdMS_TO_TICKS(200));
  return false;
}

static bool _tryAllSSIDs(uint32_t perSsidMs) {
  g_wifi_conn_busy = true;
  for (uint8_t i = 0; i < VBSettings::MAX_SSID; i++) {
    if (vb.ssid[i].isEmpty()) continue;
    if (_trySSID(vb.ssid[i].c_str(), vb.pass[i].c_str(), perSsidMs)) {
      g_wifi_conn_busy = false;
      return true;
    }
  }
  g_wifi_conn_busy = false;
  return false;
}

void vb_start_captive_ap() {
  WiFi.mode(WIFI_AP_STA);
  String ap = "Voltbench-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  WiFi.softAP(ap.c_str());
  vb_dns.start(53, "*", WiFi.softAPIP());
  vb_in_captive = true;
  Dbg.printf("Captive AP: %s @ %s\n", ap.c_str(), WiFi.softAPIP().toString().c_str());
}

// =============================================================================
//   Auth
// =============================================================================
String vb_make_token() {
  String t;
  for (int i = 0; i < 4; i++) t += String(esp_random(), HEX);
  return t;
}

bool vb_authed(AsyncWebServerRequest *req) {
  if (!vb.require_login) return true;
  if (req->hasHeader("X-Auth")) return req->getHeader("X-Auth")->value() == vb.auth_token;
  return false;
}
#define REQUIRE_AUTH(req) do { if (!vb_authed(req)) { \
  req->send(401, "application/json", "{\"err\":\"auth\"}"); return; } } while (0)

// =============================================================================
//   Status frame (spec §2.4) — filled from ONE seqlock read, so it is
//   internally consistent by construction.
// =============================================================================
static void fill_status(JsonDocument &d) {
  VbSnapshot s;
  vb_snapshot(&s);

  d["ts"]  = s.ts_ms;
  d["up"]  = s.uptime_s;
  d["api"] = VB_API_VERSION;

  JsonArray arr = d["ch"].to<JsonArray>();
  for (int k = 0; k < VB_CHANNELS; k++) {
    const VbChanSnap *c = &s.ch[k];
    JsonObject o = arr.add<JsonObject>();
    o["en"]      = (bool)c->en;
    o["v"]       = c->v_mV / 1000.0f;
    o["i"]       = c->i_mA / 1000.0f;
    o["w"]       = c->w_mW / 1000.0f;
    o["vset"]    = c->vset_mV / 1000.0f;
    o["iset"]    = c->iset_mA / 1000.0f;
    o["mode"]    = c->state == PS_CC ? "cc" : "cv";
    o["prot"]    = c->state == PS_TRIPPED ? "trip" : "ok";
    o["reason"]  = prot_reason_str((ProtReason)c->reason);
    o["cc_mode"] = c->cc_mode == CCM_TRIP ? "trip" : "limit";
    o["ovp"]     = c->ovp_mV / 1000.0f;
    o["ocp"]     = c->ocp_mA / 1000.0f;
    o["sense"]   = (bool)c->sense_ok;
  }

  d["temp"]     = s.temp_c10 / 10.0f;
  d["temp_src"] = "mcp9700";

  JsonObject sq = d["seq"].to<JsonObject>();
  sq["state"] = seq_state_str((SeqState)s.seq_state);
  sq["step"]  = s.seq_pc;
  sq["n"]     = s.seq_n;
  sq["el"]    = s.seq_el_ms;

  JsonObject lg = d["log"].to<JsonObject>();
  lg["on"]  = (bool)s.log_on;
  lg["n"]   = s.log_n;
  lg["cap"] = s.log_cap;

  JsonArray cal = d["cal"].to<JsonArray>();
  for (int k = 0; k < VB_CHANNELS; k++) cal.add((bool)s.ch[k].cal_valid);

  d["rev_set"] = s.rev_set;
  d["hw"]      = (bool)s.hw_fault;
}

uint32_t vb_last_push = 0;
void vb_push_status() {
  if (vb_ws.count() == 0) return;
  // Edge pushes bypass the rate limit once — but STILL check
  // availableForWriteAll() and drop rather than queue. A client that stops
  // ACKing must never back up the send queues.
  bool edge = g_push_now;
  if (!edge && millis() - vb_last_push < 250) return;
  if (!vb_ws.availableForWriteAll()) return;
  g_push_now    = false;
  vb_last_push  = millis();

  JsonDocument d;
  fill_status(d);
  String out;
  serializeJson(d, out);
  vb_ws.textAll(out);
}

// =============================================================================
//   WebSocket command channel
// =============================================================================
static void on_ws_event(AsyncWebSocket *, AsyncWebSocketClient *c,
                        AwsEventType t, void *arg, uint8_t *data, size_t len) {
  if (t != WS_EVT_DATA) return;
  AwsFrameInfo *fi = (AwsFrameInfo *)arg;
  if (!fi->final || fi->index != 0 || fi->len != len || fi->opcode != WS_TEXT) return;

  JsonDocument doc;
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) return;

  if (vb.require_login) {
    const char *tok = doc["token"];
    if (!tok || String(tok) != vb.auth_token) { c->text("{\"err\":\"auth\"}"); return; }
  }

  const char *action = doc["action"];
  if (!action) return;
  // "output" is v1's name for the channel; accept both for one release.
  uint8_t ch = doc["ch"].is<int>()     ? (uint8_t)doc["ch"].as<int>()
             : doc["output"].is<int>() ? (uint8_t)doc["output"].as<int>() : 0;
  float   v  = doc["value"] | 0.0f;
  if (doc["value"].is<bool>()) v = doc["value"].as<bool>() ? 1.0f : 0.0f;

  VbErr e = apply_command_str(String(action), ch, v, SRC_WS);

  JsonDocument resp;
  fill_status(resp);
  resp["err"] = vb_err_str(e);
  String out;
  serializeJson(resp, out);
  c->text(out);
}

// =============================================================================
//   Routes
// =============================================================================
static void register_routes() {
  server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest *req) {
    String pw = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : "";
    if (vb.require_login && pw != vb.password) {
      req->send(401, "application/json", "{\"err\":\"auth\"}");
      return;
    }
    vb.auth_token = vb_make_token();          // rotates: log in once per run
    vb_save_token();
    req->send(200, "application/json", "{\"ok\":true,\"token\":\"" + vb.auth_token + "\"}");
  });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req);
    JsonDocument d;
    fill_status(d);
    String out;
    serializeJson(d, out);
    AsyncWebServerResponse *r = req->beginResponse(200, "application/json", out);
    // Without this browsers heuristically cache it and the displayed voltage
    // appears frozen.
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
  });

  server.on("/api/info", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument d;
    d["fw"]            = CURRENT_FIRMWARE_VERSION;
    d["api"]           = VB_API_VERSION;
    d["channels"]      = VB_CHANNELS;
    d["mdns"]          = vb.mdns;
    d["require_login"] = vb.require_login;
    d["captive"]       = vb_in_captive;
    if (vb_authed(req)) {
      d["ip"]       = WiFi.localIP().toString();
      d["mac"]      = WiFi.macAddress();
      d["ssid"]     = WiFi.SSID();
      d["rssi"]     = WiFi.RSSI();
      d["uptime"]   = millis() / 1000;
      d["vmin"]     = VB_VMIN_MV / 1000.0f;
      d["vmax"]     = VB_VMAX_MV / 1000.0f;
      d["imax"]     = VB_IMAX_MA / 1000.0f;
      d["temp_src"] = "mcp9700";
      d["heap"]     = ESP.getFreeHeap();
      JsonObject sl = d["seq_limits"].to<JsonObject>();
      sl["steps"]   = SEQ_MAX_STEPS;
      JsonObject wd = d["wifi_drop"].to<JsonObject>();
      wd["n"]       = g_wifi_drops;
      wd["reason"]  = g_wifi_last_reason;
      wd["at"]      = g_wifi_last_drop_s;
      wd["prev"]    = g_wifi_boot_reason;
    }
    String out;
    serializeJson(d, out);
    req->send(200, "application/json", out);
  });

  // v1-compatible control endpoint.
  server.on("/api/control", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req);
    String action = req->hasParam("action", true) ? req->getParam("action", true)->value() : "";
    uint8_t ch = 0;
    if      (req->hasParam("ch", true))     ch = (uint8_t)req->getParam("ch", true)->value().toInt();
    else if (req->hasParam("output", true)) ch = (uint8_t)req->getParam("output", true)->value().toInt();
    float v = req->hasParam("value", true) ? req->getParam("value", true)->value().toFloat() : 0.0f;

    VbErr e = apply_command_str(action, ch, v, SRC_WEB);
    if (e == VB_EQFULL) { req->send(503, "application/json", "{\"err\":\"qfull\"}"); return; }
    req->send(e == VB_OK || e == VB_ECLAMPED ? 200 : 400, "application/json",
              String("{\"err\":\"") + vb_err_str(e) + "\"}");
  });

  // Atomic per-channel update — what the UI actually needs.
  server.on("/api/channel", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req);
    uint8_t ch = req->hasParam("ch", true) ? (uint8_t)req->getParam("ch", true)->value().toInt() : 0;
    if (ch < 1 || ch > VB_CHANNELS) { req->send(400, "application/json", "{\"err\":\"badch\"}"); return; }
    if (req->hasParam("v", true))
      apply_command_str("set_voltage", ch, req->getParam("v", true)->value().toFloat(), SRC_WEB);
    if (req->hasParam("i", true))
      apply_command_str("set_current", ch, req->getParam("i", true)->value().toFloat(), SRC_WEB);
    if (req->hasParam("cc_mode", true))
      apply_command_str("cc_mode", ch,
        req->getParam("cc_mode", true)->value() == "trip" ? 1.0f : 0.0f, SRC_WEB);
    if (req->hasParam("en", true))
      apply_command_str("toggle", ch,
        req->getParam("en", true)->value() == "1" ? 1.0f : 0.0f, SRC_WEB);
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/prot/clear", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req);
    uint8_t ch = req->hasParam("ch", true) ? (uint8_t)req->getParam("ch", true)->value().toInt() : 0;
    if (ch < 1 || ch > VB_CHANNELS) { req->send(400, "application/json", "{\"err\":\"badch\"}"); return; }
    VbCommand c; memset(&c, 0, sizeof(c));
    c.op = OP_CLEAR_TRIP; c.ch = ch; c.src = SRC_WEB;
    VbErr e = apply_command(&c);
    // The FSM refuses while the fault persists; 409 says "not yet", not "no".
    req->send(e == VB_OK ? 200 : 409, "application/json",
              e == VB_OK ? "{\"ok\":true}" : "{\"err\":\"fault-present\"}");
  });

  // Chunked CSV. NEVER built into a String: 2048 rows is ~107 KB and String's
  // doubling realloc is an instant OOM on this heap.
  server.on("/api/log.csv", HTTP_GET, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req);
    g_logring.paused = 1;                       // tear-free snapshot
    uint32_t *cursor = new uint32_t(log_first(&g_logring));
    bool     *hdr    = new bool(false);

    AsyncWebServerResponse *r = req->beginChunkedResponse("text/csv",
      [cursor, hdr](uint8_t *buf, size_t maxLen, size_t) -> size_t {
        if (!*hdr) { *hdr = true; return (size_t)log_csv_header((char *)buf, (int)maxLen); }
        size_t used = 0;
        while (*cursor < g_logring.head && maxLen - used > 160) {
          int n = log_csv_row(&g_logring, *cursor, (char *)buf + used, (int)(maxLen - used));
          (*cursor)++;
          if (n <= 0) continue;
          used += (size_t)n;
        }
        if (!used) { g_logring.paused = 0; delete cursor; delete hdr; }
        return used;
      });
    r->addHeader("Content-Disposition", "attachment; filename=voltbench.csv");
    req->send(r);
  });

  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req);
    req->send(200, "application/json", "{\"ok\":true}");
    delay(100);
    ESP.restart();
  });

  // OTA upload — outputs are forced off first, because a reboot mid-experiment
  // with a live rail is exactly the accident this protects against.
  server.on("/api/ota/upload", HTTP_POST,
    [](AsyncWebServerRequest *req) {
      bool ok = !Update.hasError();
      req->send(ok ? 200 : 500, "application/json",
                ok ? "{\"ok\":true}" : "{\"err\":\"update\"}");
      if (ok) { delay(200); ESP.restart(); }
    },
    [](AsyncWebServerRequest *req, String, size_t index, uint8_t *data, size_t len, bool final) {
      if (!vb_authed(req)) return;
      if (!index) {
        for (int k = 0; k < VB_CHANNELS; k++) g_en_cmd[k] = 0;
        Dbg.println("OTA upload starting — outputs forced off");
        Update.begin(UPDATE_SIZE_UNKNOWN);
      }
      Update.write(data, len);
      if (final) Update.end(true);
    });

  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req);
    if (g_scan_result == -1) { req->send(202, "application/json", "{\"scanning\":true}"); return; }
    if (g_scan_result < 0)   { vb_trigger_scan(); req->send(202, "application/json", "{\"scanning\":true}"); return; }
    JsonDocument d;
    JsonArray a = d["nets"].to<JsonArray>();
    for (int i = 0; i < g_scan_result; i++) {
      if (WiFi.SSID(i).isEmpty()) continue;      // skip hidden
      JsonObject o = a.add<JsonObject>();
      o["ssid"] = WiFi.SSID(i);
      o["rssi"] = WiFi.RSSI(i);
    }
    String out; serializeJson(d, out);
    req->send(200, "application/json", out);
  });

  server.onNotFound([](AsyncWebServerRequest *req) {
    if (vb_in_captive) { req->redirect("/"); return; }
    req->send(404, "application/json", "{\"err\":\"notfound\"}");
  });

  vb_ws.onEvent(on_ws_event);
  server.addHandler(&vb_ws);
  logws.onEvent([](AsyncWebSocket *, AsyncWebSocketClient *, AwsEventType, void *, uint8_t *, size_t) {});
  server.addHandler(&logws);
}

// =============================================================================
//   WiFi events + manager
// =============================================================================
static void on_wifi_event(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    // The reason code IS the diagnosis: arduino-esp32 decides whether to
    // auto-reconnect from it (STA.cpp:58 _is_staReconnectableReason), and the
    // codes it refuses — AUTH_FAIL 202, ASSOC_LEAVE 8 — are exactly the ones
    // that stranded a v1 unit for a week until it was power-cycled.
    uint8_t reason = info.wifi_sta_disconnected.reason;
    g_wifi_last_reason = reason;
    g_wifi_last_drop_s = millis() / 1000;
    g_wifi_drops++;
    Dbg.printf("WiFi DROP #%u: reason=%u (%s) at %us uptime\n",
               (unsigned)g_wifi_drops, (unsigned)reason,
               WiFi.STA.disconnectReasonName((wifi_err_reason_t)reason),
               (unsigned)g_wifi_last_drop_s);
    return;
  }
  if (event == ARDUINO_EVENT_WIFI_STA_LOST_IP) {
    Dbg.println("WiFi LOST_IP — associated but no lease; watchdog will rejoin");
    return;
  }
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    if (vb_in_captive) {
      vb_dns.stop();
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      vb_in_captive = false;
      MDNS.begin(vb.mdns.c_str());
      Dbg.printf("Joined %s — captive AP down. IP=%s\n",
                 WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    } else {
      Dbg.printf("WiFi reconnected: %s @ %s\n",
                 WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    }
  }
}

static void wifiMgrTask(void *) {
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);

  uint32_t last_drop_s  = 0;
  uint8_t  saved_reason = vb_load_wifi_drop(&last_drop_s);
  g_wifi_boot_reason    = saved_reason;
  if (saved_reason)
    Dbg.printf("Last WiFi drop before this boot: reason=%u (%s) at %us uptime\n",
               (unsigned)saved_reason,
               WiFi.STA.disconnectReasonName((wifi_err_reason_t)saved_reason),
               (unsigned)last_drop_s);

  if (_tryAllSSIDs(5000)) {
    WiFi.setAutoReconnect(true);
    MDNS.begin(vb.mdns.c_str());
    Dbg.printf("WiFi OK: %s @ %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  } else {
    vb_start_captive_ap();
    // Scanning immediately after softAP() can return ESP_ERR_WIFI_STATE and
    // leave the scan permanently stuck. Let the radio settle first.
    vTaskDelay(pdMS_TO_TICKS(800));
    vb_trigger_scan();
  }
  xSemaphoreGive(g_wifi_ready);

  // The connection watchdog. setAutoReconnect(true) is NOT a recovery
  // guarantee and must never be the only one — see on_wifi_event above.
  // WL_CONNECTED is set on GOT_IP and cleared on LOST_IP, so this also covers a
  // DHCP lease that expires without renewing (associated but unreachable).
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(60000));

    // Flush the post-mortem BEFORE any `continue`: a drop that recovers on its
    // own must still be recorded. Gated on change, so a storm costs one write.
    if (g_wifi_last_reason != saved_reason) {
      saved_reason = g_wifi_last_reason;
      vb_save_wifi_drop(saved_reason, g_wifi_last_drop_s);
    }

    if (!vb_in_captive && WiFi.status() == WL_CONNECTED) continue;
    if (g_wifi_conn_busy) continue;
    while (g_scan_busy) vTaskDelay(pdMS_TO_TICKS(300));
    if (!vb_in_captive && WiFi.status() == WL_CONNECTED) continue;

    if (!vb_in_captive)
      Dbg.printf("WiFi link lost (status=%d) — retrying saved SSIDs\n", (int)WiFi.status());

    WiFi.setAutoReconnect(false);
    if (_tryAllSSIDs(5000)) WiFi.setAutoReconnect(true);
    else if (!vb_in_captive) vb_start_captive_ap();
  }
}

// =============================================================================
void vb_setup() {
  if (vb.auth_token.isEmpty()) { vb.auth_token = vb_make_token(); vb_save_token(); }
  WiFi.onEvent(on_wifi_event);

  g_wifi_ready = xSemaphoreCreateBinary();
  xTaskCreate(wifiMgrTask, "wifiMgr", STACK_WIFI, nullptr, PRIO_WIFI, nullptr);
  xSemaphoreTake(g_wifi_ready, pdMS_TO_TICKS(30000));

  register_routes();
}

void vb_loop() {
  if (vb_in_captive) vb_dns.processNextRequest();
  vb_push_status();
  vb_ws.cleanupClients();
  logws.cleanupClients();
}
