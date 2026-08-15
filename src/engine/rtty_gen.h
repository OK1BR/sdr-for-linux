/*
 * sdr-for-linux — RTTY (Baudot FSK) IQ generator. docs/RTTY-SCOPE.md.
 *
 * PURE, offline, no radio and no threads — the cw_gen contract verbatim. Turns
 * text into a unit-amplitude, phase-continuous FSK IQ stream at complex
 * baseband: ITA2 (5-bit, LSB transmitted first, automatic FIGS/LTRS with
 * unshift-on-space — the exact table skimmer-for-linux decodes) framed
 * 1 start + 5 data + 1.5 stop at 45.45 Bd, mark = +85 Hz / space = −85 Hz
 * around 0 (mark = higher RF; the dial carries the pair CENTRE). Timing is
 * carried in SAMPLE COUNTS — the caller pulls IQ in lockstep with the
 * radio-clocked TX stream, so the baud rate is locked to the radio's crystal.
 * 45.45 Bd / 170 Hz are constants by decision (RTTY-SCOPE §7E), not knobs.
 *
 * Envelope: raised-cosine ramp at key-on/key-off only — between them the
 * amplitude is constant 1.0 (FSK keys frequency, never amplitude; an
 * amplitude step or a phase jump is a key click). A send starting from idle
 * prepends a steady-mark preamble (receivers sync on idle mark) + one LTRS
 * character (parks the receiver's shift state); the schedule ends with a
 * ~1-bit mark tail before the envelope ramps down.
 *
 * ⛔ This file NEVER keys the radio — it only produces IQ. Whether those
 * samples ever reach the exciter is the TX runtime's decision (MOX, PA,
 * tx_gate), exactly as with cw_gen.
 */
#ifndef SDRFL_ENGINE_RTTY_GEN_H
#define SDRFL_ENGINE_RTTY_GEN_H

typedef struct rtty_gen rtty_gen;

/* Create a generator at `sample_rate` Hz (the TX IQ rate). NULL on OOM. */
rtty_gen *rtty_gen_new(int sample_rate);
void      rtty_gen_free(rtty_gen *g);

/*
 * Pure ITA2 encoder step, exposed for the offline gate: append the codes for
 * ASCII `c` to out[] (0..2 entries — an implicit FIGS/LTRS shift char may
 * precede the character code) given the current shift state (*figs, 0 = LTRS),
 * which is updated (space resets it to LTRS — unshift-on-space). Returns the
 * number of codes written; 0 = unmappable character (skipped). Codes are
 * 5-bit values whose LSB is the FIRST transmitted bit.
 */
int rtty_encode_char(char c, int *figs, unsigned char out[2]);

/*
 * Queue `text` (appended to the schedule). Unknown characters are skipped.
 * A send starting from IDLE skips leading whitespace (the word gap already
 * elapsed as real silence — the cw_gen_send_text rule, TX-DESIGN §10) and
 * begins a new over: preamble + LTRS + a fresh HUD record. Thread-note: call
 * from the same side as pull() (tx_run serializes under its lock).
 */
void rtty_gen_send_text(rtty_gen *g, const char *text);

/* Drop everything queued; the envelope ramps down within a ramp length (an
 * abort must cut inside one IQ block — no key click, RTTY-SCOPE §0). */
void rtty_gen_flush(rtty_gen *g);

/*
 * HUD progress snapshot (cw_gen_progress contract): the CURRENT over's text
 * around the playhead; a send from idle starts a new over. Auto-inserted
 * shift characters and the preamble are invisible (they extend the preceding
 * char's schedule instead). Returns chars written; *cur = first char not yet
 * fully sent.
 */
int rtty_gen_progress(rtty_gen *g, char *buf, int buflen, int *cur);

/*
 * Pull the next `n_pairs` IQ pairs (interleaved I,Q doubles, unit amplitude)
 * into iq[]. Advances the sample clock by exactly n_pairs; emits zeros when
 * idle. Returns the number of samples for which the envelope target was 1
 * (the "are we sending" signal, like cw_gen_pull's keydown count).
 */
int rtty_gen_pull(rtty_gen *g, double *iq, int n_pairs);

/* 1 if nothing is queued AND the envelope has settled to 0 (safe to unkey). */
int rtty_gen_idle(const rtty_gen *g);

#endif /* SDRFL_ENGINE_RTTY_GEN_H */
