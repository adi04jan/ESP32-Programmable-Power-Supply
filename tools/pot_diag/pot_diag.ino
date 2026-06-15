// =============================================================================
//   pot_diag — MCP4017 / output-1 characterization (TEMPORARY DIAGNOSTIC)
//   Goal: resolve 10K-vs-50K pot, measure Vout per wiper step, and quantify
//         the most stable achievable output near 13.5 V.
//   Standalone: no WiFi, direct I2C (bypasses SW_MCP4017 serial spam).
// =============================================================================
#include <Wire.h>

#define MCP4017ADDRESS       0x2F
#define VOLTAGE_READ_PIN_VV  0      // ADC pin for Vout feedback (47K:1K -> x48)
#define ENABLE_VV_PIN        5      // output-1 enable (HIGH = on)
#define ADC_SCALE            48

#define TARGET_MV            13500u
#define SWEEP_START_STEP     40     // start high R (low V), descend toward step 0
#define SWEEP_CEIL_MV        18000u // stop descending once Vout exceeds this

// Direct wiper write — one I2C byte, no debug prints.
static void setWiper(uint8_t step) {
  Wire.beginTransmission(MCP4017ADDRESS);
  Wire.write(step);
  Wire.endTransmission();
}

static uint32_t readVout(int n = 16) {
  uint32_t sum = 0;
  for (int i = 0; i < n; i++) { sum += analogReadMilliVolts(VOLTAGE_READ_PIN_VV); delay(2); }
  return (sum / n) * ADC_SCALE;
}

// Heavy filter: mean of 256 samples over ~512 ms (~25-30 mains cycles) so any
// 50/60 Hz hum on the high-Z divider node averages out. Reveals the *real*
// output by separating measurement noise from actual movement.
static uint32_t readVoutFiltered() {
  uint64_t sum = 0;
  for (int i = 0; i < 256; i++) { sum += analogReadMilliVolts(VOLTAGE_READ_PIN_VV); delay(2); }
  return (uint32_t)((sum / 256) * ADC_SCALE);
}

// Log the *filtered* value repeatedly; if its ptp is small while raw ptp is
// large, the noise is in the measurement and the true output is stable.
static void filteredHold(int step, int reps) {
  if (step < 0) { Serial.println("[DIAG] no step"); return; }
  setWiper((uint8_t)step);
  delay(600);
  uint32_t fmin = 0xFFFFFFFFu, fmax = 0; uint64_t fsum = 0;
  Serial.printf("=== FILT-HOLD step=%d reps=%d (each = mean of 256 over ~512ms) ===\n", step, reps);
  for (int i = 0; i < reps; i++) {
    uint32_t f = readVoutFiltered();
    if (f < fmin) fmin = f; if (f > fmax) fmax = f; fsum += f;
    Serial.printf("FILT %6u\n", f);
  }
  Serial.printf("=== FILT-STATS step=%d min=%u max=%u avg=%u ptp=%u mV ===\n",
                step, fmin, fmax, (uint32_t)(fsum / (reps ? reps : 1)), fmax - fmin);
  setWiper(127);
}

// Sweep step high->low (V low->high). Record nearest-to-TARGET step. Cap at ceiling.
static int   g_best_step = -1;
static uint32_t g_best_vout = 0;
static uint32_t g_best_err  = 0xFFFFFFFFu;

static void runSweep() {
  g_best_step = -1; g_best_err = 0xFFFFFFFFu;
  Serial.println("=== SWEEP step,vout_mV (V ascending) ===");
  for (int s = SWEEP_START_STEP; s >= 0; s--) {
    setWiper((uint8_t)s);
    delay(250);                       // settle (rising V, no load = fast)
    uint32_t v = readVout();
    Serial.printf("SWEEP %3d %6u\n", s, v);
    uint32_t err = (v > TARGET_MV) ? (v - TARGET_MV) : (TARGET_MV - v);
    if (err < g_best_err) { g_best_err = err; g_best_step = s; g_best_vout = v; }
    if (v > SWEEP_CEIL_MV) { Serial.printf("=== ceiling %u mV hit @ step %d -> stop ===\n", SWEEP_CEIL_MV, s); break; }
  }
  setWiper(127);                       // park at lowest V
  delay(400);
  Serial.printf("=== SWEEP DONE. nearest %u mV: step=%d vout=%u err=%d mV ===\n",
                TARGET_MV, g_best_step, g_best_vout, (int)g_best_err);
}

// Hold a fixed step and log Vout for ms; report min/max/avg/peak-to-peak.
static void holdAndLog(int step, uint32_t ms) {
  if (step < 0) { Serial.println("[DIAG] no step to hold"); return; }
  setWiper((uint8_t)step);
  delay(600);                          // settle from parked low V
  uint32_t vmin = 0xFFFFFFFFu, vmax = 0; uint64_t sum = 0; int n = 0;
  uint32_t t0 = millis();
  Serial.printf("=== HOLD step=%d for %u ms ===\n", step, ms);
  while (millis() - t0 < ms) {
    uint32_t v = readVout();
    if (v < vmin) vmin = v; if (v > vmax) vmax = v; sum += v; n++;
    Serial.printf("HOLD %6u\n", v);
    delay(200);
  }
  Serial.printf("=== STATS step=%d n=%d min=%u max=%u avg=%u ptp=%u mV ===\n",
                step, n, vmin, vmax, (uint32_t)(sum / (n ? n : 1)), vmax - vmin);
  setWiper(127);                       // park low again
}

void setup() {
  Serial.begin(115200);
  delay(3000);                         // let native-USB CDC enumerate on COM7
  Serial.println("\n[DIAG] pot_diag — MCP4017 / output-1 characterization");
  pinMode(ENABLE_VV_PIN, OUTPUT);
  digitalWrite(ENABLE_VV_PIN, HIGH);   // enable output 1
  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);
  Wire.begin();
  Wire.beginTransmission(MCP4017ADDRESS);
  Serial.println(Wire.endTransmission() == 0 ? "[DIAG] MCP4017 OK @0x2F" : "[DIAG] MCP4017 NOT FOUND");
  setWiper(127); delay(500);
  Serial.printf("[DIAG] parked: wiper=127 Vout=%u mV\n", readVout());
}

void loop() {
  static bool done = false;
  if (!done) {
    done = true;
    runSweep();
    holdAndLog(g_best_step, 10000);    // 10 s deviation at nearest-13.5V step
    Serial.println("[DIAG] complete. cmds: s<n> set step, h<n> hold+log, r re-sweep");
  }

  // Manual follow-up commands over serial.
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n'); line.trim();
    if (line.length() == 0) return;
    char c = line[0];
    long arg = (line.length() > 1) ? line.substring(1).toInt() : -1;
    if (c == 'r') runSweep();
    else if (c == 's' && arg >= 0 && arg <= 127) {
      setWiper((uint8_t)arg); delay(500);
      Serial.printf("[DIAG] step=%ld Vout=%u mV\n", arg, readVout());
    }
    else if (c == 'h' && arg >= 0 && arg <= 127) holdAndLog((int)arg, 10000);
    else if (c == 'f' && arg >= 0 && arg <= 127) filteredHold((int)arg, 20);
    else Serial.println("[DIAG] usage: s<0-127> | h<0-127> | f<0-127> | r");
  }
}
