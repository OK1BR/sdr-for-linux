/*
 * sdrfl-txkey — headless FIRST-KEYING probe (F5, docs/TX-DESIGN.md).
 *
 * Brings up the P2 RX link (for telemetry), the WDSP TX channel, and TUNEs the
 * radio into a DUMMY LOAD through the real tx_gate safety layer, printing live
 * forward/reverse power + SWR. Every keyed packet passes through tx_gate, so the
 * in-band / PA / SWR / open-antenna guards are exercised end to end.
 *
 * ⛔ THIS KEYS THE RADIO. Use ONLY with a 50-ohm dummy load + wattmeter on ANT1,
 * piHPSDR disconnected, and the operator watching. SAFE DEFAULTS: PA OFF and
 * drive 0 (a "dry key" with no RF). Enable the PA / raise drive deliberately.
 *
 *   SDRFL_RADIO_IP    radio IP                         (default 192.168.1.247)
 *   SDRFL_FREQ        TX frequency, Hz (must be in band) (default 14100000)
 *   SDRFL_TX_PA       1 = enable the PA                 (default 0 = PA OFF)
 *   SDRFL_TX_WATTS    requested power, W (hard cap 10)  (default 1.0)
 *   SDRFL_TX_PA_CAL   PA calibration, dB (per band!)    (default 53.0 = G2E default)
 *   SDRFL_TX_SECS     key duration, s (hard cap 10)     (default 3)
 *   SDRFL_TX_SWR_ALARM SWR trip threshold               (default 3.0)
 *
 * Ctrl-C unkeys and exits cleanly.
 */
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "discovered.h"
#include "discovery.h"
#include "protocol2.h"
#include "tx.h"
#include "tx_meter.h"
#include "tx_gate.h"
#include "bandplan.h"

#define TXKEY_USB 1        /* WDSP mode for the TX channel (tone bypasses the modulator) */
#define TXKEY_MAX_WATTS 10.0  /* HARD cap on the requested power for this bring-up probe */
#define TXKEY_PA_CAL_DFLT 53.0 /* G2E default pa_calibration, dB (band.c:317; HIGH = safe/low drive) */
#define TXKEY_MAX_BYTE  50    /* HARD cap on a raw drive-byte override (~10-15 W est; << 100 W rated) */

static volatile sig_atomic_t running = 1;
static void on_sigint(int s) { (void)s; running = 0; }

/* RX IQ: not needed here (we only want the link + telemetry). */
static void on_rx(const double *iq, int n, void *u) { (void)iq; (void)n; (void)u; }

/* TX IQ from the WDSP channel → frame → port-1029 socket. */
static p2_tx_iq_framer g_framer;
static void on_tx_iq(const double *iq, int n, void *u) { (void)u; p2_tx_iq_framer_push(&g_framer, iq, n); }

static long long getenv_ll(const char *n, long long d) {
  const char *v = getenv(n); return (v && *v) ? strtoll(v, NULL, 10) : d;
}
static double getenv_d(const char *n, double d) {
  const char *v = getenv(n); return (v && *v) ? atof(v) : d;
}
static double now_s(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(void) {
  const char *ip = getenv("SDRFL_RADIO_IP");
  snprintf(ipaddr_radio, sizeof(ipaddr_radio), "%s", (ip && *ip) ? ip : "192.168.1.247");
  long long freq   = getenv_ll("SDRFL_FREQ", 14100000);
  int       pa     = (int)getenv_ll("SDRFL_TX_PA", 0);
  double    watts  = getenv_d("SDRFL_TX_WATTS", 1.0);
  double    pa_cal = getenv_d("SDRFL_TX_PA_CAL", TXKEY_PA_CAL_DFLT);
  int       secs   = (int)getenv_ll("SDRFL_TX_SECS", 3);
  double    alarm  = getenv_d("SDRFL_TX_SWR_ALARM", 3.0);
  if (watts < 0.0) { watts = 0.0; }
  if (watts > TXKEY_MAX_WATTS) { watts = TXKEY_MAX_WATTS; }   /* HARD power cap */
  if (secs  < 1) { secs  = 1; } if (secs  > 10)  { secs  = 10; }   /* HARD time cap */
  /* Drive byte via calcLevel — respects the PA gain (pa_cal), so a modest watts
   * request can never become an overdrive byte. */
  int drive = tx_calc_drive_byte(watts, pa_cal);
  /* Calibration override: a raw drive byte for the operator-run byte→watts ramp,
   * hard-capped so it can never reach the overdrive region. Takes precedence. */
  int byte_override = (int)getenv_ll("SDRFL_TX_DRIVE_BYTE", -1);
  if (byte_override >= 0) {
    if (byte_override > TXKEY_MAX_BYTE) { byte_override = TXKEY_MAX_BYTE; }  /* HARD cap */
    drive = byte_override;
  }
  int is_6m = freq >= 50000000LL && freq < 54000000LL;

  signal(SIGINT, on_sigint);

  printf("=== sdrfl-txkey — FIRST-KEYING probe (F5) ===\n");
  printf("  radio %s   freq %lld Hz   %d s\n", ipaddr_radio, freq, secs);
  if (byte_override >= 0) {
    printf("  PA %s   RAW drive byte %d/255 (cap %d, calibration ramp)   SWR alarm %.1f   ANT1\n",
           pa ? "*** ON ***" : "OFF (dry key, no RF)", drive, TXKEY_MAX_BYTE, alarm);
  } else {
    printf("  PA %s   request %.2f W  (pa_cal %.1f dB → drive byte %d/255)   SWR alarm %.1f   ANT1\n",
           pa ? "*** ON ***" : "OFF (dry key, no RF)", watts, pa_cal, drive, alarm);
  }
  if (pa) {
    printf("  ⚠  PA ON — RF WILL be produced into the dummy load. WATCH THE WATTMETER.\n");
    printf("  ⚠  drive byte %d/255. If the meter climbs past your target, Ctrl-C at once.\n", drive);
  }
  printf("\n");

  /* In-band pre-check (independent of the gate, so we fail early and clearly). */
  if (!bp_band_for_freq(BP_R1, "", freq, NULL, NULL)) {
    fprintf(stderr, "REFUSED: %lld Hz is out of band (R1). Pick a ham-band freq.\n", freq);
    return 1;
  }

  printf("discovering %s ...\n", ipaddr_radio);
  p2_discovery();
  if (devices <= 0) { fprintf(stderr, "no radio found\n"); return 1; }
  const DISCOVERED *dev = &discovered[selected_device];
  printf("using %s at %s\n", dev->name, inet_ntoa(dev->network.address.sin_addr));

  if (p2_rx_start(dev, freq, 192000, on_rx, NULL) != 0) {
    fprintf(stderr, "p2_rx_start failed\n"); return 2;
  }
  usleep(1000000);   /* let the link + telemetry settle */

  /* RX baseline — forward power must read ~0 before we key. */
  { p2_telemetry t; p2_get_telemetry(&t);
    printf("RX baseline: fwd_raw=%d rev_raw=%d (expect near 0)\n\n", t.fwd_raw, t.rev_raw); }

  /* TX DSP + IQ path. */
  p2_tx_iq_framer_init(&g_framer, p2_tx_iq_socket_emit, NULL);
  tx_dsp_create(TXKEY_USB, 150.0, 2850.0, 0, on_tx_iq, NULL);   /* P2 gate */
  tx_dsp_run(1);
  tx_meter_reset();
  tx_gate_reset();

  tx_gate_cfg cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.pa_enabled = pa; cfg.antenna = 0 /* ANT1 */;
  cfg.drive_byte = drive; cfg.tune_byte = drive;
  cfg.swr_protect = 1; cfg.swr_alarm = alarm; cfg.allow_oob = 0;
  cfg.region = BP_R1; cfg.country_key = "";

  /* KEY: TUNE. Set the gate-approved state, tone on, let MOX+PA reach the radio. */
  printf(">>> KEYING (TUNE) — watch the wattmeter <<<\n");
  { tx_gate_in in = { .want_tune = 1, .freq_hz = freq, .swr = 1.0, .fwd_w = 0, .rev_w = 0 };
    tx_gate_result r; tx_gate_evaluate(&cfg, &in, &r);
    if (!r.keyed) { fprintf(stderr, "gate refused to key: %s\n", r.reason); goto unkey; }
    tx_dsp_tune_tone(1, 0.0);
    p2_set_tx_state(&r.state);
  }
  usleep(250000);   /* HP(MOX) + General(PA) land */

  {
    float silence[512];
    memset(silence, 0, sizeof silence);
    double t0 = now_s(), elapsed = 0.0;
    int gate_slot = -1, print_slot = -1;
    while (running && elapsed < secs) {
      tx_dsp_feed_mic(silence, 512);   /* → IQ → framer → port 1029 */
      usleep(10600);                   /* ~real-time (512 @ 48 kHz) */
      elapsed = now_s() - t0;

      int gs = (int)(elapsed * 10.0);  /* re-evaluate the gate at ~10 Hz */
      if (gs != gate_slot) {
        gate_slot = gs;
        p2_telemetry t; p2_get_telemetry(&t);
        tx_meter_update(t.fwd_raw, t.rev_raw, p2_tx_fwd_max_take(), is_6m);
        tx_gate_in in = { .want_tune = 1, .freq_hz = freq, .swr = tx_meter_swr(),
                          .fwd_w = tx_meter_fwd_w(), .rev_w = tx_meter_rev_w() };
        tx_gate_result r; tx_gate_evaluate(&cfg, &in, &r);
        if (!r.keyed) {                       /* trip/refuse → drop MOX immediately */
          p2_set_tx_state(NULL);
          printf("!! GATE STOP: %s\n", r.reason);
          break;
        }
        p2_set_tx_state(&r.state);

        int ps = (int)(elapsed * 4.0);   /* print at ~4 Hz */
        if (ps != print_slot) {
          print_slot = ps;
          printf("t=%4.1fs  fwd=%6.2f W  rev=%5.2f W  SWR=%5.2f  (fwd_raw=%d rev_raw=%d exc_raw=%d)\n",
                 elapsed, tx_meter_fwd_w(), tx_meter_rev_w(), tx_meter_swr(),
                 t.fwd_raw, t.rev_raw, t.exciter_raw);
          fflush(stdout);
        }
      }
    }
  }

unkey:
  /* UNKEY — drop MOX first, then stop the tone and let the no-MOX HP land. */
  p2_set_tx_state(NULL);
  tx_dsp_tune_tone(0, 0.0);
  { float silence[512]; memset(silence, 0, sizeof silence);
    for (int i = 0; i < 4; i++) { tx_dsp_feed_mic(silence, 512); usleep(10600); } }
  usleep(300000);
  printf(">>> UNKEYED <<<\n");

  tx_dsp_destroy();
  p2_rx_stop();
  return 0;
}
