// =============================================================================
//   vb_control.h — the ctrl task (spec §3.1, §3.4).
//
//   ctrl runs at priority 5, ABOVE AsyncTCP (3), so an over-current trip never
//   queues behind a WebSocket burst. The cost is one absolute rule:
//
//        CTRL MUST NEVER BLOCK WITHOUT YIELDING,
//
//   or AsyncTCP starves and the device drops off the network. Its duty is ~3 ms
//   of the 20 ms tick plus up to 6 ms of display flush; everything else is
//   vTaskDelayUntil.
//
//   ctrl owns, exclusively: all I2C (DAC, both INA219s, OLED), the temp ADC,
//   the EN/CAL GPIOs, the protection FSM, the sequencer, and the log ring.
// =============================================================================
#pragma once
#include "vb_config.h"
#include "vb_log.h"
#include "vb_seq.h"

void vb_control_task(void *);

// Boot sequence, run from setup() BEFORE the task starts. Returns false if the
// hardware could not be proven safe — in which case outputs can never be
// enabled this boot, which is the correct outcome, not a bug to work around.
bool vb_control_boot();

extern volatile bool g_hw_fault;         // I2C NACK or DAC readback mismatch
extern LogRing       g_logring;
extern SeqCtx        g_seqctx;
extern SeqStep       g_seq[SEQ_MAX_STEPS];

// Self-cal load — interlocked to <=3 V MEASURED and 2 s, because the 10 Ohm 5 W
// part would see 40 W at 20 V.
bool vb_cal_load_engage(uint8_t ch);
void vb_cal_load_release();
