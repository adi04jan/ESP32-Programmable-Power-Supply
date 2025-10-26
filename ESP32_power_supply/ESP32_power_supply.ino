#include <Wire.h>
#include <SW_MCP4017.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include "HttpsOTAUpdate.h"
#include "driver/rtc_io.h"
#include "credential.h"
#include "web_page.h"
#include <ESPmDNS.h>

#define CURRENT_FIRMWARE_VERSION "0.0.1"  // Change this as needed
#define WIFI_TIMEOUT 60

#define DC_R2_REF 10000
#define DC_V_REF 1235
#define MCPWIPEROHMS 15
#define VOLTAGE_READ_PIN_VV 0
#define VOLTAGE_READ_PIN_5V 1
#define VOLTAGE_READ_PIN_3V3 3

#define VOLTAGE_ERROR_MAX 100  //in millivolts

#define ENABLE_VV_PIN 5
#define ENABLE_5V_PIN 6
#define ENABLE_3V3_PIN 7

#define MCP4017ADDRESS 0x2F

uint8_t dpMaxSteps = 128;  //remember even thought the the digital pot has 128 steps it looses one on either end (usually cant go all the way to last tick)
int maxRangeOhms = 10000;  //this is a 10K potentiometer

const uint8_t total_ssid_count = sizeof(ssids) / sizeof(ssids[0]);
bool WIFI_STATUS = false;

MCP4017 i2cDP(MCP4017ADDRESS, dpMaxSteps, maxRangeOhms);

DNSServer dnsServer;
AsyncWebServer server(80);

struct State {
  bool output1 = false;
  bool output2 = false;
  bool output3 = false;
  float voltage1 = 2.5;  // initial (slider default)
} psState;

static const char *server_certificate = "-----BEGIN CERTIFICATE-----\n"
                                        "MIIEkjCCA3qgAwIBAgIQCgFBQgAAAVOFc2oLheynCDANBgkqhkiG9w0BAQsFADA/\n"
                                        "MSQwIgYDVQQKExtEaWdpdGFsIFNpZ25hdHVyZSBUcnVzdCBDby4xFzAVBgNVBAMT\n"
                                        "DkRTVCBSb290IENBIFgzMB4XDTE2MDMxNzE2NDA0NloXDTIxMDMxNzE2NDA0Nlow\n"
                                        "SjELMAkGA1UEBhMCVVMxFjAUBgNVBAoTDUxldCdzIEVuY3J5cHQxIzAhBgNVBAMT\n"
                                        "GkxldCdzIEVuY3J5cHQgQXV0aG9yaXR5IFgzMIIBIjANBgkqhkiG9w0BAQEFAAOC\n"
                                        "AQ8AMIIBCgKCAQEAnNMM8FrlLke3cl03g7NoYzDq1zUmGSXhvb418XCSL7e4S0EF\n"
                                        "q6meNQhY7LEqxGiHC6PjdeTm86dicbp5gWAf15Gan/PQeGdxyGkOlZHP/uaZ6WA8\n"
                                        "SMx+yk13EiSdRxta67nsHjcAHJyse6cF6s5K671B5TaYucv9bTyWaN8jKkKQDIZ0\n"
                                        "Z8h/pZq4UmEUEz9l6YKHy9v6Dlb2honzhT+Xhq+w3Brvaw2VFn3EK6BlspkENnWA\n"
                                        "a6xK8xuQSXgvopZPKiAlKQTGdMDQMc2PMTiVFrqoM7hD8bEfwzB/onkxEz0tNvjj\n"
                                        "/PIzark5McWvxI0NHWQWM6r6hCm21AvA2H3DkwIDAQABo4IBfTCCAXkwEgYDVR0T\n"
                                        "AQH/BAgwBgEB/wIBADAOBgNVHQ8BAf8EBAMCAYYwfwYIKwYBBQUHAQEEczBxMDIG\n"
                                        "CCsGAQUFBzABhiZodHRwOi8vaXNyZy50cnVzdGlkLm9jc3AuaWRlbnRydXN0LmNv\n"
                                        "bTA7BggrBgEFBQcwAoYvaHR0cDovL2FwcHMuaWRlbnRydXN0LmNvbS9yb290cy9k\n"
                                        "c3Ryb290Y2F4My5wN2MwHwYDVR0jBBgwFoAUxKexpHsscfrb4UuQdf/EFWCFiRAw\n"
                                        "VAYDVR0gBE0wSzAIBgZngQwBAgEwPwYLKwYBBAGC3xMBAQEwMDAuBggrBgEFBQcC\n"
                                        "ARYiaHR0cDovL2Nwcy5yb290LXgxLmxldHNlbmNyeXB0Lm9yZzA8BgNVHR8ENTAz\n"
                                        "MDGgL6AthitodHRwOi8vY3JsLmlkZW50cnVzdC5jb20vRFNUUk9PVENBWDNDUkwu\n"
                                        "Y3JsMB0GA1UdDgQWBBSoSmpjBH3duubRObemRWXv86jsoTANBgkqhkiG9w0BAQsF\n"
                                        "AAOCAQEA3TPXEfNjWDjdGBX7CVW+dla5cEilaUcne8IkCJLxWh9KEik3JHRRHGJo\n"
                                        "uM2VcGfl96S8TihRzZvoroed6ti6WqEBmtzw3Wodatg+VyOeph4EYpr/1wXKtx8/\n"
                                        "wApIvJSwtmVi4MFU5aMqrSDE6ea73Mj2tcMyo5jMd6jmeWUHK8so/joWUoHOUgwu\n"
                                        "X4Po1QYz+3dszkDqMp4fklxBwXRsW10KXzPMTZ+sOPAveyxindmjkW8lGy+QsRlG\n"
                                        "PfZ+G6Z6h7mjem0Y+iWlkYcV4PIWL1iwBi8saCbGS5jN2p8M+X+Q7UNKEkROb3N6\n"
                                        "KOqkqm57TH2H3eDJAkSnh6/DNFu0Qg==\n"
                                        "-----END CERTIFICATE-----";

void HttpEvent(HttpEvent_t *event) {
  switch (event->event_id) {
    case HTTP_EVENT_ERROR: Serial.println("Http Event Error"); break;
    case HTTP_EVENT_ON_CONNECTED: Serial.println("Http Event On Connected"); break;
    case HTTP_EVENT_HEADER_SENT: Serial.println("Http Event Header Sent"); break;
    case HTTP_EVENT_ON_HEADER: Serial.printf("Http Event On Header, key=%s, value=%s\n", event->header_key, event->header_value); break;
    case HTTP_EVENT_ON_DATA: break;
    case HTTP_EVENT_ON_FINISH: Serial.println("Http Event On Finish"); break;
    case HTTP_EVENT_DISCONNECTED: Serial.println("Http Event Disconnected"); break;
    case HTTP_EVENT_REDIRECT: Serial.println("Http Event Redirect"); break;
  }
}


uint8_t connect_wifi() {
  if (WIFI_STATUS != false || WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi already connected");
    return 0;
  }
  Serial.println("Scanning for available WiFi networks...");

  int networks = WiFi.scanNetworks();
  if (networks == 0) {
    Serial.println("No networks found.");
    return 1;
  }

  for (int i = 0; i < networks; i++) {
    String ssid_found = WiFi.SSID(i);
    Serial.print("Found SSID: ");
    Serial.println(ssid_found);

    for (uint8_t j = 0; j < total_ssid_count; ++j) {
      if (ssid_found == ssids[j]) {
        Serial.print("Known SSID matched: ");
        Serial.println(ssids[j]);

        WiFi.begin(ssids[j], passwords[j]);

        // Try connecting
        for (uint8_t k = 0; k < WIFI_TIMEOUT; ++k) {
          if (WiFi.status() == WL_CONNECTED) {
            WIFI_STATUS = true;
            Serial.print("Connected to ");
            Serial.println(ssids[j]);
            Serial.print("IP Address: ");
            Serial.println(WiFi.localIP());
            Serial.print("RSSI: ");
            Serial.println(WiFi.RSSI());
            return 0;
          }
          delay(500);
          Serial.print(".");
        }

        Serial.println("\nFailed to connect to matched SSID.");
      }
    }
  }
  Serial.println("No known SSIDs available.");
  return 1;
}

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

String get_connected_wifi_info() {
  if (WiFi.status() == WL_CONNECTED) {
    String ssid = WiFi.SSID();
    long rssi = WiFi.RSSI();
    String wifi_info = "SSID: " + ssid + " | RSSI: " + String(rssi) + " dBm";
    return wifi_info;
  } else {
    return "Not connected to any WiFi.";
  }
}

void perform_ota() {
  static HttpsOTAStatus_t otastatus;
  static int total_ota_time = 0;
  Serial.println("Starting OTA...!!!!");

  // Step 1: Download version.txt
  HTTPClient http_data;
  String version_url = String(base_url) + "version.txt";
  http_data.begin(version_url);
  int httpCode = http_data.GET();

  if (httpCode != 200) {
    Serial.printf("Failed to fetch version.txt, HTTP code: %d\n", httpCode);
    http_data.end();
    return;
  }

  String new_version = http_data.getString();
  new_version.trim();  // Remove extra whitespace or newline
  http_data.end();

  Serial.printf("Current Version: %s | New Version: %s\n", CURRENT_FIRMWARE_VERSION, new_version.c_str());

  // Step 2: Compare with current version
  if (new_version.equals(CURRENT_FIRMWARE_VERSION)) {
    Serial.println("Already running the latest firmware.");
    return;
  }

  Serial.println("New firmware available. Starting OTA update...");

  // Step 3: Construct firmware URL
  String firmware_url = String(base_url) + "firmware_" + new_version + ".bin";
  Serial.println("Firmware URL: " + firmware_url);

  HttpsOTA.onHttpEvent(HttpEvent);
  HttpsOTA.begin(firmware_url.c_str(), server_certificate, false);

  while (true) {
    otastatus = HttpsOTA.status();
    total_ota_time++;
    if (otastatus == HTTPS_OTA_SUCCESS) {
      Serial.println("Firmware written successfully. To reboot device, call API ESP.restart() or PUSH restart button on device");
      ESP.restart();
    } else if (otastatus == HTTPS_OTA_FAIL) {
      Serial.println("Firmware Upgrade Fail");
      break;
    } else {
      Serial.println("OTA going on");
    }
    if (total_ota_time > 120) {
      total_ota_time = 0;
      Serial.println("OTA taking too long, returning...");
      return;  // Exit OTA loop after 2 minutes
    }
    delay(1000);
  }
}

void print_wakeup_reason() {
  esp_sleep_wakeup_cause_t wakeup_reason;

  wakeup_reason = esp_sleep_get_wakeup_cause();

  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0:
      Serial.println("Wakeup caused by external signal using RTC_IO");
      break;
    case ESP_SLEEP_WAKEUP_EXT1:
      Serial.println("Wakeup caused by external signal using RTC_CNTL");
      break;
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Wakeup caused by timer");
      break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      Serial.println("Wakeup caused by touchpad");
      break;
    case ESP_SLEEP_WAKEUP_ULP:
      Serial.println("Wakeup caused by ULP program");
      break;
    default:
      Serial.printf("Wakeup was not caused by deep sleep: %d\n", wakeup_reason);
      break;
  }
}

void Power_ON(int pin) {
  digitalWrite(pin, HIGH);
}

void Power_OFF(int pin) {
  digitalWrite(pin, LOW);
}

int read_any_volt(int pin, uint32_t *sample, bool *firstrun, int sample_c) {
  long sum = 0;
  for (int j = sample_c - 1; j > 0; j--) {
    sample[j] = sample[j - 1];
  }
  sample[0] = analogReadMilliVolts(pin);
  if (!*firstrun && sample[0] != 0) {
    for (int j = 1; j < sample_c; j++) {
      sample[j] = sample[0];
    }
    *firstrun = true;
  }
  for (int j = 0; j < sample_c; j++) {
    sum += sample[j];
  }
  long avg = sum / sample_c;
  return avg;
}

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

int read_5V_volt() {
  const int sample_c = 5;
  static uint32_t samples[sample_c] = { 0 };
  static bool initialized = false;
  uint32_t VV_voltage = (read_any_volt(VOLTAGE_READ_PIN_5V, samples, &initialized, sample_c) * 11);
  return VV_voltage;
}

int read_3V3_volt() {
  const int sample_c = 5;
  static uint32_t samples[sample_c] = { 0 };
  static bool initialized = false;
  uint32_t VV_voltage = (read_any_volt(VOLTAGE_READ_PIN_3V3, samples, &initialized, sample_c) * 11);
  return VV_voltage;
}

void fine_tune_volt(uint32_t target_mV) {
  const int STEP_ADJ_MAX = 10;
  int settling_delay = 30;
  if (target_mV > 10000) settling_delay = 60;

  int curr_resistance = i2cDP.calcResistance();
  uint32_t measured = read_VV_volt();

  for (int i = 0; i < STEP_ADJ_MAX; i++) {
    long diff = (long)target_mV - (long)measured;

    if (abs(diff) <= VOLTAGE_ERROR_MAX) {
      // target reached
      return;
    }

    if (diff > 0) {
      // measured < target → need higher voltage → decrease resistance
      curr_resistance = max(curr_resistance - 85, 0);
      i2cDP.setResistance(max(curr_resistance - 250, 0));
      delay(30);
      i2cDP.setResistance(curr_resistance);
    } else {
      // measured > target → need lower voltage → increase resistance
      curr_resistance = min(curr_resistance + 85, 10000);
      i2cDP.setResistance(min(curr_resistance + 250, 10000));
      delay(30);
      i2cDP.setResistance(curr_resistance);
    }

    delay(settling_delay);
    measured = read_VV_volt();
    Serial.println("Fine-tune #" + String(i + 1) + " | R=" + String(curr_resistance) + " Ω → " + String(measured) + " mV");
  }

  Serial.println("⚠️ Fine-tuning finished, voltage may not be exact.");
}

void set_voltage(uint32_t target_mV) {
  int step_jump = 0;
  uint64_t resistance_val = 0;
  //Power_OFF(ENABLE_VV_PIN);
  resistance_val = (DC_R2_REF * DC_V_REF) / (target_mV - DC_V_REF) - MCPWIPEROHMS;
  i2cDP.setResistance(resistance_val);
  delay(100);
  uint32_t measured = read_VV_volt();
  Serial.println("cali resistance is " + String(resistance_val) + " for mv " + String(target_mV) + " Real Volt " + String(measured) + "pot value " + String(i2cDP.calcResistance()));

  for (int i = 0; i < 3; i++) {
    measured = read_VV_volt();
    long diff = (long)target_mV - (long)measured;
    if (abs(diff) >= VOLTAGE_ERROR_MAX) {
      // target reached
      break;
    }
    fine_tune_volt(target_mV);
  }
  //Power_ON(ENABLE_VV_PIN);
  measured = read_VV_volt();
  Serial.println("cal resistance is " + String(resistance_val) + " for mv " + String(target_mV) + " Real Volt " + String(measured) + "pot value " + String(i2cDP.calcResistance()));
}

void setOutput(uint8_t output, bool state) {
  uint8_t pin = (output == 1) ? ENABLE_VV_PIN : (output == 2) ? ENABLE_5V_PIN
                                                              : ENABLE_3V3_PIN;
  digitalWrite(pin, state ? HIGH : LOW);
  if (output == 1) psState.output1 = state;
  if (output == 2) psState.output2 = state;
  if (output == 3) psState.output3 = state;
}

void setVoltage(float voltage) {
  uint32_t set_mv = (uint32_t)(voltage * 1000);
  set_voltage(set_mv);
  uint32_t measured = read_VV_volt();
  voltage = (float)(measured / 1000);
  psState.voltage1 = voltage;
}

void ota_task(void *pv) {
  perform_ota();  // will run synchronously inside this task
  vTaskDelete(NULL);
}

void perform_ota_tasked() {
  xTaskCreatePinnedToCore(ota_task, "otaTask", 8192, NULL, 5, NULL, 0);
}

void setup() {
  static int ret = 0;
  static int level = 0;

  pinMode(ENABLE_VV_PIN, OUTPUT);
  pinMode(ENABLE_3V3_PIN, OUTPUT);
  pinMode(ENABLE_5V_PIN, OUTPUT);
  Power_OFF(ENABLE_VV_PIN);
  Power_OFF(ENABLE_3V3_PIN);
  Power_OFF(ENABLE_5V_PIN);
  // initialize digital pin LED_BUILTIN as an output.
  Serial.begin(115200);
  analogReadResolution(12);
  Serial.flush();
  print_wakeup_reason();
  Wire.begin();
  ret = connect_wifi();
  if (ret != 0) {
    Serial.println("Failed to connect to any wifi network");
    no_network();
  }
  unsigned long start = millis();
  while (millis() - start < 5000) {
    if (WiFi.status() == WL_CONNECTED) break;
    delay(200);
  }
  Serial.println("WiFi Connected : " + get_connected_wifi_info());
  Serial.println(WiFi.localIP());
  // perform_ota_tasked(); 
  perform_ota();
  if (MDNS.begin("ESP32PS")) {
    Serial.println("MDNS responder started: http://ESP32PS.local/");
  } else {
    Serial.println("MDNS responder failed to start");
  }
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });

  server.on("/control", HTTP_POST, [](AsyncWebServerRequest *request) {
    String action, sval;
    int output = 0;
    float value = 0;
    if (request->hasParam("action", true))
      action = request->getParam("action", true)->value();
    if (request->hasParam("output", true))
      output = request->getParam("output", true)->value().toInt();
    if (request->hasParam("value", true))
      value = request->getParam("value", true)->value().toFloat();
    if (action == "toggle") setOutput(output, value == 1);
    if (action == "set_voltage" && output == 1) setVoltage(value);
    request->send(200, "text/plain", "OK");
  });

  server.begin();
  Serial.println("Server started");
  //set the resolution to 12 bits (0-4095)
}

// the loop function runs over and over again forever
void loop() {ṭ
  uint32_t currtime = millis();
  static uint32_t lasttime = 0;

  if (currtime >= lasttime + 15000) {
    lasttime = currtime;
    connect_wifi();
  }
}
