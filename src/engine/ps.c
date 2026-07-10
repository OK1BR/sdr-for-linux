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
    SetPSControl(s_ch, 0, 0, 1, 0);
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

void ps_key(int keyed) {
  if (!s_started) { return; }
  int prev = g_atomic_int_get(&s_keyed);
  keyed = keyed ? 1 : 0;
  if (keyed == prev) { return; }
  if (keyed) { g_atomic_int_set(&s_fill, 0); }   /* fresh over: no stale half-buffer */
  g_atomic_int_set(&s_keyed, keyed);
  SetPSMox(s_ch, keyed);             /* lock-free in calcc — safe from the keying path */
}

void ps_get_status(ps_status *out) {
  if (!out) { return; }
  memset(out, 0, sizeof(*out));
  g_mutex_lock(&s_lock);
  int on = s_started && s_enable;
  g_mutex_unlock(&s_lock);
  if (!on) { return; }
  int info[16];
  GetPSInfo(s_ch, info);
  out->on         = 1;
  out->fdbk       = info[4];
  out->cals       = info[5];
  out->sln        = info[6];
  out->correcting = info[14];
  out->state      = info[15];
  GetPSMaxTX(s_ch, &out->getpk);
}
