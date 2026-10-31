/*
 * sdrfl-txdsp-test — OFFLINE TX DSP + IQ-encoding gate (F2, docs/TX-DESIGN.md).
 *
 * Three offline checks, NO radio and NO socket:
 *  1. WDSP TX DSP: feed a 1000 Hz mic tone in USB and confirm the 192 kHz IQ
 *     output is a clean upper-sideband tone at +1000 Hz (the −1000 Hz image is
 *     suppressed) — proves the modulator + bandpass + resampler chain works.
 *  2. p2_tx_iq_encode: 24-bit-BE mapping, by hand values + an encode→decode
 *     round-trip against the RX-side decode formula (np.c:2446).
 *  3. p2_tx_iq_framer: 240-sample / 1444-byte packetisation + sequence numbers.
 *
 * Producing IQ here cannot key the radio (needs MOX + PA, both off). Exit 0 = pass.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tx.h"
#include "tx_run.h"    /* TX_PHROT_HZ_* + the pure tx_phrot_active / tx_mode_is_voice (#23) */
#include "protocol2.h"

#define TXTEST_USB 1   /* WDSP/DEMOD mode integer for USB */
#define TXTEST_LSB 0   /* WDSP/DEMOD mode integer for LSB */

/* ---- tiny check framework ------------------------------------------------ */
static int g_checks, g_fail;
static void ok(const char *what, int cond, const char *detail) {
  g_checks++;
  if (cond) { printf("  ok    %-40s %s\n", what, detail ? detail : ""); }
  else      { printf("  FAIL  %-40s %s\n", what, detail ? detail : ""); g_fail++; }
}

/* ---- 1. TX DSP: collect the IQ the callback produces --------------------- */
#define MAXP 640000   /* 300 blocks × 2048 pairs — room for the PROC settle runs */
static double g_iq[2 * MAXP];
static int    g_np;
static void on_tx_iq(const double *iq, int n, void *u) {
  (void)u;
  for (int i = 0; i < n && g_np < MAXP; i++) {
    g_iq[2 * g_np]     = iq[2 * i];
    g_iq[2 * g_np + 1] = iq[2 * i + 1];
    g_np++;
  }
}
/* |DFT| of the complex IQ at frequency f (Hz), sample rate fs, over n pairs. */
static double cmag_at(const double *iq, int n, double f, double fs) {
  double re = 0, im = 0;
  for (int k = 0; k < n; k++) {
    double th = 2.0 * M_PI * f * (double)k / fs;   /* exp(-j th) */
    double c = cos(th), s = sin(th);
    double I = iq[2 * k], Q = iq[2 * k + 1];
    re += I * c + Q * s;     /* Re[(I+jQ)(c - js)] */
    im += Q * c - I * s;     /* Im[(I+jQ)(c - js)] */
  }
  return sqrt(re * re + im * im) / n;
}
static int    g_gate_on;          /* mic noise gate for the next run_tone */
static double g_gate_db = -35.0;
static double g_gate_depth = 20.0;   /* below-threshold cut, dB */
static double g_mic_gain = 0.0;      /* panel gain — the gate threshold is
                                        operator-scale (post-gain dBFS) */
static int    g_gate_reapply;     /* re-push the (unchanged) gate every block,
                                     like the live tx_run slot does */

/* Peak envelope sqrt(I²+Q²) over a window — for the ALC-ceiling check. */
static double env_peak(const double *iq, int n) {
  double pk = 0.0;
  for (int k = 0; k < n; k++) {
    double e = iq[2 * k] * iq[2 * k] + iq[2 * k + 1] * iq[2 * k + 1];
    if (e > pk) { pk = e; }
  }
  return sqrt(pk);
}

/* Feed a 1000 Hz mic tone (amplitude `amp`, `blocks`×512 samples, optional PROC)
 * through a fresh TX channel in `mode` on the `p1` (48 k, no CFIR) or P2 (192 k)
 * chain; return the |DFT| at +1000/-1000/+5000 Hz plus the envelope peak over
 * the settled tail. Returns the fexchange0 error. */
static int run_tone_p(int mode, int p1, double amp, int blocks, int comp,
                      double comp_db, double *m_plus, double *m_minus,
                      double *m_junk, double *m_env) {
  g_np = 0;
  double rate = p1 ? 48000.0 : 192000.0;
  /* Signed passband selects the sideband in WDSP (same convention as RX): USB is
   * a positive passband, LSB a negative one (cf. gui.c FILT_USB/FILT_LSB). */
  double flo = (mode == TXTEST_LSB) ? -2850.0 : 150.0;
  double fhi = (mode == TXTEST_LSB) ?  -150.0 : 2850.0;
  tx_dsp_create(mode, flo, fhi, p1, on_tx_iq, NULL);
  tx_dsp_set_compressor(comp, comp_db);
  tx_dsp_set_mic_gain(g_mic_gain);
  tx_dsp_set_gate_depth(g_gate_depth);
  tx_dsp_set_gate(g_gate_on, g_gate_db);
  tx_dsp_run(1);
  float mic[512];
  double ph = 0.0, dph = 2.0 * M_PI * 1000.0 / 48000.0;
  for (int b = 0; b < blocks; b++) {
    for (int i = 0; i < 512; i++) { mic[i] = (float)(amp * sin(ph)); ph += dph; }
    tx_dsp_feed_mic(mic, 512);
    if (g_gate_reapply) {   /* the live gate-slot pattern: same values, ~20 Hz */
      tx_dsp_set_mic_gain(g_mic_gain);
      tx_dsp_set_gate_depth(g_gate_depth);
      tx_dsp_set_gate(g_gate_on, g_gate_db);
    }
  }
  int an = p1 ? 10000 : 40000, a0 = g_np - an;   /* settled tail, past transients */
  if (a0 < 0) { a0 = g_np / 2; an = g_np - a0; }
  *m_plus  = cmag_at(g_iq + 2 * a0, an, +1000.0, rate);
  *m_minus = cmag_at(g_iq + 2 * a0, an, -1000.0, rate);
  *m_junk  = cmag_at(g_iq + 2 * a0, an, +5000.0, rate);
  if (m_env) { *m_env = env_peak(g_iq + 2 * a0, an); }
  int err = tx_dsp_last_error();
  tx_dsp_destroy();
  return err;
}

static int run_tone(int mode, double amp, int blocks, int comp, double comp_db,
                    double *m_plus, double *m_minus, double *m_junk, double *m_env) {
  return run_tone_p(mode, 0, amp, blocks, comp, comp_db, m_plus, m_minus, m_junk, m_env);
}

/* ---- 1d. TX phase rotator (WDSP PHROT, issue #23) ------------------------ */
static int    g_ph_on, g_ph_auto, g_ph_reapply;
static double g_ph_hz = TX_PHROT_HZ_DFLT;
#define PH_BLOCKS 188          /* ≈ 2 s of mic at 48 k */
#define PH_TAIL   38400        /* 200 ms at 192 k = 60 cycles of 300 Hz: every
                                  harmonic below is an integer number of cycles
                                  in the window, so cmag_at reads them leak-free */
/* A deliberately ASYMMETRIC "voice": harmonics 300/600/900/1200 Hz in cosine
 * phase (the positive peaks pile up, the negative ones do not), amplitude well
 * below the ALC. Two tones would prove nothing — an all-pass only time-shifts a
 * two-tone envelope; from three components on, the envelope and the waveform
 * asymmetry depend on the relative phases, which is exactly what the rotator
 * moves. USB on the P2 chain; the rotator's own readout comes back too. */
static int run_harmonics(double *in_ratio, double *out_ratio, double *fc, double *step) {
  g_np = 0;
  tx_dsp_create(TXTEST_USB, 150.0, 2850.0, 0, on_tx_iq, NULL);
  tx_dsp_set_compressor(0, 0.0);
  tx_dsp_set_mic_gain(0.0);
  tx_dsp_set_gate_depth(20.0);
  tx_dsp_set_gate(0, -35.0);
  tx_dsp_set_phrot(g_ph_on, g_ph_hz, g_ph_auto);
  tx_dsp_run(1);
  static const double f[4] = { 300.0, 600.0, 900.0, 1200.0 };
  double ph[4] = { 0.0, 0.0, 0.0, 0.0 };
  float mic[512];
  for (int b = 0; b < PH_BLOCKS; b++) {
    for (int i = 0; i < 512; i++) {
      double s = 0.0;
      for (int k = 0; k < 4; k++) { s += 0.08 * cos(ph[k]); ph[k] += 2.0 * M_PI * f[k] / 48000.0; }
      mic[i] = (float)s;
    }
    tx_dsp_feed_mic(mic, 512);
    if (g_ph_reapply) { tx_dsp_set_phrot(g_ph_on, g_ph_hz, g_ph_auto); }   /* the live slot pattern */
  }
  int run = 0;
  tx_dsp_get_phrot(&run, in_ratio, out_ratio, fc, step);
  int err = tx_dsp_last_error();
  tx_dsp_destroy();
  return err;
}
/* Waveform asymmetry of Re(IQ) over the settled tail — for USB that is the
 * on-air audio itself (the TX dump's txre.f32 convention): |pos−neg|/(pos+neg). */
static double re_asym(const double *iq, int n, double *pos, double *neg) {
  double p = 0.0, m = 0.0;
  for (int k = 0; k < n; k++) {
    double v = iq[2 * k];
    if (v > p)  { p = v; }
    if (-v > m) { m = -v; }
  }
  if (pos) { *pos = p; }
  if (neg) { *neg = m; }
  return (p + m) > 1e-12 ? fabs(p - m) / (p + m) : 0.0;
}
static double env_rms(const double *iq, int n) {
  double s = 0.0;
  for (int k = 0; k < n; k++) { s += iq[2 * k] * iq[2 * k] + iq[2 * k + 1] * iq[2 * k + 1]; }
  return sqrt(s / n);
}

/* ---- 2. RX-side decode (mirror of decode_iq, np.c:2446) ------------------ */
static double decode3(const unsigned char *b) {
  int s  = (int)((signed char) b[0]) << 16;
  s |= (int)((((unsigned char) b[1]) << 8) & 0xFF00);
  s |= (int)((unsigned char) b[2] & 0xFF);
  return (double)s * 1.1920928955078125E-7;   /* 1/2^23 */
}

/* ---- 3. framer emit capture ---------------------------------------------- */
static int g_pkts, g_lastlen;
static uint32_t g_seq[8];
static void cap_emit(const unsigned char *pkt, int len, void *user) {
  (void)user;
  if (g_pkts < 8) {
    g_seq[g_pkts] = ((uint32_t)pkt[0] << 24) | ((uint32_t)pkt[1] << 16) |
                    ((uint32_t)pkt[2] << 8) | pkt[3];
  }
  g_lastlen = len;
  g_pkts++;
}

int main(void) {
  printf("=== sdrfl-txdsp-test — offline TX DSP + IQ-encoding gate (F2) ===\n\n");

  /* ---- 1. TX DSP: clean SSB, USB/LSB on opposite sidebands ------------- */
  /* Convention-independent: we verify each mode is a clean single-sideband tone
   * and that USB and LSB land on OPPOSITE sides. WDSP TXA puts the USB tone on the
   * −1000 Hz side of the complex baseband; the radio's DUC up-converts that to the
   * correct upper sideband on air (verified live at F5). */
  printf("[TX DSP] 1000 Hz mic tone; each mode clean SSB, USB/LSB opposite:\n");
  double up, um, uj, lp, lm, lj;
  int uerr = run_tone(TXTEST_USB, 0.5, 90, 0, 0.0, &up, &um, &uj, NULL);
  int lerr = run_tone(TXTEST_LSB, 0.5, 90, 0, 0.0, &lp, &lm, &lj, NULL);
  char det[160];
  ok("fexchange0 no error (USB+LSB)", uerr == 0 && lerr == 0, "");

  /* ---- 1b. P1 (HL2) chain: 48 k out, no CFIR — same clean-SSB criteria ---- */
  {
    printf("\n[P1 TX DSP] the 48 k / CFIR-off chain (T2, docs/P1-TX-SCOPE.md):\n");
    double pp, pm, pj;
    int np_before;
    int perr = run_tone_p(TXTEST_USB, 1, 0.5, 90, 0, 0.0, &pp, &pm, &pj, NULL);
    np_before = g_np;
    ok("fexchange0 no error (P1 USB)", perr == 0, "");
    snprintf(det, sizeof det, "out pairs %d (in 90*512 @1:1)", np_before);
    ok("P1 output rate 1:1 with mic rate", np_before == 90 * 512, det);
    /* Same WDSP baseband convention as the P2 USB case: the tone lands on the
     * −1 kHz side (the DUC flips it on air). Magnitude 0.5 vs P2's 0.4481 =
     * ×0.896 — direct proof the P2-only CFIR really is off on the P1 chain. */
    snprintf(det, sizeof det, "-1k %.4f  +1k %.6f  +5k %.6f (P2 CFIR would give %.4f)",
             pm, pp, pj, pm * 0.896);
    ok("P1 tone clean (image/junk < 1%)", pm > 0.45 && pp < pm / 100 && pj < pm / 100, det);
  }
  double us_hi = up > um ? up : um, us_lo = up > um ? um : up;
  double ls_hi = lp > lm ? lp : lm, ls_lo = lp > lm ? lm : lp;
  snprintf(det, sizeof det, "USB +1k=%.4f -1k=%.4f | LSB +1k=%.4f -1k=%.4f", up, um, lp, lm);
  ok("USB clean SSB (>20x, no +5k spur)", us_hi > 1e-3 && us_hi > 20.0*us_lo && us_hi > 20.0*uj, det);
  ok("LSB clean SSB (>20x, no +5k spur)", ls_hi > 1e-3 && ls_hi > 20.0*ls_lo && ls_hi > 20.0*lj, "");
  int usb_side = up > um ? +1 : -1;
  int lsb_side = lp > lm ? +1 : -1;
  snprintf(det, sizeof det, "USB→%+dkHz side, LSB→%+dkHz side", usb_side, lsb_side);
  ok("USB and LSB on opposite sidebands", usb_side != lsb_side, det);

  /* ---- 1b. PROC (leveler + COMP): the only makeup gain in the chain ------ */
  /* Without PROC the chain is unity (ALC only attenuates) — a quiet mic gives a
   * quiet exciter. With PROC on, a quiet tone must come up (leveler +8 dB and
   * COMP gain), a modest tone must reach ~full scale, the ALC must hold the
   * ceiling, and the signal must stay clean SSB. 300 blocks ≈ 3.2 s so the
   * leveler (decay 500 ms) settles before the measured tail. */
  printf("\n[PROC] leveler + compressor (piHPSDR tx_set_compressor):\n");
  {
    double qp, qm, qj, cp, cm, cj, fenv;
    run_tone(TXTEST_USB, 0.05, 300, 0, 0.0,  &qp, &qm, &qj, NULL);  /* quiet, off  */
    run_tone(TXTEST_USB, 0.05, 300, 1, 10.0, &cp, &cm, &cj, NULL);  /* quiet, 10dB */
    /* USB puts the tone on the −1 kHz side of the complex baseband (see check 1);
     * the signal is max(±1 kHz), the leftover image is min(±1 kHz). */
    double qs = qp > qm ? qp : qm;
    double cs = cp > cm ? cp : cm, ci = cp > cm ? cm : cp;
    snprintf(det, sizeof det, "off=%.4f on=%.4f (%.1fx)", qs, cs, qs > 0 ? cs / qs : 0.0);
    ok("PROC lifts a quiet (-26 dBFS) tone >2x", qs > 1e-3 && cs > 2.0 * qs, det);
    ok("PROC output stays clean SSB", cs > 20.0 * ci && cs > 20.0 * cj, "");
    double fp, fm, fj;
    run_tone(TXTEST_USB, 0.5, 300, 1, 10.0, &fp, &fm, &fj, &fenv);  /* modest, 10dB */
    snprintf(det, sizeof det, "envelope peak = %.3f", fenv);
    ok("PROC drives a modest tone to full scale", fenv > 0.85, det);
    ok("ALC holds the ceiling (env <= 1.05)", fenv <= 1.05, det);
  }

  /* ---- 1c. Mic noise gate (AMSQ downward expander) ----------------------- */
  /* A "noise" tone below the threshold must come out ~20 dB down (the muted
   * gain) versus gate-off; a "speech" tone above the threshold must pass at
   * unity. Verifies the whole SetTXAAMSQ* path — if this passes, a live "gate
   * does nothing" is a threshold-tuning issue, not a DSP bug. */
  printf("\n[gate] mic noise gate (WDSP AMSQ, threshold -35 dBFS):\n");
  {
    double np_, nm, nj, sp_, sm, sj, e;
    g_gate_on = 1; g_gate_db = -35.0;
    run_tone(TXTEST_USB, 0.005, 90, 0, 0.0, &np_, &nm, &nj, &e);  /* -46 dBFS "noise" */
    double gated = np_ > nm ? np_ : nm;
    g_gate_on = 0;
    run_tone(TXTEST_USB, 0.005, 90, 0, 0.0, &np_, &nm, &nj, &e);
    double open_ = np_ > nm ? np_ : nm;
    snprintf(det, sizeof det, "below thr: open=%.5f gated=%.5f (%.1f dB down)",
             open_, gated, 20.0 * log10(gated / open_));
    ok("below threshold drops ~20 dB", gated < open_ * 0.20 && gated > open_ * 0.05, det);
    g_gate_on = 1;
    run_tone(TXTEST_USB, 0.5, 90, 0, 0.0, &sp_, &sm, &sj, &e);    /* -6 dBFS "speech" */
    double loud = sp_ > sm ? sp_ : sm;
    snprintf(det, sizeof det, "above thr: %.4f (expect ~0.45)", loud);
    ok("above threshold passes at unity", loud > 0.40, det);
    /* Operator gate depth (SetDEXPExpansionRatio): 10 dB cut instead of the
     * hard 20 — the shipped GUI default. Same -46 dBFS tone, expect ~-10 dB. */
    g_gate_depth = 10.0;
    run_tone(TXTEST_USB, 0.005, 90, 0, 0.0, &np_, &nm, &nj, &e);
    double soft = np_ > nm ? np_ : nm;
    snprintf(det, sizeof det, "below thr @depth 10: %.5f vs open %.5f (%.1f dB down)",
             soft, open_, 20.0 * log10(soft / open_));
    ok("gate depth 10 cuts ~10 dB (not 20)", soft < open_ * 0.45 && soft > open_ * 0.20, det);
    g_gate_depth = 20.0;
    /* Threshold is OPERATOR scale (post-mic-gain dBFS, = the Mic bar): with
     * +20 dB mic gain the -46 dBFS raw tone reads -26 on the meter — ABOVE the
     * -35 threshold — so the gate must stay open (output = open baseline x10).
     * Under the old pre-gain semantics it would have closed (-46 < -35). */
    g_mic_gain = 20.0;
    run_tone(TXTEST_USB, 0.005, 90, 0, 0.0, &np_, &nm, &nj, &e);
    double resc = np_ > nm ? np_ : nm;
    snprintf(det, sizeof det, "gain +20: %.5f vs open %.5f (%.1fx, expect ~10x)",
             resc, open_, resc / open_);
    ok("threshold rescales with mic gain (gate stays open)",
       resc > open_ * 5.0 && resc < open_ * 15.0, det);
    g_mic_gain = 0.0;
    /* ⛔ Regression tripwire for the "gate deforms audio" bug: DEXP setters
     * decalc+calc on EVERY call (state → LOW, detector → 0), so re-pushing
     * unchanged values from the ~20 Hz gate slot chopped all speech ~9 dB.
     * tx.c gate_apply must therefore apply on change only — with that, the
     * live re-apply pattern is bit-transparent vs a single apply. */
    double rp_, rm, rj;
    run_tone(TXTEST_USB, 0.5, 90, 0, 0.0, &sp_, &sm, &sj, &e);   /* single apply */
    double once = sp_ > sm ? sp_ : sm;
    g_gate_reapply = 1;
    run_tone(TXTEST_USB, 0.5, 90, 0, 0.0, &rp_, &rm, &rj, &e);
    g_gate_reapply = 0;
    double reap = rp_ > rm ? rp_ : rm;
    snprintf(det, sizeof det, "single=%.5f reapplied=%.5f (ratio %.4f)",
             once, reap, once > 0 ? reap / once : 0.0);
    ok("slot-style re-apply is transparent (no state reset)",
       fabs(reap / once - 1.0) < 0.01, det);
    g_gate_on = 0;
  }

  /* ---- 1d. TX phase rotator (WDSP PHROT, issue #23) ---------------------- */
  printf("\n[phrot] TX phase rotator (all-pass cascade after the mic gain):\n");
  {
    double in_r, out_r, fc, step, pos, neg;
    /* OFF = the reference. */
    g_ph_on = 0; g_ph_auto = 0; g_ph_reapply = 0;
    ok("harmonics, rotator off: fexchange0 ok", run_harmonics(&in_r, &out_r, &fc, &step) == 0, "");
    int a0 = g_np - PH_TAIL;
    double asym_off = re_asym(g_iq + 2 * a0, PH_TAIL, &pos, &neg);
    double env_off  = env_peak(g_iq + 2 * a0, PH_TAIL);
    double rms_off  = env_rms(g_iq + 2 * a0, PH_TAIL);
    /* Components sit on the −f side of the WDSP baseband for USB (section 1's
     * convention; the DUC flips them on air). */
    double m300_off = cmag_at(g_iq + 2 * a0, PH_TAIL, -300.0, 192000.0);
    double m900_off = cmag_at(g_iq + 2 * a0, PH_TAIL, -900.0, 192000.0);
    ok("off: components on the USB side (premise)", m300_off > 0.05 && m900_off > 0.05, "");
    snprintf(det, sizeof det, "pos=%.4f neg=%.4f asym=%.3f crest=%.2f step=%.0f",
             pos, neg, asym_off, env_off / rms_off, step);
    ok("off: test waveform is asymmetric (premise)", asym_off > 0.15, det);
    ok("off: optimizer reports 0 (not running)", step == 0.0, "");

    /* ON at the default corner. */
    g_ph_on = 1;
    ok("harmonics, rotator on: fexchange0 ok", run_harmonics(&in_r, &out_r, &fc, &step) == 0, "");
    a0 = g_np - PH_TAIL;
    double asym_on = re_asym(g_iq + 2 * a0, PH_TAIL, &pos, &neg);
    double env_on  = env_peak(g_iq + 2 * a0, PH_TAIL);
    double rms_on  = env_rms(g_iq + 2 * a0, PH_TAIL);
    double m300_on = cmag_at(g_iq + 2 * a0, PH_TAIL, -300.0, 192000.0);
    double m900_on = cmag_at(g_iq + 2 * a0, PH_TAIL, -900.0, 192000.0);
    snprintf(det, sizeof det, "asym off=%.3f on=%.3f (pos=%.4f neg=%.4f)", asym_off, asym_on, pos, neg);
    ok("on: on-air waveform more symmetric", asym_on < 0.7 * asym_off, det);
    snprintf(det, sizeof det, "WDSP in=%.3f out=%.3f fc=%.0f Hz step=%.0f", in_r, out_r, fc, step);
    ok("on: WDSP asymmetry readout out < in", out_r < in_r && in_r > 0.0, det);
    ok("on: corner reads the 338 Hz default", fabs(fc - TX_PHROT_HZ_DFLT) < 0.01, det);
    ok("on: optimizer still off (step 0)", step == 0.0, det);
    double d300 = 20.0 * log10(m300_on / m300_off), d900 = 20.0 * log10(m900_on / m900_off);
    snprintf(det, sizeof det, "300 Hz %+.3f dB, 900 Hz %+.3f dB", d300, d900);
    ok("on: amplitude response flat (all-pass)", fabs(d300) < 0.05 && fabs(d900) < 0.05, det);
    snprintf(det, sizeof det, "rms off=%.4f on=%.4f (%+.3f dB)", rms_off, rms_on, 20.0 * log10(rms_on / rms_off));
    ok("on: same power (no gain change)", fabs(20.0 * log10(rms_on / rms_off)) < 0.05, det);
    printf("        (SSB envelope peak/RMS: off %.3f, on %.3f — informative; real speech decides)\n",
           env_off / rms_off, env_on / rms_on);

    /* ⛔ Slot-style re-apply must be transparent — the cache tripwire (TX-DESIGN
     * §8): without tx.c's apply-on-change cache every block would re-run
     * SetTXAPHROTCorner (all-pass history freed → a transient per block, so
     * the output waveform changes) and Run(1) (the asymmetry envelopes zeroed
     * → WDSP's out ratio collapses to one block's worth, ~1/375 of this).
     * The output IQ is delivered in order and is bit-exact between runs; the
     * WDSP metric readout is not — WDSP runs the TXA chain in its own thread
     * and the last block may still be in flight when we read, so that one
     * gets a few per cent of slack. */
    g_ph_reapply = 1;
    double in_1 = in_r, out_1 = out_r;
    ok("harmonics, rotator re-applied per block: ok", run_harmonics(&in_r, &out_r, &fc, &step) == 0, "");
    g_ph_reapply = 0;
    a0 = g_np - PH_TAIL;
    double asym_re = re_asym(g_iq + 2 * a0, PH_TAIL, NULL, NULL);
    snprintf(det, sizeof det, "asym single=%.6f reapplied=%.6f; WDSP out single=%.5f reapplied=%.5f",
             asym_on, asym_re, out_1, out_r);
    ok("slot-style re-apply is transparent (no state reset)",
       fabs(asym_re - asym_on) < 1e-6 && fabs(out_r / out_1 - 1.0) < 0.05 && fabs(in_r / in_1 - 1.0) < 0.05, det);

    /* Auto: the optimizer is armed (its step stays at the initial 50 Hz until
     * the first 15 s A/B window — far longer than this feed). */
    g_ph_auto = 1;
    ok("harmonics, auto corner: ok", run_harmonics(&in_r, &out_r, &fc, &step) == 0, "");
    snprintf(det, sizeof det, "step=%.0f Hz fc=%.0f", step, fc);
    ok("auto: optimizer armed (step > 0, not locked)", step > 0.0, det);
    g_ph_auto = 0; g_ph_on = 0;

    /* The engine-side policy, pure (tx_run.h): where the rotator may act. */
    ok("policy: USB + MOX + live mic → on",     tx_phrot_active(1, DEMOD_USB, 0, 1, 0, 0) == 1, "");
    ok("policy: LSB / AM / SAM → on",          tx_phrot_active(1, DEMOD_LSB, 0, 1, 0, 0) &&
                                               tx_phrot_active(1, DEMOD_AM,  0, 1, 0, 0) &&
                                               tx_phrot_active(1, DEMOD_SAM, 0, 1, 0, 0), "");
    ok("policy: switched off → off",           tx_phrot_active(0, DEMOD_USB, 0, 1, 0, 0) == 0, "");
    ok("policy: TCI audio source → off",       tx_phrot_active(1, DEMOD_USB, 1, 1, 0, 0) == 0, "");
    ok("policy: DIGU / DIGL → off (clean chain)", !tx_phrot_active(1, DEMOD_DIGU, 0, 1, 0, 0) &&
                                               !tx_phrot_active(1, DEMOD_DIGL, 0, 1, 0, 0), "");
    ok("policy: CW / RTTY → off",              !tx_phrot_active(1, DEMOD_CWU,  0, 1, 0, 0) &&
                                               !tx_phrot_active(1, DEMOD_CWL,  0, 1, 0, 0) &&
                                               !tx_phrot_active(1, DEMOD_RTTY, 0, 1, 0, 0), "");
    ok("policy: TUNE / two-tone → off",        !tx_phrot_active(1, DEMOD_USB, 0, 1, 1, 0) &&
                                               !tx_phrot_active(1, DEMOD_USB, 0, 1, 0, 1), "");
    ok("policy: not keyed → off",              tx_phrot_active(1, DEMOD_USB, 0, 0, 0, 0) == 0, "");
    ok("policy: the EQ shares the predicate (#48)",   /* tx_voice_proc_active = the family's one rule */
       tx_voice_proc_active(1, DEMOD_USB, 0, 1, 0, 0) == 1 && !tx_voice_proc_active(1, DEMOD_DIGU, 0, 1, 0, 0) &&
       !tx_voice_proc_active(1, DEMOD_USB, 1, 1, 0, 0) && !tx_voice_proc_active(1, DEMOD_USB, 0, 1, 1, 0), "");
    ok("mode table: voice = not CW/RTTY/DIGU/DIGL",
       tx_mode_is_voice(DEMOD_USB) && tx_mode_is_voice(DEMOD_LSB) && tx_mode_is_voice(DEMOD_AM) &&
       tx_mode_is_voice(DEMOD_SAM) && !tx_mode_is_voice(DEMOD_CWL) && !tx_mode_is_voice(DEMOD_CWU) &&
       !tx_mode_is_voice(DEMOD_RTTY) && !tx_mode_is_voice(DEMOD_DIGU) && !tx_mode_is_voice(DEMOD_DIGL), "");
  }

  /* ---- 2. encoder: hand values + round-trip --------------------------- */
  printf("\n[encode] 24-bit BE mapping (np.c:2919):\n");
  unsigned char e[6];
  double z[2] = { 0.0, 0.0 };
  p2_tx_iq_encode(z, 1, e);
  ok("(0,0) → 00 00 00 00 00 00",
     e[0]==0&&e[1]==0&&e[2]==0&&e[3]==0&&e[4]==0&&e[5]==0, "");
  double p1[2] = { 1.0, 0.0 };
  p2_tx_iq_encode(p1, 1, e);
  snprintf(det, sizeof det, "%02x %02x %02x", e[0], e[1], e[2]);
  ok("I=+1.0 → 7F FF AB", e[0]==0x7F&&e[1]==0xFF&&e[2]==0xAB, det);
  double m1[2] = { -1.0, 0.0 };
  p2_tx_iq_encode(m1, 1, e);
  snprintf(det, sizeof det, "%02x %02x %02x", e[0], e[1], e[2]);
  ok("I=-1.0 → 80 00 55", e[0]==0x80&&e[1]==0x00&&e[2]==0x55, det);

  /* round-trip through the RX decode: max error must be tiny */
  {
    double vals[] = { -0.9, -0.5, -0.1, 0.0, 0.1, 0.5, 0.9 };
    double maxerr = 0.0;
    for (unsigned i = 0; i < sizeof(vals)/sizeof(vals[0]); i++) {
      double iq[2] = { vals[i], -vals[i] };
      unsigned char b[6];
      p2_tx_iq_encode(iq, 1, b);
      double di = decode3(b), dq = decode3(b + 3);
      double ei = fabs(di - vals[i]), eq = fabs(dq + vals[i]);
      if (ei > maxerr) maxerr = ei;
      if (eq > maxerr) maxerr = eq;
    }
    snprintf(det, sizeof det, "max round-trip error = %.2e", maxerr);
    ok("encode→decode round-trip < 1e-3", maxerr < 1e-3, det);
  }

  /* ---- 3. framer: 240-sample / 1444-byte packets + sequence ----------- */
  printf("\n[framer] 240-sample / 1444-byte packets + sequence:\n");
  {
    p2_tx_iq_framer f;
    p2_tx_iq_framer_init(&f, cap_emit, NULL);
    double *blk = g_iq;              /* reuse: any 500 pairs of data */
    for (int i = 0; i < 500; i++) { blk[2*i] = 0.1; blk[2*i+1] = -0.1; }
    p2_tx_iq_framer_push(&f, blk, 500);
    ok("500 samples → 2 packets emitted", g_pkts == 2, "");
    ok("packet length = 1444 bytes",      g_lastlen == 1444, "");
    ok("sequence 0 then 1",               g_seq[0] == 0 && g_seq[1] == 1, "");
    ok("20 samples held over",            f.fill == 20, "");
  }

  printf("\n=== %d checks, %d failures ===\n", g_checks, g_fail);
  if (g_fail == 0) { printf("PASS — TX DSP produces a clean SSB IQ tone; encoder + framer match P2.\n"); }
  else             { printf("FAIL — %d mismatch(es) above.\n", g_fail); }
  return g_fail ? 1 : 0;
}
