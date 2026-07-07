/*
 * sdr-for-linux — thin WDSP analyzer wrapper (headless).
 *
 * Turns an interleaved double I/Q stream (as delivered by protocol2.c's
 * on_rx_iq) into panadapter pixels using WDSP's analyzer (XCreateAnalyzer /
 * SetAnalyzer / Spectrum0 / GetPixels). Holds its own small state — it does NOT
 * pull piHPSDR's RECEIVER struct (Option B, see docs/WDSP-ANALYZER-SCOPE.md).
 * Reference RX values from piHPSDR receiver.c @ 974acba.
 *
 * A single analyzer instance (one RX) for now; the WDSP `disp` id is the arg.
 */
#ifndef SDRFL_ENGINE_ANALYZER_H
#define SDRFL_ENGINE_ANALYZER_H

/*
 * Create the analyzer `id` producing `pixels` output columns from a
 * `sample_rate` Hz complex IQ stream, targeting `fps` frames/s. Configures FFT
 * size, window, overlap, averaging and detector. Returns 0 on success.
 */
int analyzer_create(int id, int pixels, int sample_rate, int fps);

/*
 * Feed `n_pairs` interleaved double I/Q samples ([I0,Q0,I1,Q1,...]). Re-buffers
 * to WDSP's block size and calls Spectrum0 once per full block. Safe to call
 * from the RX listener thread (fenced against create/destroy).
 */
void analyzer_feed(const double *iq, int n_pairs);

/*
 * Copy the latest pixel frame (dB, see scope doc §4) into out[0..pixels-1].
 * Returns 1 if a fresh frame was available (out written), 0 otherwise (out
 * untouched). Poll at ~fps.
 */
int analyzer_get_pixels(float *out, int pixels);

/*
 * Set the zoom factor (1 = full span; Z shows sample_rate/Z centred on the DDC).
 * Re-configures the analyzer's span clip (fscLin/fscHin); thread-safe. Sharp up
 * to ~afft/pixels (≈8× at 192 kHz / A_MSIZE 16384), interpolated beyond.
 */
void analyzer_set_zoom(double zoom);

/* Change the target frame rate live (recomputes overlap + averaging; thread-safe).
 * The redraw gate follows automatically as the analyzer emits more/fewer frames. */
void analyzer_set_fps(int fps);

/* Destroy the analyzer and free buffers. */
void analyzer_destroy(void);

#endif /* SDRFL_ENGINE_ANALYZER_H */
