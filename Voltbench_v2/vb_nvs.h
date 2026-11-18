// =============================================================================
//   vb_nvs.h — Preferences wrapper, namespace "voltbench" (spec §3.6).
//
//   The partition is 20 KB and that is the binding constraint — which is why
//   sequences live in RAM, not here.
//
//   TWO RULES, BOTH LEARNED THE HARD WAY:
//   * NVS writes happen ONLY on loopTask (priority 1, harmless to block), via
//     the dirty flags in vb_state.h. A Preferences write is a 10-40 ms flash
//     erase; on ctrl it blows the control tick, on async_tcp it stalls the
//     entire network stack.
//   * Wear: writing last_* on every setpoint change would burn the partition
//     during a sequencer ramp (50 writes/s). At most once per 30 s, only if
//     changed, never while a sequence runs.
//
//   Migration is ADDITIVE via a `schema` key. Do NOT factory-reset on upgrade —
//   that destroys WiFi credentials and forces a captive-portal dance on every
//   dev flash.
// =============================================================================
#pragma once
#include "vb_config.h"
#include "vb_cal.h"
#include <Preferences.h>

#define VB_NVS_SCHEMA 2

typedef struct {          // exactly 22 bytes; 8 slots = one 176-byte blob
  char    name[12];
  int32_t v_mV;
  int32_t i_mA;
  uint8_t cc_mode;
  uint8_t used;
} VbPreset;

// Power-on policy per channel.
enum { PO_OFF = 0, PO_ON_LAST, PO_ON_FIXED };

struct VBSettings {
  String   mdns          = "voltbench";
  bool     require_login = true;
  String   password      = "admin";
  String   auth_token    = "";

  String   ota_url       = OTA_GITHUB_URL;
  bool     ota_ssl       = false;
  bool     ota_auto      = true;

  bool     mqtt_en       = false;
  String   mqtt_host     = "";
  uint16_t mqtt_port     = 1883;
  String   mqtt_user     = "";
  String   mqtt_pass     = "";
  String   mqtt_topic    = "voltbench";

  // Per-channel persisted operating state
  uint8_t  po_mode[VB_CHANNELS] = { PO_OFF, PO_OFF };
  int32_t  po_v[VB_CHANNELS]    = { 5000, 5000 };
  int32_t  po_i[VB_CHANNELS]    = { 500, 500 };
  int32_t  last_v[VB_CHANNELS]  = { 5000, 5000 };
  int32_t  last_i[VB_CHANNELS]  = { 500, 500 };
  uint8_t  last_en[VB_CHANNELS] = { 0, 0 };
  int32_t  vmax[VB_CHANNELS]    = { VB_VMAX_MV, VB_VMAX_MV };
  int32_t  imax[VB_CHANNELS]    = { VB_IMAX_MA, VB_IMAX_MA };

  static const uint8_t MAX_SSID = 4;
  String   ssid[MAX_SSID];
  String   pass[MAX_SSID];
};

extern VBSettings vb;
extern Preferences vbPrefs;
extern VbPreset   g_presets[PRESET_SLOTS];

void vb_load();
void vb_save();
void vb_save_token();

void vb_cal_save();
void vb_cal_load();

void vb_presets_save();
void vb_presets_load();
void vb_preset_save(uint8_t slot, uint8_t ch);
void vb_preset_load(uint8_t slot, uint8_t ch);

void vb_save_wifi_slot(uint8_t i, const String &ssid, const String &pass);
bool vb_remember_wifi(const String &ssid, const String &pass);
void vb_forget_wifi(const String &ssid);

// WiFi drop post-mortem — carried from v1. The debug ring is RAM, so a power
// cycle (the only way back from a wedged link) erases the evidence for the one
// failure you cannot watch happen. Written only when the reason CHANGES.
void    vb_save_wifi_drop(uint8_t reason, uint32_t uptime_s);
uint8_t vb_load_wifi_drop(uint32_t *uptime_s);

// Call from loopTask ONLY. Honours the dirty flags and the 30 s wear budget.
void vb_nvs_flush_tick();
