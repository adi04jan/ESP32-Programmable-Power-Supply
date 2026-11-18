#include "vb_hal.h"
#include "vb_debug.h"
#include <Wire.h>

// =============================================================================
//   Bus
// =============================================================================
bool hal_i2c_begin() {
  // Wire.begin(SDA, SCL, hz) must be explicit: the C3's default pins are NOT
  // 6/7, and the silent-failure mode is every device NACKing forever.
  return Wire.begin(PIN_SDA, PIN_SCL, I2C_HZ);
}

bool hal_probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool hal_i2c_recover() {
  // A slave that died mid-read can hold SDA low forever. Clock it out by hand,
  // then re-init. Bit-banging requires releasing the peripheral first.
  Wire.end();
  pinMode(PIN_SDA, INPUT_PULLUP);
  pinMode(PIN_SCL, OUTPUT);
  for (int i = 0; i < 9; i++) {
    digitalWrite(PIN_SCL, LOW);  delayMicroseconds(5);
    digitalWrite(PIN_SCL, HIGH); delayMicroseconds(5);
  }
  // STOP condition
  pinMode(PIN_SDA, OUTPUT);
  digitalWrite(PIN_SDA, LOW);  delayMicroseconds(5);
  digitalWrite(PIN_SCL, HIGH); delayMicroseconds(5);
  digitalWrite(PIN_SDA, HIGH); delayMicroseconds(5);

  bool freed = digitalRead(PIN_SDA) == HIGH;
  hal_i2c_begin();
  return freed;
}

// =============================================================================
//   MCP4728 quad DAC
//
//   Multi-write frame (datasheet §5.6.2):
//     b0: 0 1 0 0 0 DAC1 DAC0 UDAC
//     b1: VREF PD1 PD0 GX D11 D10 D9 D8
//     b2: D7..D0
//   VREF=1 selects the internal 2.048 V reference, so setpoints are independent
//   of the 3.3 V rail. GX=0 is gain 1.
// =============================================================================
bool dac_write(uint8_t chan, uint16_t code) {
  if (chan > 3) return false;
  if (code > 4095) code = 4095;

  Wire.beginTransmission(ADDR_MCP4728);
  Wire.write((uint8_t)(0x40 | ((chan & 0x03) << 1)));      // multi-write, UDAC=0
  Wire.write((uint8_t)(0x80 | ((code >> 8) & 0x0F)));      // VREF=1, PD=0, GX=0
  Wire.write((uint8_t)(code & 0xFF));
  return Wire.endTransmission() == 0;
}

bool dac_readback(uint16_t dac[4], uint16_t eep[4]) {
  // A read returns 24 bytes: per channel, 3 bytes of DAC register followed by
  // 3 bytes of EEPROM.
  uint8_t b[24];
  if (Wire.requestFrom((int)ADDR_MCP4728, 24) != 24) return false;
  for (int i = 0; i < 24; i++) b[i] = (uint8_t)Wire.read();

  for (int c = 0; c < 4; c++) {
    const uint8_t *p = &b[c * 6];
    dac[c] = (uint16_t)(((p[1] & 0x0F) << 8) | p[2]);
    eep[c] = (uint16_t)(((p[4] & 0x0F) << 8) | p[5]);
  }
  return true;
}

bool dac_write_eeprom_safe() {
  // Sequential write (0b01010xxx) starting at channel A, committing all four
  // channels to EEPROM. CV channels to 4095 = minimum volts, CC to 0.
  static const uint16_t safe[4] = { CV_CODE_SAFE, CC_CODE_SAFE, CV_CODE_SAFE, CC_CODE_SAFE };
  Wire.beginTransmission(ADDR_MCP4728);
  Wire.write((uint8_t)0x50);                    // sequential write from ch A
  for (int c = 0; c < 4; c++) {
    Wire.write((uint8_t)(0x80 | ((safe[c] >> 8) & 0x0F)));
    Wire.write((uint8_t)(safe[c] & 0xFF));
  }
  if (Wire.endTransmission() != 0) return false;
  delay(60);                                    // EEPROM write cycle, ~50 ms
  return true;
}

// =============================================================================
//   INA219
// =============================================================================
static bool ina_w16(uint8_t addr, uint8_t reg, uint16_t v) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write((uint8_t)(v >> 8));
  Wire.write((uint8_t)(v & 0xFF));
  return Wire.endTransmission() == 0;
}

static bool ina_r16(uint8_t addr, uint8_t reg, uint16_t *out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, 2) != 2)   return false;
  uint16_t hi = (uint8_t)Wire.read();
  uint16_t lo = (uint8_t)Wire.read();
  *out = (uint16_t)((hi << 8) | lo);
  return true;
}

bool ina_begin(uint8_t addr) {
  // BRNG=1 (32 V), PGA=/8 (+/-320 mV across the 0.1 Ohm shunt = +/-3.2 A of
  // headroom), 12-bit 8-sample averaging on both ADCs (~4.3 ms, which also
  // filters the 20-30 mV output ripple), continuous shunt+bus.
  const uint16_t cfg = (1u << 13) | (0x3u << 11) | (0x0Bu << 7) | (0x0Bu << 3) | 0x07u;
  if (!ina_w16(addr, 0x00, cfg)) return false;
  // Cal for 0.1 Ohm at 100 uA/LSB: 0.04096 / (100e-6 * 0.1) = 4096 = 0x1000.
  return ina_w16(addr, 0x05, INA_CAL_REG);
}

bool ina_read(uint8_t addr, int32_t *mV, int32_t *uA) {
  uint16_t bus, cur;
  if (!ina_r16(addr, 0x02, &bus)) return false;
  if (!ina_r16(addr, 0x04, &cur)) return false;

  *mV = (int32_t)(bus >> 3) * INA_V_LSB_MV;          // low 3 bits are flags
  *uA = (int32_t)(int16_t)cur * INA_I_LSB_UA;        // current reg is signed
  if (*uA < 0) *uA = 0;                              // reverse current is not a thing here
  return true;
}

// =============================================================================
//   MCP9700 — analog, on ADC1_CH4. Chosen over an I2C part partly on cost and
//   partly because it cannot wedge the bus the OLED and DAC share.
// =============================================================================
int32_t temp_read_c10() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_TEMP);
  float mv = (float)sum / 8.0f;
  return (int32_t)lrintf((mv - TEMP_MV_AT_0C) / TEMP_MV_PER_C * 10.0f);
}

// =============================================================================
//   GPIO
// =============================================================================
void hal_gpio_init() {
  // EN and CAL low FIRST, before anything else can go wrong. Each has a 10k
  // hardware pull-down too, so even this code failing leaves the outputs dead.
  pinMode(PIN_EN1,  OUTPUT); digitalWrite(PIN_EN1,  LOW);
  pinMode(PIN_EN2,  OUTPUT); digitalWrite(PIN_EN2,  LOW);
  pinMode(PIN_CAL1, OUTPUT); digitalWrite(PIN_CAL1, LOW);
  pinMode(PIN_CAL2, OUTPUT); digitalWrite(PIN_CAL2, LOW);

  pinMode(PIN_BTN_SEL,  INPUT_PULLUP);
  pinMode(PIN_BTN_UP,   INPUT_PULLUP);
  pinMode(PIN_BTN_DOWN, INPUT_PULLUP);

  // GPIO8 is a strapping pin: driving it is only safe once boot is done.
  pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, HIGH);   // active-low on most C3 minis

  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);
}

void hal_set_en(uint8_t ch, bool on) {
  digitalWrite(ch == 1 ? PIN_EN1 : PIN_EN2, on ? HIGH : LOW);
}

void hal_set_cal_load(uint8_t ch, bool on) {
  digitalWrite(ch == 1 ? PIN_CAL1 : PIN_CAL2, on ? HIGH : LOW);
}

void hal_led(bool on) { digitalWrite(PIN_LED, on ? LOW : HIGH); }
