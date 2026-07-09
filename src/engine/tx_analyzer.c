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
static int     t_iq_rate;   /* TX IQ rate (Hz) — clip reference */
static int     t_fps;
static double  t_span;      /* current display span (Hz) */
static double *t_acc;       /* accumulator 2*t_bf doubles */
static int     t_fill;
static int     t_ready;
static GMutex  t_lock;

/* (Re)configure the WDSP analyzer for a `span_hz`-wide window: pick afft so the
 * surviving bins ≈ the pixel count (sharp at any zoom, like piHPSDR re-zoom),
 * clip the rest of the 192 kHz IQ. Same FFT sizes as the RX analyzer, so FFTW
 * wisdom covers them → no re-plan stutter. Must hold t_lock. */
static void tx_ana_configure(double span_hz) {
  if (span_hz > (double)t_iq_rate) { span_hz = (double)t_iq_rate; }   /* can't exceed the TX IQ */
  if (span_hz < 300.0)             { span_hz = 300.0; }
  t_span = span_hz;
  int want = (int)(((long)t_iq_rate * (long)t_pixels) / (long)span_hz);
  if      (want <=  16384) { t_afft =  16384; }
  else if (want <=  32768) { t_afft =  32768; }
  else if (want <=  65536) { t_afft =  65536; }
  else if (want <= 131072) { t_afft = 131072; }
  else                     { t_afft = 262144; }

  double clipf   = (double)t_afft * (0.5 - (span_hz * 0.5) / (double)t_iq_rate);  /* each side */
  double keep_t  = 0.1;
  int    overlap = (int)fmax(0.0, ceil((double)t_afft - (double)t_iq_rate / (double)t_fps));
  int    max_w   = t_afft + (int)fmin(keep_t * (double)t_iq_rate,
                                      keep_t * (double)t_afft * (double)t_fps);
  int    flp[1]  = { 0 };
  SetAnalyzer(TXA_DISP, 1, 1, 1 /*complex*/, flp, t_afft, t_bf, 5 /*window*/, 14.0,
              overlap, 0, clipf, clipf, t_pixels, 1, 0, 0.0, 0.0, max_w);
}

int tx_analyzer_create(int pixels, int iq_rate, int bf_size, int fps) {
  int rc = -1;
  if (pixels <= 0 || iq_rate <= 0 || bf_size <= 0) { return -1; }
  t_pixels  = pixels;
  t_bf      = bf_size;
  t_iq_rate = iq_rate;
  if (fps < 1) { fps = 1; }
  t_fps     = fps;

  t_acc  = g_new0(double, 2 * t_bf);
  t_fill = 0;

  g_mutex_lock(&t_lock);
  XCreateAnalyzer(TXA_DISP, &rc, 262144, 1, 1, NULL);
  if (rc != 0) {
    g_mutex_unlock(&t_lock);
    g_free(t_acc); t_acc = NULL;
    return -1;
  }
  tx_ana_configure((double)iq_rate);   /* default: full TX IQ; the GUI sets the zoomed span */
  SetDisplayNormOneHz(TXA_DISP, 0, 1);
  SetDisplaySampleRate(TXA_DISP, iq_rate);
  /* Detector + very light averaging so the TX display tracks fast CW keying (the
   * GUI adds a short EMA on top). Small time constant = snappy on/off. */
  double t    = 0.008;
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

void tx_analyzer_set_span(double span_hz) {
  g_mutex_lock(&t_lock);
  if (t_ready && span_hz > 0.0) { tx_ana_configure(span_hz); }
  g_mutex_unlock(&t_lock);
}

double tx_analyzer_base_span(void) { return (double)t_iq_rate; }   /* max span = full TX IQ */

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
