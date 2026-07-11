/*
 * sdr-for-linux — TX safety gate (F4, docs/TX-DESIGN.md).
 *
 * The decision brain between "operator asks to transmit" and "these are the exact
 * bytes we'd send". Given the request (MOX/TUNE), the frequency, the PA/antenna
 * config and the live SWR/power, it returns the p2_tx_state to send AND whether TX
 * is permitted. It enforces every safety rule from docs/TX-SAFETY.md:
 *   - in-band lockout (via the band plan) — refuse out of band unless allow_oob;
 *   - PA gate — TX_RELAY / atten-31 only with the PA enabled (set in the state);
 *   - SWR shutdown — SWR >= alarm on TWO consecutive polls → drop MOX + drive 0,
 *     latched until release. Trips under MOX; during TUNE high SWR does NOT trip
 *     (deliberate ATU mismatch tuning) but IS still flagged (out->high_swr);
 *   - open-antenna detection (Thetis) — fwd > 10 W && fwd-rev < 1 W → same trip,
 *     ALWAYS active, incl. TUNE (you never legitimately key into an open antenna,
 *     and TUNE can now run to full power);
 *   - separate MOX vs TUNE drive.
 *
 * ⛔ F4: this only DECIDES — it does not send. Nothing wires tx_gate_evaluate()'s
 * result into a live packet until the F5 keying milestone, so no RF is produced.
 *
 * Drive is a raw 0-255 exciter byte, NOT watts: with an uncalibrated
 * pa_calibration piHPSDR's calcLevel() saturates to 255 (full power) for any
 * sane watt value, so the safe bring-up is to dial a small raw byte, key into a
 * dummy load + wattmeter, and measure the curve (that measurement IS the F6
 * calibration). The watts<->byte UI arrives at F6.
 */
#ifndef SDRFL_ENGINE_TX_GATE_H
#define SDRFL_ENGINE_TX_GATE_H

#include "protocol2.h"   /* p2_tx_state */
#include "bandplan.h"    /* bp_region_t */

/* Operator/config side — the persisted TX settings feed this at F5. */
typedef struct {
  int         pa_enabled;      /* global PA-enable setting                        */
  int         band_disable_pa; /* per-band disablePA (PA off for this band)       */
  int         antenna;         /* 0/1/2 → ANT1/2/3                                */
  int         drive_byte;      /* MOX/voice exciter drive, 0-255 (raw until F6)   */
  int         tune_byte;       /* separate TUNE exciter drive, 0-255 (low default) */
  int         swr_protect;     /* SWR/open-ant protection enabled (our default: 1) */
  double      swr_alarm;       /* SWR trip threshold (default 3.0)                */
  int         allow_oob;       /* allow out-of-band TX (default 0)                */
  bp_region_t region;          /* band-plan region for the in-band check          */
  const char *country_key;     /* "" / "CZ" / "US" national overrides             */
} tx_gate_cfg;

/* Live inputs each evaluation (control + tx_meter). */
typedef struct {
  int       want_mox;   /* operator/PTT requests MOX      */
  int       want_tune;  /* TUNE requested                 */
  long long freq_hz;    /* current TX frequency           */
  double    swr;        /* smoothed SWR (tx_meter)        */
  double    fwd_w;      /* forward power, W (tx_meter)    */
  double    rev_w;      /* reverse power, W (tx_meter)    */
  int       stale_reading; /* 1 = evaluation WITHOUT a fresh coupler poll (an
                              edge-triggered gate run re-uses the last meter
                              state): the SWR 2-consecutive-readings filter
                              must ignore it — a duplicate of one bad sample
                              must not count as the second reading. 0 (the
                              default) = genuine fresh poll, filter advances. */
} tx_gate_in;

typedef struct {
  p2_tx_state state;    /* the TX state to send (all-off when not keyed)          */
  int         keyed;    /* 1 = exciter should be keyed (MOX/TUNE)                 */
  int         allowed;  /* 1 = TX permitted here (in band / not refused)          */
  int         tripped;  /* 1 = a protection trip is latched                       */
  int         high_swr; /* 1 = SWR >= alarm right now (indicator; lights in TUNE too) */
  const char *reason;   /* "" or why refused/tripped (for the UI/log)            */
} tx_gate_result;

/* Clear the protection latch + SWR history. Call on unkey / RX->TX entry. */
void tx_gate_reset(void);

/* Evaluate one control tick. Pure decision — no side effects beyond the internal
 * trip latch. Writes the result into *out. */
void tx_gate_evaluate(const tx_gate_cfg *cfg, const tx_gate_in *in, tx_gate_result *out);

/*
 * Convert a requested power (watts) to the 0-255 exciter drive byte, using
 * piHPSDR's calcLevel (radio.c:2879) with the per-band PA calibration (dB). This
 * RESPECTS the PA gain: with the G1 default pa_calibration = 53 dB, rated 100 W is
 * only byte ~51, so a raw byte near 255 would massively overdrive (and destroy)
 * the PA. Always drive the PA through this, never a raw byte guess. Returns 0 for
 * watts <= 0; result clamped to [0,255].
 */
int tx_calc_drive_byte(double watts, double pa_calibration);

#endif /* SDRFL_ENGINE_TX_GATE_H */
