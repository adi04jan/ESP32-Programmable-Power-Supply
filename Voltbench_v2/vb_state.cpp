#include "vb_state.h"
#include "vb_debug.h"

// ---- (a) seqlock ------------------------------------------------------------
// Even = stable, odd = write in progress. The writer is ctrl (prio 5) and every
// reader is strictly below it, which is what makes the reader's retry loop
// terminate rather than merely "usually terminate".
static volatile uint32_t g_snap_ver = 0;
static VbSnapshot        g_snap;

void vb_publish(const VbSnapshot *s) {
  g_snap_ver++;                       // -> odd: readers back off
  __asm__ __volatile__("" ::: "memory");
  g_snap = *s;
  __asm__ __volatile__("" ::: "memory");
  g_snap_ver++;                       // -> even: stable again
}

void vb_snapshot(VbSnapshot *out) {
  uint32_t v1, v2;
  int guard = 0;
  do {
    v1 = g_snap_ver;
    if (v1 & 1) { if (++guard > 64) break; continue; }   // writer mid-copy
    __asm__ __volatile__("" ::: "memory");
    *out = g_snap;
    __asm__ __volatile__("" ::: "memory");
    v2 = g_snap_ver;
  } while (v1 != v2 && ++guard <= 64);
}

// ---- (b) setpoint cells -----------------------------------------------------
volatile int32_t  g_set_mV[VB_CHANNELS]  = { 5000, 5000 };
volatile int32_t  g_set_mA[VB_CHANNELS]  = { 500, 500 };
volatile uint8_t  g_en_cmd[VB_CHANNELS]  = { 0, 0 };
volatile uint8_t  g_cc_mode[VB_CHANNELS] = { CCM_LIMIT, CCM_LIMIT };
volatile int32_t  g_ovp_mV[VB_CHANNELS]  = { OVP_DEFAULT_MV, OVP_DEFAULT_MV };
volatile int32_t  g_ocp_mA[VB_CHANNELS]  = { OCP_DEFAULT_MA, OCP_DEFAULT_MA };
volatile int32_t  g_otp_c    = OTP_DEFAULT_C;
volatile uint32_t g_rev_set  = 0;
volatile uint8_t  g_last_src = SRC_PANEL;

// ---- (c) queue + flags ------------------------------------------------------
QueueHandle_t     g_cmd_q  = nullptr;
SemaphoreHandle_t g_nvs_mux = nullptr;
volatile bool     g_push_now = false;
volatile bool     g_nvs_dirty_settings = false;
volatile bool     g_nvs_dirty_cal      = false;
volatile bool     g_nvs_dirty_last     = false;

ChanCal g_cal[VB_CHANNELS];
bool    g_cal_valid[VB_CHANNELS] = { false, false };

void vb_state_init() {
  g_nvs_mux = xSemaphoreCreateMutex();
  g_cmd_q   = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(VbCommand));
  for (int k = 0; k < VB_CHANNELS; k++) cal_defaults(&g_cal[k]);
}

// -----------------------------------------------------------------------------
//   The single dispatcher. Six transports funnel through here; adding a command
//   anywhere else means five transports silently lack the feature.
//
//   Setpoint-shaped commands are applied to the volatile cells directly (cheap,
//   legal from any task). Everything else is queued for ctrl, because it needs
//   the I2C bus or the log ring, and NOTHING but ctrl may touch those.
// -----------------------------------------------------------------------------
static inline int ch_ok(uint8_t ch) { return ch >= 1 && ch <= VB_CHANNELS; }

static void stamp(uint8_t src) {
  g_last_src = src;
  g_rev_set++;              // the panel's remote-change detection reads this
}

VbErr apply_command(const VbCommand *c) {
  const uint8_t k = c->ch ? (uint8_t)(c->ch - 1) : 0;

  switch (c->op) {
    case OP_SET_V:
      if (!ch_ok(c->ch)) return VB_EBADCH;
      if (c->arg < 0 || c->arg > VB_VMAX_MV) return VB_ERANGE;
      g_set_mV[k] = c->arg;
      stamp(c->src);
      return VB_OK;

    case OP_SET_I:
      if (!ch_ok(c->ch)) return VB_EBADCH;
      if (c->arg < 0 || c->arg > VB_IMAX_MA) return VB_ERANGE;
      g_set_mA[k] = c->arg;
      stamp(c->src);
      return VB_OK;

    // Relative adjust structurally eliminates the remote-edit race: three UP
    // presses always mean "+3 steps from whatever is authoritative right now",
    // never "write 12.03" computed from a value that went stale in between.
    case OP_ADJ_V: {
      if (!ch_ok(c->ch)) return VB_EBADCH;
      int32_t ceil_mV = g_ovp_mV[k] < VB_VMAX_MV ? g_ovp_mV[k] : VB_VMAX_MV;
      int32_t v = g_set_mV[k] + c->arg;
      VbErr   e = VB_OK;
      if (v < VB_VMIN_MV) { v = VB_VMIN_MV; e = VB_ECLAMPED; }
      if (v > ceil_mV)    { v = ceil_mV;    e = VB_ECLAMPED; }
      g_set_mV[k] = v;
      stamp(c->src);
      return e;
    }
    case OP_ADJ_I: {
      if (!ch_ok(c->ch)) return VB_EBADCH;
      int32_t i = g_set_mA[k] + c->arg;
      VbErr   e = VB_OK;
      if (i < VB_IMIN_MA) { i = VB_IMIN_MA; e = VB_ECLAMPED; }
      if (i > VB_IMAX_MA) { i = VB_IMAX_MA; e = VB_ECLAMPED; }
      g_set_mA[k] = i;
      stamp(c->src);
      return e;
    }

    case OP_ENABLE:
      if (!ch_ok(c->ch)) return VB_EBADCH;
      g_en_cmd[k] = c->arg ? 1 : 0;
      g_push_now  = true;
      return VB_OK;

    case OP_ALL_OFF:
      for (int j = 0; j < VB_CHANNELS; j++) g_en_cmd[j] = 0;
      g_push_now = true;
      return VB_OK;

    case OP_CC_MODE:
      if (!ch_ok(c->ch)) return VB_EBADCH;
      g_cc_mode[k] = c->arg ? CCM_TRIP : CCM_LIMIT;
      g_nvs_dirty_settings = true;
      return VB_OK;

    case OP_SET_OVP:
      if (!ch_ok(c->ch)) return VB_EBADCH;
      if (c->arg < 1000 || c->arg > OVP_HARD_CAP) return VB_ERANGE;
      g_ovp_mV[k] = c->arg;
      g_nvs_dirty_settings = true;
      return VB_OK;

    case OP_SET_OCP:
      if (!ch_ok(c->ch)) return VB_EBADCH;
      if (c->arg < 10 || c->arg > VB_IMAX_MA) return VB_ERANGE;
      g_ocp_mA[k] = c->arg;
      g_nvs_dirty_settings = true;
      return VB_OK;

    case OP_SET_OTP:
      if (c->arg < 40 || c->arg > 100) return VB_ERANGE;
      g_otp_c = c->arg;
      g_nvs_dirty_settings = true;
      return VB_OK;

    // Everything below needs the I2C bus, the log ring, or the sequencer — all
    // owned exclusively by ctrl. Queue it; never do it here.
    default:
      if (!g_cmd_q) return VB_EUNKNOWN;
      if (xQueueSend(g_cmd_q, c, 0) != pdTRUE) return VB_EQFULL;
      return VB_OK;
  }
}

VbErr apply_command_str(const String &action, uint8_t ch, float value, uint8_t src) {
  VbCommand c;
  memset(&c, 0, sizeof(c));
  c.ch  = ch;
  c.src = src;

  if      (action == "set_voltage") { c.op = OP_SET_V;     c.arg = (int32_t)lrintf(value * 1000.0f); }
  else if (action == "set_current") { c.op = OP_SET_I;     c.arg = (int32_t)lrintf(value * 1000.0f); }
  else if (action == "adj_voltage") { c.op = OP_ADJ_V;     c.arg = (int32_t)lrintf(value * 1000.0f); }
  else if (action == "adj_current") { c.op = OP_ADJ_I;     c.arg = (int32_t)lrintf(value * 1000.0f); }
  else if (action == "toggle")      { c.op = OP_ENABLE;    c.arg = value >= 0.5f ? 1 : 0; }
  else if (action == "all_off")     { c.op = OP_ALL_OFF; }
  else if (action == "cc_mode")     { c.op = OP_CC_MODE;   c.arg = value >= 0.5f ? 1 : 0; }
  else if (action == "clear_trip")  { c.op = OP_CLEAR_TRIP; }
  else if (action == "set_ovp")     { c.op = OP_SET_OVP;   c.arg = (int32_t)lrintf(value * 1000.0f); }
  else if (action == "set_ocp")     { c.op = OP_SET_OCP;   c.arg = (int32_t)lrintf(value * 1000.0f); }
  else if (action == "set_otp")     { c.op = OP_SET_OTP;   c.arg = (int32_t)lrintf(value); }
  else return VB_EUNKNOWN;

  return apply_command(&c);
}
