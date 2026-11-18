#include "vb_control.h"
#include "vb_state.h"
#include "vb_hal.h"
#include "vb_debug.h"
#include "vb_nvs.h"
#include "vb_ui.h"
#include "vb_cal.h"
#include "vb_protect.h"

volatile bool g_hw_fault = false;

static LogRec  s_logbuf[LOG_RING_RECORDS];   // 24 576 B static — never malloc
LogRing        g_logring;
SeqCtx         g_seqctx;
SeqStep        g_seq[SEQ_MAX_STEPS];

static ProtCtx  s_prot[VB_CHANNELS];
static uint8_t  s_sense_fail[VB_CHANNELS] = { 0, 0 };
static bool     s_dac_ok = false;
static uint8_t  s_cal_load_ch = 0;           // 0 = released
static uint32_t s_cal_load_t0 = 0;

static const uint8_t INA_ADDR[VB_CHANNELS] = { ADDR_INA219_1, ADDR_INA219_2 };
static const uint8_t DAC_CV[VB_CHANNELS]   = { DAC_CH_CV1, DAC_CH_CV2 };
static const uint8_t DAC_CC[VB_CHANNELS]   = { DAC_CH_CC1, DAC_CH_CC2 };

// Last codes we believe are in the DAC, so a readback can be checked against
// intent rather than against "some plausible number".
static uint16_t s_want_cv[VB_CHANNELS] = { CV_CODE_SAFE, CV_CODE_SAFE };
static uint16_t s_want_cc[VB_CHANNELS] = { CC_CODE_SAFE, CC_CODE_SAFE };

// =============================================================================
//   DAC helpers
// =============================================================================
static bool dac_force_safe(uint8_t k) {
  s_want_cv[k] = CV_CODE_SAFE;
  s_want_cc[k] = CC_CODE_SAFE;
  bool ok = dac_write(DAC_CC[k], CC_CODE_SAFE);
  ok = dac_write(DAC_CV[k], CV_CODE_SAFE) && ok;
  return ok;
}

// Write CC first, then CV, ALWAYS. If CV went first, there would be a window
// where a raised voltage sat behind a stale, lower current limit — or worse, a
// raised limit behind a stale voltage.
static bool dac_apply(uint8_t k, uint16_t cv_code, uint16_t cc_code) {
  bool ok = true;
  if (cc_code != s_want_cc[k]) { ok = dac_write(DAC_CC[k], cc_code) && ok; s_want_cc[k] = cc_code; }
  if (cv_code != s_want_cv[k]) { ok = dac_write(DAC_CV[k], cv_code) && ok; s_want_cv[k] = cv_code; }
  return ok;
}

static bool dac_verify() {
  uint16_t dac[4], eep[4];
  if (!dac_readback(dac, eep)) return false;
  for (int k = 0; k < VB_CHANNELS; k++) {
    if (dac[DAC_CV[k]] != s_want_cv[k]) return false;
    if (dac[DAC_CC[k]] != s_want_cc[k]) return false;
  }
  return true;
}

// =============================================================================
//   Boot sequence (spec §3.4). Steps 6 and 10 — the two readbacks — are the
//   difference between "the DAC was probably written" and "the DAC was
//   written", and they are the ones people skip.
// =============================================================================
bool vb_control_boot() {
  log_init(&g_logring, s_logbuf, LOG_RING_RECORDS);
  seq_init(&g_seqctx);
  for (int k = 0; k < VB_CHANNELS; k++) prot_init(&s_prot[k]);

  if (!hal_i2c_begin()) { Dbg.println("FATAL: I2C init failed"); g_hw_fault = true; return false; }

  // 3. Probe. A missing DAC means outputs can NEVER be enabled this boot.
  bool dac_present = hal_probe(ADDR_MCP4728);
  Dbg.printf("I2C: MCP4728@0x60 %s · INA@0x40 %s · INA@0x41 %s · OLED@0x3C %s\n",
             dac_present            ? "ok" : "MISSING",
             hal_probe(ADDR_INA219_1) ? "ok" : "MISSING",
             hal_probe(ADDR_INA219_2) ? "ok" : "MISSING",
             hal_probe(ADDR_SSD1306)  ? "ok" : "missing");
  if (!dac_present) {
    Dbg.println("FATAL: no MCP4728 — outputs disabled for this boot");
    g_hw_fault = true;
    return false;
  }

  // 4. Minimum volts, zero current, before anything else.
  for (int k = 0; k < VB_CHANNELS; k++) dac_force_safe((uint8_t)k);

  // 5. INA219s.
  for (int k = 0; k < VB_CHANNELS; k++)
    if (!ina_begin(INA_ADDR[k])) Dbg.printf("WARN: INA219 ch%d init failed\n", k + 1);

  // 6. FIRST readback. Also check the EEPROM defaults and complain loudly if
  //    they are wrong — a board that powers up at 20 V before the ESP32 runs is
  //    a hardware-level hazard no firmware can cover.
  uint16_t dac[4], eep[4];
  if (!dac_readback(dac, eep)) {
    Dbg.println("FATAL: DAC readback failed");
    g_hw_fault = true;
    return false;
  }
  for (int k = 0; k < VB_CHANNELS; k++) {
    if (dac[DAC_CV[k]] != CV_CODE_SAFE || dac[DAC_CC[k]] != CC_CODE_SAFE) {
      Dbg.printf("FATAL: DAC ch%d readback mismatch (cv=%u cc=%u)\n",
                 k + 1, dac[DAC_CV[k]], dac[DAC_CC[k]]);
      g_hw_fault = true;
      return false;
    }
    if (eep[DAC_CV[k]] != CV_CODE_SAFE || eep[DAC_CC[k]] != CC_CODE_SAFE)
      Dbg.printf("WARN: DAC ch%d EEPROM is NOT safe (cv=%u cc=%u) — "
                 "this board powers up hot. Run CAL:EEPROM.\n",
                 k + 1, eep[DAC_CV[k]], eep[DAC_CC[k]]);
  }

  // 7. NVS (cal, limits, power-on policy) is loaded by the caller before this.
  // 8-10. Setpoints are applied by the first tick, which repeats the readback.
  s_dac_ok = true;
  Dbg.println("Control: DAC verified safe, outputs armed for enable");
  return true;
}

// =============================================================================
//   Self-cal load
// =============================================================================
bool vb_cal_load_engage(uint8_t ch) {
  if (ch < 1 || ch > VB_CHANNELS) return false;
  VbSnapshot s; vb_snapshot(&s);
  // Interlock on MEASURED volts, not commanded — a commanded 2 V that is
  // actually sitting at 18 V because the FB network is broken is exactly the
  // case this protects against.
  if (s.ch[ch - 1].v_mV > CAL_LOAD_MAX_MV) return false;
  s_cal_load_ch = ch;
  s_cal_load_t0 = millis();
  hal_set_cal_load(ch, true);
  return true;
}

void vb_cal_load_release() {
  if (s_cal_load_ch) hal_set_cal_load(s_cal_load_ch, false);
  s_cal_load_ch = 0;
}

// =============================================================================
//   Queued command handling — the ops that need the bus or the ring
// =============================================================================
static void handle_queued(const VbCommand *c) {
  const uint8_t k = c->ch ? (uint8_t)(c->ch - 1) : 0;
  switch (c->op) {
    case OP_CLEAR_TRIP:
      // The FSM decides whether this is allowed; it refuses while the fault
      // condition persists, because a clear that relatches is just noise.
      break;                                  // consumed via s_clear_req below

    case OP_LOG:
      if      (c->arg == LOGOP_START) g_logring.running = 1;
      else if (c->arg == LOGOP_STOP)  g_logring.running = 0;
      else if (c->arg == LOGOP_CLEAR) log_clear(&g_logring);
      else if (c->arg == LOGOP_RATE)  g_logring.rate_hz10 = (uint16_t)c->arg2;
      break;

    case OP_SEQ:
      if      (c->arg == SEQOP_RUN)    seq_start(&g_seqctx, g_seqctx.n);
      else if (c->arg == SEQOP_STOP) { g_seqctx.state = SQ_IDLE;
                                       for (int j = 0; j < VB_CHANNELS; j++) g_en_cmd[j] = 0; }
      else if (c->arg == SEQOP_PAUSE)  { if (g_seqctx.state == SQ_RUNNING) g_seqctx.state = SQ_PAUSED; }
      else if (c->arg == SEQOP_RESUME) { if (g_seqctx.state == SQ_PAUSED)  g_seqctx.state = SQ_RUNNING; }
      break;

    case OP_CAL_PT: {
      // One measured point. CV needs two before a fit is possible, so the
      // first is stashed and the second completes it.
      static int32_t  pend_mV[VB_CHANNELS]   = { 0, 0 };
      static uint16_t pend_code[VB_CHANNELS] = { 0, 0 };
      if (c->arg == CALPT_CV) {
        if (!pend_code[k]) { pend_code[k] = s_want_cv[k]; pend_mV[k] = c->arg2;
                             Dbg.printf("cal ch%d: point 1 stored\n", k + 1); }
        else {
          ChanCal trial = g_cal[k];
          if (cal_fit_cv(pend_code[k], pend_mV[k], s_want_cv[k], c->arg2, &trial)) {
            g_cal[k] = trial; g_cal_valid[k] = true; g_nvs_dirty_cal = true;
            Dbg.printf("cal ch%d: v0=%.1fmV slope=%.1fuV ACCEPTED\n",
                       k + 1, trial.cv_v0_mV, trial.cv_slope_uV);
          } else {
            g_cal_valid[k] = false;
            Dbg.printf("cal ch%d: fit REJECTED (out of bounds) — keeping nominal\n", k + 1);
          }
          pend_code[k] = 0;
        }
      } else {
        if (cal_fit_cc_point(g_set_mA[k], c->arg2, &g_cal[k])) g_nvs_dirty_cal = true;
      }
      break;
    }

    case OP_PRESET_SAVE: vb_preset_save((uint8_t)c->arg, c->ch); break;
    case OP_PRESET_LOAD: vb_preset_load((uint8_t)c->arg, c->ch); break;

    case OP_CAL_AUTO:
      if (!vb_cal_load_engage(c->ch))
        Dbg.printf("cal ch%d: load REFUSED — output above %d mV\n", c->ch, CAL_LOAD_MAX_MV);
      break;

    default: break;
  }
}

// =============================================================================
//   The task
// =============================================================================
void vb_control_task(void *) {
  TickType_t next = xTaskGetTickCount();
  uint32_t   prev_ms = millis();
  uint8_t    clear_req[VB_CHANNELS] = { 0, 0 };
  uint32_t   t_last_pub = 0;

  for (;;) {
    vTaskDelayUntil(&next, pdMS_TO_TICKS(CTRL_TICK_MS));
    uint32_t now = millis();
    uint32_t dt  = now - prev_ms;
    prev_ms = now;
    if (dt == 0) dt = CTRL_TICK_MS;         // clock did something odd; do not divide by it

    // ---- drain the command queue ------------------------------------------
    VbCommand c;
    while (g_cmd_q && xQueueReceive(g_cmd_q, &c, 0) == pdTRUE) {
      if (c.op == OP_CLEAR_TRIP) {
        if (c.ch >= 1 && c.ch <= VB_CHANNELS) clear_req[c.ch - 1] = 1;
      } else {
        handle_queued(&c);
      }
    }

    // ---- self-cal load deadline -------------------------------------------
    // A 2 s hard stop, checked here rather than trusted to whoever engaged it.
    if (s_cal_load_ch && (now - s_cal_load_t0) > CAL_LOAD_MAX_MS) {
      Dbg.println("cal: load deadline expired — releasing");
      vb_cal_load_release();
    }

    // ---- sense ------------------------------------------------------------
    int32_t v_mV[VB_CHANNELS], i_mA[VB_CHANNELS];
    bool    sense_ok[VB_CHANNELS];
    for (int k = 0; k < VB_CHANNELS; k++) {
      int32_t mv = 0, ua = 0;
      sense_ok[k] = ina_read(INA_ADDR[k], &mv, &ua);
      if (sense_ok[k]) {
        s_sense_fail[k] = 0;
        ua = cal_subtract_phantom(ua, mv);           // the divider's own draw
        v_mV[k] = (int32_t)lrintf((float)mv * g_cal[k].ina_v_gain);
        i_mA[k] = (int32_t)lrintf((float)ua * g_cal[k].ina_i_gain / 1000.0f);
      } else {
        if (s_sense_fail[k] < 255) s_sense_fail[k]++;
        v_mV[k] = 0; i_mA[k] = 0;
      }
    }
    int32_t temp_c10 = temp_read_c10();

    // ---- protection FSM, per channel --------------------------------------
    ProtOut po[VB_CHANNELS];
    bool    any_trip = false, force_safe[VB_CHANNELS] = { false, false };
    for (int k = 0; k < VB_CHANNELS; k++) {
      ProtIn in;
      memset(&in, 0, sizeof(in));
      in.v_mV    = v_mV[k];       in.i_mA    = i_mA[k];
      in.vset_mV = g_set_mV[k];   in.iset_mA = g_set_mA[k];
      in.ovp_mV  = g_ovp_mV[k];   in.ocp_mA  = g_ocp_mA[k];
      in.temp_c10 = temp_c10;     in.otp_c   = g_otp_c;
      in.cc_mode  = (CcMode)g_cc_mode[k];
      in.en_cmd   = g_en_cmd[k];
      in.hw_fault = g_hw_fault || !s_dac_ok;
      // Three consecutive read failures latch SENSE: OVP and OCP both depend on
      // this reading, so a channel that cannot measure itself must not stay on.
      in.sense_fault = s_sense_fail[k] >= 3;
      in.interlock   = (s_cal_load_ch == k + 1);
      in.clear_req   = clear_req[k];

      prot_tick(&s_prot[k], &in, dt, &po[k]);
      clear_req[k] = 0;

      if (po[k].clear_en_cmd) g_en_cmd[k] = 0;
      if (po[k].push_now)     g_push_now  = true;
      if (po[k].log_now)      any_trip    = true;
      force_safe[k] = po[k].force_dac_safe;
      if (po[k].reg_warn)
        Dbg.printf("ch%d regulation: set=%dmV meas=%dmV — check the FB network\n",
                   k + 1, (int)g_set_mV[k], (int)v_mV[k]);
    }

    // ---- sequencer ---------------------------------------------------------
    if (g_seqctx.state == SQ_RUNNING) {
      SeqWorld w;
      SeqAction a;
      for (int k = 0; k < VB_CHANNELS; k++) {
        w.v_mV[k]    = v_mV[k];
        w.i_mA[k]    = i_mA[k];
        w.tripped[k] = (s_prot[k].state == PS_TRIPPED);
      }
      seq_tick(&g_seqctx, g_seq, &w, dt, &a);
      if (a.do_all_off) for (int k = 0; k < VB_CHANNELS; k++) g_en_cmd[k] = 0;
      if (a.do_set && a.ch) {
        g_set_mV[a.ch - 1] = a.v_mV;
        if (a.i_mA >= 0) g_set_mA[a.ch - 1] = a.i_mA;   // -1 = leave the limit
        g_last_src = SRC_SEQ; g_rev_set++;
      }
      if (a.do_enable && a.ch) g_en_cmd[a.ch - 1] = a.en;
      if (a.finished)          g_push_now = true;
    }

    // ---- drive the DAC and EN ---------------------------------------------
    for (int k = 0; k < VB_CHANNELS; k++) {
      if (force_safe[k]) {
        // A trip drives the DAC safe AS WELL AS dropping EN, so a stuck EN net
        // cannot hold 20 V on a tripped output.
        dac_force_safe((uint8_t)k);
      } else {
        uint16_t cv_code, cc_code;
        int32_t  cv_act, cc_act;
        int32_t  ceiling = g_ovp_mV[k] < VB_VMAX_MV ? g_ovp_mV[k] : VB_VMAX_MV;

        CvCodeResult rv = cal_cv_code(&g_cal[k], g_set_mV[k], VB_VMIN_MV, ceiling,
                                      &cv_code, &cv_act);
        cal_cc_code(&g_cal[k], g_set_mA[k], VB_IMAX_MA, &cc_code, &cc_act);

        if (rv == CVC_REFUSED) {
          // The calibration is describing hardware we do not have. Refusing is
          // the only safe answer — obeying it is how a DUT dies.
          Dbg.printf("ch%d CV assert-back FAILED (want %d, code %u -> %d) — HW_FAULT\n",
                     k + 1, (int)g_set_mV[k], cv_code, (int)cv_act);
          g_hw_fault = true;
          dac_force_safe((uint8_t)k);
        } else if (!dac_apply((uint8_t)k, cv_code, cc_code)) {
          g_hw_fault = true;
        }
      }
      hal_set_en((uint8_t)(k + 1), po[k].en_out);
    }

    // ---- periodic DAC verify ----------------------------------------------
    // Cheap insurance against a register corrupted by a glitch on a shared bus.
    static uint8_t verify_div = 0;
    if (++verify_div >= 50) {                 // ~1 Hz
      verify_div = 0;
      if (s_dac_ok && !dac_verify()) {
        Dbg.println("DAC readback mismatch during run — HW_FAULT");
        g_hw_fault = true;
      }
    }

    // ---- log ---------------------------------------------------------------
    {
      uint16_t lv[2], li[2];
      for (int k = 0; k < VB_CHANNELS; k++) {
        lv[k] = (uint16_t)(v_mV[k] < 0 ? 0 : (v_mV[k] > 65535 ? 65535 : v_mV[k]));
        li[k] = log_pack_i(i_mA[k], po[k].en_out,
                           s_prot[k].cc_badge, s_prot[k].state == PS_TRIPPED);
      }
      log_append(&g_logring, now, lv, li, any_trip ? 1 : 0);
    }

    // ---- publish the snapshot (one seqlock write) --------------------------
    if (now - t_last_pub >= 50) {
      t_last_pub = now;
      VbSnapshot s;
      memset(&s, 0, sizeof(s));
      s.ts_ms    = now;
      s.uptime_s = now / 1000;
      for (int k = 0; k < VB_CHANNELS; k++) {
        s.ch[k].v_mV    = v_mV[k];
        s.ch[k].i_mA    = i_mA[k];
        s.ch[k].w_mW    = (int32_t)((int64_t)v_mV[k] * i_mA[k] / 1000);
        s.ch[k].vset_mV = g_set_mV[k];
        s.ch[k].iset_mA = g_set_mA[k];
        s.ch[k].ovp_mV  = g_ovp_mV[k];
        s.ch[k].ocp_mA  = g_ocp_mA[k];
        s.ch[k].en      = g_en_cmd[k];
        s.ch[k].state   = (uint8_t)s_prot[k].state;
        s.ch[k].reason  = (uint8_t)s_prot[k].reason;
        s.ch[k].cc_mode = g_cc_mode[k];
        s.ch[k].cal_valid = g_cal_valid[k];
        s.ch[k].sense_ok  = sense_ok[k];
      }
      s.temp_c10  = temp_c10;
      s.temp_ok   = 1;
      s.seq_state = (uint8_t)g_seqctx.state;
      s.seq_pc    = g_seqctx.pc;
      s.seq_n     = g_seqctx.n;
      s.seq_el_ms = g_seqctx.t_total;
      s.log_on    = g_logring.running;
      s.log_n     = log_count(&g_logring);
      s.log_cap   = g_logring.cap;
      s.rev_set   = g_rev_set;
      s.last_src  = g_last_src;
      s.hw_fault  = g_hw_fault;
      vb_publish(&s);
    }

    // ---- display, inside this task and inside a hard 6 ms budget -----------
    // Per spec §2.1 this is why there is no ui task and no I2C mutex anywhere.
    ui_tick(now);
  }
}
