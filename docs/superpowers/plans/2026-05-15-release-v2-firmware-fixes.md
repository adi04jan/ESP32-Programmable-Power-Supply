# release_v2 Firmware Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix seven bugs identified in the v1.0 firmware code review, improving voltage control reliability, WiFi reconnection, OTA security, and stability.

**Architecture:** All changes are in the Arduino sketch (`ESP32_power_supply.ino`) and the credentials header (`credential.h`). No new files or libraries are needed. The MCP4017 range stays at 10K (confirmed as the actual hardware used). Because this is embedded firmware with no unit test framework, each fix is verified by a compile check and the expected Serial monitor output noted so it can be confirmed on hardware.

**Tech Stack:** Arduino framework for ESP32-C3, ESPAsyncWebServer, SW_MCP4017, HttpsOTA, FreeRTOS

---

## Files Modified

| File | Changes |
|------|---------|
| `ESP32_power_supply/ESP32_power_supply.ino` | Fix 6 bugs (set_voltage logic, WIFI_STATUS reset, race condition, no_network counter, VV averaging, resistance underflow) |
| `ESP32_power_supply/credential.h` | Fix OTA URLs to HTTPS + update SSL certificate placeholder |

---

### Task 1: Fix inverted condition in `set_voltage()`

The loop at lines 359–367 currently **breaks when the error is large** and fine-tunes when the error is already small — completely backwards.

**Files:**
- Modify: `ESP32_power_supply/ESP32_power_supply.ino` (lines 359–367)

- [ ] **Step 1: Locate the broken block**

Open `ESP32_power_supply.ino`. Find this block (around line 359):

```cpp
for (int i = 0; i < 3; i++) {
  measured = read_VV_volt();
  long diff = (long)target_mV - (long)measured;
  if (abs(diff) >= VOLTAGE_ERROR_MAX) {
    // target reached
    break;
  }
  fine_tune_volt(target_mV);
}
```

- [ ] **Step 2: Replace with corrected logic**

The condition must be inverted: break when already within tolerance, fine-tune when outside it.

```cpp
for (int i = 0; i < 3; i++) {
  measured = read_VV_volt();
  long diff = (long)target_mV - (long)measured;
  if (abs(diff) < VOLTAGE_ERROR_MAX) {
    break;  // already within tolerance
  }
  fine_tune_volt(target_mV);
}
```

- [ ] **Step 3: Verify compile**

In Arduino IDE: Sketch → Verify/Compile. Expected: no errors.

- [ ] **Step 4: Commit**

```bash
git add ESP32_power_supply/ESP32_power_supply.ino
git commit -m "fix: correct inverted voltage tolerance check in set_voltage"
```

---

### Task 2: Fix `WIFI_STATUS` never reset — reconnect after drop

`WIFI_STATUS` is only ever set to `true`, so `connect_wifi()` always exits early after first connect. After a drop, the device never retries.

**Files:**
- Modify: `ESP32_power_supply/ESP32_power_supply.ino` (`connect_wifi()` function)

- [ ] **Step 1: Locate the early-exit guard in `connect_wifi()`**

Find (around line 94):

```cpp
uint8_t connect_wifi() {
  if (WIFI_STATUS != false || WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi already connected");
    return 0;
  }
```

- [ ] **Step 2: Replace the guard to check actual WiFi state**

The guard should only short-circuit if WiFi is **actually** still connected. When it's not, clear the flag so a real reconnect attempt runs.

```cpp
uint8_t connect_wifi() {
  if (WiFi.status() == WL_CONNECTED) {
    WIFI_STATUS = true;
    return 0;
  }
  WIFI_STATUS = false;  // clear stale flag — WiFi is not actually connected
```

- [ ] **Step 3: Verify compile**

Arduino IDE: Sketch → Verify/Compile. Expected: no errors.

- [ ] **Step 4: Serial monitor expectation**

On hardware, after unplugging/reconnecting the router, the Serial monitor should print the scan and reconnect sequence within ~15 seconds (the `loop()` poll interval).

- [ ] **Step 5: Commit**

```bash
git add ESP32_power_supply/ESP32_power_supply.ino
git commit -m "fix: reset WIFI_STATUS on disconnect so reconnect retries actually run"
```

---

### Task 3: Fix race condition — remove `control_voltage` global

`control_voltage` is a global float written and read inside an async POST handler running in a separate FreeRTOS task. Making it local to the handler eliminates the race.

**Files:**
- Modify: `ESP32_power_supply/ESP32_power_supply.ino` (global declaration + `/control` handler)

- [ ] **Step 1: Remove the global declaration**

Find and delete this line near the top of the file (around line 51):

```cpp
float control_voltage = 0;
```

- [ ] **Step 2: Declare it locally inside the `/control` handler**

Find the handler (around line 435):

```cpp
server.on("/control", HTTP_POST, [](AsyncWebServerRequest *request) {
  String action, sval;
  int output = 0;
  if (request->hasParam("action", true))
    action = request->getParam("action", true)->value();
  if (request->hasParam("output", true))
    output = request->getParam("output", true)->value().toInt();
  if (request->hasParam("value", true))
    control_voltage = request->getParam("value", true)->value().toFloat();
  if (action == "toggle") setOutput(output, control_voltage == 1);
  if (action == "set_voltage" && output == 1) setVoltage(control_voltage);
  request->send(200, "text/plain", "OK");
});
```

Replace with:

```cpp
server.on("/control", HTTP_POST, [](AsyncWebServerRequest *request) {
  String action;
  int output = 0;
  float value = 0.0f;
  if (request->hasParam("action", true))
    action = request->getParam("action", true)->value();
  if (request->hasParam("output", true))
    output = request->getParam("output", true)->value().toInt();
  if (request->hasParam("value", true))
    value = request->getParam("value", true)->value().toFloat();
  if (action == "toggle") setOutput(output, value == 1.0f);
  if (action == "set_voltage" && output == 1) setVoltage(value);
  request->send(200, "text/plain", "OK");
});
```

- [ ] **Step 3: Verify compile**

Arduino IDE: Sketch → Verify/Compile. Expected: no errors. If `control_voltage` was referenced anywhere else, the compiler will flag it — check and remove any remaining references.

- [ ] **Step 4: Commit**

```bash
git add ESP32_power_supply/ESP32_power_supply.ino
git commit -m "fix: remove control_voltage global to eliminate async task race condition"
```

---

### Task 4: Fix `no_network()` static counter — restarts immediately on second call

The `static int count` accumulates across calls. On a second invocation, `count` is already ≥10 and `ESP.restart()` fires immediately.

**Files:**
- Modify: `ESP32_power_supply/ESP32_power_supply.ino` (`no_network()` function)

- [ ] **Step 1: Locate the function**

Find (around line 143):

```cpp
void no_network() {
  static int ret = 0;
  static int count = 0;
  Serial.println("No WiFi network found, entering continous scan");
  while (count++ < 10) {
    ret = connect_wifi();
    if (ret == 0) {
      Serial.println("Connected to WiFi network");
      return;
    }
    Serial.println("Failed to connect to any wifi network");
  }
  ESP.restart();
}
```

- [ ] **Step 2: Convert static variables to local**

```cpp
void no_network() {
  int ret = 0;
  int count = 0;
  Serial.println("No WiFi network found, entering continuous scan");
  while (count++ < 10) {
    ret = connect_wifi();
    if (ret == 0) {
      Serial.println("Connected to WiFi network");
      return;
    }
    Serial.println("Failed to connect to any wifi network");
  }
  ESP.restart();
}
```

- [ ] **Step 3: Verify compile**

Arduino IDE: Sketch → Verify/Compile. Expected: no errors.

- [ ] **Step 4: Commit**

```bash
git add ESP32_power_supply/ESP32_power_supply.ino
git commit -m "fix: no_network counter is now local so repeated calls retry properly"
```

---

### Task 5: Re-enable VV ADC averaging

The variable voltage channel (`read_VV_volt`) uses a raw single ADC read while the 5V and 3V3 channels use a 5-sample moving average. The noisy single read destabilises the `fine_tune_volt()` control loop.

**Files:**
- Modify: `ESP32_power_supply/ESP32_power_supply.ino` (`read_VV_volt()` function)

- [ ] **Step 1: Locate the function**

Find (around line 282):

```cpp
int read_VV_volt() {
  const int sample_c = 2;
  static uint32_t samples[sample_c] = { 0 };
  static bool initialized = false;
  uint32_t VV_voltage = 0;
  // for (int i = 0; i < sample_c; i++) {
  //   VV_voltage = (read_any_volt(VOLTAGE_READ_PIN_VV, samples, &initialized, sample_c) * 11);
  //   delay(25);
  // }
  VV_voltage = analogReadMilliVolts(VOLTAGE_READ_PIN_VV) * 11;
  return VV_voltage;
}
```

- [ ] **Step 2: Replace with proper averaging — same pattern as 5V/3V3 channels**

Increase sample count to 5 (matches the other channels) and use `read_any_volt()`:

```cpp
int read_VV_volt() {
  const int sample_c = 5;
  static uint32_t samples[sample_c] = { 0 };
  static bool initialized = false;
  uint32_t VV_voltage = (read_any_volt(VOLTAGE_READ_PIN_VV, samples, &initialized, sample_c) * 11);
  return VV_voltage;
}
```

- [ ] **Step 3: Verify compile**

Arduino IDE: Sketch → Verify/Compile. Expected: no errors.

- [ ] **Step 4: Serial monitor expectation**

On hardware, voltage readings for Output 1 on the Serial monitor should be smoother/less jittery. The fine-tune loop should converge in fewer iterations.

- [ ] **Step 5: Commit**

```bash
git add ESP32_power_supply/ESP32_power_supply.ino
git commit -m "fix: re-enable 5-sample moving average for VV ADC channel"
```

---

### Task 6: Fix `resistance_val` unsigned underflow in `set_voltage()`

If the formula result is less than `MCPWIPEROHMS` (15Ω), subtracting it from a `uint64_t` wraps to a huge value.

**Files:**
- Modify: `ESP32_power_supply/ESP32_power_supply.ino` (`set_voltage()` function)

- [ ] **Step 1: Locate the calculation**

Find (around line 351):

```cpp
void set_voltage(uint32_t target_mV) {
  int step_jump = 0;
  uint64_t resistance_val = 0;
  //Power_OFF(ENABLE_VV_PIN);
  resistance_val = (DC_R2_REF * DC_V_REF) / (target_mV - DC_V_REF) - MCPWIPEROHMS;
```

- [ ] **Step 2: Use a signed intermediate and clamp to zero**

```cpp
void set_voltage(uint32_t target_mV) {
  //Power_OFF(ENABLE_VV_PIN);
  long resistance_calc = (long)((DC_R2_REF * DC_V_REF) / (target_mV - DC_V_REF)) - MCPWIPEROHMS;
  uint64_t resistance_val = (resistance_calc > 0) ? (uint64_t)resistance_calc : 0;
```

Also remove the now-unused `int step_jump = 0;` line immediately below.

- [ ] **Step 3: Verify compile**

Arduino IDE: Sketch → Verify/Compile. Expected: no errors.

- [ ] **Step 4: Commit**

```bash
git add ESP32_power_supply/ESP32_power_supply.ino
git commit -m "fix: use signed intermediate for resistance calc to prevent uint underflow"
```

---

### Task 7: Fix OTA — update SSL certificate and enforce HTTPS URLs

The hardcoded certificate (DST Root CA X3) expired March 2021. The `credential.h` placeholder uses `http://` which is insecure and incompatible with `HttpsOTA`.

**Files:**
- Modify: `ESP32_power_supply/ESP32_power_supply.ino` (replace `server_certificate`)
- Modify: `ESP32_power_supply/credential.h` (update URL scheme and add comment)

- [ ] **Step 1: Replace the expired certificate with ISRG Root X1**

In `ESP32_power_supply.ino`, find the `server_certificate` block (lines 52–78) and replace it entirely:

```cpp
static const char *server_certificate = "-----BEGIN CERTIFICATE-----\n"
                                        "MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
                                        "TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
                                        "cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
                                        "WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
                                        "ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
                                        "MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoBggIBAK3oJHP0FDfzm54rVygc\n"
                                        "h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
                                        "0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
                                        "A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
                                        "T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
                                        "B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
                                        "B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
                                        "KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
                                        "OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
                                        "jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
                                        "qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
                                        "rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
                                        "HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
                                        "hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
                                        "ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
                                        "3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
                                        "NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
                                        "ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
                                        "TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
                                        "jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
                                        "oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
                                        "4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
                                        "mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
                                        "emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
                                        "-----END CERTIFICATE-----";
```

This is the **ISRG Root X1** certificate, valid until 2035-06-04. It is the trust anchor for all modern Let's Encrypt certificates.

- [ ] **Step 2: Update `credential.h` to use HTTPS**

The OTA URLs must use `https://` for `HttpsOTA` to work:

```cpp

static const char *base_url = "https://your-server.example.com/ota/";  // MUST be HTTPS for HttpsOTA

static const char *ssids[] = {
    "wifi_ssid1",
    "wifi_ssid2"
};

static const char *passwords[] = {
    "wifi_password1",
    "wifi_password2"
};
```

(Remove the now-unused `url` variable — only `base_url` is referenced by the firmware.)

- [ ] **Step 3: Verify compile**

Arduino IDE: Sketch → Verify/Compile. Expected: no errors.

- [ ] **Step 4: Commit**

```bash
git add ESP32_power_supply/ESP32_power_supply.ino ESP32_power_supply/credential.h
git commit -m "fix: replace expired DST Root CA X3 with ISRG Root X1, enforce HTTPS OTA URLs"
```

---

### Task 8: Final — push branch

- [ ] **Step 1: Confirm all changes are committed**

```bash
git log --oneline release_v2
```

Expected output (7 commits above the base):
```
fix: replace expired DST Root CA X3 with ISRG Root X1, enforce HTTPS OTA URLs
fix: use signed intermediate for resistance calc to prevent uint underflow
fix: re-enable 5-sample moving average for VV ADC channel
fix: no_network counter is now local so repeated calls retry properly
fix: remove control_voltage global to eliminate async task race condition
fix: reset WIFI_STATUS on disconnect so reconnect retries actually run
fix: correct inverted voltage tolerance check in set_voltage
```

- [ ] **Step 2: Push the branch**

```bash
git push -u origin release_v2
```

---

## Quick Reference — What Was NOT Changed

| Item | Reason left as-is |
|------|-------------------|
| `maxRangeOhms = 10000` | Confirmed: actual hardware uses a 10K pot |
| `DC_V_REF 1235` | Within LM2596 spec tolerance (1.23V typ), not causing measurable error |
| Web UI HTML/JS | No issues found in UI code |
| MDNS / server routes | No issues found |
