/*
 * sdr-for-linux — PureSignal runtime (F7 / PS-2, docs/PS-SCOPE.md).
 *
 * The WDSP side of PureSignal: owns the calcc control calls (SetPSControl /
 * SetPSMox / params), accumulates the P2 feedback stream (p2_ps_iq_cb) into
 * 1024-pair blocks and feeds pscc(), and snapshots GetPSInfo for the UI.
 * The wire side (feedback DDC pair, attenuator exception, ALEX_PS_BIT) is
 * PS-1 in protocol2.c — this module drives it via p2_set_ps().
 *
 * ⛔ TX safety: PS never keys anything. It only becomes active while the
 * operator is keyed through tx_gate (tx_run calls ps_key on the gate slot),
 * and the approved delta #1 (controlled ADC0 attenuator during PS-TX,
 * PS-SCOPE §6) reaches the wire only inside that keyed state. With the
 * PureSignal setting off, p2_set_ps(NULL) keeps every packet bit-identical
 * to the PS-less build (txprobe-enforced).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SDRFL_ENGINE_PS_H
#define SDRFL_ENGINE_PS_H

/*
 * Initialise after the WDSP TX channel exists (tx_dsp_create): declares the
 * fixed 192 kHz P2 feedback rate (SetPSFeedbackRate), applies the piHPSDR
 * default parameter set (map/pin/ptol/moxdelay 0.2 s/ampdelay 150 ns;
 * transmitter.c:1069-1080) and registers the P2 feedback callback.
 * PS itself stays OFF until ps_set(). Returns 0 on success.
 */
int  ps_start(void);

/* Unregister the feedback callback and force PS off (safe if not started). */
void ps_stop(void);

/*
 * Operator config, applied live (thread-safe, called from the GUI):
 * `enable` arms/disarms the whole feature (wire config via p2_set_ps + WDSP
 * automode via SetPSControl); `att_db` is the ADC0 feedback attenuator used
 * during PS-TX (0-31, the approved delta #1); `setpk` is WDSP's expected
 * full-scale TX envelope (SetPSHWPeak; G1/P2 default 0.2899 — too small
 * causes "very strange things" per piHPSDR, so values < 0.01 are clamped
 * to the default). Disabling follows piHPSDR's reset choreography: correction
 * off + ~7 zero buffers through pscc so the reset takes effect while no
 * feedback flows (transmitter.c:2477-2499).
 */
void ps_set(int enable, int att_db, double setpk);

/*
 * Key-state hand-off from the tx_run gate slot (~20 Hz, every slot): `keyed`
 * is "MOX/TUNE keyed AND not CW-carrier" (CW bypasses WDSP so feedback is
 * meaningless — piHPSDR transmitter.c:2114-2120). Calls SetPSMox (lock-free)
 * and zeroes the feedback accumulator on the key-on edge (piHPSDR
 * radio.c:2046-2052: no stale half-buffer may survive into a new over).
 */
void ps_key(int keyed);

/* GetPSInfo subset for the UI/status (poll ~20 Hz; zeros when off). */
typedef struct {
  int on;         /* PS enabled (operator setting)                        */
  int correcting; /* info[14]: correction currently applied               */
  int state;      /* info[15]: calcc state machine (0=Reset..9=TurnOn)    */
  int fdbk;       /* info[4]: feedback level — ideal ~152, good 140-165   */
  int cals;       /* info[5]: calibration attempts (counts up per calc)   */
  int sln;        /* info[6]: last correction-sanity bitmask (calcc.c     */
                  /*   scheck: 0x1 NaN, 0x2 empty bin, 0x4 gain>1,        */
                  /*   0x8 endpoint, 0x10/20 neg gain, 0x40 jump)         */
  double getpk;   /* GetPSMaxTX: max TX envelope seen while collecting —  */
                  /*   the measured truth for the SetPk setting           */
} ps_status;
void ps_get_status(ps_status *out);

#endif /* SDRFL_ENGINE_PS_H */
