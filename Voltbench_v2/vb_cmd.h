// =============================================================================
//   vb_cmd.h — PURE: the command type every transport funnels into, plus the
//   serial line tokeniser (spec §4.1, §4.3).
//
//   WHY THIS IS HEAP-FREE, AND WHY THAT IS NON-NEGOTIABLE:
//   the serial port exists to rescue a device whose network stack has wedged,
//   and those scenarios include heap exhaustion. A JSON parser needs the heap
//   and therefore fails exactly when it is needed. This tokeniser works over a
//   96-byte stack buffer and allocates nothing, ever.
//
//   The number parser is hand-written rather than strtof for the same reason,
//   plus one more: strtod is locale-sensitive, and a locale with a comma
//   decimal separator would silently reinterpret "VOLT 1,5.0".
// =============================================================================
#pragma once
#include <stdint.h>
#include <string.h>

typedef enum {
  OP_SET_V = 0, OP_SET_I, OP_ADJ_V, OP_ADJ_I, OP_ENABLE, OP_ALL_OFF,
  OP_CC_MODE, OP_CLEAR_TRIP, OP_SET_OVP, OP_SET_OCP, OP_SET_OTP,
  OP_LOG, OP_SEQ, OP_PRESET_SAVE, OP_PRESET_LOAD, OP_CAL_AUTO, OP_CAL_PT
} VbOp;

typedef enum {
  VB_OK = 0, VB_EBADCH, VB_ERANGE, VB_ECLAMPED, VB_EBUSY, VB_ETRIP,
  VB_EINTERLOCK, VB_EQFULL, VB_EUNKNOWN, VB_EHW
} VbErr;

// Who issued it. Every setpoint change stamps last_src and bumps rev_set — the
// OLED's remote-change detection is built entirely on those two fields.
typedef enum {
  SRC_PANEL = 0, SRC_WEB, SRC_WS, SRC_MQTT, SRC_TELNET, SRC_SERIAL, SRC_SEQ
} VbSrc;

// Sub-ops carried in .arg for the grouped operations.
enum { LOGOP_START = 0, LOGOP_STOP, LOGOP_CLEAR, LOGOP_RATE };
enum { SEQOP_RUN = 0, SEQOP_STOP, SEQOP_PAUSE, SEQOP_RESUME };
enum { CALPT_CV = 0, CALPT_CC };
// Kept numerically identical to CcMode in vb_protect.h without including it —
// these two PURE headers stay independent. tools/host_test asserts they match.
enum { CCM_LIMIT_VAL = 0, CCM_TRIP_VAL = 1 };

typedef struct {
  uint8_t op;      // VbOp
  uint8_t ch;      // 1..2; 0 means "all" / global
  uint8_t src;     // VbSrc
  int32_t arg;     // mV, mA, sub-op, slot — op-specific
  int32_t arg2;
} VbCommand;

static inline const char *vb_err_str(VbErr e) {
  switch (e) {
    case VB_OK:         return "ok";
    case VB_EBADCH:     return "badch";
    case VB_ERANGE:     return "range";
    case VB_ECLAMPED:   return "clamped";
    case VB_EBUSY:      return "busy";
    case VB_ETRIP:      return "trip";
    case VB_EINTERLOCK: return "interlock";
    case VB_EQFULL:     return "qfull";
    case VB_EUNKNOWN:   return "unknown";
    case VB_EHW:        return "hw";
  }
  return "?";
}

// ---------------------------------------------------------------------------
//   Number parsing: "5.001" -> 5001 milli-units. Rejects anything it does not
//   fully consume, so "5.0abc" is an error rather than a silent 5.000.
//   Extra decimals past the third are truncated, not rounded — a setpoint that
//   rounds UP on parse is a setpoint that can exceed a ceiling the user typed.
// ---------------------------------------------------------------------------
static inline int vb_parse_milli(const char *s, int32_t *out) {
  if (!s || !*s) return 0;
  int neg = 0;
  if (*s == '+') s++;
  else if (*s == '-') { neg = 1; s++; }
  if (!*s) return 0;

  int32_t whole = 0, frac = 0, fdigits = 0, digits = 0;
  while (*s >= '0' && *s <= '9') {
    if (whole > 2000000) return 0;               // absurd; refuse rather than wrap
    whole = whole * 10 + (*s - '0');
    s++; digits++;
  }
  if (*s == '.') {
    s++;
    while (*s >= '0' && *s <= '9') {
      if (fdigits < 3) { frac = frac * 10 + (*s - '0'); fdigits++; }
      s++; digits++;
    }
  }
  if (!digits || *s) return 0;                   // trailing garbage
  while (fdigits < 3) { frac *= 10; fdigits++; }

  int32_t v = whole * 1000 + frac;
  *out = neg ? -v : v;
  return 1;
}

static inline int vb_parse_int(const char *s, int32_t *out) {
  if (!s || !*s) return 0;
  int neg = 0;
  if (*s == '+') s++;
  else if (*s == '-') { neg = 1; s++; }
  int32_t v = 0, digits = 0;
  while (*s >= '0' && *s <= '9') {
    if (v > 100000000) return 0;
    v = v * 10 + (*s - '0');
    s++; digits++;
  }
  if (!digits || *s) return 0;
  *out = neg ? -v : v;
  return 1;
}

// ---------------------------------------------------------------------------
//   Line parsing
// ---------------------------------------------------------------------------
typedef enum {
  CK_NONE = 0,     // blank line — no reply at all
  CK_APPLY,        // .cmd is ready to hand to apply_command
  CK_IDN, CK_MEAS, CK_STATE, CK_VOLT_Q, CK_CURR_Q, CK_OUT_Q, CK_PROT_Q,
  CK_LOG_Q, CK_LOGDUMP, CK_SEQ_Q, CK_SEQLOAD, CK_PRESET_Q, CK_CAL_Q,
  CK_NET, CK_REBOOT, CK_HELP, CK_STREAM,
  CK_ERR
} CmdKind;

typedef struct {
  CmdKind    kind;
  VbCommand  cmd;        // valid when kind == CK_APPLY
  uint8_t    ch;         // for queries; 0 = unspecified/all
  int32_t    arg;        // for CK_STREAM (hz), CK_PRESET_Q, ...
  const char *err;       // stable token when kind == CK_ERR
} ParsedLine;

static inline char vb__up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static inline int vb__eq(const char *a, const char *b) {
  while (*a && *b) { if (vb__up(*a) != vb__up(*b)) return 0; a++; b++; }
  return !*a && !*b;
}

// Split `s` on commas in place, returning up to `max` field pointers.
static inline int vb__split(char *s, char **f, int max) {
  int n = 0;
  if (!s || !*s) return 0;
  f[n++] = s;
  while (*s && n < max) {
    if (*s == ',') { *s = 0; f[n++] = s + 1; }
    s++;
  }
  return n;
}

static inline void vb__err(ParsedLine *p, const char *tok) {
  p->kind = CK_ERR;
  p->err  = tok;
}

// Parse one line into `out`. `line` is copied into a caller-free 96-byte stack
// buffer, so the caller's buffer is never modified. Case-insensitive; the
// channel is ALWAYS explicit — no stateful channel selection, which is the
// single biggest source of automation bugs in real SCPI gear.
static inline void vb_parse_line(const char *line, uint8_t src, ParsedLine *out) {
  memset(out, 0, sizeof(*out));
  out->kind = CK_NONE;

  char buf[97];
  size_t n = 0;
  while (line[n] && n < 96) { buf[n] = line[n]; n++; }
  if (line[n]) { vb__err(out, "overflow"); return; }     // never a truncated command
  buf[n] = 0;

  // trim
  char *s = buf;
  while (*s == ' ' || *s == '\t') s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
  if (!*s) return;                                        // blank -> CK_NONE

  // verb = up to first space
  char *verb = s;
  char *rest = strchr(s, ' ');
  if (rest) { *rest++ = 0; while (*rest == ' ') rest++; }

  char *f[4] = {0, 0, 0, 0};
  int   nf   = vb__split(rest, f, 4);

  out->cmd.src = src;

  // --- identity / trivia ----------------------------------------------------
  if (vb__eq(verb, "*IDN?"))  { out->kind = CK_IDN;    return; }
  if (vb__eq(verb, "STATE?")) { out->kind = CK_STATE;  return; }
  if (vb__eq(verb, "NET?"))   { out->kind = CK_NET;    return; }
  if (vb__eq(verb, "HELP") || vb__eq(verb, "?")) { out->kind = CK_HELP; return; }
  if (vb__eq(verb, "REBOOT")) { out->kind = CK_REBOOT; return; }

  if (vb__eq(verb, "MEAS?")) {
    out->kind = CK_MEAS;
    if (nf >= 1 && *f[0]) {
      int32_t c;
      if (!vb_parse_int(f[0], &c) || c < 1 || c > 2) { vb__err(out, "badch"); return; }
      out->ch = (uint8_t)c;
    }
    return;
  }

  if (vb__eq(verb, "STREAM")) {
    int32_t hz;
    if (nf < 1 || !vb_parse_int(f[0], &hz) || hz < 0 || hz > 10) {
      vb__err(out, "range"); return;
    }
    out->kind = CK_STREAM; out->arg = hz; return;
  }

  if (vb__eq(verb, "ALLOFF")) {
    out->kind = CK_APPLY; out->cmd.op = OP_ALL_OFF; return;
  }

  // --- <VERB> <ch>,<value> forms -------------------------------------------
  int is_volt  = vb__eq(verb, "VOLT"),  is_voltq = vb__eq(verb, "VOLT?");
  int is_curr  = vb__eq(verb, "CURR"),  is_currq = vb__eq(verb, "CURR?");
  int is_out   = vb__eq(verb, "OUT"),   is_outq  = vb__eq(verb, "OUT?");
  int is_ccm   = vb__eq(verb, "CCMODE");
  int is_protq = vb__eq(verb, "PROT?"), is_calq  = vb__eq(verb, "CAL?");

  if (is_voltq || is_currq || is_outq || is_protq || is_calq) {
    int32_t c;
    if (nf < 1 || !vb_parse_int(f[0], &c) || c < 1 || c > 2) { vb__err(out, "badch"); return; }
    out->ch   = (uint8_t)c;
    out->kind = is_voltq ? CK_VOLT_Q : is_currq ? CK_CURR_Q
              : is_outq  ? CK_OUT_Q  : is_protq ? CK_PROT_Q : CK_CAL_Q;
    return;
  }

  if (is_volt || is_curr || is_out || is_ccm) {
    int32_t c;
    if (nf < 2 || !vb_parse_int(f[0], &c) || c < 1 || c > 2) { vb__err(out, "badch"); return; }
    out->kind   = CK_APPLY;
    out->cmd.ch = (uint8_t)c;

    if (is_volt) {
      if (!vb_parse_milli(f[1], &out->cmd.arg)) { vb__err(out, "range"); return; }
      out->cmd.op = OP_SET_V;
    } else if (is_curr) {
      if (!vb_parse_milli(f[1], &out->cmd.arg)) { vb__err(out, "range"); return; }
      out->cmd.op = OP_SET_I;
    } else if (is_out) {
      int32_t v;
      if (!vb_parse_int(f[1], &v) || (v != 0 && v != 1)) { vb__err(out, "range"); return; }
      out->cmd.op = OP_ENABLE; out->cmd.arg = v;
    } else {
      if      (vb__eq(f[1], "LIMIT")) out->cmd.arg = CCM_LIMIT_VAL;
      else if (vb__eq(f[1], "TRIP"))  out->cmd.arg = CCM_TRIP_VAL;
      else { vb__err(out, "range"); return; }
      out->cmd.op = OP_CC_MODE;
    }
    return;
  }

  // --- PROT:* ---------------------------------------------------------------
  if (vb__eq(verb, "PROT:CLR")) {
    int32_t c;
    if (nf < 1 || !vb_parse_int(f[0], &c) || c < 1 || c > 2) { vb__err(out, "badch"); return; }
    out->kind = CK_APPLY; out->cmd.op = OP_CLEAR_TRIP; out->cmd.ch = (uint8_t)c;
    return;
  }
  if (vb__eq(verb, "PROT:OVP") || vb__eq(verb, "PROT:OCP")) {
    int32_t c;
    if (nf < 2 || !vb_parse_int(f[0], &c) || c < 1 || c > 2) { vb__err(out, "badch"); return; }
    if (!vb_parse_milli(f[1], &out->cmd.arg)) { vb__err(out, "range"); return; }
    out->kind = CK_APPLY;
    out->cmd.op = vb__eq(verb, "PROT:OVP") ? OP_SET_OVP : OP_SET_OCP;
    out->cmd.ch = (uint8_t)c;
    return;
  }
  if (vb__eq(verb, "PROT:OTP")) {
    int32_t t;
    if (nf < 1 || !vb_parse_int(f[0], &t) || t < 40 || t > 100) { vb__err(out, "range"); return; }
    out->kind = CK_APPLY; out->cmd.op = OP_SET_OTP; out->cmd.arg = t;
    return;
  }

  // --- LOG:* ----------------------------------------------------------------
  if (vb__eq(verb, "LOG?"))     { out->kind = CK_LOG_Q;   return; }
  if (vb__eq(verb, "LOG:DUMP")) { out->kind = CK_LOGDUMP; return; }
  if (vb__eq(verb, "LOG:RATE")) {
    int32_t hz10;
    if (nf < 1 || !vb_parse_milli(f[0], &hz10)) { vb__err(out, "range"); return; }
    hz10 /= 100;                                     // milli-Hz -> tenths of Hz
    if (hz10 < 1 || hz10 > 100) { vb__err(out, "range"); return; }
    out->kind = CK_APPLY; out->cmd.op = OP_LOG;
    out->cmd.arg = LOGOP_RATE; out->cmd.arg2 = hz10;
    return;
  }
  if (vb__eq(verb, "LOG")) {
    if (nf < 1) { vb__err(out, "range"); return; }
    int32_t sub = vb__eq(f[0], "START") ? LOGOP_START
                : vb__eq(f[0], "STOP")  ? LOGOP_STOP
                : vb__eq(f[0], "CLEAR") ? LOGOP_CLEAR : -1;
    if (sub < 0) { vb__err(out, "range"); return; }
    out->kind = CK_APPLY; out->cmd.op = OP_LOG; out->cmd.arg = sub;
    return;
  }

  // --- SEQ:* ----------------------------------------------------------------
  if (vb__eq(verb, "SEQ?"))     { out->kind = CK_SEQ_Q;   return; }
  if (vb__eq(verb, "SEQ:LOAD")) { out->kind = CK_SEQLOAD; return; }
  if (vb__eq(verb, "SEQ")) {
    if (nf < 1) { vb__err(out, "range"); return; }
    int32_t sub = vb__eq(f[0], "RUN")    ? SEQOP_RUN
                : vb__eq(f[0], "STOP")   ? SEQOP_STOP
                : vb__eq(f[0], "PAUSE")  ? SEQOP_PAUSE
                : vb__eq(f[0], "RESUME") ? SEQOP_RESUME : -1;
    if (sub < 0) { vb__err(out, "range"); return; }
    out->kind = CK_APPLY; out->cmd.op = OP_SEQ; out->cmd.arg = sub;
    return;
  }

  // --- PRESET:* -------------------------------------------------------------
  if (vb__eq(verb, "PRESET?")) { out->kind = CK_PRESET_Q; return; }
  if (vb__eq(verb, "PRESET:SAVE") || vb__eq(verb, "PRESET:LOAD")) {
    int32_t slot, c;
    if (nf < 2 || !vb_parse_int(f[0], &slot) || slot < 0 || slot > 7) {
      vb__err(out, "range"); return;
    }
    if (!vb_parse_int(f[1], &c) || c < 1 || c > 2) { vb__err(out, "badch"); return; }
    out->kind   = CK_APPLY;
    out->cmd.op = vb__eq(verb, "PRESET:SAVE") ? OP_PRESET_SAVE : OP_PRESET_LOAD;
    out->cmd.ch = (uint8_t)c;
    out->cmd.arg = slot;
    return;
  }

  // --- CAL:* ----------------------------------------------------------------
  if (vb__eq(verb, "CAL:AUTO")) {
    int32_t c;
    if (nf < 1 || !vb_parse_int(f[0], &c) || c < 1 || c > 2) { vb__err(out, "badch"); return; }
    out->kind = CK_APPLY; out->cmd.op = OP_CAL_AUTO; out->cmd.ch = (uint8_t)c;
    return;
  }
  if (vb__eq(verb, "CAL:PT")) {
    int32_t c, meas;
    if (nf < 3 || !vb_parse_int(f[0], &c) || c < 1 || c > 2) { vb__err(out, "badch"); return; }
    int32_t which = vb__eq(f[1], "CV") ? CALPT_CV : vb__eq(f[1], "CC") ? CALPT_CC : -1;
    if (which < 0 || !vb_parse_milli(f[2], &meas)) { vb__err(out, "range"); return; }
    out->kind = CK_APPLY; out->cmd.op = OP_CAL_PT; out->cmd.ch = (uint8_t)c;
    out->cmd.arg = which; out->cmd.arg2 = meas;
    return;
  }

  vb__err(out, "unknown");
}
