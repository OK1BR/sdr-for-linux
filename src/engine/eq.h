/*
 * sdr-for-linux — parametric equalizer model (issue #48). Headless, pure.
 *
 * The band model (frequency / gain / Q; peaking, low shelf, high shelf) lives
 * HERE, not in WDSP: the TAPR WDSP 2.11 equalizer is a "continuous-gain" FIR
 * whose response is a profile of up to 256 frequency/gain points joined
 * linearly (SetRXAEQProfile / SetTXAEQProfile, Guide §5.3.14 / §6.3.6) — it
 * has no notion of Q (Thetis got one only by patching its own eq.c copy). So
 * the app evaluates the analytic magnitude of the bands (RBJ Audio-EQ-Cookbook
 * analog prototypes, summed in dB) on a dense grid and hands WDSP that grid as
 * a plain profile; WDSP's only job is the 16384-tap minimum-phase FIR. Real Q,
 * vendor/wdsp untouched.
 *
 * Grid rules (eq_profile_grid): a log-spaced base from EQ_F_MIN to the chain's
 * Nyquist plus extra points across every band's own bandwidth, sorted and
 * de-duplicated, at most EQ_MAX_POINTS (= WDSP EQ_MAXIMUM_CONTROL_POINTS); the
 * first point is 0 Hz and the last the Nyquist frequency, both with the curve's
 * own asymptotic value, so WDSP's 4th-power roll-off outside the first/last
 * point (ctfmode 0 — the only one coded) never engages. WDSP rebuilds the
 * impulse only when the profile passes checkSplineInputs against the NURBS it
 * created with degree 3, i.e. with at least 4 points — eq_profile_grid always
 * returns more. The caller (demod.c / tx.c) pushes a grid only when it changed:
 * every push recomputes the impulse and swaps the fircore.
 */
#ifndef SDRFL_ENGINE_EQ_H
#define SDRFL_ENGINE_EQ_H

#include <stddef.h>

#define EQ_MAX_BANDS   8
#define EQ_MAX_POINTS  256        /* WDSP EQ_MAXIMUM_CONTROL_POINTS (eq.h) */
#define EQ_F_MIN       20.0       /* band centre range, Hz */
#define EQ_F_MAX       10000.0
#define EQ_GAIN_MAX    15.0       /* ±dB */
#define EQ_Q_MIN       0.3
#define EQ_Q_MAX       10.0
#define EQ_Q_DFLT_PEAK  1.4
#define EQ_Q_DFLT_SHELF 0.71      /* Butterworth shelf: no overshoot */

enum { EQ_PEAK = 0, EQ_LOW_SHELF = 1, EQ_HIGH_SHELF = 2, EQ_NTYPES = 3 };

typedef struct {
  int    type;   /* EQ_PEAK / EQ_LOW_SHELF / EQ_HIGH_SHELF */
  double f;      /* centre (peak) or corner (shelf: the half-gain point), Hz */
  double g;      /* gain, dB (peak height / shelf plateau) */
  double q;      /* peak: Q = fc / bandwidth at half gain; shelf: resonance
                    (0.71 = flat Butterworth knee, more = a bump at the knee) */
} EqBand;

typedef struct {
  int    n;                 /* bands in use, 0..EQ_MAX_BANDS */
  EqBand b[EQ_MAX_BANDS];
} EqProfile;

/* Analytic magnitude, dB, of one band / of the whole profile at f Hz. */
double eq_band_db(const EqBand *b, double f);
double eq_profile_db(const EqProfile *p, double f);

/* Clamp a band into the legal ranges (type, f, g, q). */
void   eq_band_clamp(EqBand *b);
const char *eq_type_name(int type);   /* "Peak" / "Low shelf" / "High shelf" */

/* Build the WDSP profile for a chain running at `fs` Hz: F[1..n], G[1..n] in
 * Hz / dB, G[0] = 0 (preamp), F[0] unused. F and G hold EQ_MAX_POINTS + 1
 * doubles. Returns n (4 ≤ n ≤ EQ_MAX_POINTS); an empty profile yields a flat
 * (0 dB) grid. */
int    eq_profile_grid(const EqProfile *p, double fs, double *F, double *G);

/* Persistence: "type:f:g:q;type:f:g:q;…" — locale-independent ('.' decimal
 * point). format returns the length written (0 for an empty profile); parse
 * accepts NULL/"" as empty, clamps every band, drops malformed tokens, and
 * returns the number of bands read. */
int    eq_profile_format(const EqProfile *p, char *buf, size_t len);
int    eq_profile_parse(const char *s, EqProfile *p);
int    eq_profile_equal(const EqProfile *a, const EqProfile *b);

#endif /* SDRFL_ENGINE_EQ_H */
