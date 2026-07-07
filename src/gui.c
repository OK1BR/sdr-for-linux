/*
 * gui.c — GTK4 panadapter + waterfall window for sdr-for-linux.
 *
 * Two data sources, same renderer (panadapter.{h,c} / waterfall.{h,c}):
 *   - default: DIRECT RADIO — HPSDR Protocol-2 discovery + RX (engine/protocol2)
 *     feeding the WDSP analyzer (engine/analyzer); full float pixels. TAKES THE
 *     radio (one owner) — piHPSDR must be disconnected.
 *   - `--server [host [port [pwd]]]`: the v0 network path (client.{h,c}) — a
 *     remote head onto a running piHPSDR server.
 *
 * A GdkFrameClock tick pulls the latest frame (analyzer or network) and redraws.
 *
 * Usage:  sdr-for-linux [--server [host [port [pwd]]]]
 *   env:  SDRFL_RADIO_IP, SDRFL_FREQ, SDRFL_RATE, SDRFL_SOFFSET   (radio mode)
 *         PIHPSDR_PWD                                             (server mode)
 * Run with GSK_RENDERER=cairo to avoid the NVIDIA+Wayland GTK4 GL crash.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <gtk/gtk.h>
#include <adwaita.h>
#include <glib-unix.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "client.h"
#include "panadapter.h"
#include "waterfall.h"

#include <strings.h>

#include "discovered.h"
#include "discovery.h"
#include "protocol2.h"
#include "analyzer.h"
#include "demod.h"
#include "audio.h"
#include "settings.h"
#include "wisdom_gate.h"

#define ENGINE_PIXELS 2048
#define ENGINE_FPS    25
/* Frames to average before locking the display offset (~1 s at ENGINE_FPS). */
#define SETTLE_FRAMES 15
/* Where the noise floor lands after auto-levelling (dBm, inside the pan window). */
#define TARGET_FLOOR  -115.0

typedef struct {
  int         radio_mode;   /* 1 = direct radio, 0 = network server            */

  /* network (server) path */
  Client     *client;
  int         conn_err;

  /* radio (engine) path */
  int         engine_ok;    /* discovery + RX started                          */
  int         audio_ok;     /* demod + audio sink up                           */
  long long   freq;         /* DDC centre = tuned frequency (Model A)          */
  int         mode;         /* current demod mode (DEMOD_*)                    */
  int         rate;         /* IQ sample rate = full panadapter span (Hz)      */
  double      volume;       /* AF gain (dB)                                    */
  double      gain;         /* digital master gain                            */
  int         atten;        /* ADC0 step attenuator (dB, 0-31)                 */
  int         agc;          /* AGC mode 0=off,1=long,2=slow,3=med,4=fast       */
  double      agc_gain;     /* AGC-T threshold/gain (dB)                       */
  int         nr, nb, anf;  /* noise reduction / blanker / auto-notch on-off   */
  int         fps;          /* panadapter frame rate                          */
  int         latency;      /* audio target latency (ms)                      */
  char        radio_ip[64]; /* resolved radio IP (for persistence)            */
  guint       save_timer_id;/* debounced settings save (0 = none pending)     */
  long long   drag_base_freq; /* app->freq at drag-begin (pan is absolute)     */
  int         pixels;
  float       eng_raw[SPECTRUM_DATA_SIZE];
  double      soffset;
  int         soffset_locked;
  int         cal_frames;

  int         connected;    /* data source is up (either path)                 */

  ClientFrame frame;        /* metadata for the readouts (+ dbm on net path)   */
  uint64_t    last_seq;
  int         have_frame;

  /* Time-averaged trace, held in dBm (so the renderer is source-agnostic). */
  float       ema[SPECTRUM_DATA_SIZE];
  int         ema_w;

  Waterfall  *wf;
  GtkWidget  *area;
  GtkWidget  *mode_btns[7];  /* toggle per DEMOD_* id (keys ↔ buttons in sync) */
  double      zoom;          /* current display zoom (1 = full span)           */
  long long   tune_step;     /* scroll-tuning step (Hz); freq snaps to it       */
  GtkWidget  *step_dd;       /* footer step selector                           */
  GtkWidget  *span_label;    /* footer span readout                            */
  double      pending_zoom;  /* slider target; applied ≤1×/frame in tick_cb    */
  int         zoom_dirty;    /* pending_zoom needs applying                    */
  int         filter_idx;    /* selected preset in the current mode's table    */
  double      flo, fhi;      /* current passband (Hz, rel. centre) — for drawing */
  GtkWidget  *filter_dd;     /* filter dropdown (repopulated per mode)         */
  gint64      ovl0_until;    /* ADC0-overload badge lit until (monotonic µs)   */
  gint64      ovl1_until;    /* ADC1-overload badge lit until (monotonic µs)   */
  int         tlm_valid;     /* HP-status telemetry seen at least once          */
  double      supply_v;      /* supply voltage (V), EMA-smoothed; 0 = unknown   */
} App;

/* Supply-voltage calibration: V = k * raw_adc1. No G1 divider is documented, so
 * k is anchored empirically — Richard's Microset measured 13.46 V while the G1
 * reported raw_adc1 ~= 797.5 (bytes 55-56). SDRFL_VOLT_CAL overrides k (V per
 * count) for a precise re-trim without a rebuild. */
#define SUPPLY_V_PER_COUNT (13.46 / 797.5)
#define SUPPLY_V_EMA 0.1   /* smooths the ±1-count (~±0.017 V) raw jitter */

/* How long an ADC-overload badge stays lit after a clip (µs). A clip may span
 * just one 50 ms status packet — hold it so it's visible at any frame rate. */
#define ADC_OVL_HOLD_US 700000

#define PANADAPTER_FRACTION 0.5
#define EMA_FACTOR 0.55f

/* Scroll-tuning steps (Hz): the frequency snaps to a multiple of the active one.
 * Labels are NULL-terminated for gtk_drop_down_new_from_strings(). */
static const long long TUNE_STEPS[] = { 1, 10, 100, 1000, 10000 };
static const char * const TUNE_STEP_LABELS[] = { "1 Hz", "10 Hz", "100 Hz", "1 kHz", "10 kHz", NULL };
#define TUNE_STEP_DEFAULT 100

static void feed_cb(const double *iq, int n_pairs, void *user) {
  (void)user;
  analyzer_feed(iq, n_pairs);   /* panadapter */
  demod_feed(iq, n_pairs);      /* audio */
}

/* Filter presets per mode — piHPSDR filter.c @ 974acba (named presets; Var later). */
typedef struct { int low, high; const char *name; } FilterPreset;

static const FilterPreset FILT_LSB[] = {
  {-5150,-150,"5.0k"},{-4550,-150,"4.4k"},{-3950,-150,"3.8k"},{-3450,-150,"3.3k"},
  {-3050,-150,"2.9k"},{-2850,-150,"2.7k"},{-2550,-150,"2.4k"},{-2250,-150,"2.1k"},
  {-1950,-150,"1.8k"},{-1150,-150,"1.0k"},
};
static const FilterPreset FILT_USB[] = {
  {150,5150,"5.0k"},{150,4550,"4.4k"},{150,3950,"3.8k"},{150,3450,"3.3k"},
  {150,3050,"2.9k"},{150,2850,"2.7k"},{150,2550,"2.4k"},{150,2250,"2.1k"},
  {150,1950,"1.8k"},{150,1150,"1.0k"},
};
static const FilterPreset FILT_CW[] = {   /* CWL/CWU: symmetric around the CW pitch */
  {-500,500,"1.0k"},{-400,400,"800"},{-375,375,"750"},{-300,300,"600"},
  {-250,250,"500"},{-200,200,"400"},{-125,125,"250"},{-50,50,"100"},
  {-25,25,"50"},{-13,13,"25"},
};
static const FilterPreset FILT_AM[] = {
  {-8000,8000,"16k"},{-6000,6000,"12k"},{-5000,5000,"10k"},{-4000,4000,"8k"},
  {-3300,3300,"6.6k"},{-2600,2600,"5.2k"},{-2000,2000,"4.0k"},{-1550,1550,"3.1k"},
  {-1450,1450,"2.9k"},{-1200,1200,"2.4k"},
};

/* Filter table + count + default index for a mode. */
static const FilterPreset *mode_filters(int mode, int *n, int *deflt) {
  *n = 10;
  switch (mode) {
    case DEMOD_LSB: *deflt = 5; return FILT_LSB;   /* 2.7k */
    case DEMOD_USB: *deflt = 5; return FILT_USB;   /* 2.7k */
    case DEMOD_CWL:
    case DEMOD_CWU: *deflt = 4; return FILT_CW;    /* 500  */
    case DEMOD_AM:  *deflt = 4; return FILT_AM;    /* 6.6k */
    default:        *deflt = 5; return FILT_USB;
  }
}

/* Parse a mode name (USB/LSB/CWU/CWL/AM) to a DEMOD_* id; -1 if unknown/NULL. */
static int mode_from_name(const char *m) {
  if      (m && !strcasecmp(m, "usb")) return DEMOD_USB;
  else if (m && !strcasecmp(m, "lsb")) return DEMOD_LSB;
  else if (m && !strcasecmp(m, "cwu")) return DEMOD_CWU;
  else if (m && !strcasecmp(m, "cwl")) return DEMOD_CWL;
  else if (m && !strcasecmp(m, "am"))  return DEMOD_AM;
  return -1;
}

static int cmp_float(const void *a, const void *b) {
  float fa = *(const float *)a, fb = *(const float *)b;
  return (fa > fb) - (fa < fb);
}

static void draw_cb(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data) {
  (void)area;
  App *app = (App *)data;

  if (!app->connected) {
    const char *msg = app->radio_mode ? "No radio found on the LAN"
                                       : client_strerror(app->conn_err);
    char buf[160];
    snprintf(buf, sizeof(buf), "Not connected: %s", msg);
    panadapter_draw(cr, w, h, NULL, NULL, 0, 1, buf);
    return;
  }
  if (!app->have_frame) {
    panadapter_draw(cr, w, h, NULL, NULL, 0, 1,
                    app->radio_mode ? "Radio up — calibrating…" : "Connected — waiting for spectrum…");
    return;
  }

  int ph = (int)(h * PANADAPTER_FRACTION);
  if (ph < 1) ph = 1;

  double low, span;
  waterfall_range(app->wf, &low, &span);

  const float *smoothed = (app->ema_w == app->frame.width) ? app->ema : NULL;
  cairo_save(cr);
  cairo_rectangle(cr, 0, 0, w, ph);
  cairo_clip(cr);
  panadapter_draw(cr, w, ph, &app->frame, smoothed, low, span, NULL);
  /* Filter passband overlay (Model A: VFO = span centre). Scales with zoom. */
  if (app->radio_mode && app->fhi > app->flo) {
    double hz_per_px = (double)app->rate / app->zoom / w;
    double cx = w / 2.0;
    double x0 = cx + app->flo / hz_per_px;
    double x1 = cx + app->fhi / hz_per_px;
    cairo_set_source_rgba(cr, 0.35, 0.75, 1.0, 0.12);   /* passband fill  */
    cairo_rectangle(cr, x0, 0, x1 - x0, ph);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.55, 0.85, 1.0, 0.55);   /* passband edges */
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x0 + 0.5, 0); cairo_line_to(cr, x0 + 0.5, ph);
    cairo_move_to(cr, x1 - 0.5, 0); cairo_line_to(cr, x1 - 0.5, ph);
    cairo_stroke(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.35);     /* VFO centre line */
    cairo_move_to(cr, cx + 0.5, 0); cairo_line_to(cr, cx + 0.5, ph);
    cairo_stroke(cr);
  }
  cairo_restore(cr);

  /* ADC-overload badge, top-left of the panadapter. Warns the input is
   * clipping → add attenuation. Held ADC_OVL_HOLD_US after the last clip. */
  gint64 now = g_get_monotonic_time();
  int ovl0 = now < app->ovl0_until;
  int ovl1 = now < app->ovl1_until;
  if (ovl0 || ovl1) {
    const char *txt = (ovl0 && ovl1) ? "ADC0+1 OVL" : ovl0 ? "ADC0 OVL" : "ADC1 OVL";
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12.0);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, txt, &ext);
    const double pad = 5.0, bx = 8.0, by = 8.0;
    cairo_set_source_rgba(cr, 0.78, 0.06, 0.06, 0.85);   /* red badge */
    cairo_rectangle(cr, bx, by, ext.width + 2 * pad, ext.height + 2 * pad);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.96);
    cairo_move_to(cr, bx + pad - ext.x_bearing, by + pad - ext.y_bearing);
    cairo_show_text(cr, txt);
  }

  /* Supply-voltage readout, top-right of the panadapter. Green in-band, amber
   * on a mild excursion, red on a fault. Anchored to a single multimeter point
   * (see SUPPLY_V_PER_COUNT) — a warning gauge, not a precise DVM. */
  if (app->radio_mode && app->supply_v > 0.0) {
    double v = app->supply_v;
    double r, g, b;
    if      (v < 12.0 || v > 15.0) { r = 0.95; g = 0.15; b = 0.15; }  /* fault */
    else if (v < 12.8 || v > 14.5) { r = 0.98; g = 0.72; b = 0.10; }  /* warn  */
    else                           { r = 0.55; g = 0.95; b = 0.55; }  /* ok    */
    char vb[32];
    snprintf(vb, sizeof vb, "%.2f V", v);
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 13.0);
    cairo_text_extents_t ex;
    cairo_text_extents(cr, vb, &ex);
    const double pad = 5.0;
    double bw = ex.width + 2 * pad, bx = w - bw - 8.0, by = 8.0;
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.45);
    cairo_rectangle(cr, bx, by, bw, ex.height + 2 * pad);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, r, g, b, 0.96);
    cairo_move_to(cr, bx + pad - ex.x_bearing, by + pad - ex.y_bearing);
    cairo_show_text(cr, vb);
  }

  waterfall_draw(app->wf, cr, 0, ph, w, h - ph);

  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
  cairo_rectangle(cr, 0, ph - 1, w, 2);
  cairo_fill(cr);
}

/* Network path: pull a decoded frame and fold it into the dBm EMA. */
static void tick_network(App *app, GtkWidget *widget) {
  if (!client_latest(app->client, &app->frame, &app->last_seq)) { return; }
  const ClientFrame *f = &app->frame;
  if (app->ema_w != f->width) {
    for (int i = 0; i < f->width; i++) { app->ema[i] = (float)f->dbm[i] - 200.0f; }
    app->ema_w = f->width;
  } else {
    for (int i = 0; i < f->width; i++) {
      app->ema[i] += EMA_FACTOR * ((float)f->dbm[i] - 200.0f - app->ema[i]);
    }
  }
  waterfall_push(app->wf, f->dbm, f->width);
  app->have_frame = 1;
  gtk_widget_queue_draw(widget);
}

/* Radio path: pull one analyzer frame, auto-level it into dBm, EMA it. The EMA
 * holds relative dB until the offset locks, then is shifted once into dBm; after
 * that the incoming relative-dB pixels are offset on the fly. */
static void tick_radio(App *app, GtkWidget *widget) {
  if (!analyzer_get_pixels(app->eng_raw, app->pixels)) { return; }
  const float *raw = app->eng_raw;
  int n = app->pixels;

  if (app->ema_w != n) {
    memcpy(app->ema, raw, n * sizeof(float));
    app->ema_w = n;
    app->cal_frames = 0;
  } else {
    /* Analyzer uses 1 Hz PSD norm → floor is zoom-invariant; no compensation. */
    double so = app->soffset_locked ? app->soffset : 0.0;
    for (int i = 0; i < n; i++) {
      app->ema[i] += EMA_FACTOR * ((float)(raw[i] + so) - app->ema[i]);
    }
  }
  app->cal_frames++;

  if (!app->soffset_locked) {
    if (app->cal_frames < SETTLE_FRAMES) { return; }   /* still calibrating */
    /* Lock: measure the ~20th-percentile noise floor and shift EMA to dBm. */
    static float sorted[SPECTRUM_DATA_SIZE];
    memcpy(sorted, app->ema, n * sizeof(float));
    qsort(sorted, n, sizeof(float), cmp_float);
    double floor_db = sorted[(int)(n * 0.20)];
    const char *so = getenv("SDRFL_SOFFSET");
    app->soffset = (so && *so) ? strtod(so, NULL) : (TARGET_FLOOR - floor_db);
    for (int i = 0; i < n; i++) { app->ema[i] += (float)app->soffset; }
    app->soffset_locked = 1;
    printf("radio: display locked — soffset=%.1f, floor=%.1f dBm, %d px; rendering\n",
           app->soffset, floor_db + app->soffset, n);
    fflush(stdout);
  }

  /* SDRFL_DEBUG_LEVELS: once per second dump the RAW analyzer levels (no
   * soffset). Note our display norm is 1 Hz PSD (SetDisplaySampleRate=rate);
   * piHPSDR norms per pixel width (SetDisplaySampleRate=width*zoom), so
   * piHPSDR-equivalent dBm = raw + 10*log10(rate/(zoom*n)) ... *n/n_px_pihpsdr
   * — do the conversion offline, here we just log the raw numbers. */
  static int dbg = -1;
  if (dbg < 0) { dbg = getenv("SDRFL_DEBUG_LEVELS") != NULL; }
  if (dbg) {
    static gint64 dbg_next = 0;
    gint64 now = g_get_monotonic_time();
    if (now >= dbg_next) {
      dbg_next = now + G_USEC_PER_SEC;
      static float dsort[SPECTRUM_DATA_SIZE];
      memcpy(dsort, raw, n * sizeof(float));
      qsort(dsort, n, sizeof(float), cmp_float);
      int   pk_i = 0;
      for (int i = 1; i < n; i++) { if (raw[i] > raw[pk_i]) { pk_i = i; } }
      double hz_per_px = (double)app->rate / app->zoom / n;
      printf("spectrum: raw peak=%.1f @%+.1f kHz | floor20=%.1f | median=%.1f "
             "(n=%d, zoom=%g, rate=%d, soffset=%+.1f, pixnorm_delta=%+.1f dB)\n",
             raw[pk_i], (pk_i - n / 2) * hz_per_px / 1000.0,
             dsort[(int)(n * 0.20)], dsort[n / 2],
             n, app->zoom, app->rate, app->soffset,
             10.0 * log10((double)app->rate / (app->zoom * (double)n)));
      fflush(stdout);
    }
  }

  /* EMA now in dBm. Build metadata + waterfall bytes. */
  app->frame.width      = n;
  app->frame.vfo_a_freq = app->freq;
  double peak = app->ema[0];
  static uint8_t bytes[SPECTRUM_DATA_SIZE];
  for (int i = 0; i < n; i++) {
    if (app->ema[i] > peak) { peak = app->ema[i]; }
    /* Waterfall from the analyzer pixels (WDSP averaging only) + the locked
     * offset — NOT the extra GUI EMA the trace uses. That double smoothing was
     * what blurred the waterfall; the raw-er feed keeps signal streaks crisp. */
    double b = (double)raw[i] + app->soffset + 200.0;
    bytes[i] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
  }
  app->frame.s_dbm = peak;
  waterfall_push(app->wf, bytes, n);
  app->have_frame = 1;
  gtk_widget_queue_draw(widget);
}

static void update_span_label(App *app);   /* fwd: zoom slider updates it via tick */

static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
  (void)clock;
  App *app = (App *)data;
  if (app->radio_mode && app->zoom_dirty) {   /* coalesce slider events: ≤1 reconfig/frame */
    analyzer_set_zoom(app->pending_zoom);
    app->zoom = app->pending_zoom;
    app->zoom_dirty = 0;
    update_span_label(app);
  }
  if (app->connected) {
    if (app->radio_mode) {
      /* Poll HP-status telemetry once per frame (read-and-clear → single
       * consumer). Latch the overload badges with a hold; raw analog words
       * are parsed but not shown until a live voltage calibration exists. */
      p2_telemetry t;
      p2_get_telemetry(&t);
      gint64 now = g_get_monotonic_time();
      if (t.adc0_overload) { app->ovl0_until = now + ADC_OVL_HOLD_US; }
      if (t.adc1_overload) { app->ovl1_until = now + ADC_OVL_HOLD_US; }
      app->tlm_valid = t.valid;
      if (t.valid && t.raw_adc1 > 0) {
        static double kcal = -1.0;
        if (kcal < 0.0) {
          const char *e = getenv("SDRFL_VOLT_CAL");
          kcal = (e && *e) ? atof(e) : SUPPLY_V_PER_COUNT;
        }
        double v = t.raw_adc1 * kcal;
        app->supply_v = (app->supply_v > 0.0)
                        ? app->supply_v + SUPPLY_V_EMA * (v - app->supply_v) : v;
      }
      tick_radio(app, widget);
    } else {
      tick_network(app, widget);
    }
  }
  return G_SOURCE_CONTINUE;
}

/* ---- persistent state (see settings.h) ----------------------------------- */

static void app_to_settings(const App *app, Settings *s) {
  g_strlcpy(s->ip, app->radio_ip, sizeof(s->ip));
  s->freq   = app->freq;
  s->rate   = app->rate;
  s->mode   = app->mode;
  s->volume  = app->volume;
  s->gain    = app->gain;
  s->fps     = app->fps;
  s->latency = app->latency;
  s->step    = (int)app->tune_step;
  s->zoom    = app->zoom;
  s->atten   = app->atten;
  s->agc     = app->agc;
  s->agc_gain = app->agc_gain;
  s->filter  = app->filter_idx;
  s->nr      = app->nr;
  s->nb      = app->nb;
  s->anf     = app->anf;
}

static gboolean do_save_cb(gpointer data) {
  App *app = (App *)data;
  app->save_timer_id = 0;
  Settings s;
  app_to_settings(app, &s);
  settings_save(&s);
  return G_SOURCE_REMOVE;
}

/* Debounced save: write ~1 s after the last change (rapid tuning coalesces). */
static void schedule_save(App *app) {
  if (!app->radio_mode) { return; }
  if (app->save_timer_id) { g_source_remove(app->save_timer_id); }
  app->save_timer_id = g_timeout_add_seconds(1, do_save_cb, app);
}

/* Mouse wheel over the panadapter re-tunes the DDC (Model A: the whole span
 * moves, passband stays centred) by the selected step. Each notch snaps to the
 * next multiple of the step in the scroll direction, so the frequency always
 * lands on a clean grid line (no leftover units to hand-trim later). */
static gboolean on_scroll(GtkEventControllerScroll *ctl, double dx, double dy, gpointer data) {
  (void)dx; (void)ctl;
  App *app = (App *)data;
  if (!app->radio_mode || !app->engine_ok) { return FALSE; }
  int dir = (int)llround(-dy);        /* wheel up (dy < 0) tunes higher */
  if (dir == 0) { return FALSE; }
  long long step = app->tune_step > 0 ? app->tune_step : TUNE_STEP_DEFAULT;
  long long f = app->freq;
  /* Snap to the grid in the scroll direction (floor going up, ceil going down),
   * then move whole steps — so nf is always a multiple of step. */
  long long base = (dir > 0) ? (f / step) * step
                             : ((f + step - 1) / step) * step;
  long long nf = base + (long long)dir * step;
  if (nf < 1) { nf = 1; }
  app->freq = nf;              /* readout follows on the next tick */
  p2_set_frequency(nf);
  schedule_save(app);
  return TRUE;
}

/* Left-drag grabs the spectrum and pans the DDC centre (Model A). The pan is
 * absolute from drag-begin (freq = base − offset·Hz-per-pixel), so the grabbed
 * point tracks the cursor smoothly with no accumulation drift. */
static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer data) {
  (void)g; (void)x; (void)y;
  App *app = (App *)data;
  app->drag_base_freq = app->freq;
}

static void on_drag_update(GtkGestureDrag *g, double off_x, double off_y, gpointer data) {
  (void)g; (void)off_y;
  App *app = (App *)data;
  if (!app->radio_mode || !app->engine_ok) { return; }
  int w = gtk_widget_get_width(app->area);
  if (w < 1) { return; }
  double hz_per_px = (double)app->rate / app->zoom / w;   /* narrower span when zoomed */
  /* Drag right → content follows the cursor → view moves to lower frequency. */
  long long nf = app->drag_base_freq - (long long)llround(off_x * hz_per_px);
  if (nf < 1) { nf = 1; }
  app->freq = nf;             /* readout follows on the next tick */
  p2_set_frequency(nf);
  schedule_save(app);
}

/* Keys u/l/c/a switch demod mode by activating the matching strip toggle
 * (which drives the engine), so keyboard and buttons stay in sync. */
static gboolean on_key(GtkEventControllerKey *ctl, guint keyval, guint keycode,
                       GdkModifierType state, gpointer data) {
  (void)ctl; (void)keycode; (void)state;
  App *app = (App *)data;
  if (!app->radio_mode) { return FALSE; }
  int mode;
  switch (gdk_keyval_to_lower(keyval)) {
    case GDK_KEY_u: mode = DEMOD_USB; break;
    case GDK_KEY_l: mode = DEMOD_LSB; break;
    case GDK_KEY_c: mode = DEMOD_CWU; break;
    case GDK_KEY_a: mode = DEMOD_AM;  break;
    default: return FALSE;
  }
  if (app->mode_btns[mode]) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->mode_btns[mode]), TRUE);
  }
  return TRUE;
}

/* ---- control-strip callbacks (wired to the engine) ----------------------- */

/* Filter dropdown → apply the selected preset's passband live. */
static void on_filter_changed(GtkDropDown *dd, GParamSpec *ps, gpointer data) {
  (void)ps;
  App *app = (App *)data;
  int nf, dfl;
  const FilterPreset *ft = mode_filters(app->mode, &nf, &dfl);
  guint idx = gtk_drop_down_get_selected(dd);
  if ((int)idx >= nf) { return; }
  app->filter_idx = (int)idx;
  app->flo = ft[idx].low;
  app->fhi = ft[idx].high;
  demod_set_passband(app->flo, app->fhi);
  schedule_save(app);                        /* persist the chosen filter */
}

/* Rebuild the filter dropdown for the current mode and select filter_idx. */
static void populate_filter_dd(App *app) {
  if (!app->filter_dd) { return; }
  int nf, dfl;
  const FilterPreset *ft = mode_filters(app->mode, &nf, &dfl);
  const char *names[16];
  for (int i = 0; i < nf; i++) { names[i] = ft[i].name; }
  names[nf] = NULL;
  GtkStringList *m = gtk_string_list_new(names);
  g_signal_handlers_block_by_func(app->filter_dd, (gpointer)on_filter_changed, app);
  gtk_drop_down_set_model(GTK_DROP_DOWN(app->filter_dd), G_LIST_MODEL(m));
  gtk_drop_down_set_selected(GTK_DROP_DOWN(app->filter_dd), app->filter_idx);
  g_signal_handlers_unblock_by_func(app->filter_dd, (gpointer)on_filter_changed, app);
  g_object_unref(m);
}

static void on_mode_toggled(GtkToggleButton *b, gpointer data) {
  if (!gtk_toggle_button_get_active(b)) { return; }  /* ignore the deselect half */
  App *app = (App *)data;
  int mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "mode"));
  int nf, dfl;
  const FilterPreset *ft = mode_filters(mode, &nf, &dfl);
  app->mode       = mode;
  app->filter_idx = dfl;                 /* mode's default filter */
  app->flo = ft[dfl].low;
  app->fhi = ft[dfl].high;
  demod_set_mode(mode, app->flo, app->fhi);
  populate_filter_dd(app);               /* refill dropdown for the new mode */
  schedule_save(app);
}

static void on_volume_changed(GtkRange *r, gpointer data) {
  App *app = (App *)data;
  double v = gtk_range_get_value(r);
  demod_set_volume(v);
  app->volume = v;
  schedule_save(app);
}

/* RF front-end: ADC0 step attenuator (0-31 dB; 0 = max sensitivity). */
static void on_atten_changed(GtkRange *r, gpointer data) {
  App *app = (App *)data;
  int db = (int)gtk_range_get_value(r);
  p2_set_attenuation(db);
  app->atten = db;
  schedule_save(app);
}

/* AGC dropdown index → mode; dropdown order is Med,Fast,Slow,Long,Off. */
static const int AGC_MODE_OF_IDX[] = { 3, 4, 2, 1, 0 };

static void on_agc_changed(GtkDropDown *dd, GParamSpec *ps, gpointer data) {
  (void)ps;
  App *app = (App *)data;
  guint i = gtk_drop_down_get_selected(dd);
  if (i >= G_N_ELEMENTS(AGC_MODE_OF_IDX)) { return; }
  app->agc = AGC_MODE_OF_IDX[i];
  demod_set_agc(app->agc);
  schedule_save(app);
}

/* AGC-T threshold/gain slider. */
static void on_agct_changed(GtkRange *r, gpointer data) {
  App *app = (App *)data;
  double g = gtk_range_get_value(r);
  demod_set_agc_gain(g);
  app->agc_gain = g;
  schedule_save(app);
}

/* Noise reduction / blanker / auto-notch toggles. */
static void on_nr_toggled(GtkToggleButton *b, gpointer data) {
  App *app = (App *)data;
  app->nr = gtk_toggle_button_get_active(b);
  demod_set_nr(app->nr);
  schedule_save(app);
}
static void on_nb_toggled(GtkToggleButton *b, gpointer data) {
  App *app = (App *)data;
  app->nb = gtk_toggle_button_get_active(b);
  demod_set_nb(app->nb);
  schedule_save(app);
}
static void on_anf_toggled(GtkToggleButton *b, gpointer data) {
  App *app = (App *)data;
  app->anf = gtk_toggle_button_get_active(b);
  demod_set_anf(app->anf);
  schedule_save(app);
}

static void on_band_clicked(GtkButton *b, gpointer data) {
  App *app = (App *)data;
  long long f = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "freq"));
  app->freq = f;
  p2_set_frequency(f);
  schedule_save(app);
}

/* Small "label: widget" pair for the strip. */
static GtkWidget *labeled(const char *text, GtkWidget *w) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *l = gtk_label_new(text);
  gtk_widget_add_css_class(l, "dim");
  gtk_box_append(GTK_BOX(box), l);
  gtk_box_append(GTK_BOX(box), w);
  return box;
}

/* Build the horizontal control strip and wire it to the engine. Radio mode. */
static GtkWidget *build_controls(App *app) {
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(bar, "controlbar");

  /* Mode — segmented, grouped; keep a handle per DEMOD id for key sync. */
  static const int         mids[]   = {DEMOD_USB, DEMOD_LSB, DEMOD_CWL, DEMOD_CWU, DEMOD_AM};
  static const char *const mlabels[] = {"USB", "LSB", "CWL", "CWU", "AM"};
  GtkWidget *modebox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(modebox, "linked");
  GtkWidget *group = NULL;
  for (int i = 0; i < 5; i++) {
    GtkWidget *b = gtk_toggle_button_new_with_label(mlabels[i]);
    gtk_widget_add_css_class(b, "mode");
    g_object_set_data(G_OBJECT(b), "mode", GINT_TO_POINTER(mids[i]));
    if (!group) { group = b; } else { gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(group)); }
    app->mode_btns[mids[i]] = b;
    gtk_box_append(GTK_BOX(modebox), b);
  }
  if (app->mode_btns[app->mode]) {   /* reflect the resolved startup mode … */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->mode_btns[app->mode]), TRUE);
  }
  for (int i = 0; i < 5; i++) {       /* … then connect, so this doesn't re-fire the engine */
    g_signal_connect(app->mode_btns[mids[i]], "toggled", G_CALLBACK(on_mode_toggled), app);
  }
  gtk_box_append(GTK_BOX(bar), modebox);

  /* Filter — piHPSDR presets for the current mode; repopulated on mode change. */
  app->filter_dd = gtk_drop_down_new(NULL, NULL);
  g_signal_connect(app->filter_dd, "notify::selected", G_CALLBACK(on_filter_changed), app);
  populate_filter_dd(app);
  gtk_box_append(GTK_BOX(bar), labeled("Filter", app->filter_dd));

  /* RF attenuator (front-end gain): ADC0 step attenuator, 0-31 dB, 0 = max sens. */
  GtkWidget *att = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 31, 1);
  gtk_range_set_value(GTK_RANGE(att), app->atten);      /* before wiring: no spurious send */
  gtk_widget_set_size_request(att, 110, -1);
  gtk_scale_set_draw_value(GTK_SCALE(att), TRUE);
  gtk_scale_set_value_pos(GTK_SCALE(att), GTK_POS_RIGHT);
  g_signal_connect(att, "value-changed", G_CALLBACK(on_atten_changed), app);
  gtk_box_append(GTK_BOX(bar), labeled("Att", att));

  /* AGC character + AGC-T (threshold/gain). NR·NB·ANF still placeholders below. */
  GtkWidget *agc_dd = gtk_drop_down_new_from_strings(
      (const char *[]){"Med","Fast","Slow","Long","Off", NULL});
  guint aidx = 0;
  for (guint i = 0; i < G_N_ELEMENTS(AGC_MODE_OF_IDX); i++) {
    if (AGC_MODE_OF_IDX[i] == app->agc) { aidx = i; break; }
  }
  gtk_drop_down_set_selected(GTK_DROP_DOWN(agc_dd), aidx);   /* before wiring: no re-fire */
  g_signal_connect(agc_dd, "notify::selected", G_CALLBACK(on_agc_changed), app);
  gtk_box_append(GTK_BOX(bar), labeled("AGC", agc_dd));

  GtkWidget *agct = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -20, 120, 1);
  gtk_range_set_value(GTK_RANGE(agct), app->agc_gain);       /* before wiring */
  gtk_widget_set_size_request(agct, 110, -1);
  gtk_scale_set_draw_value(GTK_SCALE(agct), TRUE);
  gtk_scale_set_value_pos(GTK_SCALE(agct), GTK_POS_RIGHT);
  g_signal_connect(agct, "value-changed", G_CALLBACK(on_agct_changed), app);
  gtk_box_append(GTK_BOX(bar), labeled("AGC-T", agct));

  GtkWidget *nrbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(nrbox, "linked");
  GtkWidget *nr_b  = gtk_toggle_button_new_with_label("NR");
  GtkWidget *nb_b  = gtk_toggle_button_new_with_label("NB");
  GtkWidget *anf_b = gtk_toggle_button_new_with_label("ANF");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(nr_b),  app->nr);   /* before wiring */
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(nb_b),  app->nb);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(anf_b), app->anf);
  g_signal_connect(nr_b,  "toggled", G_CALLBACK(on_nr_toggled),  app);
  g_signal_connect(nb_b,  "toggled", G_CALLBACK(on_nb_toggled),  app);
  g_signal_connect(anf_b, "toggled", G_CALLBACK(on_anf_toggled), app);
  gtk_box_append(GTK_BOX(nrbox), nr_b);
  gtk_box_append(GTK_BOX(nrbox), nb_b);
  gtk_box_append(GTK_BOX(nrbox), anf_b);
  gtk_box_append(GTK_BOX(bar), nrbox);

  /* AF volume — live. */
  GtkWidget *vol = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -40, 0, 1);
  gtk_range_set_value(GTK_RANGE(vol), app->volume);
  gtk_widget_set_size_request(vol, 130, -1);
  gtk_scale_set_draw_value(GTK_SCALE(vol), FALSE);
  g_signal_connect(vol, "value-changed", G_CALLBACK(on_volume_changed), app);
  gtk_box_append(GTK_BOX(bar), labeled("AF", vol));

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(bar), spacer);

  /* Band buttons — jump the VFO. */
  static const struct { const char *l; int f; } bands[] = {
    {"160", 1840000}, {"80", 3600000}, {"40", 7074000}, {"20", 14074000},
    {"17", 18100000}, {"15", 21074000}, {"12", 24915000}, {"10", 28074000},
  };
  GtkWidget *bandbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(bandbox, "linked");
  for (int i = 0; i < 8; i++) {
    GtkWidget *b = gtk_button_new_with_label(bands[i].l);
    gtk_widget_add_css_class(b, "band");
    g_object_set_data(G_OBJECT(b), "freq", GINT_TO_POINTER(bands[i].f));
    g_signal_connect(b, "clicked", G_CALLBACK(on_band_clicked), app);
    gtk_box_append(GTK_BOX(bandbox), b);
  }
  gtk_box_append(GTK_BOX(bar), bandbox);
  return bar;
}

#define ZOOM_MAX 128.0

static void update_span_label(App *app) {
  if (!app->span_label) { return; }
  char buf[40];
  snprintf(buf, sizeof buf, "%.1f kHz  ·  %g×", (double)app->rate / app->zoom / 1000.0, app->zoom);
  gtk_label_set_text(GTK_LABEL(app->span_label), buf);
}

/* Zoom slider snaps to octave detents (1×,2×,…,128×): continuous re-config while
 * dragging looked rough, so reconfig fires only when the detent actually changes
 * (one cheap wisdom-backed SetAnalyzer per notch). */
static void on_zoom_changed(GtkRange *r, gpointer data) {
  App *app = (App *)data;
  double z = pow(2.0, lround(gtk_range_get_value(r)));   /* nearest octave */
  if (z == app->pending_zoom) { return; }
  app->pending_zoom = z;
  app->zoom_dirty = 1;       /* applied on the next frame tick */
  schedule_save(app);        /* debounced; persists the applied app->zoom */
}

/* Bottom bar (AdwToolbarView bottom slot): view/display controls — zoom for now. */
/* Step selector: set the scroll step, and snap the current frequency to the
 * nearest multiple of the new step so switching steps leaves no sub-step units. */
static void on_step_changed(GtkDropDown *dd, GParamSpec *ps, gpointer data) {
  (void)ps;
  App *app = (App *)data;
  guint i = gtk_drop_down_get_selected(dd);
  if (i >= G_N_ELEMENTS(TUNE_STEPS)) { return; }
  long long step = TUNE_STEPS[i];
  app->tune_step = step;
  long long f = app->freq;
  long long snapped = ((f + step / 2) / step) * step;   /* round to nearest */
  if (snapped < 1) { snapped = step; }
  if (snapped != f) {
    app->freq = snapped;                 /* readout follows on the next tick */
    if (app->engine_ok) { p2_set_frequency(snapped); }
  }
  schedule_save(app);
}

static GtkWidget *build_bottom_controls(App *app) {
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(bar, "controlbar");

  GtkWidget *zoom = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 7.0, 1.0); /* octaves 1x..128x */
  gtk_range_set_round_digits(GTK_RANGE(zoom), 0);          /* snap the handle to detents */
  for (int i = 0; i <= 7; i++) { gtk_scale_add_mark(GTK_SCALE(zoom), i, GTK_POS_BOTTOM, NULL); }
  gtk_range_set_value(GTK_RANGE(zoom), log2(app->zoom));   /* reflect saved zoom (before wiring) */
  gtk_widget_set_size_request(zoom, 200, -1);
  gtk_scale_set_draw_value(GTK_SCALE(zoom), FALSE);
  g_signal_connect(zoom, "value-changed", G_CALLBACK(on_zoom_changed), app);
  gtk_box_append(GTK_BOX(bar), labeled("Zoom", zoom));

  app->span_label = gtk_label_new("");
  gtk_widget_add_css_class(app->span_label, "span");
  gtk_box_append(GTK_BOX(bar), app->span_label);
  update_span_label(app);

  GtkWidget *sdd = gtk_drop_down_new_from_strings(TUNE_STEP_LABELS);
  guint sidx = 0;
  for (guint i = 0; i < G_N_ELEMENTS(TUNE_STEPS); i++) {
    if (TUNE_STEPS[i] == app->tune_step) { sidx = i; break; }
  }
  gtk_drop_down_set_selected(GTK_DROP_DOWN(sdd), sidx);  /* set before wiring: no retune on build */
  g_signal_connect(sdd, "notify::selected", G_CALLBACK(on_step_changed), app);
  app->step_dd = sdd;
  gtk_box_append(GTK_BOX(bar), labeled("Step", sdd));

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(bar), spacer);
  return bar;
}

static void css_load(void) {
  GtkCssProvider *p = gtk_css_provider_new();
  const char *c =
    ".controlbar { padding: 7px 10px; }"
    ".dim { opacity: 0.6; font-size: 12px; }"
    ".span { font-family: monospace; opacity: 0.75; }"
    "button.mode, button.band { min-width: 30px; padding-left: 7px; padding-right: 7px; }"
    "button.mode:checked { background: #1d6fa5; color: #fff; }";
  gtk_css_provider_load_from_string(p, c);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
      GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* ---- preferences dialog (AdwPreferencesDialog) --------------------------- */

static const int PREF_RATES[] = {48000, 96000, 192000, 384000, 768000, 1536000};

static void on_pref_fps(AdwSpinRow *r, GParamSpec *ps, gpointer data) {
  (void)ps;
  App *app = (App *)data;
  app->fps = (int)adw_spin_row_get_value(r);
  analyzer_set_fps(app->fps);   /* live */
  schedule_save(app);
}
static void on_pref_gain(AdwSpinRow *r, GParamSpec *ps, gpointer data) {
  (void)ps;
  App *app = (App *)data;
  app->gain = adw_spin_row_get_value(r);
  demod_set_gain(app->gain);    /* live */
  schedule_save(app);
}
static void on_pref_latency(AdwSpinRow *r, GParamSpec *ps, gpointer data) {
  (void)ps;
  App *app = (App *)data;
  app->latency = (int)adw_spin_row_get_value(r);   /* applied on restart */
  schedule_save(app);
}
static void on_pref_rate(AdwComboRow *r, GParamSpec *ps, gpointer data) {
  (void)ps;
  App *app = (App *)data;
  guint i = adw_combo_row_get_selected(r);
  if (i < G_N_ELEMENTS(PREF_RATES)) { app->rate = PREF_RATES[i]; schedule_save(app); }  /* restart */
}
static void on_pref_ip(GtkEditable *e, gpointer data) {
  App *app = (App *)data;
  g_strlcpy(app->radio_ip, gtk_editable_get_text(e), sizeof(app->radio_ip));   /* restart */
  schedule_save(app);
}

static GtkWidget *pref_spin(const char *title, const char *subtitle,
                            double lo, double hi, double val, GCallback cb, App *app) {
  GtkAdjustment *a = gtk_adjustment_new(val, lo, hi, 1, 1, 0);
  GtkWidget *row = g_object_new(ADW_TYPE_SPIN_ROW, "title", title, "subtitle", subtitle,
                                "adjustment", a, "digits", 0, NULL);
  g_signal_connect(row, "notify::value", cb, app);   /* connect after ctor → no spurious fire */
  return row;
}

static AdwDialog *build_prefs(App *app) {
  AdwPreferencesDialog *dlg = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());

  /* Radio — all restart-to-apply. */
  AdwPreferencesPage *p = ADW_PREFERENCES_PAGE(g_object_new(ADW_TYPE_PREFERENCES_PAGE,
      "title", "Radio", "icon-name", "network-workgroup-symbolic", NULL));
  AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(g_object_new(ADW_TYPE_PREFERENCES_GROUP,
      "title", "Connection", "description", "Applies on restart", NULL));
  GtkWidget *ip = g_object_new(ADW_TYPE_ENTRY_ROW, "title", "Radio IP address", NULL);
  gtk_editable_set_text(GTK_EDITABLE(ip), app->radio_ip);
  g_signal_connect(ip, "changed", G_CALLBACK(on_pref_ip), app);
  adw_preferences_group_add(g, ip);
  GtkStringList *rm = gtk_string_list_new((const char *[]){"48 kHz","96 kHz","192 kHz","384 kHz","768 kHz","1536 kHz", NULL});
  guint ri = 2;
  for (guint i = 0; i < G_N_ELEMENTS(PREF_RATES); i++) { if (PREF_RATES[i] == app->rate) { ri = i; } }
  GtkWidget *rate = g_object_new(ADW_TYPE_COMBO_ROW, "title", "Sample rate", "model", rm, "selected", ri, NULL);
  g_signal_connect(rate, "notify::selected", G_CALLBACK(on_pref_rate), app);
  adw_preferences_group_add(g, rate);
  adw_preferences_page_add(p, g);
  adw_preferences_dialog_add(dlg, p);

  /* Audio — gain live, latency restart. */
  p = ADW_PREFERENCES_PAGE(g_object_new(ADW_TYPE_PREFERENCES_PAGE,
      "title", "Audio", "icon-name", "audio-speakers-symbolic", NULL));
  g = ADW_PREFERENCES_GROUP(g_object_new(ADW_TYPE_PREFERENCES_GROUP, "title", "Output", NULL));
  adw_preferences_group_add(g, pref_spin("Digital master gain", "Applies live",
      1, 32, app->gain, G_CALLBACK(on_pref_gain), app));
  adw_preferences_group_add(g, pref_spin("Audio latency", "ms · restart to apply",
      5, 100, app->latency, G_CALLBACK(on_pref_latency), app));
  adw_preferences_page_add(p, g);
  adw_preferences_dialog_add(dlg, p);

  /* Display — fps live. */
  p = ADW_PREFERENCES_PAGE(g_object_new(ADW_TYPE_PREFERENCES_PAGE,
      "title", "Display", "icon-name", "video-display-symbolic", NULL));
  g = ADW_PREFERENCES_GROUP(g_object_new(ADW_TYPE_PREFERENCES_GROUP, "title", "Panadapter", NULL));
  adw_preferences_group_add(g, pref_spin("Frame rate", "fps · applies live",
      10, 60, app->fps, G_CALLBACK(on_pref_fps), app));
  adw_preferences_page_add(p, g);
  adw_preferences_dialog_add(dlg, p);

  return ADW_DIALOG(dlg);
}

static void act_prefs(GSimpleAction *a, GVariant *param, gpointer data) {
  (void)a; (void)param;
  App *app = (App *)data;
  GtkWindow *win = gtk_application_get_active_window(
      GTK_APPLICATION(g_application_get_default()));
  adw_dialog_present(build_prefs(app), GTK_WIDGET(win));
}

static void on_activate(GtkApplication *gtkapp, gpointer data) {
  App *app = (App *)data;
  css_load();

  GtkWidget *win = adw_application_window_new(gtkapp);
  gtk_window_set_title(GTK_WINDOW(win),
                       app->radio_mode ? "SDR for Linux — radio" : "SDR for Linux — server");
  gtk_window_set_default_size(GTK_WINDOW(win), 1320, 720);

  GtkWidget *header = adw_header_bar_new();
  GtkWidget *status = gtk_label_new(app->radio_mode ? "●  ANAN G1" : "●  server");
  gtk_widget_add_css_class(status, "dim");
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), status);
  if (app->radio_mode) {
    GSimpleAction *pa = g_simple_action_new("preferences", NULL);
    g_signal_connect(pa, "activate", G_CALLBACK(act_prefs), app);
    g_action_map_add_action(G_ACTION_MAP(gtkapp), G_ACTION(pa));
    g_object_unref(pa);

    GMenu *m = g_menu_new();
    g_menu_append(m, "Preferences", "app.preferences");
    GtkWidget *menu = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu), G_MENU_MODEL(m));
    g_object_unref(m);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu);
  }

  GtkWidget *tv = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);

  app->area = gtk_drawing_area_new();
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app->area), draw_cb, app, NULL);
  gtk_widget_set_vexpand(app->area, TRUE);

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  if (app->radio_mode) {
    gtk_box_append(GTK_BOX(content), build_controls(app));
    gtk_box_append(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  }
  gtk_box_append(GTK_BOX(content), app->area);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), content);
  if (app->radio_mode) {
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(tv), build_bottom_controls(app));
  }
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), tv);

  /* Input controllers (self-gate on radio mode): wheel tunes, drag pans, u/l/c/a mode. */
  GtkEventControllerScroll *scroll = GTK_EVENT_CONTROLLER_SCROLL(
      gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL));
  g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), app);
  gtk_widget_add_controller(app->area, GTK_EVENT_CONTROLLER(scroll));

  GtkGesture *drag = gtk_gesture_drag_new();   /* left button by default */
  g_signal_connect(drag, "drag-begin",  G_CALLBACK(on_drag_begin),  app);
  g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), app);
  gtk_widget_add_controller(app->area, GTK_EVENT_CONTROLLER(drag));

  GtkEventControllerKey *keys =
      GTK_EVENT_CONTROLLER_KEY(gtk_event_controller_key_new());
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), app);
  gtk_widget_add_controller(win, GTK_EVENT_CONTROLLER(keys));

  gtk_widget_add_tick_callback(app->area, tick_cb, app, NULL);
  gtk_window_present(GTK_WINDOW(win));
}

/* Bring up the direct-radio engine: discover → analyzer → P2 RX.
 * State precedence: env var > saved config > built-in default. */
static void start_radio(App *app) {
  Settings st = { .freq = 14100000, .rate = 192000, .mode = -1,
                  .volume = -10.0, .gain = 1.0, .fps = 25, .latency = 10,
                  .step = TUNE_STEP_DEFAULT, .zoom = 1.0, .atten = 0,
                  .agc = 3, .agc_gain = 80.0, .filter = -1 };
  g_strlcpy(st.ip, "192.168.1.247", sizeof(st.ip));
  if (settings_load(&st)) { printf("settings: loaded %s\n", settings_path()); }

  const char *e;
  if ((e = getenv("SDRFL_RADIO_IP")) && *e) { g_strlcpy(st.ip, e, sizeof(st.ip)); }
  if ((e = getenv("SDRFL_FREQ"))     && *e) { st.freq = strtoll(e, NULL, 10); }
  if ((e = getenv("SDRFL_RATE"))     && *e) { st.rate = atoi(e); }
  if ((e = getenv("SDRFL_VOLUME"))   && *e) { st.volume = atof(e); }
  if ((e = getenv("SDRFL_GAIN"))     && *e) { st.gain = atof(e); }
  if ((e = getenv("SDRFL_FPS"))      && *e) { st.fps = atoi(e); }
  if ((e = getenv("SDRFL_LAT"))      && *e) { st.latency = atoi(e); }
  /* Mode: env SDRFL_MODE > saved mode (if valid) > by-band default. */
  int mode = mode_from_name(getenv("SDRFL_MODE"));
  if (mode < 0) { mode = st.mode; }
  if (mode < 0) { mode = (st.freq < 10000000) ? DEMOD_LSB : DEMOD_USB; }

  snprintf(ipaddr_radio, sizeof(ipaddr_radio), "%s", st.ip);
  g_strlcpy(app->radio_ip, st.ip, sizeof(app->radio_ip));
  app->freq   = st.freq;
  app->rate   = st.rate;
  app->mode   = mode;
  app->volume = st.volume;
  app->gain   = st.gain;
  app->atten  = st.atten < 0 ? 0 : (st.atten > 31 ? 31 : st.atten);
  app->agc    = st.agc < 0 ? 0 : (st.agc > 4 ? 4 : st.agc);
  app->agc_gain = st.agc_gain < -20.0 ? -20.0 : (st.agc_gain > 120.0 ? 120.0 : st.agc_gain);
  app->nr     = st.nr ? 1 : 0;
  app->nb     = st.nb ? 1 : 0;
  app->anf    = st.anf ? 1 : 0;
  app->fps    = st.fps;
  app->latency = st.latency;
  /* Snap the saved zoom to the nearest octave detent in [1, ZOOM_MAX]. */
  app->zoom = st.zoom;
  if (!(app->zoom >= 1.0))  { app->zoom = 1.0; }      /* NaN or < 1 */
  if (app->zoom > ZOOM_MAX) { app->zoom = ZOOM_MAX; }
  app->zoom = pow(2.0, lround(log2(app->zoom)));
  app->pending_zoom = app->zoom;
  app->pixels = ENGINE_PIXELS;
  app->tune_step = TUNE_STEP_DEFAULT;   /* keep only known step values */
  for (guint i = 0; i < G_N_ELEMENTS(TUNE_STEPS); i++) {
    if (TUNE_STEPS[i] == st.step) { app->tune_step = st.step; break; }
  }
  int rate    = st.rate;

  printf("Discovering radio at %s ...\n", ipaddr_radio);
  p2_discovery();
  if (devices <= 0) { fprintf(stderr, "no radio found\n"); return; }
  const DISCOVERED *dev = &discovered[selected_device];
  if (dev->status == 3) {   /* P2 discovery reply byte[4]: 2 = idle, 3 = streaming */
    fprintf(stderr, "radio at %s is IN USE by another program (piHPSDR?) — close it first\n",
            inet_ntoa(dev->network.address.sin_addr));
    return;
  }
  printf("Using %s at %s — RX %lld Hz @ %d Hz\n", dev->name,
         inet_ntoa(dev->network.address.sin_addr), app->freq, rate);

  if (analyzer_create(0, app->pixels, rate, app->fps) != 0) {
    fprintf(stderr, "analyzer_create failed\n");
    return;
  }
  if (app->zoom != 1.0) { analyzer_set_zoom(app->zoom); }   /* restore saved zoom */

  /* Audio: WDSP demod → native PipeWire sink. Default filter = mode's preset. */
  int nf, dfl;
  const FilterPreset *ft = mode_filters(mode, &nf, &dfl);
  app->filter_idx = (st.filter >= 0 && st.filter < nf) ? st.filter : dfl;  /* saved or mode default */
  app->flo = ft[app->filter_idx].low;
  app->fhi = ft[app->filter_idx].high;
  if (audio_start(48000, 1, app->latency) == 0 && demod_create(0, rate, mode, app->flo, app->fhi, app->volume) == 0) {
    demod_set_gain(app->gain);
    demod_set_agc(app->agc);            /* saved AGC character + threshold */
    demod_set_agc_gain(app->agc_gain);
    demod_set_nr(app->nr);              /* saved NR/NB/ANF */
    demod_set_nb(app->nb);
    demod_set_anf(app->anf);
    app->audio_ok = 1;
  } else {
    fprintf(stderr, "audio/demod init failed — panadapter only\n");
  }

  p2_set_attenuation(app->atten);   /* front-end gain: goes out in the first HP packet */
  if (p2_rx_start(dev, app->freq, rate, feed_cb, NULL) != 0) {
    fprintf(stderr, "p2_rx_start failed\n");
    if (app->audio_ok) { demod_destroy(); audio_stop(); }
    analyzer_destroy();
    return;
  }
  app->engine_ok = 1;
  app->connected = 1;
}

/* SIGINT/SIGTERM → quit the run loop so main()'s cleanup (p2_rx_stop) runs and
 * the radio is released — otherwise Ctrl-C would leave it streaming. */
static gboolean on_signal(gpointer data) {
  g_application_quit(G_APPLICATION(data));
  return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
  App app;
  memset(&app, 0, sizeof(app));
  app.wf = waterfall_new();

  int server_mode = (argc > 1) && (strcmp(argv[1], "--server") == 0);

  if (server_mode) {
    const char *host = (argc > 2) ? argv[2] : "127.0.0.1";
    int         port = (argc > 3) ? atoi(argv[3]) : 50000;
    const char *pwd  = (argc > 4) ? argv[4] : getenv("PIHPSDR_PWD");
    if (!pwd) { pwd = ""; }
    app.radio_mode = 0;
    app.client = client_new(host, port, pwd);
    app.conn_err = client_connect(app.client);
    if (app.conn_err == CLIENT_OK) {
      app.connected = 1;
      client_start(app.client);
      printf("Connected to %s:%d — streaming.\n", host, port);
    } else {
      fprintf(stderr, "connect failed: %s\n", client_strerror(app.conn_err));
    }
  } else {
    app.radio_mode = 1;
    wisdom_ensure();      /* first run: build FFTW wisdom (progress window) so the
                             analyzer's PATIENT plans don't freeze on deep zoom */
    start_radio(&app);
  }

  AdwApplication *gtkapp = adw_application_new("cz.ok1br.sdr_for_linux",
                                               G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(gtkapp, "activate", G_CALLBACK(on_activate), &app);
  g_unix_signal_add(SIGINT, on_signal, gtkapp);
  g_unix_signal_add(SIGTERM, on_signal, gtkapp);
  int status = g_application_run(G_APPLICATION(gtkapp), 0, NULL);

  g_object_unref(gtkapp);
  if (app.radio_mode) {
    if (app.save_timer_id) { g_source_remove(app.save_timer_id); app.save_timer_id = 0; }
    if (app.rate > 0) { Settings s; app_to_settings(&app, &s); settings_save(&s); }
    if (app.engine_ok) { p2_rx_stop(); }
    if (app.audio_ok)  { demod_destroy(); audio_stop(); }
    if (app.engine_ok) { analyzer_destroy(); }
  } else {
    client_free(app.client);
  }
  waterfall_free(app.wf);
  return status;
}
