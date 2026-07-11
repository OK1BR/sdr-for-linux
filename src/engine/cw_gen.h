/*
 * sdr-for-linux — CW (Morse) envelope generator. F6d, docs/TX-DESIGN.md.
 *
 * PURE, offline, no radio and no threads. Turns text (and later raw key events)
 * into a sample-accurate keyed RF envelope in [0,1]. The whole point: timing is
 * carried in SAMPLE COUNTS, not wall-clock sleeps — the caller pulls envelope
 * samples in lockstep with the outgoing TX-IQ stream, which the radio clocks at a
 * fixed rate, so the rhythm is locked to the radio's crystal and immune to host
 * scheduling jitter at any WPM. Mirrors piHPSDR's host-generated CW path
 * (transmitter.c: I = 0.896·envelope, Q = 0); WDSP is not involved.
 *
 * ⛔ This file NEVER keys the radio — it only produces an envelope. Whether those
 * samples ever reach the exciter is the TX runtime's decision (MOX, PA, tx_gate).
 */
#ifndef SDRFL_ENGINE_CW_GEN_H
#define SDRFL_ENGINE_CW_GEN_H

typedef struct cw_gen cw_gen;

/*
 * Create a generator at `sample_rate` Hz. `wpm` (1..60), `weight` (0..100, 50 =
 * standard 3:1 dash:dot), `ramp_ms` rise/fall of the raised-cosine edge (~9 ms).
 * Returns NULL on OOM.
 */
cw_gen *cw_gen_new(int sample_rate, int wpm, double weight, double ramp_ms);
void    cw_gen_set_speed(cw_gen *g, int wpm, double weight);   /* live WPM change */
void    cw_gen_set_ramp(cw_gen *g, double ramp_ms);
void    cw_gen_free(cw_gen *g);

/*
 * Element timings for the current speed (48k-independent, in THIS gen's samples):
 * one dot. dash = 3·dot·weight/50; the caller rarely needs these — exposed for the
 * offline timing gate. */
int     cw_gen_dot_samples(const cw_gen *g);

/*
 * Queue the Morse for an ASCII string (appended to the schedule; letters, digits,
 * common punctuation and prosigns via '<..>' are supported, space = word gap).
 * Unknown characters are skipped. Thread-note: call from the same side as pull().
 */
void    cw_gen_send_text(cw_gen *g, const char *text);

/* Drop everything queued and return the envelope to rest (e.g. an abort). */
void    cw_gen_flush(cw_gen *g);

/*
 * HUD progress snapshot (display-only bookkeeping — the envelope schedule is
 * untouched). Copies the CURRENT over's text — from its first character to the
 * end of the queue; a send starting from idle begins a new over and drops the
 * previous record — into buf (NUL-terminated) and sets *cur to the index of
 * the first character that has NOT finished sounding yet (== the returned
 * length once everything queued has been sent). Returns the number of
 * characters written. Thread-note: call from the same side as pull().
 */
int     cw_gen_progress(cw_gen *g, char *buf, int buflen, int *cur);

/*
 * Pull the next `n` envelope samples [0,1] into out[]. Advances the sample clock by
 * exactly `n`. When the schedule is empty it emits silence (0). Returns the number
 * of samples in this block for which the key was DOWN (envelope target 1) — a cheap
 * "are we keying" signal for MOX/break-in logic. */
int     cw_gen_pull(cw_gen *g, float *out, int n);

/* 1 if nothing is queued AND the envelope has settled back to 0 (safe to unkey). */
int     cw_gen_idle(const cw_gen *g);

#endif /* SDRFL_ENGINE_CW_GEN_H */
