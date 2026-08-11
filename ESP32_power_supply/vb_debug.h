// =============================================================================
//   vb_debug.h — remote debug log. Fans out to USB Serial + a ring buffer that
//   telnet (:23) and the browser log WebSocket drain from vb_console.cpp, so a
//   late-connecting client still sees recent output.
//
//   Producers (any task) only touch Serial + the ring. The telnet/WS consumers
//   are pumped from loop() to respect AsyncTCP's single-task rule.
// =============================================================================
#pragma once
#include "vb_config.h"

// NOTE: `if (Serial)` is NOT enough — after an esptool flash the CDC session is
// left stale-"connected" (port enumerated, nobody reading). Once the TX ring
// fills, HWCDC::write's retry counter underflows with txTimeout=0 (core 3.2.1
// HWCDC.cpp:448-479: `tries--` from 0) and blocks loop() ~forever. Writing no
// more than availableForWrite() keeps every write on the non-blocking fast path.
// DO NOT "simplify" the write() bodies in vb_debug.cpp.
class DbgPrint : public Print {
 public:
  size_t write(uint8_t c) override;
  size_t write(const uint8_t *b, size_t n) override;
};

extern DbgPrint Dbg;

// The log ring itself — drained by vb_console.cpp.
extern char              g_log[LOGN];
extern volatile uint32_t g_logw;          // monotonic write counter
