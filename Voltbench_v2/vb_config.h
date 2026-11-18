// =============================================================================
//   vb_config.h — pins, I2C addresses, electrical constants, limits, version.
//
//   EVERY translation unit must include this FIRST. Three Arduino-IDE rules
//   depend on it (spec §3.3):
//     1. .cpp files in a sketch folder get NO implicit #include <Arduino.h>.
//        This header provides it.
//     2. #pragma GCC optimize("Os") is PER-TU. It lives here. A .cpp that
//        forgets this include compiles unoptimised; losing it across the board
//        costs ~60 KB of flash. If a build suddenly grows ~60 KB, look here.
//     3. Files in subdirectories are NOT compiled unless the dir is named src/.
//        Keep this folder flat — flat files also get IDE tabs.
// =============================================================================
#pragma once
#pragma GCC optimize("Os")
#include <Arduino.h>

#define CURRENT_FIRMWARE_VERSION "4.0.0"
#define VB_API_VERSION           2          // status-frame shape version (spec §2.4)
#define VB_PROTO_VERSION         1          // serial line protocol version (spec §4.3)
#define VB_CHANNELS              2

#define OTA_GITHUB_URL "https://github.com/adi04jan/ESP32-Programmable-Power-Supply/releases/latest/download/"

// -----------------------------------------------------------------------------
//   GPIO map — ESP32-C3 Super Mini (Hardware/WIRING-GUIDE.md)
//
//   GPIO2 is deliberately UNCONNECTED: it is a strapping pin that must read
//   HIGH at reset. GPIO8 (also strapping) is the onboard LED, driven as an
//   output only after boot. GPIO9 is BOOT — left alone.
//   BTN3 sits on GPIO21 = U0TXD, which the ROM drives during boot; the 1 kOhm
//   series resistor in the wiring guide is what makes that safe. Do not move
//   a button onto GPIO2 — an analog sensor or button idling low there bricks
//   the boot, which is exactly the bug that forced this pin rotation.
// -----------------------------------------------------------------------------
#define PIN_BTN_SEL    0
#define PIN_BTN_UP     1
#define PIN_BTN_DOWN  21      // 1 kOhm series — ROM drives U0TXD
#define PIN_CAL2       3      // self-cal load FET, ch2 (10k pull-down at gate)
#define PIN_TEMP       4      // ADC1_CH4 — MCP9700 via 1k + 100nF
#define PIN_EN2        5
#define PIN_SDA        6
#define PIN_SCL        7
#define PIN_LED        8      // strapping — output only after boot
#define PIN_EN1       10
#define PIN_CAL1      20      // input during ROM boot, so the load stays off

#define I2C_HZ       400000UL

// -----------------------------------------------------------------------------
//   I2C addresses (Hardware/HARDWARE-DESIGN-V2.md — no conflicts)
// -----------------------------------------------------------------------------
#define ADDR_MCP4728   0x60
#define ADDR_INA219_1  0x40
#define ADDR_INA219_2  0x41
#define ADDR_SSD1306   0x3C

// MCP4728 channel assignment. A/C are CV (inverted!), B/D are CC.
#define DAC_CH_CV1  0
#define DAC_CH_CC1  1
#define DAC_CH_CV2  2
#define DAC_CH_CC2  3

// -----------------------------------------------------------------------------
//   Electrical constants
//
//   THE INVERTED CV MAPPING GOVERNS EVERY FAILURE MODE (spec §1):
//       Vout = 20.18 V - 9.52 * Vdac  =  20180 mV - code * 4.763 mV
//   so DAC code 0 = 20.18 V. ANY path that leaves a CV DAC unwritten is a 20 V
//   path. That is why the boot sequence writes 4095 before anything else and
//   reads every register back twice before EN can go high.
// -----------------------------------------------------------------------------
#define CV_V0_MV_NOM      20180.0f   // mV at DAC code 0
#define CV_SLOPE_UV_NOM    4763.0f   // uV of output per DAC code (falling)
#define CC_GAIN_NOM           2.0f   // DAC codes per mA (0.5 mA/LSB)
#define CC_OFF_MA_NOM         0.0f

// Below-spec region of the CV DAC — never command into it.
#define CV_CODE_MAX        4069
#define CV_CODE_SAFE       4095      // minimum volts = safest CV code
#define CC_CODE_SAFE          0      // zero current limit = safest CC code

// Two-point calibration codes (spec §3.4). The low point (~2.1 V) sits inside
// the self-cal load's <=3 V interlock so one routine trims both.
#define CAL_CODE_LO        3800      // ~2.1 V
#define CAL_CODE_HI        1700      // ~12.1 V
#define CAL_CODE_MIN_SPAN  1500

// Calibration acceptance bounds — a fit outside these is rejected and nominal
// is kept with cal_valid=false, which is what stops a corrupt fit from
// commanding 20 V.
#define CAL_V0_MIN_MV     18500.0f
#define CAL_V0_MAX_MV     22000.0f
#define CAL_SLOPE_MIN_UV   4300.0f
#define CAL_SLOPE_MAX_UV   5300.0f

// INA219: 0.1 Ohm shunt, cal register 0x1000 -> 100 uA/LSB, bus LSB 4 mV.
#define INA_CAL_REG       0x1000
#define INA_I_LSB_UA         100
#define INA_V_LSB_MV           4

// Phantom divider current (spec §9.4): the 100k CV divider taps after the
// shunt, so ~187 uA at 20 V reads as load current. Subtract v_mV/107 uA.
#define PHANTOM_DIV_OHMS     107

// MCP9700: 500 mV at 0 C, 10 mV/C.
#define TEMP_MV_AT_0C        500.0f
#define TEMP_MV_PER_C         10.0f

// -----------------------------------------------------------------------------
//   Operating limits
// -----------------------------------------------------------------------------
#define VB_VMIN_MV      1000      // MP1584 min-on-time floor; 0.8 V is best-effort
#define VB_VMAX_MV     20000
#define VB_IMIN_MA         0
#define VB_IMAX_MA      2048      // CC DAC hardware ceiling

#define OVP_DEFAULT_MV 21000
#define OVP_HARD_CAP   21500
#define OCP_DEFAULT_MA  2048
#define OTP_DEFAULT_C     80
#define OTP_HYST_C         5

// Protection debounce, milliseconds (spec §3.5). These are not arbitrary:
// OCP must clear both the analog CC band (+/-75 mA) and integrator windup,
// and CC exit is deliberately ~3x CC entry so the badge cannot flicker.
#define DEB_OVP_MS        20
#define DEB_OCP_MS       100
#define DEB_CC_ENTER_MS   60
#define DEB_CC_EXIT_MS   200
#define DEB_CC_TRIP_MS    60
#define DEB_OTP_MS      1000
#define DEB_HW_TICKS       3
#define GRACE_MS         250      // STARTING: protection suppressed through inrush
#define CLEARING_MS      200
#define CH2_STAGGER_MS   100      // avoid a double inrush on power-on policy
#define REG_WARN_MS     3000      // |v-vset|>1V -> log only, never a trip

// CC entry predicate margins.
#define CC_MARGIN_MA      50      // or 5% of the limit, whichever is larger
#define CC_V_DROOP_MV    250      // clears ripple + regulation error
#define OCP_OVER_LIMIT_MA 150     // OCP = min(ocp, i_limit + 150 mA)

// Self-cal load interlock (spec §8 phase 9) — 10 Ohm 5 W part, so 20 V would
// be 40 W. Refuse above 3 V MEASURED, and hard-stop after 2 s.
#define CAL_LOAD_MAX_MV  3000
#define CAL_LOAD_MAX_MS  2000

// -----------------------------------------------------------------------------
//   Task shape (spec §3.1). ESP32-C3 is SINGLE-CORE: priority ordering IS the
//   concurrency design, and every pinned-to-core-0 call in v1 was decorative.
//
//   ctrl runs at 5 — ABOVE AsyncTCP (3) — so an over-current trip never queues
//   behind a WebSocket burst. The cost is one absolute rule: ctrl must never
//   block without yielding, or AsyncTCP starves and the device drops off the
//   network.
// -----------------------------------------------------------------------------
#define PRIO_CTRL   5
#define PRIO_BTN    4
#define PRIO_CON    2
#define PRIO_WIFI   2
#define STACK_CTRL  6144
#define STACK_BTN   2048
#define STACK_CON   3072
#define STACK_WIFI  8192

#define CTRL_TICK_MS   20
#define BTN_TICK_MS     5
#define CON_TICK_MS    10
#define UI_FLUSH_BUDGET_MS 6      // hard per-tick display budget; full repaints
                                  // are CHUNKED to this, never blasted at once

#define LOGN 3072                 // DbgPrint ring bytes
#define LOG_RING_RECORDS 2048     // 12-byte LogRec each = 24576 B static
#define SEQ_MAX_STEPS      64
#define PRESET_SLOTS        8
#define CMD_QUEUE_DEPTH    16
#define CON_LINE_MAX       96     // spec §4.3: longer lines are discarded whole
