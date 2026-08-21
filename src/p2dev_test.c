/*
 * sdrfl-p2dev-test — OFFLINE per-device Protocol-2 byte gate (S2,
 * docs/RADIOS-SCOPE.md).
 *
 * The ANAN G2 (Saturn) bring-up cannot be tested on hardware here — we own a
 * G2E, not a G2, and that model is unlocked for RX *and* TX on an audit alone
 * (the sanctioned exception in src/radio_support.h). This gate is therefore
 * the only executable verification the model gets before a real operator keys
 * it, and it asserts:
 *
 *   1. SATURN / ORION2 get the bytes piHPSDR @974acba sends them:
 *        general[59] = 0x03   two Alex boards enabled   (np.c:693-697)
 *        rxspec[4]   = 2      two ADCs                  (radio.c:1541-1587)
 *        rxspec[7]   = 0x04   the RX DDC is DDC2        (np.c:1627-1631)
 *        HP  [9..12] AND [17..20] carry the RX phase    (np.c:816-835)
 *        HP  alex0 band-PASS knees (the "g2class" table, np.c:1044-1069)
 *   1b. The keyed Saturn packets: MOX, drive, the Alex TX words (BPF | the
 *       right LPF bank | ANT relay | TX relay), both step attenuators forced
 *       to 31 dB, general[59] STILL 0x03 while transmitting (the TX LPF sits
 *       on Alex 1 on this class), the off-band drive kill and the park packet.
 *   1c. The whitelists themselves (who may connect / key / run PureSignal)
 *       and the Saturn's per-model TX profile — including that it keeps its
 *       own [tx-saturn] config group so it cannot overwrite the G2E's [tx].
 *   2. The two P2 radios we DO support are byte-identical to before this
 *      change. Not by sampling: for each device it ENUMERATES every byte the
 *      RX build may set (General, RX-specific, High-Priority) and asserts that
 *      everything else in the 1444-byte packet is zero. The real regression
 *      risk of the Saturn work is the G2E and the ANAN 10E, not the radio we
 *      cannot try. (The P1/HL2 path is untouched here — sdrfl-p1txprobe.)
 *
 * NO radio, NO socket — pure buffer construction. Exit 0 = every check passes.
 *
 * Phase words (2^32 / 122880000 = 65536/1875):
 *   7.100 MHz -> 0x0ECAAAAA,  14.200 MHz -> 0x1D955555
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "discovered.h"     /* NEW_DEVICE_G1 / _HERMES2 / _SATURN / _ORION2 */
#include "protocol2.h"
#include "radio_support.h"  /* the whitelists + per-model TX profile under test */

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
  {
    /* Enumerate EVERY byte the RX build may set, so "unchanged" is literal:
     * anything outside this list must be zero. */
    const int allow[] = { 4, 5, 6, 7, 17 + ddc * 6, 18 + ddc * 6,
                          19 + ddc * 6, 20 + ddc * 6, 21 + ddc * 6, 22 + ddc * 6 };
    only_these_nonzero("rxspec: no byte outside the DDC block",
                       buf, 1444, allow, (int)(sizeof allow / sizeof allow[0]));
  }
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
  {
    /* Same enumeration for the High-Priority packet: run bit, the DDC0 phase,
     * the real DDC slot, and the two Alex words — nothing else. */
    int allow[24], n = 0;   /* 1 run + 4 DDC0 + 4 DDC-slot + 4 + 4 alex = 17 */
    allow[n++] = 4;
    for (int i = 0; i < 4; i++) { allow[n++] = 9 + i; }
    for (int i = 0; i < 4; i++) { allow[n++] = 9 + ddc * 4 + i; }
    for (int i = 0; i < 4; i++) { allow[n++] = 1428 + i; }
    for (int i = 0; i < 4; i++) { allow[n++] = 1432 + i; }
    only_these_nonzero("hp: no byte outside run/phase/alex", buf, 1444, allow, n);
  }

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

  /* ---- Saturn TX: the keyed packets ------------------------------------
   * ⛔ The G2 is TX-unlocked without a live test here (radio_support.h header),
   * so these bytes are the only pre-flight the TX path gets on this model.
   * Values by hand from piHPSDR (alex.h + np.c:1244-1266), 40 m / ANT1 /
   * drive 200: alex0 TX = BPF 40/30 m 0x10 | ALEX_60_40_LPF 0x00200000
   * (7.1 MHz is in the >5 M..8 M bank!) | ALEX_TX_ANTENNA_1 0x01000000 |
   * ALEX_TX_RELAY 0x08000000; alex1 is the TX-case word, so no BPF.
   * G7 confirmed while writing this: for the G1/ORION2/SATURN class upstream
   * skips the "RX uses the TX LPF" branch entirely (np.c:1224-1226), which is
   * what our builder does by always taking the LPF from tx_freq. */
  printf("\n[Saturn TX] keyed High-Priority + General (device %d)\n", NEW_DEVICE_SATURN);
  {
    const long long F40 = 7100000LL;
    p2_tx_state tx; memset(&tx, 0, sizeof tx);
    tx.mox = 1; tx.pa_enabled = 1; tx.in_band = 1; tx.drive = 200;
    tx.tx_freq = F40; tx.antenna = 0;
    p2_build_high_priority(buf, NEW_DEVICE_SATURN, F40, 1, &tx, NULL);
    chk("hp[4] run + MOX",                buf[4], 0x03);
    chk("hp[345] drive",                  buf[345], 200);
    chk("hp[329..332] DUC phase",         be32(buf + 329), 0x0ECAAAAAu);
    chk("hp alex0 TX = BPF|LPF|ANT1|RLY", be32(buf + 1432), 0x09200010u);
    chk("hp alex1 TX = LPF|ANT1|RELAY",   be32(buf + 1428), 0x09200000u);
    chk("hp[1442] ADC1 att = 31 (TX+PA)", buf[1442], 31);
    chk("hp[1443] ADC0 att = 31 (TX+PA)", buf[1443], 31);
    /* ⛔ The General packet must STILL enable both Alex boards while keyed —
     * the TX LPF sits on Alex 1 on this class, so a 0x01 here would key into
     * an unswitched filter board. */
    p2_build_general(buf, NEW_DEVICE_SATURN, 1);
    chk("general[58] PA enable = 1",       buf[58], 0x01);
    chk("general[59] STILL Alex 0+1 on TX", buf[59], 0x03);
    /* Off-band kill and the park packet are device-independent, but assert
     * them here too: this radio is unlocked sight-unseen. */
    p2_tx_state txo = tx; txo.in_band = 0;
    p2_build_high_priority(buf, NEW_DEVICE_SATURN, F40, 1, &txo, NULL);
    chk("hp off-band: drive forced 0",    buf[345], 0);
    chk("hp off-band: still keyed",       buf[4], 0x03);
    p2_build_high_priority(buf, NEW_DEVICE_SATURN, F40, 0, &tx, NULL);
    chk("hp park cannot carry MOX",       buf[4], 0x00);
    chk("hp park: alex0 released",        be32(buf + 1432), 0);
  }

  /* ---- PureSignal feedback pair: DDC1 <- pseudo-ADC n_adc --------------- */
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

  /* ---- whitelists + per-model TX profile (src/radio_support.h) -----------
   * A tripwire, not a tautology: these are the values a wrong edit would
   * silently change — an unlisted radio slipping into a whitelist, or the
   * Saturn losing its own config group and writing into the G2E's [tx]. */
  printf("\n[whitelists] radio_support.h — who may connect, key, and run PS\n");
  {
    DISCOVERED d;
    struct { int proto, dev; int conn, tx, ps; const char *name; } w[] = {
      { NEW_PROTOCOL,      NEW_DEVICE_G1,       1, 1, 1, "ANAN G2E" },
      { NEW_PROTOCOL,      NEW_DEVICE_HERMES2,  1, 1, 0, "ANAN 10E (PS wedges fw 10.3)" },
      { NEW_PROTOCOL,      NEW_DEVICE_SATURN,   1, 1, 1, "ANAN G2 / Saturn" },
      { NEW_PROTOCOL,      NEW_DEVICE_ORION2,   0, 0, 0, "ORION2 — NOT unlocked" },
      { NEW_PROTOCOL,      NEW_DEVICE_ORION,    0, 0, 0, "Orion — NOT unlocked" },
      { ORIGINAL_PROTOCOL, DEVICE_HERMES_LITE2, 1, 1, 1, "Hermes Lite 2 (P1)" },
      { ORIGINAL_PROTOCOL, DEVICE_HERMES,       0, 0, 0, "Hermes (P1) — NOT unlocked" },
    };
    for (unsigned i = 0; i < sizeof(w) / sizeof(w[0]); i++) {
      char lbl[80];
      memset(&d, 0, sizeof d);
      d.protocol = w[i].proto; d.device = w[i].dev;
      snprintf(lbl, sizeof lbl, "%s: connect", w[i].name);
      chk(lbl, radio_supported(&d) ? 1 : 0, w[i].conn);
      snprintf(lbl, sizeof lbl, "%s: TX", w[i].name);
      chk(lbl, radio_tx_supported(&d) ? 1 : 0, w[i].tx);
      snprintf(lbl, sizeof lbl, "%s: PureSignal", w[i].name);
      chk(lbl, radio_ps_supported(&d) ? 1 : 0, w[i].ps);
    }
    chk("NULL device refused (connect)", radio_supported(NULL) ? 1 : 0, 0);
    chk("NULL device refused (TX)",      radio_tx_supported(NULL) ? 1 : 0, 0);
  }

  printf("\n[TX profile] Saturn per-model numbers vs piHPSDR\n");
  {
    DISCOVERED d; memset(&d, 0, sizeof d);
    d.protocol = NEW_PROTOCOL; d.device = NEW_DEVICE_SATURN;
    const radio_tx_profile_t *p = radio_tx_profile(&d);
    chk("PA rating 100 W (radio.c:1306)",     (long)(p->pa_watts + 0.5), 100);
    chk("pa_cal floor 38.8 (band.c:571)",     (long)(p->pacal_min * 10 + 0.5), 388);
    chk("wattmeter c1 = 5.0  (tx.c:667)",     (long)(p->m_c1 * 100 + 0.5), 500);
    chk("wattmeter c2 = 0.12 (ANAN-7000)",    (long)(p->m_c2 * 100 + 0.5), 12);
    chk("reverse HF 0.15",                    (long)(p->m_rc2_hf * 100 + 0.5), 15);
    chk("reverse 6 m 0.70",                   (long)(p->m_rc2_6m * 100 + 0.5), 70);
    chk("fwd offset 32",                      p->m_fwd_off, 32);
    chk("rev offset 28",                      p->m_rev_off, 28);
    chk("⭐ ps_setpk 0.6121 (NOT 0.2899)",    (long)(p->ps_setpk * 10000 + 0.5), 6121);
    chk("config group is its own",            strcmp(p->cfg_group, "tx-saturn") == 0, 1);
    /* ⛔ The leak this guards: a Saturn session must never write TX-cal keys
     * into the G2E's live-calibrated [tx] group. */
    d.device = NEW_DEVICE_G1;
    chk("G2E still owns [tx]", strcmp(radio_tx_profile(&d)->cfg_group, "tx") == 0, 1);
    chk("G2E ps_setpk still 0.2899",
        (long)(radio_tx_profile(&d)->ps_setpk * 10000 + 0.5), 2899);
  }

  printf("\n=== %d checks, %d failures ===\n", g_checks, g_fail);
  if (g_fail == 0) {
    printf("PASS — Saturn/ORION2 wire bytes match piHPSDR; G2E and 10E unchanged.\n");
  } else {
    printf("FAIL — %d mismatch(es) above.\n", g_fail);
  }
  return g_fail ? 1 : 0;
}
