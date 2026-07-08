/*
 * sdr-for-linux — TX power + SWR metering. See tx_meter.h.
 *
 * Mirrors piHPSDR transmitter.c @974acba, the G1 branch (:645-758):
 *   v      = ((raw - cal_offset) / 4095) * constant1        [ADC volts]
 *   watts  = compute_power(v*v / constant2)
 *   gamma  = sqrt(rev_w / fwd_w)   (clamped 0.95)
 *   swr    = 0.7*(1+gamma)/(1-gamma) + 0.3*swr    (smoothed; decays to 1 on RX)
 * With the default (linear) pa_trim, compute_power is the identity, so watts =
 * v*v/constant2 directly (see tx_meter.h note; live pa_trim calibration is F6).
 */
#include <math.h>

#include "tx_meter.h"

/* G1 (ANAN-7000 PA board, Hermes-class 3.3 V ADCs) — transmitter.c:645-662. */
#define G1_C1        3.3     /* constant1: ADC full-scale volts   */
#define G1_C2        0.12    /* constant2: forward coupler        */
#define G1_RC2_HF    0.15    /* rconstant2 on HF                  */
#define G1_RC2_6M    0.70    /* rconstant2 on 6 m                 */
#define G1_FWD_OFF   48      /* fwd_cal_offset                    */
#define G1_REV_OFF   42      /* rev_cal_offset                    */

static double m_fwd = 0.0;   /* forward watts */
static double m_rev = 0.0;   /* reverse watts */
static double m_swr = 1.0;   /* smoothed VSWR */

void tx_meter_reset(void) {
  m_fwd = 0.0;
  m_rev = 0.0;
  m_swr = 1.0;
}

/* raw coupler word -> watts (compute_power is identity with the default pa_trim). */
static double raw_to_watts(int raw, int cal_offset, double rconstant) {
  int p = raw - cal_offset;
  if (p < 0) { p = 0; }
  double v = ((double)p / 4095.0) * G1_C1;
  return (v * v) / rconstant;
}

void tx_meter_update(int fwd_raw, int rev_raw, int is_6m) {
  m_fwd = raw_to_watts(fwd_raw, G1_FWD_OFF, G1_C2);
  m_rev = raw_to_watts(rev_raw, G1_REV_OFF, is_6m ? G1_RC2_6M : G1_RC2_HF);

  if (m_fwd > 0.25) {
    double gamma = sqrt(m_rev / m_fwd);
    if (gamma > 0.95) { gamma = 0.95; }        /* keep SWR finite so the avg recovers */
    m_swr = 0.7 * (1.0 + gamma) / (1.0 - gamma) + 0.3 * m_swr;
  } else {
    m_swr = 0.7 + 0.3 * m_swr;                 /* no drive: settle toward 1.0 */
  }
}

double tx_meter_fwd_w(void) { return m_fwd; }
double tx_meter_rev_w(void) { return m_rev; }
double tx_meter_swr(void)   { return m_swr; }
