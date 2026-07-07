/*
 * sdr-for-linux — WDSP demodulator wrapper. See demod.h.
 *
 * Mirrors piHPSDR receiver.c @ 974acba: OpenChannel (:1001) + the RXA setters
 * (:1019-1047) + fexchange0 (:1334). We take the demodulated LEFT channel as
 * mono and hand it to the audio sink. WDSP resamples the input IQ rate down to
 * 48 kHz internally, so the audio output is always 48 kHz.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "wdsp.h"
#include "demod.h"
#include "audio.h"

#define D_BUFSIZE  1024     /* IQ samples per fexchange0 (in_size)        */
#define D_DSPSIZE  2048     /* WDSP internal DSP block                    */
#define AUDIO_RATE 48000    /* WDSP dsp + output rate                     */

static int      d_id;
static int      d_ready;
static double  *d_iq;       /* accumulator, 2*D_BUFSIZE interleaved I/Q   */
static int      d_fill;     /* pairs currently accumulated                */
static double  *d_audio;    /* fexchange0 output, 2*d_output interleaved  */
static float   *d_mono;     /* d_output mono floats for the sink          */
static int      d_output;   /* audio frames per block = 1024/(rate/48k)   */
static GMutex   d_lock;     /* fences feed vs create/destroy              */
static double   d_peak;     /* max |audio| since last demod_peak()        */
static int      d_err;      /* last non-zero fexchange0 error             */
static int      d_ferr;     /* count of fexchange0 calls with error       */
static long     d_blocks;   /* fexchange0 calls                           */
static double   d_gain = 1.0; /* digital master gain after WDSP (SDRFL_GAIN) */
static int      d_agc_mode = 3;    /* 0=off,1=long,2=slow,3=med,4=fast */
static double   d_agc_top  = 80.0; /* AGC-T threshold/gain (dB)        */
static int      d_nr, d_nb, d_anf; /* NR (ANR) / NB (ANB) / ANF on-off */
static int      d_rate;            /* IQ input rate (blocks/s = d_rate/1024) */
static double   d_vol_db;          /* AF volume in dB (panel gain source)    */
static int      d_dbg;             /* SDRFL_DEBUG_LEVELS: 1 Hz meter dump    */
static int      d_dbg_blocks;      /* fexchange0 calls since last dump       */
static double   d_dbg_pk;          /* raw WDSP-out peak since last dump      */

/* Apply the AGC character (d_agc_mode) + threshold (d_agc_top). Mirrors piHPSDR
 * rx_set_agc: WDSP mode 5 = AGC on, 0 = off; long/slow/med/fast differ only in
 * hang/decay. Caller holds d_lock. */
static void apply_agc(void) {
  int id = d_id;
  if (d_agc_mode <= 0) {                 /* off: fixed 0 dB gain */
    SetRXAAGCFixed(id, 0.0);
    SetRXAAGCMode(id, 0);
    return;
  }
  SetRXAAGCAttack(id, 2);
  switch (d_agc_mode) {
    case 1: SetRXAAGCHang(id, 2000); SetRXAAGCDecay(id, 2000); break;  /* long */
    case 2: SetRXAAGCHang(id, 1000); SetRXAAGCDecay(id, 500);  break;  /* slow */
    case 4: SetRXAAGCHang(id, 0);    SetRXAAGCDecay(id, 50);   break;  /* fast */
    default: SetRXAAGCHang(id, 0);   SetRXAAGCDecay(id, 250);  break;  /* medium */
  }
  SetRXAAGCHangThreshold(id, 0);
  SetRXAAGCSlope(id, 35);
  SetRXAAGCTop(id, d_agc_top);
  SetRXAAGCMode(id, 5);
}

/* One-time NR/NB/ANF parameter setup + apply the on/off flags (piHPSDR
 * rx_set_noise defaults: ANB slew/lead/lag 1e-5 s, thresh 4.95; ANR
 * 64/16/16e-4/10e-7; ANF 64 taps, 16 delay, -80 dB gain, -20 dB leakage;
 * position 1 = after AGC). NR = WDSP ANR, NB = ANB. Caller holds d_lock. */
static void setup_noise(void) {
  int id = d_id;
  SetEXTANBTau(id, 0.00001);
  SetEXTANBHangtime(id, 0.00001);
  SetEXTANBAdvtime(id, 0.00001);
  SetEXTANBThreshold(id, 4.95);
  SetRXAANRVals(id, 64, 16, 16e-4, 10e-7);
  SetRXAANRPosition(id, 1);
  SetRXAANFTaps(id, 64);
  SetRXAANFDelay(id, 16);
  SetRXAANFGain(id, pow(10.0, 0.05 * -80.0));
  SetRXAANFLeakage(id, pow(10.0, 0.05 * -20.0));
  SetRXAANFPosition(id, 1);
  SetEXTANBRun(id, d_nb);
  SetRXAANRRun(id, d_nr);
  SetRXAANFRun(id, d_anf);
}

int demod_create(int id, int in_rate, int mode, double flo, double fhi, double volume) {
  d_id = id;
  int scale = in_rate / AUDIO_RATE;
  if (scale < 1) { scale = 1; }
  d_output = D_BUFSIZE / scale;          /* 64 @768k, 256 @192k */
  d_iq    = g_new0(double, 2 * D_BUFSIZE);
  d_audio = g_new0(double, 2 * d_output);
  d_mono  = g_new0(float,  d_output);
  d_fill  = 0;
  /* Master gain after WDSP. With the ANT1-relay fix WDSP delivers piHPSDR-level
   * audio (~0.8 peak on strong signals before panel gain), so the default is
   * unity — the old x8 "deaf RX" workaround would clip. SDRFL_GAIN overrides. */
  const char *g = getenv("SDRFL_GAIN");
  d_gain = (g && *g) ? atof(g) : 1.0;
  d_rate = in_rate;
  d_vol_db = volume;
  d_dbg = getenv("SDRFL_DEBUG_LEVELS") != NULL;
  d_dbg_blocks = 0;
  d_dbg_pk = 0.0;

  g_mutex_lock(&d_lock);
  /* Channel opens already running (state=1). */
  OpenChannel(id, D_BUFSIZE, D_DSPSIZE, in_rate, AUDIO_RATE, AUDIO_RATE,
              0, 1, 0.010, 0.025, 0.0, 0.010, 1);
  /* External noise blanker (ANB) lives OUTSIDE the RXA channel: it must be
   * created explicitly (else the SetEXTANB setters and xanbEXT dereference a
   * null struct and crash) and run on the IQ in the feed loop. Params per
   * piHPSDR receiver.c:1014. */
  create_anbEXT(id, 1, D_BUFSIZE, in_rate, 0.0001, 0.0001, 0.0001, 0.05, 20.0);
  SetRXABandpassRun(id, 1);
  SetRXAPanelRun(id, 1);
  SetRXAPanelSelect(id, 3);              /* use both I and Q */
  SetRXAMode(id, mode);
  RXASetPassband(id, flo, fhi);
  apply_agc();                          /* AGC character + threshold (d_agc_mode/d_agc_top) */
  setup_noise();                        /* NR/NB/ANF params + on-off flags */
  SetRXAPanelGain1(id, pow(10.0, 0.05 * volume));  /* AF gain dB → linear */
  d_ready = 1;
  g_mutex_unlock(&d_lock);
  return 0;
}

void demod_feed(const double *iq, int n_pairs) {
  if (!d_ready) { return; }
  g_mutex_lock(&d_lock);
  if (d_ready) {
    for (int i = 0; i < n_pairs; i++) {
      d_iq[d_fill * 2    ] = iq[i * 2    ];
      d_iq[d_fill * 2 + 1] = iq[i * 2 + 1];
      d_fill++;
      if (d_fill >= D_BUFSIZE) {
        int err = 0;
        if (d_nb) { xanbEXT(d_id, d_iq, d_iq); }   /* external noise blanker (in-place) */
        fexchange0(d_id, d_iq, d_audio, &err);   /* blocks on WDSP compute (bfo=1) */
        d_blocks++;
        if (err != 0) { d_err = err; d_ferr++; }
        for (int k = 0; k < d_output; k++) {
          double r = d_audio[k * 2];              /* raw WDSP out (incl. panel gain) */
          double a = r < 0 ? -r : r;
          if (a > d_dbg_pk) { d_dbg_pk = a; }
          double s = r * d_gain;                  /* LEFT channel = mono, × master gain */
          if (s >  1.0) { s =  1.0; }             /* clip to avoid wrap on the sink */
          if (s < -1.0) { s = -1.0; }
          d_mono[k] = (float)s;
          a = s < 0 ? -s : s;
          if (a > d_peak) { d_peak = a; }
        }
        audio_push(d_mono, d_output);
        /* SDRFL_DEBUG_LEVELS: once per second dump the WDSP meters piHPSDR shows
         * on its RX meter (needle dBm = RXA_S_AV raw; "Gain" = -RXA_AGC_GAIN;
         * "Out" = RXA_AGC_AV) + the raw WDSP audio peak. Directly comparable. */
        if (d_dbg && ++d_dbg_blocks >= d_rate / D_BUFSIZE) {
          printf("levels: S_av=%6.1f S_pk=%6.1f dBm | AGC gain=%5.1f dB out=%6.1f dB "
                 "| wdsp_pk=%.4f (vol %.0f dB -> panel %.3f, master x%.1f)\n",
                 GetRXAMeter(d_id, RXA_S_AV), GetRXAMeter(d_id, RXA_S_PK),
                 -GetRXAMeter(d_id, RXA_AGC_GAIN), GetRXAMeter(d_id, RXA_AGC_AV),
                 d_dbg_pk, d_vol_db, pow(10.0, 0.05 * d_vol_db), d_gain);
          fflush(stdout);
          d_dbg_blocks = 0;
          d_dbg_pk = 0.0;
        }
        d_fill = 0;
      }
    }
  }
  g_mutex_unlock(&d_lock);
}

double demod_peak(void) {
  double p = d_peak;
  d_peak = 0.0;
  return p;
}

int demod_last_error(void) { return d_err; }

void demod_set_mode(int mode, double flo, double fhi) {
  g_mutex_lock(&d_lock);
  if (d_ready) {
    SetRXAMode(d_id, mode);
    RXASetPassband(d_id, flo, fhi);
  }
  g_mutex_unlock(&d_lock);
}

void demod_set_gain(double gain) {
  g_mutex_lock(&d_lock);
  d_gain = gain;
  g_mutex_unlock(&d_lock);
}

void demod_set_volume(double db) {
  g_mutex_lock(&d_lock);
  d_vol_db = db;
  if (d_ready) { SetRXAPanelGain1(d_id, pow(10.0, 0.05 * db)); }  /* AF gain dB → linear */
  g_mutex_unlock(&d_lock);
}

/* AGC character: 0=off, 1=long, 2=slow, 3=medium, 4=fast (thread-safe). */
void demod_set_agc(int mode) {
  g_mutex_lock(&d_lock);
  d_agc_mode = mode;
  if (d_ready) { apply_agc(); }
  g_mutex_unlock(&d_lock);
}

/* AGC-T threshold/gain in dB (thread-safe; live when AGC is on). */
void demod_set_agc_gain(double db) {
  g_mutex_lock(&d_lock);
  d_agc_top = db;
  if (d_ready && d_agc_mode > 0) { SetRXAAGCTop(d_id, db); }
  g_mutex_unlock(&d_lock);
}

/* Noise reduction (ANR) / noise blanker (ANB) / auto-notch — on-off (thread-safe). */
void demod_set_nr(int on) {
  g_mutex_lock(&d_lock);
  d_nr = on ? 1 : 0;
  if (d_ready) { SetRXAANRRun(d_id, d_nr); }
  g_mutex_unlock(&d_lock);
}
void demod_set_nb(int on) {
  g_mutex_lock(&d_lock);
  d_nb = on ? 1 : 0;
  if (d_ready) { SetEXTANBRun(d_id, d_nb); }
  g_mutex_unlock(&d_lock);
}
void demod_set_anf(int on) {
  g_mutex_lock(&d_lock);
  d_anf = on ? 1 : 0;
  if (d_ready) { SetRXAANFRun(d_id, d_anf); }
  g_mutex_unlock(&d_lock);
}

void demod_set_passband(double flo, double fhi) {
  g_mutex_lock(&d_lock);
  if (d_ready) { RXASetPassband(d_id, flo, fhi); }
  g_mutex_unlock(&d_lock);
}

void demod_destroy(void) {
  g_mutex_lock(&d_lock);
  if (d_ready) {
    CloseChannel(d_id);
    destroy_anbEXT(d_id);       /* free the external noise blanker */
    d_ready = 0;
  }
  g_free(d_iq);    d_iq = NULL;
  g_free(d_audio); d_audio = NULL;
  g_free(d_mono);  d_mono = NULL;
  g_mutex_unlock(&d_lock);
}
