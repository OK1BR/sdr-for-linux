/*
 * ⛔ Radio whitelist — alpha policy (Richard, 2026-07-10). NON-NEGOTIABLE.
 *
 * Every radio model must be brought up and LIVE-TESTED on real hardware
 * before it is allowed to connect: the Alex antenna-relay words, PA-enable
 * and attenuator bytes are per-model, and wrong ones can physically damage
 * a PA (on RX alone the missing ANT-relay bit once cost −45 dB, c4b9243 —
 * on TX the same class of mistake burns hardware). Blocked ≠ forgotten:
 * each new radio gets its own bring-up + live test in a later version
 * (next candidate: Square SDR).
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
 *    contains no TX code at all and locks the T/R relay to RX). */
static inline int radio_supported(const DISCOVERED *d) {
  if (d == NULL) { return 0; }
  if (d->protocol == NEW_PROTOCOL &&
      (d->device == NEW_DEVICE_G1 || d->device == NEW_DEVICE_HERMES2)) { return 1; }
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
 *    PA off + 1 W, so first keying is the dry-key step). */
static inline int radio_tx_supported(const DISCOVERED *d) {
  return d != NULL && d->protocol == NEW_PROTOCOL &&
         (d->device == NEW_DEVICE_G1 || d->device == NEW_DEVICE_HERMES2);
}

/* ⛔ PureSignal whitelist — G2E only. LIVE EVIDENCE (2026-07-12, twice on the
 * ANAN 10E, Hermes fw 10.3): keying with PS enabled kills the radio outright —
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
  return d != NULL && d->protocol == NEW_PROTOCOL && d->device == NEW_DEVICE_G1;
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
  const char *cfg_group;             /* per-radio TX-cal settings group        */
} radio_tx_profile_t;

static inline const radio_tx_profile_t *radio_tx_profile(const DISCOVERED *d) {
  static const radio_tx_profile_t g2e = {   /* live-calibrated (TX-DESIGN §7) */
    100.0, 38.8, 5.0, 0.12, 0.15, 0.70, 48, 42, "tx"
  };
  static const radio_tx_profile_t hermes2 = {  /* ANAN 10E — piHPSDR defaults */
    10.0, 25.0, 3.3, 0.095, 0.095, 0.5, 6, 3, "tx-hermes2"
  };
  if (d != NULL && d->device == NEW_DEVICE_HERMES2) { return &hermes2; }
  return &g2e;
}

#endif
