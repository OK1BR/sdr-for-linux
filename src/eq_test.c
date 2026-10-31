/*
 * sdrfl-eq-test — OFFLINE equalizer gate (issue #48). NO radio, NO audio device.
 *
 *  1. [model]   the band shapes (RBJ analog prototypes): exact gain at the
 *               centre, log-frequency symmetry and the half-gain bandwidth of
 *               a peak, the plateau / half-gain knee / no-overshoot of a shelf,
 *               summation, clamping.
 *  2. [grid]    the WDSP profile eq_profile_grid builds: 0 Hz first, Nyquist
 *               last, strictly increasing, ≥ 4 and ≤ 256 points, flat for an
 *               empty profile, and dense enough that WDSP's linear
 *               interpolation between the points stays within 0.1 dB of the
 *               analytic curve — also for a Q 10 bell.
 *  3. [persist] format/parse round trip, malformed tokens, clamping, and a
 *               '.' decimal point under a comma locale.
 *  4. [policy]  tx_voice_proc_active — the one predicate the rotator and the
 *               EQ share — and its table.
 *  5. [RXA]     the REAL RX path (demod.c: OpenChannel like the app, AGC off):
 *               a multi-tone IQ feed, the per-tone level with the EQ off vs on
 *               at 48 / 96 / 192 kHz AF must follow the analytic curve
 *               (±0.5 dB at band centres, ±0.2 dB where no band acts); the
 *               apply-on-change tripwire (N identical pushes = one WDSP push);
 *               the digi bypass and the NNR-CW "not computed" state.
 *  6. [TXA]     the REAL TX path (tx.c at 96 kHz dsp): a multi-tone mic feed,
 *               the per-tone IQ level with the EQ off vs on follows the curve;
 *               the tripwire; the run flag follows `on`.
 *
 * SDRFL_WISDOM_DIR=<dir with trailing />: import the FFTW wisdom cache first
 * (the app's ~/.config/sdr-for-linux/), else the first OpenChannel at each
 * rate pays FFTW_PATIENT planning (tens of seconds). Exit 0 = pass.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <glib.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wdsp.h"
#include "eq.h"
#include "demod.h"
#include "tx.h"
#include "tx_run.h"

/* demod.c hands its sink-rate output to audio_push (audio_pw.c, PipeWire) —
 * no sink here: the 48 k TCI tap is the measuring point. */
void audio_push(const float *samples, int frames) { (void)samples; (void)frames; }

static int g_checks, g_fail;
static void ok(const char *what, int cond, const char *detail) {
  g_checks++;
  if (cond) { printf("  ok    %-48s %s\n", what, detail ? detail : ""); }
  else      { printf("  FAIL  %-48s %s\n", what, detail ? detail : ""); g_fail++; }
}
static char det[256];

/* Goertzel power of tone f over n mono samples at `rate` (n = whole cycles). */
static double goertzel_f(const float *x, int n, double f, int rate) {
  double w = 2.0 * M_PI * f / rate, c = 2.0 * cos(w), s1 = 0, s2 = 0;
  for (int i = 0; i < n; i++) { double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
  return (s1 * s1 + s2 * s2 - c * s1 * s2) / ((double)n * n / 4.0);
}
/* |DFT| of complex IQ at f over n pairs. */
static double cmag_at(const double *iq, int n, double f, double fs) {
  double re = 0, im = 0;
  for (int k = 0; k < n; k++) {
    double th = 2.0 * M_PI * f * (double)k / fs, c = cos(th), s = sin(th);
    double I = iq[2 * k], Q = iq[2 * k + 1];
    re += I * c + Q * s; im += Q * c - I * s;
  }
  return sqrt(re * re + im * im) / n;
}

/* ---- 1. model ---------------------------------------------------------------- */
static void test_model(void) {
  printf("\n[model] RBJ band shapes:\n");
  EqBand pk = { EQ_PEAK, 1000.0, 6.0, 1.4 };
  snprintf(det, sizeof det, "%.6f dB", eq_band_db(&pk, 1000.0));
  ok("peak: exact gain at fc", fabs(eq_band_db(&pk, 1000.0) - 6.0) < 1e-9, det);
  snprintf(det, sizeof det, "%.4f / %.4f dB", eq_band_db(&pk, 10.0), eq_band_db(&pk, 20000.0));
  ok("peak: back to 0 dB far away", fabs(eq_band_db(&pk, 10.0)) < 0.02 && fabs(eq_band_db(&pk, 20000.0)) < 0.02, det);
  int sym = 1;
  for (double x = 0.25; x <= 3.0; x += 0.25) {
    if (fabs(eq_band_db(&pk, 1000.0 * pow(2, x)) - eq_band_db(&pk, 1000.0 / pow(2, x))) > 1e-9) { sym = 0; }
  }
  ok("peak: symmetric in log frequency", sym, "");
  /* cookbook: Q = fc / BW with BW between the half-gain points,
   * 1/Q = 2 sinh(ln2/2 · BW_oct) → BW_oct = 2 asinh(1/2Q) / ln 2 */
  double bwo = 2.0 * asinh(1.0 / (2.0 * 1.4)) / M_LN2;
  double hg1 = eq_band_db(&pk, 1000.0 * pow(2, bwo / 2)), hg2 = eq_band_db(&pk, 1000.0 / pow(2, bwo / 2));
  snprintf(det, sizeof det, "BW %.3f oct → %.4f / %.4f dB (want 3.0)", bwo, hg1, hg2);
  ok("peak: Q = fc/BW at the half-gain points", fabs(hg1 - 3.0) < 1e-6 && fabs(hg2 - 3.0) < 1e-6, det);
  EqBand cut = { EQ_PEAK, 2500.0, -9.0, 4.0 };
  snprintf(det, sizeof det, "%.6f dB", eq_band_db(&cut, 2500.0));
  ok("cut: exact depth at fc", fabs(eq_band_db(&cut, 2500.0) + 9.0) < 1e-9, det);
  bwo = 2.0 * asinh(1.0 / (2.0 * 4.0)) / M_LN2;
  ok("cut: half-depth at the Q bandwidth", fabs(eq_band_db(&cut, 2500.0 * pow(2, bwo / 2)) + 4.5) < 1e-6, "");

  EqBand ls = { EQ_LOW_SHELF, 200.0, 4.0, EQ_Q_DFLT_SHELF };
  snprintf(det, sizeof det, "plateau %.4f, knee %.4f, far %.4f dB", eq_band_db(&ls, 1.0), eq_band_db(&ls, 200.0), eq_band_db(&ls, 20000.0));
  ok("low shelf: plateau = gain", fabs(eq_band_db(&ls, 1.0) - 4.0) < 0.01, det);
  ok("low shelf: half gain at the corner", fabs(eq_band_db(&ls, 200.0) - 2.0) < 1e-9, det);
  ok("low shelf: 0 dB far above", fabs(eq_band_db(&ls, 20000.0)) < 0.01, det);
  double mx = -99, mn = 99;
  for (double f = 1.0; f < 24000.0; f *= 1.01) { double v = eq_band_db(&ls, f); if (v > mx) { mx = v; } if (v < mn) { mn = v; } }
  snprintf(det, sizeof det, "range %.4f .. %.4f dB", mn, mx);
  ok("low shelf Q 0.71: no overshoot", mx <= 4.0 + 1e-3 && mn >= -1e-3, det);   /* 0.71 vs 1/√2: 0.0001 dB */
  EqBand hs = { EQ_HIGH_SHELF, 3000.0, -6.0, EQ_Q_DFLT_SHELF };
  snprintf(det, sizeof det, "low %.4f, knee %.4f, plateau %.4f dB", eq_band_db(&hs, 20.0), eq_band_db(&hs, 3000.0), eq_band_db(&hs, 300000.0));
  ok("high shelf: 0 dB far below", fabs(eq_band_db(&hs, 20.0)) < 0.01, det);
  ok("high shelf: half gain at the corner", fabs(eq_band_db(&hs, 3000.0) + 3.0) < 1e-9, det);
  ok("high shelf: plateau = gain", fabs(eq_band_db(&hs, 300000.0) + 6.0) < 0.01, det);
  EqBand res = { EQ_LOW_SHELF, 200.0, 4.0, 3.0 };
  mx = -99; for (double f = 1.0; f < 24000.0; f *= 1.01) { double v = eq_band_db(&res, f); if (v > mx) { mx = v; } }
  snprintf(det, sizeof det, "max %.3f dB", mx);
  ok("shelf Q 3: a resonant bump above the plateau", mx > 4.5, det);

  EqProfile p = { 2, { pk, ls } };
  ok("profile = sum of its bands", fabs(eq_profile_db(&p, 700.0) - (eq_band_db(&pk, 700.0) + eq_band_db(&ls, 700.0))) < 1e-12, "");
  EqBand bad = { 7, 5.0, 40.0, 0.0 };
  eq_band_clamp(&bad);
  snprintf(det, sizeof det, "type %d f %.0f g %.0f q %.2f", bad.type, bad.f, bad.g, bad.q);
  ok("clamp: type/f/g/q into range", bad.type == EQ_PEAK && bad.f == EQ_F_MIN && bad.g == EQ_GAIN_MAX && bad.q == EQ_Q_MIN, det);
  EqBand zero = { EQ_PEAK, 1000.0, 0.0, 1.0 };
  ok("0 dB band contributes nothing", eq_band_db(&zero, 1000.0) == 0.0, "");
}

/* ---- 2. grid ----------------------------------------------------------------- */
static double lerp_grid(const double *F, const double *G, int n, double f) {
  for (int i = 1; i < n; i++) {
    if (f >= F[i] && f <= F[i + 1]) {
      double t = (F[i + 1] > F[i]) ? (f - F[i]) / (F[i + 1] - F[i]) : 0.0;
      return G[i] + t * (G[i + 1] - G[i]);
    }
  }
  return G[n];
}
static int grid_sane(const double *F, const double *G, int n, double fs) {
  if (n < 4 || n > EQ_MAX_POINTS) { return 0; }
  if (F[1] != 0.0 || fabs(F[n] - fs / 2.0) > 1e-9 || G[0] != 0.0) { return 0; }
  for (int i = 1; i < n; i++) { if (!(F[i + 1] > F[i])) { return 0; } }
  return 1;
}
static void test_grid(void) {
  printf("\n[grid] the WDSP profile:\n");
  static double F[EQ_MAX_POINTS + 1], G[EQ_MAX_POINTS + 1];
  const int rates[] = { 48000, 96000, 192000 };
  EqProfile empty = { 0 };
  for (int r = 0; r < 3; r++) {
    double fs = rates[r];
    int n = eq_profile_grid(&empty, fs, F, G);
    int flat = 1; for (int i = 0; i <= n; i++) { if (G[i] != 0.0) { flat = 0; } }
    snprintf(det, sizeof det, "fs %d: n %d, F[1] %.0f, F[n] %.0f", (int)fs, n, F[1], F[n]);
    ok("empty profile → sane flat grid", grid_sane(F, G, n, fs) && flat, det);
  }
  EqProfile full = { 8, {
    { EQ_LOW_SHELF, 150.0, 3.0, 0.71 }, { EQ_PEAK, 300.0, -4.0, 2.0 }, { EQ_PEAK, 700.0, 2.0, 1.0 },
    { EQ_PEAK, 1000.0, 6.0, 10.0 },     { EQ_PEAK, 1800.0, -3.0, 3.0 }, { EQ_PEAK, 2500.0, 5.0, 1.4 },
    { EQ_PEAK, 4000.0, -8.0, 6.0 },     { EQ_HIGH_SHELF, 6000.0, -5.0, 0.71 } } };
  for (int r = 0; r < 3; r++) {
    double fs = rates[r];
    int n = eq_profile_grid(&full, fs, F, G);
    int match = 1; for (int i = 1; i <= n; i++) { if (fabs(G[i] - eq_profile_db(&full, F[i])) > 1e-12) { match = 0; } }
    snprintf(det, sizeof det, "fs %d: n %d points", (int)fs, n);
    ok("8 bands → sane grid, G = curve at every point", grid_sane(F, G, n, fs) && match, det);
    /* WDSP joins the points linearly (degree 0): the interpolated grid must
     * track the analytic curve — the whole reason for the per-band points. */
    double worst = 0.0, wf = 0.0;
    for (double f = 20.0; f < fs / 2.0; f *= 1.003) {
      double e = fabs(lerp_grid(F, G, n, f) - eq_profile_db(&full, f));
      if (e > worst) { worst = e; wf = f; }
    }
    snprintf(det, sizeof det, "fs %d: worst %.4f dB at %.0f Hz", (int)fs, worst, wf);
    ok("linear interpolation within 0.1 dB of the curve", worst < 0.1, det);
  }
  /* build cost — the grid is computed under the RX/TX chain lock on every
   * (debounced) drag step; report it so a stall would be seen here first */
  {
    gint64 t0 = g_get_monotonic_time();
    for (int k = 0; k < 20; k++) { eq_profile_grid(&full, 48000.0, F, G); }
    double ms8 = (g_get_monotonic_time() - t0) / 1e3 / 20.0;
    EqProfile one1 = { 1, { { EQ_PEAK, 1000.0, 6.0, 1.4 } } };
    t0 = g_get_monotonic_time();
    for (int k = 0; k < 20; k++) { eq_profile_grid(&one1, 48000.0, F, G); }
    double ms1 = (g_get_monotonic_time() - t0) / 1e3 / 20.0;
    snprintf(det, sizeof det, "8 bands %.2f ms, 1 band %.2f ms per grid", ms8, ms1);
    ok("grid build cost under 2 ms (8 bands)", ms8 < 2.0, det);
  }
  EqProfile narrow = { 1, { { EQ_PEAK, 1000.0, 12.0, EQ_Q_MAX } } };
  int n = eq_profile_grid(&narrow, 48000.0, F, G);
  int within = 0; for (int i = 1; i <= n; i++) { if (F[i] > 1000.0 / 1.1 && F[i] < 1000.0 * 1.1) { within++; } }
  double worst = 0.0;
  for (double f = 500.0; f < 2000.0; f *= 1.001) { double e = fabs(lerp_grid(F, G, n, f) - eq_profile_db(&narrow, f)); if (e > worst) { worst = e; } }
  snprintf(det, sizeof det, "%d points within ±10 %% of fc, worst %.4f dB", within, worst);
  ok("Q 10 bell: densely sampled, within 0.1 dB", within >= 10 && worst < 0.1, det);
}

/* ---- 3. persistence ---------------------------------------------------------- */
static void test_persist(void) {
  printf("\n[persist] \"type:f:g:q;\" strings:\n");
  EqProfile p = { 3, { { EQ_PEAK, 1000.0, 6.0, 1.4 }, { EQ_LOW_SHELF, 200.0, -3.5, 0.71 }, { EQ_HIGH_SHELF, 2500.0, 4.25, 1.0 } } }, q;
  char buf[512];
  int len = eq_profile_format(&p, buf, sizeof buf);
  snprintf(det, sizeof det, "\"%.200s\"", buf);
  ok("format: three tokens", len > 0 && strcmp(buf, "0:1000.0:6.00:1.400;1:200.0:-3.50:0.710;2:2500.0:4.25:1.000;") == 0, det);
  ok("parse: round trip", eq_profile_parse(buf, &q) == 3 && eq_profile_equal(&p, &q), "");
  ok("parse: empty / NULL → 0 bands", eq_profile_parse("", &q) == 0 && q.n == 0 && eq_profile_parse(NULL, &q) == 0, "");
  ok("format: empty profile → \"\"", eq_profile_format(&(EqProfile){ 0 }, buf, sizeof buf) == 0 && buf[0] == '\0', "");
  int n = eq_profile_parse("x:y;0:500:3:1;junk;1:2:3;2:99999:-40:0;", &q);
  snprintf(det, sizeof det, "%d bands: f %.0f/%.0f g %.0f/%.0f q %.2f/%.2f", n, q.b[0].f, q.b[1].f, q.b[0].g, q.b[1].g, q.b[0].q, q.b[1].q);
  ok("parse: malformed tokens dropped, values clamped", n == 2 && q.b[0].f == 500.0 && q.b[0].g == 3.0 &&
     q.b[1].type == EQ_HIGH_SHELF && q.b[1].f == EQ_F_MAX && q.b[1].g == -EQ_GAIN_MAX && q.b[1].q == EQ_Q_MIN, det);
  char many[1024] = "";
  for (int i = 0; i < 12; i++) { strcat(many, "0:1000:1:1;"); }
  ok("parse: more than EQ_MAX_BANDS → capped", eq_profile_parse(many, &q) == EQ_MAX_BANDS, "");
  const char *loc = setlocale(LC_NUMERIC, "cs_CZ.UTF-8");
  if (!loc) { loc = setlocale(LC_NUMERIC, "de_DE.UTF-8"); }
  if (loc) {
    eq_profile_format(&p, buf, sizeof buf);
    snprintf(det, sizeof det, "%s: \"%.24s…\"", loc, buf);
    ok("format under a comma locale keeps '.'", strchr(buf, ',') == NULL && eq_profile_parse(buf, &q) == 3 && eq_profile_equal(&p, &q), det);
    setlocale(LC_NUMERIC, "C");
  } else {
    printf("        (no comma locale installed — locale check skipped)\n");
  }
}

/* ---- 4. policy --------------------------------------------------------------- */
static void test_policy(void) {
  printf("\n[policy] tx_voice_proc_active (shared by the rotator and the EQ):\n");
  ok("USB + MOX + live mic → on",        tx_voice_proc_active(1, DEMOD_USB, 0, 1, 0, 0) == 1, "");
  ok("LSB / AM / SAM → on",              tx_voice_proc_active(1, DEMOD_LSB, 0, 1, 0, 0) && tx_voice_proc_active(1, DEMOD_AM, 0, 1, 0, 0) && tx_voice_proc_active(1, DEMOD_SAM, 0, 1, 0, 0), "");
  ok("switched off → off",               tx_voice_proc_active(0, DEMOD_USB, 0, 1, 0, 0) == 0, "");
  ok("TCI audio source → off",           tx_voice_proc_active(1, DEMOD_USB, 1, 1, 0, 0) == 0, "");
  ok("DIGU / DIGL → off (clean chain)",  !tx_voice_proc_active(1, DEMOD_DIGU, 0, 1, 0, 0) && !tx_voice_proc_active(1, DEMOD_DIGL, 0, 1, 0, 0), "");
  ok("CW / RTTY → off",                  !tx_voice_proc_active(1, DEMOD_CWU, 0, 1, 0, 0) && !tx_voice_proc_active(1, DEMOD_CWL, 0, 1, 0, 0) && !tx_voice_proc_active(1, DEMOD_RTTY, 0, 1, 0, 0), "");
  ok("TUNE / two-tone → off",            !tx_voice_proc_active(1, DEMOD_USB, 0, 1, 1, 0) && !tx_voice_proc_active(1, DEMOD_USB, 0, 1, 0, 1), "");
  ok("not keyed → off",                  tx_voice_proc_active(1, DEMOD_USB, 0, 0, 0, 0) == 0, "");
  int same = 1;
  for (int m = 0; m < DEMOD_NMODES; m++) for (int k = 0; k < 16; k++) {
    if (tx_phrot_active(1, m, k & 1, (k >> 1) & 1, (k >> 2) & 1, (k >> 3) & 1) !=
        tx_voice_proc_active(1, m, k & 1, (k >> 1) & 1, (k >> 2) & 1, (k >> 3) & 1)) { same = 0; }
  }
  ok("tx_phrot_active ≡ tx_voice_proc_active", same, "");
}

/* ---- 5. RXA through demod.c -------------------------------------------------- */
#define IQ_RATE 192000
#define RX_TONES 7
static const double rx_tone[RX_TONES] = { 120.0, 300.0, 1000.0, 1800.0, 2500.0, 4000.0, 7000.0 };
static float *tap_buf; static int tap_n, tap_cap;
static void on_tap(const float *mono48k, int n) {
  if (tap_n + n > tap_cap) { n = tap_cap - tap_n; }
  if (n > 0) { memcpy(tap_buf + tap_n, mono48k, (size_t)n * sizeof(float)); tap_n += n; }
}
/* Feed `secs` of the multi-tone and return the per-tone power from the 48 k
 * tap over the LAST `meas` seconds. ⛔ The HPSDR wire IQ is spectrally
 * inverted (RTTY-SCOPE §7; tci_server conjugates the RX stream): a station
 * heard in USB at +f audio is e^{−jωt} on the wire, so the tones go in as
 * (cos, −sin) — with (cos, +sin) they land on the LSB side and only the
 * filter's stopband leakage (~−100 dBFS) reaches the tap. */
static void rx_feed_measure(double secs, double meas, double *pw) {
  static double iq[2 * 1024];
  static double ph[RX_TONES];
  long blocks = (long)(secs * IQ_RATE / 1024);
  tap_n = 0;
  for (long b = 0; b < blocks; b++) {
    for (int i = 0; i < 1024; i++) {
      double I = 0, Q = 0;
      for (int t = 0; t < RX_TONES; t++) { I += 0.01 * cos(ph[t]); Q -= 0.01 * sin(ph[t]); ph[t] += 2.0 * M_PI * rx_tone[t] / IQ_RATE; if (ph[t] > 2 * M_PI) { ph[t] -= 2 * M_PI; } }
      iq[2 * i] = I; iq[2 * i + 1] = Q;
    }
    demod_feed(iq, 1024);
  }
  int nm = (int)(meas * 48000.0);
  if (nm > tap_n) { nm = tap_n; }
  const float *x = tap_buf + (tap_n - nm);
  for (int t = 0; t < RX_TONES; t++) { pw[t] = goertzel_f(x, nm, rx_tone[t], 48000); }
}
static void test_rxa(void) {
  printf("\n[RXA] the RX path (demod.c) — realized response vs the analytic curve:\n");
  tap_cap = 48000 * 8; tap_buf = g_new0(float, tap_cap);
  const int rates[] = { 48000, 96000, 192000 };
  EqProfile one  = { 1, { { EQ_PEAK, 1000.0, 6.0, 4.0 } } };
  EqProfile four = { 4, { { EQ_LOW_SHELF, 200.0, 4.0, 0.71 }, { EQ_PEAK, 1000.0, 6.0, 1.4 },
                          { EQ_PEAK, 2500.0, -9.0, 4.0 },      { EQ_HIGH_SHELF, 6000.0, -6.0, 0.71 } } };
  demod_set_agc(0);                        /* linear chain: no AGC */
  demod_set_audio_tap(on_tap);
  for (int r = 0; r < 3; r++) {
    int arate = rates[r];
    demod_set_audio_rate(arate);
    demod_set_eq(0, &one);
    long p0 = demod_eq_pushes();             /* demod_create pushes the cached table once */
    gint64 t0 = g_get_monotonic_time();
    int rc = demod_create(0, IQ_RATE, DEMOD_USB, 50.0, 9000.0, 0.0);
    printf("  [AF %d] demod_create %.0f ms (rc %d)\n", arate, (g_get_monotonic_time() - t0) / 1e3, rc);
    double off[RX_TONES], on[RX_TONES];
    rx_feed_measure(3.0, 2.0, off);
    { double ss = 0; for (int i = 0; i < tap_n; i++) { ss += (double)tap_buf[i] * tap_buf[i]; }
      printf("        tap: %d samples, rms %.1f dBFS, demod_peak %.4f\n", tap_n, 10 * log10(ss / (tap_n ? tap_n : 1) + 1e-30), demod_peak()); }
    snprintf(det, sizeof det, "tone power %.1f / %.1f / %.1f / %.1f / %.1f / %.1f / %.1f dB",
             10 * log10(off[0] + 1e-30), 10 * log10(off[1] + 1e-30), 10 * log10(off[2] + 1e-30), 10 * log10(off[3] + 1e-30),
             10 * log10(off[4] + 1e-30), 10 * log10(off[5] + 1e-30), 10 * log10(off[6] + 1e-30));
    ok("EQ off: all tones near −40 dB (wire-side USB)", off[0] > 1e-5 && off[2] > 1e-5 && off[6] > 1e-5 && off[2] < 1e-3, det);
    /* single narrow peak: centre within 0.5 dB, flat elsewhere within 0.2 dB */
    demod_set_eq(1, &one);
    for (int k = 0; k < 40; k++) { demod_set_eq(1, &one); }     /* the ~20 Hz GUI/slot re-push */
    rx_feed_measure(3.0, 2.0, on);
    double d1000 = 10.0 * log10(on[2] / off[2]), d300 = 10.0 * log10(on[1] / off[1]), d4000 = 10.0 * log10(on[5] / off[5]);
    snprintf(det, sizeof det, "AF %d: 1 kHz %+.3f dB (want +6), 300 Hz %+.3f, 4 kHz %+.3f", arate, d1000, d300, d4000);
    ok("one peak: centre +6 dB, neighbours flat", fabs(d1000 - 6.0) < 0.5 && fabs(d300) < 0.2 && fabs(d4000) < 0.2, det);
    snprintf(det, sizeof det, "pushes %ld (create + 41 calls, one grid)", demod_eq_pushes() - p0);
    ok("apply-on-change: create + 41 identical calls = 1 push", demod_eq_pushes() - p0 == 1, det);
    ok("state: acting (1)", demod_eq_state() == 1, "");
    /* the four-band curve at every tone */
    demod_set_eq(1, &four);
    rx_feed_measure(3.0, 2.0, on);
    double worst = 0.0; int wt = 0;
    for (int t = 0; t < RX_TONES; t++) {
      double d = 10.0 * log10(on[t] / off[t]), want = eq_profile_db(&four, rx_tone[t]);
      if (fabs(d - want) > worst) { worst = fabs(d - want); wt = t; }
    }
    snprintf(det, sizeof det, "AF %d: worst %.3f dB at %.0f Hz (got %+.2f, curve %+.2f)", arate, worst, rx_tone[wt],
             10.0 * log10(on[wt] / off[wt]), eq_profile_db(&four, rx_tone[wt]));
    ok("four bands: every tone within 0.5 dB of the curve", worst < 0.5, det);
    ok("a changed table = one more push", demod_eq_pushes() - p0 == 2, "");
    /* bypass + states */
    demod_set_mode(DEMOD_DIGU, 50.0, 9000.0);
    ok("DIGU: bypassed (state 2)", demod_eq_state() == 2, "");
    rx_feed_measure(2.0, 1.0, on);
    double dd = 10.0 * log10(on[2] / off[2]);
    snprintf(det, sizeof det, "1 kHz %+.3f dB with the EQ switched on in DIGU", dd);
    ok("DIGU: the audio is unshaped", fabs(dd) < 0.2, det);
    demod_set_mode(DEMOD_RTTY, 50.0, 9000.0);
    ok("RTTY: bypassed (state 2)", demod_eq_state() == 2, "");
    demod_set_mode(DEMOD_USB, 50.0, 9000.0);
    ok("USB again: acting (1)", demod_eq_state() == 1, "");
    demod_set_nr(3);
    demod_set_mode(DEMOD_CWU, 50.0, 9000.0);
    ok("CWU + NNR: 'not computed' state (3)", demod_eq_state() == 3, "");
    demod_set_nr(0);
    demod_set_mode(DEMOD_USB, 50.0, 9000.0);
    demod_set_eq(0, &four);
    ok("switched off: state 0", demod_eq_state() == 0, "");
    rx_feed_measure(2.0, 1.0, on);
    dd = 10.0 * log10(on[2] / off[2]);
    snprintf(det, sizeof det, "1 kHz %+.3f dB", dd);
    ok("switched off: audio back to flat", fabs(dd) < 0.2, det);
    demod_destroy();
  }
  demod_set_audio_tap(NULL);
}

/* ---- 6. TXA through tx.c ----------------------------------------------------- */
#define TX_TONES 4
static const double tx_tone[TX_TONES] = { 300.0, 1000.0, 2000.0, 2800.0 };
#define TX_CAP 480000
static double g_iq[2 * TX_CAP]; static int g_np;
static void on_tx_iq(const double *iq, int n, void *u) {
  (void)u;
  for (int i = 0; i < n && g_np < TX_CAP; i++) { g_iq[2 * g_np] = iq[2 * i]; g_iq[2 * g_np + 1] = iq[2 * i + 1]; g_np++; }
}
static EqProfile *g_tx_eq; static int g_tx_eq_on, g_tx_reapply;
static void tx_run_tones(double *mag) {
  g_np = 0;
  tx_dsp_create(DEMOD_USB, 100.0, 4000.0, 0, on_tx_iq, NULL);
  tx_dsp_set_compressor(0, 0.0);
  tx_dsp_set_mic_gain(0.0);
  tx_dsp_set_gate(0, -45.0);
  tx_dsp_set_eq(g_tx_eq_on, g_tx_eq);
  tx_dsp_run(1);
  float mic[512]; double ph[TX_TONES] = { 0 };
  for (int b = 0; b < 188; b++) {                 /* ≈ 2 s of mic */
    for (int i = 0; i < 512; i++) {
      double v = 0; for (int t = 0; t < TX_TONES; t++) { v += 0.01 * sin(ph[t]); ph[t] += 2.0 * M_PI * tx_tone[t] / 48000.0; }
      mic[i] = (float)v;
    }
    tx_dsp_feed_mic(mic, 512);
    if (g_tx_reapply) { tx_dsp_set_eq(g_tx_eq_on, g_tx_eq); }
  }
  int an = 19200, a0 = g_np - an;                 /* 100 ms = whole cycles of every 100 Hz multiple */
  for (int t = 0; t < TX_TONES; t++) {
    double mp = cmag_at(g_iq + 2 * a0, an, tx_tone[t], 192000.0), mm = cmag_at(g_iq + 2 * a0, an, -tx_tone[t], 192000.0);
    mag[t] = mp > mm ? mp : mm;                   /* WDSP's baseband puts USB on one side; take it */
  }
  tx_dsp_destroy();
}
static void test_txa(void) {
  printf("\n[TXA] the TX path (tx.c, 96 kHz dsp) — realized response vs the curve:\n");
  EqProfile p = { 3, { { EQ_LOW_SHELF, 200.0, -6.0, 0.71 }, { EQ_PEAK, 1000.0, 6.0, 1.4 }, { EQ_HIGH_SHELF, 2500.0, 4.0, 0.71 } } };
  double off[TX_TONES], on[TX_TONES];
  g_tx_eq = &p; g_tx_eq_on = 0; g_tx_reapply = 0;
  tx_run_tones(off);
  ok("EQ off: tones present", off[0] > 1e-6 && off[1] > 1e-6 && off[3] > 1e-6, "");
  ok("EQ off: not active", tx_dsp_eq_active() == 0, "");
  long p0 = tx_dsp_eq_pushes();
  g_tx_eq_on = 1; g_tx_reapply = 1;
  tx_run_tones(on);
  double worst = 0.0; int wt = 0;
  for (int t = 0; t < TX_TONES; t++) {
    double d = 20.0 * log10(on[t] / off[t]), want = eq_profile_db(&p, tx_tone[t]);
    if (fabs(d - want) > worst) { worst = fabs(d - want); wt = t; }
  }
  snprintf(det, sizeof det, "worst %.3f dB at %.0f Hz (got %+.2f, curve %+.2f)", worst, tx_tone[wt],
           20.0 * log10(on[wt] / off[wt]), eq_profile_db(&p, tx_tone[wt]));
  ok("three bands: every tone within 0.5 dB of the curve", worst < 0.5, det);
  snprintf(det, sizeof det, "pushes %ld for 189 calls", tx_dsp_eq_pushes() - p0);
  ok("apply-on-change: per-block re-push = 1 WDSP push", tx_dsp_eq_pushes() - p0 == 1, det);
  g_tx_reapply = 0;
  EqProfile flat = { 0 };
  g_tx_eq = &flat; g_tx_eq_on = 1;
  tx_run_tones(on);
  double d = 20.0 * log10(on[1] / off[1]);
  snprintf(det, sizeof det, "1 kHz %+.3f dB", d);
  ok("on with no bands: not active, audio flat", tx_dsp_eq_active() == 0 && fabs(d) < 0.1, det);
}

int main(void) {
  const char *wd = getenv("SDRFL_WISDOM_DIR");
  if (wd && *wd) { printf("wisdom: WDSPwisdom(%s) -> %d (0 = imported, 1 = rebuilt)\n", wd, WDSPwisdom((char *)wd)); }
  printf("sdrfl-eq-test — WDSP %d, parametric EQ (issue #48)\n", GetWDSPVersion());
  test_model();
  test_grid();
  test_persist();
  test_policy();
  test_rxa();
  test_txa();
  printf("\n=== %d checks, %d failures ===\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
