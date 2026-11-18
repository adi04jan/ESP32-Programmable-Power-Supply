// =============================================================================
//   vb_hal.h — MCP4728 quad DAC, INA219 x2, MCP9700 temp, EN/CAL GPIO.
//
//   EVERY FUNCTION HERE IS CTRL-TASK-ONLY. ctrl is the sole owner of the I2C
//   bus (DAC, both INA219s, and the display), which is what lets the whole
//   firmware run without a single I2C mutex. An I2C write from an AsyncTCP
//   callback will race the control task — this is the same rule that governed
//   v1, and it is why apply_command queues anything needing the bus.
//
//   Drivers are hand-written rather than pulled from Adafruit: the MCP4728 is
//   a 3-byte write and the INA219 is three registers, so a library would be
//   more code than this, plus a dependency that must track core versions.
// =============================================================================
#pragma once
#include "vb_config.h"

// ---- bus --------------------------------------------------------------------
bool hal_i2c_begin();
bool hal_probe(uint8_t addr);
// 9-clock recovery for a slave holding SDA low. Returns true if the bus freed.
bool hal_i2c_recover();

// ---- MCP4728 ----------------------------------------------------------------
// Single-channel write, internal 2.048 V reference, gain 1. Single-channel form
// is used deliberately: LDAC is tied low on the common breakouts, so a
// multi-channel write glitches every output as it shifts through.
bool dac_write(uint8_t chan, uint16_t code);

// Read all four DAC registers AND all four EEPROM values in one 24-byte burst.
// The boot sequence calls this TWICE — before and after writing setpoints —
// because "the DAC was probably written" and "the DAC was written" are
// different claims, and only one of them is safe to enable an output on.
bool dac_readback(uint16_t dac[4], uint16_t eep[4]);

// Persist the current registers to EEPROM so a power cycle comes up safe
// (CV=4095 minimum volts, CC=0) before the ESP32 has even booted. ~50 ms.
bool dac_write_eeprom_safe();

// ---- INA219 -----------------------------------------------------------------
bool ina_begin(uint8_t addr);
// Bus voltage in mV and current in uA. Returns false on NACK — three
// consecutive failures latch a SENSE trip, because OVP and OCP both depend on
// this reading and a supply that cannot measure itself must not stay enabled.
bool ina_read(uint8_t addr, int32_t *mV, int32_t *uA);

// ---- MCP9700 ----------------------------------------------------------------
// Tenths of a degree C. 500 mV at 0 C, 10 mV/C.
int32_t temp_read_c10();

// ---- GPIO -------------------------------------------------------------------
// The invariant behind every failure mode: output enable is a plain GPIO with a
// hardware pull-down, so killing an output never depends on a working bus,
// display, or sensor.
void hal_gpio_init();
void hal_set_en(uint8_t ch, bool on);
void hal_set_cal_load(uint8_t ch, bool on);
void hal_led(bool on);
