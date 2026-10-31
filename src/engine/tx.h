/*
 * sdr-for-linux — WDSP TX DSP channel (headless, GLib-only). F2, docs/TX-DESIGN.md.
 *
 * Builds the exciter chain: mic 48 kHz mono -> WDSP TXA (mode + bandpass + ALC)
 * -> 192 kHz IQ at the DUC rate, delivered to a callback. This is PURE DSP: no
 * MOX, no PA, no keying — it merely turns audio into the IQ the radio's DUC would
 * up-convert. Mirrors piHPSDR transmitter.c @974acba (OpenChannel :1307, the
 * fixed SetTXA* config :1323-1356, fexchange0 :1665).
 *
 * ⛔ The live engine does NOT run this until the F5 keying milestone; in F1-F4 it
 * exists only for the offline sdrfl-txdsp-test gate. Producing IQ here cannot key
 * the radio (that needs the MOX bit + PA, which stay off — docs/TX-SAFETY.md).
 */
#ifndef SDRFL_ENGINE_TX_H
#define SDRFL_ENGINE_TX_H

/*
 * TX IQ callback: interleaved I/Q doubles [I0,Q0,I1,Q1,...], `n_pairs` pairs, at
 * the DUC output rate (192 kHz on P2, 48 kHz on P1). Called from whatever thread
 * feeds the mic.
 */
#include "eq.h"   /* EqProfile — the TX equalizer's band table (issue #48) */

typedef void (*tx_iq_cb)(const double *iq, int n_pairs, void *user);

/*
 * Create the TX channel (WDSP id chosen internally, distinct from the RX channel).
 * `mode` is a WDSP mode (DEMOD_USB=1, DEMOD_LSB=0, …); flo/fhi is the TX passband
 * in Hz (e.g. 150/2850). `p1` selects the protocol's DUC rate chain (piHPSDR
 * transmitter.c:987-1010): P2 = 512 in / 96 k dsp / 192 k out with the P2
 * compensating CFIR; P1 = 1024 in / 48 k dsp / 48 k out, ⛔ CFIR OFF
 * (transmitter.c:1325 — the CFIR compensates the P2 firmware's interpolation
 * filter; on P1 it would distort the TX spectrum). Decoded IQ is delivered to
 * `cb`/`user`. The channel is created STOPPED — call tx_dsp_run(1) before
 * feeding. Returns 0 on success.
 */
int tx_dsp_create(int mode, double flo, double fhi, int p1, tx_iq_cb cb, void *user);

/* Start (1) / stop (0) the WDSP TX channel (SetChannelState). Off after create. */
void tx_dsp_run(int on);

/*
 * Feed `n` mono mic samples (floats ~[-1,1]) at 48 kHz. Accumulates to the WDSP
 * block size (512), runs fexchange0, and delivers each 192 kHz IQ block to the
 * callback. A no-op until tx_dsp_create() has run.
 */
void tx_dsp_feed_mic(const float *mic, int n);

void tx_dsp_set_mode(int mode, double flo, double fhi);
void tx_dsp_set_mic_gain(double db);        /* mic gain in dB (SetTXAPanelGain1) */

/*
 * Speech processor (PROC) — piHPSDR tx_set_compressor @974acba: the WDSP COMP
 * compressor at `gain_db` (0-20 dB; 0 dB still useful) plus, whenever on, the
 * auto-LEVELER (attack 1 / decay 500 / top 8 dB) and, above 5.5 dB, the CESSB
 * overshoot control. This is the ONLY makeup gain in the TX chain: without it
 * voice PEP is exactly (mic peak × mic gain)² — the ALC only attenuates.
 */
void tx_dsp_set_compressor(int on, double gain_db);

/*
 * Mic noise gate (downward expander) — WDSP TXA AMSQ, positioned after the mic
 * gain and BEFORE the leveler/COMP, so PROC has nothing to pump up in speech
 * gaps. thresh_db is on the post-mic-gain signal (dBFS); below it the mic drops
 * by the gate depth (default 10 dB) — and tx_run additionally hands the radio
 * EXACT ZEROS once the mic has stayed below the threshold for hold + release
 * (RF_MUTE_AFTER_US), because the G2E puts ~1.5 W on the air for any non-zero
 * DUC data (2026-10-24).
 */
/* thresh_db is in Mic-METER dBFS (post-mic-gain — what the TX HUD bar shows);
 * the engine rescales the DEXP trigger by the current mic gain internally. */
void tx_dsp_set_gate(int on, double thresh_db);
/* Below-threshold attenuation in dB (piHPSDR fixed 20; our default 10). */
void tx_dsp_set_gate_depth(double depth_db);

/*
 * TX phase rotator — WDSP TXA PHROT (issue #23): a cascade of identical
 * first-order all-pass stages right after the mic gain (TXA.c xtxa: panel →
 * phrot → mic meter → … → leveler → COMP → ALC). Flat amplitude response, so
 * the spectrum does not change; the differential phase across the audio band
 * makes the speech waveform symmetric → lower peak-to-average → the ALC can
 * run a higher average level at the same peak limit (WDSP Guide 2.1.1
 * §6.3.10 / §12.2). `corner_hz` = the 90°-per-stage frequency, 50–2000
 * (WDSP clamps); `automode` hands the corner to WDSP's A/B optimizer, which
 * steps every 15 s of fed audio and locks once its step falls to 5 Hz.
 * ⛔ The PHROT setters are NOT no-ops on unchanged values: Corner and Nstages
 * free + re-allocate the all-pass history, Run(1) zeroes the asymmetry
 * envelopes, Corner also restarts a running search. tx_run re-pushes every
 * slot (~20 Hz), so this applies ON CHANGE ONLY — the gate_apply idiom.
 */
/* TX parametric pre-EQ (issue #48): the WDSP TXA equalizer between the gate
 * and the leveler, driven by the band model in eq.h. `on` is the CALLER's
 * decision — tx_run passes tx_voice_proc_active() (voice mode + live mic +
 * MOX, never TUNE / two-tone / CW / RTTY / digi / TCI audio: the clean-chain
 * rule). The table is cached, the WDSP profile pushed on change only (every
 * push rebuilds a 16384-tap impulse), re-applied when the channel is created. */
void tx_dsp_set_eq(int on, const EqProfile *p);
int  tx_dsp_eq_active(void);    /* the TXA EQ runs right now (last push) */
long tx_dsp_eq_pushes(void);    /* WDSP profile pushes so far (gate tripwire) */

void tx_dsp_set_phrot(int on, double corner_hz, int automode);
/* Restart the optimizer search from the 338 Hz default (SetTXAPHROTAutoReset). */
void tx_dsp_phrot_reset(void);
/*
 * Rotator readout (GetTXAPHROTAsymmetry): `run` = the rotator is switched on;
 * `in_ratio` / `out_ratio` = waveform asymmetry |pos−neg|/(pos+neg) before /
 * after it (0..1, a 30 s EMA inside WDSP, zeroed at every channel stop);
 * `fc` = the corner in use (moves under the optimizer); `auto_step` 0 =
 * optimizer off, −1 = converged (locked), else the current search step in Hz.
 * NULL args are skipped; all zero before tx_dsp_create.
 */
void tx_dsp_get_phrot(int *run, double *in_ratio, double *out_ratio,
                      double *fc, double *auto_step);

/*
 * The WDSP TX channel's mic input rate (Hz). The mic capture must deliver samples
 * at exactly this rate — feed_mic → fexchange0 assumes it. Exposed so the GUI can
 * open the PipeWire capture at the right rate without hard-coding 48000.
 */
int  tx_dsp_in_rate(void);

/* WDSP TXA channel id (-1 before tx_dsp_create) — PureSignal targets it. */
int  tx_dsp_channel(void);

/*
 * Live WDSP TX level meters, in dB (GetTXAMeter, like piHPSDR): `mic_pk_db` is the
 * mic-input peak in dBFS (≤ 0; -99 floor; measured before the noise gate),
 * `alc_gain_db` the ALC gain reduction in dB (0 = none, negative = clamping),
 * `lvlr_gain_db` the leveler makeup gain in dB (0..+8; only moves with PROC on).
 * Only meaningful while the channel runs; all fall to the floor (-99 / 0 / 0)
 * when not ready. NULL args are skipped.
 */
void tx_dsp_get_meters(double *mic_pk_db, double *alc_gain_db, double *lvlr_gain_db);

/*
 * TUNE tone via the WDSP post generator (transmitter.c:2872). on=1 injects a
 * full-scale single carrier at `offset_hz` from the dial (0 = carrier at dial);
 * on=0 stops it. Used at F5 for TUNE; the tone power is set by the drive byte,
 * not this magnitude.
 */
void tx_dsp_tune_tone(int on, double offset_hz);

/* Two-tone test generator (PS calibration / IMD check): tones at f1/f2 Hz
 * audio offset, each ~half amplitude (envelope peaks at full scale). The
 * PostGen replaces the chain signal while running. Negate f1/f2 for LSB. */
void tx_dsp_two_tone(int on, double f1, double f2);

int  tx_dsp_last_error(void);               /* last non-zero fexchange0 error */
void tx_dsp_destroy(void);

#endif /* SDRFL_ENGINE_TX_H */
