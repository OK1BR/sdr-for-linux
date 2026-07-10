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
/* Mode ids == WDSP RXA/TXA mode enum (RXA.h): LSB 0, USB 1, DSB 2, CWL 3,
 * CWU 4, FM 5, AM 6, DIGU 7, SPEC 8, DIGL 9, SAM 10, DRM 11. */
enum { DEMOD_LSB = 0, DEMOD_USB = 1, DEMOD_CWL = 3, DEMOD_CWU = 4, DEMOD_AM = 6,
       DEMOD_DIGU = 7, DEMOD_DIGL = 9 };
#define DEMOD_NMODES 12   /* size for mode-indexed tables (ids are sparse) */

/*
 * Set the RX audio OUTPUT sample rate (Hz) for the next demod_create(). This is
 * the WDSP RXA output + sink rate — orthogonal to the audio bandwidth (the demod
 * filter passband), which stays user-controlled. Must be a divisor of the IQ
 * `in_rate` and ≤ it (demod_create clamps otherwise). Default 48000. Call before
 * demod_create; restart-to-apply (a live channel keeps its create-time rate).
 */
void demod_set_audio_rate(int rate);

/* The audio output rate the current channel actually opened with (Hz). */
int  demod_audio_rate(void);

/*
 * Open demod channel `id` for `in_rate` Hz IQ, `mode`, passband [flo,fhi] Hz,
 * AF `volume` in dB (0..−40). The audio sink (audio_start) must already be up at
 * demod_audio_rate(). Returns 0 on success.
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

/* Noise reduction: 0 off / 1 ANR (LMS) / 2 NR2 (EMNR spectral) /
 *                  3 NR3 (RNNoise, RNN) / 4 NR4 (libspecbleach, spectral).
 * Noise blanker: 0 off / 1 ANB / 2 NB2 (SNBA). Auto-notch: on/off. */
void demod_set_nr(int mode);
void demod_set_nb(int mode);
void demod_set_anf(int on);

/* Binaural stereo audio: 0 = mono (L=R), 1 = binaural (L=I, R=Q). Live. */
void demod_set_binaural(int on);

/* Set the AF volume (dB, 0..-40) live on the running channel (thread-safe). */
void demod_set_volume(double db);

/* Mute the RX audio output (silence to the sink) — used to silence the receiver
 * while transmitting. Thread-safe; the WDSP demod keeps running underneath. */
void demod_set_mute(int on);

/*
 * TX monitor (hear your own transmission): push mono audio at src_rate from the
 * TX feed thread; it is rate-converted, mixed into the sink AFTER the RX mute
 * gain and clamped. Lock-free SPSC ring; drop-on-full; no-op before demod_create.
 * demod_set_monitor_gain sets the monitor level in dB (≤ 0).
 */
void demod_monitor_push(const float *mono, int n, int src_rate);
void demod_set_monitor_gain(double db);

/*
 * RX audio tap (TCI, F6d-2b): when set, the demod feed thread calls cb with
 * volume-compensated mono audio at a FIXED 48 kHz (decimated from the sink
 * rate), taken BEFORE the RX-on-TX mute and the TX monitor mix — digital-mode
 * decoding must not depend on the volume knob or mute state. cb runs on the
 * demod feed thread: it must be cheap and lock-free (SPSC ring push).
 */
void demod_set_audio_tap(void (*cb)(const float *mono48k, int n));

/* Set the filter passband [flo,fhi] Hz live (same mode); thread-safe. */
void demod_set_passband(double flo, double fhi);

void demod_destroy(void);

/* Diagnostics: peak |audio| since last call (and reset); last fexchange0 error. */
double demod_peak(void);
int    demod_last_error(void);

/* Signal-strength S-meter of the tuned signal (dBm), read from WDSP. */
double demod_s_meter(void);

#endif /* SDRFL_ENGINE_DEMOD_H */
