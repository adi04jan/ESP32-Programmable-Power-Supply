// Host self-check for vb_proto.h (console argument parsing + OTA version compare).
// Plain g++, no Arduino:  g++ -std=c++17 -Wall -Wextra -Werror test_vb_proto.cpp
#include <assert.h>
#include <stdio.h>
#include "../../ESP32_power_supply/vb_proto.h"

static uint32_t mv(const char *s) { uint32_t v = 0xDEADBEEF; assert(vp_parse_mv(s, &v)); return v; }
static bool     bad_mv(const char *s) { uint32_t v; return !vp_parse_mv(s, &v); }

int main() {
  // --- voltages: volts by default, optional V / mV suffix, blanks tolerated
  assert(mv("13.5") == 13500);
  assert(mv("13.485") == 13485);
  assert(mv("3.3") == 3300);              // integer maths: 3.3 must not become 3299
  assert(mv(" 5 ") == 5000);
  assert(mv("12V") == 12000);
  assert(mv("12.0v") == 12000);
  assert(mv("13500mV") == 13500);
  assert(mv("13500MV") == 13500);
  assert(mv("2500.4mv") == 2500);         // sub-mV rounds to nearest
  assert(mv("2500.5mv") == 2501);
  assert(mv(".5") == 500);
  assert(mv("16.0004") == 16000);
  // garbage the old first-character parser turned into "0 V, output ON"
  assert(bad_mv("version"));
  assert(bad_mv("erbose"));
  assert(bad_mv(""));
  assert(bad_mv("   "));
  assert(bad_mv("-1"));
  assert(bad_mv("+5"));
  assert(bad_mv("1e3"));
  assert(bad_mv("13.5q"));
  assert(bad_mv("13.5 v x"));
  assert(bad_mv("."));
  assert(bad_mv("mv"));
  assert(bad_mv("99999999999"));          // overflow is rejected, not wrapped
  assert(bad_mv(nullptr));

  // --- unsigned integers (cycle seconds, channel numbers)
  { uint32_t u = 0;
    assert(vp_parse_u32("15", &u) && u == 15);
    assert(vp_parse_u32(" 3600 ", &u) && u == 3600);
    assert(!vp_parse_u32("1.5", &u));
    assert(!vp_parse_u32("", &u));
    assert(!vp_parse_u32("x", &u));
    assert(!vp_parse_u32("99999999999", &u)); }

  // --- semver: only a strictly NEWER remote counts (fixes the OTA downgrade)
  assert( vp_semver_newer("3.1.0", "3.0.1"));
  assert( vp_semver_newer("3.10.0", "3.9.9"));   // numeric, not lexical
  assert( vp_semver_newer("4.0.0", "3.99.99"));
  assert(!vp_semver_newer("3.0.1", "3.1.0"));    // older: never "update"
  assert(!vp_semver_newer("3.1.0", "3.1.0"));
  assert(!vp_semver_newer("", "3.0.1"));
  assert(!vp_semver_newer("garbage", "3.0.1"));
  assert(!vp_semver_newer("3.1", "3.0.1"));      // malformed remote -> no update
  assert(!vp_semver_newer("3.1.0\n<html>", "3.0.1"));
  assert(!vp_semver_newer("3.1.0", "garbage"));  // malformed local -> refuse, don't guess
  assert( vp_semver_newer(" 3.1.0\r\n", "3.0.1"));   // version.txt whitespace is fine

  puts("vb_proto: all checks passed");
  return 0;
}
