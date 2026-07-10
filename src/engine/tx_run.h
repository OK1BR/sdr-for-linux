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
  double      tune_w;          /* TUNE power request, W (separate; up to full)     */
  double      pa_calibration;  /* PA calibration for the CURRENT band, dB (F6b)     */
  int         swr_protect;     /* 1 = SWR/open-antenna protection on               */
  double      swr_alarm;       /* SWR trip threshold                               */
  int         allow_oob;       /* allow out-of-band TX                             */
  int         region;          /* band-plan region index (bp_region_t)             */
  const char *country_key;     /* "" / "CZ" / "US" (copied internally)             */
  int         mode;            /* WDSP/demod mode for the TX channel               */
  int         ptt_enabled;     /* footswitch: radio PTT input = MOX intent (voice
                                  modes only; still gated by tx_gate like the
                                  MOX button — never keys on its own)             */
  double      pa_trim[11];     /* wattmeter correction curve, W (F6b; identity dflt)*/
  double      tx_flo, tx_fhi;  /* TX audio passband edges, Hz (0/0 = 150/2850);
                                  sideband sign is applied per mode internally     */
} tx_run_cfg;

/* Live status for the meter / UI (thread-safe snapshot). */
typedef struct {
  int    running;   /* runtime up (thread alive + channel open)             */
  int    keyed;     /* exciter keyed right now                              */
  int    tune;      /* TUNE is the active key source                        */
  int    mox;       /* MOX is the active key source                         */
  int    tripped;   /* protection latched                                   */
  int    allowed;   /* TX permitted at the current frequency (in band)      */
  int    high_swr;  /* SWR >= alarm now (indicator; set in TUNE + MOX)      */
  double fwd_w, rev_w, swr;
  double fwd_pep_w; /* forward PEP (raw packet max, decaying) — display power  */
  double mic_pk;    /* mic-input peak, dBFS (≤ 0; -99 floor); valid while keyed */
  double alc_gain;  /* ALC gain reduction, dB (0 = none, negative = clamping)   */
  double lvlr_gain; /* leveler makeup gain, dB (0..+8; valid only with PROC on) */
  /* PureSignal (F7/PS-2; zeros when the setting is off): */
  int    ps_on;         /* PS enabled                                           */
  int    ps_correcting; /* correction applied right now (GetPSInfo[14])         */
  int    ps_state;      /* calcc state machine (GetPSInfo[15], 0=Reset..9)      */
  int    ps_fdbk;       /* feedback level (GetPSInfo[4]) — ideal ~152, 140-165  */
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

/* Mic gain in dB → the WDSP TX panel (SetTXAPanelGain1). Safe if TX isn't up. */
void tx_run_set_mic_gain(double db);

/* Speech processor (PROC): COMP at gain_db + auto-leveler, piHPSDR
 * tx_set_compressor semantics (see tx_dsp_set_compressor). Safe if TX isn't up. */
void tx_run_set_comp(int on, double gain_db);

/* Mic noise gate (downward expander; see tx_dsp_set_gate). Safe if TX isn't up. */
void tx_run_set_gate(int on, double thresh_db);

/* TX monitor (self-listen): while keyed, feed the mic (voice) or a sidetone
 * shaped by the CW envelope into the host audio via demod_monitor_push. Level
 * is set separately (demod_set_monitor_gain). Safe if TX isn't up. */
void tx_run_set_monitor(int on);

/* TX panadapter zoom: set the displayed span (Hz). Safe if TX isn't up. */
void tx_run_set_span(double span_hz);

/*
 * CW (F6d). Queue Morse for `text` (appended); the TX runtime's break-in logic
 * keys the exciter through tx_gate while there's Morse to send, then holds MOX for
 * the hang time. tx_run_cw_abort() drops the queue. tx_run_set_cw() sets speed
 * (WPM), weight (0-100), rise/fall ramp (ms) and break-in hang (ms). All safe if
 * TX isn't up (no-ops). Keying still only happens in a CW mode + through the gate.
 * tx_run_set_sidetone() sets the monitor sidetone pitch (Hz) and level (dBFS
 * before the shared monitor gain; default −20 ≈ piHPSDR sidetone volume 50/127).
 */
void tx_run_cw_send(const char *text);
void tx_run_cw_abort(void);
void tx_run_set_cw(int wpm, double weight, double ramp_ms, int hang_ms);
void tx_run_set_sidetone(int pitch_hz, double level_db);

/*
 * External TX audio source (TCI, F6d-2c). set_ext_source(1) makes the feed
 * loop pull mono 48 kHz audio pushed via tx_run_ext_push (thread-safe SPSC —
 * the TCI service thread produces) instead of the mic; keying still only
 * happens through tx_gate, exactly like voice MOX. set_ext_notify registers
 * the pacing clock: called from the TX feed thread with the sample count to
 * request from the client (TCI TX_CHRONO). All safe when TX isn't up.
 */
void tx_run_set_ext_source(int on);
void tx_run_ext_push(const float *mono48k, int n);
void tx_run_set_ext_notify(void (*cb)(int nsamples));

/*
 * Request keying. want_mox / want_tune are the operator's intent; the safety gate
 * decides whether it actually keys. (0,0) = unkey. Thread-safe.
 */
void tx_run_request(int want_mox, int want_tune);

/* Snapshot the current status for the meter/UI (thread-safe). */
void tx_run_get_status(tx_run_status *out);

#endif /* SDRFL_ENGINE_TX_RUN_H */
