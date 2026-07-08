/*
 * sdr-for-linux — TX forward/reverse power + SWR from the ALEX coupler words.
 * F3, docs/TX-DESIGN.md. Pure, read-only: it turns the raw sensor words the radio
 * reports (p2_telemetry.fwd_raw / rev_raw, np.c port-1025 status) into watts and a
 * (smoothed) SWR, using the G1 calibration from piHPSDR transmitter.c:645-758.
 *
 * ⛔ This computes only — it never keys or changes anything sent to the radio. The
 * SWR-driven drive/MOX cut-out is the F4 safety layer (this just provides the
 * number). On RX the words are ~0, so fwd/rev read ~0 W and SWR settles to 1.0.
 *
 * NOTE: absolute watts use the piHPSDR default (linear) pa_trim, i.e. compute_power
 * is the identity — the numbers are structurally correct but UNCALIBRATED until a
 * live wattmeter pass (F6). SWR is a ratio and does not depend on that calibration.
 */
#ifndef SDRFL_ENGINE_TX_METER_H
#define SDRFL_ENGINE_TX_METER_H

/* Reset the running SWR/power state (call at TX channel create / RX->TX). */
void tx_meter_reset(void);

/*
 * Feed one set of averaged coupler words (p2_telemetry.fwd_raw / rev_raw) and
 * advance the smoothed SWR by one step. `is_6m` selects the 6 m reverse constant.
 * Call at the meter rate (~10-20 Hz), like piHPSDR's tx_update_display.
 */
void tx_meter_update(int fwd_raw, int rev_raw, int is_6m);

double tx_meter_fwd_w(void);   /* smoothed forward power, watts   */
double tx_meter_rev_w(void);   /* smoothed reverse power, watts   */
double tx_meter_swr(void);     /* smoothed VSWR (>= 1.0)          */

#endif /* SDRFL_ENGINE_TX_METER_H */
