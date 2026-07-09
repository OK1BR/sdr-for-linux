/*
 * sdr-for-linux — TX panadapter analyzer (F6a, docs/TX-DESIGN.md).
 *
 * A second WDSP analyzer instance (disp id distinct from the RX analyzer's) that
 * turns the transmitted IQ (the WDSP TX channel output, engine/tx.c) into
 * panadapter pixels — the spectrum of what we are transmitting. Mirrors piHPSDR
 * tx_set_analyzer (transmitter.c:2340-2409 @974acba): a fixed 24 kHz-wide window
 * centred on the carrier, clipped from the 192 kHz TX IQ. No zoom/pan (TX span is
 * always 24 kHz). Fed only while keyed (the TX channel produces IQ only then).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SDRFL_ENGINE_TX_ANALYZER_H
#define SDRFL_ENGINE_TX_ANALYZER_H

/*
 * Create the TX analyzer producing `pixels` columns across the 24 kHz TX span
 * from an `iq_rate` Hz complex TX-IQ stream fed `bf_size` pairs at a time,
 * targeting `fps`. Returns 0 on success.
 */
int  tx_analyzer_create(int pixels, int iq_rate, int bf_size, int fps);

/* Feed `n_pairs` interleaved double I/Q (the TX channel output). Re-buffers to
 * bf_size and calls Spectrum0. Safe from the TX worker thread. */
void tx_analyzer_feed(const double *iq, int n_pairs);

/* Copy the latest TX pixel frame (dB) into out[0..pixels-1]. Returns 1 if fresh. */
int  tx_analyzer_get_pixels(float *out, int pixels);

/* Re-zoom the TX window to `span_hz` wide (clamped to [300, base]); reconfigures
 * the WDSP analyzer (afft chosen for sharpness). Safe to call live from the GUI. */
void   tx_analyzer_set_span(double span_hz);
double tx_analyzer_base_span(void);   /* the full (zoom-1) span, Hz */

void tx_analyzer_destroy(void);

#endif /* SDRFL_ENGINE_TX_ANALYZER_H */
