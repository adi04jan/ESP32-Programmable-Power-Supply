// =============================================================================
//   vb_net.cpp — was firmware-additions.h. Split out of the single translation
//   unit in Phase 0; the mid-file #include that this file used to require is
//   gone, and the forward-declaration block it carried is now real headers.
// =============================================================================
#include "vb_net.h"
#include "vb_config.h"
#include "vb_state.h"
#include "vb_nvs.h"
#include "vb_control.h"
#include "vb_debug.h"
#include "vb_ota.h"
#include "web_page.h"        // index_html_gz, index_html_gz_len
#include "vb_proto.h"        // vp_semver_newer / vp_parse_mv (host-tested)

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_random.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <Update.h>
#include <ESPmDNS.h>

// =============================================================================
//   WiFi manager — globals shared between manager task, scan task, HTTP handlers
// =============================================================================
static volatile int16_t  g_scan_result    = WIFI_SCAN_FAILED; // -2=idle,-1=running,>=0=count
static volatile bool     g_scan_busy      = false;
static volatile uint32_t g_scan_done_at   = 0;     // millis() at last scan completion (W5 cache)
static volatile bool     g_wifi_conn_busy = false; // true during any connection attempt
static SemaphoreHandle_t g_wifi_ready     = nullptr; // given when initial WiFi phase ends

// Forward declaration (defined in captive portal section below)
void vb_start_captive_ap();

// ---------- WiFi drop post-mortem ----------
// Set on the WiFi event task, persisted by wifiMgrTask — a Preferences write is
// a 10-40 ms flash erase and must not run on the event task. Read by /api/info
// straight from RAM, never via NVS, so the AsyncTCP task can't block on g_nvs_mux.
volatile uint8_t  g_wifi_last_reason = 0;   // last disconnect reason this boot (0 = none)
volatile uint32_t g_wifi_drops       = 0;   // drop count this boot
volatile uint32_t g_wifi_last_drop_s = 0;   // uptime seconds at the last drop
volatile uint8_t  g_wifi_boot_reason = 0;   // reason loaded from NVS at boot = why it died last time

// ---------- Scan task ----------
// Runs WiFi.scanNetworks() (blocking) on its own stack so it never executes on
// the lwIP / AsyncWebServer task.  CRITICAL: esp_wifi_connect() called during an
// active scan silently kills the scan and never fires WIFI_EVENT_SCAN_DONE —
// waitStatusBits then blocks for 60 s.  vb_trigger_scan() guards against this.
static void _wfScanTask(void*) {
  WiFi.scanDelete();
  int16_t n = WiFi.scanNetworks(false, true);  // blocking, include hidden SSIDs
  g_scan_result = (n < 0) ? WIFI_SCAN_FAILED : n;
  g_scan_done_at = millis();
  g_scan_busy   = false;
  vTaskDelete(NULL);
}

static void vb_trigger_scan() {
  if (g_scan_busy || g_wifi_conn_busy) return;
  g_scan_busy   = true;
  g_scan_result = WIFI_SCAN_RUNNING;
  xTaskCreate(_wfScanTask, "wfscan", 4096, nullptr, 1, nullptr);
}

// =============================================================================
//   Settings + persistent storage
// =============================================================================

// =============================================================================
//   Captive portal AP fallback
// =============================================================================
DNSServer vb_dns;
bool vb_in_captive = false;

// ---------- Connection helpers (must be called from a FreeRTOS task) ----------

// Try one SSID for up to timeoutMs; returns true on success.
static bool _trySSID(const char* ssid, const char* pass, uint32_t timeoutMs) {
  WiFi.begin(ssid, pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs)
    vTaskDelay(pdMS_TO_TICKS(100));
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.disconnect(false, false);
  vTaskDelay(pdMS_TO_TICKS(200));  // let the stack reach idle before next attempt
  return false;
}

// Try every saved SSID in order; holds g_wifi_conn_busy for the full duration so
// the scan task won't start while a connection attempt is in progress.
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
  WiFi.mode(WIFI_AP_STA);   // STA interface required for scanNetworks() in AP mode
  String apName = "Voltbench-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  WiFi.softAP(apName.c_str());
  vb_dns.start(53, "*", WiFi.softAPIP());
  vb_in_captive = true;
  Dbg.printf("Captive AP: %s @ %s\n", apName.c_str(), WiFi.softAPIP().toString().c_str());
  // Scan is triggered by wifiMgrTask after an 800 ms settle delay, not here
}

// =============================================================================
//   Auth helpers
// =============================================================================
String vb_make_token() {
  String t = "";
  for (int i = 0; i < 4; i++) t += String(esp_random(), HEX);
  return t;
}

bool vb_authed(AsyncWebServerRequest *req) {
  if (!vb.require_login) return true;
  if (req->hasHeader("X-Auth"))
    return req->getHeader("X-Auth")->value() == vb.auth_token;
  return false;
}
#define VB_REQUIRE_AUTH(req) do { if (!vb_authed(req)) { req->send(401, "application/json", "{\"err\":\"auth\"}"); return; } } while (0)

// "1"/"on"/"true" -> 1, "0"/"off"/"false" -> 0, a decimal -> its value, else NAN
// (which apply_command rejects). The old toFloat() turned garbage into 0, and
// "on"/"true" therefore switched an output OFF.
static float vb_val_from_str(String v) {
  v.trim(); v.toLowerCase();
  if (v == "on"  || v == "true")  return 1.0f;
  if (v == "off" || v == "false") return 0.0f;
  uint32_t mv;
  return vp_parse_mv(v.c_str(), &mv) ? mv / 1000.0f : NAN;
}

// =============================================================================
//   WebSocket + telemetry broadcast
// =============================================================================
AsyncWebSocket vb_ws("/ws");
AsyncWebSocket logws("/logws");   // browser debug-log stream (view-only)

void vb_fill_status(JsonDocument &d) {
  d["output1"]  = psState.output1;
  d["output2"]  = psState.output2;
  d["output3"]  = psState.output3;
  d["voltage1"] = g_display_mV / 1000.0f;   // CH1 sense, median-of-5 (reads LOW under load)
  d["expected1"] = vb_expected1_mv() / 1000.0f;  // map value for the current pot step
  d["cal"]      = g_cal_valid;
  d["voltage2"] = g_5v_mV  / 1000.0f;
  d["voltage3"] = g_3v3_mV / 1000.0f;
  d["set1"]     = g_setpoint_mV / 1000.0f;
  d["ts"]       = (uint64_t)millis();
  d["state"]    = vb_ctl_name(g_vctrl_state);
}

uint32_t vb_last_push = 0;
void vb_push_status() {
  if (vb_ws.count() == 0) return;
  if (millis() - vb_last_push < (vb.exp_fast ? 250 : 1000)) return;
  // A client that stops ACKing must not back up the send queues — queueing onto
  // it starves loop()'s whole TX path (observed: frames delivered minutes late,
  // telnet dead). Library docs mandate this check before textAll.
  if (!vb_ws.availableForWriteAll()) return;   // skip this round; drop, don't queue
  vb_last_push = millis();
  JsonDocument d;
  vb_fill_status(d);
  String out; serializeJson(d, out);
  vb_ws.textAll(out);
}

void vb_on_ws_event(AsyncWebSocket *srv, AsyncWebSocketClient *c,
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
  String act = action;
  int    output = doc["output"] | 0;
  if (output < 0 || output > 3) output = 0;   // reject out-of-range before narrowing to uint8_t
  JsonVariant jv = doc["value"];
  float value = jv.is<bool>()        ? (jv.as<bool>() ? 1.0f : 0.0f)
              : jv.is<const char*>() ? vb_val_from_str(jv.as<const char*>())
              : jv.isNull()          ? 0.0f
              :                        jv.as<float>();
  bool ok = apply_command(act, (uint8_t)output, value, SRC_WS);

  JsonDocument resp;
  vb_fill_status(resp);
  if (!ok) resp["err"] = "rejected";    // unknown action, bad channel, or value out of range
  String out; serializeJson(resp, out);
  c->text(out);
}

// =============================================================================
//   MQTT bridge
// =============================================================================
WiFiClient   vb_mqtt_net;
PubSubClient vb_mqtt(vb_mqtt_net);
uint32_t     vb_last_mqtt = 0;

void vb_mqtt_cmd(char *topic, byte *payload, unsigned int len) {
  String t = topic;
  String p; p.reserve(len);
  for (unsigned i = 0; i < len; i++) p += (char)payload[i];
  String base = vb.mqtt_topic + "/cmd/";
  if (!t.startsWith(base)) return;
  String tail = t.substring(base.length());
  bool ok;
  if (tail == "all_off") ok = apply_command("all_off", 0, 0, SRC_MQTT);
  else {
    int slash = tail.indexOf('/');
    if (slash < 0) return;
    uint8_t ch = (uint8_t)tail.substring(2, slash).toInt();
    String  fn = tail.substring(slash + 1);
    float   v  = vb_val_from_str(p);
    if      (fn == "output")  ok = apply_command("toggle",      ch, v, SRC_MQTT);
    else if (fn == "voltage") ok = apply_command("set_voltage", ch, v, SRC_MQTT);
    else if (fn == "cycle")   ok = apply_command("cycle",       ch, v, SRC_MQTT);
    else ok = false;
  }
  if (!ok) Dbg.printf("[mqtt] rejected %s = '%s'\n", t.c_str(), p.c_str());
}

void vb_mqtt_setup() {
  if (!vb.mqtt_en || vb.mqtt_host.isEmpty()) return;
  vb_mqtt.setServer(vb.mqtt_host.c_str(), vb.mqtt_port);
  vb_mqtt.setCallback(vb_mqtt_cmd);
  vb_mqtt.setSocketTimeout(2);      // seconds — an unreachable broker must not stall loop()
  vb_mqtt_net.setTimeout(2000);     // ms — underlying WiFiClient read timeout
}

void vb_mqtt_loop() {
  if (!vb.mqtt_en || vb.mqtt_host.isEmpty() || vb_in_captive) return;
  if (!vb_mqtt.connected()) {
    // Each connect attempt blocks loop() up to the socket timeout (~2 s), which
    // stalls the USB console, telnet and WS pushes. Back off to once a minute
    // while the broker is unreachable; retry quickly only after a live link drops.
    static uint32_t backoff = 5000;
    if (millis() - vb_last_mqtt < backoff) return;
    vb_last_mqtt = millis();
    String cid  = "voltbench-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    String will = vb.mqtt_topic + "/status";
    bool ok = vb_mqtt.connect(cid.c_str(),
                              vb.mqtt_user.isEmpty() ? nullptr : vb.mqtt_user.c_str(),
                              vb.mqtt_user.isEmpty() ? nullptr : vb.mqtt_pass.c_str(),
                              will.c_str(), 0, true, "offline");   // LWT: broker marks us offline
    backoff = ok ? 5000 : 60000;
    if (ok) {
      vb_mqtt.subscribe((vb.mqtt_topic + "/cmd/#").c_str());
      vb_mqtt.publish ((vb.mqtt_topic + "/status").c_str(), "online", true);
    }
    return;
  }
  vb_mqtt.loop();
  static uint32_t last = 0;
  if (millis() - last > 1000) {
    last = millis();
    JsonDocument d; vb_fill_status(d);
    String out; serializeJson(d, out);
    vb_mqtt.publish((vb.mqtt_topic + "/tele").c_str(), out.c_str());
  }
}

// =============================================================================
//   HTTP routes (/api/* — replaces old /, /control, /status)
// =============================================================================
void vb_register_routes() {

  // Gzipped index
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    AsyncWebServerResponse *res =
      req->beginResponse_P(200, "text/html", index_html_gz, index_html_gz_len);
    res->addHeader("Content-Encoding", "gzip");
    res->addHeader("Cache-Control", "no-cache");
    req->send(res);
  });

  server.onNotFound([](AsyncWebServerRequest *req) {
    if (vb_in_captive) { req->redirect("/"); return; }
    req->send(404, "text/plain", "Not found");
  });

  // Status — live telemetry, must never be cached (browsers heuristically cache
  // GETs with no Cache-Control, which froze the displayed voltage).
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    VB_REQUIRE_AUTH(req);
    JsonDocument d; vb_fill_status(d);
    String out; serializeJson(d, out);
    AsyncWebServerResponse *res = req->beginResponse(200, "application/json", out);
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
    return;
  });

  // Info
  server.on("/api/info", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument d;
    d["fw"]            = CURRENT_FIRMWARE_VERSION;
    d["mdns"]          = vb.mdns;
    d["require_login"] = vb.require_login;
    d["captive"]       = vb_in_captive;
    if (vb_authed(req)) {
      d["ip"]            = WiFi.localIP().toString();
      d["mac"]           = WiFi.macAddress();
      d["ssid"]          = WiFi.SSID();
      d["rssi"]          = WiFi.RSSI();
      d["uptime"]        = millis() / 1000;
      d["reset"]         = vb_reset_reason();
      d["restore"]       = vb.restore;
      // WiFi post-mortem. Served from RAM only — an NVS read here would take
      // g_nvs_mux on the AsyncTCP task and stall the stack behind an NVS write.
      auto wd = d["wifi_drop"].to<JsonObject>();
        wd["n"]      = g_wifi_drops;         // drops seen this boot
        wd["reason"] = g_wifi_last_reason;   // last reason this boot (0 = none)
        wd["at"]     = g_wifi_last_drop_s;
        wd["prev"]   = g_wifi_boot_reason;   // why the link died before this boot
      auto ota = d["ota"].to<JsonObject>();
        ota["auto"] = vb.ota_auto;
      auto exp = d["exp"].to<JsonObject>();
        exp["fast"]   = vb.exp_fast;
        exp["smooth"] = vb.exp_smooth;  exp["dial"]  = vb.exp_dial;
      auto m = d["mqtt"].to<JsonObject>();
        m["enabled"] = vb.mqtt_en; m["host"] = vb.mqtt_host;
        m["port"]    = vb.mqtt_port; m["user"] = vb.mqtt_user;
        m["topic"]   = vb.mqtt_topic;
      auto saved = d["saved_ssids"].to<JsonArray>();
      for (uint8_t i = 0; i < VBSettings::MAX_SSID; i++)
        if (!vb.ssid[i].isEmpty()) saved.add(vb.ssid[i]);
      d["vmax"] = VB_VMAX_MV / 1000.0f;
    }
    String out; serializeJson(d, out);
    req->send(200, "application/json", out);
  });

  // Login
  server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest *req) {
    String pass = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : "";
    if (!vb.require_login || pass == vb.password) {
      vb.auth_token = vb_make_token();
      vb_save_token();
      JsonDocument d; d["ok"] = true; d["token"] = vb.auth_token;
      String out; serializeJson(d, out);
      req->send(200, "application/json", out);
    } else {
      req->send(200, "application/json", "{\"ok\":false,\"err\":\"bad-pass\"}");
    }
  });

  // Control
  server.on("/api/control", HTTP_POST, [](AsyncWebServerRequest *req) {
    VB_REQUIRE_AUTH(req);
    String action = req->hasParam("action", true) ? req->getParam("action", true)->value() : "";
    long   out    = req->hasParam("output", true) ? req->getParam("output", true)->value().toInt() : 0;
    float  val    = req->hasParam("value",  true) ? vb_val_from_str(req->getParam("value", true)->value()) : 0.0f;
    if (out < 0 || out > 3) out = 255;           // never let 257 narrow to channel 1
    if (apply_command(action, (uint8_t)out, val, SRC_HTTP))
      req->send(200, "application/json", "{\"ok\":true}");
    else
      req->send(400, "application/json",
        "{\"ok\":false,\"err\":\"rejected: unknown action, bad output, or value out of range (set_voltage 2.5-17 V)\"}");
  });

  // Settings (JSON body)
  AsyncCallbackJsonWebHandler *settingsHandler =
    new AsyncCallbackJsonWebHandler("/api/settings",
      [](AsyncWebServerRequest *req, JsonVariant &json) {
        if (!vb_authed(req)) { req->send(401); return; }
        JsonObject s = json.as<JsonObject>();
        if (s["mdns"].is<const char*>())     vb.mdns          = s["mdns"].as<String>();
        if (s["require_login"].is<bool>())   vb.require_login = s["require_login"];
        if (s["password"].is<const char*>()) vb.password      = s["password"].as<String>();
        if (s["restore"].is<bool>())         vb.restore       = s["restore"];
        if (s["ota"].is<JsonObject>()) {
          JsonObject o = s["ota"];
          if (o["auto"].is<bool>())       vb.ota_auto = o["auto"];
        }
        if (s["exp"].is<JsonObject>()) {
          JsonObject e = s["exp"];
          if (e["fast"]   .is<bool>()) vb.exp_fast    = e["fast"];
          if (e["smooth"] .is<bool>()) vb.exp_smooth  = e["smooth"];
          if (e["dial"]   .is<bool>()) vb.exp_dial    = e["dial"];
        }
        if (s["mqtt"].is<JsonObject>()) {
          JsonObject m = s["mqtt"];
          if (m["enabled"].is<bool>())     vb.mqtt_en   = m["enabled"];
          if (m["host"].is<const char*>()) vb.mqtt_host = m["host"].as<String>();
          if (m["port"].is<int>())         vb.mqtt_port = m["port"];
          if (m["user"].is<const char*>()) vb.mqtt_user = m["user"].as<String>();
          if (m["pass"].is<const char*>()) vb.mqtt_pass = m["pass"].as<String>();
          if (m["topic"].is<const char*>())vb.mqtt_topic= m["topic"].as<String>();
          vb_mqtt_setup();
        }
        vb_save();
        req->send(200, "application/json", "{\"ok\":true}");
      });
  server.addHandler(settingsHandler);

  // Change log: last 32 output/setpoint changes with their source.
  server.on("/api/log", HTTP_GET, [](AsyncWebServerRequest *req) {
    VB_REQUIRE_AUTH(req);
    JsonDocument d;
    auto a = d["lines"].to<JsonArray>();
    char buf[96];
    int n = vb_log_count();
    for (int i = 0; i < n; i++) if (vb_log_line(i, buf, sizeof(buf))) a.add(buf);
    d["n"] = n;
    String out; serializeJson(d, out);
    AsyncWebServerResponse *res = req->beginResponse(200, "application/json", out);
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
  });
  server.on("/api/cal/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    VB_REQUIRE_AUTH(req);
    JsonDocument d;
    d["valid"]   = g_cal_valid;
    d["fixed"]   = true;               // compiled-in map, no sweep (control_math.h)
    auto a = d["mv"].to<JsonArray>();
    for (int i = 0; i < 128; i++) a.add(g_cal_mv[i]);
    String out; serializeJson(d, out);
    req->send(200, "application/json", out);
  });

  // Wi-Fi scan — results come from a dedicated FreeRTOS task (blocking scan).
  // Never start a scan while a WiFi connection attempt is in progress; calling
  // esp_wifi_connect() during a scan kills it silently (no SCAN_DONE event).
  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *req) {
    VB_REQUIRE_AUTH(req);
    if (g_wifi_conn_busy || g_scan_result == WIFI_SCAN_RUNNING) {
      req->send(202, "application/json", "{\"scanning\":true}"); return;
    }
    if (g_scan_result == WIFI_SCAN_FAILED || millis() - g_scan_done_at > 20000) {
      vb_trigger_scan();
      req->send(202, "application/json", "{\"scanning\":true}"); return;
    }
    JsonDocument d;
    auto arr = d["networks"].to<JsonArray>();
    for (int16_t i = 0; i < g_scan_result; i++) {
      if (WiFi.SSID(i).isEmpty()) continue;          // W6: skip hidden networks
      auto o = arr.add<JsonObject>();
      o["ssid"] = WiFi.SSID(i);
      o["rssi"] = WiFi.RSSI(i);
      o["sec"]  = WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured";
    }
    String out; serializeJson(d, out);
    req->send(200, "application/json", out);
    // W5: results stay cached for 20 s — no consume-and-invalidate.
  });

  // Wi-Fi connect — one attempt at a time; creds saved only after success.
  server.on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *req) {
    VB_REQUIRE_AUTH(req);
    String ssid = req->hasParam("ssid", true) ? req->getParam("ssid", true)->value() : "";
    String pass = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : "";
    if (ssid.isEmpty())    { req->send(400, "application/json", "{\"err\":\"ssid\"}"); return; }
    if (g_wifi_conn_busy)  { req->send(409, "application/json", "{\"err\":\"busy\"}"); return; }
    g_wifi_conn_busy = true;
    struct Req { String ssid, pass; };
    auto *r = new Req{ssid, pass};
    BaseType_t ok = xTaskCreate([](void *pv) {
      Req *r = (Req *)pv;
      vTaskDelay(pdMS_TO_TICKS(500));            // let the HTTP response flush
      WiFi.setAutoReconnect(false);
      WiFi.disconnect(false, false);
      vTaskDelay(pdMS_TO_TICKS(300));
      WiFi.begin(r->ssid.c_str(), r->pass.c_str());
      uint32_t t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000)
        vTaskDelay(pdMS_TO_TICKS(100));
      if (WiFi.status() == WL_CONNECTED) {
        vb_remember_wifi(r->ssid, r->pass);      // W4: save only creds that worked
        WiFi.setAutoReconnect(true);
        g_wifi_conn_busy = false;
      } else {
        Dbg.printf("[WiFi] join '%s' failed — recovering\n", r->ssid.c_str());
        g_wifi_conn_busy = false;                // release before _tryAllSSIDs re-claims
        if (_tryAllSSIDs(5000)) WiFi.setAutoReconnect(true);
        else if (!vb_in_captive) vb_start_captive_ap();   // W1: never orphaned
      }
      delete r;
      vTaskDelete(NULL);
    }, "wifiSwitch", 4096, r, 1, NULL);
    if (ok != pdPASS) {
      delete r;
      g_wifi_conn_busy = false;
      req->send(500, "application/json", "{\"err\":\"task\"}");
      return;
    }
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // Wi-Fi forget
  server.on("/api/wifi/forget", HTTP_POST, [](AsyncWebServerRequest *req) {
    VB_REQUIRE_AUTH(req);
    if (!req->hasParam("ssid", true)) { req->send(400); return; }
    vb_forget_wifi(req->getParam("ssid", true)->value());
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // OTA
  server.on("/api/ota/check", HTTP_GET, [](AsyncWebServerRequest *req) {
    VB_REQUIRE_AUTH(req);
    if (!ota_busy && (ota_checked_at == 0 || millis() - ota_checked_at > 60000)) {
      ota_busy = true;
      xTaskCreatePinnedToCore([](void *) {
        ota_latest     = ota_fetch_latest(vb.ota_url, vb.ota_ssl);
        ota_checked_at = millis();
        ota_busy       = false;
        vTaskDelete(NULL);
      }, "otaChk", 16384, NULL, 1, NULL, 0);
    }
    if (ota_busy) { req->send(202, "application/json", "{\"checking\":true}"); return; }
    JsonDocument d;
    d["current"] = CURRENT_FIRMWARE_VERSION;
    d["latest"]  = ota_latest.length() ? ota_latest : "unknown";
    d["newer"]   = vp_semver_newer(ota_latest.c_str(), CURRENT_FIRMWARE_VERSION);
    String out; serializeJson(d, out);
    req->send(200, "application/json", out);
  });
  server.on("/api/ota/update", HTTP_POST, [](AsyncWebServerRequest *req) {
    VB_REQUIRE_AUTH(req);
    if (ota_busy) { req->send(409, "application/json", "{\"ok\":false,\"err\":\"busy\"}"); return; }
    if (ota_latest.length() && !vp_semver_newer(ota_latest.c_str(), CURRENT_FIRMWARE_VERSION)) {
      req->send(200, "application/json", "{\"ok\":false,\"err\":\"up-to-date\"}"); return;
    }
    req->send(200, "application/json", "{\"ok\":true,\"msg\":\"checking GitHub\"}");
    extern void perform_ota_tasked(bool);
    perform_ota_tasked(false);            // only update if version.txt is newer
  });
  server.on("/api/ota/force", HTTP_POST, [](AsyncWebServerRequest *req) {
    VB_REQUIRE_AUTH(req);
    if (ota_busy) { req->send(409, "application/json", "{\"ok\":false,\"err\":\"busy\"}"); return; }
    req->send(200, "application/json", "{\"ok\":true,\"msg\":\"forcing re-flash from GitHub\"}");
    extern void perform_ota_tasked(bool);
    perform_ota_tasked(true);             // re-download even if same version
  });

  // MQTT test
  server.on("/api/mqtt/test", HTTP_GET, [](AsyncWebServerRequest *req) {
    VB_REQUIRE_AUTH(req);
    if (vb.mqtt_host.isEmpty()) {
      req->send(200, "application/json", "{\"ok\":false,\"err\":\"no broker host configured\"}");
      return;
    }
    // ponytail: still a blocking connect on the AsyncTCP task, capped at 1 s;
    // move to a 202 + task (like /api/ota/check) if a slow broker ever bites.
    WiFiClient c;
    bool ok = c.connect(vb.mqtt_host.c_str(), vb.mqtt_port, 1000);
    c.stop();
    req->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"connect failed\"}");
  });

  // Reboot
  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *req) {
    VB_REQUIRE_AUTH(req);
    req->send(200, "application/json", "{\"ok\":true}");
    delay(200);
    ESP.restart();
  });

  // Factory reset
  server.on("/api/factory_reset", HTTP_POST, [](AsyncWebServerRequest *req) {
    VB_REQUIRE_AUTH(req);
    vb_factory_reset();
    req->send(200, "application/json", "{\"ok\":true}");
    delay(200);
    ESP.restart();
  });

  // Web-upload OTA — POST the compiled .bin (multipart, field name doesn't matter):
  //   curl -H "X-Auth: <token>" -F "f=@ESP32_power_supply.ino.bin" http://<ip>/api/ota/upload
  // Writes the inactive OTA slot via Update.h and reboots on success. No USB needed.
  server.on("/api/ota/upload", HTTP_POST,
    [](AsyncWebServerRequest *req) {
      if (!vb_authed(req)) { req->send(401, "application/json", "{\"err\":\"auth\"}"); return; }
      // _tempObject marks the request that owns the Update session (freed by the
      // request's destructor). A rejected concurrent upload must not touch it.
      if (!req->_tempObject) { req->send(409, "application/json", "{\"ok\":false,\"err\":\"busy\"}"); return; }
      bool ok = Update.isFinished() && !Update.hasError();
      if (!ok) { if (Update.isRunning()) Update.abort(); ota_busy = false; }
      AsyncWebServerResponse *res = req->beginResponse(ok ? 200 : 500, "application/json",
        ok ? "{\"ok\":true,\"msg\":\"flashed, rebooting\"}" : "{\"ok\":false,\"err\":\"update failed\"}");
      res->addHeader("Connection", "close");
      req->send(res);
      if (ok) { delay(300); ESP.restart(); }
    },
    [](AsyncWebServerRequest *req, String fn, size_t index, uint8_t *data, size_t len, bool final) {
      static uint32_t last_chunk = 0;
      if (!vb_authed(req)) return;                       // ignore unauthenticated uploads
      if (index == 0) {
        // A client that vanished mid-upload leaves a half-written session; abort
        // it once it has been idle 10 s instead of blocking every later upload.
        if (Update.isRunning() && millis() - last_chunk > 10000) {
          Update.abort(); ota_busy = false;
          Dbg.println("[OTA] stale upload session aborted");
        }
        // Busy = another upload in flight OR a pull-OTA/check running. The old
        // test was inverted: it let a second upload write into the first's session.
        if (ota_busy || Update.isRunning()) { Dbg.println("[OTA] busy — upload rejected"); return; }
        ota_busy = true;
        req->_tempObject = malloc(1);
        Dbg.printf("[OTA] web upload start: %s\n", fn.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Dbg);
      }
      if (!req->_tempObject) return;                     // not the owning upload
      last_chunk = millis();
      if (Update.isRunning() && Update.write(data, len) != len) Update.printError(Dbg);
      if (final && Update.isRunning()) {
        if (Update.end(true)) Dbg.printf("[OTA] web upload ok: %u bytes\n", (unsigned)(index + len));
        else Update.printError(Dbg);
        ota_busy = false;
      }
    });

  // WebSocket(s)
  vb_ws.onEvent(vb_on_ws_event);
  server.addHandler(&vb_ws);
  logws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType, void*, uint8_t*, size_t){});
  server.addHandler(&logws);
}

// =============================================================================
//   Setup / loop helpers called from the main sketch
// =============================================================================
static volatile bool g_wifi_up = false;   // set on GOT_IP; a drop is only a drop from here

void vb_on_wifi_event(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    // Our own retries (_trySSID's disconnect, failed joins) fire this too. Only
    // the first event after a working link is the real cause; recording the
    // rest overwrote it with retry noise and inflated the drop count.
    if (!g_wifi_up) return;
    g_wifi_up = false;
    // The reason code is the whole diagnosis: arduino-esp32 decides whether to
    // auto-reconnect from it (STA.cpp:58 _is_staReconnectableReason), and the
    // codes it refuses — AUTH_FAIL 202, ASSOC_LEAVE 8 — are exactly the ones
    // that used to strand the device. Nothing logged this before.
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
    g_wifi_up = true;
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

// ---------- WiFi manager task ----------
// Owns the entire connection lifecycle so loop() never blocks on WiFi calls.
// Priority 2 (same as voltageControlTask) — time-shares core 0 via vTaskDelay yields.
static void wifiMgrTask(void*) {
  WiFi.setAutoReconnect(false);   // we drive all reconnection ourselves
  WiFi.mode(WIFI_STA);

  // First thing after a power cycle: say why the last link died. Without this
  // the RAM log ring is already gone and the failure is unfalsifiable.
  uint32_t last_drop_s   = 0;
  uint8_t  saved_reason  = vb_load_wifi_drop(&last_drop_s);
  g_wifi_boot_reason     = saved_reason;
  if (saved_reason)
    Dbg.printf("Last WiFi drop before this boot: reason=%u (%s) at %us uptime\n",
               (unsigned)saved_reason,
               WiFi.STA.disconnectReasonName((wifi_err_reason_t)saved_reason),
               (unsigned)last_drop_s);

  // Boot: try every saved SSID with a 5 s timeout (was 12 s; reduces AP-start delay)
  bool ok = _tryAllSSIDs(5000);

  if (ok) {
    WiFi.setAutoReconnect(true);
    MDNS.begin(vb.mdns.c_str());
    Dbg.printf("WiFi OK: %s @ %s\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  } else {
    vb_start_captive_ap();
    // Wait 800 ms for the AP radio to settle before starting the first scan.
    // Calling esp_wifi_scan_start() immediately after softAP() can return
    // ESP_ERR_WIFI_STATE and leave the scan permanently stuck.
    vTaskDelay(pdMS_TO_TICKS(800));
    vb_trigger_scan();
  }

  // Signal vb_setup() — it has been blocking on this semaphore
  xSemaphoreGive(g_wifi_ready);

  // Background loop: the connection watchdog. Retries saved SSIDs whenever the
  // link is down — in captive portal mode OR after the link dropped in service.
  //
  // setAutoReconnect(true) is NOT a recovery guarantee and must never be the only
  // one. arduino-esp32 3.2.1 STA.cpp gives up permanently on any disconnect reason
  // missing from _is_staReconnectableReason(): WIFI_REASON_AUTH_FAIL (202) falls
  // through to `default: return false`, and WIFI_REASON_ASSOC_LEAVE (8) is an
  // explicit "Voluntarily disconnected. Don't reconnect!". One such deauth — a
  // router reboot, PSK rotation, band steering — used to wedge the STA until a
  // power cycle, because this loop only ever looked at vb_in_captive.
  // WL_CONNECTED is set on GOT_IP and cleared on LOST_IP, so it also covers a
  // DHCP lease that expires without renewing (associated but unreachable).
  //
  // NEVER runs concurrently with g_scan_busy; doing so would call esp_wifi_connect()
  // mid-scan which silently kills the scan (no WIFI_EVENT_SCAN_DONE ever fires).
  //
  // In service the watchdog NEVER opens the access point: that AP is open (no
  // password) and sat next to a 16 V supply after any router outage over 60 s.
  // It just keeps retrying the saved networks every 15 s. The AP is a boot-time
  // fallback only (no saved network reachable at power-up); the USB console
  // always works regardless.
  uint32_t last_captive_try = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(15000));   // check interval

    // Flush the post-mortem BEFORE any `continue` — a drop that recovers on its
    // own must still be recorded, since a self-healing drop today is the same
    // fault as a fatal one tomorrow. Gated on change: a storm costs one write.
    if (g_wifi_last_reason != saved_reason) {
      saved_reason = g_wifi_last_reason;
      vb_save_wifi_drop(saved_reason, g_wifi_last_drop_s);
    }

    if (!vb_in_captive && WiFi.status() == WL_CONNECTED) continue;
    if (g_wifi_conn_busy) continue;     // a user-initiated connect owns the radio
    // In captive mode each STA attempt hops the radio and drops phones on the
    // AP mid-setup, so only try once a minute there.
    if (vb_in_captive && millis() - last_captive_try < 60000) continue;
    last_captive_try = millis();

    // Politely wait for any in-progress scan to finish before connecting
    while (g_scan_busy) vTaskDelay(pdMS_TO_TICKS(300));
    if (!vb_in_captive && WiFi.status() == WL_CONNECTED) continue;  // re-check

    if (!vb_in_captive)
      Dbg.printf("WiFi link lost (status=%d) — retrying saved SSIDs\n", (int)WiFi.status());

    WiFi.setAutoReconnect(false);
    bool reconnected = _tryAllSSIDs(5000);
    if (reconnected) WiFi.setAutoReconnect(true);
    else if (!vb_in_captive) Dbg.println("WiFi still down — retrying in 15 s (no AP in service)");
    // vb_on_wifi_event handles AP teardown when GOT_IP fires after reconnect
  }
}

void vb_setup() {
  if (vb.auth_token.isEmpty()) { vb.auth_token = vb_make_token(); vb_save_token(); }
  WiFi.onEvent(vb_on_wifi_event);

  g_wifi_ready = xSemaphoreCreateBinary();
  // Stack 8 KB — WiFi.begin() and the IDF scan stack are deeper than they look
  xTaskCreate(wifiMgrTask, "wifiMgr", 8192, nullptr, 2, nullptr);

  // Block until wifiMgrTask finishes its initial phase (max ~25 s: 4 SSIDs × 5 s + margin)
  xSemaphoreTake(g_wifi_ready, pdMS_TO_TICKS(30000));

  vb_register_routes();
  vb_mqtt_setup();
}

void vb_loop() {
  if (vb_in_captive) vb_dns.processNextRequest();
  // WiFi connection management is handled entirely by wifiMgrTask — nothing here.
  vb_push_status();
  vb_mqtt_loop();
  vb_ws.cleanupClients();
  logws.cleanupClients();
}
