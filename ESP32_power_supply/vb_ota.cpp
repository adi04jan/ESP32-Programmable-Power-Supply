#include "vb_ota.h"
#include "vb_debug.h"
#include "vb_nvs.h"          // vb.ota_ssl / vb.ota_url for the tasked wrapper
#include "vb_proto.h"        // vp_semver_newer
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>

volatile bool     ota_busy       = false;   // one OTA operation at a time (pull or upload)
String            ota_latest     = "";      // cached /api/ota/check result
volatile uint32_t ota_checked_at = 0;       // millis of last successful check (0 = never)

// Fetch <base>version.txt (follows GitHub's 302). "" on any failure.
String ota_fetch_latest(const String &ota_url, bool verify_ssl) {
  bool   https       = ota_url.startsWith("https");
  String version_url = ota_url + "version.txt";
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  bool begun;
  WiFiClientSecure sec; WiFiClient plain;
  if (https) { if (!verify_ssl) sec.setInsecure(); begun = http.begin(sec, version_url); }
  else       { begun = http.begin(plain, version_url); }
  if (!begun) { Dbg.println("OTA: version begin failed"); return ""; }
  int code = http.GET();
  if (code != 200) { http.end(); Dbg.printf("OTA: version.txt HTTP %d\n", code); return ""; }
  String latest = http.getString(); latest.trim(); http.end();
  return latest;
}

void perform_ota(bool force, bool verify_ssl, const String &ota_url) {
  bool https = ota_url.startsWith("https");

  String latest = ota_fetch_latest(ota_url, verify_ssl);

  Dbg.printf("OTA: current=%s latest=%s\n", CURRENT_FIRMWARE_VERSION, latest.c_str());
  if (latest.isEmpty()) { Dbg.println("OTA: empty version, abort"); return; }
  // Semver-NEWER only. The old `!=` test treated an OLDER release as an update,
  // so a device running ahead of GitHub's latest downgraded itself on boot.
  if (!force && !vp_semver_newer(latest.c_str(), CURRENT_FIRMWARE_VERSION)) {
    Dbg.println("OTA: no newer release"); return;
  }

  // --- download + flash firmware_<latest>.bin ---
  String firmware_url = ota_url + "firmware_" + latest + ".bin";
  Dbg.printf("OTA: downloading %s\n", firmware_url.c_str());
  httpUpdate.rebootOnUpdate(true);
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  t_httpUpdate_return ret;
  if (https) {
    WiFiClientSecure sec; if (!verify_ssl) sec.setInsecure();
    ret = httpUpdate.update(sec, firmware_url);
  } else {
    WiFiClient plain;
    ret = httpUpdate.update(plain, firmware_url);
  }
  if (ret == HTTP_UPDATE_FAILED)
    Dbg.printf("OTA: failed (%d) %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
  else if (ret == HTTP_UPDATE_NO_UPDATES)
    Dbg.println("OTA: server reports no update");
}

static bool ota_force_flag = false;
static void ota_task_fn(void *pv) {
  perform_ota(ota_force_flag, vb.ota_ssl, vb.ota_url);
  ota_force_flag = false;
  ota_busy = false;
  vTaskDelete(NULL);
}
// 16 KB stack — TLS (WiFiClientSecure) handshake/buffers are stack-hungry.
void perform_ota_tasked(bool force) {
  if (ota_busy) { Dbg.println("OTA: busy — request ignored"); return; }
  ota_busy = true;
  ota_force_flag = force;
  xTaskCreatePinnedToCore(ota_task_fn, "otaTask", 16384, NULL, 1, NULL, 0);   // background work: lowest prio
}
