/*
 * sdrfl-txprobe — OFFLINE TX byte-layout gate (F1, docs/TX-DESIGN.md).
 *
 * Builds the General / High-Priority / Transmit-specific packets with a "hot" TX
 * state and asserts every TX-relevant byte against values derived BY HAND from
 * piHPSDR @974acba (new_protocol.c + alex.h) — an independent cross-check of our
 * protocol2.c builders. Also asserts the OFF state (pa=0, tx=NULL, cw=NULL)
 * reproduces the RX-only bytes with NO MOX, NO ALEX_TX_RELAY and drive 0 — the
 * F1 "RF is impossible from the live path" property.
 *
 * NO radio, NO socket — pure buffer construction. Exit 0 = every check passes.
 *
 *   Expected DUC/DDC phase for 14.200 MHz (2^32 / 122880000 = 65536/1875):
 *     floor(14200000 * 65536 / 1875) = 496325973 = 0x1D955555
 *   Alex bits (alex.h): TX_RELAY 0x08000000, TX_ANTENNA_1/2/3 0x01/02/04 000000,
 *     30/20 LPF 0x00100000; RX 20/15 BPF 0x00000002.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "discovered.h"   /* NEW_DEVICE_G1 */
#include "protocol2.h"

static int g_checks = 0;
static int g_fail = 0;

static void chk(const char *what, long got, long want) {
  g_checks++;
  if (got != want) {
    printf("  FAIL  %-38s got=0x%lx want=0x%lx\n", what,
           (unsigned long)got, (unsigned long)want);
    g_fail++;
  } else {
    printf("  ok    %-38s = 0x%lx\n", what, (unsigned long)got);
  }
}

static uint32_t be32(const unsigned char *b) {
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
         ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}

int main(void) {
  unsigned char buf[1500];
  const int dev = NEW_DEVICE_G1;
  const long long f = 14200000LL;      /* 20 m test frequency */
  const uint32_t PHASE = 0x1D955555u;  /* DUC/DDC phase for 14.200 MHz (see header) */

  /* Alex word expectations for 20 m, ANT1 (BPF 0x02 | LPF 0x00100000 | ANT1). */
  const uint32_t ALEX0_RX = 0x01100002u;              /* RX: BPF|LPF|ANT1        */
  const uint32_t ALEX1_RX = 0x01100000u;              /* RX: LPF|ANT1            */
  const uint32_t ALEX0_TX = ALEX0_RX | 0x08000000u;   /* +TX_RELAY when keyed+PA */
  const uint32_t ALEX1_TX = ALEX1_RX | 0x08000000u;

  printf("=== sdrfl-txprobe — offline TX byte-layout gate (F1) ===\n");
  printf("test freq %lld Hz (20 m); expected values by hand from piHPSDR @974acba\n\n", f);

  /* ---- General: PA-enable byte 58 -------------------------------------- */
  printf("[General] PA-enable (byte 58):\n");
  p2_build_general(buf, 1);
  chk("general[58] pa_enabled=1", buf[58], 0x01);
  chk("general[59] Alex0 enable", buf[59], 0x01);
  p2_build_general(buf, 0);
  chk("general[58] pa_enabled=0 (live)", buf[58], 0x00);

  /* ---- HP OFF (tx=NULL): the no-TX guarantee -------------------------- */
  printf("\n[HP OFF] tx=NULL — RX-only: no MOX, no TX_RELAY, drive 0:\n");
  p2_build_high_priority(buf, dev, f, 1, NULL, NULL);
  chk("hp[4] run only (no MOX)",       buf[4], 0x01);
  chk("hp[329..332] DUC phase = 0",    be32(buf + 329), 0);
  chk("hp[345] drive = 0",             buf[345], 0);
  chk("hp alex0 = RX (BPF|LPF|ANT1)",  be32(buf + 1432), ALEX0_RX);
  chk("hp alex1 = RX (LPF|ANT1)",      be32(buf + 1428), ALEX1_RX);
  chk("hp[1442] atten1 = 0",           buf[1442], 0);
  chk("hp[1443] atten0 = 0",           buf[1443], 0);

  /* ---- HP HOT: full keyed TX ----------------------------------------- */
  printf("\n[HP HOT] mox=1 pa=1 in_band drive=200 ANT1:\n");
  p2_tx_state tx = {0};
  tx.mox = 1; tx.pa_enabled = 1; tx.in_band = 1; tx.drive = 200;
  tx.tx_freq = f; tx.antenna = 0;
  p2_build_high_priority(buf, dev, f, 1, &tx, NULL);
  chk("hp[4] run+MOX (0x03)",          buf[4], 0x03);
  chk("hp[329..332] DUC phase",        be32(buf + 329), PHASE);
  chk("hp[9..12] DDC0 phase",          be32(buf + 9),   PHASE);
  chk("hp[345] drive = 200",           buf[345], 200);
  chk("hp alex0 = TX (+TX_RELAY)",     be32(buf + 1432), ALEX0_TX);
  chk("hp alex1 = TX (+TX_RELAY)",     be32(buf + 1428), ALEX1_TX);
  chk("hp[1442] atten1 = 31 (TX+PA)",  buf[1442], 31);
  chk("hp[1443] atten0 = 31 (TX+PA)",  buf[1443], 31);

  /* ---- HP HOT out-of-band: drive forced 0 (fast kill) ---------------- */
  printf("\n[HP HOT off-band] in_band=0 → drive forced 0, still keyed:\n");
  p2_tx_state txo = tx; txo.in_band = 0;
  p2_build_high_priority(buf, dev, f, 1, &txo, NULL);
  chk("hp[345] drive = 0 (off-band)",  buf[345], 0);
  chk("hp[4] still run+MOX",           buf[4], 0x03);

  /* ---- HP HOT: ANT3 + PA disabled → no TX_RELAY, atten stays RX ------ */
  printf("\n[HP HOT] antenna=ANT3, PA disabled → no TX_RELAY, atten unchanged:\n");
  p2_tx_state txa = tx; txa.antenna = 2; txa.pa_enabled = 0;
  p2_build_high_priority(buf, dev, f, 1, &txa, NULL);
  chk("hp alex0 = ANT3, no relay",     be32(buf + 1432), 0x04100002u);
  chk("hp alex1 = ANT3, no relay",     be32(buf + 1428), 0x04100000u);
  chk("hp[1442] atten1 = 0 (PA off)",  buf[1442], 0);
  chk("hp[1443] atten0 = 0 (PA off)",  buf[1443], 0);

  /* ---- TX-specific OFF (cw=NULL): all-zero, keyer disabled ----------- */
  printf("\n[TXspec OFF] cw=NULL — all-zero (byte5=0 → FPGA CW keyer disabled):\n");
  p2_build_transmit_specific(buf, NULL, NULL);
  chk("txspec[4] nDAC = 0",   buf[4], 0);
  chk("txspec[5] CW cfg = 0", buf[5], 0);
  chk("txspec[50] mic cfg = 0", buf[50], 0);
  chk("txspec[58] atten = 0", buf[58], 0);

  /* ---- TX-specific HOT: CW keyer + mic config ------------------------ */
  printf("\n[TXspec HOT] internal keyer(modeB)+sidetone+breakin, mic boost, PA on:\n");
  p2_tx_cw cw = {0};
  cw.pa_enabled = 1;
  cw.cw_internal = 1; cw.cw_mode_b = 1; cw.cw_sidetone_on = 1; cw.cw_breakin = 1;
  cw.cw_sidetone_vol = 100; cw.cw_sidetone_freq = 600;
  cw.cw_speed = 25; cw.cw_weight = 55; cw.cw_hang_ms = 300;
  cw.cw_ptt_delay = 20; cw.cw_ramp_width = 9;
  cw.mic_boost = 1; cw.linein_gain_db = 0;
  p2_build_transmit_specific(buf, &cw, NULL);
  chk("txspec[4] nDAC = 1",   buf[4], 1);
  chk("txspec[5] CW cfg",     buf[5], 0x02 | 0x28 | 0x10 | 0x80);  /* = 0xBA */
  chk("txspec[6] sidetone vol", buf[6], 100);
  chk("txspec[7..8] sidetone freq", (buf[7] << 8) | buf[8], 600);
  chk("txspec[9] speed",      buf[9], 25);
  chk("txspec[10] weight",    buf[10], 55);
  chk("txspec[11..12] hang",  (buf[11] << 8) | buf[12], 300);
  chk("txspec[13] ptt delay", buf[13], 20);
  chk("txspec[17] ramp width", buf[17], 9);
  chk("txspec[50] mic (boost)", buf[50], 0x02);
  chk("txspec[51] linein gain", buf[51], (int)((0 + 34.0) * 0.6739 + 0.5));  /* 23 */
  chk("txspec[58] atten1 = 31 (PA)", buf[58], 31);
  chk("txspec[59] atten0 = 31 (PA)", buf[59], 31);

  /* ==== PureSignal (PS-1, docs/PS-SCOPE.md) — expected values by hand from
   *      piHPSDR @974acba np.c:1649-1668 (rx-spec), 871-883 + 1034-1038 +
   *      1584-1586 (HP/tx-spec), alex.h:94 (ALEX_PS_BIT 0x00040000). ====== */

  /* ---- PS OFF regression: ps=NULL vs enabled=0 → byte-identical ------- */
  printf("\n[PS OFF] enabled=0 must be byte-identical to ps=NULL (all 3 packets):\n");
  unsigned char ref[1500];
  p2_ps_state ps0 = {0};              /* enabled = 0 */
  p2_build_high_priority(ref, dev, f, 1, &tx, NULL);
  p2_build_high_priority(buf, dev, f, 1, &tx, &ps0);
  chk("hp: memcmp(NULL, {0}) == 0", memcmp(ref, buf, 1444), 0);
  p2_build_receive_specific(ref, dev, 1536000, NULL, 0);
  p2_build_receive_specific(buf, dev, 1536000, &ps0, 1);
  chk("rxspec: memcmp(NULL, {0}) == 0", memcmp(ref, buf, 1444), 0);
  p2_build_transmit_specific(ref, &cw, NULL);
  p2_build_transmit_specific(buf, &cw, &ps0);
  chk("txspec: memcmp(NULL, {0}) == 0", memcmp(ref, buf, 60), 0);

  /* ---- RX-specific: PS enabled but NOT keyed → normal packet ---------- */
  printf("\n[RXspec PS+RX] enabled=1 xmit=0 → normal RX config, untouched:\n");
  p2_ps_state ps = {0};
  ps.enabled = 1; ps.attenuation = 12; ps.feedback_ant = 0;
  p2_build_receive_specific(ref, dev, 1536000, NULL, 0);
  p2_build_receive_specific(buf, dev, 1536000, &ps, 0);
  chk("rxspec: PS on + RX == normal", memcmp(ref, buf, 1444), 0);

  /* ---- RX-specific: PS-TX feedback pair (np.c:1649-1668) -------------- */
  printf("\n[RXspec PS-TX] DDC0=RX-fb(ADC0) + DDC1=TX-DAC(pseudo-ADC 1), 192k, synced:\n");
  p2_build_receive_specific(buf, dev, 1536000, &ps, 1);
  chk("rxspec[7] enable = DDC0 only",  buf[7],  0x01);
  chk("rxspec[17] DDC0 <- ADC0",       buf[17], 0);
  chk("rxspec[18..19] DDC0 rate 192k", (buf[18] << 8) | buf[19], 192);
  chk("rxspec[22] DDC0 24 bit",        buf[22], 24);
  chk("rxspec[23] DDC1 <- ADC1 (DAC)", buf[23], 1);
  chk("rxspec[24..25] DDC1 rate 192k", (buf[24] << 8) | buf[25], 192);
  chk("rxspec[26] DDC1 24 bit",        buf[26], 24);
  chk("rxspec[1363] sync DDC1->DDC0",  buf[1363], 0x02);

  /* ---- HP: PS enabled, NOT keyed → only ALEX_PS_BIT in alex1 ---------- */
  printf("\n[HP PS+RX] tx=NULL ps=on → alex1 gets PS bit, all else RX bytes:\n");
  p2_build_high_priority(buf, dev, f, 1, NULL, &ps);
  chk("hp[4] run only (no MOX)",       buf[4], 0x01);
  chk("hp[9..12] DDC0 = RX phase",     be32(buf + 9), PHASE);
  chk("hp[13..16] DDC1 untouched (0)", be32(buf + 13), 0);
  chk("hp alex0 = RX (no PS bit)",     be32(buf + 1432), ALEX0_RX);
  chk("hp alex1 = RX + ALEX_PS_BIT",   be32(buf + 1428), ALEX1_RX | 0x00040000u);
  chk("hp[1443] atten0 = RX value",    buf[1443], 0);

  /* ---- HP: PS-TX — NCOs on DUC, PS bits, delta-#1 attenuator ---------- */
  printf("\n[HP PS-TX] mox+PA+PS att=12: DDC0+DDC1=DUC, PS bits, atten0=12/atten1=31:\n");
  p2_build_high_priority(buf, dev, f, 1, &tx, &ps);
  chk("hp[4] run+MOX",                 buf[4], 0x03);
  chk("hp[9..12] DDC0 = DUC phase",    be32(buf + 9),  PHASE);
  chk("hp[13..16] DDC1 = DUC phase",   be32(buf + 13), PHASE);
  chk("hp[329..332] DUC phase",        be32(buf + 329), PHASE);
  chk("hp alex0 = TX + PS bit",        be32(buf + 1432), ALEX0_TX | 0x00040000u);
  chk("hp alex1 = TX + PS bit",        be32(buf + 1428), ALEX1_TX | 0x00040000u);
  chk("hp[1442] atten1 = 31 (stays)",  buf[1442], 31);
  chk("hp[1443] atten0 = 12 (PS, delta #1)", buf[1443], 12);

  /* ---- HP: PS-TX with BYPASS feedback ant ----------------------------- */
  printf("\n[HP PS-TX bypass] feedback_ant=7 → +ALEX_RX_ANTENNA_BYPASS in alex0:\n");
  p2_ps_state psb = ps; psb.feedback_ant = 7;
  p2_build_high_priority(buf, dev, f, 1, &tx, &psb);
  chk("hp alex0 = TX+PS+BYPASS", be32(buf + 1432),
      ALEX0_TX | 0x00040000u | 0x00000800u);

  /* ---- TX-specific: PS attenuator (np.c:1584-1586) --------------------- */
  printf("\n[TXspec PS] PA on + PS att=12 → [59]=12 (ADC0), [58]=31 (ADC1):\n");
  p2_build_transmit_specific(buf, &cw, &ps);
  chk("txspec[58] atten1 = 31",        buf[58], 31);
  chk("txspec[59] atten0 = 12 (PS)",   buf[59], 12);

  /* ---- PS feedback de-interleave (np.c:2554-2571) ---------------------- */
  printf("\n[PS decode] synthetic synced packet: pair order + 1/2^23 scale:\n");
  {
    unsigned char pkt[16 + 4 * 6] = {0};
    pkt[14] = 0; pkt[15] = 4;          /* 4 samples total = 2 pairs           */
    /* pair 0: DDC0 (rxfb) I=+0.5 (0x400000) Q=-0.5 (0xC00000);
     *         DDC1 (txfb) I=+0.25 (0x200000) Q=0 */
    pkt[16] = 0x40; pkt[19] = 0xC0; pkt[22] = 0x20;
    /* pair 1: DDC0 I=0 Q=0; DDC1 I=0 Q=+0.5 */
    pkt[16 + 12 + 9] = 0x40;
    double txfb[8], rxfb[8];
    int pairs = p2_ps_decode(pkt, (int)sizeof(pkt), txfb, rxfb, 8);
    chk("pairs = 2",                    pairs, 2);
    chk("rxfb[0] I = +0.5  (x1000)",    (long)(rxfb[0] * 1000 + 0.5), 500);
    chk("rxfb[0] Q = -0.5  (x1000)",    (long)(rxfb[1] * 1000 - 0.5), -500);
    chk("txfb[0] I = +0.25 (x1000)",    (long)(txfb[0] * 1000 + 0.5), 250);
    chk("txfb[1] Q = +0.5  (x1000)",    (long)(txfb[3] * 1000 + 0.5), 500);
    chk("rxfb[1] I = 0",                (long)(rxfb[2] * 1000), 0);
  }

  printf("\n=== %d checks, %d failures ===\n", g_checks, g_fail);
  if (g_fail == 0) {
    printf("PASS — TX byte layout matches piHPSDR; OFF state carries no MOX / no "
           "TX_RELAY / drive 0.\n");
  } else {
    printf("FAIL — %d mismatch(es) above.\n", g_fail);
  }
  return g_fail ? 1 : 0;
}
