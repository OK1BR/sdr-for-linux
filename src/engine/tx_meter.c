/*
 * sdr-for-linux — TX power + SWR metering. See tx_meter.h.
 *
 * Mirrors piHPSDR transmitter.c @974acba, the G2E branch (:645-758):
 *   v      = ((raw - cal_offset) / 4095) * constant1        [ADC volts]
 *   watts  = compute_power(v*v / constant2)
 *   gamma  = sqrt(rev_w / fwd_w)   (clamped 0.95)
 *   swr    = 0.7*(1+gamma)/(1-gamma) + 0.3*swr    (smoothed; decays to 1 on RX)
 * watts pass through the pa_trim correction curve (compute_power); the default
 * curve is linear (identity), so watts = v*v/constant2 directly until the operator
 * calibrates it against an external wattmeter (F6b, tx_meter_set_trim).
 */
#include <math.h>

#include "tx_meter.h"

/* Per-device bridge calibration (tx_meter_set_cal). DEFAULT = the G2E
 * (ANAN-7000 PA board), base constants from transmitter.c:645-662 EXCEPT C1:
 * C1 is the slow-ADC full-scale voltage; Thetis/piHPSDR assume 3.3 V for the
 * G2E, but a live wattmeter check on this G2E (OK1BR, 20 m, byte 20 → 16 W
 * measured vs 7 W computed) shows 5.0 V: (5.0/3.3)^2 = 2.29 ≈ the 2.26×
 * under-read, and it scales fwd+rev together so SWR is unchanged.
 * The Hermes-class (ANAN 10E) values come from the METIS/HERMES/ANGELIA
 * branch (transmitter.c:622-634) via radio_tx_profile → gui.c. */
static tx_meter_cal m_cal = {
  .c1 = 5.0,                       /* ADC full-scale volts (G2E calibrated, was 3.3) */
  .c2 = 0.12,                      /* forward coupler                   */
  .rc2_hf = 0.15, .rc2_6m = 0.70,  /* reverse coupler HF / 6 m          */
  .fwd_off = 48, .rev_off = 42,    /* ADC pedestal offsets              */
  .pa_watts = 100.0,               /* rating → trim grid 0,10,..,100 W  */
};

static double m_fwd = 0.0;   /* forward watts (EMA words)      */
static double m_pep = 0.0;   /* forward PEP watts (raw maximum) */
static double m_rev = 0.0;   /* reverse watts */
static double m_swr = 1.0;   /* smoothed VSWR */

/* Wattmeter correction breakpoints: m_trim[i] is the raw-computed power that
 * reads when the true output is i*(pa_watts/10) watts. Default = identity
 * (linear) on the rating grid — piHPSDR radio.c:1330 pa_trim[i] = i*rating*0.1,
 * reseeded when the rating changes (radio_menu.c:439-442); set_cal mirrors that. */
static double m_trim[11] = { 0.0, 10.0, 20.0, 30.0, 40.0, 50.0,
                             60.0, 70.0, 80.0, 90.0, 100.0 };

void tx_meter_set_cal(const tx_meter_cal *cal) {
  m_cal = *cal;
  if (m_cal.pa_watts <= 0.0) { m_cal.pa_watts = 100.0; }
  for (int i = 0; i < 11; i++) { m_trim[i] = i * m_cal.pa_watts * 0.1; }
}

void tx_meter_reset(void) {
  m_fwd = 0.0;
  m_pep = 0.0;
  m_rev = 0.0;
  m_swr = 1.0;
}

void tx_meter_set_trim(const double trim[11]) {
  m_trim[0] = 0.0;                       /* the origin is fixed */
  for (int i = 1; i < 11; i++) { m_trim[i] = trim[i]; }
}

/* Piecewise-linear wattmeter correction (piHPSDR transmitter.c compute_power):
 * interpolate the raw-computed power p through the operator's measured breakpoints
 * onto the true-watts grid (0..pa_watts in 10 steps). Identity with the default. */
static double compute_power(double p) {
  int i = 0;
  if (p > m_trim[10]) {
    i = 9;
  } else {
    while (i < 10 && p > m_trim[i]) { i++; }
    if (i > 0) { i--; }
  }
  if (m_trim[i + 1] - m_trim[i] < 0.001) { return m_trim[i + 1]; }  /* guard /0 */
  double frac = (p - m_trim[i]) / (m_trim[i + 1] - m_trim[i]);
  return m_cal.pa_watts * 0.1 * ((1.0 - frac) * (double)i + frac * (double)(i + 1));
}

/* raw coupler word -> watts, through the wattmeter correction curve. */
static double raw_to_watts(int raw, int cal_offset, double rconstant) {
  int p = raw - cal_offset;
  if (p < 0) { p = 0; }
  double v = ((double)p / 4095.0) * m_cal.c1;
  return compute_power((v * v) / rconstant);
}

void tx_meter_update(int fwd_raw, int rev_raw, int fwd_max_raw, int is_6m) {
  m_fwd = raw_to_watts(fwd_raw, m_cal.fwd_off, m_cal.c2);
  m_pep = raw_to_watts(fwd_max_raw, m_cal.fwd_off, m_cal.c2);
  m_rev = raw_to_watts(rev_raw, m_cal.rev_off, is_6m ? m_cal.rc2_6m : m_cal.rc2_hf);

  if (m_fwd > 0.25) {
    double gamma = sqrt(m_rev / m_fwd);
    if (gamma > 0.95) { gamma = 0.95; }        /* keep SWR finite so the avg recovers */
    m_swr = 0.7 * (1.0 + gamma) / (1.0 - gamma) + 0.3 * m_swr;
  } else {
    m_swr = 0.7 + 0.3 * m_swr;                 /* no drive: settle toward 1.0 */
  }
}

double tx_meter_fwd_w(void)     { return m_fwd; }
double tx_meter_fwd_pep_w(void) { return m_pep; }
double tx_meter_rev_w(void)     { return m_rev; }
double tx_meter_swr(void)       { return m_swr; }
