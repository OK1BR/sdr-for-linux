/*
 * ⛔ Radio whitelist — alpha policy (Richard, 2026-07-10). NON-NEGOTIABLE.
 *
 * Every radio model must be brought up and LIVE-TESTED on real hardware
 * before it is allowed to connect: the Alex antenna-relay words, PA-enable
 * and attenuator bytes are per-model, and wrong ones can physically damage
 * a PA (on RX alone the missing ANT-relay bit once cost −45 dB, c4b9243 —
 * on TX the same class of mistake burns hardware). Blocked ≠ forgotten:
 * each new radio gets its own bring-up + live test in a later version
 * (next candidates: ANAN 10E, Hermes Lite 2 — the HL2 speaks Protocol 1
 * only, which we do not implement yet at all).
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
 *    against piHPSDR np.c default branch. RX-ONLY — see radio_tx_supported. */
static inline int radio_supported(const DISCOVERED *d) {
  return d != NULL && d->protocol == NEW_PROTOCOL &&
         (d->device == NEW_DEVICE_G1 || d->device == NEW_DEVICE_HERMES2);
}

/* ⛔ TX whitelist — strictly narrower than the connect whitelist (Richard,
 * 2026-07-11). TX needs per-model PA calibration + the full docs/TX-SAFETY.md
 * checklist keyed live into a dummy load on that very model; until then the
 * GUI must not even start the TX runtime (tx_ready stays 0 → every keying
 * path is dead and the HP builder only ever sees tx=NULL → no MOX bit, PA
 * enable 0, zeroed TX-specific — the three no-TX guarantees hold). */
static inline int radio_tx_supported(const DISCOVERED *d) {
  return d != NULL && d->protocol == NEW_PROTOCOL && d->device == NEW_DEVICE_G1;
}

#endif
