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
#define ENGINE_FPS    15
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

/* Standard passband [flo,fhi] Hz for a demod mode (relative to the DDC centre). */
static void passband_for_mode(int mode, double *flo, double *fhi) {
  const double st = 600.0;
  switch (mode) {
    case DEMOD_USB: *flo =  150;      *fhi = 2850;      break;
    case DEMOD_LSB: *flo = -2850;     *fhi = -150;      break;
    case DEMOD_CWU: *flo =  st - 250; *fhi = st + 250;  break;
    case DEMOD_CWL: *flo = -(st+250); *fhi = -(st-250); break;
    case DEMOD_AM:  *flo = -4000;     *fhi = 4000;      break;
    default:        *flo =  150;      *fhi = 2850;      break;
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
  s->volume = app->volume;
  s->gain   = app->gain;
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
  double hz_per_px = (double)app->rate / w;
  /* Drag right → content follows the cursor → view moves to lower frequency. */
  long long nf = app->drag_base_freq - (long long)llround(off_x * hz_per_px);
  if (nf < 1) { nf = 1; }
  app->freq = nf;             /* readout follows on the next tick */
  p2_set_frequency(nf);
  schedule_save(app);
}

/* Keys u/l/c/a switch demod mode live (radio + audio path only). */
static gboolean on_key(GtkEventControllerKey *ctl, guint keyval, guint keycode,
                       GdkModifierType state, gpointer data) {
  (void)ctl; (void)keycode; (void)state;
  App *app = (App *)data;
  if (!app->radio_mode || !app->audio_ok) { return FALSE; }
  int mode;
  switch (gdk_keyval_to_lower(keyval)) {
    case GDK_KEY_u: mode = DEMOD_USB; break;
    case GDK_KEY_l: mode = DEMOD_LSB; break;
    case GDK_KEY_c: mode = DEMOD_CWU; break;
    case GDK_KEY_a: mode = DEMOD_AM;  break;
    default: return FALSE;
  }
  double flo, fhi;
  passband_for_mode(mode, &flo, &fhi);
  demod_set_mode(mode, flo, fhi);
  app->mode = mode;
  schedule_save(app);
  printf("mode -> %d  passband [%.0f, %.0f] Hz\n", mode, flo, fhi);
  fflush(stdout);
  return TRUE;
}

static void on_activate(GtkApplication *gtkapp, gpointer data) {
  App *app = (App *)data;

  GtkWidget *win = gtk_application_window_new(gtkapp);
  gtk_window_set_title(GTK_WINDOW(win),
                       app->radio_mode ? "SDR for Linux — radio" : "SDR for Linux — server");
  gtk_window_set_default_size(GTK_WINDOW(win), 1300, 680);

  app->area = gtk_drawing_area_new();
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app->area), draw_cb, app, NULL);
  gtk_window_set_child(GTK_WINDOW(win), app->area);

  /* Input controllers (self-gate on radio mode): wheel tunes, u/l/c/a set mode. */
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
                  .volume = -10.0, .gain = 8.0 };
  g_strlcpy(st.ip, "192.168.1.247", sizeof(st.ip));
  if (settings_load(&st)) { printf("settings: loaded %s\n", settings_path()); }

  const char *e;
  if ((e = getenv("SDRFL_RADIO_IP")) && *e) { g_strlcpy(st.ip, e, sizeof(st.ip)); }
  if ((e = getenv("SDRFL_FREQ"))     && *e) { st.freq = strtoll(e, NULL, 10); }
  if ((e = getenv("SDRFL_RATE"))     && *e) { st.rate = atoi(e); }
  if ((e = getenv("SDRFL_VOLUME"))   && *e) { st.volume = atof(e); }
  if ((e = getenv("SDRFL_GAIN"))     && *e) { st.gain = atof(e); }
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
  app->pixels = ENGINE_PIXELS;
  int rate    = st.rate;

  printf("Discovering radio at %s ...\n", ipaddr_radio);
  p2_discovery();
  if (devices <= 0) { fprintf(stderr, "no radio found\n"); return; }
  const DISCOVERED *dev = &discovered[selected_device];
  printf("Using %s at %s — RX %lld Hz @ %d Hz\n", dev->name,
         inet_ntoa(dev->network.address.sin_addr), app->freq, rate);

  if (analyzer_create(0, app->pixels, rate, ENGINE_FPS) != 0) {
    fprintf(stderr, "analyzer_create failed\n");
    return;
  }

  /* Audio: WDSP demod → native PipeWire sink. */
  double flo, fhi;
  passband_for_mode(mode, &flo, &fhi);
  int lat = (getenv("SDRFL_LAT") && *getenv("SDRFL_LAT")) ? atoi(getenv("SDRFL_LAT")) : 10;
  if (audio_start(48000, 1, lat) == 0 && demod_create(0, rate, mode, flo, fhi, app->volume) == 0) {
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

  GtkApplication *gtkapp = gtk_application_new("cz.ok1br.sdr_for_linux",
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
