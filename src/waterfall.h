/*
 * waterfall.h — scrolling waterfall for the RX spectrum.
 *
 * Keeps a colour-mapped image of recent spectrum rows. Each pushed frame adds
 * a new line at the top and scrolls the rest down. Rendered with Cairo. GTK-free
 * so it can also be driven by the headless render test.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PIHPSDR_CLIENT_WATERFALL_H
#define PIHPSDR_CLIENT_WATERFALL_H

#include <stdint.h>
#include <cairo.h>

typedef struct Waterfall Waterfall;

Waterfall *waterfall_new(void);
void       waterfall_free(Waterfall *wf);

/*
 * Add one spectrum row. `dbm` holds `n` raw column bytes (dBm = dbm[i] - 200).
 * The waterfall auto-sizes to `n` columns; a width change clears the history.
 */
void waterfall_push(Waterfall *wf, const uint8_t *dbm, int n);

/* Blit the waterfall into the rectangle (x,y,w,h), scaled to fit. */
void waterfall_draw(Waterfall *wf, cairo_t *cr, int x, int y, int w, int h);

/*
 * Shared amplitude palette, so the panadapter can colour its trace/fill with
 * the same hues (noise = blue … peaks = red). `t` in [0,1], returns rgb [0,1].
 */
void waterfall_palette_rgb(double t, double *r, double *g, double *b);

/* Current colour-map range in dBm: *low = tracked noise floor, *span = dB to
 * the top of the palette. Lets the panadapter map dBm -> colour identically. */
void waterfall_range(const Waterfall *wf, double *low, double *span);

/*
 * Pin the colour-map to a fixed [low, low+span] dB window instead of the auto
 * noise-floor tracking (span > 0). Used by the TX waterfall so it follows the
 * operator's manual dB scale. span <= 0 restores auto-ranging.
 */
void waterfall_set_manual_range(Waterfall *wf, double low, double span);

/*
 * Selectable colour schemes. The selected palette backs waterfall_palette_rgb()
 * (and therefore both the waterfall and the panadapter). Index 0 is "Classic".
 */
int         waterfall_palette_count(void);
const char *waterfall_palette_name(int idx);   /* "" if out of range */
int         waterfall_get_palette(void);
/* Select palette `idx`: rebuilds `wf`'s LUT and recolours its whole history in
 * place. Pass wf = NULL to only set the global (e.g. before a waterfall exists). */
void        waterfall_set_palette(Waterfall *wf, int idx);

#endif /* PIHPSDR_CLIENT_WATERFALL_H */
