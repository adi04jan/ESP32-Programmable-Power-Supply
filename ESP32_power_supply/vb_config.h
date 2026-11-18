// =============================================================================
//   vb_config.h — build-wide constants: version, pins, I2C addresses, limits.
//
//   Included FIRST by every translation unit. It carries the size-optimisation
//   pragma, which is PER-TU: a .cpp that forgets to include this compiles at -Og
//   and the sketch grows ~60 KB. Include it first, always.
//
//   NOTE for .cpp files in this sketch folder: unlike .ino files, they do NOT
//   get an implicit `#include <Arduino.h>`. This header provides it.
// =============================================================================
#pragma once
#pragma GCC optimize("Os")

#include <Arduino.h>

#define CURRENT_FIRMWARE_VERSION "3.1.2"

// Pull-OTA source: this repo's GitHub "latest release" assets. The device fetches
// <base>version.txt, and if it is semver-NEWER than CURRENT_FIRMWARE_VERSION
// (vp_semver_newer), downloads <base>firmware_<latest>.bin. Older or garbled
// versions are ignored, so a device running ahead of GitHub never downgrades.
#define OTA_GITHUB_URL "https://github.com/adi04jan/ESP32-Programmable-Power-Supply/releases/latest/download/"

// ---- LM2596-ADJ feedback network: VOUT — 10K — FB — MCP4017 — GND ----
#define DC_R2_REF          10000
#define DC_V_REF           1235
#define MCPWIPEROHMS       15

// VV-rail ADC divider scale. Nominal 48.0 (47K:1K), trimmed against the bench
// DMM 2026-07-21: 5-point fit (5→14.8 V) gave pure gain error x0.9876, zero
// offset — resistor tolerance. Re-derive if the divider is ever reworked:
// slope of DMM-vs-reported over >=4 points, then VV_ADC_SCALE = 48 * slope.
// Refit CM_FIX_* in control_math.h after changing this (the map is in scaled mV).
#define VV_ADC_SCALE 47.40f

#define VOLTAGE_READ_PIN_VV  0
#define VOLTAGE_READ_PIN_5V  1
#define VOLTAGE_READ_PIN_3V3 3

#define ENABLE_VV_PIN   5
#define ENABLE_5V_PIN   6
#define ENABLE_3V3_PIN  7

#define MCP4017ADDRESS  0x2F

// Output-1 setpoint limits. Below ~2.47 V the LM2596 cannot regulate (its 1.235 V
// reference + the pot floor) and the controller used to hang in "settling";
// 17 V admits pot step 9 (16.6 V, still regulating) but never steps 0-8: there the
// LM2596 is in dropout and outputs the input adapter minus ~1 V (~18 V today), i.e.
// whatever adapter is plugged in. Out-of-range is an ERROR, not a clamp.
#define VB_VMIN_MV  2500u
#define VB_VMAX_MV 17000u

static const uint8_t dpMaxSteps   = 128;
static const int     maxRangeOhms = 10000;

// Shared debug-log ring size (vb_debug.cpp owns the buffer, vb_console.cpp
// drains it to telnet and the browser log WebSocket).
static const int LOGN = 3072;
