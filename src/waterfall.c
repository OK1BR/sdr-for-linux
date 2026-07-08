/*
 * waterfall.c — see waterfall.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "waterfall.h"

#include <stdlib.h>
#include <string.h>

/*
 * The colour map is auto-ranged: the low end tracks the noise floor (a low
 * percentile of each frame) and the top sits WF_SPAN dB above it. This keeps
 * high contrast — dark noise, bright signals — regardless of band conditions,
 * which is what makes the waterfall look crisp rather than a low-contrast wash.
 */
#define WF_ROWS       256
#define WF_SPAN        42.0   /* dB from noise floor to the top of the palette */
#define WF_NOISE_PCT   20     /* percentile used as the noise-floor estimate   */
#define WF_LOW_SMOOTH  0.04   /* EMA factor for the tracked noise floor        */

struct Waterfall {
  cairo_surface_t *surf;   /* ARGB32, cols x WF_ROWS; row 0 is newest */
  int              cols;
  uint32_t         lut[256];
  uint8_t         *idx;    /* cols x WF_ROWS palette indices (row 0 newest), so
                              a palette switch can recolour the whole history */
  double           auto_low;   /* tracked noise floor (dBm) */
  int              auto_init;
};

/* -------- Selectable colour palettes ---------------------------------------
 * A palette is a table of {position, r, g, b} stops (0-1 position, 0-255 rgb),
 * linearly interpolated. The selected palette is a module-global so the
 * panadapter (which shares waterfall_palette_rgb for its fill/trace/background)
 * recolours in lockstep with the waterfall. "Classic" stays index 0 as the
 * back-compat default. */
typedef struct { double t, r, g, b; } PalStop;
typedef struct { const char *name; const PalStop *stop; int n; } Palette;

/* Classic: black-blue -> cyan -> green -> yellow -> orange -> red. */
static const PalStop pal_classic[] = {
  {0.00,   0,   0,  12}, {0.20,   0,  18, 110}, {0.40,   0, 130, 180},
  {0.58,   0, 200, 120}, {0.74, 180, 220,   0}, {0.88, 255, 165,   0},
  {1.00, 255,  40,  30},
};
/* Mono white: neutral greyscale, black -> white (Richard's favourite). */
static const PalStop pal_mono_white[] = {
  {0.00, 0, 0, 0}, {0.35, 68, 68, 68}, {0.72, 168, 168, 168}, {1.00, 255, 255, 255},
};
/* Mono green: retro phosphor. */
static const PalStop pal_mono_green[] = {
  {0.00, 0, 3, 0}, {0.35, 0, 70, 18}, {0.70, 40, 190, 60}, {1.00, 200, 255, 190},
};
/* Mono amber: warm monochrome. */
static const PalStop pal_mono_amber[] = {
  {0.00, 3, 1, 0}, {0.35, 90, 45, 0}, {0.70, 220, 140, 15}, {1.00, 255, 240, 205},
};
/* Inferno: perceptually-uniform black -> purple -> red -> orange -> yellow. */
static const PalStop pal_inferno[] = {
  {0.00, 0, 0, 4}, {0.15, 22, 11, 57}, {0.30, 66, 10, 104}, {0.45, 114, 31, 107},
  {0.60, 168, 46, 86}, {0.74, 216, 83, 45}, {0.87, 245, 140, 20}, {1.00, 252, 255, 164},
};
/* Turbo: high-detail improved rainbow. */
static const PalStop pal_turbo[] = {
  {0.00, 48, 18, 59}, {0.13, 62, 74, 211}, {0.25, 40, 150, 240}, {0.38, 43, 210, 180},
  {0.50, 130, 235, 80}, {0.62, 220, 220, 40}, {0.75, 250, 150, 30}, {0.88, 230, 60, 20},
  {1.00, 122, 4, 3},
};

static const Palette g_palettes[] = {
  { "Classic",    pal_classic,    (int)(sizeof(pal_classic)    / sizeof(pal_classic[0])) },
  { "Mono white", pal_mono_white, (int)(sizeof(pal_mono_white) / sizeof(pal_mono_white[0])) },
  { "Mono green", pal_mono_green, (int)(sizeof(pal_mono_green) / sizeof(pal_mono_green[0])) },
  { "Mono amber", pal_mono_amber, (int)(sizeof(pal_mono_amber) / sizeof(pal_mono_amber[0])) },
  { "Inferno",    pal_inferno,    (int)(sizeof(pal_inferno)    / sizeof(pal_inferno[0])) },
  { "Turbo",      pal_turbo,      (int)(sizeof(pal_turbo)      / sizeof(pal_turbo[0])) },
};
static int g_sel = 0;   /* selected palette (shared with the panadapter) */

int waterfall_palette_count(void) {
  return (int)(sizeof(g_palettes) / sizeof(g_palettes[0]));
}
const char *waterfall_palette_name(int idx) {
  if (idx < 0 || idx >= waterfall_palette_count()) { return ""; }
  return g_palettes[idx].name;
}
int waterfall_get_palette(void) { return g_sel; }

void waterfall_palette_rgb(double t, double *r, double *g, double *b) {
  const Palette *p = &g_palettes[g_sel];
  const PalStop *stop = p->stop;
  const int nstops = p->n;
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  int s = 0;
  while (s < nstops - 2 && t > stop[s + 1].t) {
    s++;
  }
  double t0 = stop[s].t, t1 = stop[s + 1].t;
  double f = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0;
  if (f < 0) f = 0;
  if (f > 1) f = 1;
  *r = (stop[s].r + f * (stop[s + 1].r - stop[s].r)) / 255.0;
  *g = (stop[s].g + f * (stop[s + 1].g - stop[s].g)) / 255.0;
  *b = (stop[s].b + f * (stop[s + 1].b - stop[s].b)) / 255.0;
}

static void build_lut(uint32_t *lut) {
  for (int i = 0; i < 256; i++) {
    double r, g, b;
    waterfall_palette_rgb(i / 255.0, &r, &g, &b);
    lut[i] = 0xFFu << 24 | (uint32_t)(r * 255) << 16 |
             (uint32_t)(g * 255) << 8 | (uint32_t)(b * 255);
  }
}

/* Switch palette. Rebuilds this waterfall's LUT and recolours the whole stored
 * history through it, so the change is instant across every row (not only rows
 * that arrive after the switch). The panadapter picks up g_sel on its next draw. */
void waterfall_set_palette(Waterfall *wf, int idx) {
  if (idx < 0 || idx >= waterfall_palette_count()) { return; }
  g_sel = idx;
  if (!wf) { return; }
  build_lut(wf->lut);
  if (wf->surf && wf->idx) {
    cairo_surface_flush(wf->surf);
    uint32_t *data = (uint32_t *)cairo_image_surface_get_data(wf->surf);
    int stride = cairo_image_surface_get_stride(wf->surf) / 4;
    for (int r = 0; r < WF_ROWS; r++) {
      for (int x = 0; x < wf->cols; x++) {
        data[r * stride + x] = wf->lut[wf->idx[r * wf->cols + x]];
      }
    }
    cairo_surface_mark_dirty(wf->surf);
  }
}

void waterfall_range(const Waterfall *wf, double *low, double *span) {
  *low = wf->auto_init ? wf->auto_low : -120.0;
  *span = WF_SPAN;
}

Waterfall *waterfall_new(void) {
  Waterfall *wf = calloc(1, sizeof(Waterfall));
  build_lut(wf->lut);
  return wf;
}

void waterfall_free(Waterfall *wf) {
  if (!wf) {
    return;
  }
  if (wf->surf) {
    cairo_surface_destroy(wf->surf);
  }
  free(wf->idx);
  free(wf);
}

static void ensure_surface(Waterfall *wf, int n) {
  if (wf->surf && wf->cols == n) {
    return;
  }
  if (wf->surf) {
    cairo_surface_destroy(wf->surf);
  }
  wf->surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, n, WF_ROWS);
  wf->cols = n;
  free(wf->idx);
  wf->idx = calloc((size_t)n * WF_ROWS, 1);   /* all-zero = palette index 0 */
  /* Clear to the palette's lowest colour. */
  cairo_surface_flush(wf->surf);
  uint32_t *data = (uint32_t *)cairo_image_surface_get_data(wf->surf);
  int stride = cairo_image_surface_get_stride(wf->surf) / 4;
  for (int r = 0; r < WF_ROWS; r++) {
    for (int x = 0; x < n; x++) {
      data[r * stride + x] = wf->lut[0];
    }
  }
  cairo_surface_mark_dirty(wf->surf);
}

void waterfall_push(Waterfall *wf, const uint8_t *dbm, int n) {
  if (n < 1) {
    return;
  }
  ensure_surface(wf, n);

  /* Estimate the noise floor from this frame (WF_NOISE_PCT-th percentile) via
   * a coarse histogram, then track it slowly so the mapping stays stable. */
  int hist[140];
  memset(hist, 0, sizeof(hist));
  const double hlo = -160.0, hbin = 1.0; /* 1 dB bins over -160..-20 */
  for (int x = 0; x < n; x++) {
    int b = (int)(((double)dbm[x] - 200.0) - hlo);
    if (b < 0) b = 0;
    if (b > 139) b = 139;
    hist[b]++;
  }
  int target = n * WF_NOISE_PCT / 100, cum = 0, pb = 0;
  for (int b = 0; b < 140; b++) {
    cum += hist[b];
    if (cum >= target) { pb = b; break; }
  }
  double noise = hlo + pb + 0.5;
  if (!wf->auto_init) {
    wf->auto_low = noise;
    wf->auto_init = 1;
  } else {
    wf->auto_low += WF_LOW_SMOOTH * (noise - wf->auto_low);
  }

  cairo_surface_flush(wf->surf);
  uint8_t *base = cairo_image_surface_get_data(wf->surf);
  int stride = cairo_image_surface_get_stride(wf->surf); /* bytes */

  /* Scroll everything down by one row, then write the new row at the top. The
   * palette-index buffer scrolls in lockstep so a later palette switch can
   * recolour the entire history. */
  memmove(base + stride, base, (size_t)stride * (WF_ROWS - 1));
  if (wf->idx) {
    memmove(wf->idx + n, wf->idx, (size_t)n * (WF_ROWS - 1));
  }

  uint32_t *row = (uint32_t *)base;
  const double low = wf->auto_low;
  const double scale = 255.0 / WF_SPAN;
  for (int x = 0; x < n; x++) {
    double d = (double)dbm[x] - 200.0;
    int idx = (int)((d - low) * scale);
    if (idx < 0) idx = 0;
    if (idx > 255) idx = 255;
    if (wf->idx) { wf->idx[x] = (uint8_t)idx; }
    row[x] = wf->lut[idx];
  }

  cairo_surface_mark_dirty(wf->surf);
}

void waterfall_draw(Waterfall *wf, cairo_t *cr, int x, int y, int w, int h) {
  if (!wf->surf || wf->cols < 1) {
    return;
  }
  cairo_save(cr);
  cairo_rectangle(cr, x, y, w, h);
  cairo_clip(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, (double)w / wf->cols, (double)h / WF_ROWS);
  cairo_set_source_surface(cr, wf->surf, 0, 0);
  /* NEAREST keeps signal streaks crisp (like piHPSDR) instead of blurring. */
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
  cairo_paint(cr);
  cairo_restore(cr);
}
