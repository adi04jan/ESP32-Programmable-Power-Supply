#include "vb_nvs.h"
#include "vb_state.h"
#include "vb_debug.h"
#include "vb_seq.h"
#include "vb_control.h"

VBSettings  vb;
Preferences vbPrefs;
VbPreset    g_presets[PRESET_SLOTS];

static void nvs_lock()   { if (g_nvs_mux) xSemaphoreTake(g_nvs_mux, portMAX_DELAY); }
static void nvs_unlock() { if (g_nvs_mux) xSemaphoreGive(g_nvs_mux); }

static const char *K_PO_MODE[] = { "po_mode1", "po_mode2" };
static const char *K_PO_V[]    = { "po_v1",    "po_v2"    };
static const char *K_PO_I[]    = { "po_i1",    "po_i2"    };
static const char *K_LAST_V[]  = { "lastv1",   "lastv2"   };
static const char *K_LAST_I[]  = { "lasti1",   "lasti2"   };
static const char *K_LAST_EN[] = { "lasten1",  "lasten2"  };
static const char *K_OVP[]     = { "ovp1",     "ovp2"     };
static const char *K_OCP[]     = { "ocp1",     "ocp2"     };
static const char *K_CCMODE[]  = { "ccmode1",  "ccmode2"  };
static const char *K_VMAX[]    = { "vmax1",    "vmax2"    };
static const char *K_IMAX[]    = { "imax1",    "imax2"    };
static const char *K_CAL[]     = { "cal1",     "cal2"     };
static const char *K_CALOK[]   = { "calok1",   "calok2"   };

// -----------------------------------------------------------------------------
void vb_load() {
  nvs_lock();
  vbPrefs.begin("voltbench", true);

  uint32_t schema = vbPrefs.getUInt("schema", 0);

  vb.mdns          = vbPrefs.getString("mdns",      vb.mdns);
  vb.require_login = vbPrefs.getBool  ("req_login", vb.require_login);
  vb.password      = vbPrefs.getString("password",  vb.password);
  vb.auth_token    = vbPrefs.getString("token",     vb.auth_token);
  vb.ota_auto      = vbPrefs.getBool  ("ota_auto",  vb.ota_auto);
  vb.mqtt_en       = vbPrefs.getBool  ("mq_en",     vb.mqtt_en);
  vb.mqtt_host     = vbPrefs.getString("mq_host",   vb.mqtt_host);
  vb.mqtt_port     = vbPrefs.getUShort("mq_port",   vb.mqtt_port);
  vb.mqtt_user     = vbPrefs.getString("mq_user",   vb.mqtt_user);
  vb.mqtt_pass     = vbPrefs.getString("mq_pass",   vb.mqtt_pass);
  vb.mqtt_topic    = vbPrefs.getString("mq_topic",  vb.mqtt_topic);

  for (int k = 0; k < VB_CHANNELS; k++) {
    vb.po_mode[k] = vbPrefs.getUChar(K_PO_MODE[k], vb.po_mode[k]);
    vb.po_v[k]    = vbPrefs.getInt  (K_PO_V[k],    vb.po_v[k]);
    vb.po_i[k]    = vbPrefs.getInt  (K_PO_I[k],    vb.po_i[k]);
    vb.last_v[k]  = vbPrefs.getInt  (K_LAST_V[k],  vb.last_v[k]);
    vb.last_i[k]  = vbPrefs.getInt  (K_LAST_I[k],  vb.last_i[k]);
    vb.last_en[k] = vbPrefs.getUChar(K_LAST_EN[k], vb.last_en[k]);
    vb.vmax[k]    = vbPrefs.getInt  (K_VMAX[k],    vb.vmax[k]);
    vb.imax[k]    = vbPrefs.getInt  (K_IMAX[k],    vb.imax[k]);
    g_ovp_mV[k]   = vbPrefs.getInt  (K_OVP[k],     OVP_DEFAULT_MV);
    g_ocp_mA[k]   = vbPrefs.getInt  (K_OCP[k],     OCP_DEFAULT_MA);
    g_cc_mode[k]  = vbPrefs.getUChar(K_CCMODE[k],  CCM_LIMIT);
  }
  g_otp_c = vbPrefs.getInt("otpc", OTP_DEFAULT_C);

  for (uint8_t i = 0; i < VBSettings::MAX_SSID; i++) {
    vb.ssid[i] = vbPrefs.getString(("ssid" + String(i)).c_str(), "");
    vb.pass[i] = vbPrefs.getString(("pass" + String(i)).c_str(), "");
  }
  vbPrefs.end();
  nvs_unlock();

  // Additive migration. v1's 128-entry calmap is meaningless to a linear DAC,
  // so it is dropped — but WiFi credentials and everything else survive.
  if (schema < VB_NVS_SCHEMA) {
    nvs_lock();
    vbPrefs.begin("voltbench", false);
    vbPrefs.remove("calmap");
    vbPrefs.remove("calok");
    vbPrefs.putUInt("schema", VB_NVS_SCHEMA);
    vbPrefs.end();
    nvs_unlock();
    Dbg.printf("NVS migrated schema %u -> %u (v1 calmap dropped)\n",
               (unsigned)schema, VB_NVS_SCHEMA);
  }
}

void vb_save() {
  nvs_lock();
  vbPrefs.begin("voltbench", false);
  vbPrefs.putString("mdns",      vb.mdns);
  vbPrefs.putBool  ("req_login", vb.require_login);
  vbPrefs.putString("password",  vb.password);
  vbPrefs.putBool  ("ota_auto",  vb.ota_auto);
  vbPrefs.putBool  ("mq_en",     vb.mqtt_en);
  vbPrefs.putString("mq_host",   vb.mqtt_host);
  vbPrefs.putUShort("mq_port",   vb.mqtt_port);
  vbPrefs.putString("mq_user",   vb.mqtt_user);
  vbPrefs.putString("mq_pass",   vb.mqtt_pass);
  vbPrefs.putString("mq_topic",  vb.mqtt_topic);
  for (int k = 0; k < VB_CHANNELS; k++) {
    vbPrefs.putUChar(K_PO_MODE[k], vb.po_mode[k]);
    vbPrefs.putInt  (K_PO_V[k],    vb.po_v[k]);
    vbPrefs.putInt  (K_PO_I[k],    vb.po_i[k]);
    vbPrefs.putInt  (K_VMAX[k],    vb.vmax[k]);
    vbPrefs.putInt  (K_IMAX[k],    vb.imax[k]);
    vbPrefs.putInt  (K_OVP[k],     g_ovp_mV[k]);
    vbPrefs.putInt  (K_OCP[k],     g_ocp_mA[k]);
    vbPrefs.putUChar(K_CCMODE[k],  g_cc_mode[k]);
  }
  vbPrefs.putInt("otpc", g_otp_c);
  vbPrefs.putUInt("schema", VB_NVS_SCHEMA);
  vbPrefs.end();
  nvs_unlock();
}

void vb_save_token() {
  nvs_lock();
  vbPrefs.begin("voltbench", false);
  vbPrefs.putString("token", vb.auth_token);
  vbPrefs.end();
  nvs_unlock();
}

// -----------------------------------------------------------------------------
//   Calibration: ONE 24-byte blob per channel, not six float keys. Fewer keys
//   is fewer partition entries, and it makes the fit atomic — you can never
//   load a v0 from one calibration run and a slope from another.
// -----------------------------------------------------------------------------
void vb_cal_save() {
  nvs_lock();
  vbPrefs.begin("voltbench", false);
  for (int k = 0; k < VB_CHANNELS; k++) {
    vbPrefs.putBytes(K_CAL[k], &g_cal[k], sizeof(ChanCal));
    vbPrefs.putBool (K_CALOK[k], g_cal_valid[k]);
  }
  vbPrefs.end();
  nvs_unlock();
}

void vb_cal_load() {
  nvs_lock();
  vbPrefs.begin("voltbench", true);
  for (int k = 0; k < VB_CHANNELS; k++) {
    ChanCal tmp;
    size_t n = vbPrefs.getBytes(K_CAL[k], &tmp, sizeof(tmp));
    // Bounds-check on LOAD as well as on fit: NVS corruption is a real path to
    // a 20 V command, and it does not go through cal_fit_cv.
    if (n == sizeof(tmp) && cal_plausible(&tmp)) {
      g_cal[k]       = tmp;
      g_cal_valid[k] = vbPrefs.getBool(K_CALOK[k], false);
    } else {
      cal_defaults(&g_cal[k]);
      g_cal_valid[k] = false;
      if (n) Dbg.printf("cal ch%d in NVS is implausible — using nominal\n", k + 1);
    }
  }
  vbPrefs.end();
  nvs_unlock();
}

// -----------------------------------------------------------------------------
void vb_presets_save() {
  nvs_lock();
  vbPrefs.begin("voltbench", false);
  vbPrefs.putBytes("presets", g_presets, sizeof(g_presets));
  vbPrefs.end();
  nvs_unlock();
}

void vb_presets_load() {
  nvs_lock();
  vbPrefs.begin("voltbench", true);
  if (vbPrefs.getBytes("presets", g_presets, sizeof(g_presets)) != sizeof(g_presets))
    memset(g_presets, 0, sizeof(g_presets));
  vbPrefs.end();
  nvs_unlock();
}

void vb_preset_save(uint8_t slot, uint8_t ch) {
  if (slot >= PRESET_SLOTS || ch < 1 || ch > VB_CHANNELS) return;
  VbPreset *p = &g_presets[slot];
  p->v_mV    = g_set_mV[ch - 1];
  p->i_mA    = g_set_mA[ch - 1];
  p->cc_mode = g_cc_mode[ch - 1];
  p->used    = 1;
  if (!p->name[0]) snprintf(p->name, sizeof(p->name), "P%u", slot);
  vb_presets_save();
}

void vb_preset_load(uint8_t slot, uint8_t ch) {
  if (slot >= PRESET_SLOTS || ch < 1 || ch > VB_CHANNELS) return;
  const VbPreset *p = &g_presets[slot];
  if (!p->used) return;
  g_set_mV[ch - 1]  = p->v_mV;
  g_set_mA[ch - 1]  = p->i_mA;
  g_cc_mode[ch - 1] = p->cc_mode;
  g_rev_set++;
}

// -----------------------------------------------------------------------------
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
  if (ssid.isEmpty()) return false;
  for (uint8_t i = 0; i < VBSettings::MAX_SSID; i++)
    if (vb.ssid[i] == ssid) { vb_save_wifi_slot(i, ssid, pass); return true; }
  for (uint8_t i = 0; i < VBSettings::MAX_SSID; i++)
    if (vb.ssid[i].isEmpty()) { vb_save_wifi_slot(i, ssid, pass); return true; }
  vb_save_wifi_slot(0, ssid, pass);          // all full: evict the oldest slot
  return true;
}

void vb_forget_wifi(const String &ssid) {
  for (uint8_t i = 0; i < VBSettings::MAX_SSID; i++)
    if (vb.ssid[i] == ssid) vb_save_wifi_slot(i, "", "");
}

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

// -----------------------------------------------------------------------------
//   Deferred flush. loopTask ONLY.
// -----------------------------------------------------------------------------
void vb_nvs_flush_tick() {
  static uint32_t t_last = 0;
  uint32_t now = millis();

  if (g_nvs_dirty_settings) { g_nvs_dirty_settings = false; vb_save(); }
  if (g_nvs_dirty_cal)      { g_nvs_dirty_cal      = false; vb_cal_save(); }

  // last_* is the wear footgun: a sequencer ramp changes setpoints ~50x/s, and
  // writing each one would burn the partition in an afternoon.
  if (g_nvs_dirty_last && (now - t_last) >= 30000 && g_seqctx.state != SQ_RUNNING) {
    bool changed = false;
    for (int k = 0; k < VB_CHANNELS; k++)
      if (vb.last_v[k] != g_set_mV[k] || vb.last_i[k] != g_set_mA[k] ||
          vb.last_en[k] != g_en_cmd[k]) changed = true;
    if (changed) {
      nvs_lock();
      vbPrefs.begin("voltbench", false);
      for (int k = 0; k < VB_CHANNELS; k++) {
        vb.last_v[k]  = g_set_mV[k];
        vb.last_i[k]  = g_set_mA[k];
        vb.last_en[k] = g_en_cmd[k];
        vbPrefs.putInt  (K_LAST_V[k],  vb.last_v[k]);
        vbPrefs.putInt  (K_LAST_I[k],  vb.last_i[k]);
        vbPrefs.putUChar(K_LAST_EN[k], vb.last_en[k]);
      }
      vbPrefs.end();
      nvs_unlock();
    }
    g_nvs_dirty_last = false;
    t_last = now;
  }
}
