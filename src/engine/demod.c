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

int demod_create(int id, int in_rate, int mode, double flo, double fhi, double volume) {
  d_id = id;
  int scale = in_rate / AUDIO_RATE;
  if (scale < 1) { scale = 1; }
  d_output = D_BUFSIZE / scale;          /* 64 @768k, 256 @192k */
  d_iq    = g_new0(double, 2 * D_BUFSIZE);
  d_audio = g_new0(double, 2 * d_output);
  d_mono  = g_new0(float,  d_output);
  d_fill  = 0;
  /* WDSP's RXA audio comes out very low for us (~0.03 peak on a strong signal),
   * so a digital master gain lifts it to a usable level. 8 is a sane default;
   * SDRFL_GAIN overrides. (Proper AGC-target calibration is a later refinement.) */
  const char *g = getenv("SDRFL_GAIN");
  d_gain = (g && *g) ? atof(g) : 8.0;

  g_mutex_lock(&d_lock);
  /* Channel opens already running (state=1). */
  OpenChannel(id, D_BUFSIZE, D_DSPSIZE, in_rate, AUDIO_RATE, AUDIO_RATE,
              0, 1, 0.010, 0.025, 0.0, 0.010, 1);
  SetRXABandpassRun(id, 1);
  SetRXAPanelRun(id, 1);
  SetRXAPanelSelect(id, 3);              /* use both I and Q */
  SetRXAMode(id, mode);
  RXASetPassband(id, flo, fhi);
  /* Medium AGC preset (receiver.c:1885-1893). */
  SetRXAAGCMode(id, 5);
  SetRXAAGCAttack(id, 2);
  SetRXAAGCDecay(id, 250);
  SetRXAAGCHang(id, 0);
  SetRXAAGCHangThreshold(id, 0);
  SetRXAAGCSlope(id, 35);
  SetRXAAGCTop(id, 80.0);
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
        fexchange0(d_id, d_iq, d_audio, &err);   /* blocks on WDSP compute (bfo=1) */
        d_blocks++;
        if (err != 0) { d_err = err; d_ferr++; }
        for (int k = 0; k < d_output; k++) {
          double s = d_audio[k * 2] * d_gain;     /* LEFT channel = mono, × master gain */
          if (s >  1.0) { s =  1.0; }             /* clip to avoid wrap on the sink */
          if (s < -1.0) { s = -1.0; }
          d_mono[k] = (float)s;
          double a = s < 0 ? -s : s;
          if (a > d_peak) { d_peak = a; }
        }
        audio_push(d_mono, d_output);
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

void demod_destroy(void) {
  g_mutex_lock(&d_lock);
  if (d_ready) {
    CloseChannel(d_id);
    d_ready = 0;
  }
  g_free(d_iq);    d_iq = NULL;
  g_free(d_audio); d_audio = NULL;
  g_free(d_mono);  d_mono = NULL;
  g_mutex_unlock(&d_lock);
}
