// =============================================================================
//   vb_nvs.h — persistent settings (NVS namespace "voltbench").
//
//   One mutex around every vbPrefs.begin()/end() pair: cal_save() runs on the
//   control task while web handlers save settings on the AsyncTCP task.
//
//   A Preferences write is a 10-40 ms flash erase. Never call these from an
//   AsyncTCP callback in v2.1 — it stalls the entire network stack.
// =============================================================================
#pragma once
#include "vb_config.h"
#include <Preferences.h>

struct VBSettings {
  String   mdns          = "voltbench";
  bool     require_login = true;
  String   password      = "admin";
  String   auth_token    = "";

  String   ota_url       = "https://updates.example.com/voltbench/";
  bool     ota_ssl       = true;
  bool     ota_auto      = true;

  bool     exp_fast      = true;
  bool     exp_smooth    = false;
  bool     exp_dial      = false;

  bool     mqtt_en       = false;
  String   mqtt_host     = "";
  uint16_t mqtt_port     = 1883;
  String   mqtt_user     = "";
  String   mqtt_pass     = "";
  String   mqtt_topic    = "voltbench";

  static const uint8_t MAX_SSID = 4;
  String   ssid[MAX_SSID];
  String   pass[MAX_SSID];
};

extern VBSettings        vb;
extern Preferences       vbPrefs;
extern SemaphoreHandle_t g_nvs_mux;

void vb_load();
void vb_save();
void vb_save_token();

// Output-1 calibration map persistence (keys "calmap"/"calok").
void cal_save();
void cal_load();

void vb_save_wifi_slot(uint8_t i, const String &ssid, const String &pass);
bool vb_remember_wifi(const String &ssid, const String &pass);
void vb_forget_wifi(const String &ssid);
void vb_factory_reset();
