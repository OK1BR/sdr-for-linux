/*
 * ⛔ Radio whitelist — alpha policy (Richard, 2026-07-10). NON-NEGOTIABLE.
 *
 * Every radio model must be brought up and LIVE-TESTED on real hardware
 * before it is allowed to connect: the Alex antenna-relay words, PA-enable
 * and attenuator bytes are per-model, and wrong ones can physically damage
 * a PA (on RX alone the missing ANT-relay bit once cost −45 dB, c4b9243 —
 * on TX the same class of mistake burns hardware). Blocked ≠ forgotten:
 * each new radio gets its own bring-up + live test in a later version
 * (next candidate: Square SDR; ORION2 = ANAN 7000/8000/DLE 7000 rides on the
 * Saturn work below and is NOT unlocked — docs/RADIOS-SCOPE.md §6).
 *
 * ⛔ ONE sanctioned exception, Richard 2026-08-21 (docs/RADIOS-SCOPE.md
 * §PRIORITY): the ANAN G2 (Saturn) is enabled — RX, TX and PureSignal —
 * WITHOUT a live test here, because we own no G2 and the only operator who
 * has one is remote. Richard extended it from RX to TX the same day, so that
 * operator can walk the whole path inside issue #1. What makes it defensible,
 * and what must stay true for it:
 *   - NOTHING is guessed. Every per-device value comes from piHPSDR @974acba:
 *     the wire bytes (pinned by the offline gate sdrfl-p2dev-test), the PA
 *     rating, the ANAN-7000 wattmeter branch and ps_setpk 0.6121. Where
 *     upstream has no per-device answer our code is already device-neutral —
 *     the TX-specific packet has no device branch at all, in either codebase.
 *   - The radio starts SAFE. [tx-saturn] is its own config group with no
 *     saved calibration, so a first connect comes up PA off, ANT1, 1 W
 *     drive/tune and PA calibration at the conservative 53 dB default
 *     (that direction UNDER-drives) — the first keying IS the dry-key step.
 *   - Every runtime protection is model-independent and stays armed: the
 *     out-of-band gate, the SWR alarm (drive to zero on two consecutive
 *     readings) and the forced 31 dB RX attenuators during TX.
 *   - ⛔ What we CANNOT do from here is the live half of docs/TX-SAFETY.md and
 *     RADIOS-SCOPE §3 S4/S5 — dry key, 1 W, PA-calibration walk-in, SWR into a
 *     dummy load. That is DELEGATED, not waived: until the operator with the
 *     radio walks it (RADIOS-SCOPE §7), this model is "unlocked, unproven" —
 *     its wattmeter reading in particular is a piHPSDR starting value.
 *
 * Both gates use this predicate: the startup picker (row greyed out) and
 * the connect path in gui.c (covers SDRFL_RADIO_IP and "Add by IP").
 */
#ifndef SDRFL_RADIO_SUPPORT_H
#define SDRFL_RADIO_SUPPORT_H

#include "discovered.h"

/* Connect whitelist (RX). A model is added ONLY after its RX bring-up passed
 * live on real hardware (the header policy above).
 *  - NEW_DEVICE_G1 = the ANAN G2E (official name; piHPSDR's vendored
 *    discovered.h calls device id 20 "G1" — kept unmodified per policy).
 *  - NEW_DEVICE_HERMES2 = ANAN 10E/100B. RX gates (rxprobe/panprobe/
 *    audioprobe) passed live 2026-07-11; Hermes-class HPF knees verified
 *    against piHPSDR np.c default branch.
 *  - DEVICE_HERMES_LITE2 = Hermes Lite 2, Protocol 1, ⛔ RX ONLY (R1+R2
 *    gates passed live 2026-07-12, docs/P1-SCOPE.md; the P1 link module
 *    contains no TX code at all and locks the T/R relay to RX).
 *  - NEW_DEVICE_SATURN = ANAN G2 (and the G2's Saturn board; discovery maps
 *    ids 1010/1011 here). ⛔ The ONE model enabled without a live test on our
 *    own bench — see the sanctioned exception in the header.
 *    S1-S3 of docs/RADIOS-SCOPE.md §3: the P2 link is the one we already
 *    ship, the per-device bytes (Alex 0+1, n_adc, DDC2, band-pass knees) are
 *    piHPSDR's and are gated offline by sdrfl-p2dev-test. First live proof
 *    comes from the operator who has the radio. */
static inline int radio_supported(const DISCOVERED *d) {
  if (d == NULL) { return 0; }
  if (d->protocol == NEW_PROTOCOL &&
      (d->device == NEW_DEVICE_G1 || d->device == NEW_DEVICE_HERMES2 ||
       d->device == NEW_DEVICE_SATURN)) { return 1; }
  return d->protocol == ORIGINAL_PROTOCOL && d->device == DEVICE_HERMES_LITE2;
}

/* ⛔ TX whitelist — strictly narrower than the connect whitelist (Richard,
 * 2026-07-11). TX needs per-model PA calibration + the full docs/TX-SAFETY.md
 * checklist keyed live into a dummy load on that very model; until then the
 * GUI must not even start the TX runtime (tx_ready stays 0 → every keying
 * path is dead and the HP builder only ever sees tx=NULL → no MOX bit, PA
 * enable 0, zeroed TX-specific — the three no-TX guarantees hold).
 *  - G2E: full checklist keyed live 2026-07-08..10 (TX-DESIGN §7/§8).
 *  - HERMES2 (ANAN 10E): TX path audited vs piHPSDR @974acba 2026-07-11 —
 *    identical to the verified G2E wire path except the per-device profile
 *    below (10 W PA scale + Hermes wattmeter constants); enabled for the
 *    live dummy-load checklist with Richard (per-radio config starts with
 *    PA off + 1 W, so first keying is the dry-key step).
 *  - HERMES_LITE2 (P1): T1-T3 of docs/P1-TX-SCOPE.md passed offline
 *    2026-07-12 (byte gate vs piHPSDR, 48 k CFIR-off chain, thermal trip +
 *    FIFO health); enabled 2026-07-12 with Richard's consent for the T4
 *    dummy-load checklist — same safe-start rule ([tx-hl2] begins PA off +
 *    1 W, first keying is the dry-key step; PA off keeps the T/R relay in
 *    RX by wire, 0x12-C2=0x04).
 *  - SATURN (ANAN G2): enabled 2026-08-21 WITHOUT a live test here — the
 *    sanctioned exception in the header, extended from RX to TX by Richard so
 *    the remote operator can walk the whole path. The audit behind it: our TX
 *    wire is ALREADY right for this model. p2_build_transmit_specific() has
 *    no device branch, and neither does upstream's (np.c:1480-1590); the HP
 *    TX bytes (MOX, ALEX_TX_RELAY, TX LPF, ANT relay, drive, the forced 31 dB
 *    attenuators) are the G2E's and the Saturn sits in the same upstream
 *    "g2class"; general[59]=0x03 and n_adc=2 are handled per device (S2).
 *    Even the PS feedback antenna path is shared: piHPSDR adds 100 to rxant
 *    for G1 *and* SATURN (np.c:1294-1300), so the G2E path we live-verified
 *    IS the ORION2-class path. The only per-model numbers live in
 *    radio_tx_profile() below, all from piHPSDR (PA_100W radio.c:1306-1308,
 *    ANAN-7000 wattmeter branch transmitter.c:664-682).
 *    ⛔ [tx-saturn] holds NO calibration until the operator makes one: first
 *    connect is PA off, ANT1, 1 W, pa_calibration 53 dB (under-drives). The
 *    live dummy-load checklist is delegated, not waived — until it is walked,
 *    this model's wattmeter reading is a piHPSDR starting value, not proof. */
static inline int radio_tx_supported(const DISCOVERED *d) {
  if (d == NULL) { return 0; }
  if (d->protocol == NEW_PROTOCOL &&
      (d->device == NEW_DEVICE_G1 || d->device == NEW_DEVICE_HERMES2 ||
       d->device == NEW_DEVICE_SATURN)) { return 1; }
  return d->protocol == ORIGINAL_PROTOCOL && d->device == DEVICE_HERMES_LITE2;
}

/* ⛔ PureSignal whitelist — narrower again than the TX whitelist: the ANAN 10E
 * is excluded on LIVE EVIDENCE (2026-07-12, twice on that radio, Hermes fw
 * 10.3): keying with PS enabled kills the radio outright —
 * mid-TX "no packets from the radio for 3 s", then the network stack is gone
 * (no ARP) until a power cycle.
 *
 * Root cause (piHPSDR + Thetis audit, 2026-07-12; TX-DESIGN §9): NOT a
 * firmware limitation — Thetis runs P2 PS on the 10E with the byte-identical
 * feedback config and gates on exactly fw >= 10.3. The difference is
 * SEQUENCING: Thetis reconfigures the DDCs (sync + 192k) BEFORE the HP packet
 * that raises PTT, and restores the RX config BEFORE dropping PTT on unkey.
 * Our keepalive (like piHPSDR, which has no proven 10E-P2-PS either) can put
 * the MOX HP up to 200 ms before the PS RX-specific — the fw 10.3 FPGA
 * wedges on switching sync mode while already transmitting; the G2E (fw
 * 4.4-class) tolerates it. Lifting this requires the Thetis key-down/key-up
 * ordering in the TX path + a live re-test (each failed try = power cycle). */
static inline int radio_ps_supported(const DISCOVERED *d) {
  if (d == NULL) { return 0; }

  /* G2E (P2): live-verified 2026-07-11. HL2 (P1): live test 2026-07-12,
   * wire per docs/P1-TX-SCOPE.md §6.
   * ⛔ ANAN 10E/HERMES2 over P2 stays LOCKED OUT — keying with PS enabled
   * wedges fw 10.3 until a power cycle (TX-DESIGN §9, live-proven twice).
   * SATURN (ANAN G2): enabled 2026-08-21 together with TX, under the same
   * sanctioned exception. Why this is NOT the 10E situation: that lockout is
   * live-proven on Hermes fw 10.3 and is a sequencing bug in OLD firmware,
   * while piHPSDR and Thetis both run P2 PureSignal on the Saturn/ORION2
   * class as a matter of course. Its one genuinely different number,
   * ⭐ ps_setpk 0.6121 (transmitter.c:1206 — roughly double every other P2
   * radio's TX-DAC feedback peak), is carried by radio_tx_profile() below and
   * reaches WDSP through ps_configure(); shipping the generic 0.2899 here is
   * exactly the mistake upstream warns about at :1195-1201. piHPSDR's 8.5 dB
   * attenuation offset (transmitter.c:842) has NO counterpart in our code —
   * we replaced that recipe with the Thetis-style any-TX auto-attenuate
   * (feedback window 129-181, ps.c), which measures the feedback instead of
   * predicting it and is device-independent by construction.
   * ⛔ PS defaults to OFF and to att 0 in a fresh [tx-saturn]: switching it on
   * is the operator's deliberate act, AFTER the TX checklist, not before. */
  if (d->protocol == NEW_PROTOCOL &&
      (d->device == NEW_DEVICE_G1 || d->device == NEW_DEVICE_SATURN)) { return 1; }

  if (d->protocol == ORIGINAL_PROTOCOL && d->device == DEVICE_HERMES_LITE2) { return 1; }

  return 0;
}

/*
 * Per-model TX profile — everything the TX path must switch per device.
 * Values audited first-hand against piHPSDR @974acba (2026-07-11):
 *  - pa_watts: radio.c:1281-1325 pa_power (G2E PA_100W, HERMES2 PA_10W
 *    "most likely, this is an Anan-10E"). Scales the drive/tune sliders,
 *    the digi cap and the wattmeter-trim grid (radio.c:1329-1331
 *    pa_trim[i] = i * rating/10).
 *  - Wattmeter bridge calibration (transmitter.c tx_update_display):
 *    HERMES2 shares the METIS/HERMES/ANGELIA branch (:622-634): c1=3.3,
 *    c2=0.095, rc2 6m 0.5, offsets fwd 6 / rev 3. The G2E branch (:645-662)
 *    keeps OUR live-calibrated c1=5.0 (not piHPSDR's 3.3 — see tx_meter.c).
 *  - cfg_group: the config.ini group holding this radio's TX calibration
 *    ("tx" = the legacy/G2E group). ⛔ pa_cal/pa_trim/drive are PER RADIO:
 *    a 10 W calibration applied to a 100 W PA (or vice versa) yields a
 *    wildly wrong drive byte for the same watts request.
 */
typedef struct {
  double      pa_watts;              /* rated PA power (slider max, trim grid) */
  double      pacal_min;             /* pa_calibration clamp floor, dB — the
                                        max drive byte a watts request can
                                        reach. piHPSDR's 38.8 (band.c:571-577)
                                        assumes a 100 W-class chain; the 10E
                                        makes rated power only near DAC full
                                        scale (live 2026-07-12: byte 83 →
                                        ~1.5 W), i.e. true cal ≈ 29-33 dB —
                                        the floor must sit below that.        */
  double      m_c1, m_c2;            /* wattmeter: ADC volts, fwd coupler      */
  double      m_rc2_hf, m_rc2_6m;    /* wattmeter: reverse coupler HF / 6 m    */
  int         m_fwd_off, m_rev_off;  /* wattmeter: ADC pedestal offsets        */
  double      ps_setpk;              /* PureSignal SetPk (expected full-scale
                                        TX-DAC feedback envelope) default —
                                        piHPSDR transmitter.c:1203-1241: P2
                                        non-Saturn 0.2899, P1 HL2 0.2400
                                        ("measured value 0.2386")             */
  const char *cfg_group;             /* per-radio TX-cal settings group        */
} radio_tx_profile_t;

static inline const radio_tx_profile_t *radio_tx_profile(const DISCOVERED *d) {
  static const radio_tx_profile_t g2e = {   /* live-calibrated (TX-DESIGN §7) */
    100.0, 38.8, 5.0, 0.12, 0.15, 0.70, 48, 42, 0.2899, "tx"
  };
  static const radio_tx_profile_t hermes2 = {  /* ANAN 10E — piHPSDR defaults */
    10.0, 25.0, 3.3, 0.095, 0.095, 0.5, 6, 3, 0.2899, "tx-hermes2"
  };
  static const radio_tx_profile_t hl2 = {
    /* Hermes Lite 2, 5 W PA — piHPSDR transmitter.c:685-693 wattmeter branch
     * (c2=1.5, ~16× the 10E's 0.095!) + fwd/rev offsets 6/6; pa_calibration
     * default upstream is 40.5 dB ("the No. 1 problem for new HermesLite
     * users is 'no RF output'", band.c) → clamp floor 25 leaves calibration
     * room below it. All P1-TX-SCOPE §2; ps_setpk §6. */
    5.0, 25.0, 3.3, 1.5, 1.5, 1.5, 6, 6, 0.2400, "tx-hl2"
  };
  static const radio_tx_profile_t saturn = {
    /* ANAN G2 / Saturn — the per-model half of the TX unlock (2026-08-21).
     * Two jobs:
     *   1. cfg_group isolation. settings_save() writes the TX-cal keys into
     *      the connected radio's group; without an entry here a Saturn
     *      session would fall through to the G2E profile and rewrite [tx] —
     *      Richard's live-calibrated G2E calibration. Per-radio, never shared.
     *      [tx-saturn] also means a first connect starts PA off + 1 W.
     *   2. It carries the audited piHPSDR values, so the S4/S5 live checklist
     *      starts from upstream's numbers instead of a guess.
     * Values: PA_100W (radio.c:1306-1308 puts SATURN with G1); wattmeter =
     * the ANAN-7000 branch at PA_100W (transmitter.c:664-682: c1 5.0, c2 0.12,
     * rev 0.15 HF / 0.7 on 6 m, offsets fwd 32 / rev 28); ps_setpk 0.6121
     * (transmitter.c:1206 — the single largest per-device TX difference);
     * pa_calibration floor 38.8 = piHPSDR's 100 W-class clamp (band.c:571-577).
     * ⛔ Every one of these is a STARTING POINT for a live calibration on that
     * physical radio, not a shipping value (RADIOS-SCOPE §2). */
    100.0, 38.8, 5.0, 0.12, 0.15, 0.70, 32, 28, 0.6121, "tx-saturn"
  };
  if (d != NULL && d->protocol == ORIGINAL_PROTOCOL &&
      d->device == DEVICE_HERMES_LITE2) { return &hl2; }
  if (d != NULL && d->protocol == NEW_PROTOCOL &&
      d->device == NEW_DEVICE_SATURN) { return &saturn; }
  if (d != NULL && d->protocol == NEW_PROTOCOL &&
      d->device == NEW_DEVICE_HERMES2) { return &hermes2; }
  return &g2e;
}

#endif
