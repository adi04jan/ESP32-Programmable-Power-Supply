#include "vb_console.h"
#include "vb_control.h"
#include "vb_debug.h"
#include "vb_net.h"          // logws
#include "vb_nvs.h"          // vb.require_login / vb.password / vb.restore
#include "vb_proto.h"        // vp_parse_mv / vp_parse_u32 (host-tested)
#include "vb_state.h"
#include <WiFi.h>

// Telnet (:23) — live log stream + password-gated console.
static WiFiServer telnetSrv(23);
static WiFiClient telnetCli;          // the active session
static bool       telnetAuthed = false;
static WiFiClient telnetNew;          // newcomer still typing its password
static String     tin, tnew;          // line buffers for the two
static uint32_t   g_telnetRead = 0;   // ring cursor for telnet client
static uint32_t   g_wsLogRead  = 0;   // ring cursor for browser log WS
static bool       g_quiet      = false;   // `quiet 1`: no periodic [tel] line

void vb_console_begin() {
  telnetSrv.begin();
  telnetSrv.setNoDelay(true);
}

// USB replies must arrive. Dbg truncates to the CDC ring's free space and drops
// at 0 (the HWCDC-wedge guard), and between host sessions the ring fills with
// stale log lines — so `?` "only sometimes" answered. This writer keeps the
// wedge rule (never more than availableForWrite() per call) but waits for room,
// at most 100 ms per reply, instead of dropping.
class UsbReply : public Print {
 public:
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *b, size_t n) override {
    if (!Serial) return n;                        // no host session: nothing to deliver
    uint32_t t0 = millis();
    size_t done = 0;
    while (done < n) {
      int a = Serial.availableForWrite();
      if (a > 0) {
        size_t k = n - done < (size_t)a ? n - done : (size_t)a;
        Serial.write(b + done, k);
        done += k;
      } else if (millis() - t0 > 100) {
        break;                                    // host not reading: give up, never stall loop()
      } else {
        delay(1);
      }
    }
    return n;
  }
};
static UsbReply UsbOut;

static const char HELP[] =
  "OK cmds: ? s id v<volts|mV> o<1-3> <0|1> o0 o1 off cycle <ch> <sec> "
  "log? quiet <0|1> restore <0|1|?> help";

static void reply_outputs(Print &out, const char *prefix) {
  out.printf("OK %sout=%d,%d,%d\n", prefix, psState.output1, psState.output2, psState.output3);
}

// Execute one bench-console command, replying on `out`. Shared by USB + telnet.
// Protocol: every command gets exactly ONE line starting "OK" or "ERR <code>"
// (log? sends "> " rows first, then its OK line). Whole tokens only: the old
// first-character match ran a calibration sweep on "meas" and switched output 1
// on for "version". Hosts: send "\n", ignore lines not starting OK/ERR.
void console_exec(const String &lineIn, Print &out, uint8_t src) {
  String l = lineIn; l.trim(); l.toLowerCase();
  if (!l.length()) return;
  const char *p = l.c_str();

  if (l == "?") {             // legacy line, fields kept for existing `grep set=` / `out=` parsers
    out.printf("OK set=%umV meas=%umV disp=%umV state=%u R=%dohm cal=%d out=%d%d%d\n",
               g_setpoint_mV, g_measured_mV, g_display_mV, g_vctrl_state, (int)dpCalcR(),
               g_cal_valid ? 1 : 0, psState.output1, psState.output2, psState.output3);
  }
  else if (l == "s") {        // fixed format; rails are measured BEFORE the output switches
    out.printf("OK STAT out=%d,%d,%d set_mv=%u rail1_mv=%u rail2_mv=%u rail3_mv=%u "
               "ctl=%s cal=%d pend=%u restore=%d up_s=%lu\n",
               psState.output1, psState.output2, psState.output3, g_setpoint_mV,
               g_display_mV, g_5v_mV, g_3v3_mV, vb_ctl_name(g_vctrl_state),
               g_cal_valid ? 1 : 0, vb_pending_mask(), vb.restore ? 1 : 0,
               (unsigned long)(millis() / 1000));
  }
  else if (l == "id" || l == "*idn?") {
    out.printf("OK ID voltbench fw=%s mac=%s ip=%s mdns=%s up_s=%lu reset=%s\n",
               CURRENT_FIRMWARE_VERSION, WiFi.macAddress().c_str(),
               WiFi.localIP().toString().c_str(), vb.mdns.c_str(),
               (unsigned long)(millis() / 1000), vb_reset_reason());
  }
  else if (l == "help") out.println(HELP);
  else if (l == "o0" || l == "o1") {                          // legacy: output 1
    apply_command("toggle", 1, l[1] == '1' ? 1.0f : 0.0f, src)
      ? reply_outputs(out, "") : (void)out.println("ERR rejected");
  }
  else if (l.length() == 4 && p[0] == 'o' && p[1] >= '1' && p[1] <= '3' && p[2] == ' ' &&
           (p[3] == '0' || p[3] == '1')) {
    apply_command("toggle", p[1] - '0', p[3] == '1' ? 1.0f : 0.0f, src)
      ? reply_outputs(out, "") : (void)out.println("ERR rejected");
  }
  else if (l == "off") { apply_command("all_off", 0, 0, src); reply_outputs(out, ""); }
  else if (p[0] == 'v' && l.length() > 1) {                  // setpoint ONLY — never switches
    uint32_t mv;
    if (!vp_parse_mv(p + 1, &mv))
      out.println("ERR arg expected v<volts> or v<millivolts>mV, e.g. v13.5 or v13500mV");
    else if (mv < VB_VMIN_MV || mv > VB_VMAX_MV)
      out.printf("ERR range %lu mV outside %u-%u mV\n", (unsigned long)mv, VB_VMIN_MV, VB_VMAX_MV);
    else {
      apply_command("set_voltage", 1, mv / 1000.0f, src);   // float round-trips exactly at <= 16000 mV
      out.printf("OK set_mv=%u out=%d,%d,%d\n", g_target_mV,
                 psState.output1, psState.output2, psState.output3);
    }
  }
  else if (l.startsWith("cycle ")) {
    String a = l.substring(6); a.trim();
    int sp = a.indexOf(' ');
    uint32_t ch = 0, sec = 0;
    if (sp < 0 || !vp_parse_u32(a.substring(0, sp).c_str(), &ch) ||
        !vp_parse_u32(a.substring(sp + 1).c_str(), &sec))
      out.println("ERR arg expected cycle <ch 1-3> <seconds 1-3600>");
    else if (!apply_command("cycle", (uint8_t)(ch > 255 ? 0 : ch), (float)sec, src))
      out.println("ERR range ch 1-3, seconds 1-3600");
    else out.printf("OK cycle ch=%lu off_s=%lu (back on only if the rail checks out)\n",
                    (unsigned long)ch, (unsigned long)sec);
  }
  else if (l == "log?") {
    char buf[96];
    int n = vb_log_count();
    for (int i = 0; i < n; i++) if (vb_log_line(i, buf, sizeof(buf))) out.printf("> %s\n", buf);
    out.printf("OK LOG n=%d\n", n);
  }
  else if (l == "quiet 0" || l == "quiet 1") { g_quiet = l[6] == '1'; out.printf("OK quiet=%d\n", g_quiet); }
  else if (l == "restore 0" || l == "restore 1") {
    vb.restore = l[8] == '1'; vb_save();
    out.printf("OK restore=%d\n", vb.restore ? 1 : 0);
  }
  else if (l == "restore?") {
    out.printf("OK restore=%d saved_mv=%lu saved_out=%d,%d,%d\n", vb.restore ? 1 : 0,
               (unsigned long)vb.r_mv, vb.r_out & 1, (vb.r_out >> 1) & 1, (vb.r_out >> 2) & 1);
  }
  else out.printf("ERR unknown '%s' (send help)\n", l.c_str());
}

// Ring -> telnet, in contiguous chunks bounded by the socket's free space (a
// stalled peer must never block loop(); the old loop sent 1 byte per write).
static void telnet_flush() {
  int budget = telnetCli.availableForWrite();
  uint32_t w = g_logw;
  if (w - g_telnetRead > (uint32_t)LOGN) g_telnetRead = w - LOGN;
  while (budget > 0 && g_telnetRead < w) {
    uint32_t off = g_telnetRead % LOGN;
    uint32_t n   = w - g_telnetRead;
    if (n > (uint32_t)LOGN - off) n = LOGN - off;
    if (n > (uint32_t)budget)     n = budget;
    size_t k = telnetCli.write((const uint8_t *)&g_log[off], n);
    if (!k) break;
    g_telnetRead += k; budget -= (int)k;
  }
}

static void telnet_promote(WiFiClient &c) {
  if (telnetCli && telnetCli.connected()) { telnetCli.println("\n[telnet] superseded"); telnetCli.stop(); }
  telnetCli = c;
  telnetAuthed = true;
  tin = "";
  g_telnetRead = (g_logw > (uint32_t)LOGN) ? g_logw - LOGN : 0;   // begin with recent history
  telnetCli.printf("Voltbench v%s debug. %s\n", CURRENT_FIRMWARE_VERSION, HELP);
}

static void remote_debug_pump() {
  // --- Telnet (:23). A newcomer must authenticate BEFORE it replaces the
  // active session — any TCP connect used to kick the logged-in user. ---
  if (telnetSrv.hasClient()) {
    WiFiClient c = telnetSrv.available();
    c.setNoDelay(true);
    if (!vb.require_login) telnet_promote(c);
    else {
      if (telnetNew) telnetNew.stop();
      telnetNew = c; tnew = "";
      telnetNew.printf("Voltbench v%s debug. password:\n", CURRENT_FIRMWARE_VERSION);
    }
  }
  if (telnetNew && telnetNew.connected()) {
    while (telnetNew.available()) {
      char ch = (char)telnetNew.read();
      if (ch == '\n' || ch == '\r') {
        if (!tnew.length()) continue;
        if (tnew == vb.password) { WiFiClient c = telnetNew; telnetNew = WiFiClient(); telnet_promote(c); }
        else { telnetNew.println("bad password"); telnetNew.stop(); }
        tnew = "";
        break;
      } else if (tnew.length() < 64) tnew += ch;
    }
  }
  if (telnetCli && telnetCli.connected() && telnetAuthed) {
    telnet_flush();
    while (telnetCli.available()) {
      char ch = (char)telnetCli.read();
      if (ch == '\n' || ch == '\r') {
        if (tin.length()) { console_exec(tin, telnetCli, SRC_TEL); tin = ""; }
      } else if (tin.length() < 64) tin += ch;
    }
  }
  // --- Browser log WebSocket (view-only) ---
  if (logws.count()) {
    uint32_t w = g_logw;
    if (w - g_wsLogRead > (uint32_t)LOGN) g_wsLogRead = w - LOGN;
    // Same rule as vb_push_status: never queue onto a backed-up client — the
    // ring cursor just lags (and skips ahead, above) until the client drains.
    if (w > g_wsLogRead && logws.availableForWriteAll()) {
      String s; s.reserve(w - g_wsLogRead);
      while (g_wsLogRead < w) { s += g_log[g_wsLogRead % LOGN]; g_wsLogRead++; }
      logws.textAll(s);
    }
  } else {
    g_wsLogRead = g_logw;     // no viewers -> don't build a backlog
  }
}

// Single pump called from loop(): telnet + browser log WS, the USB bench
// console, and the periodic output-1 telemetry line.
void vb_console_pump() {
  remote_debug_pump();

  // Bench console over USB (same commands as telnet). Non-blocking accumulator
  // (readStringUntil would stall loop() up to 1 s per partially-typed line).
  static String usbin;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (usbin.length()) { console_exec(usbin, UsbOut, SRC_USB); usbin = ""; }
    } else if (usbin.length() < 64) usbin += ch;
  }

  // Periodic telemetry while output 1 is on -> USB, telnet, web log. Tagged
  // [tel], not [con]: it is NOT a command reply (it used to look like one).
  static uint32_t t_con = 0;
  if (!g_quiet && psState.output1 && millis() - t_con > 1000) {
    t_con = millis();
    Dbg.printf("[tel] set=%umV meas=%umV disp=%umV state=%s out=%d%d%d\n",
               g_setpoint_mV, g_measured_mV, g_display_mV, vb_ctl_name(g_vctrl_state),
               psState.output1, psState.output2, psState.output3);
  }
}
