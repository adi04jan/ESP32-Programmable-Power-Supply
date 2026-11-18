// =============================================================================
//   vb_ui.h — SSD1306 rendering + the 3-button panel (spec §5).
//
//   WHY THERE IS A HAND-WRITTEN DRIVER HERE instead of Adafruit_SSD1306:
//   that library's display() pushes all 1024 bytes every call, which is ~24.5 ms
//   at 400 kHz and would blow the 20 ms control tick on its own. The spec's
//   budget (0.6% bus share idle, 2.7% during a ramp) is only reachable with
//   per-page dirty tracking and a hard per-tick flush budget, so that is what
//   this is.
//
//   The gate that does the real work is FORMAT-THEN-COMPARE: each dynamic field
//   renders into a small char[] and is redrawn only if the string changed. A
//   stable rail therefore produces ZERO display traffic no matter how much the
//   ADC dithers below the displayed resolution.
//
//   ui_tick() runs INSIDE the control task (spec §2.1). That is what removes
//   both a whole task and every I2C mutex in the firmware.
// =============================================================================
#pragma once
#include "vb_config.h"

bool ui_begin();                 // false if the OLED did not answer
void ui_tick(uint32_t now_ms);   // called from ctrl, self-limited to 6 ms
void ui_button_task(void *);     // priority 4, owns the 3 GPIOs and nothing else

// Set by the button task, consumed by ui_tick.
extern volatile bool g_ui_alloff_gesture;
