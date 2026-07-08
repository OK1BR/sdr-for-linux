/*
 * sdrfl-txgate-test — OFFLINE TX safety-gate check (F4, docs/TX-DESIGN.md).
 *
 * Exercises each guard in tx_gate.c with synthetic inputs, NO radio and NO
 * socket. The gate only DECIDES a p2_tx_state; nothing here (or in F4) sends it,
 * so no RF is produced. Exit 0 = every guard behaves.
 */
#include <stdio.h>
#include <string.h>

#include "tx_gate.h"

static int g_checks, g_fail;
static void ok(const char *what, int cond, const char *detail) {
  g_checks++;
  if (cond) { printf("  ok    %-44s %s\n", what, detail ? detail : ""); }
  else      { printf("  FAIL  %-44s %s\n", what, detail ? detail : ""); g_fail++; }
}

/* Base config: R1, PA on, ANT1, drive 100 / tune 20, SWR protection on @ 3.0. */
static tx_gate_cfg base_cfg(void) {
  tx_gate_cfg c;
  memset(&c, 0, sizeof c);
  c.pa_enabled = 1; c.antenna = 0; c.drive_byte = 100; c.tune_byte = 20;
  c.swr_protect = 1; c.swr_alarm = 3.0; c.allow_oob = 0;
  c.region = BP_R1; c.country_key = "";
  return c;
}
/* Base input: 20 m in-band, MOX requested, matched (SWR 1, fwd 20 W). */
static tx_gate_in base_in(void) {
  tx_gate_in i;
  memset(&i, 0, sizeof i);
  i.want_mox = 1; i.freq_hz = 14200000; i.swr = 1.0; i.fwd_w = 20.0; i.rev_w = 0.5;
  return i;
}

int main(void) {
  tx_gate_result r;
  char det[160];
  printf("=== sdrfl-txgate-test — offline TX safety-gate check (F4) ===\n\n");

  /* 1. normal keyed TX in band */
  printf("[keyed] in-band, PA on, matched:\n");
  tx_gate_reset();
  { tx_gate_cfg c = base_cfg(); tx_gate_in i = base_in(); tx_gate_evaluate(&c, &i, &r); }
  snprintf(det, sizeof det, "keyed=%d mox=%d pa=%d inband=%d drive=%d ant=%d",
           r.keyed, r.state.mox, r.state.pa_enabled, r.state.in_band, r.state.drive, r.state.antenna);
  ok("keyed with mox=1, pa=1, drive=100", r.keyed && r.state.mox == 1 &&
     r.state.pa_enabled == 1 && r.state.in_band == 1 && r.state.drive == 100, det);
  ok("allowed, not tripped", r.allowed && !r.tripped, "");

  /* 2. out of band → refuse */
  printf("\n[out-of-band] 13.5 MHz → refuse:\n");
  tx_gate_reset();
  { tx_gate_cfg c = base_cfg(); tx_gate_in i = base_in(); i.freq_hz = 13500000;
    tx_gate_evaluate(&c, &i, &r); }
  ok("out of band: not keyed, not allowed", !r.keyed && !r.allowed, r.reason);

  /* 3. out of band + allow_oob → permitted */
  printf("\n[out-of-band + allow_oob] 13.5 MHz → permitted:\n");
  tx_gate_reset();
  { tx_gate_cfg c = base_cfg(); c.allow_oob = 1; tx_gate_in i = base_in();
    i.freq_hz = 13500000; tx_gate_evaluate(&c, &i, &r); }
  ok("allow_oob: keyed + allowed", r.keyed && r.allowed, "");

  /* 4. PA disabled for the band → pa off in the state (no TX_RELAY downstream) */
  printf("\n[PA off] band_disable_pa=1 → state.pa_enabled=0:\n");
  tx_gate_reset();
  { tx_gate_cfg c = base_cfg(); c.band_disable_pa = 1; tx_gate_in i = base_in();
    tx_gate_evaluate(&c, &i, &r); }
  ok("keyed but pa_enabled=0", r.keyed && r.state.pa_enabled == 0, "");

  /* 5. SWR spike (one reading) → NOT tripped (2-consecutive filter) */
  printf("\n[SWR spike] one high reading → no trip:\n");
  tx_gate_reset();
  { tx_gate_cfg c = base_cfg(); tx_gate_in i = base_in(); i.swr = 5.0;
    tx_gate_evaluate(&c, &i, &r); }
  ok("single high-SWR sample does not trip", !r.tripped && r.keyed, "");

  /* 6. SWR trip (two readings) → drop MOX, latched */
  printf("\n[SWR trip] two high readings → drop MOX, latch:\n");
  tx_gate_reset();
  { tx_gate_cfg c = base_cfg(); tx_gate_in i = base_in(); i.swr = 5.0;
    tx_gate_evaluate(&c, &i, &r);          /* 1st */
    tx_gate_evaluate(&c, &i, &r); }        /* 2nd → trip */
  ok("two high-SWR samples trip", r.tripped && !r.keyed && r.state.mox == 0, r.reason);
  /* re-key without release, SWR now fine → still latched */
  { tx_gate_cfg c = base_cfg(); tx_gate_in i = base_in(); i.swr = 1.0;
    tx_gate_evaluate(&c, &i, &r); }
  ok("stays latched until release (SWR back to 1)", r.tripped && !r.keyed, "");

  /* 7. high SWR during TUNE → protection suppressed */
  printf("\n[TUNE] high SWR during tune → NOT tripped (ATU):\n");
  tx_gate_reset();
  { tx_gate_cfg c = base_cfg(); tx_gate_in i = base_in();
    i.want_mox = 0; i.want_tune = 1; i.swr = 5.0;
    tx_gate_evaluate(&c, &i, &r);
    tx_gate_evaluate(&c, &i, &r);
    tx_gate_evaluate(&c, &i, &r); }
  snprintf(det, sizeof det, "keyed=%d tune=%d drive=%d tripped=%d",
           r.keyed, r.state.tune, r.state.drive, r.tripped);
  ok("tune keyed, tune drive, no trip", r.keyed && r.state.tune == 1 &&
     r.state.drive == 20 && !r.tripped, det);

  /* 8. open antenna (fwd>10, fwd-rev<1) with LOW SWR → trip anyway */
  printf("\n[open antenna] fwd=15 rev=14.5, SWR low → trip:\n");
  tx_gate_reset();
  { tx_gate_cfg c = base_cfg(); tx_gate_in i = base_in();
    i.swr = 1.2; i.fwd_w = 15.0; i.rev_w = 14.5;
    tx_gate_evaluate(&c, &i, &r);
    tx_gate_evaluate(&c, &i, &r); }
  ok("open-antenna trips (low SWR)", r.tripped && !r.keyed, "");

  /* 9. release resets the latch, then re-key recovers */
  printf("\n[release] release clears latch, re-key recovers:\n");
  tx_gate_reset();
  { tx_gate_cfg c = base_cfg(); tx_gate_in i = base_in(); i.swr = 5.0;
    tx_gate_evaluate(&c, &i, &r); tx_gate_evaluate(&c, &i, &r); }   /* trip */
  { tx_gate_cfg c = base_cfg(); tx_gate_in i = base_in();
    i.want_mox = 0; i.swr = 1.0; tx_gate_evaluate(&c, &i, &r); }    /* release */
  ok("release clears the latch", !r.tripped, "");
  { tx_gate_cfg c = base_cfg(); tx_gate_in i = base_in(); i.swr = 1.0;
    tx_gate_evaluate(&c, &i, &r); }                                 /* re-key */
  ok("re-key after release is permitted", r.keyed && !r.tripped, "");

  printf("\n=== %d checks, %d failures ===\n", g_checks, g_fail);
  if (g_fail == 0) { printf("PASS — every safety guard behaves; the gate never keys out of band or through a trip.\n"); }
  else             { printf("FAIL — %d mismatch(es) above.\n", g_fail); }
  return g_fail ? 1 : 0;
}
