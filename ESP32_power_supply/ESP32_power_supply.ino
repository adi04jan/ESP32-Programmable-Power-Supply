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

volatile uint32_t g_target_mV    = 2500;
volatile uint32_t g_setpoint_mV  = 2500;
volatile uint32_t g_measured_mV  = 0;
volatile bool     g_ctrl_output1 = false;

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
  if (WiFi.status() == WL_CONNECTED) {
    WIFI_STATUS = true;
    return 0;
  }
  WIFI_STATUS = false;  // clear stale flag — WiFi is not actually connected
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
  const int sample_c = 5;
  static uint32_t samples[sample_c] = { 0 };
  static bool initialized = false;
  uint32_t VV_voltage = (read_any_volt(VOLTAGE_READ_PIN_VV, samples, &initialized, sample_c) * 11);
  return VV_voltage;
}

int read_5V_volt() {
  const int sample_c = 5;
  static uint32_t samples[sample_c] = { 0 };
  static bool initialized = false;
  uint32_t VV_voltage = (read_any_volt(VOLTAGE_READ_PIN_5V, samples, &initialized, sample_c) * 2);
  return VV_voltage;
}

int read_3V3_volt() {
  const int sample_c = 5;
  static uint32_t samples[sample_c] = { 0 };
  static bool initialized = false;
  uint32_t VV_voltage = (read_any_volt(VOLTAGE_READ_PIN_3V3, samples, &initialized, sample_c) * 2);
  return VV_voltage;
}

void setOutput(uint8_t output, bool state) {
  uint8_t pin = (output == 1) ? ENABLE_VV_PIN : (output == 2) ? ENABLE_5V_PIN
                                                              : ENABLE_3V3_PIN;
  digitalWrite(pin, state ? HIGH : LOW);
  if (output == 1) { psState.output1 = state; g_ctrl_output1 = state; }
  if (output == 2) psState.output2 = state;
  if (output == 3) psState.output3 = state;
}

void voltageControlTask(void* pvParameters) {
  const int      FINE_TUNE_R2_MIN   = 3000;  // below this ohms, each step > tolerance
  const int      FINE_TUNE_MAX_ITER = 3;
  const uint32_t VOLTAGE_TOL_MV    = 100;
  const uint32_t MONITOR_MS        = 200;

  uint32_t lastTarget = g_target_mV;

  for (;;) {
    uint32_t target = g_target_mV;

    if (target != lastTarget) {
      lastTarget = target;

      // Formula-based initial resistance set
      if (target > (uint32_t)DC_V_REF) {
        long r_calc = (long)((DC_R2_REF * DC_V_REF) / (target - DC_V_REF)) - MCPWIPEROHMS;
        uint32_t r_val = (r_calc > 0) ? (uint32_t)r_calc : 0;
        i2cDP.setResistance(r_val);
        Serial.printf("[VCtrl] target=%umV R=%u\n", target, r_val);
      }
      vTaskDelay(pdMS_TO_TICKS(150));

      // Sample ADC 3x with gaps
      uint32_t sum = 0;
      for (int s = 0; s < 3; s++) {
        sum += analogReadMilliVolts(VOLTAGE_READ_PIN_VV);
        if (s < 2) vTaskDelay(pdMS_TO_TICKS(10));
      }
      g_measured_mV = (sum / 3) * 11;

      // Fine-tune only in low-voltage range where pot resolution allows it
      if (g_ctrl_output1) {
        int r_now = (int)i2cDP.calcResistance();
        for (int i = 0; i < FINE_TUNE_MAX_ITER; i++) {
          if (g_target_mV != lastTarget) break;   // new target arrived
          if (r_now < FINE_TUNE_R2_MIN) break;     // step too coarse at high V

          sum = 0;
          for (int s = 0; s < 3; s++) {
            sum += analogReadMilliVolts(VOLTAGE_READ_PIN_VV);
            if (s < 2) vTaskDelay(pdMS_TO_TICKS(10));
          }
          uint32_t measured = (sum / 3) * 11;
          g_measured_mV = measured;

          long diff = (long)target - (long)measured;
          if (abs(diff) <= (long)VOLTAGE_TOL_MV) break;

          if (diff > 0) r_now = max(r_now - 79, 0);
          else          r_now = min(r_now + 79, DC_R2_REF);
          i2cDP.setResistance((uint32_t)r_now);
          Serial.printf("[VCtrl] fine-tune #%d R=%d meas=%umV\n", i + 1, r_now, measured);
          vTaskDelay(pdMS_TO_TICKS(80));
        }
      }
    } else {
      // Idle — keep measured voltage fresh for browser
      vTaskDelay(pdMS_TO_TICKS(MONITOR_MS));
      uint32_t sum = 0;
      for (int s = 0; s < 3; s++) {
        sum += analogReadMilliVolts(VOLTAGE_READ_PIN_VV);
        if (s < 2) vTaskDelay(pdMS_TO_TICKS(10));
      }
      g_measured_mV = (sum / 3) * 11;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
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
  Wire.beginTransmission(MCP4017ADDRESS);
  if (Wire.endTransmission() == 0) {
    Serial.println("MCP4017 found at 0x2F");
  } else {
    Serial.println("WARNING: MCP4017 not found at 0x2F — check I2C wiring!");
  }
  xTaskCreatePinnedToCore(voltageControlTask, "VoltCtrl", 4096, nullptr, 2, nullptr, 0);
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
    if (action == "set_voltage" && output == 1) {
      uint32_t mv = (uint32_t)(value * 1000.0f);
      g_target_mV   = mv;
      g_setpoint_mV = mv;
      psState.voltage1 = value;
    }
    request->send(200, "text/plain", "OK");
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    // Prepare JSON payload with live output states and voltages
    // For voltages: use latest readings; for outputs: use psState
    String json = "{";
    json += "\"output1\":" + String(psState.output1 ? "true" : "false") + ",";
    json += "\"output2\":" + String(psState.output2 ? "true" : "false") + ",";
    json += "\"output3\":" + String(psState.output3 ? "true" : "false") + ",";
    json += "\"voltage1\":" + String(g_measured_mV / 1000.0f, 2) + ",";
    json += "\"setpoint1\":" + String(g_setpoint_mV / 1000.0f, 2) + ",";
    json += "\"voltage2\":" + String(read_5V_volt() / 1000.0, 2) + ",";
    json += "\"voltage3\":" + String(read_3V3_volt() / 1000.0, 2);
    json += "}";
    request->send(200, "application/json", json);
  });

  server.begin();
  Serial.println("Server started");
  //set the resolution to 12 bits (0-4095)
}

// the loop function runs over and over again forever
void loop() {
  uint32_t currtime = millis();
  static uint32_t lasttime = 0;

  if (currtime >= lasttime + 15000) {
    lasttime = currtime;
    connect_wifi();
  }
}
