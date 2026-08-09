/*
 * wisdom_gate.h — first-run FFTW wisdom builder.
 *
 * WDSP plans its FFTs with FFTW_PATIENT; for the deep-zoom analyzer sizes (up
 * to 262144) that costs tens of seconds to PLAN — which was the whole "zoom
 * stutter" (FFTW re-planning, not computing). WDSP ships the cure (WDSPwisdom):
 * build the plans once, cache to disk, import instantly ever after — exactly
 * what piHPSDR/Thetis do. We just call it; nothing in vendor/wdsp is modified.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SDRFL_WISDOM_GATE_H
#define SDRFL_WISDOM_GATE_H

/* Ensure FFTW wisdom is loaded into this process. On every start this imports
 * the on-disk cache (fast). On the very first run, when no cache exists, it
 * builds the plans while showing a small progress window, then returns once the
 * cache is written. Call once, before analyzer_create()/demod_create(). */
void wisdom_ensure(void);

/* Resolved cache directory (~/.config/sdr-for-linux/wisdom/<fftw-build>, or
 * the SDRFL_WISDOM_DIR override) — for the About debug info. Caller frees. */
char *wisdom_cache_dir(void);

/* FFTW's own build string (e.g. "fftw-3.3.11-sse2-avx2") — the wisdom cache
 * is only readable by the exact build that wrote it, so bug reports need it. */
const char *wisdom_fftw_version(void);

#endif /* SDRFL_WISDOM_GATE_H */
