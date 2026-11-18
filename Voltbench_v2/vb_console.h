// =============================================================================
//   vb_console.h — the `con` task (USB-CDC) and telnet, sharing one parser.
//
//   con IS A SEPARATE TASK, NOT PART OF loop(). The serial console exists to
//   rescue a device whose network stack has wedged — the exact HWCDC scenario
//   in CLAUDE.md. Inside loop() it would die with the thing it exists to rescue.
//
//   Reply discipline (spec §4.3) — the first character classifies every line,
//   which is what makes this machine-parseable where v1's console was not:
//       OK                 command succeeded
//       OK <csv|json>      query reply
//       ERR <code> <text>  failure; code is a stable token
//       ! <event...>       asynchronous (trip, seq step, telemetry, READY)
//       # <text>           DbgPrint log fan-out
//       > <text>           multi-line payload continuation
//
//   Serial is UNAUTHENTICATED by default: physical access has already crossed
//   the trust boundary, and locking yourself out of the recovery path is worse
//   than the threat it prevents. factory_reset and password changes are simply
//   not exposed here. Telnet reuses the identical parser but stays password-
//   gated, because it is network-reachable.
// =============================================================================
#pragma once
#include "vb_config.h"

void vb_console_task(void *);      // priority 2
void vb_console_begin();           // telnet listener; call after WiFi is up
void vb_console_pump();            // telnet + /logws drain, from loop()
