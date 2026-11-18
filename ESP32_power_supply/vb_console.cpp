#include "vb_console.h"
#include "vb_control.h"
#include "vb_debug.h"
#include "vb_net.h"          // logws
#include "vb_nvs.h"          // vb.require_login / vb.password for the telnet gate
#include "vb_state.h"
#include <WiFi.h>

// Telnet (:23) — live log stream + password-gated console.
static WiFiServer telnetSrv(23);
static WiFiClient telnetCli;
static bool       telnetAuthed = false;
static uint32_t   g_telnetRead = 0;   // ring cursor for telnet client
static uint32_t   g_wsLogRead  = 0;   // ring cursor for browser log WS

void vb_console_begin() {
  telnetSrv.begin();
  telnetSrv.setNoDelay(true);
}

// Execute one bench-console command, replying on `out`. Shared by USB + telnet.
void console_exec(const String &lineIn, Print &out) {
  String l = lineIn; l.trim();
  if (!l.length()) return;
  char c = l[0];
  if      (c == 'v') { float v = l.substring(1).toFloat();
                       apply_command("set_voltage", 1, v); setOutput(1, true);
                       out.printf("[con] set=%dmV out1=on\n", (int)(psState.voltage1 * 1000)); }
  else if (l == "o1") { apply_command("toggle", 1, 1.0f); out.println("[con] out1=on"); }
  else if (l == "o0") { apply_command("toggle", 1, 0.0f); out.println("[con] out1=off"); }
  // Fixed 5 V / 3.3 V rails were web/MQTT-only, so a headless host on USB (the
  // build box, Voltbench off-LAN) could not switch them. Exact match only.
  else if (l == "o2 1" || l == "o2 0" || l == "o3 1" || l == "o3 0") {
                       uint8_t ch = l[1] - '0'; bool on = l[3] == '1';
                       apply_command("toggle", ch, on ? 1.0f : 0.0f);
                       out.printf("[con] out%u=%s\n", ch, on ? "on" : "off"); }
  else if (c == 'm')  { g_cal_sweep_req = true; out.println("[con] calibration sweep requested"); }
  else if (c == 'c')  { out.printf("[con] cal valid=%d\n", g_cal_valid ? 1 : 0); }
  // out=<1><2><3> appended last so existing `?` parsers keep working; it is the
  // only way to read the switch states back (the rail ack is otherwise the sole signal).
  else if (c == '?')  { out.printf("[con] set=%umV meas=%umV disp=%umV state=%u R=%dohm cal=%d out=%d%d%d\n",
                          g_setpoint_mV, g_measured_mV, g_display_mV, g_vctrl_state, (int)dpCalcR(), g_cal_valid ? 1 : 0,
                          psState.output1, psState.output2, psState.output3); }
  else out.println("[con] cmds: v<volts> o1 o0 o2|o3 <0|1> m c ?");
}

static void remote_debug_pump() {
  // --- Telnet (:23): live log stream + password-gated console ---
  if (telnetSrv.hasClient()) {
    if (telnetCli && telnetCli.connected()) { telnetCli.println("\n[telnet] superseded"); telnetCli.stop(); }
    telnetCli = telnetSrv.available();
    telnetCli.setNoDelay(true);
    telnetAuthed = !vb.require_login;
    g_telnetRead = (g_logw > (uint32_t)LOGN) ? g_logw - LOGN : 0;   // begin with recent history
    telnetCli.printf("Voltbench v%s debug. ", CURRENT_FIRMWARE_VERSION);
    telnetCli.println(telnetAuthed ? "cmds: v<volts> o1 o0 o2|o3 <0|1> m c ?" : "password:");
  }
  if (telnetCli && telnetCli.connected()) {
    // Budget the flush to the socket's free send space — a stalled peer must
    // never block loop() (same failure class as the WS queue backlog).
    int budget = telnetCli.availableForWrite();
    uint32_t w = g_logw;
    if (w - g_telnetRead > (uint32_t)LOGN) g_telnetRead = w - LOGN;
    while (g_telnetRead < w && budget-- > 0) {
      telnetCli.write((uint8_t)g_log[g_telnetRead % LOGN]); g_telnetRead++;
    }
    static String tin;
    while (telnetCli.available()) {
      char ch = (char)telnetCli.read();
      if (ch == '\n' || ch == '\r') {
        if (tin.length()) {
          if (!telnetAuthed) {
            telnetAuthed = (tin == vb.password);
            telnetCli.println(telnetAuthed ? "ok. cmds: v<volts> o1 o0 o2|o3 <0|1> m c ?" : "bad password");
          } else console_exec(tin, telnetCli);
          tin = "";
        }
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

  // Bench console over USB (same commands as telnet): v<volts> o1 o0 m c ?
  // Non-blocking accumulator (readStringUntil would stall loop() up to 1 s
  // per partially-typed line).
  static String usbin;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (usbin.length()) { console_exec(usbin, Dbg); usbin = ""; }
    } else if (usbin.length() < 64) usbin += ch;
  }

  // Periodic telemetry while output 1 is on -> fans out to USB, telnet, web log.
  static uint32_t t_con = 0;
  if (psState.output1 && millis() - t_con > 1000) {
    t_con = millis();
    Dbg.printf("[con] set=%umV meas=%umV disp=%umV state=%u R=%dohm\n",
               g_setpoint_mV, g_measured_mV, g_display_mV, g_vctrl_state, (int)dpCalcR());
  }
}
