/*
 * sdr-for-linux — WDSP demodulator wrapper. See demod.h.
 *
 * Mirrors piHPSDR receiver.c @ 974acba: OpenChannel (:1001) + the RXA setters
 * (:1019-1047) + fexchange0 (:1334). We hand both demodulated channels (L/R) to
 * the (always-stereo) audio sink; with binaural off the WDSP panel makes L=R so
 * it plays as mono, with binaural on L=I / R=Q for the spatial effect. WDSP
 * resamples the input IQ rate down to 48 kHz internally, so audio out is 48 kHz.
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
#define AUDIO_RATE_DEFAULT 48000  /* default WDSP dsp + output rate       */

static int d_arate = AUDIO_RATE_DEFAULT;  /* selectable RX audio out rate (Hz) */

static int      d_id;
static int      d_ready;
static double  *d_iq;       /* accumulator, 2*D_BUFSIZE interleaved I/Q   */
static int      d_fill;     /* pairs currently accumulated                */
static double  *d_audio;    /* fexchange0 output, 2*d_output interleaved  */
static float   *d_out;      /* d_output interleaved L/R floats for the sink */
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
static int      d_binaural;        /* binaural stereo output (WDSP panel copy) */
static int      d_rate;            /* IQ input rate (blocks/s = d_rate/1024) */
static double   d_vol_db;          /* AF volume in dB (panel gain source)    */
static int      d_dbg;             /* SDRFL_DEBUG_LEVELS: 1 Hz meter dump    */
static int      d_mute_target;     /* g_atomic: 1 = want RX muted (RX-on-TX)  */
static double   d_mute_gain = 1.0; /* applied output gain, ramped → target (feed thread) */
#define D_FADE_STEP (1.0 / 960.0)  /* ~20 ms mute/unmute fade at 48 kHz (no click) */
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
  SetRXAEMNRgainMethod(id, 2);          /* NR2 (EMNR): gamma gain, per piHPSDR */
  SetRXAEMNRnpeMethod(id, 0);
  SetRXAEMNRaeRun(id, 1);
  SetRXAANFTaps(id, 64);
  SetRXAANFDelay(id, 16);
  SetRXAANFGain(id, pow(10.0, 0.05 * -80.0));
  SetRXAANFLeakage(id, pow(10.0, 0.05 * -20.0));
  SetRXAANFPosition(id, 1);
  /* NR: 0 off / 1 ANR (LMS) / 2 NR2 (EMNR) / 3 NR3 (RNNoise) / 4 NR4 (specbleach).
   * NB: 0 off / 1 ANB / 2 NB2 (SNBA). RNNR/SBNR are created by OpenChannel (WDSP
   * RXA.c) with sane defaults (sbnr: 10 dB reduction, 20 ms adaptive frame); we
   * only flip their run flags. */
  SetEXTANBRun(id, d_nb == 1);
  SetRXASNBARun(id, d_nb == 2);
  SetRXAANRRun(id, d_nr == 1);
  SetRXAEMNRRun(id, d_nr == 2);
  SetRXARNNRRun(id, d_nr == 3);
  SetRXASBNRRun(id, d_nr == 4);
  SetRXAANFRun(id, d_anf);
}

void demod_set_audio_rate(int rate) {
  if (rate >= 48000) { d_arate = rate; }   /* applied at the next demod_create */
}
int demod_audio_rate(void) { return d_arate; }

int demod_create(int id, int in_rate, int mode, double flo, double fhi, double volume) {
  d_id = id;
  /* Audio rate must divide the IQ rate and not exceed it (else fexchange0 would
   * emit more frames than d_audio holds). Clamp to a safe divisor. */
  if (d_arate > in_rate) { d_arate = in_rate; }
  int scale = in_rate / d_arate;
  if (scale < 1) { scale = 1; }
  d_arate = in_rate / scale;             /* snap to an exact integer divisor */
  d_output = D_BUFSIZE / scale;          /* frames/block: 32 @1536k/48k, 1024 @192k/192k */
  d_iq    = g_new0(double, 2 * D_BUFSIZE);
  d_audio = g_new0(double, 2 * d_output);
  d_out   = g_new0(float,  2 * d_output);   /* interleaved L/R for the sink */
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
  OpenChannel(id, D_BUFSIZE, D_DSPSIZE, in_rate, d_arate, d_arate,
              0, 1, 0.010, 0.025, 0.0, 0.010, 1);
  /* External noise blanker (ANB) lives OUTSIDE the RXA channel: it must be
   * created explicitly (else the SetEXTANB setters and xanbEXT dereference a
   * null struct and crash) and run on the IQ in the feed loop. Params per
   * piHPSDR receiver.c:1014. */
  create_anbEXT(id, 1, D_BUFSIZE, in_rate, 0.0001, 0.0001, 0.0001, 0.05, 20.0);
  SetRXABandpassRun(id, 1);
  SetRXAPanelRun(id, 1);
  SetRXAPanelSelect(id, 3);              /* use both I and Q */
  SetRXAPanelBinaural(id, d_binaural);   /* copy=1 (L=R, mono) off / copy=0 (L=I,R=Q) on */
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
        if (d_nb == 1) { xanbEXT(d_id, d_iq, d_iq); }   /* ext noise blanker (NB, in-place) */
        fexchange0(d_id, d_iq, d_audio, &err);   /* blocks on WDSP compute (bfo=1) */
        d_blocks++;
        if (err != 0) { d_err = err; d_ferr++; }
        /* RX-on-TX mute: ramp a gain toward 0 (muted) / 1 (open) so the sink never
         * sees a step → no click. The WDSP demod keeps running underneath. */
        double mgoal = g_atomic_int_get(&d_mute_target) ? 0.0 : 1.0;
        for (int k = 0; k < d_output; k++) {
          double rl = d_audio[k * 2];             /* raw WDSP out L (I)            */
          double rr = d_audio[k * 2 + 1];         /* raw WDSP out R (Q; == L unless binaural) */
          double a = rl < 0 ? -rl : rl;
          if (a > d_dbg_pk) { d_dbg_pk = a; }
          double sl = rl * d_gain, sr = rr * d_gain;   /* × master gain */
          if (sl >  1.0) { sl =  1.0; }           /* clip to avoid wrap on the sink */
          if (sl < -1.0) { sl = -1.0; }
          if (sr >  1.0) { sr =  1.0; }
          if (sr < -1.0) { sr = -1.0; }
          if      (d_mute_gain < mgoal) { d_mute_gain += D_FADE_STEP; if (d_mute_gain > mgoal) { d_mute_gain = mgoal; } }
          else if (d_mute_gain > mgoal) { d_mute_gain -= D_FADE_STEP; if (d_mute_gain < mgoal) { d_mute_gain = mgoal; } }
          d_out[k * 2]     = (float)(sl * d_mute_gain);   /* interleaved L/R */
          d_out[k * 2 + 1] = (float)(sr * d_mute_gain);
          a = sl < 0 ? -sl : sl;
          if (a > d_peak) { d_peak = a; }
        }
        audio_push(d_out, d_output);
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

double demod_s_meter(void) {
  if (!d_ready) { return -200.0; }
  return GetRXAMeter(d_id, RXA_S_PK);   /* tuned-signal strength, dBm (like piHPSDR) */
}

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

void demod_set_mute(int on) { g_atomic_int_set(&d_mute_target, on ? 1 : 0); }

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
void demod_set_nr(int mode) {   /* 0 off / 1 ANR / 2 EMNR(NR2) / 3 RNNoise(NR3) / 4 specbleach(NR4) */
  g_mutex_lock(&d_lock);
  d_nr = mode;
  if (d_ready) {
    SetRXAANRRun(d_id, d_nr == 1);
    SetRXAEMNRRun(d_id, d_nr == 2);
    SetRXARNNRRun(d_id, d_nr == 3);
    SetRXASBNRRun(d_id, d_nr == 4);
  }
  g_mutex_unlock(&d_lock);
}
void demod_set_nb(int mode) {   /* 0 off / 1 ANB / 2 SNBA (NB2) */
  g_mutex_lock(&d_lock);
  d_nb = mode;
  if (d_ready) { SetEXTANBRun(d_id, d_nb == 1); SetRXASNBARun(d_id, d_nb == 2); }
  g_mutex_unlock(&d_lock);
}
void demod_set_anf(int on) {
  g_mutex_lock(&d_lock);
  d_anf = on ? 1 : 0;
  if (d_ready) { SetRXAANFRun(d_id, d_anf); }
  g_mutex_unlock(&d_lock);
}
/* Binaural stereo output: 0 = mono (L=R), 1 = binaural (L=I, R=Q). WDSP's panel
 * does the split via copy=1-bin; the sink is always stereo so this is live. */
void demod_set_binaural(int on) {
  g_mutex_lock(&d_lock);
  d_binaural = on ? 1 : 0;
  if (d_ready) { SetRXAPanelBinaural(d_id, d_binaural); }
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
  g_free(d_out);   d_out = NULL;
  g_mutex_unlock(&d_lock);
}
