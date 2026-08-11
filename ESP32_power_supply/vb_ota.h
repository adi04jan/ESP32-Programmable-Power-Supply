// =============================================================================
//   vb_ota.h — pull-OTA from GitHub release assets.
//
//   Uses WiFiClientSecure with setInsecure() DELIBERATELY: the core's embedded
//   CA bundle symbol is not in the indexed format esp_crt_bundle_set() needs,
//   so verification fails with "No CA Chain is set". Accepted trade-off — do
//   not "fix" without a build-time bundle or firmware signing.
// =============================================================================
#pragma once
#include "vb_config.h"

extern volatile bool     ota_busy;        // one OTA operation at a time (pull or upload)
extern String            ota_latest;      // cached /api/ota/check result
extern volatile uint32_t ota_checked_at;  // millis of last successful check (0 = never)

// Fetch <base>version.txt (follows GitHub's 302). "" on any failure.
String ota_fetch_latest(const String &ota_url, bool verify_ssl);

void perform_ota(bool force, bool verify_ssl, const String &ota_url);

// 16 KB stack — TLS handshake/buffers are stack-hungry.
void perform_ota_tasked(bool force);
