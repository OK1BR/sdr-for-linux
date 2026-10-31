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
 * CWU 4, FM 5, AM 6, DIGU 7, SPEC 8, DIGL 9, SAM 10, DRM 11.
 * DEMOD_RTTY = 12 is OURS (the first id beyond the WDSP enum, RTTY-SCOPE §7B):
 * WDSP has no FSK — at every WDSP boundary (SetRXAMode here, tx_passband/
 * SetTXAMode in tx_run) RTTY maps to DIGL; TX bypasses WDSP entirely
 * (rtty_gen direct-FSK IQ, the CW pattern). ⛔ Any code that branches on
 * DEMOD_DIGU/DEMOD_DIGL must decide whether DEMOD_RTTY belongs in the same
 * branch — sweep with `grep -ni digu` (case-insensitive: the C identifiers,
 * not just the name strings) when touching mode logic. */
enum { DEMOD_LSB = 0, DEMOD_USB = 1, DEMOD_CWL = 3, DEMOD_CWU = 4, DEMOD_AM = 6,
       DEMOD_DIGU = 7, DEMOD_DIGL = 9, DEMOD_SAM = 10, DEMOD_RTTY = 12 };
/* DEMOD_SAM = WDSP RXA_SAM (issue #30): the same AMD demodulator as AM run as a
 * PLL-locked synchronous detector (carrier capture ±2 kHz about the dial,
 * amd.c create_amd fmin/fmax — no setter), with an optional single-sideband
 * output that is NOT a filter (sideband separation by phasing). TX in SAM is
 * AM: WDSP TXA_SAM runs the AM modulator (TXA.c SetTXAMode), piHPSDR
 * transmitter.c groups modeSAM with modeAM for the TX passband too. */
#define DEMOD_NMODES 13   /* size for mode-indexed tables (ids are sparse) */

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
 *                  3 NNR (WDSP neural — the former RNNoise slot, issue #22) /
 *                  4 NR4 (libspecbleach, spectral).
 * Noise blanker: 0 off / 1 ANB / 2 NB2 (SNBA). Auto-notch: on/off. */
void demod_set_nr(int mode);
/* NNR parameters (live, thread-safe): model 0 Standard / 1 Premium (the CW
 * variants are picked automatically in CWU/CWL), mask floor in dB (−50..−10;
 * WDSP default −25). Cached before the channel exists. */
void demod_set_nnr(int model, double floor_db);
void demod_set_nb(int mode);
void demod_set_anf(int on);

/* AM/SAM demodulator options (WDSP Guide §5.3.8; live, thread-safe, cached
 * before the channel exists and re-applied after every mode change):
 *  - SAM sideband: 0 both / 1 LSB only / 2 USB only (SetRXAAMDSBMode — SAM
 *    only; a phasing selector, the station need not be centred);
 *  - carrier levelling ("fade leveler", SetRXAAMDFadeLevel): the fading
 *    carrier is stripped and a steady one inserted — AM and SAM; WDSP's
 *    default is ON and so was ours before this knob existed. */
void demod_set_sam_sideband(int sb);
void demod_set_am_fade(int on);

/* Binaural stereo audio: 0 = mono (L=R), 1 = binaural (L=I, R=Q). Live. */
void demod_set_binaural(int on);

/* Set the AF volume (dB, 0..-40) live on the running channel (thread-safe).
 * At the range floor (≤ −40 dB, e.g. the GUI slider minimum or TCI MUTE's −60)
 * the sink output is hard-muted (true silence, click-free ramp) — the panel
 * gain alone would still leave AGC-levelled audio faintly audible. The 48 kHz
 * TCI tap keeps flowing regardless (decoding is volume-independent). */
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

/* Monitor content type: 1 = absolute (CW sidetone — the producer's own trim
 * is the final level, the monitor gain is skipped; piHPSDR's sidetone volume
 * is likewise independent), 0 = voice (scaled by the monitor gain). The TX
 * feed thread sets this alongside each push. */
void demod_monitor_absolute(int on);

/*
 * RX audio tap (TCI, F6d-2b): when set, the demod feed thread calls cb with
 * volume-compensated mono audio at a FIXED 48 kHz (decimated from the sink
 * rate), taken BEFORE the RX-on-TX mute and the TX monitor mix — digital-mode
 * decoding must not depend on the volume knob or mute state. cb runs on the
 * demod feed thread: it must be cheap and lock-free (SPSC ring push).
 */
void demod_set_audio_tap(void (*cb)(const float *mono48k, int n));

/* Set the filter passband [flo,fhi] Hz live (same mode); thread-safe.
 * Passbands are GUI-space (symmetric around the dial); in CW modes the demod
 * shifts them (and the spectrum) by the sidetone pitch internally — the dial
 * reads the carrier and a spot-on station is heard at the pitch. */
void demod_set_passband(double flo, double fhi);

/* CW sidetone pitch = the CW BFO offset, Hz (live; thread-safe). */
void demod_set_cw_pitch(int hz);

/* CTUN: the RX dial's offset from the DDC centre, Hz (live; thread-safe).
 * Applied in the RXA shifter like piHPSDR's rx_set_offset, so the TCI IQ
 * stream stays the un-shifted DDC; 0 = Model A (dial == centre). */
void demod_set_ctun_offset(double hz);

/* RTTY audio pitch = the FSK pair centre in audio, Hz (live; thread-safe).
 * The dial reads the pair CENTRE (what the skimmer spots); the LSB-side
 * mapping puts mark/space at pitch∓85 — default 800 = 715/885; 2210 is the
 * classic 2125/2295 pair (RTTY-SCOPE §7A). */
void demod_set_rtty_pitch(int hz);

void demod_destroy(void);

/* Diagnostics: peak |audio| since last call (and reset); last fexchange0 error. */
double demod_peak(void);
int    demod_last_error(void);

/* Signal-strength S-meter of the tuned signal (dBm), read from WDSP. */
double demod_s_meter(void);

/* AGC meters (dB), display only: `gain` = the gain the AGC is applying right
 * now (piHPSDR's "Gain" reading = -RXA_AGC_GAIN), `out` = RXA_AGC_AV, the
 * levelled output. -200 when the channel is not up. */
void   demod_agc_meters(double *gain_db, double *out_db);

#endif /* SDRFL_ENGINE_DEMOD_H */
