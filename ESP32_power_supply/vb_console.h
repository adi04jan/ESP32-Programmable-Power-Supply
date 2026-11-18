// =============================================================================
//   vb_console.h — the bench console (USB Serial + telnet :23) and the drain
//   side of the debug-log ring.
//
//   Everything here is pumped from loop(): AsyncTCP requires that its sockets
//   are only touched from a single task, and the telnet/WS writers below share
//   that constraint.
// =============================================================================
#pragma once
#include "vb_config.h"

// Execute one bench-console command, replying on `out` with exactly one line
// starting "OK" or "ERR <code>". Shared by USB + telnet; src is a VbSrc.
void console_exec(const String &lineIn, Print &out, uint8_t src);

// Start the telnet server (:23). Call from setup(), after WiFi is up.
void vb_console_begin();

// Call once per loop(): services the USB console, the telnet client (log
// stream + password-gated commands), the browser log WebSocket, and the
// periodic output-1 telemetry line.
void vb_console_pump();
