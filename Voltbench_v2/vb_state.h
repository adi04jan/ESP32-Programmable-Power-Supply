// =============================================================================
//   vb_state.h — the three (and ONLY three) mechanisms for shared state.
//
//   (a) TELEMETRY OUT — seqlock. ctrl publishes one VbSnapshot behind an
//       even/odd version counter. This replaces v1's loose volatiles, which
//       could report voltage from tick N and trip state from tick N+1 and so
//       produce "output OFF but reading 12 V" on screen.
//
//       The retry loop provably terminates: every reader (async_tcp 3, con 2,
//       loop 1) is strictly BELOW the writer (ctrl 5), so a reader can be
//       preempted mid-copy but the writer can never be preempted by a reader.
//       Safe from an AsyncTCP callback; never blocks.
//
//   (b) SETPOINTS IN — last-writer-wins volatile cells. 32-bit aligned scalars
//       are atomic on RV32. ctrl diffs them each tick.
//
//   (c) EVERYTHING ELSE — one FreeRTOS queue, xQueueSend(..., 0) so it is legal
//       from an AsyncTCP callback. Full -> VB_EQFULL -> HTTP 503.
//
//   FORBIDDEN INSIDE ASYNCTCP CALLBACKS: Wire.*, Preferences.* (a 10-40 ms
//   flash erase stalls the entire network stack), anything blocking, any String
//   over 1 KB.
// =============================================================================
#pragma once
#include "vb_config.h"
#include "vb_cmd.h"
#include "vb_protect.h"
#include "vb_cal.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

typedef struct {
  int32_t v_mV, i_mA, w_mW;       // measured
  int32_t vset_mV, iset_mA;       // commanded
  int32_t ovp_mV, ocp_mA;
  uint8_t en;                     // user's enable request
  uint8_t state;                  // ProtState
  uint8_t reason;                 // ProtReason
  uint8_t cc_mode;                // CcMode
  uint8_t cal_valid;
  uint8_t sense_ok;               // INA219 answering
} VbChanSnap;

typedef struct {
  uint32_t   ts_ms;
  uint32_t   uptime_s;
  VbChanSnap ch[VB_CHANNELS];
  int32_t    temp_c10;
  uint8_t    temp_ok;
  uint8_t    seq_state;
  uint16_t   seq_pc, seq_n;
  uint32_t   seq_el_ms;
  uint8_t    log_on;
  uint32_t   log_n;
  uint16_t   log_cap;
  uint32_t   rev_set;             // bumped on every setpoint change
  uint8_t    last_src;            // VbSrc of the last setpoint change
  uint8_t    hw_fault;
} VbSnapshot;

// ---- (a) seqlock ------------------------------------------------------------
void vb_publish(const VbSnapshot *s);   // ctrl task ONLY
void vb_snapshot(VbSnapshot *out);      // any reader, any task, never blocks

// ---- (b) setpoint cells -----------------------------------------------------
// Written by any transport, diffed by ctrl. Last writer wins by design: two
// clients fighting over a setpoint is a user problem, not a data-race problem.
extern volatile int32_t g_set_mV[VB_CHANNELS];
extern volatile int32_t g_set_mA[VB_CHANNELS];
extern volatile uint8_t g_en_cmd[VB_CHANNELS];
extern volatile uint8_t g_cc_mode[VB_CHANNELS];
extern volatile int32_t g_ovp_mV[VB_CHANNELS];
extern volatile int32_t g_ocp_mA[VB_CHANNELS];
extern volatile int32_t g_otp_c;
extern volatile uint32_t g_rev_set;
extern volatile uint8_t  g_last_src;

// ---- (c) command queue ------------------------------------------------------
extern QueueHandle_t g_cmd_q;
extern SemaphoreHandle_t g_nvs_mux;

// Set by ctrl on any trip, CC transition, or sequencer step change; honoured
// once by loop(), bypassing the 250 ms WS rate limit.
extern volatile bool g_push_now;

// Deferred NVS writes: ctrl and async_tcp set dirty flags, loopTask (priority 1,
// harmless to block) does the actual 10-40 ms flash erase.
extern volatile bool g_nvs_dirty_settings;
extern volatile bool g_nvs_dirty_cal;
extern volatile bool g_nvs_dirty_last;

// Per-channel calibration, owned by ctrl, published through the snapshot.
extern ChanCal g_cal[VB_CHANNELS];
extern bool    g_cal_valid[VB_CHANNELS];

void vb_state_init();

// The one dispatcher every transport funnels into (spec §4.1). The typed core
// is what the OLED and sequencer call — no String allocation per button press.
VbErr apply_command(const VbCommand *c);
VbErr apply_command_str(const String &action, uint8_t ch, float value, uint8_t src);
