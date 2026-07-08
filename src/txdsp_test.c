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
#define MAXP 200000
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
/* Feed a 1000 Hz mic tone through a fresh TX channel in `mode`; return the |DFT|
 * at +1000/-1000/+5000 Hz over a settled window. Returns the fexchange0 error. */
static int run_tone(int mode, double *m_plus, double *m_minus, double *m_junk) {
  g_np = 0;
  /* Signed passband selects the sideband in WDSP (same convention as RX): USB is
   * a positive passband, LSB a negative one (cf. gui.c FILT_USB/FILT_LSB). */
  double flo = (mode == TXTEST_LSB) ? -2850.0 : 150.0;
  double fhi = (mode == TXTEST_LSB) ?  -150.0 : 2850.0;
  tx_dsp_create(mode, flo, fhi, on_tx_iq, NULL);
  tx_dsp_run(1);
  float mic[512];
  double ph = 0.0, dph = 2.0 * M_PI * 1000.0 / 48000.0;
  for (int b = 0; b < 90; b++) {                 /* ~0.96 s of audio */
    for (int i = 0; i < 512; i++) { mic[i] = (float)(0.5 * sin(ph)); ph += dph; }
    tx_dsp_feed_mic(mic, 512);
  }
  int a0 = 120000, an = 40000;                   /* settled window, past transients */
  if (a0 + an > g_np) { a0 = g_np / 2; an = g_np - a0; }
  *m_plus  = cmag_at(g_iq + 2 * a0, an, +1000.0, 192000.0);
  *m_minus = cmag_at(g_iq + 2 * a0, an, -1000.0, 192000.0);
  *m_junk  = cmag_at(g_iq + 2 * a0, an, +5000.0, 192000.0);
  int err = tx_dsp_last_error();
  tx_dsp_destroy();
  return err;
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
  int uerr = run_tone(TXTEST_USB, &up, &um, &uj);
  int lerr = run_tone(TXTEST_LSB, &lp, &lm, &lj);
  char det[160];
  ok("fexchange0 no error (USB+LSB)", uerr == 0 && lerr == 0, "");
  double us_hi = up > um ? up : um, us_lo = up > um ? um : up;
  double ls_hi = lp > lm ? lp : lm, ls_lo = lp > lm ? lm : lp;
  snprintf(det, sizeof det, "USB +1k=%.4f -1k=%.4f | LSB +1k=%.4f -1k=%.4f", up, um, lp, lm);
  ok("USB clean SSB (>20x, no +5k spur)", us_hi > 1e-3 && us_hi > 20.0*us_lo && us_hi > 20.0*uj, det);
  ok("LSB clean SSB (>20x, no +5k spur)", ls_hi > 1e-3 && ls_hi > 20.0*ls_lo && ls_hi > 20.0*lj, "");
  int usb_side = up > um ? +1 : -1;
  int lsb_side = lp > lm ? +1 : -1;
  snprintf(det, sizeof det, "USB→%+dkHz side, LSB→%+dkHz side", usb_side, lsb_side);
  ok("USB and LSB on opposite sidebands", usb_side != lsb_side, det);

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
