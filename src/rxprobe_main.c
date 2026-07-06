/*
 * sdrfl-rxprobe — headless Protocol-2 RX + IQ probe (Milestone 1, step 3 gate).
 *
 * Discovers the radio, starts one DDC (RX1) over Protocol 2 at a given
 * frequency / sample rate, collects the IQ stream for ~2 s via our own
 * callback, and prints the sample count, effective sample rate, and IQ RMS.
 * Proves the P2 RX path end-to-end before WDSP is wired in (step 4).
 *
 *   PIHPSDR must be disconnected — opening the P2 data path takes the radio
 *   (one owner at a time).
 *
 * Env:
 *   SDRFL_RADIO_IP   radio IP for directed discovery   (default 192.168.1.247)
 *   SDRFL_FREQ       RX frequency in Hz                 (default 14100000)
 *   SDRFL_RATE       DDC sample rate in Hz              (default 192000)
 *   SDRFL_SECS       collection time in seconds         (default 2)
 *   SDRFL_DRYRUN     if set: build+hexdump the outgoing packets and exit
 *                    (no socket, no radio — offline byte-offset self-check)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "discovered.h"
#include "discovery.h"
#include "protocol2.h"

typedef struct {
  long long pairs;      // I/Q sample pairs seen
  double    sumsq;      // sum of I^2 + Q^2
  double    first_i[4]; // first few samples, for a sanity peek
  double    first_q[4];
  int       nfirst;
} stats_t;

static void on_iq(const double *iq, int n_pairs, void *user) {
  stats_t *s = user;
  for (int i = 0; i < n_pairs; i++) {
    double I = iq[2 * i], Q = iq[2 * i + 1];
    s->sumsq += I * I + Q * Q;
    if (s->nfirst < 4) {
      s->first_i[s->nfirst] = I;
      s->first_q[s->nfirst] = Q;
      s->nfirst++;
    }
  }
  s->pairs += n_pairs;
}

static long long getenv_ll(const char *name, long long dflt) {
  const char *v = getenv(name);
  return (v && *v) ? strtoll(v, NULL, 10) : dflt;
}

static void hexdump(const char *label, const unsigned char *buf, int len, int show) {
  printf("%s (%d bytes, first %d shown):\n", label, len, show);
  for (int i = 0; i < show; i += 16) {
    printf("  %04x:", i);
    for (int j = i; j < i + 16 && j < show; j++) { printf(" %02x", buf[j]); }
    printf("\n");
  }
}

int main(void) {
  const char *ip = getenv("SDRFL_RADIO_IP");
  snprintf(ipaddr_radio, sizeof(ipaddr_radio), "%s", (ip && *ip) ? ip : "192.168.1.247");

  long long freq = getenv_ll("SDRFL_FREQ", 14100000);
  int        rate = (int)getenv_ll("SDRFL_RATE", 192000);
  int        secs = (int)getenv_ll("SDRFL_SECS", 2);

  if (getenv("SDRFL_DRYRUN")) {
    // Offline: build the three outgoing packets and hexdump them. Device G1 is
    // assumed for the DDC mapping; check offsets against docs/P2-RX-SCOPE.md.
    unsigned char buf[1500];
    int dev = NEW_DEVICE_G1;
    printf("sdrfl-rxprobe DRYRUN: device=G1(%d) freq=%lld Hz rate=%d Hz\n\n", dev, freq, rate);
    hexdump("General [37]=08 [38]=01", buf, p2_build_general(buf), 48);
    hexdump("RX-specific [4]=n_adc [7]=DDC-en [17]=adc [18/19]=rate/1k [22]=24",
            buf, p2_build_receive_specific(buf, dev, rate), 48);
    hexdump("High-Priority [4]=run [9..12]=DDC0 phase",
            buf, p2_build_high_priority(buf, dev, freq, 1), 48);
    return 0;
  }

  printf("sdrfl-rxprobe: discovering %s ...\n", ipaddr_radio);
  p2_discovery();
  if (devices <= 0) {
    fprintf(stderr, "no radio found\n");
    return 1;
  }
  const DISCOVERED *dev = &discovered[selected_device];
  printf("using [%d] %s at %s  (dev=%d)\n", selected_device, dev->name,
         inet_ntoa(dev->network.address.sin_addr), dev->device - 1000);
  printf("starting RX: %lld Hz @ %d Hz sample rate, collecting %d s ...\n", freq, rate, secs);

  stats_t st;
  memset(&st, 0, sizeof(st));

  if (p2_rx_start(dev, freq, rate, on_iq, &st) != 0) {
    fprintf(stderr, "p2_rx_start failed\n");
    return 2;
  }

  // Let the stream settle past the start handshake, then measure only the
  // steady window (baseline snapshot) so the rate isn't inflated by the IQ that
  // arrived during p2_rx_start's post-run usleep. 64-bit aligned reads of the
  // counters race benignly with the listener — fine for a measurement.
  usleep(200000);
  struct timespec t0, t1;
  long long pairs0 = st.pairs;
  double    sumsq0 = st.sumsq;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  sleep(secs);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long long pairs = st.pairs - pairs0;
  double    sumsq = st.sumsq - sumsq0;

  p2_rx_stop();

  double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
  double eff_rate = elapsed > 0 ? pairs / elapsed : 0.0;
  double rms = pairs > 0 ? sqrt(sumsq / (double)pairs) : 0.0;

  printf("\n--- results ---\n");
  printf("IQ pairs collected : %lld  (steady window; %lld total)\n", pairs, st.pairs);
  printf("elapsed            : %.3f s\n", elapsed);
  printf("effective rate     : %.0f Hz  (requested %d Hz)\n", eff_rate, rate);
  printf("IQ RMS             : %.6g", rms);
  if (rms > 0) { printf("  (%.1f dBFS)", 20.0 * log10(rms)); }
  printf("\n");
  printf("first samples      :");
  for (int i = 0; i < st.nfirst; i++) { printf(" (%.4f,%.4f)", st.first_i[i], st.first_q[i]); }
  printf("\n");

  if (pairs <= 0) {
    fprintf(stderr, "no IQ received — check radio ownership / DDC port\n");
    return 3;
  }
  return 0;
}
