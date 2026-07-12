/*
 * sdrfl-p1txprobe — OFFLINE Protocol-1 TX byte-layout gate (T1,
 * docs/P1-TX-SCOPE.md). NO radio, NO socket — builds the EP2 C&C frames and
 * the TX IQ payload with the pure builders and asserts every byte against
 * the piHPSDR @974acba layout established in the audit:
 *
 *  1. OFF regression: tx=NULL must yield the exact bytes the live RX-only
 *     link sends today — MOX bit 0 on every frame, drive 0, T/R relay locked
 *     RX (0x12-C2=0x04), LNA/OC/0x2E per the RX build.
 *  2. HOT layout: a keyed state sets C0 bit0 on EVERY frame, drive byte and
 *     PA-enable land where piHPSDR puts them, out-of-band forces drive 0 and
 *     PA-off keeps the T/R relay in RX even with MOX set (the dry-key state).
 *  3. TX IQ encode: audio slots zero (HL2 extended-addr hazard), 16-bit BE
 *     piHPSDR scaling, CWX LSB guard on HL-class only.
 *
 * Exit 0 = every assertion holds.
 */
#include <stdio.h>
#include <string.h>

#include "discovered.h"
#include "protocol1.h"

static int g_fail;

static void expect(const char *what, const unsigned char *got,
                   const unsigned char *want, int n) {
  if (memcmp(got, want, n) != 0) {
    g_fail = 1;
    printf("FAIL %s\n  got :", what);
    for (int i = 0; i < n; i++) { printf(" %02X", got[i]); }
    printf("\n  want:");
    for (int i = 0; i < n; i++) { printf(" %02X", want[i]); }
    printf("\n");
  } else {
    printf("ok   %s\n", what);
  }
}

static void check(const char *what, int cond) {
  printf("%s %s\n", cond ? "ok  " : "FAIL", what);
  if (!cond) { g_fail = 1; }
}

int main(void) {
  const int       DEV  = DEVICE_HERMES_LITE2;
  const long long FREQ = 7020000;              /* 40 m → N2ADR OC 68 */
  const int       RATE = 0x02;                 /* 192 k */
  const int       GAIN = 14;
  unsigned char c[5];
  printf("=== sdrfl-p1txprobe — offline P1 TX byte gate ===\n");

  /* ---- 1. OFF regression (tx = NULL): today's RX-only wire ---- */
  p1_build_cc_general(c, DEV, RATE, FREQ, NULL);
  expect("general OFF (rate|OC 68<<1|duplex)", c,
         (const unsigned char[]){0x00, 0x02, 0x88, 0x00, 0x04}, 5);

  static const unsigned char off_rr[11][5] = {
    {0x02, 0x00, 0x6B, 0x1D, 0xE0},   /* TX NCO = 7020000            */
    {0x04, 0x00, 0x6B, 0x1D, 0xE0},   /* RX1 NCO                     */
    {0x12, 0x00, 0x04, 0x00, 0x00},   /* drive 0 + T/R locked RX ⛔  */
    {0x14, 0x00, 0x00, 0x00, 0x5A},   /* LNA 0x40|(14+12)            */
    {0x16, 0x00, 0x00, 0x00, 0x00},
    {0x1C, 0x00, 0x00, 0x00, 0x00},
    {0x1E, 0x00, 0x00, 0x00, 0x00},   /* FPGA keyer disabled         */
    {0x20, 0x00, 0x00, 0x00, 0x00},
    {0x22, 25,   0x00, 100,  0x00},   /* PWM min/max                 */
    {0x2E, 0x00, 0x00, 20,   40  },   /* PTT hang 20 / TX latency 40 */
    {0x24, 0x00, 0x00, 0x00, 0x00},
  };
  int step = 0;
  for (int i = 0; i < 11; i++) {
    char what[64];
    snprintf(what, sizeof what, "round-robin OFF step %d (C0=0x%02X)", i, off_rr[i][0]);
    step = p1_build_cc_round_robin(c, DEV, FREQ, GAIN, step, NULL);
    expect(what, c, off_rr[i], 5);
    check("  MOX bit clear", (c[0] & 0x01) == 0);
  }
  check("round-robin wraps after 11 steps", step == 0);

  /* ---- 2. HOT layout ---- */
  p1_tx_state hot = { .mox = 1, .pa_enabled = 1, .in_band = 1,
                      .drive_att = 240, .tune = 0 };
  p1_build_cc_general(c, DEV, RATE, FREQ, &hot);
  check("general HOT: MOX bit set", (c[0] & 0x01) == 1);

  step = 0;
  for (int i = 0; i < 11; i++) {
    step = p1_build_cc_round_robin(c, DEV, FREQ, GAIN, step, &hot);
    char what[48];
    snprintf(what, sizeof what, "HOT step %d MOX bit set", i);
    check(what, (c[0] & 0x01) == 1);
    if (i == 2) {
      expect("HOT 0x12: drive 240 + PA enable 0x08", c,
             (const unsigned char[]){0x13, 0xF0, 0x08, 0x00, 0x00}, 5);
    }
  }

  p1_tx_state oob = hot;                       /* keyed but out of band */
  oob.in_band = 0;
  step = 0; step = p1_build_cc_round_robin(c, DEV, FREQ, GAIN, 2, &oob);
  expect("HOT out-of-band 0x12: drive FORCED 0", c,
         (const unsigned char[]){0x13, 0x00, 0x08, 0x00, 0x00}, 5);

  p1_tx_state dry = hot;                       /* keyed, PA off = dry key */
  dry.pa_enabled = 0;
  step = p1_build_cc_round_robin(c, DEV, FREQ, GAIN, 2, &dry);
  expect("HOT PA-off 0x12: T/R relay stays RX ⛔", c,
         (const unsigned char[]){0x13, 0xF0, 0x04, 0x00, 0x00}, 5);

  /* ---- 3. TX IQ encode ---- */
  unsigned char pkt[16];
  double iq[2] = { 0.5, -0.5 };
  int n = p1_tx_iq_encode(iq, 1, 1.0, DEV, pkt);
  check("encode returns 8 B/pair", n == 8);
  /* (int32)(0.5*32766.672+32767.5)-32767 = 16383 = 0x3FFF, LSB &0xFE;
   * -0.5 → -16383 = 0xC001, LSB &0xFE = 0x00; audio slots zero. */
  expect("IQ +0.5/-0.5 HL2 (zero audio + CWX LSB guard)", pkt,
         (const unsigned char[]){0, 0, 0, 0, 0x3F, 0xFE, 0xC0, 0x00}, 8);

  p1_tx_iq_encode(iq, 1, 0.0, DEV, pkt);       /* scale 0 = silence */
  expect("IQ scale 0 → all-zero payload", pkt,
         (const unsigned char[]){0, 0, 0, 0, 0, 0, 0, 0}, 8);

  p1_tx_iq_encode(iq, 1, 1.0, DEVICE_METIS, pkt);   /* non-HL keeps LSB */
  expect("IQ non-HL: LSB kept (no CWX guard)", pkt + 4,
         (const unsigned char[]){0x3F, 0xFF, 0xC0, 0x01}, 4);

  printf(g_fail ? "\nFAIL — TX byte layout diverges from piHPSDR.\n"
                : "\nPASS — OFF state byte-identical, HOT layout matches piHPSDR.\n");
  return g_fail;
}
