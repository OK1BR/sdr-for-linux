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
 * Our own ceiling on the analyzer column count (RX and TX alike). The column
 * count follows the display width (issue #15), so it is no longer the vendored
 * piHPSDR SPECTRUM_DATA_SIZE (4096 — the network protocol's cap, which stays
 * for the network head only). WDSP itself allows dMAX_PIXELS = 16384; 8192
 * covers a 5120-px monitor at scale 1 and 4 K at scale 2. Every GUI buffer
 * that holds analyzer columns is sized by this.
 */
#define ANALYZER_MAX_PIXELS 8192

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

/*
 * Pan the zoomed view within the captured span: pan in [-1,1], 0 = centred on
 * the DDC (VFO), +1 = view slid to the high-frequency edge, -1 = the low edge.
 * No effect at zoom 1. Thread-safe; cheap (only re-splits the clip). The GUI
 * must offset its VFO-centred overlays by the same pan.
 */
void analyzer_set_pan(double pan);

/* Change the target frame rate live (recomputes overlap + averaging; thread-safe).
 * The redraw gate follows automatically as the analyzer emits more/fewer frames. */
void analyzer_set_fps(int fps);

/*
 * Change the output column count live (clamped to [2, ANALYZER_MAX_PIXELS]);
 * thread-safe. Keeps zoom + pan; the FFT size follows pixels*zoom as in
 * analyzer_set_zoom. WDSP stops and restarts its dispatcher for this, so the
 * caller debounces (a resize in progress must not call it per frame). The
 * first frame after the change may still carry the old column count's data —
 * skip it. analyzer_get_pixels() needs `pixels` >= the new count.
 */
void analyzer_set_pixels(int pixels);

/* Destroy the analyzer and free buffers. */
void analyzer_destroy(void);

#endif /* SDRFL_ENGINE_ANALYZER_H */
