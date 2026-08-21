/*
 * sdrfl-p2dev-test — OFFLINE per-device Protocol-2 byte gate (S2,
 * docs/RADIOS-SCOPE.md).
 *
 * The ANAN G2 (Saturn) bring-up cannot be tested on hardware here — we own a
 * G2E, not a G2. This gate is therefore the ONLY executable verification of the
 * per-device wire differences, and it asserts two things:
 *
 *   1. SATURN / ORION2 get the bytes piHPSDR @974acba sends them:
 *        general[59] = 0x03   two Alex boards enabled   (np.c:693-697)
 *        rxspec[4]   = 2      two ADCs                  (radio.c:1541-1587)
 *        rxspec[7]   = 0x04   the RX DDC is DDC2        (np.c:1627-1631)
 *        HP  [9..12] AND [17..20] carry the RX phase    (np.c:816-835)
 *        HP  alex0 band-PASS knees (the "g2class" table, np.c:1044-1069)
 *   2. The three radios we DO support are byte-identical to before this
 *      change — the real regression risk of the Saturn work is the G2E, the
 *      ANAN 10E and (P1, untouched here) the HL2, not the radio we cannot try.
 *
 * NO radio, NO socket — pure buffer construction. Exit 0 = every check passes.
 *
 * Phase words (2^32 / 122880000 = 65536/1875):
 *   7.100 MHz -> 0x0ECAAAAA,  14.200 MHz -> 0x1D955555
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "discovered.h"   /* NEW_DEVICE_G1 / _HERMES2 / _SATURN / _ORION2 */
#include "protocol2.h"

static int g_checks = 0;
static int g_fail = 0;

static void chk(const char *what, long got, long want) {
  g_checks++;
  if (got != want) {
    printf("  FAIL  %-42s got=0x%lx want=0x%lx\n", what,
           (unsigned long)got, (unsigned long)want);
    g_fail++;
  } else {
    printf("  ok    %-42s = 0x%lx\n", what, (unsigned long)got);
  }
}

static uint32_t be32(const unsigned char *b) {
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
         ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}

/* Every byte outside `allow` must be zero — catches a stray write into a slot
 * we never inspect (the packets are 1444 B of mostly-zero). */
static void only_these_nonzero(const char *what, const unsigned char *buf, int len,
                               const int *allow, int nallow) {
  int extra = -1;
  for (int i = 0; i < len && extra < 0; i++) {
    if (buf[i] == 0) { continue; }
    int ok = 0;
    for (int j = 0; j < nallow && !ok; j++) { ok = (allow[j] == i); }
    if (!ok) { extra = i; }
  }
  g_checks++;
  if (extra >= 0) {
    printf("  FAIL  %-42s stray byte [%d] = 0x%02x\n", what, extra, buf[extra]);
    g_fail++;
  } else {
    printf("  ok    %-42s (no stray non-zero bytes)\n", what);
  }
}

/* One RX-only device pass: the bytes a running RX link actually puts on the
 * wire (pa_enabled=0, tx=NULL, ps=NULL — the live engine's arguments). */
static void device_pass(const char *label, int dev, int alex_en, int n_adc,
                        int ddc, uint32_t alex0_40m) {
  unsigned char buf[1500];
  const long long F40 = 7100000LL;          /* 40 m — the class discriminator  */
  const uint32_t  PH40 = 0x0ECAAAAAu;

  printf("\n[%s] device=%d\n", label, dev);

  /* ---- General ---------------------------------------------------------- */
  p2_build_general(buf, dev, 0);
  chk("general[59] Alex enable",        buf[59], alex_en);
  chk("general[58] PA enable = 0",      buf[58], 0x00);
  chk("general[37] phase-word mode",    buf[37], 0x08);
  chk("general[38] hardware timer",     buf[38], 0x01);
  {
    static const int allow[] = { 37, 38, 59 };   /* [0..3] = sequence, 0 here */
    only_these_nonzero("general: RX build is minimal", buf, 60, allow, 3);
  }

  /* ---- RX-specific ------------------------------------------------------ */
  p2_build_receive_specific(buf, dev, 192000, NULL, 0);
  chk("rxspec[4] n_adc",                buf[4], n_adc);
  chk("rxspec[5] dither off",           buf[5], 0);
  chk("rxspec[6] random off",           buf[6], 0);
  chk("rxspec[7] DDC enable bitmap",    buf[7], 1 << ddc);
  chk("rxspec ADC feeding the DDC = 0", buf[17 + ddc * 6], 0);
  chk("rxspec rate MSB (192 kHz)",      buf[18 + ddc * 6], 0);
  chk("rxspec rate LSB (192 kHz)",      buf[19 + ddc * 6], 192);
  chk("rxspec bits per sample",         buf[22 + ddc * 6], 24);

  /* ---- High-Priority, running, RX only ---------------------------------- */
  p2_build_high_priority(buf, dev, F40, 1, NULL, NULL);
  chk("hp[4] run bit, no MOX",          buf[4], 0x01);
  chk("hp[9..12] DDC0 phase (40 m)",    be32(buf + 9), PH40);
  chk("hp DDC-slot phase = DDC0",       be32(buf + 9 + ddc * 4), PH40);
  chk("hp[329..332] DUC phase = 0",     be32(buf + 329), 0);
  chk("hp[345] drive = 0",              buf[345], 0);
  chk("hp alex0 40 m knee + ANT1",      be32(buf + 1432), alex0_40m | 0x01200000u);
  chk("hp alex1 = LPF|ANT1 (no relay)", be32(buf + 1428), 0x01200000u);
  chk("hp[1443] ADC0 attenuator",       buf[1443], 0);

  /* Park packet (run=0): both Alex words zero → antenna relay released. */
  p2_build_high_priority(buf, dev, F40, 0, NULL, NULL);
  chk("hp park: run bit clear",         buf[4], 0x00);
  chk("hp park: alex0 = 0",             be32(buf + 1432), 0);
  chk("hp park: alex1 = 0",             be32(buf + 1428), 0);
}

int main(void) {
  unsigned char buf[1500];

  printf("=== sdrfl-p2dev-test — per-device P2 byte gate (docs/RADIOS-SCOPE.md §1) ===\n");
  printf("expected values taken BY HAND from piHPSDR @974acba\n");

  /* ⛔ The two radios we live-tested: these lines are the regression guard.
   * alex0 40 m: g2class band-PASS 0x10 (40/30 m bank) vs Hermes-class HPF
   * 0x20 (6.5 MHz corner) — the one knee where the two tables disagree. */
  device_pass("ANAN G2E (piHPSDR 'G1')", NEW_DEVICE_G1,     0x01, 1, 0, 0x00000010u);
  device_pass("ANAN 10E/100B (HERMES2)", NEW_DEVICE_HERMES2, 0x01, 1, 0, 0x00000020u);

  /* The bring-up under test — no hardware here, this gate is the evidence. */
  device_pass("ANAN G2 / Saturn",        NEW_DEVICE_SATURN, 0x03, 2, 2, 0x00000010u);
  device_pass("ORION2 (7000/8000/DLE)",  NEW_DEVICE_ORION2, 0x03, 2, 2, 0x00000010u);

  /* ---- band-pass class table, Saturn (np.c:1044-1069) -------------------- */
  printf("\n[Saturn band-pass knees] alex0 RX bits vs np.c ALEX_ANAN7000_RX_*_BPF\n");
  {
    static const struct { long long f; uint32_t bpf; const char *name; } t[] = {
      {   600000LL, 0x00001000u, "MW  -> BYPASS_BPF" },
      {  1900000LL, 0x00000040u, "160 m" },
      {  3600000LL, 0x00000020u, "80/60 m" },
      {  7100000LL, 0x00000010u, "40/30 m" },
      { 14200000LL, 0x00000002u, "20/15 m" },
      { 28500000LL, 0x00000004u, "12/10 m" },
      { 50100000LL, 0x00000008u, "6 m + preamp" },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
      p2_build_high_priority(buf, NEW_DEVICE_SATURN, t[i].f, 1, NULL, NULL);
      uint32_t alex0 = be32(buf + 1432);
      chk(t[i].name, alex0 & 0x0000FFFFu, t[i].bpf);
    }
  }

  /* ---- PureSignal feedback pair: DDC1 <- pseudo-ADC n_adc ----------------
   * ⛔ PS stays LOCKED OUT for the Saturn (radio_ps_supported), and its
   * ps_setpk differs (0.6121). This only pins the byte so a future PS
   * milestone starts from the right wire, per np.c:1663. */
  printf("\n[PureSignal feedback pair] rxspec[23] = TX-DAC pseudo-ADC index\n");
  {
    p2_ps_state ps; memset(&ps, 0, sizeof ps);
    ps.enabled = 1;
    p2_build_receive_specific(buf, NEW_DEVICE_G1, 192000, &ps, 1);
    chk("G2E  DDC1 <- ADC1 (n_adc = 1)", buf[23], 1);
    chk("G2E  sync bitmap DDC1 -> DDC0", buf[1363], 0x02);
    p2_build_receive_specific(buf, NEW_DEVICE_SATURN, 192000, &ps, 1);
    chk("Saturn DDC1 <- ADC2 (n_adc = 2)", buf[23], 2);
    chk("Saturn rxspec[4] still n_adc = 2", buf[4], 2);
  }

  printf("\n=== %d checks, %d failures ===\n", g_checks, g_fail);
  if (g_fail == 0) {
    printf("PASS — Saturn/ORION2 wire bytes match piHPSDR; G2E and 10E unchanged.\n");
  } else {
    printf("FAIL — %d mismatch(es) above.\n", g_fail);
  }
  return g_fail ? 1 : 0;
}
