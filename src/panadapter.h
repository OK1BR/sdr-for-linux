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
 */
void panadapter_draw(cairo_t *cr, int w, int h,
                     const ClientFrame *frame, const float *dbm,
                     double cmap_low, double cmap_span,
                     const char *status);

#endif /* PIHPSDR_CLIENT_PANADAPTER_H */
