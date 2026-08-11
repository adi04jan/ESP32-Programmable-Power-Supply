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

#define CURRENT_FIRMWARE_VERSION "3.0.1"

// Pull-OTA source: this repo's GitHub "latest release" assets. The device fetches
// <base>version.txt, and if it differs from CURRENT_FIRMWARE_VERSION, downloads
// <base>firmware_<latest>.bin. /releases/latest/download/ always resolves to the
// newest release, so each new release is auto-discovered.
#define OTA_GITHUB_URL "https://github.com/adi04jan/ESP32-Programmable-Power-Supply/releases/latest/download/"

// ---- LM2596-ADJ feedback network: VOUT — 10K — FB — MCP4017 — GND ----
#define DC_R2_REF          10000
#define DC_V_REF           1235
#define MCPWIPEROHMS       15

// VV-rail ADC divider scale. Nominal 48.0 (47K:1K), trimmed against the bench
// DMM 2026-07-21: 5-point fit (5→14.8 V) gave pure gain error x0.9876, zero
// offset — resistor tolerance. Re-derive if the divider is ever reworked:
// slope of DMM-vs-reported over >=4 points, then VV_ADC_SCALE = 48 * slope.
// Re-run the calibration sweep after changing this (map stores scaled mV).
#define VV_ADC_SCALE 47.40f

#define VOLTAGE_READ_PIN_VV  0
#define VOLTAGE_READ_PIN_5V  1
#define VOLTAGE_READ_PIN_3V3 3

#define ENABLE_VV_PIN   5
#define ENABLE_5V_PIN   6
#define ENABLE_3V3_PIN  7

#define MCP4017ADDRESS  0x2F

static const uint8_t dpMaxSteps   = 128;
static const int     maxRangeOhms = 10000;

// Shared debug-log ring size (vb_debug.cpp owns the buffer, vb_console.cpp
// drains it to telnet and the browser log WebSocket).
static const int LOGN = 3072;
