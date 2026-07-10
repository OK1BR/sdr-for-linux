/*
 * sdr-for-linux — PureSignal runtime (F7 / PS-2). See ps.h.
 *
 * References: piHPSDR @974acba transmitter.c (tx_ps_* 2426-2580, feedback
 * accumulation 2068-2132, defaults 1069-1080/1203-1241) and vendored WDSP
 * calcc.c (control 891-1132, state machine 617-799). All WDSP calls target
 * the TXA channel (tx_dsp_channel()); pscc runs on the caller's thread and
 * never blocks (the spline solve lives on calcc's own worker thread).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wdsp.h"
#include "protocol2.h"
#include "tx.h"
#include "ps.h"

#define PS_SETPK_DEFAULT 0.2899   /* piHPSDR P2 non-Saturn default (transmitter.c:1213) */
#define PS_BLOCK         1024     /* pairs per pscc call (piHPSDR buffer_size, ~5.3 ms @192k) */

static int      s_started;
static int      s_ch = -1;         /* WDSP TXA channel id                       */
static GMutex   s_lock;            /* fences the config below (GUI vs status)   */
static int      s_enable;
static int      s_att;
static double   s_setpk = PS_SETPK_DEFAULT;

static volatile gint s_keyed;      /* g_atomic: keyed && !CW (feed gate)        */
static volatile gint s_auto = 1;   /* g_atomic: "Auto attenuate" (piHPSDR auto_on) */

void ps_set_auto(int on) {
  g_atomic_int_set(&s_auto, on ? 1 : 0);
}

/* Arm continuous calibration (piHPSDR tx_ps_resume automode branch,
 * transmitter.c:2544-2556). Single-cal was removed by decision 2026-07-11 —
 * continuous automode self-heals (2 failed fits → LRESET → automode → LWAIT
 * → retry), so no oneshot dead-end exists. */
static void ps_resume(void) {
  SetPSControl(s_ch, 0, 0, 1, 0);
}

/* Feedback accumulator — written ONLY on the P2 listener thread (feed_cb);
 * the fill counter is atomically zeroed from the tx thread on key-on. */
static double s_txacc[2 * PS_BLOCK];
static double s_rxacc[2 * PS_BLOCK];
static volatile gint s_fill;

/* piHPSDR default PS 2.0 parameter set (transmitter.c:1069-1080, applied via
 * tx_ps_setparams 2564-2580). ints/spi stay at calcc's own defaults (16/256) —
 * calling SetPSIntsAndSpi is a stop-the-world resize and is skipped on purpose. */
static void apply_params(void) {
  SetPSHWPeak(s_ch, s_setpk);
  SetPSMapMode(s_ch, 1);
  SetPSPtol(s_ch, 0.8);
  SetPSStabilize(s_ch, 0);
  SetPSPinMode(s_ch, 1);
  SetPSMoxDelay(s_ch, 0.2);
  SetPSTXDelay(s_ch, 150e-9);      /* amp delay 150 ns */
  SetPSLoopDelay(s_ch, 0.0);
}

/* P2 feedback (listener thread): accumulate 1024 pairs, then hand to the
 * calibration engine — exactly piHPSDR tx_add_ps_iq_samples (transmitter.c:
 * 2068-2132; G1 has no do_scale, so no DAC-feedback rescaling). Dropped
 * whenever not keyed-non-CW: stale/CW feedback must never reach a running
 * calibration. */
static void feed_cb(const double *txfb, const double *rxfb, int n_pairs, void *user) {
  (void)user;
  if (!g_atomic_int_get(&s_keyed)) { return; }
  /* SDRFL_PS_DEBUG=1: ~1 Hz RMS of both streams. Ground-truth discriminator
   * for the wire pair order: the COUPLER stream tracks the ADC0 attenuator,
   * the DAC loopback does not. If "tx" moves with the att slider, the pair
   * is swapped on the wire and the decode must flip. */
  static int dbg = -1;
  if (dbg < 0) { dbg = getenv("SDRFL_PS_DEBUG") != NULL; }
  if (dbg) {
    static double acc_tx, acc_rx; static long acc_n;
    for (int i = 0; i < n_pairs; i++) {
      acc_tx += txfb[2*i]*txfb[2*i] + txfb[2*i+1]*txfb[2*i+1];
      acc_rx += rxfb[2*i]*rxfb[2*i] + rxfb[2*i+1]*rxfb[2*i+1];
    }
    acc_n += n_pairs;
    if (acc_n >= 192000) {
      fprintf(stderr, "ps dbg: rms_tx(DAC)=%.4f  rms_rx(coupler)=%.4f\n",
              sqrt(acc_tx / acc_n), sqrt(acc_rx / acc_n));
      fflush(stderr);
      acc_tx = acc_rx = 0.0; acc_n = 0;
    }
  }
  /* SDRFL_PS_DUMP=<path>: capture the first ~2 s of keyed feedback (raw
   * doubles, [txI txQ rxI rxQ] per pair) for offline analysis — measures the
   * actual tx/rx alignment (cross-correlation) and the envelope transfer
   * curve that calcc has to fit. One-shot per run. */
  static FILE *dumpf; static long dump_left = -1;
  if (dump_left < 0) {
    const char *p = getenv("SDRFL_PS_DUMP");
    dump_left = 0;
    if (p && *p) {
      dumpf = fopen(p, "wb");
      if (dumpf) { dump_left = 2 * 192000; }
    }
  }
  if (dumpf && dump_left > 0) {
    for (int i = 0; i < n_pairs && dump_left > 0; i++, dump_left--) {
      double rec[4] = { txfb[2*i], txfb[2*i+1], rxfb[2*i], rxfb[2*i+1] };
      fwrite(rec, sizeof rec, 1, dumpf);
    }
    if (dump_left == 0) {
      fclose(dumpf); dumpf = NULL;
      fprintf(stderr, "ps: feedback dump complete (2 s)\n"); fflush(stderr);
    }
  }

  int fill = g_atomic_int_get(&s_fill);
  for (int i = 0; i < n_pairs; i++) {
    s_txacc[2 * fill]     = txfb[2 * i];
    s_txacc[2 * fill + 1] = txfb[2 * i + 1];
    s_rxacc[2 * fill]     = rxfb[2 * i];
    s_rxacc[2 * fill + 1] = rxfb[2 * i + 1];
    if (++fill >= PS_BLOCK) {
      pscc(s_ch, PS_BLOCK, s_txacc, s_rxacc);
      fill = 0;
    }
  }
  g_atomic_int_set(&s_fill, fill);
}

int ps_start(void) {
  if (s_started) { return 0; }
  s_ch = tx_dsp_channel();
  if (s_ch < 0) { return -1; }
  SetPSFeedbackRate(s_ch, 192000);   /* P2 feedback DDCs are fixed 192 kHz (radio.c:977) */
  apply_params();
  p2_set_ps_iq_cb(feed_cb, NULL);
  s_started = 1;
  return 0;
}

void ps_stop(void) {
  if (!s_started) { return; }
  p2_set_ps_iq_cb(NULL, NULL);
  g_mutex_lock(&s_lock);
  int was_on = s_enable;
  s_enable = 0;
  g_mutex_unlock(&s_lock);
  if (was_on) {
    SetPSControl(s_ch, 1, 0, 0, 0);
    p2_set_ps(NULL);
  }
  g_atomic_int_set(&s_keyed, 0);
  s_started = 0;
}

void ps_set(int enable, int att_db, double setpk) {
  if (att_db < 0)    { att_db = 0; }
  if (att_db > 31)   { att_db = 31; }
  if (setpk < 0.01)  { setpk = PS_SETPK_DEFAULT; }   /* guard: tiny SetPk breaks calcc */
  if (setpk > 1.01)  { setpk = PS_SETPK_DEFAULT; }

  g_mutex_lock(&s_lock);
  int was_on = s_enable;
  s_enable = enable ? 1 : 0;
  s_att    = att_db;
  s_setpk  = setpk;
  g_mutex_unlock(&s_lock);
  if (!s_started) { return; }        /* config cached; applied at ps_start+ps_set */

  SetPSHWPeak(s_ch, setpk);          /* cheap; picked up by the next calibration */

  if (enable) {
    /* Wire first (feedback DDC config + attenuator ride the next keyed state),
     * then arm continuous calibration (piHPSDR tx_ps_resume = automode,
     * transmitter.c:2544-2556). The state machine idles in LWAIT until MOX
     * brings feedback samples — no sleep needed on the RX side. */
    p2_ps_state ps = { .enabled = 1, .attenuation = att_db, .feedback_ant = 0 };
    p2_set_ps(&ps);
    ps_resume();
  } else if (was_on) {
    /* piHPSDR tx_ps_onoff OFF choreography (transmitter.c:2446-2499): a reset
     * only takes effect while samples keep flowing through pscc, so while
     * receiving we pump a few zero blocks through it by hand. */
    static double zeros[2 * PS_BLOCK];   /* zero-initialised, reused */
    SetPSControl(s_ch, 1, 0, 0, 0);
    if (!g_atomic_int_get(&s_keyed)) {
      for (int i = 0; i < 7; i++) { pscc(s_ch, PS_BLOCK, zeros, zeros); }
    }
    p2_set_ps(NULL);
  }
}

void ps_recal(void) {
  if (!s_started) { return; }
  int auto_on = g_atomic_int_get(&s_auto);
  g_mutex_lock(&s_lock);
  int on = s_enable;
  if (auto_on) { s_att = 0; }        /* piHPSDR Restart semantics — see below */
  int att = s_att;
  g_mutex_unlock(&s_lock);
  if (!on) { return; }
  /* piHPSDR resume_cb (ps_menu.c:537-545): with Auto attenuate, restart the
   * level hunt from 0 dB — "a very high attenuation could mean WDSP never
   * calibrates, so auto-adjust would never trigger" (too-weak feedback never
   * fills the COLLECT bins → no new calibration → no auto-att step). With
   * auto off the operator's manual attenuator is preserved, exactly like
   * piHPSDR's manual TX-ATT mode. */
  p2_ps_state ps = { .enabled = 1, .attenuation = att, .feedback_ant = 0 };
  p2_set_ps(&ps);
  SetPSControl(s_ch, 1, 0, 0, 0);    /* drop the held/stale correction */
  ps_resume();                       /* re-arm per mode (automode / one cal) */
}

void ps_key(int keyed) {
  if (!s_started) { return; }
  int prev = g_atomic_int_get(&s_keyed);
  keyed = keyed ? 1 : 0;
  if (keyed == prev) { return; }
  if (keyed) { g_atomic_int_set(&s_fill, 0); }   /* fresh over: no stale half-buffer */
  g_atomic_int_set(&s_keyed, keyed);
  SetPSMox(s_ch, keyed);             /* lock-free in calcc — safe from the keying path */
}

/* Auto-attenuate — piHPSDR ps_calibration_timer verbatim (ps_menu.c:169-281),
 * active only while the two-tone experiment keys (piHPSDR scopes it the same
 * way; in normal QSO the attenuator stays put). On each NEW calibration with
 * the feedback level outside the 140-165 accept band, step the ADC0 attenuator
 * by 20·log10(fdbk/152.293) dB (±15 dB jumps at the clip/noise extremes), then
 * run the reset → apply → reset → resume choreography so the next calibration
 * starts clean at the new level. Called from the tx_run gate slot (~20 Hz). */
void ps_auto_tick(int keyed, int twotone) {
  static int a_state, a_last_cals, a_stuck;
  if (!s_started) { return; }
  g_mutex_lock(&s_lock);
  int on = s_enable, att = s_att;
  g_mutex_unlock(&s_lock);
  if (!on || !keyed) { a_state = 0; a_stuck = 0; return; }

  int info[16];
  GetPSInfo(s_ch, info);
  int newcal = info[5] != a_last_cals;
  a_last_cals = info[5];

  /* Belt-and-braces: automode should never park in LRESET (it re-enters
   * LWAIT by itself), but if the machine ever sits there ~0.5 s while keyed,
   * re-arm — cheap insurance against a repeat of tonight's dead-ends. */
  if (info[15] == 0) {
    if (++a_stuck >= 10) { ps_resume(); a_stuck = 0; }
  } else {
    a_stuck = 0;
  }

  /* Attenuator stepping: EXACTLY piHPSDR's scoping — only during the
   * two-tone experiment and only with the Auto attenuate switch on
   * (ps_calibration_timer exists only while twotone runs, ps_menu.c:2934). */
  if (!twotone || !g_atomic_int_get(&s_auto)) { a_state = 0; return; }

  switch (a_state) {
  case 0: {
    if (!newcal) { return; }
    int f = info[4];
    if (f >= 140 && f <= 165) { return; }        /* ±0.7 dB accept band */
    int delta;
    if      (f > 275) { delta =  15; }           /* clipped — real level unknown */
    else if (f <  25) { delta = -15; }           /* buried — jump up             */
    else              { delta = (int)lround(20.0 * log10((double)f / 152.293)); }
    int na = att + delta;
    if (na < 0)  { na = 0; }
    if (na > 31) { na = 31; }
    if (na == att) { return; }
    SetPSControl(s_ch, 1, 0, 0, 0);              /* drop the (mis-scaled) cal    */
    g_mutex_lock(&s_lock); s_att = na; g_mutex_unlock(&s_lock);
    p2_ps_state ps = { .enabled = 1, .attenuation = na, .feedback_ant = 0 };
    p2_set_ps(&ps);                              /* new level on the wire (kick) */
    fprintf(stderr, "ps: auto-att %d -> %d dB (fdbk %d)\n", att, na, f);
    a_state = 1;
    break;
  }
  case 1: SetPSControl(s_ch, 1, 0, 0, 0); a_state = 2; break;
  case 2: ps_resume(); a_state = 0; break;               /* resume per mode */
  }
}

void ps_get_status(ps_status *out) {
  if (!out) { return; }
  memset(out, 0, sizeof(*out));
  g_mutex_lock(&s_lock);
  int on = s_started && s_enable;
  int att = s_att;
  g_mutex_unlock(&s_lock);
  if (!on) { return; }
  int info[16];
  GetPSInfo(s_ch, info);
  out->on         = 1;
  out->att        = att;
  out->fdbk       = info[4];
  out->cals       = info[5];
  out->sln        = info[6];
  out->correcting = info[14];
  out->state      = info[15];
  GetPSMaxTX(s_ch, &out->getpk);
}
