/*
 * sdr-for-linux — TX safety gate. See tx_gate.h.
 *
 * Mirrors the piHPSDR/Thetis safety rules (radio.c:2354-2405 mox path, band.c:683
 * TransmitAllowed, transmitter.c:775-789 SWR protection, docs/TX-SAFETY.md), but
 * with our stricter policy: on a protection trip we drop MOX (not just drive) and
 * latch until release, and we also run the Thetis open-antenna test. It computes
 * only — the F5 layer decides to send.
 */
#include <math.h>
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
  out->keyed    = 0;
  out->allowed  = 0;
  out->tripped  = s_tripped;
  out->high_swr = 0;
  out->reason   = "";

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

  /* SWR / open-antenna protection. Requires two consecutive over-limit polls.
   *  - open antenna ALWAYS trips (incl. TUNE): keying into ~nothing is never
   *    legitimate, and TUNE can now run to full power — an unprotected full-power
   *    carrier into an open port is exactly what would cook the PA.
   *  - high SWR trips under MOX only; during TUNE it does NOT trip (you must be
   *    able to tune an ATU through a deliberate mismatch) but is still flagged so
   *    the operator sees it. */
  if (cfg->swr_protect) {
    int high_swr     = in->swr >= cfg->swr_alarm;
    /* Thetis open-antenna test, scaled to the PA rating: the original 10 W /
     * 1 W constants are for a 100 W PA (console.cs:25968) — on a 10 W ANAN
     * 10E they would never arm at legal power. 10 % / 1 % of the rating keeps
     * the G2E behaviour bit-identical (10.0 / 1.0). */
    double rating    = cfg->pa_watts > 0.0 ? cfg->pa_watts : 100.0;
    int open_antenna = in->fwd_w > 0.10 * rating &&
                       (in->fwd_w - in->rev_w) < 0.01 * rating;
    out->high_swr    = high_swr;                       /* indicator: TUNE + MOX */
    /* The 2-consecutive filter advances on GENUINE coupler readings only.
     * Edge-triggered gate runs re-evaluate the last meter state; letting the
     * duplicate count would trip off a single physical sample (piHPSDR
     * requires two real polls, transmitter.c:775-789). */
    if (!in->stale_reading) {
      int trip_now = open_antenna || (high_swr && !in->want_tune);
      if (trip_now) {
        if (s_pre_high) { s_tripped = 1; }   /* second consecutive → trip + latch */
        s_pre_high = 1;
      } else {
        s_pre_high = 0;
      }
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

int tx_calc_drive_byte(double watts, double pa_calibration) {
  if (watts <= 0.0) { return 0; }
  double target_dbm   = 10.0 * log10(watts * 1000.0) - pa_calibration;
  double target_volts = sqrt(pow(10.0, target_dbm * 0.1) * 0.05);
  double volts        = target_volts / 0.8;
  if (volts > 1.0) { volts = 1.0; }
  double actual = volts * (1.0 / 0.98);
  if (actual < 0.0) { actual = 0.0; }
  if (actual > 1.0) { actual = 1.0; }
  int level = (int)(actual * 255.0);
  if (level < 0)   { level = 0; }
  if (level > 255) { level = 255; }
  return level;
}
