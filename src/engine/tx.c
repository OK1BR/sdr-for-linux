/*
 * sdr-for-linux — WDSP TX DSP channel. See tx.h.
 *
 * Mirrors piHPSDR transmitter.c @974acba: OpenChannel(type=1) at 48k in / 96k dsp
 * / 192k out (:1307), the fixed TXA config (:1323-1356), and the mic -> IQ block
 * via fexchange0 (:1665). The G2E sends the IQ un-scaled (do_scale=0), so we apply
 * no gain here — the exciter level is the HP drive byte, set elsewhere. PURE DSP:
 * this file never keys the radio (no MOX, no PA).
 */
#include <glib.h>
#include <math.h>

#include "wdsp.h"
#include "tx.h"

#define TX_CHANNEL   8      /* WDSP channel id (RX uses 0; TXA/RXA share the space) */
#define TX_DSPSIZE   2048   /* WDSP internal DSP block                              */
#define TX_IN_RATE   48000  /* mic input rate                                       */

static int       t_id;
static int       t_ready;
static int       t_in_size;       /* mic samples per fexchange0 (512 P2 / 1024 P1) */
static int       t_out_samples;   /* IQ pairs per fexchange0 out (2048 P2 / 1024 P1)*/
static double   *t_mic;           /* mic input as IQ (I=sample, Q=0), 2*t_in_size   */
static int       t_fill;          /* mic samples accumulated                         */
static double   *t_iq;            /* fexchange0 output, 2*t_out_samples interleaved   */
static tx_iq_cb  t_cb;
static void     *t_user;
static GMutex    t_lock;          /* fences feed vs create/destroy/setters           */
static int       t_err;           /* last non-zero fexchange0 error                  */
static long      t_ferr;          /* count of fexchange0 calls with error            */
static long      t_blocks;        /* fexchange0 calls                                */

int tx_dsp_create(int mode, double flo, double fhi, int p1, tx_iq_cb cb, void *user) {
  /* Rate chains verbatim piHPSDR transmitter.c:987-1010:
   * P2 192 k out → in 512 / dsp 96 k / ratio 4; P1 48 k out → in 1024 /
   * dsp 48 k / ratio 1 (large P1 output buffers would overflow the radio's
   * TX FIFO — hence the bigger input block, not a bigger output one). */
  int dsp_rate = p1 ? 48000 : 96000;
  int out_rate = p1 ? 48000 : 192000;
  t_in_size     = p1 ? 1024 : 512;
  t_id          = TX_CHANNEL;
  t_out_samples = t_in_size * (out_rate / TX_IN_RATE);
  t_mic  = g_new0(double, 2 * t_in_size);
  t_iq   = g_new0(double, 2 * t_out_samples);
  t_cb   = cb;
  t_user = user;
  t_fill = 0;
  t_err  = 0;
  t_ferr = 0;
  t_blocks = 0;

  g_mutex_lock(&t_lock);
  /* type=1 (transmit), state=0 (created stopped — tx_dsp_run starts it). */
  OpenChannel(t_id, t_in_size, TX_DSPSIZE, TX_IN_RATE, dsp_rate, out_rate,
              1, 0, 0.010, 0.025, 0.0, 0.010, 1);
  SetTXABandpassWindow(t_id, 1);        /* 7-term Blackman-Harris */
  SetTXABandpassRun(t_id, 1);
  SetTXABandpassFreqs(t_id, flo, fhi);
  SetTXACFIRRun(t_id, p1 ? 0 : 1);      /* ⛔ P2 firmware requires it, P1 must
                                           NOT run it (transmitter.c:1325)   */
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
      if (t_fill >= t_in_size) {
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

int tx_dsp_in_rate(void) { return TX_IN_RATE; }

/* WDSP TXA channel id — PureSignal (ps.c) targets its calcc with it. -1 until
 * tx_dsp_create ran. */
int tx_dsp_channel(void) { return t_mic ? t_id : -1; }

void tx_dsp_get_meters(double *mic_pk_db, double *alc_gain_db, double *lvlr_gain_db) {
  double mic = -99.0, alc = 0.0, lvl = 0.0;
  g_mutex_lock(&t_lock);
  if (t_ready) {
    mic = GetTXAMeter(t_id, TXA_MIC_PK);      /* mic-input peak, dBFS (pre-gate) */
    alc = GetTXAMeter(t_id, TXA_ALC_GAIN);    /* ALC gain reduction, dB (≤ 0) */
    lvl = GetTXAMeter(t_id, TXA_LVLR_GAIN);   /* leveler makeup, dB (0 with PROC off) */
  }
  g_mutex_unlock(&t_lock);
  if (mic_pk_db)    { *mic_pk_db    = mic; }
  if (alc_gain_db)  { *alc_gain_db  = alc; }
  if (lvlr_gain_db) { *lvlr_gain_db = lvl; }
}

void tx_dsp_set_mic_gain(double db) {
  g_mutex_lock(&t_lock);
  if (t_ready) { SetTXAPanelGain1(t_id, pow(10.0, 0.05 * db)); }
  g_mutex_unlock(&t_lock);
}

void tx_dsp_set_gate(int on, double thresh_db) {
  g_mutex_lock(&t_lock);
  if (t_ready) {
    /* WDSP TXA AMSQ = downward expander, sits after the panel (mic gain) and
     * BEFORE the leveler/COMP — so in speech gaps there is nothing for PROC to
     * pump up. Depth -20 dB mirrors piHPSDR's DEXP expansion default
     * (transmitter.c:1141 dexp_exp=20); threshold is on the post-mic-gain
     * signal. Not a hard mute: gaps drop 20 dB, keying stays natural. */
    SetTXAAMSQMutedGain(t_id, -20.0);
    SetTXAAMSQThreshold(t_id, thresh_db);
    SetTXAAMSQRun(t_id, on ? 1 : 0);
  }
  g_mutex_unlock(&t_lock);
}

void tx_dsp_set_compressor(int on, double gain_db) {
  g_mutex_lock(&t_lock);
  if (t_ready) {
    /* piHPSDR tx_set_compressor order: leveler + CESSB first, compressor run
     * last — SetTXACompressorRun re-runs TXASetupBPFilters, which needs the
     * final osctrl state to bring the bp1/bp2 aux filters in. */
    SetTXALevelerSt(t_id, on ? 1 : 0);
    SetTXALevelerAttack(t_id, 1);
    SetTXALevelerDecay(t_id, 500);
    SetTXALevelerTop(t_id, 8.0);
    SetTXAosctrlRun(t_id, (on && gain_db > 5.5) ? 1 : 0);
    SetTXACompressorGain(t_id, gain_db);
    SetTXACompressorRun(t_id, on ? 1 : 0);
  }
  g_mutex_unlock(&t_lock);
}

/* Two-tone test generator (PureSignal calibration / IMD check; piHPSDR
 * transmitter.c:2902-2932): two tones just under half amplitude each, so the
 * two-tone envelope peaks at ~full scale. The PostGen REPLACES the chain
 * signal, so the mic is irrelevant while it runs. Caller pre-negates the
 * frequencies for LSB-family modes. */
void tx_dsp_two_tone(int on, double f1, double f2) {
  g_mutex_lock(&t_lock);
  if (t_ready) {
    if (on) {
      SetTXAPostGenTTFreq(t_id, f1, f2);
      SetTXAPostGenTTMag(t_id, 0.49999, 0.49999);
      SetTXAPostGenMode(t_id, 1);              /* two-tone */
      SetTXAPostGenRun(t_id, 1);
    } else {
      SetTXAPostGenRun(t_id, 0);
    }
  }
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
