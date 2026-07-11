/*
 * sdrfl-panprobe — headless RX → WDSP analyzer → panadapter PNG.
 * Milestone-1 step-4 gate; protocol-agnostic since HL2 R2 (docs/P1-SCOPE.md).
 *
 * Discovers the radio (P2 first, P1/METIS round only when the pinned IP did
 * not answer P2 — same policy as start_radio), starts one receiver over the
 * radio's protocol, feeds the live IQ to the WDSP analyzer, averages a few
 * pixel frames, auto-levels them into the panadapter's dB window, and renders
 * panadapter + waterfall to a PNG with our existing pure-Cairo renderer.
 * Proves IQ → analyzer → our-renderer end-to-end.
 *
 *   TAKES THE RADIO — piHPSDR must be disconnected.
 *
 * SDRFL_SELFTEST=1 skips the radio entirely: it feeds a synthetic math-standard
 * complex tone (I=cos, Q=sin = +48 kHz) and checks the analyzer resolves it to a
 * sharp peak. NOTE: fed piHPSDR-style ([2i]=I,[2i+1]=Q), WDSP's Spectrum0 reads
 * the pair as (Q,I) and so images the tone to -48 kHz — this is expected and is
 * exactly what piHPSDR does (whose on-air orientation is correct). So the check
 * is: sharp peak at -tone ⇒ our feed order matches piHPSDR; a peak at +tone would
 * mean our feed order differs. An offline analyzer + feed-order check, no radio.
 *
 * Env: SDRFL_RADIO_IP (192.168.1.247), SDRFL_FREQ (14100000), SDRFL_RATE
 *      (192000), SDRFL_SECS (4), SDRFL_SOFFSET (auto), RENDER_OUT (panprobe.png)
 */
#include <cairo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "discovered.h"
#include "discovery.h"
#include "protocol1.h"
#include "protocol2.h"
#include "analyzer.h"
#include "client.h"       /* ClientFrame */
#include "panadapter.h"
#include "waterfall.h"

#define PIXELS 2048
#define FPS    10

static float g_raw[PIXELS];
static float g_ema[PIXELS];
static int   g_have_ema;
static int   g_frames;

static long long getenv_ll(const char *name, long long dflt) {
  const char *v = getenv(name);
  return (v && *v) ? strtoll(v, NULL, 10) : dflt;
}

static void feed_cb(const double *iq, int n_pairs, void *user) {
  (void)user;
  analyzer_feed(iq, n_pairs);
}

/* Poll one frame from the analyzer and fold it into the EMA. */
static void poll_frame(void) {
  if (analyzer_get_pixels(g_raw, PIXELS)) {
    if (!g_have_ema) {
      memcpy(g_ema, g_raw, sizeof(g_ema));
      g_have_ema = 1;
    } else {
      for (int j = 0; j < PIXELS; j++) { g_ema[j] += 0.35f * (g_raw[j] - g_ema[j]); }
    }
    g_frames++;
  }
}

static int cmp_float(const void *a, const void *b) {
  float fa = *(const float *)a, fb = *(const float *)b;
  return (fa > fb) - (fa < fb);
}

/* Offline: feed a synthetic +offset_hz complex tone, ~realtime paced. */
static void run_selftest(int rate, double offset_hz) {
  const int block = 256;
  double buf[2 * 256];
  double phase = 0.0, dphi = 2.0 * M_PI * offset_hz / (double)rate;
  int total = (int)(1.5 * rate / block);   /* ~1.5 s of samples */
  for (int b = 0; b < total; b++) {
    for (int i = 0; i < block; i++) {
      buf[2 * i]     = 0.1 * cos(phase);   /* I */
      buf[2 * i + 1] = 0.1 * sin(phase);   /* Q — +freq tone */
      phase += dphi;
      if (phase > 2.0 * M_PI) { phase -= 2.0 * M_PI; }
    }
    analyzer_feed(buf, block);
    if ((b % 60) == 0) { poll_frame(); }
    usleep(1000000 * block / rate);        /* ~realtime so WDSP keeps up */
  }
  for (int k = 0; k < 40 && g_frames < 30; k++) { poll_frame(); usleep(20000); }
}

int main(void) {
  long long freq = getenv_ll("SDRFL_FREQ", 14100000);
  int       rate = (int)getenv_ll("SDRFL_RATE", 192000);
  int       secs = (int)getenv_ll("SDRFL_SECS", 4);
  int       selftest = getenv("SDRFL_SELFTEST") != NULL;
  const char *out = getenv("RENDER_OUT");
  if (!out) { out = "panprobe.png"; }
  const int W = 1300, H = 680;

  if (selftest) {
    const double tone = 48000.0;
    printf("sdrfl-panprobe SELFTEST: synthetic %.0f Hz tone, %d Hz, %d px\n", tone, rate, PIXELS);
    if (analyzer_create(0, PIXELS, rate, FPS) != 0) { fprintf(stderr, "analyzer_create failed\n"); return 2; }
    run_selftest(rate, tone);
    analyzer_destroy();
    if (g_frames > 0) {
      int peak = 0;
      for (int j = 1; j < PIXELS; j++) { if (g_ema[j] > g_ema[peak]) peak = j; }
      double peak_hz = ((double)peak / PIXELS - 0.5) * rate;
      /* piHPSDR-style feed images a +tone to -tone (see file header). */
      printf("peak at pixel %d = %+.0f Hz (expected %+.0f Hz)\n", peak, peak_hz, -tone);
      printf("feed order: %s\n",
             fabs(peak_hz + tone) < rate / 20.0 ? "OK (matches piHPSDR)" :
             (fabs(peak_hz - tone) < rate / 20.0 ? "DIFFERS from piHPSDR feed order!" : "unexpected"));
    }
  } else {
    const char *ip = getenv("SDRFL_RADIO_IP");
    snprintf(ipaddr_radio, sizeof(ipaddr_radio), "%s", (ip && *ip) ? ip : "192.168.1.247");
    printf("sdrfl-panprobe: discovering %s ...\n", ipaddr_radio);
    p2_discovery();
    /* P1 (METIS) round only when the pinned IP wasn't answered by P2 — same
     * policy as start_radio (a P2 gate run pays no extra probe time). */
    {
      int need_p1 = 1;
      for (int i = 0; i < devices; i++) {
        if (strcmp(inet_ntoa(discovered[i].network.address.sin_addr), ipaddr_radio) == 0) {
          need_p1 = 0;
          break;
        }
      }
      if (need_p1) { p1_discovery(); }
    }
    if (devices <= 0) { fprintf(stderr, "no radio found\n"); return 1; }
    const DISCOVERED *dev = NULL;
    for (int i = 0; i < devices && !dev; i++) {
      if (strcmp(inet_ntoa(discovered[i].network.address.sin_addr), ipaddr_radio) == 0) {
        dev = &discovered[i];
      }
    }
    if (!dev) { dev = &discovered[0]; }
    int p1 = (dev->protocol == ORIGINAL_PROTOCOL);
    printf("using %s at %s (protocol %d)\n", dev->name,
           inet_ntoa(dev->network.address.sin_addr), p1 ? 1 : 2);
    if (dev->status == 3) {
      fprintf(stderr, "radio is IN USE by another program — close it first\n");
      return 1;
    }

    if (analyzer_create(0, PIXELS, rate, FPS) != 0) { fprintf(stderr, "analyzer_create failed\n"); return 2; }
    printf("analyzer up (%d px, %d Hz, %d fps); RX %lld Hz, collecting %d s ...\n",
           PIXELS, rate, FPS, freq, secs);
    if ((p1 ? p1_rx_start(dev, freq, rate, feed_cb, NULL)
            : p2_rx_start(dev, freq, rate, feed_cb, NULL)) != 0) {
      fprintf(stderr, "%s failed\n", p1 ? "p1_rx_start" : "p2_rx_start");
      analyzer_destroy();
      return 3;
    }
    for (int i = 0; i < secs * FPS; i++) { poll_frame(); usleep(1000000 / FPS); }
    if (p1) { p1_rx_stop(); } else { p2_rx_stop(); }
    analyzer_destroy();
  }

  const char *status = (g_frames == 0) ? "no analyzer frames" : NULL;

  /* Auto-level: WDSP pixels are relative dB, not dBm. Shift the ~20th-percentile
   * noise floor to TARGET_FLOOR so the trace lands in the panadapter's window.
   * SDRFL_SOFFSET overrides. */
  const double TARGET_FLOOR = -115.0;
  float disp[PIXELS];
  double s_peak = -140.0;
  if (g_frames > 0) {
    static float sorted[PIXELS];
    memcpy(sorted, g_ema, sizeof(sorted));
    qsort(sorted, PIXELS, sizeof(float), cmp_float);
    double floor_db = sorted[(int)(PIXELS * 0.20)];
    double peak_db  = sorted[PIXELS - 1];
    const char *so = getenv("SDRFL_SOFFSET");
    double soffset = (so && *so) ? strtod(so, NULL) : (TARGET_FLOOR - floor_db);
    for (int j = 0; j < PIXELS; j++) {
      disp[j] = (float)(g_ema[j] + soffset);
      if (disp[j] > s_peak) { s_peak = disp[j]; }
    }
    printf("frames=%d  raw dB: floor(p20)=%.1f peak=%.1f  soffset=%.1f  → dBm floor=%.1f peak=%.1f\n",
           g_frames, floor_db, peak_db, soffset, floor_db + soffset, peak_db + soffset);
  }

  ClientFrame f;
  memset(&f, 0, sizeof(f));
  f.width      = PIXELS;
  f.vfo_a_freq = freq;
  f.s_dbm      = s_peak;

  Waterfall *wf = waterfall_new();
  if (g_frames > 0) {
    static uint8_t bytes[PIXELS];
    for (int j = 0; j < PIXELS; j++) {
      double b = disp[j] + 200.0;   /* waterfall convention: dBm = byte-200 */
      bytes[j] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
    }
    waterfall_push(wf, bytes, PIXELS);
  }

  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
  cairo_t *cr = cairo_create(surf);
  if (status) {
    panadapter_draw(cr, W, H, NULL, NULL, 0, 1, status, NULL, 0.5);
  } else {
    double low, span;
    waterfall_range(wf, &low, &span);
    int ph = H / 2;
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, W, ph);
    cairo_clip(cr);
    panadapter_draw(cr, W, ph, &f, disp, low, span, NULL, NULL, 0.5);
    cairo_restore(cr);
    waterfall_draw(wf, cr, 0, ph, W, H - ph);
  }
  cairo_surface_flush(surf);
  cairo_status_t st = cairo_surface_write_to_png(surf, out);
  fprintf(stderr, "wrote %s (%s)\n", out, cairo_status_to_string(st));

  cairo_destroy(cr);
  cairo_surface_destroy(surf);
  waterfall_free(wf);
  return (g_frames > 0 && st == CAIRO_STATUS_SUCCESS) ? 0 : 1;
}
