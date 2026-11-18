#include "vb_ui.h"
#include "vb_state.h"
#include "vb_hal.h"
#include "vb_debug.h"
#include "vb_ui_logic.h"
#include "vb_protect.h"
#include "vb_seq.h"
#include <Wire.h>

// =============================================================================
//   Minimal SSD1306: 128x64, 8 pages of 128 columns, with per-page dirty
//   min/max column tracking (8 entries, static) and a hard flush budget.
// =============================================================================
#define OW 128
#define OP 8                      // pages

static uint8_t  s_fb[OW * OP];    // 1024 B framebuffer
static uint8_t  s_dmin[OP], s_dmax[OP];
static bool     s_present = false;
static uint8_t  s_flush_page = 0; // round-robin carry-over between ticks

// Classic 5x7 glyphs, ASCII 32..126. 5 bytes/char, column-major, LSB = top row.
static const uint8_t FONT5[95][5] PROGMEM = {
  {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
  {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
  {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
  {0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
  {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
  {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
  {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
  {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
  {0x00,0x56,0x36,0x00,0x00},{0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},
  {0x41,0x22,0x14,0x08,0x00},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
  {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
  {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x01,0x01},
  {0x3E,0x41,0x41,0x51,0x32},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
  {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
  {0x7F,0x02,0x04,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
  {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
  {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
  {0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},{0x63,0x14,0x08,0x14,0x63},
  {0x03,0x04,0x78,0x04,0x03},{0x61,0x51,0x49,0x45,0x43},{0x00,0x00,0x7F,0x41,0x41},
  {0x02,0x04,0x08,0x10,0x20},{0x41,0x41,0x7F,0x00,0x00},{0x04,0x02,0x01,0x02,0x04},
  {0x40,0x40,0x40,0x40,0x40},{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
  {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},
  {0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x08,0x14,0x54,0x54,0x3C},
  {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},
  {0x00,0x7F,0x10,0x28,0x44},{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
  {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,0x08},
  {0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
  {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},
  {0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
  {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},
  {0x00,0x41,0x36,0x08,0x00},{0x08,0x08,0x2A,0x1C,0x08}
};

static bool oled_cmd(uint8_t c) {
  Wire.beginTransmission(ADDR_SSD1306);
  Wire.write((uint8_t)0x00);
  Wire.write(c);
  return Wire.endTransmission() == 0;
}

static void dirty(uint8_t page, uint8_t c0, uint8_t c1) {
  if (page >= OP) return;
  if (c0 < s_dmin[page]) s_dmin[page] = c0;
  if (c1 > s_dmax[page]) s_dmax[page] = c1;
}

static void fb_clear() {
  memset(s_fb, 0, sizeof(s_fb));
  for (int p = 0; p < OP; p++) { s_dmin[p] = 0; s_dmax[p] = OW - 1; }
}

// Draw one 6x8 cell. `inv` inverts it — which is how output-ON, the cursor, and
// the CC badge are all indicated without a second font.
static void fb_char(uint8_t page, uint8_t col, char ch, bool inv) {
  if (page >= OP || col + 6 > OW) return;
  if (ch < 32 || ch > 126) ch = '?';
  uint8_t *dst = &s_fb[page * OW + col];
  for (int i = 0; i < 5; i++) {
    uint8_t bits = pgm_read_byte(&FONT5[ch - 32][i]);
    dst[i] = inv ? (uint8_t)~bits : bits;
  }
  dst[5] = inv ? 0xFF : 0x00;
  dirty(page, col, (uint8_t)(col + 5));
}

static void fb_text(uint8_t page, uint8_t col, const char *s, bool inv) {
  while (*s && col + 6 <= OW) { fb_char(page, col, *s++, inv); col += 6; }
}

// Pad to a fixed width so the format-then-compare check never has to deal with
// leftover pixels from a longer previous value.
static void fb_text_w(uint8_t page, uint8_t col, const char *s, int width, bool inv) {
  char pad[22];
  int n = snprintf(pad, sizeof(pad), "%-*.*s", width, width, s);
  (void)n;
  fb_text(page, col, pad, inv);
}

bool ui_begin() {
  if (!hal_probe(ADDR_SSD1306)) { s_present = false; return false; }
  static const uint8_t init[] = {
    0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14,
    0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0x7F, 0xD9, 0xF1,
    0xDB, 0x40, 0xA4, 0xA6, 0x2E, 0xAF
  };
  for (size_t i = 0; i < sizeof(init); i++)
    if (!oled_cmd(init[i])) { s_present = false; return false; }
  fb_clear();
  s_present = true;
  return true;
}

// Flush at most `budget_us` worth of dirty columns, carrying the cursor to the
// next tick. This is what turns a 24.5 ms full repaint into ~5 imperceptible
// 6 ms slices instead of one blown control tick.
static void oled_flush(uint32_t budget_us) {
  if (!s_present) return;
  uint32_t t0 = micros();

  for (int n = 0; n < OP; n++) {
    uint8_t p = (uint8_t)((s_flush_page + n) % OP);
    if (s_dmin[p] > s_dmax[p]) continue;                 // clean page

    uint8_t c0 = s_dmin[p], c1 = s_dmax[p];
    oled_cmd(0xB0 | p);                                  // page address
    oled_cmd((uint8_t)(0x00 | (c0 & 0x0F)));             // low col
    oled_cmd((uint8_t)(0x10 | (c0 >> 4)));               // high col

    // ~30 bytes per I2C chunk keeps each transaction short enough that the
    // budget check below stays responsive.
    uint8_t col = c0;
    while (col <= c1) {
      uint8_t chunk = (uint8_t)((c1 - col + 1) > 30 ? 30 : (c1 - col + 1));
      Wire.beginTransmission(ADDR_SSD1306);
      Wire.write((uint8_t)0x40);
      Wire.write(&s_fb[p * OW + col], chunk);
      Wire.endTransmission();
      col = (uint8_t)(col + chunk);
      if (micros() - t0 > budget_us) {                   // out of budget
        s_dmin[p] = col;                                 // resume here next tick
        s_flush_page = p;
        return;
      }
    }
    s_dmin[p] = 255; s_dmax[p] = 0;                      // page clean
  }
  s_flush_page = 0;
}

// =============================================================================
//   Buttons — a task at priority 4 that owns 3 GPIOs and nothing else.
// =============================================================================
volatile bool g_ui_alloff_gesture = false;
static volatile uint8_t s_btn_edge = 0;      // bit0 SEL, bit1 UP, bit2 DOWN
static volatile uint8_t s_btn_held = 0;
static volatile uint32_t s_btn_t0[3] = {0, 0, 0};

void ui_button_task(void *) {
  const uint8_t pins[3] = { PIN_BTN_SEL, PIN_BTN_UP, PIN_BTN_DOWN };
  uint8_t  stable[3] = {1, 1, 1}, cnt[3] = {0, 0, 0};
  uint32_t chord_t0 = 0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(BTN_TICK_MS));
    uint32_t now = millis();

    for (int b = 0; b < 3; b++) {
      uint8_t raw = (uint8_t)digitalRead(pins[b]);        // active-low
      if (raw != stable[b]) {
        if (++cnt[b] >= 3) {                              // 15 ms debounce
          stable[b] = raw; cnt[b] = 0;
          if (!raw) { s_btn_t0[b] = now; s_btn_edge |= (uint8_t)(1 << b); s_btn_held |= (uint8_t)(1 << b); }
          else      { s_btn_held &= (uint8_t)~(1 << b); }
        }
      } else cnt[b] = 0;

      // A button stuck for 30 s is latched out, so auto-repeat cannot run a
      // setpoint away while nobody is watching.
      if (!stable[b] && (now - s_btn_t0[b]) > 30000) {
        s_btn_held &= (uint8_t)~(1 << b);
      }
    }

    // UP+DOWN held 1.5 s = ALL OFF. Bound in 19 of 20 states, because the one
    // gesture that must always work is the one that makes the bench safe.
    if ((s_btn_held & 0x06) == 0x06) {
      if (!chord_t0) chord_t0 = now;
      else if (now - chord_t0 > 1500) {
        g_ui_alloff_gesture = true;
        chord_t0 = now + 100000;                          // one-shot until release
      }
    } else chord_t0 = 0;
  }
}

// =============================================================================
//   Home screen
// =============================================================================
static char s_prev[8][22];        // last string painted per page, for the gate

static void paint_line(uint8_t page, const char *s, bool inv) {
  if (!strncmp(s_prev[page], s, sizeof(s_prev[0]) - 1) && !inv) return;   // unchanged
  strncpy(s_prev[page], s, sizeof(s_prev[0]) - 1);
  s_prev[page][sizeof(s_prev[0]) - 1] = 0;
  fb_text_w(page, 0, s, 21, inv);
}

void ui_tick(uint32_t now_ms) {
  if (!s_present) {
    // Retry every 5 s. A dead OLED must not stop the instrument being driven
    // remotely — but locally, only ALL OFF stays bound, because a user who
    // cannot see what they are changing must not be able to change it.
    static uint32_t t_retry = 0;
    if (now_ms - t_retry > 5000) { t_retry = now_ms; ui_begin(); }
    if (g_ui_alloff_gesture) {
      g_ui_alloff_gesture = false;
      VbCommand c; memset(&c, 0, sizeof(c));
      c.op = OP_ALL_OFF; c.src = SRC_PANEL;
      apply_command(&c);
    }
    return;
  }

  if (g_ui_alloff_gesture) {
    g_ui_alloff_gesture = false;
    VbCommand c; memset(&c, 0, sizeof(c));
    c.op = OP_ALL_OFF; c.src = SRC_PANEL;
    apply_command(&c);
  }

  VbSnapshot s;
  vb_snapshot(&s);

  char line[24], v[10], i[10];
  for (int k = 0; k < VB_CHANNELS; k++) {
    const VbChanSnap *c = &s.ch[k];
    bool on   = c->state == PS_CV || c->state == PS_CC || c->state == PS_STARTING;
    bool trip = c->state == PS_TRIPPED;

    // Dashes never mean zero: a dead sensor must be visually distinct from a
    // rail sitting at 0 V.
    if (c->sense_ok) { ui_fmt_milli(c->v_mV, 2, v, sizeof(v));
                       ui_fmt_milli(c->i_mA, 3, i, sizeof(i)); }
    else             { ui_fmt_dashes(2, v, sizeof(v)); ui_fmt_dashes(3, i, sizeof(i)); }

    const char *badge = trip                ? "TR"
                      : c->state == PS_CC   ? "CC"
                      : on                  ? "CV" : "--";
    snprintf(line, sizeof(line), "CH%d %5sV %6sA %s", k + 1, v, i, badge);
    paint_line((uint8_t)(k * 2), line, on);

    ui_fmt_milli(c->vset_mV, 2, v, sizeof(v));
    ui_fmt_milli(c->iset_mA, 3, i, sizeof(i));
    snprintf(line, sizeof(line), "set %5sV %6sA %s", v, i,
             c->cc_mode == CCM_TRIP ? "TR" : "FB");
    paint_line((uint8_t)(k * 2 + 1), line, false);
  }

  // Detail line at full precision — the home rows show 2 decimals because the
  // INA219's 4 mV LSB plus ripple makes a third decimal dance.
  ui_fmt_milli(s.ch[0].v_mV, 3, v, sizeof(v));
  snprintf(line, sizeof(line), "CH1 %6sV %5d.%01dW", v,
           (int)(s.ch[0].w_mW / 1000), (int)((s.ch[0].w_mW % 1000) / 100));
  paint_line(4, line, false);

  // Page 5 is the alert slot, deliberately empty when idle: an inverted bar
  // appearing in otherwise-blank space is maximally noticeable.
  const char *alert = "";
  bool alert_inv = false;
  for (int k = 0; k < VB_CHANNELS; k++) {
    if (s.ch[k].state == PS_TRIPPED) {
      snprintf(line, sizeof(line), "CH%d TRIP: %s", k + 1,
               prot_reason_str((ProtReason)s.ch[k].reason));
      alert = line; alert_inv = true;
      break;
    }
  }
  if (!alert_inv && s.hw_fault) { alert = "HW FAULT - outputs off"; alert_inv = true; }
  paint_line(5, alert, alert_inv);

  snprintf(line, sizeof(line), "%d.%01dC  seq:%s", (int)(s.temp_c10 / 10),
           (int)(s.temp_c10 % 10), s.seq_state == SQ_RUNNING ? "run" : "-");
  paint_line(6, line, false);

  oled_flush(UI_FLUSH_BUDGET_MS * 1000);
}
