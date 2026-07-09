/*
 * sdr-for-linux — TX panadapter analyzer. See tx_analyzer.h.
 *
 * Mirrors piHPSDR tx_set_analyzer (transmitter.c:2340-2409 @974acba). The TX
 * spectrum is always 24 kHz wide: from the `iq_rate` Hz TX IQ we clip
 * afft*(0.5 - 12000/iq_rate) bins from each end, leaving the central ±12 kHz.
 * The FFT size is chosen so the surviving bins ≈ the pixel count (sharp). Uses a
 * separate WDSP disp id from the RX analyzer, so the two coexist.
 */
#include <glib.h>
#include <math.h>

#include "wdsp.h"
#include "tx_analyzer.h"

#define TXA_DISP   1        /* WDSP analyzer/disp id (RX analyzer uses 0) */
#define TXA_SPAN   24000.0  /* TX display span (Hz) — piHPSDR fixed 24 kHz */

static int     t_pixels;
static int     t_afft;
static int     t_bf;        /* Spectrum0 block size (pairs) */
static double *t_acc;       /* accumulator 2*t_bf doubles */
static int     t_fill;
static int     t_ready;
static GMutex  t_lock;

int tx_analyzer_create(int pixels, int iq_rate, int bf_size, int fps) {
  int rc = -1;
  if (pixels <= 0 || iq_rate <= 0 || bf_size <= 0) { return -1; }
  t_pixels = pixels;
  t_bf     = bf_size;
  if (fps < 1) { fps = 1; }

  /* afft such that surviving bins (24 kHz worth) ≈ pixels, clamped like piHPSDR. */
  int want = (int)(((long)iq_rate * (long)pixels) / (long)TXA_SPAN);
  if      (want <= 16384) { t_afft = 16384; }
  else if (want <= 32768) { t_afft = 32768; }
  else                    { t_afft = 65536; }

  double clipf     = (double)t_afft * (0.5 - 12000.0 / (double)iq_rate);  /* each side */
  double keep_time = 0.1;
  int    overlap   = (int)fmax(0.0, ceil((double)t_afft - (double)iq_rate / (double)fps));
  int    max_w     = t_afft + (int)fmin(keep_time * (double)iq_rate,
                                        keep_time * (double)t_afft * (double)fps);
  int    flp[1]    = { 0 };

  t_acc  = g_new0(double, 2 * t_bf);
  t_fill = 0;

  g_mutex_lock(&t_lock);
  XCreateAnalyzer(TXA_DISP, &rc, 262144, 1, 1, NULL);
  if (rc != 0) {
    g_mutex_unlock(&t_lock);
    g_free(t_acc); t_acc = NULL;
    return -1;
  }
  SetAnalyzer(TXA_DISP, 1, 1, 1 /*complex*/, flp, t_afft, t_bf, 5 /*window*/, 14.0,
              overlap, 0, clipf, clipf, t_pixels, 1, 0, 0.0, 0.0, max_w);
  SetDisplayNormOneHz(TXA_DISP, 0, 1);
  SetDisplaySampleRate(TXA_DISP, iq_rate);
  /* Detector + light averaging, as for the RX analyzer (the GUI adds its own EMA). */
  double t    = 0.030;
  int    navg = (int)fmax(2.0, fmin(60.0, (double)fps * t));
  double avb  = exp(-1.0 / ((double)fps * t));
  SetDisplayDetectorMode(TXA_DISP, 0, DETECTOR_MODE_PEAK);
  SetDisplayAverageMode(TXA_DISP, 0, AVERAGE_MODE_NONE);
  SetDisplayNumAverage(TXA_DISP, 0, navg);
  SetDisplayAvBackmult(TXA_DISP, 0, avb);
  SetDisplayAverageMode(TXA_DISP, 0, AVERAGE_MODE_LOG_RECURSIVE);
  t_ready = 1;
  g_mutex_unlock(&t_lock);
  return 0;
}

void tx_analyzer_feed(const double *iq, int n_pairs) {
  if (!t_ready) { return; }
  g_mutex_lock(&t_lock);
  if (t_ready) {
    for (int i = 0; i < n_pairs; i++) {
      t_acc[t_fill * 2    ] = iq[i * 2    ];
      t_acc[t_fill * 2 + 1] = iq[i * 2 + 1];
      t_fill++;
      if (t_fill >= t_bf) {
        Spectrum0(1, TXA_DISP, 0, 0, t_acc);
        t_fill = 0;
      }
    }
  }
  g_mutex_unlock(&t_lock);
}

int tx_analyzer_get_pixels(float *out, int pixels) {
  int flag = 0;
  if (!t_ready || pixels < t_pixels) { return 0; }
  GetPixels(TXA_DISP, 0, out, &flag);
  return flag;
}

void tx_analyzer_destroy(void) {
  g_mutex_lock(&t_lock);
  if (t_ready) {
    DestroyAnalyzer(TXA_DISP);
    t_ready = 0;
  }
  g_free(t_acc); t_acc = NULL;
  t_fill = 0;
  g_mutex_unlock(&t_lock);
}
