// =============================================================================
//   vb_proto.h — pure, Arduino-free parsing for the console and OTA.
//   Host-tested by tools/host_test/test_vb_proto.cpp (g++ -Werror): keep it
//   free of Arduino.h / String / FreeRTOS or that test stops compiling.
//
//   Integer-only on purpose: `3.3` must be 3300 mV, never float-truncated 3299.
// =============================================================================
#pragma once
#include <stdint.h>

static inline const char *vp_skip_blank(const char *s) {
  while (*s == ' ' || *s == '\t') s++;
  return s;
}
static inline char vp_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

// Voltage argument -> millivolts. Accepts "13.5", "13.5v", "13500mv" (suffix
// case-insensitive) with surrounding blanks. Rejects empty, signs, exponents,
// trailing junk and overflow. Range checking is the caller's job (setVoltage).
static inline bool vp_parse_mv(const char *s, uint32_t *mv) {
  if (!s || !mv) return false;
  s = vp_skip_blank(s);
  uint64_t ip = 0, frac = 0, scale = 1;
  int nd = 0, fd = 0;
  while (*s >= '0' && *s <= '9') {
    ip = ip * 10 + (uint64_t)(*s - '0'); s++; nd++;
    if (ip > 4000000000ull) return false;
  }
  if (*s == '.') {
    s++;
    while (*s >= '0' && *s <= '9') {
      if (fd < 6) { frac = frac * 10 + (uint64_t)(*s - '0'); scale *= 10; }
      fd++; s++;
    }
  }
  if (nd + fd == 0) return false;
  bool milli = false;
  if (vp_lower(s[0]) == 'm' && vp_lower(s[1]) == 'v') { milli = true; s += 2; }
  else if (vp_lower(s[0]) == 'v')                     { s += 1; }
  if (*vp_skip_blank(s)) return false;

  uint64_t out = milli ? ip + ((frac * 2 >= scale) ? 1 : 0)
                       : ip * 1000 + (frac * 1000 + scale / 2) / scale;
  if (out > 0xFFFFFFFFull) return false;
  *mv = (uint32_t)out;
  return true;
}

// Plain unsigned decimal with surrounding blanks; nothing else.
static inline bool vp_parse_u32(const char *s, uint32_t *out) {
  if (!s || !out) return false;
  s = vp_skip_blank(s);
  uint64_t v = 0; int nd = 0;
  while (*s >= '0' && *s <= '9') {
    v = v * 10 + (uint64_t)(*s - '0'); s++; nd++;
    if (v > 0xFFFFFFFFull) return false;
  }
  if (!nd || *vp_skip_blank(s)) return false;
  *out = (uint32_t)v;
  return true;
}

// "X.Y.Z" (surrounding whitespace incl. CR/LF allowed) -> parts. Strict otherwise.
static inline bool vp_semver_parse(const char *s, uint32_t p[3]) {
  if (!s) return false;
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  for (int i = 0; i < 3; i++) {
    uint64_t v = 0; int nd = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint64_t)(*s - '0'); s++; nd++; if (v > 999999) return false; }
    if (!nd) return false;
    p[i] = (uint32_t)v;
    if (i < 2) { if (*s != '.') return false; s++; }
  }
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  return *s == 0;
}

// True only if `remote` is strictly newer than `local`. Anything unparseable on
// either side is "not newer": a garbled version.txt must never trigger a flash.
// (The old `!=` test downgraded any device running ahead of the GitHub release.)
static inline bool vp_semver_newer(const char *remote, const char *local) {
  uint32_t r[3], l[3];
  if (!vp_semver_parse(remote, r) || !vp_semver_parse(local, l)) return false;
  for (int i = 0; i < 3; i++) if (r[i] != l[i]) return r[i] > l[i];
  return false;
}
