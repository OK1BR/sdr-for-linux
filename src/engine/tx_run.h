/*
 * sdr-for-linux — TX runtime (F6a, docs/TX-DESIGN.md).
 *
 * The bridge that makes the GUI app TX-capable. Owns a dedicated worker thread
 * that runs the exact keying loop proven live in sdrfl-txkey (F5): feed the WDSP
 * TX channel in real time → 24-bit IQ → framer → port 1029, poll fwd/rev, run
 * tx_meter + tx_gate at ~20 Hz, and drive p2_set_tx_state / NULL. The operator
 * only expresses intent (tx_run_request); the safety gate decides whether that
 * intent actually keys, exactly as in txkey.
 *
 * WHY A DEDICATED THREAD (safety): the GUI tick is a GdkFrameClock callback and
 * STOPS when the window is occluded/minimised. Hanging the SWR/open-antenna
 * protection off it would let the keepalive keep MOX asserted while the guard
 * slept. Here the guard + the real-time IQ feed live on their own thread, so
 * protection runs regardless of drawing. All control-packet sends still happen
 * only on the engine's single keepalive thread (p2_set_tx_state just stores the
 * state) — we never introduce send-side concurrency for the HP/General packets.
 *
 * ⛔ Keying happens only through tx_gate (in-band, PA gate, SWR/open-ant trip,
 * TUNE-exempt) with the full docs/TX-SAFETY.md checklist. F6a exposes TUNE only
 * (into a dummy load / real antenna at low tune power); MOX/voice waits for the
 * mic path (F6c). The runtime supports MOX generically, but the GUI does not
 * request it yet.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SDRFL_ENGINE_TX_RUN_H
#define SDRFL_ENGINE_TX_RUN_H

/* Operator-set TX configuration (persisted by the GUI; snapshotted internally). */
typedef struct {
  int         pa_enabled;      /* PA-enable setting (RF impossible when 0)         */
  int         antenna;         /* 0/1/2 → ALEX_TX_ANTENNA_1/2/3                    */
  double      drive_w;         /* MOX/voice power request, W (→ byte via calcLevel)*/
  double      tune_w;          /* TUNE power request, W (separate, low)            */
  double      pa_calibration;  /* PA calibration, dB (per-band in F6b; one value now)*/
  int         swr_protect;     /* 1 = SWR/open-antenna protection on               */
  double      swr_alarm;       /* SWR trip threshold                               */
  int         allow_oob;       /* allow out-of-band TX                             */
  int         region;          /* band-plan region index (bp_region_t)             */
  const char *country_key;     /* "" / "CZ" / "US" (copied internally)             */
  int         mode;            /* WDSP/demod mode for the TX channel               */
} tx_run_cfg;

/* Live status for the meter / UI (thread-safe snapshot). */
typedef struct {
  int    running;   /* runtime up (thread alive + channel open)             */
  int    keyed;     /* exciter keyed right now                              */
  int    tune;      /* TUNE is the active key source                        */
  int    mox;       /* MOX is the active key source                         */
  int    tripped;   /* protection latched                                   */
  int    allowed;   /* TX permitted at the current frequency (in band)      */
  double fwd_w, rev_w, swr;
  char   reason[64];/* "" or why refused/tripped (for the UI/log)          */
} tx_run_status;

/*
 * Create the WDSP TX channel + TX panadapter analyzer + IQ framer and spawn the
 * (idle, unkeyed) worker thread. Call AFTER the WDSP RX channel exists but BEFORE
 * p2_rx_start starts the RX flow, so OpenChannel doesn't race the live RX channel.
 * `tx_freq_hz` is the initial TX frequency; `pan_pixels`/`fps` size the TX
 * panadapter (24 kHz span). Starts with a SAFE default config (PA off, drive 0,
 * SWR protection on). Returns 0 on success, negative on error.
 */
int  tx_run_start(long long tx_freq_hz, int pan_pixels, int fps);

/* Copy the latest TX panadapter frame (dB) into out[0..pixels-1]; 1 if fresh.
 * Meaningful only while keyed (the TX channel produces IQ only then). */
int  tx_run_get_pixels(float *out, int pixels);

/* Unkey, stop the worker thread, destroy the WDSP TX channel. Safe if not started. */
void tx_run_stop(void);

/* Update the operator config (thread-safe; copies the string). */
void tx_run_set_cfg(const tx_run_cfg *cfg);

/* Update the TX frequency (follows the VFO; thread-safe). */
void tx_run_set_freq(long long tx_freq_hz);

/*
 * Request keying. want_mox / want_tune are the operator's intent; the safety gate
 * decides whether it actually keys. (0,0) = unkey. Thread-safe.
 */
void tx_run_request(int want_mox, int want_tune);

/* Snapshot the current status for the meter/UI (thread-safe). */
void tx_run_get_status(tx_run_status *out);

#endif /* SDRFL_ENGINE_TX_RUN_H */
