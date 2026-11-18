// =============================================================================
//   vb_log.h — PURE: the data-log ring, its 12-byte record, and the CSV line
//   formatter (spec §3.7).
//
//   THE LOAD-BEARING RULE: never build the CSV into a String. 2048 records is
//   ~107 KB of text, and String's doubling reallocation makes that an instant
//   OOM on a device with ~150 KB of heap. Every line is snprintf'd into a
//   caller-supplied buffer, always.
//
//   Power is derived at CSV time, never stored — it is exactly v*i and storing
//   it would cost 33% more RAM for zero information.
// =============================================================================
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define LOGREC_EN      0x1000u
#define LOGREC_CC      0x2000u
#define LOGREC_TRIP    0x4000u
#define LOGREC_IMASK   0x0FFFu

typedef struct {          // exactly 12 bytes
  uint32_t t_ms;
  uint16_t v[2];          // mV
  uint16_t i[2];          // low 12 bits mA, then en/cc/tripped flags
} LogRec;

typedef struct {
  LogRec  *buf;
  uint16_t cap;
  uint32_t head;          // monotonic write counter; index = head % cap
  uint32_t seq;           // monotonic row id, for gap-fill merging (spec §9.2)
  uint8_t  running;
  uint8_t  paused;        // held for a tear-free CSV snapshot
  uint16_t rate_hz10;     // tenths of Hz; 10 = 1 Hz
  uint32_t last_ms;
} LogRing;

static inline void log_init(LogRing *L, LogRec *storage, uint16_t cap) {
  memset(L, 0, sizeof(*L));
  L->buf       = storage;
  L->cap       = cap;
  L->rate_hz10 = 10;      // 1 Hz default -> 34 minutes at 2048 records
}

static inline uint16_t log_pack_i(int32_t mA, int en, int cc, int tripped) {
  if (mA < 0) mA = 0;
  if (mA > (int32_t)LOGREC_IMASK) mA = LOGREC_IMASK;
  uint16_t w = (uint16_t)mA;
  if (en)      w |= LOGREC_EN;
  if (cc)      w |= LOGREC_CC;
  if (tripped) w |= LOGREC_TRIP;
  return w;
}

static inline uint32_t log_count(const LogRing *L) {
  return L->head < L->cap ? L->head : L->cap;
}

// Oldest surviving record index, in monotonic head-space.
static inline uint32_t log_first(const LogRing *L) {
  return L->head > L->cap ? L->head - L->cap : 0;
}

static inline void log_clear(LogRing *L) { L->head = 0; L->seq = 0; }

// `force` bypasses both the rate limiter and the running flag. That is how the
// trip point is guaranteed to appear in the CSV — far cheaper than keeping a
// pre-trigger ring, and the one record you always want.
static inline int log_append(LogRing *L, uint32_t t_ms,
                             const uint16_t v[2], const uint16_t i[2], int force) {
  if (!force) {
    if (!L->running || L->paused) return 0;
    uint32_t period = L->rate_hz10 ? (10000u / L->rate_hz10) : 1000u;
    if (L->head && (t_ms - L->last_ms) < period) return 0;
  }
  LogRec *r = &L->buf[L->head % L->cap];
  r->t_ms = t_ms;
  r->v[0] = v[0]; r->v[1] = v[1];
  r->i[0] = i[0]; r->i[1] = i[1];
  L->head++;
  L->seq++;
  L->last_ms = t_ms;
  return 1;
}

static inline const LogRec *log_at(const LogRing *L, uint32_t idx) {
  if (idx < log_first(L) || idx >= L->head) return 0;
  return &L->buf[idx % L->cap];
}

static inline int log_csv_header(char *out, int cap) {
  return snprintf(out, (size_t)cap,
                  "seq,t_ms,v1_V,i1_A,w1_W,en1,cc1,trip1,v2_V,i2_A,w2_W,en2,cc2,trip2\n");
}

// One CSV row into a caller buffer. Returns the length written, or 0 if idx has
// already been overwritten by the ring.
static inline int log_csv_row(const LogRing *L, uint32_t idx, char *out, int cap) {
  const LogRec *r = log_at(L, idx);
  if (!r) return 0;

  int32_t  mA[2]; uint16_t fl[2];
  for (int k = 0; k < 2; k++) {
    mA[k] = r->i[k] & LOGREC_IMASK;
    fl[k] = r->i[k];
  }
  // Power derived here, never stored. mV*mA/1000 = uW; print as W.
  int32_t uW0 = (int32_t)((int64_t)r->v[0] * mA[0] / 1000);
  int32_t uW1 = (int32_t)((int64_t)r->v[1] * mA[1] / 1000);

  return snprintf(out, (size_t)cap,
    "%lu,%lu,%ld.%03ld,%ld.%03ld,%ld.%03ld,%d,%d,%d,"
    "%ld.%03ld,%ld.%03ld,%ld.%03ld,%d,%d,%d\n",
    (unsigned long)idx, (unsigned long)r->t_ms,
    (long)(r->v[0] / 1000), (long)(r->v[0] % 1000),
    (long)(mA[0]   / 1000), (long)(mA[0]   % 1000),
    (long)(uW0     / 1000), (long)(uW0     % 1000),
    !!(fl[0] & LOGREC_EN), !!(fl[0] & LOGREC_CC), !!(fl[0] & LOGREC_TRIP),
    (long)(r->v[1] / 1000), (long)(r->v[1] % 1000),
    (long)(mA[1]   / 1000), (long)(mA[1]   % 1000),
    (long)(uW1     / 1000), (long)(uW1     % 1000),
    !!(fl[1] & LOGREC_EN), !!(fl[1] & LOGREC_CC), !!(fl[1] & LOGREC_TRIP));
}
