#include "vb_debug.h"

char              g_log[LOGN];
volatile uint32_t g_logw   = 0;
static portMUX_TYPE g_logmux = portMUX_INITIALIZER_UNLOCKED;

DbgPrint Dbg;

size_t DbgPrint::write(uint8_t c) {
  if (Serial && Serial.availableForWrite() > 0) Serial.write(c);
  portENTER_CRITICAL(&g_logmux);
  g_log[g_logw % LOGN] = (char)c; g_logw++;
  portEXIT_CRITICAL(&g_logmux);
  return 1;
}

size_t DbgPrint::write(const uint8_t *b, size_t n) {
  if (Serial) {
    size_t a = (size_t)Serial.availableForWrite();
    if (a > 0) Serial.write(b, n < a ? n : a);   // truncate to free space, never block
  }
  portENTER_CRITICAL(&g_logmux);
  for (size_t i = 0; i < n; i++) { g_log[g_logw % LOGN] = (char)b[i]; g_logw++; }
  portEXIT_CRITICAL(&g_logmux);
  return n;
}
