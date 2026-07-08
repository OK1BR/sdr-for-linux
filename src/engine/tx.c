/*
 * sdr-for-linux — WDSP TX DSP channel. See tx.h.
 *
 * Mirrors piHPSDR transmitter.c @974acba: OpenChannel(type=1) at 48k in / 96k dsp
 * / 192k out (:1307), the fixed TXA config (:1323-1356), and the mic -> IQ block
 * via fexchange0 (:1665). The G1 sends the IQ un-scaled (do_scale=0), so we apply
 * no gain here — the exciter level is the HP drive byte, set elsewhere. PURE DSP:
 * this file never keys the radio (no MOX, no PA).
 */
#include <glib.h>
#include <math.h>

#include "wdsp.h"
#include "tx.h"

#define TX_CHANNEL   8      /* WDSP channel id (RX uses 0; TXA/RXA share the space) */
#define TX_BUFSIZE   512    /* mic samples per fexchange0 (in_size)                 */
#define TX_DSPSIZE   2048   /* WDSP internal DSP block                              */
#define TX_IN_RATE   48000  /* mic input rate                                       */
#define TX_DSP_RATE  96000  /* WDSP dsp rate (power-of-two multiple of both)        */
#define TX_OUT_RATE  192000 /* DUC IQ output rate (P2 / G1)                         */

static int       t_id;
static int       t_ready;
static int       t_out_samples;   /* IQ pairs per fexchange0 out = 512*192k/48k = 2048 */
static double   *t_mic;           /* mic input as IQ (I=sample, Q=0), 2*TX_BUFSIZE    */
static int       t_fill;          /* mic samples accumulated                         */
static double   *t_iq;            /* fexchange0 output, 2*t_out_samples interleaved   */
static tx_iq_cb  t_cb;
static void     *t_user;
static GMutex    t_lock;          /* fences feed vs create/destroy/setters           */
static int       t_err;           /* last non-zero fexchange0 error                  */
static long      t_ferr;          /* count of fexchange0 calls with error            */
static long      t_blocks;        /* fexchange0 calls                                */

int tx_dsp_create(int mode, double flo, double fhi, tx_iq_cb cb, void *user) {
  t_id          = TX_CHANNEL;
  t_out_samples = TX_BUFSIZE * (TX_OUT_RATE / TX_IN_RATE);   /* 2048 */
  t_mic  = g_new0(double, 2 * TX_BUFSIZE);
  t_iq   = g_new0(double, 2 * t_out_samples);
  t_cb   = cb;
  t_user = user;
  t_fill = 0;
  t_err  = 0;
  t_ferr = 0;
  t_blocks = 0;

  g_mutex_lock(&t_lock);
  /* type=1 (transmit), state=0 (created stopped — tx_dsp_run starts it). */
  OpenChannel(t_id, TX_BUFSIZE, TX_DSPSIZE, TX_IN_RATE, TX_DSP_RATE, TX_OUT_RATE,
              1, 0, 0.010, 0.025, 0.0, 0.010, 1);
  SetTXABandpassWindow(t_id, 1);        /* 7-term Blackman-Harris */
  SetTXABandpassRun(t_id, 1);
  SetTXABandpassFreqs(t_id, flo, fhi);
  SetTXACFIRRun(t_id, 1);               /* P2 compensating FIR — required */
  SetTXAAMSQRun(t_id, 0);               /* mic noise gate off */
  SetTXAALCAttack(t_id, 1);
  SetTXAALCDecay(t_id, 10);
  SetTXAALCSt(t_id, 1);                 /* ALC always on (piHPSDR: never switch off) */
  SetTXAPreGenRun(t_id, 0);
  SetTXAPostGenRun(t_id, 0);            /* tune/two-tone generators off until asked */
  SetTXAPanelRun(t_id, 1);
  SetTXAPanelSelect(t_id, 2);           /* use the Mic-I sample */
  SetTXAPanelGain1(t_id, 1.0);          /* mic gain 0 dB */
  SetTXAMode(t_id, mode);
  t_ready = 1;
  g_mutex_unlock(&t_lock);
  return 0;
}

void tx_dsp_run(int on) {
  g_mutex_lock(&t_lock);
  if (t_ready) { SetChannelState(t_id, on ? 1 : 0, on ? 0 : 1); }
  g_mutex_unlock(&t_lock);
}

void tx_dsp_feed_mic(const float *mic, int n) {
  if (!t_ready) { return; }
  g_mutex_lock(&t_lock);
  if (t_ready) {
    for (int i = 0; i < n; i++) {
      t_mic[t_fill * 2    ] = (double)mic[i];   /* I = mic sample */
      t_mic[t_fill * 2 + 1] = 0.0;              /* Q = 0 (real mic) */
      t_fill++;
      if (t_fill >= TX_BUFSIZE) {
        int err = 0;
        fexchange0(t_id, t_mic, t_iq, &err);    /* mic block -> IQ block */
        t_blocks++;
        if (err != 0) { t_err = err; t_ferr++; }
        if (t_cb) { t_cb(t_iq, t_out_samples, t_user); }
        t_fill = 0;
      }
    }
  }
  g_mutex_unlock(&t_lock);
}

void tx_dsp_set_mode(int mode, double flo, double fhi) {
  g_mutex_lock(&t_lock);
  if (t_ready) {
    SetTXAMode(t_id, mode);
    SetTXABandpassFreqs(t_id, flo, fhi);
  }
  g_mutex_unlock(&t_lock);
}

void tx_dsp_set_mic_gain(double db) {
  g_mutex_lock(&t_lock);
  if (t_ready) { SetTXAPanelGain1(t_id, pow(10.0, 0.05 * db)); }
  g_mutex_unlock(&t_lock);
}

void tx_dsp_tune_tone(int on, double offset_hz) {
  g_mutex_lock(&t_lock);
  if (t_ready) {
    if (on) {
      SetTXAPostGenToneFreq(t_id, offset_hz);
      SetTXAPostGenToneMag(t_id, 0.99999);   /* full scale; power set by drive byte */
      SetTXAPostGenMode(t_id, 0);            /* single tone */
      SetTXAPostGenRun(t_id, 1);
    } else {
      SetTXAPostGenRun(t_id, 0);
    }
  }
  g_mutex_unlock(&t_lock);
}

int tx_dsp_last_error(void) { return t_err; }

void tx_dsp_destroy(void) {
  g_mutex_lock(&t_lock);
  if (t_ready) {
    CloseChannel(t_id);
    t_ready = 0;
  }
  g_free(t_mic); t_mic = NULL;
  g_free(t_iq);  t_iq  = NULL;
  g_mutex_unlock(&t_lock);
}
