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
 * Absolute watts pass through an 11-point wattmeter correction curve (pa_trim,
 * F6b) applied to both fwd and rev, exactly like piHPSDR compute_power. The
 * DEFAULT curve is linear (identity), so out of the box watts = the base G1
 * computation (our live-validated constants); the operator can refine it against
 * an external wattmeter. SWR is a ratio and is essentially independent of it.
 */
#ifndef SDRFL_ENGINE_TX_METER_H
#define SDRFL_ENGINE_TX_METER_H

/* Reset the running SWR/power state (call at TX channel create / RX->TX). Does
 * NOT touch the wattmeter-trim curve (that is config, set via tx_meter_set_trim). */
void tx_meter_reset(void);

/*
 * Install the 11-point wattmeter correction curve (raw-computed W → true W at
 * 0,10,..,100 W). trim[0] is forced to 0 (the origin). Default is identity; pass
 * a calibrated curve to correct sensor non-linearity. Thread note: called from
 * the TX worker each meter slot, so no cross-thread torn reads of the curve.
 */
void tx_meter_set_trim(const double trim[11]);

/*
 * Feed one set of averaged coupler words (p2_telemetry.fwd_raw / rev_raw) plus
 * the decaying raw forward maximum (p2_tx_fwd_max_take()) and advance the
 * smoothed SWR by one step. `is_6m` selects the 6 m reverse constant.
 * Call at the meter rate (~10-20 Hz), like piHPSDR's tx_update_display.
 *
 * fwd/rev/SWR come from the EMA words (piHPSDR parity: SWR + protection always
 * act on the averages); fwd_pep comes from fwd_max_raw through the SAME
 * volts²+trim conversion (piHPSDR metermode 0, transmitter.c:760-766). On a
 * steady carrier PEP == avg; on voice the avg under-reads by ~6 dB.
 */
void tx_meter_update(int fwd_raw, int rev_raw, int fwd_max_raw, int is_6m);

double tx_meter_fwd_w(void);     /* smoothed forward power, watts   */
double tx_meter_fwd_pep_w(void); /* PEP forward power, watts        */
double tx_meter_rev_w(void);     /* smoothed reverse power, watts   */
double tx_meter_swr(void);       /* smoothed VSWR (>= 1.0)          */

#endif /* SDRFL_ENGINE_TX_METER_H */
