/*
 * bandplan.h — amateur band-plan reference (compiled-in, region-selectable).
 *
 * Static tables (no parser, no file IO): IARU Region 1/2/3 band edges, national
 * overrides (CZ / US), and IARU R1 HF+VHF/UHF segment designations. All values
 * are TRANSMITTED frequencies in Hz (int64) so the future out-of-band TX guard
 * can compare with no float error. The GUI stores only the *selection* (region +
 * country) in config.ini; this module owns the data.
 *
 * Reference + provenance: docs/BANDPLAN.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SDRFL_BANDPLAN_H
#define SDRFL_BANDPLAN_H

#include <stdint.h>

typedef enum { BP_R1 = 0, BP_R2 = 1, BP_R3 = 2 } bp_region_t;

/* Usage bitmask for a segment. */
enum {
  BP_CW    = 1 << 0,
  BP_NB    = 1 << 1,   /* narrow-band ≤500 Hz (CW/RTTY/PSK) */
  BP_DIGI  = 1 << 2,
  BP_PHONE = 1 << 3,
  BP_BEACON= 1 << 4,
  BP_SAT   = 1 << 5,
  BP_IMAGE = 1 << 6,
  BP_FM    = 1 << 7,
  BP_DV    = 1 << 8,
  BP_ACDS  = 1 << 9,   /* automatically-controlled (unattended) data */
  BP_EMERG = 1 << 10,
};
#define BP_ALL (BP_CW | BP_NB | BP_DIGI | BP_PHONE | BP_IMAGE | BP_FM | BP_DV)

typedef struct { const char *band; int64_t lo, hi; } bp_edge_t;
typedef struct {
  int64_t     lo, hi;    /* Hz */
  int32_t     max_bw;    /* Hz, 0 = unspecified */
  uint16_t    usage;     /* BP_* bitmask */
  const char *label;     /* human note / centre-of-activity, may be "" */
} bp_seg_t;

/* ---- Region / country selection (for the settings combos) ---------------- */

int         bp_region_count(void);
const char *bp_region_name(int r);          /* "Region 1 (EU/AF/ME)" … "" if OOR */
const char *bp_region_key(int r);           /* "R1" / "R2" / "R3" */
int         bp_region_from_key(const char *key);   /* "R1"→0; -1 if unknown */

int         bp_country_count(void);         /* index 0 = "none" */
const char *bp_country_name(int c);         /* "— none —" / "Czech Republic (OK)" / "United States" */
const char *bp_country_key(int c);          /* "" / "CZ" / "US" */
int         bp_country_from_key(const char *key);  /* ""→0; -1 if unknown */

/* ---- Lookups ------------------------------------------------------------- */

/*
 * Band edges for `region`, with `country_key` national overrides applied
 * (country_key "" or NULL = pure region plan). Writes up to `max` rows into
 * out[] and returns the number written. Bands not allocated in the region are
 * skipped. Ordered low→high.
 */
int bp_edges(bp_region_t region, const char *country_key, bp_edge_t *out, int max);

/*
 * Name of the band containing `f` (Hz) for region/country, or NULL if `f` is
 * out of every amateur band. When non-NULL, lo/hi (if given) receive the band
 * edges. Used for the on-screen band label + the future TX in-band check.
 */
const char *bp_band_for_freq(bp_region_t region, const char *country_key,
                             int64_t f, int64_t *lo, int64_t *hi);

/*
 * Segments of `band` for `region` (R1 is complete; R2/R3 currently fall back to
 * R1 HF where region-specific data isn't tabulated yet — see docs/BANDPLAN.md).
 * Returns count written into out[] (≤ max).
 */
int bp_segments(bp_region_t region, const char *band, bp_seg_t *out, int max);

/*
 * Human label for the segment `f` falls in (e.g. "All modes · 7090 SSB-QRP"),
 * or NULL if none. Static buffer, valid until the next call. For the readout.
 */
const char *bp_segment_at(bp_region_t region, const char *country_key, int64_t f);

#endif /* SDRFL_BANDPLAN_H */
