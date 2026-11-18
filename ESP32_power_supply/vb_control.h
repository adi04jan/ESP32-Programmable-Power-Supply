// =============================================================================
//   vb_control.h — MCP4017 digipot, ADC sampling, the deadbeat voltage
//   controller, output switching, and the change log. This module is the SOLE
//   owner of the I2C pot and the ADC; web handlers must never touch either.
//
//   v2.1 NOTE: everything here is built on the 128-step wiper model and dies
//   with the digipot. It is replaced by a linear DAC + 2-point calibration.
// =============================================================================
#pragma once
#include "vb_config.h"
#include "vb_state.h"

// Who asked — recorded in the change log, so "who set 2.5 V?" has an answer.
enum VbSrc : uint8_t { SRC_SYS = 0, SRC_USB, SRC_TEL, SRC_HTTP, SRC_WS, SRC_MQTT };

// Raw output switch (1..3). No logging, no pending-cancel: use apply_command().
void setOutput(uint8_t output, bool state);

// Setpoint only — NEVER switches an output. False (and no change) if NaN or
// outside VB_VMIN_MV..VB_VMAX_MV. Non-blocking: the control task does the I2C.
bool setVoltage(float volts);
bool setVoltageMv(uint32_t mv);

// Single command dispatcher shared by WS / HTTP / USB+telnet console / MQTT.
//   toggle ch 0|1 · set_voltage 1 volts · all_off · cycle ch seconds(1..3600)
// Returns false on unknown action, bad channel or out-of-range value. An
// explicit toggle/all_off cancels any pending re-enable on that channel.
bool apply_command(const String &action, uint8_t ch, float val, uint8_t src = SRC_SYS);

// Boot: log the reset, and if vb.restore is set, re-apply the setpoint saved
// before the reboot and queue the saved outputs. Call after vb_load()+cal_load(),
// BEFORE creating the control task.
void out_boot();

extern volatile bool g_sweeping;          // calibration sweep in progress

// Current wiper resistance, for diagnostics/console output (NOT the load).
float dpCalcR();

// Build the step->mV calibration map by sweeping the wiper with output 1 OFF.
// Blocks ~1 minute; only ever called from the control task.
void run_cal_sweep();

void voltageControlTask(void *pvParameters);

int read_5V_volt();
int read_3V3_volt();

const char *vb_ctl_name(uint8_t state);   // idle/settling/verifying/frozen/busy
const char *vb_reset_reason();            // why the ESP32 last reset
uint8_t     vb_pending_mask();            // channels with a queued re-enable

// Change log: last VB_LOG_N changes, oldest first. Line format:
//   "<up_s>.<ms> <src> <action> ch=<n> val=<v>"
int  vb_log_count();
bool vb_log_line(int i, char *buf, size_t n);
