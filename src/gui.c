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
  GtkWidget  *span_label;    /* footer span readout                            */
  GtkWidget  *zoom_in_btn;   /* footer zoom +/- (stepped; continuous stutters) */
  GtkWidget  *zoom_out_btn;
  int         filter_idx;    /* selected preset in the current mode's table    */
  double      flo, fhi;      /* current passband (Hz, rel. centre) — for drawing */
  GtkWidget  *filter_dd;     /* filter dropdown (repopulated per mode)         */
} App;

#define PANADAPTER_FRACTION 0.5
#define EMA_FACTOR 0.55f

/* Tuning step per wheel notch (Hz): plain / Ctrl (fine) / Shift (coarse). */
#define TUNE_STEP_HZ      100
#define TUNE_STEP_FINE    10
#define TUNE_STEP_COARSE  1000

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

  /* EMA now in dBm. Build metadata + waterfall bytes. */
  app->frame.width      = n;
  app->frame.vfo_a_freq = app->freq;
  double peak = app->ema[0];
  static uint8_t bytes[SPECTRUM_DATA_SIZE];
  for (int i = 0; i < n; i++) {
    if (app->ema[i] > peak) { peak = app->ema[i]; }
    double b = app->ema[i] + 200.0;
    bytes[i] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
  }
  app->frame.s_dbm = peak;
  waterfall_push(app->wf, bytes, n);
  app->have_frame = 1;
  gtk_widget_queue_draw(widget);
}

static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
  (void)clock;
  App *app = (App *)data;
  if (app->connected) {
    if (app->radio_mode) { tick_radio(app, widget); }
    else                 { tick_network(app, widget); }
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
 * moves, passband stays centred). Ctrl = fine, Shift = coarse step. */
static gboolean on_scroll(GtkEventControllerScroll *ctl, double dx, double dy, gpointer data) {
  (void)dx;
  App *app = (App *)data;
  if (!app->radio_mode || !app->engine_ok) { return FALSE; }
  GdkModifierType st = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(ctl));
  long long step = TUNE_STEP_HZ;
  if      (st & GDK_CONTROL_MASK) { step = TUNE_STEP_FINE; }
  else if (st & GDK_SHIFT_MASK)   { step = TUNE_STEP_COARSE; }
  /* Wheel up (dy < 0) tunes higher. (Fractional touchpad deltas round to 0 —
   * a later refinement can accumulate the residual; the wheel is the target.) */
  long long delta = (long long)llround(-dy) * step;
  if (delta == 0) { return FALSE; }
  long long nf = app->freq + delta;
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
  demod_set_passband(app->flo, app->fhi);   /* filter isn't persisted (mode default on start) */
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

  /* AGC / NR·NB·ANF — visible placeholders until their engine hooks land. */
  gtk_box_append(GTK_BOX(bar), labeled("AGC",
      gtk_drop_down_new_from_strings((const char *[]){"Med","Fast","Slow","Long","Off", NULL})));
  GtkWidget *nrbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(nrbox, "linked");
  const char *nr[] = {"NR","NB","ANF"};
  for (int i = 0; i < 3; i++) { gtk_box_append(GTK_BOX(nrbox), gtk_toggle_button_new_with_label(nr[i])); }
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

/* Apply the current zoom (stepped ×2 per click — SetAnalyzer reconfig at large
 * FFT sizes is too heavy to run continuously). */
static void zoom_apply(App *app) {
  analyzer_set_zoom(app->zoom);
  update_span_label(app);
  if (app->zoom_out_btn) { gtk_widget_set_sensitive(app->zoom_out_btn, app->zoom > 1.0); }
  if (app->zoom_in_btn)  { gtk_widget_set_sensitive(app->zoom_in_btn, app->zoom < ZOOM_MAX); }
}
static void on_zoom_in(GtkButton *b, gpointer data) {
  (void)b; App *app = (App *)data;
  if (app->zoom < ZOOM_MAX) { app->zoom *= 2.0; zoom_apply(app); }
}
static void on_zoom_out(GtkButton *b, gpointer data) {
  (void)b; App *app = (App *)data;
  if (app->zoom > 1.0) { app->zoom /= 2.0; zoom_apply(app); }
}

/* Bottom bar (AdwToolbarView bottom slot): view/display controls — zoom for now. */
static GtkWidget *build_bottom_controls(App *app) {
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(bar, "controlbar");

  GtkWidget *zbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(zbox, "linked");
  app->zoom_out_btn = gtk_button_new_from_icon_name("zoom-out-symbolic");
  app->zoom_in_btn  = gtk_button_new_from_icon_name("zoom-in-symbolic");
  g_signal_connect(app->zoom_out_btn, "clicked", G_CALLBACK(on_zoom_out), app);
  g_signal_connect(app->zoom_in_btn,  "clicked", G_CALLBACK(on_zoom_in),  app);
  gtk_box_append(GTK_BOX(zbox), app->zoom_out_btn);
  gtk_box_append(GTK_BOX(zbox), app->zoom_in_btn);
  gtk_box_append(GTK_BOX(bar), labeled("Zoom", zbox));

  app->span_label = gtk_label_new("");
  gtk_widget_add_css_class(app->span_label, "span");
  gtk_box_append(GTK_BOX(bar), app->span_label);
  gtk_widget_set_sensitive(app->zoom_out_btn, FALSE);   /* starts at 1x */
  update_span_label(app);

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
                  .volume = -10.0, .gain = 8.0, .fps = 25, .latency = 10 };
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
  app->fps    = st.fps;
  app->latency = st.latency;
  app->zoom   = 1.0;
  app->pixels = ENGINE_PIXELS;
  int rate    = st.rate;

  printf("Discovering radio at %s ...\n", ipaddr_radio);
  p2_discovery();
  if (devices <= 0) { fprintf(stderr, "no radio found\n"); return; }
  const DISCOVERED *dev = &discovered[selected_device];
  printf("Using %s at %s — RX %lld Hz @ %d Hz\n", dev->name,
         inet_ntoa(dev->network.address.sin_addr), app->freq, rate);

  if (analyzer_create(0, app->pixels, rate, app->fps) != 0) {
    fprintf(stderr, "analyzer_create failed\n");
    return;
  }

  /* Audio: WDSP demod → native PipeWire sink. Default filter = mode's preset. */
  int nf, dfl;
  const FilterPreset *ft = mode_filters(mode, &nf, &dfl);
  app->filter_idx = dfl;
  app->flo = ft[dfl].low;
  app->fhi = ft[dfl].high;
  if (audio_start(48000, 1, app->latency) == 0 && demod_create(0, rate, mode, app->flo, app->fhi, app->volume) == 0) {
    demod_set_gain(app->gain);
    app->audio_ok = 1;
  } else {
    fprintf(stderr, "audio/demod init failed — panadapter only\n");
  }

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
