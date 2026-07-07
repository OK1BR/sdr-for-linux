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

/* XCreateAnalyzer max FFT size (= piHPSDR's). The FFT grows with zoom up to this
 * cap, giving sharp zoom to A_MSIZE/pixels (128x at 2048 px). Bigger = deeper
 * sharp zoom + more FFTW memory. See docs/WDSP-ANALYZER-SCOPE.md. */
#define A_MSIZE  262144
/* Samples per Spectrum0 call (SetAnalyzer bf_sz); piHPSDR uses rx->buffer_size. */
#define A_BFSIZE 1024

static int      a_id;
static int      a_pixels;
static int      a_afft;
static int      a_rate;    /* IQ sample rate (full span)                       */
static int      a_fps;     /* target frames/s                                  */
static double   a_zoom = 1.0; /* current zoom factor (1 = full span)           */
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

/* (Re)configure the analyzer span. fsc = bins to clip from EACH end (centered,
 * pan=0): 0 = full span; a_afft*(1-1/zoom)/2 = zoomed. Shared by create + zoom;
 * caller holds a_lock. Detector/averaging are set once in create and persist. */
static void apply_analyzer(double fsc) {
  int    flp[1]    = { 0 };
  double keep_time = 0.1;
  int    overlap   = (int)fmax(0.0, ceil((double)a_afft - (double)a_rate / (double)a_fps));
  int    max_w     = a_afft + (int)fmin(keep_time * (double)a_rate,
                                        keep_time * (double)a_afft * (double)a_fps);
  SetAnalyzer(a_id, 1, 1, 1, flp, a_afft, A_BFSIZE, 5, 14.0, overlap, 0,
              fsc, fsc, a_pixels, 1, 0, 0.0, 0.0, max_w);
  /* Normalize to 1 Hz PSD (pass the true sample rate): the noise floor is then
   * invariant to both zoom and FFT size, so no per-zoom level compensation. */
  SetDisplayNormOneHz(a_id, 0, 1);
  SetDisplaySampleRate(a_id, a_rate);
}

/* Detector + averaging, per receiver.c:1941-2052 (LOG_RECURSIVE, PEAK); depends
 * on a_fps. The NONE-then-mode dance avoids a switch artifact (:1978-1980). */
static void apply_averaging(void) {
  double t    = 0.120;                                     /* 120 ms avg time */
  int    navg = (int)fmax(2.0, fmin(60.0, (double)a_fps * t));
  double avb  = exp(-1.0 / ((double)a_fps * t));
  SetDisplayDetectorMode(a_id, 0, DETECTOR_MODE_PEAK);
  SetDisplayAverageMode(a_id, 0, AVERAGE_MODE_NONE);
  SetDisplayNumAverage(a_id, 0, navg);
  SetDisplayAvBackmult(a_id, 0, avb);
  SetDisplayAverageMode(a_id, 0, AVERAGE_MODE_LOG_RECURSIVE);
}

int analyzer_create(int id, int pixels, int sample_rate, int fps) {
  int rc = -1;
  a_id     = id;
  a_pixels = pixels;
  a_afft   = clamp_afft(pixels);
  a_rate   = sample_rate;
  a_fps    = fps;
  a_zoom   = 1.0;
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

  apply_analyzer(0.0);   /* full span (no zoom/pan) — values per receiver.c:1736-1772 */
  apply_averaging();

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

void analyzer_set_fps(int fps) {
  if (fps < 1) { fps = 1; }
  g_mutex_lock(&a_lock);
  if (a_ready) {
    a_fps = fps;
    double zz = (double)a_afft * (1.0 - 1.0 / a_zoom);   /* keep current zoom */
    apply_analyzer(0.5 * zz);   /* overlap/max_w recompute from a_fps */
    apply_averaging();          /* navg/backmult recompute from a_fps */
  }
  g_mutex_unlock(&a_lock);
}

void analyzer_set_zoom(double zoom) {
  if (zoom < 1.0) { zoom = 1.0; }
  g_mutex_lock(&a_lock);
  if (a_ready) {
    a_afft = clamp_afft((int)ceil((double)a_pixels * zoom)); /* grow FFT for sharp deep zoom */
    double zz = (double)a_afft * (1.0 - 1.0 / zoom);         /* total bins to clip */
    apply_analyzer(0.5 * zz);                                /* centered (pan=0); recomputes overlap */
    a_zoom = zoom;
  }
  g_mutex_unlock(&a_lock);
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
