#include "vb_nvs.h"
#include "vb_debug.h"

VBSettings vb;

Preferences vbPrefs;

// One mutex around every vbPrefs.begin()/end() pair — vb_save_outstate runs on the
// control task while web handlers save settings on the AsyncTCP task.
SemaphoreHandle_t g_nvs_mux = nullptr;
static void nvs_lock()   { xSemaphoreTake(g_nvs_mux, portMAX_DELAY); }
static void nvs_unlock() { xSemaphoreGive(g_nvs_mux); }

void vb_load() {
  nvs_lock();
  vbPrefs.begin("voltbench", true);
  vb.mdns          = vbPrefs.getString ("mdns",          vb.mdns);
  vb.require_login = vbPrefs.getBool   ("req_login",     vb.require_login);
  vb.password      = vbPrefs.getString ("password",      vb.password);
  vb.ota_url       = vbPrefs.getString ("ota_url",       vb.ota_url);
  vb.ota_ssl       = vbPrefs.getBool   ("ota_ssl",       vb.ota_ssl);
  vb.ota_auto      = vbPrefs.getBool   ("ota_auto",      vb.ota_auto);
  vb.exp_fast      = vbPrefs.getBool   ("e_fast",        vb.exp_fast);
  vb.exp_smooth    = vbPrefs.getBool   ("e_smooth",      vb.exp_smooth);
  vb.exp_dial      = vbPrefs.getBool   ("e_dial",        vb.exp_dial);
  vb.mqtt_en       = vbPrefs.getBool   ("mq_en",         vb.mqtt_en);
  vb.mqtt_host     = vbPrefs.getString ("mq_host",       vb.mqtt_host);
  vb.mqtt_port     = vbPrefs.getUShort ("mq_port",       vb.mqtt_port);
  vb.mqtt_user     = vbPrefs.getString ("mq_user",       vb.mqtt_user);
  vb.mqtt_pass     = vbPrefs.getString ("mq_pass",       vb.mqtt_pass);
  vb.mqtt_topic    = vbPrefs.getString ("mq_topic",      vb.mqtt_topic);
  for (uint8_t i = 0; i < VBSettings::MAX_SSID; i++) {
    vb.ssid[i] = vbPrefs.getString(("ssid" + String(i)).c_str(), "");
    vb.pass[i] = vbPrefs.getString(("pass" + String(i)).c_str(), "");
  }
  vb.auth_token    = vbPrefs.getString ("token",         vb.auth_token);
  vb.restore       = vbPrefs.getBool   ("restore",       vb.restore);
  vb.r_mv          = vbPrefs.getULong  ("r_mv",          0);
  vb.r_out         = vbPrefs.getUChar  ("r_out",         0);
  vbPrefs.end();
  nvs_unlock();
}

void vb_save() {
  nvs_lock();
  vbPrefs.begin("voltbench", false);
  vbPrefs.putString ("mdns",      vb.mdns);
  vbPrefs.putBool   ("req_login", vb.require_login);
  vbPrefs.putString ("password",  vb.password);
  vbPrefs.putString ("ota_url",   vb.ota_url);
  vbPrefs.putBool   ("ota_ssl",   vb.ota_ssl);
  vbPrefs.putBool   ("ota_auto",  vb.ota_auto);
  vbPrefs.putBool   ("e_fast",    vb.exp_fast);
  vbPrefs.putBool   ("e_smooth",  vb.exp_smooth);
  vbPrefs.putBool   ("e_dial",    vb.exp_dial);
  vbPrefs.putBool   ("mq_en",     vb.mqtt_en);
  vbPrefs.putString ("mq_host",   vb.mqtt_host);
  vbPrefs.putUShort ("mq_port",   vb.mqtt_port);
  vbPrefs.putString ("mq_user",   vb.mqtt_user);
  vbPrefs.putString ("mq_pass",   vb.mqtt_pass);
  vbPrefs.putString ("mq_topic",  vb.mqtt_topic);
  vbPrefs.putBool   ("restore",   vb.restore);
  vbPrefs.end();
  nvs_unlock();
}

void vb_save_outstate(uint32_t mv, uint8_t mask) {
  nvs_lock();
  vbPrefs.begin("voltbench", false);
  vbPrefs.putULong("r_mv",  mv);
  vbPrefs.putUChar("r_out", mask);
  vbPrefs.end();
  nvs_unlock();
  vb.r_mv = mv; vb.r_out = mask;
}

// Persist just the auth token (called after login / first-boot token mint —
// avoids re-writing every other setting on the hot login path).
void vb_save_token() {
  nvs_lock();
  vbPrefs.begin("voltbench", false);
  vbPrefs.putString("token", vb.auth_token);
  vbPrefs.end();
  nvs_unlock();
}

// WiFi drop post-mortem — see vb_nvs.h. Caller gates on "reason changed", so
// this is a handful of writes over the device's life, not one per disconnect.
void vb_save_wifi_drop(uint8_t reason, uint32_t uptime_s) {
  nvs_lock();
  vbPrefs.begin("voltbench", false);
  vbPrefs.putUChar("wd_rsn", reason);
  vbPrefs.putULong("wd_up",  uptime_s);
  vbPrefs.end();
  nvs_unlock();
}

uint8_t vb_load_wifi_drop(uint32_t *uptime_s) {
  nvs_lock();
  vbPrefs.begin("voltbench", true);
  uint8_t r = vbPrefs.getUChar("wd_rsn", 0);
  if (uptime_s) *uptime_s = vbPrefs.getULong("wd_up", 0);
  vbPrefs.end();
  nvs_unlock();
  return r;
}

void vb_save_wifi_slot(uint8_t i, const String &ssid, const String &pass) {
  if (i >= VBSettings::MAX_SSID) return;
  vb.ssid[i] = ssid; vb.pass[i] = pass;
  nvs_lock();
  vbPrefs.begin("voltbench", false);
  vbPrefs.putString(("ssid" + String(i)).c_str(), ssid);
  vbPrefs.putString(("pass" + String(i)).c_str(), pass);
  vbPrefs.end();
  nvs_unlock();
}

bool vb_remember_wifi(const String &ssid, const String &pass) {
  for (uint8_t i = 0; i < VBSettings::MAX_SSID; i++)
    if (vb.ssid[i] == ssid) { vb_save_wifi_slot(i, ssid, pass); return true; }
  for (uint8_t i = 0; i < VBSettings::MAX_SSID; i++)
    if (vb.ssid[i].isEmpty()) { vb_save_wifi_slot(i, ssid, pass); return true; }
  vb_save_wifi_slot(0, ssid, pass);
  return true;
}

void vb_forget_wifi(const String &ssid) {
  for (uint8_t i = 0; i < VBSettings::MAX_SSID; i++)
    if (vb.ssid[i] == ssid) vb_save_wifi_slot(i, "", "");
}

void vb_factory_reset() {
  nvs_lock();
  vbPrefs.begin("voltbench", false);
  vbPrefs.clear();
  vbPrefs.end();
  nvs_unlock();
}
