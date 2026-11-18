#include "vb_console.h"
#include "vb_state.h"
#include "vb_cmd.h"
#include "vb_log.h"
#include "vb_protect.h"
#include "vb_seq.h"
#include "vb_control.h"
#include "vb_nvs.h"
#include "vb_net.h"
#include "vb_debug.h"
#include <WiFi.h>

static WiFiServer telnetSrv(23);
static WiFiClient telnetCli;
static bool       telnetAuthed = false;
static uint32_t   g_telnetRead = 0;
static uint32_t   g_wsLogRead  = 0;

static uint8_t    s_stream_hz  = 0;     // telemetry OFF by default, so plugging
                                        // into PuTTY gives a readable console
                                        // rather than 4 Hz of JSON

// -----------------------------------------------------------------------------
//   snprintf-built replies. NEVER ArduinoJson here — see the header.
// -----------------------------------------------------------------------------
static int fmt_state_json(char *o, int cap, const VbSnapshot *s) {
  int n = snprintf(o, (size_t)cap,
    "{\"ts\":%lu,\"up\":%lu,\"api\":%d,\"ch\":[",
    (unsigned long)s->ts_ms, (unsigned long)s->uptime_s, VB_API_VERSION);

  for (int k = 0; k < VB_CHANNELS && n < cap; k++) {
    const VbChanSnap *c = &s->ch[k];
    n += snprintf(o + n, (size_t)(cap - n),
      "%s{\"en\":%s,\"v\":%ld.%03ld,\"i\":%ld.%03ld,\"w\":%ld.%03ld,"
      "\"vset\":%ld.%03ld,\"iset\":%ld.%03ld,\"mode\":\"%s\",\"prot\":\"%s\","
      "\"reason\":\"%s\",\"cc_mode\":\"%s\",\"ovp\":%ld.%03ld,\"ocp\":%ld.%03ld}",
      k ? "," : "",
      c->en ? "true" : "false",
      (long)(c->v_mV / 1000),    (long)(c->v_mV % 1000),
      (long)(c->i_mA / 1000),    (long)(c->i_mA % 1000),
      (long)(c->w_mW / 1000),    (long)(c->w_mW % 1000),
      (long)(c->vset_mV / 1000), (long)(c->vset_mV % 1000),
      (long)(c->iset_mA / 1000), (long)(c->iset_mA % 1000),
      c->state == PS_CC ? "cc" : "cv",
      c->state == PS_TRIPPED ? "trip" : "ok",
      prot_reason_str((ProtReason)c->reason),
      c->cc_mode == CCM_TRIP ? "trip" : "limit",
      (long)(c->ovp_mV / 1000), (long)(c->ovp_mV % 1000),
      (long)(c->ocp_mA / 1000), (long)(c->ocp_mA % 1000));
  }

  if (n < cap)
    n += snprintf(o + n, (size_t)(cap - n),
      "],\"temp\":%ld.%01ld,\"temp_src\":\"mcp9700\","
      "\"seq\":{\"state\":\"%s\",\"step\":%u,\"n\":%u,\"el\":%lu},"
      "\"log\":{\"on\":%s,\"n\":%lu,\"cap\":%u},\"cal\":[%s,%s]}",
      (long)(s->temp_c10 / 10), (long)(s->temp_c10 % 10),
      seq_state_str((SeqState)s->seq_state), s->seq_pc, s->seq_n,
      (unsigned long)s->seq_el_ms,
      s->log_on ? "true" : "false", (unsigned long)s->log_n, s->log_cap,
      s->ch[0].cal_valid ? "true" : "false",
      s->ch[1].cal_valid ? "true" : "false");
  return n;
}

// -----------------------------------------------------------------------------
//   One line in, replies out. Shared verbatim by USB and telnet.
// -----------------------------------------------------------------------------
static void console_exec(const char *line, uint8_t src, Print &out) {
  ParsedLine p;
  vb_parse_line(line, src, &p);

  if (p.kind == CK_NONE) return;
  if (p.kind == CK_ERR)  { out.printf("ERR %s\n", p.err); return; }

  char buf[640];
  VbSnapshot s;

  switch (p.kind) {
    case CK_APPLY: {
      VbErr e = apply_command(&p.cmd);
      if (e == VB_OK || e == VB_ECLAMPED) out.printf("OK%s\n", e == VB_ECLAMPED ? " clamped" : "");
      else out.printf("ERR %s\n", vb_err_str(e));
      break;
    }

    case CK_IDN:
      out.printf("! READY Voltbench,%s,proto %d,ch %d\n",
                 CURRENT_FIRMWARE_VERSION, VB_PROTO_VERSION, VB_CHANNELS);
      break;

    case CK_STATE:
      vb_snapshot(&s);
      fmt_state_json(buf, sizeof(buf), &s);
      out.printf("OK %s\n", buf);
      break;

    case CK_MEAS: {
      // ONE reply per command. An explicit channel answers with a single OK;
      // "all channels" answers with '>' rows and ONE terminating OK. Emitting
      // an OK per channel would let a host consider the reply complete after
      // the first, and every later exchange would shift by one line.
      vb_snapshot(&s);
      for (int k = 0; k < VB_CHANNELS; k++) {
        if (p.ch && p.ch != k + 1) continue;
        const VbChanSnap *c = &s.ch[k];
        const char *mode = c->state == PS_CC ? "CC"
                         : c->state == PS_TRIPPED ? "TRIP" : "CV";
        if (p.ch)
          out.printf("OK %ld.%03ld,%ld.%03ld,%ld.%03ld,%s\n",
            (long)(c->v_mV / 1000), (long)(c->v_mV % 1000),
            (long)(c->i_mA / 1000), (long)(c->i_mA % 1000),
            (long)(c->w_mW / 1000), (long)(c->w_mW % 1000), mode);
        else
          out.printf("> %d,%ld.%03ld,%ld.%03ld,%ld.%03ld,%s\n", k + 1,
            (long)(c->v_mV / 1000), (long)(c->v_mV % 1000),
            (long)(c->i_mA / 1000), (long)(c->i_mA % 1000),
            (long)(c->w_mW / 1000), (long)(c->w_mW % 1000), mode);
      }
      if (!p.ch) out.println("OK");
      break;
    }

    case CK_VOLT_Q:
      vb_snapshot(&s);
      out.printf("OK %ld.%03ld\n", (long)(s.ch[p.ch - 1].vset_mV / 1000),
                                   (long)(s.ch[p.ch - 1].vset_mV % 1000));
      break;

    case CK_CURR_Q:
      vb_snapshot(&s);
      out.printf("OK %ld.%03ld\n", (long)(s.ch[p.ch - 1].iset_mA / 1000),
                                   (long)(s.ch[p.ch - 1].iset_mA % 1000));
      break;

    case CK_OUT_Q:
      vb_snapshot(&s);
      out.printf("OK %d\n", s.ch[p.ch - 1].en ? 1 : 0);
      break;

    case CK_PROT_Q:
      vb_snapshot(&s);
      out.printf("OK %s,%s,%ld.%03ld,%ld.%03ld,%ld\n",
        prot_state_str((ProtState)s.ch[p.ch - 1].state),
        prot_reason_str((ProtReason)s.ch[p.ch - 1].reason),
        (long)(s.ch[p.ch - 1].ovp_mV / 1000), (long)(s.ch[p.ch - 1].ovp_mV % 1000),
        (long)(s.ch[p.ch - 1].ocp_mA / 1000), (long)(s.ch[p.ch - 1].ocp_mA % 1000),
        (long)g_otp_c);
      break;

    case CK_CAL_Q:
      out.printf("OK %d,%.1f,%.1f,%.4f,%.1f\n",
                 g_cal_valid[p.ch - 1] ? 1 : 0,
                 g_cal[p.ch - 1].cv_v0_mV, g_cal[p.ch - 1].cv_slope_uV,
                 g_cal[p.ch - 1].cc_gain,  g_cal[p.ch - 1].cc_off_mA);
      break;

    case CK_LOG_Q:
      vb_snapshot(&s);
      out.printf("OK %s,%lu,%u,%u\n", s.log_on ? "on" : "off",
                 (unsigned long)s.log_n, s.log_cap, g_logring.rate_hz10);
      break;

    case CK_LOGDUMP: {
      // Multi-line payload, one '>' row at a time into a stack buffer. Never a
      // String: 2048 rows is ~107 KB and String's doubling realloc is an
      // instant OOM.
      //
      // The terminating OK goes LAST. Leading with it would let a host treat
      // the reply as finished and read the payload rows as the next command's
      // answer.
      char row[160];
      log_csv_header(row, sizeof(row));
      out.printf("> %s", row);
      for (uint32_t idx = log_first(&g_logring); idx < g_logring.head; idx++) {
        if (log_csv_row(&g_logring, idx, row, sizeof(row)) > 0) out.printf("> %s", row);
        if ((idx & 0x0F) == 0) vTaskDelay(1);   // never starve AsyncTCP
      }
      out.printf("OK %lu\n", (unsigned long)log_count(&g_logring));
      break;
    }

    case CK_SEQ_Q:
      vb_snapshot(&s);
      out.printf("OK %s,%u,%u,%lu\n", seq_state_str((SeqState)s.seq_state),
                 s.seq_pc, s.seq_n, (unsigned long)s.seq_el_ms);
      break;

    case CK_PRESET_Q:
      for (int i = 0; i < PRESET_SLOTS; i++)
        if (g_presets[i].used)
          out.printf("> %d,%s,%ld.%03ld,%ld.%03ld\n", i, g_presets[i].name,
                     (long)(g_presets[i].v_mV / 1000), (long)(g_presets[i].v_mV % 1000),
                     (long)(g_presets[i].i_mA / 1000), (long)(g_presets[i].i_mA % 1000));
      out.println("OK");
      break;

    case CK_NET:
      out.printf("OK %s,%s,%d\n",
                 WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "-",
                 WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "0.0.0.0",
                 (int)WiFi.RSSI());
      break;

    case CK_STREAM:
      s_stream_hz = (uint8_t)p.arg;
      out.println("OK");
      break;

    case CK_SEQLOAD:
      out.println("ERR unsupported use /api/seq");
      break;

    case CK_REBOOT:
      out.println("OK rebooting");
      delay(100);
      ESP.restart();
      break;

    case CK_HELP:
      out.println("> VOLT <ch>,<v>  CURR <ch>,<a>  OUT <ch>,<0|1>  ALLOFF");
      out.println("> VOLT? CURR? OUT? MEAS? [ch]  STATE?  *IDN?  NET?");
      out.println("> CCMODE <ch>,<LIMIT|TRIP>  PROT? <ch>  PROT:CLR <ch>");
      out.println("> PROT:OVP <ch>,<v>  PROT:OCP <ch>,<a>  PROT:OTP <c>");
      out.println("> LOG <START|STOP|CLEAR>  LOG:RATE <hz>  LOG?  LOG:DUMP");
      out.println("> SEQ <RUN|STOP|PAUSE|RESUME>  SEQ?  PRESET?");
      out.println("> PRESET:SAVE <slot>,<ch>  PRESET:LOAD <slot>,<ch>");
      out.println("> CAL:AUTO <ch>  CAL:PT <ch>,<CV|CC>,<meas>  CAL? <ch>");
      out.println("> STREAM <hz>  REBOOT  HELP");
      out.println("OK");
      break;

    default:
      out.println("ERR unknown");
      break;
  }
}

// -----------------------------------------------------------------------------
//   con task — USB CDC only. Priority 2, 10 ms.
// -----------------------------------------------------------------------------
void vb_console_task(void *) {
  static char line[CON_LINE_MAX + 2];
  size_t      n = 0;
  bool        overflowed = false;
  uint32_t    t_stream = 0;

  // The banner lets a host detect a mid-stream reboot, flush its RX buffer, and
  // fail pending requests instead of hanging.
  Serial.printf("! READY Voltbench,%s,proto %d,ch %d\n",
                CURRENT_FIRMWARE_VERSION, VB_PROTO_VERSION, VB_CHANNELS);

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(CON_TICK_MS));

    while (Serial.available()) {
      char ch = (char)Serial.read();
      if (ch == '\n' || ch == '\r') {
        if (overflowed) { Serial.println("ERR overflow"); overflowed = false; n = 0; continue; }
        line[n] = 0;
        if (n) console_exec(line, SRC_SERIAL, Serial);
        n = 0;
      } else if (n < CON_LINE_MAX) {
        line[n++] = ch;
      } else {
        // Discard to the next newline. NEVER execute a truncated command — a
        // clipped "VOLT 1,20.0" is a different and possibly dangerous request.
        overflowed = true;
      }
    }

    if (s_stream_hz) {
      uint32_t period = 1000u / s_stream_hz;
      if (millis() - t_stream >= period) {
        t_stream = millis();
        VbSnapshot s; vb_snapshot(&s);
        char buf[640];
        fmt_state_json(buf, sizeof(buf), &s);
        Serial.printf("! %s\n", buf);
      }
    }
  }
}

// -----------------------------------------------------------------------------
//   telnet + /logws drain — from loop(), because AsyncTCP is single-task.
// -----------------------------------------------------------------------------
void vb_console_begin() {
  telnetSrv.begin();
  telnetSrv.setNoDelay(true);
}

void vb_console_pump() {
  if (telnetSrv.hasClient()) {
    if (telnetCli && telnetCli.connected()) { telnetCli.println("! superseded"); telnetCli.stop(); }
    telnetCli = telnetSrv.available();
    telnetCli.setNoDelay(true);
    telnetAuthed = !vb.require_login;
    g_telnetRead = (g_logw > (uint32_t)LOGN) ? g_logw - LOGN : 0;
    telnetCli.printf("! READY Voltbench,%s,proto %d,ch %d\n",
                     CURRENT_FIRMWARE_VERSION, VB_PROTO_VERSION, VB_CHANNELS);
    if (!telnetAuthed) telnetCli.println("! password required");
  }

  if (telnetCli && telnetCli.connected()) {
    // Budget the flush to the socket's free space — a stalled peer must never
    // block loop(). Same failure class as the WS queue backlog.
    int budget = telnetCli.availableForWrite();
    uint32_t w = g_logw;
    if (w - g_telnetRead > (uint32_t)LOGN) g_telnetRead = w - LOGN;
    while (g_telnetRead < w && budget-- > 0) {
      telnetCli.write((uint8_t)g_log[g_telnetRead % LOGN]); g_telnetRead++;
    }

    static char tin[CON_LINE_MAX + 2];
    static size_t tn = 0;
    while (telnetCli.available()) {
      char ch = (char)telnetCli.read();
      if (ch == '\n' || ch == '\r') {
        tin[tn] = 0;
        if (tn) {
          if (!telnetAuthed) {
            telnetAuthed = (vb.password == tin);
            telnetCli.println(telnetAuthed ? "OK" : "ERR auth");
          } else console_exec(tin, SRC_TELNET, telnetCli);
        }
        tn = 0;
      } else if (tn < CON_LINE_MAX) tin[tn++] = ch;
    }
  }

  // Browser log WebSocket (view-only).
  if (logws.count()) {
    uint32_t w = g_logw;
    if (w - g_wsLogRead > (uint32_t)LOGN) g_wsLogRead = w - LOGN;
    if (w > g_wsLogRead && logws.availableForWriteAll()) {
      String s; s.reserve(w - g_wsLogRead);
      while (g_wsLogRead < w) { s += g_log[g_wsLogRead % LOGN]; g_wsLogRead++; }
      logws.textAll(s);
    }
  } else {
    g_wsLogRead = g_logw;
  }
}
