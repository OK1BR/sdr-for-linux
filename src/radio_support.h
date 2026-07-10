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

static inline int radio_supported(const DISCOVERED *d) {
  return d != NULL && d->protocol == NEW_PROTOCOL && d->device == NEW_DEVICE_G1;
}

#endif
