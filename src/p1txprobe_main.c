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
  p1_build_cc_general(c, DEV, RATE, FREQ, NULL, 1);
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
    step = p1_build_cc_round_robin(c, DEV, FREQ, GAIN, step, NULL, NULL, 1);
    expect(what, c, off_rr[i], 5);
    check("  MOX bit clear", (c[0] & 0x01) == 0);
  }
  check("round-robin wraps after 11 steps", step == 0);

  /* ---- 2. HOT layout ---- */
  p1_tx_state hot = { .mox = 1, .pa_enabled = 1, .in_band = 1,
                      .drive_att = 240, .tune = 0 };
  p1_build_cc_general(c, DEV, RATE, FREQ, &hot, 1);
  check("general HOT: MOX bit set", (c[0] & 0x01) == 1);

  step = 0;
  for (int i = 0; i < 11; i++) {
    step = p1_build_cc_round_robin(c, DEV, FREQ, GAIN, step, &hot, NULL, 1);
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
  step = 0; step = p1_build_cc_round_robin(c, DEV, FREQ, GAIN, 2, &oob, NULL, 1);
  expect("HOT out-of-band 0x12: drive FORCED 0", c,
         (const unsigned char[]){0x13, 0x00, 0x08, 0x00, 0x00}, 5);

  p1_tx_state dry = hot;                       /* keyed, PA off = dry key */
  dry.pa_enabled = 0;
  step = p1_build_cc_round_robin(c, DEV, FREQ, GAIN, 2, &dry, NULL, 1);
  expect("HOT PA-off 0x12: T/R relay stays RX ⛔", c,
         (const unsigned char[]){0x13, 0xF0, 0x04, 0x00, 0x00}, 5);

  /* ---- 2b. PureSignal wire (P1-TX-SCOPE §6) ---- */
  p1_ps_state psw = { .enabled = 1, .attenuation = 10 };

  /* nrx=4: general C4 gains the (nrx-1)<<3 bits, nothing else moves. */
  p1_build_cc_general(c, DEV, RATE, FREQ, NULL, 4);
  expect("PS general nrx=4: C4 duplex|3<<3", c,
         (const unsigned char[]){0x00, 0x02, 0x88, 0x00, 0x1C}, 5);

  /* nrx=4 round-robin: TX NCO + 4 DDC NCOs (RX3/RX4 = feedback, same value:
   * dial == DUC), then the registers; cycle = 14. PS off here → 0x14/0x1C
   * must stay byte-identical to the OFF table. */
  static const unsigned char ps_nco[5][5] = {
    {0x02, 0x00, 0x6B, 0x1D, 0xE0},   /* TX NCO                       */
    {0x04, 0x00, 0x6B, 0x1D, 0xE0},   /* RX1                          */
    {0x06, 0x00, 0x6B, 0x1D, 0xE0},   /* RX2                          */
    {0x08, 0x00, 0x6B, 0x1D, 0xE0},   /* RX3 = coupler feedback       */
    {0x0A, 0x00, 0x6B, 0x1D, 0xE0},   /* RX4 = TX-DAC loopback        */
  };
  step = 0;
  for (int i = 0; i < 5; i++) {
    char what[64];
    snprintf(what, sizeof what, "PS nrx=4 NCO step %d (C0=0x%02X)", i, ps_nco[i][0]);
    step = p1_build_cc_round_robin(c, DEV, FREQ, GAIN, step, NULL, NULL, 4);
    expect(what, c, ps_nco[i], 5);
  }
  for (int i = 5; i < 14; i++) {
    step = p1_build_cc_round_robin(c, DEV, FREQ, GAIN, step, NULL, NULL, 4);
    expect(i == 5 ? "PS nrx=4 registers follow (0x12 unchanged)" :
           (off_rr[i - 3][0] == 0x14 ? "PS-off 0x14 byte-identical" :
            off_rr[i - 3][0] == 0x1C ? "PS-off 0x1C byte-identical" : "PS nrx=4 register"),
           c, off_rr[i - 3], 5);
  }
  check("PS nrx=4 cycle wraps after 14 steps", step == 0);

  /* PS enabled, receiving: 0x14 gains the PS bit (C2 0x40, o_p.c:2284), C4
   * keeps the RX LNA gain; 0x1C-C3 = 0xC0|(31-att) — TX-att is static
   * config (o_p.c:2372-2390). */
  step = 0;
  for (int i = 0; i < 14; i++) {
    step = p1_build_cc_round_robin(c, DEV, FREQ, GAIN, step, NULL, &psw, 4);
    if (c[0] == 0x14) {
      expect("PS-on RX 0x14: C2|=0x40, C4=RX gain", c,
             (const unsigned char[]){0x14, 0x00, 0x40, 0x00, 0x5A}, 5);
    } else if (c[0] == 0x1C) {
      expect("PS-on 0x1C: C3=0xC0|(31-10)", c,
             (const unsigned char[]){0x1C, 0x00, 0x00, 0xD5, 0x00}, 5);
    }
  }

  /* PS enabled + keyed: 0x14-C4 = 0x40|(31-att) — the LNA IS the feedback
   * attenuator (o_p.c:2288-2308). MOX bit on every frame as ever. */
  step = 0;
  for (int i = 0; i < 14; i++) {
    step = p1_build_cc_round_robin(c, DEV, FREQ, GAIN, step, &hot, &psw, 4);
    if ((c[0] & 0xFE) == 0x14) {
      expect("PS-on TX 0x14: C4=0x40|(31-10), MOX", c,
             (const unsigned char[]){0x15, 0x00, 0x40, 0x00, 0x55}, 5);
    }
  }

  /* Attenuation clamping: att 31 → rxgain 0. */
  p1_ps_state ps31 = { .enabled = 1, .attenuation = 31 };
  step = 0;
  for (int i = 0; i < 14; i++) {
    step = p1_build_cc_round_robin(c, DEV, FREQ, GAIN, step, &hot, &ps31, 4);
    if ((c[0] & 0xFE) == 0x14) {
      check("PS att 31 → TX rxgain 0", c[4] == 0x40);
    }
  }

  /* ---- 2c. EP6 multi-RX demux (pure parser) ---- */
  {
    unsigned char f[512];
    memset(f, 0, sizeof f);
    f[0] = f[1] = f[2] = 0x7F;                  /* sync */
    /* nrx=4 payload: 19 samples × (4×6 B IQ + 2 B mic). Pattern: sample s,
     * channel ch → I = 0x010000*ch + s + 1 (24-bit BE), Q = negated. */
    unsigned char *p = f + 8;
    for (int s = 0; s < 19; s++) {
      for (int ch = 0; ch < 4; ch++) {
        int iv = 0x10000 * ch + s + 1;
        int qv = -iv;
        p[0] = (iv >> 16) & 0xFF; p[1] = (iv >> 8) & 0xFF; p[2] = iv & 0xFF;
        p[3] = (qv >> 16) & 0xFF; p[4] = (qv >> 8) & 0xFF; p[5] = qv & 0xFF;
        p += 6;
      }
      p[0] = 0; p[1] = 0; p += 2;               /* mic */
    }

    double rx1[2 * 19], txfb[2 * 19], rxfb[2 * 19];
    int n = p1_ep6_parse_samples(f, 4, rx1, txfb, rxfb);
    check("EP6 nrx=4: 19 samples/frame ((512-8)/26)", n == 19);
    int demux_ok = 1;
    const double SC = 1.1920928955078125E-7;    /* 2^-23 */
    for (int s = 0; s < 19 && demux_ok; s++) {
      demux_ok = rx1 [2*s] == (0x10000*0 + s + 1) * SC &&   /* chan 0 → RX1  */
                 rxfb[2*s] == (0x10000*2 + s + 1) * SC &&   /* chan 2 → rxfb */
                 txfb[2*s] == (0x10000*3 + s + 1) * SC &&   /* chan 3 → txfb */
                 rx1[2*s + 1] == -rx1[2*s];                 /* Q sign intact */
    }
    check("EP6 nrx=4: chan0→RX1, chan2→rxfb, chan3→txfb", demux_ok);

    unsigned char f1[512];                      /* nrx=1 regression frame */
    memset(f1, 0, sizeof f1);
    f1[0] = f1[1] = f1[2] = 0x7F;
    f1[8] = 0x00; f1[9] = 0x00; f1[10] = 0x2A;  /* sample 0: I = 42 */
    n = p1_ep6_parse_samples(f1, 1, rx1, NULL, NULL);
    check("EP6 nrx=1: 63 samples/frame, I decode", n == 63 && rx1[0] == 42 * SC);

    f1[1] = 0x00;                               /* broken sync */
    check("EP6 bad sync → -1", p1_ep6_parse_samples(f1, 1, rx1, NULL, NULL) == -1);
  }

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
