/*
 * sdrfl-p1probe — headless Protocol-1 RX gate (R1, docs/P1-SCOPE.md).
 *
 * The P1 twin of sdrfl-rxprobe: discover the radio (default the HL2 at
 * $SDRFL_RADIO_IP or 192.168.1.21), start ONE receiver, count the IQ stream
 * for a few seconds and sanity-check it: sample-rate accuracy, DC/RMS level,
 * EP6 sequence/sync errors, telemetry (temperature). RX only — the link
 * module has no TX code at all; on HL-class radios the C&C locks the T/R
 * relay to RX (⛔ no-TX guarantees, P1-SCOPE §3).
 *
 * Exit 0 = IQ flows at the expected rate with a clean stream.
 */
#include <arpa/inet.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>

#include "discovered.h"
#include "discovery.h"
#include "protocol1.h"

#define RUN_SECONDS 5

static volatile gint64 g_pairs;
static double  g_sum_i, g_sum_q, g_sum_sq;   /* listener thread only */
static double  g_peak;

static void on_iq(const double *iq, int n_pairs, void *user) {
  (void)user;

  for (int i = 0; i < n_pairs; i++) {
    double iv = iq[2 * i], qv = iq[2 * i + 1];
    g_sum_i  += iv;
    g_sum_q  += qv;
    g_sum_sq += iv * iv + qv * qv;
    double m = fabs(iv) > fabs(qv) ? fabs(iv) : fabs(qv);

    if (m > g_peak) { g_peak = m; }
  }

  g_pairs += n_pairs;
}

int main(void) {
  const char *ip = getenv("SDRFL_RADIO_IP");
  long long freq = 7100000;
  int rate = 192000;
  const char *e;

  if ((e = getenv("SDRFL_FREQ")) && *e) { freq = strtoll(e, NULL, 10); }

  if ((e = getenv("SDRFL_RATE")) && *e) { rate = atoi(e); }

  snprintf(ipaddr_radio, sizeof(ipaddr_radio), "%s", (ip && *ip) ? ip : "192.168.1.21");
  printf("=== sdrfl-p1probe — Protocol-1 RX gate ===\n");
  printf("probing %s ... (freq %lld Hz, rate %d Hz)\n", ipaddr_radio, freq, rate);
  p1_discovery();

  const DISCOVERED *dev = NULL;

  for (int i = 0; i < devices && !dev; i++) {
    if (discovered[i].protocol == ORIGINAL_PROTOCOL) { dev = &discovered[i]; }
  }

  if (!dev) { fprintf(stderr, "FAIL: no Protocol-1 radio answered\n"); return 1; }

  printf("using %s (device %d, fw %d.%d) at %s\n", dev->name, dev->device,
         dev->software_version / 10, dev->software_version % 10,
         inet_ntoa(dev->network.address.sin_addr));

  if (dev->status != 2) {
    fprintf(stderr, "FAIL: radio is IN USE (status %d) — close the other client\n",
            dev->status);
    return 1;
  }

  if (p1_rx_start(dev, freq, rate, on_iq, NULL) != 0) {
    fprintf(stderr, "FAIL: p1_rx_start\n");
    return 1;
  }

  for (int s = 1; s <= RUN_SECONDS; s++) {
    g_usleep(1000000);
    p1_telemetry t;
    p1_get_telemetry(&t);
    double temp_c = 0.0795898 * t.temp_raw - 50.0;   /* HL2 (rx_panadapter.c:884) */
    printf("  t=%ds  pairs=%lld  seq_err=%d sync_err=%d ovl=%d  temp=%.1f C  ptt=%d\n",
           s, (long long)g_pairs, t.seq_errors, t.sync_errors, t.adc_overload,
           temp_c, t.ptt);
  }

  long long got = g_pairs;
  p1_rx_stop();

  double expect = (double)rate * RUN_SECONDS;
  double ratio  = got / expect;
  double rms    = got ? sqrt(g_sum_sq / (2.0 * got)) : 0.0;
  double dc_i   = got ? g_sum_i / got : 0.0;
  double dc_q   = got ? g_sum_q / got : 0.0;
  printf("\nIQ pairs: %lld (expected ~%.0f, ratio %.4f)\n", got, expect, ratio);
  printf("levels: RMS %.6f  peak %.6f  DC I %.2e / Q %.2e (full scale = 1.0)\n",
         rms, g_peak, dc_i, dc_q);

  int ok = ratio > 0.98 && ratio < 1.02 && rms > 0.0 && g_peak < 1.0;
  printf(ok ? "PASS — P1 IQ flows at the expected rate, stream clean.\n"
            : "FAIL — rate off or stream broken (see numbers above).\n");
  return ok ? 0 : 1;
}
