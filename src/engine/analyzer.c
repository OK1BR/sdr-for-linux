/*
 * sdr-for-linux — thin WDSP analyzer wrapper. See analyzer.h.
 *
 * Mirrors the analyzer setup/feed/readout of piHPSDR receiver.c @ 974acba
 * (rx_set_analyzer 1663-1776, rx_full_buffer feed 1340-1344, rx_get_pixels
 * 1616-1621, rx_set_average 1941-1981) but with our own minimal state and no
 * WDSP demod channel (analyzer only — no OpenChannel/fexchange0).
 */
#include <glib.h>
#include <math.h>
#include <string.h>

#include "wdsp.h"      /* WDSP analyzer API + DETECTOR_/AVERAGE_ constants */
#include "analyzer.h"

/* XCreateAnalyzer max FFT size. 16384 = zoom-1 build (low memory); raising it
 * is all that a future zoom needs. See docs/WDSP-ANALYZER-SCOPE.md decision A. */
#define A_MSIZE  16384
/* Samples per Spectrum0 call (SetAnalyzer bf_sz); piHPSDR uses rx->buffer_size. */
#define A_BFSIZE 1024

static int      a_id;
static int      a_pixels;
static int      a_afft;
static double  *a_acc;     /* accumulator: 2*A_BFSIZE doubles (I,Q interleaved) */
static int      a_fill;    /* sample pairs currently in a_acc                   */
static int      a_ready;   /* analyzer created                                  */
static GMutex   a_lock;    /* fences feed vs create/destroy (static → zero-init) */

/* Clamp the requested FFT size up to WDSP's allowed set, capped at A_MSIZE. */
static int clamp_afft(int want) {
  int sz = 16384;
  if      (want <= 16384)  sz = 16384;
  else if (want <= 32768)  sz = 32768;
  else if (want <= 65536)  sz = 65536;
  else if (want <= 131072) sz = 131072;
  else                     sz = 262144;
  return sz > A_MSIZE ? A_MSIZE : sz;
}

int analyzer_create(int id, int pixels, int sample_rate, int fps) {
  int rc = -1;
  a_id     = id;
  a_pixels = pixels;
  a_afft   = clamp_afft(pixels);
  a_acc    = g_new0(double, 2 * A_BFSIZE);
  a_fill   = 0;

  g_mutex_lock(&a_lock);
  XCreateAnalyzer(id, &rc, A_MSIZE, 1, 1, NULL);
  if (rc != 0) {
    g_mutex_unlock(&a_lock);
    g_free(a_acc);
    a_acc = NULL;
    return -1;
  }

  int    flp[1]    = { 0 };
  double keep_time = 0.1;
  int    overlap   = (int)fmax(0.0, ceil((double)a_afft - (double)sample_rate / (double)fps));
  int    max_w     = a_afft + (int)fmin(keep_time * (double)sample_rate,
                                        keep_time * (double)a_afft * (double)fps);

  /* SetAnalyzer — 19 args, values per receiver.c:1736-1755 (zoom=1, pan=0). */
  SetAnalyzer(id,
              1,          /* n_pixout                                          */
              1,          /* n_fft — no spur elimination                       */
              1,          /* typ — complex I&Q                                 */
              flp,        /* low-side LO, no flip                              */
              a_afft,     /* sz — FFT size                                     */
              A_BFSIZE,   /* bf_sz — samples per Spectrum0                     */
              5,          /* win_type — Kaiser                                 */
              14.0,       /* pi — Kaiser beta                                  */
              overlap,    /* ovrlp                                             */
              0,          /* clp                                               */
              0.0, 0.0,   /* fscLin/fscHin — full span (no zoom/pan)           */
              pixels,     /* n_pix — output columns                            */
              1,          /* n_stch                                            */
              0,          /* calset                                            */
              0.0, 0.0,   /* fmin/fmax — calibration off                       */
              max_w);     /* max input write-ahead                             */

  /* Bandwidth normalization, per receiver.c:1771-1772 (pass width*zoom=pixels). */
  SetDisplayNormOneHz(id, 0, 1);
  SetDisplaySampleRate(id, pixels);

  /* Averaging + detector, per receiver.c:1941-2052 (LOG_RECURSIVE, PEAK). The
   * NONE-then-mode dance avoids a switch artifact (receiver.c:1978-1980). */
  double t    = 0.120;                                        /* 120 ms avg time */
  int    navg = (int)fmax(2.0, fmin(60.0, (double)fps * t));
  double avb  = exp(-1.0 / ((double)fps * t));
  SetDisplayDetectorMode(id, 0, DETECTOR_MODE_PEAK);
  SetDisplayAverageMode(id, 0, AVERAGE_MODE_NONE);
  SetDisplayNumAverage(id, 0, navg);
  SetDisplayAvBackmult(id, 0, avb);
  SetDisplayAverageMode(id, 0, AVERAGE_MODE_LOG_RECURSIVE);

  a_ready = 1;
  g_mutex_unlock(&a_lock);
  return 0;
}

void analyzer_feed(const double *iq, int n_pairs) {
  if (!a_ready) { return; }
  g_mutex_lock(&a_lock);
  if (a_ready) {
    for (int i = 0; i < n_pairs; i++) {
      a_acc[a_fill * 2    ] = iq[i * 2    ];   /* I (feed 1:1 as piHPSDR does) */
      a_acc[a_fill * 2 + 1] = iq[i * 2 + 1];   /* Q                            */
      a_fill++;
      if (a_fill >= A_BFSIZE) {
        Spectrum0(1, a_id, 0, 0, a_acc);
        a_fill = 0;
      }
    }
  }
  g_mutex_unlock(&a_lock);
}

int analyzer_get_pixels(float *out, int pixels) {
  int flag = 0;
  if (!a_ready || pixels < a_pixels) { return 0; }
  /* GetPixels is lock-free per WDSP's triple-buffer; the render thread and
   * analyzer_destroy are serialized by the caller, so no guard needed here. */
  GetPixels(a_id, 0, out, &flag);
  return flag;
}

void analyzer_destroy(void) {
  g_mutex_lock(&a_lock);
  if (a_ready) {
    DestroyAnalyzer(a_id);
    a_ready = 0;
  }
  g_free(a_acc);
  a_acc = NULL;
  g_mutex_unlock(&a_lock);
}
