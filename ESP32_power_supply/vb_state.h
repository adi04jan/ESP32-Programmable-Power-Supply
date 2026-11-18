// =============================================================================
//   vb_state.h — state shared between the control task and the web/console
//   layers. Definitions live in vb_state.cpp.
//
//   Concurrency contract (unchanged from v1): 32-bit aligned reads/writes are
//   atomic on ESP32-C3 RISC-V, so these volatiles need no lock. The control
//   task is the sole WRITER of the measurement globals; web handlers only read
//   them, and only ever write g_target_mV.
// =============================================================================
#pragma once
#include "vb_config.h"
#include <ESPAsyncWebServer.h>

struct State {
  bool  output1  = false;
  bool  output2  = false;
  bool  output3  = false;
  float voltage1 = 2.5f;   // commanded set-point (V)
};
extern State psState;

extern AsyncWebServer server;

// Written by web/console, read by the control task.
extern volatile uint32_t g_target_mV;
// Written by the control task, read by everyone else.
extern volatile uint32_t g_setpoint_mV;
extern volatile uint32_t g_measured_mV;
extern volatile uint8_t  g_vctrl_state;   // 0=idle 1=settling 2=verifying 3=frozen
extern volatile uint32_t g_5v_mV;         // rails sampled by the control task so
extern volatile uint32_t g_3v3_mV;        // web handlers never touch the ADC

// Output-1 calibration map: measured mV at each wiper step (0 = uncalibrated).
extern uint16_t          g_cal_mv[128];
extern volatile bool     g_cal_valid;
extern volatile uint32_t g_display_mV;     // median-of-5 reading for the UI
