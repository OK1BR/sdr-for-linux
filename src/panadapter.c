/*
 * panadapter.c — see panadapter.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "panadapter.h"
#include "waterfall.h"   /* shared amplitude palette */

#include <stdio.h>
#include <string.h>
#include <math.h>

/* Visible amplitude window (dBm) — runtime-adjustable (drag/scroll the scale,
 * or Settings). Defaults match the old fixed window. */
static double pan_high = -50.0;
static double pan_low  = -140.0;

void panadapter_set_range(double high, double low) {
  if (!(high > low)) { return; }   /* reject NaN / inverted / degenerate */
  pan_high = high;
  pan_low  = low;
}

/* dB grid lines + scale labels, toggled independently (default on). */
static int show_db_grid   = 1;
static int show_db_labels = 1;

void panadapter_set_grid(int show_grid, int show_labels) {
  show_db_grid   = show_grid;
  show_db_labels = show_labels;
}

/* dB grid spacing chosen so the window shows ~4-6 labelled lines regardless of
 * the current range (a fixed 20 dB step goes sparse when zoomed in). */
static double pan_grid_step(void) {
  double range = pan_high - pan_low;
  const double steps[] = { 5, 10, 20, 50, 100 };
  for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
    if (range / steps[i] <= 6.0) { return steps[i]; }
  }
  return 100.0;
}

/* Format Hz as a grouped string, e.g. 14250000 -> "14 250 000". */
static void format_hz(long long hz, char *buf, size_t n) {
  int neg = hz < 0;
  unsigned long long v = neg ? (unsigned long long)(-hz) : (unsigned long long)hz;
  char tmp[32];
  int len = snprintf(tmp, sizeof(tmp), "%llu", v);
  char out[48];
  int oi = 0;
  for (int i = 0; i < len; i++) {
    if (i > 0 && (len - i) % 3 == 0) {
      out[oi++] = ' ';
    }
    out[oi++] = tmp[i];
  }
  out[oi] = '\0';
  snprintf(buf, n, "%s%s", neg ? "-" : "", out);
}

/* Map a dBm value to a y pixel within [0,h]. */
static double dbm_to_y(double dbm, int h) {
  double t = (pan_high - dbm) / (pan_high - pan_low);
  if (t < 0.0) t = 0.0;
  if (t > 1.0) t = 1.0;
  return t * h;
}

static void draw_grid(cairo_t *cr, int w, int h) {
  if (!show_db_grid && !show_db_labels) { return; }
  cairo_select_font_face(cr, FONT_MONO, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 11.0);
  cairo_set_line_width(cr, 1.0);

  double step = pan_grid_step();
  double top = floor(pan_high / step) * step;   /* align labels to the step grid */
  for (double db = top; db >= pan_low; db -= step) {
    double y = dbm_to_y(db, h);
    if (show_db_grid) {
      cairo_set_source_rgba(cr, 0.45, 0.55, 0.65, 0.14);
      cairo_move_to(cr, 0, y + 0.5);
      cairo_line_to(cr, w, y + 0.5);
      cairo_stroke(cr);
    }
    if (show_db_labels) {
      char lbl[16];
      snprintf(lbl, sizeof(lbl), "%d", (int)db);
      cairo_set_source_rgba(cr, 0.55, 0.65, 0.75, 0.75);
      cairo_move_to(cr, 4, y - 3);
      cairo_show_text(cr, lbl);
    }
  }
}

/* Linearly interpolated dBm at fractional column position for smooth scaling. */
static double dbm_lerp(const float *dbm, int n, double colf) {
  if (colf <= 0) {
    return dbm[0];
  }
  if (colf >= n - 1) {
    return dbm[n - 1];
  }
  int c0 = (int)colf;
  double t = colf - c0;
  return (1.0 - t) * dbm[c0] + t * dbm[c0 + 1];
}

/*
 * dBm to plot for output pixel x. When there are more data columns than pixels
 * (n > w, e.g. 4096 columns in a 1200 px window) take the MAX over the columns
 * that fall in this pixel so signal peaks are never skipped; otherwise
 * interpolate for a smooth upscaled curve.
 */
static double column_value(const float *dbm, int n, int x, int w) {
  double c0 = (double)x * n / w;
  double c1 = (double)(x + 1) * n / w;
  if (c1 - c0 >= 1.0) {
    int a = (int)c0, b = (int)c1;
    if (a < 0) a = 0;
    if (b > n) b = n;
    double m = dbm[a < n ? a : n - 1];
    for (int k = a + 1; k < b; k++) {
      if (dbm[k] > m) m = dbm[k];
    }
    return m;
  }
  return dbm_lerp(dbm, n, c0);
}

/*
 * `dbm` is a length-n array of dBm values (already smoothed by the caller).
 * `low`/`span` define the dBm->colour mapping shared with the waterfall, so the
 * fill and trace go blue at the noise floor and red at strong peaks.
 */
static void draw_spectrum(cairo_t *cr, const float *dbm, int n, int w, int h,
                          double low, double span) {
  if (n < 2) {
    return;
  }

  /* Filled area under the trace. */
  cairo_new_path(cr);
  cairo_move_to(cr, 0, h);
  for (int x = 0; x < w; x++) {
    cairo_line_to(cr, x, dbm_to_y(column_value(dbm, n, x, w), h));
  }
  cairo_line_to(cr, w, h);
  cairo_close_path(cr);

  /* Vertical gradient coloured by amplitude (same palette as the waterfall):
   * each y maps to a dBm, then to a palette colour. */
  cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, h);
  for (int s = 0; s <= 24; s++) {
    double off = s / 24.0;
    double d = pan_high - off * (pan_high - pan_low);
    double r, g, b;
    waterfall_palette_rgb((d - low) / span, &r, &g, &b);
    cairo_pattern_add_color_stop_rgba(grad, off, r, g, b, 0.55);
  }
  cairo_set_source(cr, grad);
  cairo_fill(cr);
  cairo_pattern_destroy(grad);

  /* Trace, coloured per segment by its own amplitude. */
  cairo_set_line_width(cr, 1.2);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  double pd = column_value(dbm, n, 0, w);
  double py = dbm_to_y(pd, h);
  for (int x = 1; x < w; x++) {
    double d = column_value(dbm, n, x, w);
    double y = dbm_to_y(d, h);
    double r, g, b;
    /* colour by the brighter of the two endpoints so peaks stay vivid */
    double dc = (d > pd) ? d : pd;
    waterfall_palette_rgb((dc - low) / span, &r, &g, &b);
    /* lift strongly toward white so the trace reads bright/near-white above the
     * fill (peaks saturate to white); keeps the palette's hue direction. */
    cairo_set_source_rgba(cr, 0.5 + 0.5 * r, 0.5 + 0.5 * g, 0.5 + 0.5 * b, 0.98);
    cairo_move_to(cr, x - 1, py);
    cairo_line_to(cr, x, y);
    cairo_stroke(cr);
    pd = d;
    py = y;
  }
}

static void draw_center_line(cairo_t *cr, int w, int h, double vfo_frac) {
  if (vfo_frac < 0.0 || vfo_frac > 1.0) { return; }   /* VFO panned off-screen */
  double x = vfo_frac * w;
  cairo_set_source_rgba(cr, 1.0, 0.78, 0.25, 0.45);
  cairo_set_line_width(cr, 1.0);
  cairo_move_to(cr, x + 0.5, 0);
  cairo_line_to(cr, x + 0.5, h);
  cairo_stroke(cr);
}

static int p_show_readout = 1;   /* GUI suppresses this to draw a custom TX readout */

void panadapter_set_readout(int on) { p_show_readout = on; }

static void draw_readouts(cairo_t *cr, const ClientFrame *f, int w, const char *band) {
  char buf[64];

  /* VFO frequency (big), top-left, clear of the top freq ruler. Prefer CTUN if it
   * differs from the dial. */
  cairo_select_font_face(cr, FONT_MONO, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 32.0);
  long long freq = (f->vfo_a_ctun_freq && f->vfo_a_ctun_freq != f->vfo_a_freq)
                     ? f->vfo_a_ctun_freq : f->vfo_a_freq;
  format_hz(freq, buf, sizeof(buf));
  cairo_set_source_rgba(cr, 0.93, 0.96, 1.0, 0.96);
  cairo_move_to(cr, 44, 60);
  cairo_show_text(cr, buf);

  /* Sub-line under the frequency: "Hz · VFO A", plus the band-plan band +
   * recommended mode when supplied. Enlarged for legibility. */
  char sub[64];
  if (band && *band) { snprintf(sub, sizeof sub, "Hz  ·  VFO A  ·  %s", band); }
  else               { snprintf(sub, sizeof sub, "Hz  ·  VFO A"); }
  cairo_set_font_size(cr, 15.0);
  cairo_set_source_rgba(cr, 0.62, 0.72, 0.85, 0.9);
  cairo_move_to(cr, 44, 82);
  cairo_show_text(cr, sub);
  /* S-meter is drawn by the GUI overlay (graphical bar), not here. */
  (void)w;
}

static void draw_status(cairo_t *cr, const char *msg, int w, int h) {
  cairo_select_font_face(cr, FONT_UI, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 16.0);
  cairo_text_extents_t ext;
  cairo_text_extents(cr, msg, &ext);
  cairo_set_source_rgba(cr, 0.8, 0.85, 0.9, 0.85);
  cairo_move_to(cr, (w - ext.width) / 2.0, h / 2.0);
  cairo_show_text(cr, msg);
}

void panadapter_draw(cairo_t *cr, int w, int h,
                     const ClientFrame *frame, const float *dbm,
                     double cmap_low, double cmap_span,
                     const char *status, const char *band, double vfo_frac) {
  /* Background = the palette's noise-floor colour, so the empty area matches the
   * fill under the trace at the floor (and the waterfall) — no cool-grey seam. */
  double br, bg, bb;
  waterfall_palette_rgb(0.0, &br, &bg, &bb);
  cairo_set_source_rgb(cr, br, bg, bb);
  cairo_paint(cr);

  draw_grid(cr, w, h);

  if (status) {
    draw_status(cr, status, w, h);
    return;
  }

  /* Use the caller's smoothed dBm if provided; else derive raw dBm from bytes. */
  const float *vals = dbm;
  float tmp[SPECTRUM_DATA_SIZE];
  if (!vals) {
    for (int i = 0; i < frame->width; i++) {
      tmp[i] = (float)frame->dbm[i] - 200.0f;
    }
    vals = tmp;
  }

  if (cmap_span < 1.0) cmap_span = 1.0;
  draw_spectrum(cr, vals, frame->width, w, h, cmap_low, cmap_span);
  draw_center_line(cr, w, h, vfo_frac);
  if (p_show_readout) { draw_readouts(cr, frame, w, band); }
}
