/*
 * sdr-for-linux — parametric equalizer model (issue #48). See eq.h.
 *
 * Band shapes = the RBJ Audio-EQ-Cookbook ANALOG prototypes (Robert
 * Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter
 * coefficients"), evaluated as |H(jω)| with ω = f / fc — the target magnitude
 * the WDSP FIR then realizes. The analog form is deliberate: a digital biquad's
 * response warps toward Nyquist, the FIR has no such constraint, and the
 * cookbook's own definitions of gain and Q are stated on the analog prototype.
 *
 *   peaking:    H(s) = (s² + s·A/Q + 1) / (s² + s/(A·Q) + 1)
 *   low shelf:  H(s) = A · (s² + s·√A/Q + A) / (A·s² + s·√A/Q + 1)
 *   high shelf: H(s) = A · (A·s² + s·√A/Q + 1) / (s² + s·√A/Q + A)
 *   with A = 10^(dB/40): the peak reaches exactly dB at fc, a shelf reaches
 *   dB on its plateau and dB/2 at fc.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "eq.h"

static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* |N(jω)|² / |D(jω)|² for N = n2 s² + n1 s + n0, D likewise, s = jω. */
static double biquad_mag2(double w, double n2, double n1, double n0,
                          double d2, double d1, double d0) {
  double w2 = w * w;
  double nr = n0 - n2 * w2, ni = n1 * w;
  double dr = d0 - d2 * w2, di = d1 * w;
  double den = dr * dr + di * di;
  if (den < 1e-300) { den = 1e-300; }
  return (nr * nr + ni * ni) / den;
}

double eq_band_db(const EqBand *b, double f) {
  if (!b || b->f <= 0.0 || b->g == 0.0) { return 0.0; }
  double A = pow(10.0, b->g / 40.0);
  double q = b->q > 0.0 ? b->q : EQ_Q_DFLT_PEAK;
  double w = f / b->f;
  double m2, lead = 1.0;
  switch (b->type) {
    case EQ_LOW_SHELF:
      lead = A;
      m2 = biquad_mag2(w, 1.0, sqrt(A) / q, A,   A, sqrt(A) / q, 1.0);
      break;
    case EQ_HIGH_SHELF:
      lead = A;
      m2 = biquad_mag2(w, A, sqrt(A) / q, 1.0,   1.0, sqrt(A) / q, A);
      break;
    default:   /* EQ_PEAK */
      m2 = biquad_mag2(w, 1.0, A / q, 1.0,   1.0, 1.0 / (A * q), 1.0);
      break;
  }
  return 10.0 * log10(m2 * lead * lead + 1e-300);
}

double eq_profile_db(const EqProfile *p, double f) {
  double s = 0.0;
  if (!p) { return 0.0; }
  for (int i = 0; i < p->n && i < EQ_MAX_BANDS; i++) { s += eq_band_db(&p->b[i], f); }
  return s;
}

void eq_band_clamp(EqBand *b) {
  if (b->type < 0 || b->type >= EQ_NTYPES) { b->type = EQ_PEAK; }
  if (!(b->f == b->f)) { b->f = 1000.0; }          /* NaN guard */
  if (!(b->g == b->g)) { b->g = 0.0; }
  if (!(b->q == b->q)) { b->q = b->type == EQ_PEAK ? EQ_Q_DFLT_PEAK : EQ_Q_DFLT_SHELF; }
  b->f = clampd(b->f, EQ_F_MIN, EQ_F_MAX);
  b->g = clampd(b->g, -EQ_GAIN_MAX, EQ_GAIN_MAX);
  b->q = clampd(b->q, EQ_Q_MIN, EQ_Q_MAX);
}

const char *eq_type_name(int type) {
  switch (type) {
    case EQ_LOW_SHELF:  return "Low shelf";
    case EQ_HIGH_SHELF: return "High shelf";
    default:            return "Peak";
  }
}

/* ---- the WDSP grid -------------------------------------------------------- */

#define GRID_BASE   32      /* log-spaced seed points, EQ_F_MIN .. Nyquist      */
#define GRID_TOL    0.01    /* dB: stop refining when every interval is this close */

static int cmp_d(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

/* Largest |linear interpolation − curve| inside [f0, f1], probed at 1/4, 1/2
 * and 3/4 (three probes so a bell narrower than the interval cannot hide
 * between the endpoints), and where it is. */
static double interval_err(const EqProfile *p, double f0, double g0, double f1, double g1, double *at) {
  double worst = -1.0;
  for (int k = 1; k <= 3; k++) {
    double t = 0.25 * k, f = f0 + t * (f1 - f0);
    double e = fabs(g0 + t * (g1 - g0) - eq_profile_db(p, f));
    if (e > worst) { worst = e; *at = f; }
  }
  return worst;
}

int eq_profile_grid(const EqProfile *p, double fs, double *F, double *G) {
  double nyq = fs > 0.0 ? fs / 2.0 : 24000.0;
  double seed[GRID_BASE + 5 * EQ_MAX_BANDS];
  int ns = 0;
  /* seeds: a log grid plus every band's centre and its ±1/±2 half-bandwidth
   * points (peak: half-gain bandwidth from Q; shelf: the knee ±1/Q octaves),
   * so no band can fall between the probes of the refinement below. */
  double lmin = log(EQ_F_MIN), lmax = log(nyq);
  for (int i = 0; i < GRID_BASE; i++) {
    seed[ns++] = exp(lmin + (lmax - lmin) * (double)i / (double)(GRID_BASE - 1));
  }
  if (p) {
    for (int i = 0; i < p->n && i < EQ_MAX_BANDS; i++) {
      const EqBand *b = &p->b[i];
      if (b->f <= 0.0) { continue; }
      double q = b->q > 0.0 ? b->q : EQ_Q_DFLT_PEAK;
      double h = (b->type == EQ_PEAK) ? asinh(1.0 / (2.0 * q)) / M_LN2 : 1.0 / q;   /* half-span, octaves */
      static const double k[5] = { -2.0, -1.0, 0.0, 1.0, 2.0 };
      for (int j = 0; j < 5; j++) {
        double f = b->f * pow(2.0, k[j] * h);
        if (f > 1.0 && f < nyq) { seed[ns++] = f; }
      }
    }
  }
  qsort(seed, (size_t)ns, sizeof(double), cmp_d);
  int n = 0;
  F[++n] = 0.0;
  for (int i = 0; i < ns; i++) {
    double f = seed[i];
    if (f <= 0.0 || f >= nyq) { continue; }
    if (F[n] > 0.0 && f < F[n] * 1.001) { continue; }
    F[++n] = f;
  }
  F[++n] = nyq;
  for (int i = 1; i <= n; i++) { G[i] = eq_profile_db(p, F[i]); }
  /* refine: split the interval with the largest interpolation error until
   * all are within GRID_TOL or the WDSP cap is reached. The first interval
   * (0 Hz .. 20 Hz) is left alone: nothing audible lives there. Interval
   * errors are cached (err[i] belongs to [F[i], F[i+1]]) and only the two
   * intervals a split creates are re-probed — this runs under the RX/TX
   * chain lock at every drag step, and the rescan-everything version cost
   * 7.5 ms for eight bands (sdrfl-eq-test reports the number). */
  double err[EQ_MAX_POINTS + 1], at[EQ_MAX_POINTS + 1];
  err[1] = -1.0; at[1] = 0.0;
  for (int i = 2; i < n; i++) { err[i] = interval_err(p, F[i], G[i], F[i + 1], G[i + 1], &at[i]); }
  while (n < EQ_MAX_POINTS) {
    int wi = -1; double worst = -1.0;
    for (int i = 2; i < n; i++) { if (err[i] > worst) { worst = err[i]; wi = i; } }
    if (wi < 0 || worst <= GRID_TOL) { break; }
    double f = at[wi];
    if (f <= F[wi] || f >= F[wi + 1]) { f = 0.5 * (F[wi] + F[wi + 1]); }
    memmove(&F[wi + 2],   &F[wi + 1],   (size_t)(n - wi) * sizeof F[0]);
    memmove(&G[wi + 2],   &G[wi + 1],   (size_t)(n - wi) * sizeof G[0]);
    memmove(&err[wi + 2], &err[wi + 1], (size_t)(n - wi) * sizeof err[0]);
    memmove(&at[wi + 2],  &at[wi + 1],  (size_t)(n - wi) * sizeof at[0]);
    F[wi + 1] = f;
    G[wi + 1] = eq_profile_db(p, f);
    n++;
    err[wi]     = interval_err(p, F[wi],     G[wi],     F[wi + 1], G[wi + 1], &at[wi]);
    err[wi + 1] = interval_err(p, F[wi + 1], G[wi + 1], F[wi + 2], G[wi + 2], &at[wi + 1]);
  }
  F[0] = 0.0;
  G[0] = 0.0;                                 /* preamp: level is made elsewhere */
  return n;
}

/* ---- persistence ---------------------------------------------------------- */

int eq_profile_format(const EqProfile *p, char *buf, size_t len) {
  if (!buf || !len) { return 0; }
  buf[0] = '\0';
  if (!p) { return 0; }
  size_t used = 0;
  for (int i = 0; i < p->n && i < EQ_MAX_BANDS; i++) {
    char fb[G_ASCII_DTOSTR_BUF_SIZE], gb[G_ASCII_DTOSTR_BUF_SIZE], qb[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(fb, sizeof fb, "%.1f", p->b[i].f);
    g_ascii_formatd(gb, sizeof gb, "%.2f", p->b[i].g);
    g_ascii_formatd(qb, sizeof qb, "%.3f", p->b[i].q);
    int w = snprintf(buf + used, len - used, "%d:%s:%s:%s;", p->b[i].type, fb, gb, qb);
    if (w < 0 || (size_t)w >= len - used) { buf[used] = '\0'; break; }   /* whole tokens only */
    used += (size_t)w;
  }
  return (int)used;
}

int eq_profile_parse(const char *s, EqProfile *p) {
  if (!p) { return 0; }
  memset(p, 0, sizeof *p);
  if (!s || !*s) { return 0; }
  char *buf = g_strdup(s), *sv = NULL;
  for (char *tok = strtok_r(buf, ";", &sv); tok && p->n < EQ_MAX_BANDS; tok = strtok_r(NULL, ";", &sv)) {
    char **f = g_strsplit(tok, ":", 4);
    int nf = 0; while (f[nf]) { nf++; }
    if (nf == 4) {
      char *e1 = NULL, *e2 = NULL, *e3 = NULL;
      EqBand b;
      b.type = atoi(f[0]);
      b.f = g_ascii_strtod(f[1], &e1);
      b.g = g_ascii_strtod(f[2], &e2);
      b.q = g_ascii_strtod(f[3], &e3);
      if (e1 != f[1] && e2 != f[2] && e3 != f[3]) {
        eq_band_clamp(&b);
        p->b[p->n++] = b;
      }
    }
    g_strfreev(f);
  }
  g_free(buf);
  return p->n;
}

int eq_profile_equal(const EqProfile *a, const EqProfile *b) {
  if (a->n != b->n) { return 0; }
  for (int i = 0; i < a->n; i++) {
    if (a->b[i].type != b->b[i].type || a->b[i].f != b->b[i].f ||
        a->b[i].g != b->b[i].g || a->b[i].q != b->b[i].q) { return 0; }
  }
  return 1;
}
