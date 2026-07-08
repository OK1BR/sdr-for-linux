/*
 * sdrfl-swr-test — OFFLINE TX metering + SWR gate (F3, docs/TX-DESIGN.md).
 *
 * Feeds synthetic ALEX coupler words into tx_meter.c and checks the G1 watts +
 * SWR math against hand values from piHPSDR transmitter.c:645-758:
 *   - RX / matched load  → SWR settles to ~1.0
 *   - a ~2:1 mismatch    → SWR settles to ~2.0, forward watts plausible
 *   - SWR rises monotonically with reflected power
 *   - an extreme mismatch is clamped to a finite SWR (gamma ≤ 0.95)
 *
 * Pure math — NO radio, NO socket. Exit 0 = pass.
 */
#include <math.h>
#include <stdio.h>

#include "tx_meter.h"

static int g_checks, g_fail;
static void ok(const char *what, int cond, const char *detail) {
  g_checks++;
  if (cond) { printf("  ok    %-42s %s\n", what, detail ? detail : ""); }
  else      { printf("  FAIL  %-42s %s\n", what, detail ? detail : ""); g_fail++; }
}

/* Reset, feed the same words `iters` times to let the smoothed SWR converge,
 * and return the final SWR (fwd/rev watts are readable afterwards via getters). */
static double converge(int fwd_raw, int rev_raw, int is_6m, int iters) {
  tx_meter_reset();
  for (int i = 0; i < iters; i++) { tx_meter_update(fwd_raw, rev_raw, is_6m); }
  return tx_meter_swr();
}

int main(void) {
  char det[160];
  printf("=== sdrfl-swr-test — offline TX metering + SWR gate (F3) ===\n\n");

  /* ---- RX: no coupler drive → SWR stays 1, watts ~0 ------------------- */
  printf("[RX] fwd_raw=0 rev_raw=0 → SWR settles to 1.0, ~0 W:\n");
  double swr_rx = converge(0, 0, 0, 60);
  snprintf(det, sizeof det, "swr=%.3f fwd=%.3fW rev=%.3fW", swr_rx, tx_meter_fwd_w(), tx_meter_rev_w());
  ok("RX SWR ~ 1.0", fabs(swr_rx - 1.0) < 0.02, det);
  ok("RX forward power ~ 0 W", tx_meter_fwd_w() < 0.01, "");

  /* ---- matched load: strong fwd, no rev → SWR ~ 1 -------------------- */
  printf("\n[matched] fwd_raw=2000 rev_raw=42(→0) → SWR ~ 1.0:\n");
  double swr_m = converge(2000, 42, 0, 60);
  double fwd_m = tx_meter_fwd_w();
  snprintf(det, sizeof det, "swr=%.3f fwd=%.2fW", swr_m, fwd_m);
  ok("matched SWR ~ 1.0", fabs(swr_m - 1.0) < 0.05, det);
  ok("forward power plausible (40-55 W)", fwd_m > 40.0 && fwd_m < 55.0, "");

  /* ---- ~2:1 mismatch: fwd_raw=2000 rev_raw=770 → SWR ~ 2.0 ----------- */
  printf("\n[2:1] fwd_raw=2000 rev_raw=770 → SWR ~ 2.0:\n");
  double swr_2 = converge(2000, 770, 0, 60);
  snprintf(det, sizeof det, "swr=%.3f fwd=%.2fW rev=%.2fW", swr_2, tx_meter_fwd_w(), tx_meter_rev_w());
  ok("2:1 mismatch SWR ~ 2.0", swr_2 > 1.85 && swr_2 < 2.15, det);
  ok("reverse power plausible (4.5-6 W)", tx_meter_rev_w() > 4.5 && tx_meter_rev_w() < 6.0, "");

  /* ---- monotonic: more reflected power → higher SWR ------------------ */
  printf("\n[monotonic] SWR rises with reflected power:\n");
  double s0 = converge(2000, 42,  0, 60);   /* matched */
  double s1 = converge(2000, 400, 0, 60);   /* mild    */
  double s2 = converge(2000, 770, 0, 60);   /* ~2:1    */
  snprintf(det, sizeof det, "%.2f < %.2f < %.2f", s0, s1, s2);
  ok("SWR monotonic in reflected power", s0 < s1 && s1 < s2, det);

  /* ---- clamp: extreme mismatch → finite SWR (gamma ≤ 0.95) ----------- */
  printf("\n[clamp] extreme mismatch → finite SWR (gamma clamped 0.95):\n");
  double swr_hi = converge(2000, 2200, 0, 60);
  snprintf(det, sizeof det, "swr=%.2f (gamma clamp → ~39)", swr_hi);
  ok("extreme SWR clamped finite (30-50)", swr_hi > 30.0 && swr_hi < 50.0, det);

  printf("\n=== %d checks, %d failures ===\n", g_checks, g_fail);
  if (g_fail == 0) { printf("PASS — G1 watts + SWR match piHPSDR; RX settles to SWR 1.0.\n"); }
  else             { printf("FAIL — %d mismatch(es) above.\n", g_fail); }
  return g_fail ? 1 : 0;
}
