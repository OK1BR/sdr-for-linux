/*
 * sdr-for-linux — WDSP demodulator wrapper (headless). See demod.c.
 *
 * A second consumer of the RX IQ (the analyzer is the first): accumulates the
 * same 1024-sample block, runs fexchange0, and pushes the demodulated mono
 * audio to the sink (audio.h). Own small state; no piHPSDR RECEIVER.
 */
#ifndef SDRFL_ENGINE_DEMOD_H
#define SDRFL_ENGINE_DEMOD_H

/* WDSP mode ids (match wdsp/RXA.h and piHPSDR mode.h). */
enum { DEMOD_LSB = 0, DEMOD_USB = 1, DEMOD_CWL = 3, DEMOD_CWU = 4, DEMOD_AM = 6 };

/*
 * Open demod channel `id` for `in_rate` Hz IQ, `mode`, passband [flo,fhi] Hz,
 * AF `volume` in dB (0..−40). The audio sink (audio_start) must already be up.
 * Returns 0 on success.
 */
int demod_create(int id, int in_rate, int mode, double flo, double fhi, double volume);

/*
 * Feed interleaved double I/Q (n_pairs). Accumulates to the 1024 block, runs
 * fexchange0, pushes mono audio. Call from the RX listener thread.
 */
void demod_feed(const double *iq, int n_pairs);

/*
 * Live mode/filter change on the running channel: SetRXAMode + RXASetPassband.
 * Thread-safe (fenced against demod_feed); safe to call from the GUI thread.
 */
void demod_set_mode(int mode, double flo, double fhi);

/* Set the digital master gain applied after WDSP (thread-safe). */
void demod_set_gain(double gain);

/* AGC character (0=off,1=long,2=slow,3=medium,4=fast) + AGC-T gain (dB). */
void demod_set_agc(int mode);
void demod_set_agc_gain(double db);

/* Noise reduction (ANR) / noise blanker (ANB) / auto-notch filter — on/off. */
void demod_set_nr(int on);
void demod_set_nb(int on);
void demod_set_anf(int on);

/* Set the AF volume (dB, 0..-40) live on the running channel (thread-safe). */
void demod_set_volume(double db);

/* Set the filter passband [flo,fhi] Hz live (same mode); thread-safe. */
void demod_set_passband(double flo, double fhi);

void demod_destroy(void);

/* Diagnostics: peak |audio| since last call (and reset); last fexchange0 error. */
double demod_peak(void);
int    demod_last_error(void);

#endif /* SDRFL_ENGINE_DEMOD_H */
