/*
 * sdr-for-linux — TX safety gate. See tx_gate.h.
 *
 * Mirrors the piHPSDR/Thetis safety rules (radio.c:2354-2405 mox path, band.c:683
 * TransmitAllowed, transmitter.c:775-789 SWR protection, docs/TX-SAFETY.md), but
 * with our stricter policy: on a protection trip we drop MOX (not just drive) and
 * latch until release, and we also run the Thetis open-antenna test. It computes
 * only — the F5 layer decides to send.
 */
#include <string.h>

#include "tx_gate.h"

/* Protection trip state (single control thread; no atomics). */
static int s_tripped  = 0;   /* latched: refuse keying until release */
static int s_pre_high = 0;   /* saw one over-limit reading (2-consecutive filter) */

void tx_gate_reset(void) {
  s_tripped  = 0;
  s_pre_high = 0;
}

void tx_gate_evaluate(const tx_gate_cfg *cfg, const tx_gate_in *in, tx_gate_result *out) {
  memset(&out->state, 0, sizeof(out->state));
  out->keyed   = 0;
  out->allowed = 0;
  out->tripped = s_tripped;
  out->reason  = "";

  int want = in->want_mox || in->want_tune;

  /* In-band check via the band plan (NULL = out of every amateur band). */
  int in_band = bp_band_for_freq(cfg->region, cfg->country_key, in->freq_hz, NULL, NULL) != NULL;
  int band_ok = in_band || cfg->allow_oob;

  /* Not transmitting → release: clear the protection latch, stay unkeyed. */
  if (!want) {
    s_tripped  = 0;
    s_pre_high = 0;
    out->allowed = band_ok;
    out->tripped = 0;
    out->reason  = in_band ? "" : "out of band";
    return;
  }

  /* Refuse to key out of band (unless the user explicitly allows OOB TX). */
  if (!band_ok) {
    out->allowed = 0;
    out->reason  = "out of band — TX refused";
    return;
  }
  out->allowed = 1;

  int pa_on = cfg->pa_enabled && !cfg->band_disable_pa;

  /* SWR / open-antenna protection — SUPPRESSED during TUNE (deliberate mismatch
   * into the ATU at low power). Requires two consecutive over-limit polls. */
  if (!in->want_tune && cfg->swr_protect) {
    int high_swr     = in->swr >= cfg->swr_alarm;
    int open_antenna = in->fwd_w > 10.0 && (in->fwd_w - in->rev_w) < 1.0;  /* Thetis */
    if (high_swr || open_antenna) {
      if (s_pre_high) { s_tripped = 1; }   /* second consecutive → trip + latch */
      s_pre_high = 1;
    } else {
      s_pre_high = 0;
    }
  }

  /* Latched trip: drop MOX (state stays off) and keep refusing until release. */
  if (s_tripped) {
    out->tripped = 1;
    out->reason  = "SWR/antenna protection — release to reset";
    /* carry freq/antenna/PA so an RX-side view is still consistent, but NOT keyed */
    out->state.tx_freq    = in->freq_hz;
    out->state.antenna    = cfg->antenna;
    out->state.pa_enabled = pa_on;
    out->state.in_band    = band_ok;
    return;
  }

  /* Permitted: build the keyed TX state. */
  out->state.mox        = in->want_mox;
  out->state.tune       = in->want_tune;
  out->state.pa_enabled = pa_on;
  out->state.in_band    = band_ok;
  out->state.drive      = in->want_tune ? cfg->tune_byte : cfg->drive_byte;
  out->state.tx_freq    = in->freq_hz;
  out->state.antenna    = cfg->antenna;
  out->keyed   = 1;
  out->tripped = 0;
}
