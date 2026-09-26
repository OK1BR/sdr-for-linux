/*
 * panadapter.h — Cairo rendering of the RX panadapter.
 *
 * Pure drawing: given a cairo context, a size and a decoded frame, it paints
 * the dark background, dB grid, gradient-filled spectrum trace, VFO centre line
 * and readouts. Kept free of GTK so it can be driven both by the live GUI and
 * by a headless render test.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PIHPSDR_CLIENT_PANADAPTER_H
#define PIHPSDR_CLIENT_PANADAPTER_H

#include <cairo.h>
#include <stdint.h>

/*
 * Canvas fonts. The generic Cairo "monospace" resolves (via fontconfig) to a
 * serif Courier clone — Nimbus Mono PS — on this system, which clashes with the
 * sans libadwaita UI. Name the libadwaita families explicitly so the whole app
 * reads as one type system: Adwaita Mono (tabular readouts) + Adwaita Sans (UI).
 */
#define FONT_MONO "Adwaita Mono"
#define FONT_UI   "Adwaita Sans"

#include "client.h"

/*
 * Render into `cr` at size w x h.
 *   - if `status` is non-NULL, the background + grid are drawn with that message
 *     centred (used for "connecting" / error states) and `frame` is ignored;
 *   - otherwise `frame` (non-NULL) supplies the readouts (freq, S-meter) and,
 *     when `dbm` is NULL, the trace as well (raw 1 dB steps). If `dbm` is
 *     non-NULL it is a `frame->width`-long array of smoothed dBm values (e.g.
 *     time-averaged) used for the trace, giving a finer sub-dB curve.
 *
 * `cmap_low` / `cmap_span` set the dBm->colour mapping (noise floor and dB
 * span) so the trace and its fill share the waterfall's hues — pass the values
 * from waterfall_range().
 *
 * `band`, when non-NULL, is appended to the VFO readout (e.g. "… VFO A · 20m").
 * `vfo_frac` places the VFO centre line (0..1 across the width; 0.5 = centred,
 * off-centre when panned). Outside [0,1] the line is hidden (VFO panned away).
 */
void panadapter_draw(cairo_t *cr, int w, int h,
                     const ClientFrame *frame, const float *dbm,
                     double cmap_low, double cmap_span,
                     const char *status, const char *band, double vfo_frac);

/*
 * Set the visible amplitude window (dBm) for the vertical axis: `high` at the
 * top, `low` at the bottom. Drives both the trace mapping and the dB scale
 * labels. Persistent module state — call before panadapter_draw() each frame.
 * `high` must be > `low`; out-of-order or degenerate values are ignored.
 */
void panadapter_set_range(double high, double low);

/*
 * Toggle the dB grid lines and the dB scale labels independently (persistent
 * module state; set before panadapter_draw() each frame). Both default on.
 */
void panadapter_set_grid(int show_grid, int show_labels);

/*
 * GPU spectrum body (issue #15). The fill and the trace are the expensive part
 * of the cairo raster (a per-pixel path fill plus one stroke per column — GTK
 * rasterizes the whole cairo node on the CPU at render time, 7–13 ms at
 * 2048–5120 px). On the snapshot path the GUI renders them as GPU nodes
 * instead, from data this module computes:
 *   fill  = A8 coverage mask × a vertical gradient (palette by the dBm at
 *           that row, alpha 0.55 — panadapter_fill_stops);
 *   trace = A8 coverage mask × a W×1 colour strip stretched vertically
 *           (per-column colour = the palette at the brighter of the two
 *           columns the segment joins, lifted 50 % to white, alpha 0.98 —
 *           exactly draw_spectrum's per-segment colouring).
 * panadapter_set_body(0) then makes panadapter_draw() paint only the readout
 * block; the background, the dB grid lines and labels, the body and the VFO
 * line are the caller's nodes (panadapter_draw_db_labels for the gutter, the
 * VFO line = PANADAPTER_VFO_RGB at 60 %, 0.75 px, pixel-centre snapped).
 * Status screens always paint in full. The masks are in DEVICE pixels.
 */
void panadapter_set_body(int on);   /* off: panadapter_draw() paints only the readout */
/* Background colour = the palette's noise-floor colour (rgb 0..1). */
void panadapter_bg_rgb(double *r, double *g, double *b);
/* dB grid lines: y (px, top of the 1-px line) of each line in an h-px strip;
 * returns the count (0 when the grid is off). `rgba` = the line colour. */
int  panadapter_grid_rows(int h, double *ys, int max, double *rgba);
/* Fill gradient stops, top → bottom: offs[i] in 0..1, rgba[4*i..]. Returns n. */
int  panadapter_fill_stops(double cmap_low, double cmap_span, float *offs, float *rgba, int max);
/* Step 1: plot `n` dBm columns into a W×H strip (current dB range) and return
 * the row band [r0, r1) the masks need — above r0 both are empty, below r1 the
 * fill is solid (the caller paints the plain gradient there) and the trace
 * empty. A weak band keeps the masks a fraction of the strip. Returns 0 (no
 * band) when n < 2 or the strip is degenerate. */
int  panadapter_body_band(const float *dbm, int n, int W, int H, int *r0, int *r1);
/* Step 2 (same thread, right after step 1): the coverage masks for rows
 * [r0, r1) — `fill` and `trace` are W*(r1-r0) bytes, row-major (0 = clear,
 * 255 = full) — and `strip`, W premultiplied BGRA pixels (the trace colour per
 * column). */
void panadapter_body_masks(int W, int r0, int r1, double cmap_low, double cmap_span,
                           uint8_t *fill, uint8_t *trace, uint32_t *strip);
/* Body off: the dB grid LABELS alone (the gutter), for a narrow node. */
void panadapter_draw_db_labels(cairo_t *cr, int h);

/*
 * Suppress (0) or restore (1) the built-in top-left readout (VFO frequency +
 * sub-line). The GUI turns it off around the TX panadapter so it can draw its own
 * red power/SWR readout in its place. Persistent module state; default on.
 */
void panadapter_set_readout(int on);

/* Measured extent (right edge x1, bottom y1) of the last-drawn VFO readout
 * block — overlays (DX-spot labels) use it to steer clear. Zero before the
 * first draw or while the readout is suppressed. */
void panadapter_readout_extent(double *x1, double *y1);

/* Format Hz as a grouped string, e.g. 14250000 -> "14 250 000". */
void panadapter_format_hz(long long hz, char *buf, size_t n);

/* Width (px) of the left dB-scale gutter — the grab zone for vertical
 * pan/zoom. The GUI hit-tests against this so the affordance matches the draw. */
#define PANADAPTER_GUTTER_W 46
/* The RX VFO line colour (RGB for cairo_set_source_rgba(cr, PANADAPTER_VFO_RGB, a)).
 * Colour language (Richard 2026-09-06): RX = green, TX = red. Shared with
 * gui.c (waterfall carry-down, select-mode aim line, filter-dialog carrier). */
#define PANADAPTER_VFO_RGB 0.42, 0.96, 0.56

#endif /* PIHPSDR_CLIENT_PANADAPTER_H */
