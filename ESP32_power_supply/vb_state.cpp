#include "vb_state.h"

State psState;
AsyncWebServer server(80);

volatile uint32_t g_target_mV   = 2500;
volatile uint32_t g_setpoint_mV = 2500;
volatile uint32_t g_measured_mV = 0;
volatile uint8_t  g_vctrl_state = 0;
volatile uint32_t g_5v_mV       = 0;
volatile uint32_t g_3v3_mV      = 0;

uint16_t          g_cal_mv[128]   = {0};
volatile bool     g_cal_valid     = false;
volatile bool     g_cal_sweep_req = false;
volatile uint32_t g_display_mV    = 0;
