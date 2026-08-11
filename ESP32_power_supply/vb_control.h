// =============================================================================
//   vb_control.h — MCP4017 digipot, ADC sampling, and the deadbeat voltage
//   controller. This module is the SOLE owner of the I2C pot and the ADC;
//   web handlers must never touch either.
//
//   v2.1 NOTE: everything here is built on the 128-step wiper model and dies
//   with the digipot. It is replaced by a linear DAC + 2-point calibration.
// =============================================================================
#pragma once
#include "vb_config.h"
#include "vb_state.h"

// Output enable (1..3). Single bounds guard for all callers.
void setOutput(uint8_t output, bool state);

// Non-blocking: writes globals — voltageControlTask does the actual I2C work.
void setVoltage(float voltage);

// Single command dispatcher shared by WS / HTTP / USB+telnet console / MQTT.
bool apply_command(const String &action, uint8_t ch, float val);

// Current wiper resistance, for diagnostics/console output.
float dpCalcR();

// Build the step->mV calibration map by sweeping the wiper. Blocks ~1 minute;
// only ever called from the control task.
void run_cal_sweep();

void voltageControlTask(void *pvParameters);

int read_5V_volt();
int read_3V3_volt();
