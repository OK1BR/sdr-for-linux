/*
 * sdrfl-audioprobe — headless RX → WDSP demod → PipeWire audio.
 * Milestone-2 gate; protocol-agnostic since HL2 R2 (docs/P1-SCOPE.md).
 *
 * Discovers the radio (P2 first, P1/METIS round only when the pinned IP did
 * not answer P2 — same policy as start_radio), starts one receiver over the
 * radio's protocol, demodulates it, and plays the audio through the native
 * PipeWire sink. Proves IQ → demod → sound end-to-end at low latency. On a
 * P1 radio the default 768 kHz rate is capped to P1's 384 kHz maximum.
 *
 *   TAKES THE RADIO — piHPSDR must be disconnected.
 *
 * SDRFL_TONE=1 skips the radio and plays a 700 Hz test tone — an offline check
 * that the PipeWire sink works (and you can gauge latency by ear).
 *
 * Env: SDRFL_RADIO_IP (192.168.1.247), SDRFL_FREQ (14100000), SDRFL_RATE
 *      (768000), SDRFL_SECS (10), SDRFL_MODE (auto: LSB<10MHz else USB),
 *      SDRFL_VOLUME (-10 dB), SDRFL_LAT (10 ms audio latency target)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "discovered.h"
#include "discovery.h"
#include "protocol1.h"
#include "protocol2.h"
#include "demod.h"
#include "audio.h"

#define AUDIO_RATE 48000

static long long getenv_ll(const char *n, long long d) {
  const char *v = getenv(n);
  return (v && *v) ? strtoll(v, NULL, 10) : d;
}

static void feed_cb(const double *iq, int n_pairs, void *user) {
  (void)user;
  demod_feed(iq, n_pairs);
}

/* Mode + passband from SDRFL_MODE, or by band (ham convention). */
static int pick_mode(long long freq, double *flo, double *fhi, const char **name) {
  const char *m = getenv("SDRFL_MODE");
  int mode;
  if      (m && !strcasecmp(m, "usb")) { mode = DEMOD_USB; }
  else if (m && !strcasecmp(m, "lsb")) { mode = DEMOD_LSB; }
  else if (m && !strcasecmp(m, "cwu")) { mode = DEMOD_CWU; }
  else if (m && !strcasecmp(m, "cwl")) { mode = DEMOD_CWL; }
  else if (m && !strcasecmp(m, "am"))  { mode = DEMOD_AM;  }
  else { mode = (freq < 10000000) ? DEMOD_LSB : DEMOD_USB; }

  const double st = 600.0;  /* CW sidetone */
  switch (mode) {
    case DEMOD_USB: *flo =  150;      *fhi = 2850;      *name = "USB"; break;
    case DEMOD_LSB: *flo = -2850;     *fhi = -150;      *name = "LSB"; break;
    case DEMOD_CWU: *flo =  st - 250; *fhi = st + 250;  *name = "CWU"; break;
    case DEMOD_CWL: *flo = -(st+250); *fhi = -(st-250); *name = "CWL"; break;
    case DEMOD_AM:  *flo = -4000;     *fhi = 4000;      *name = "AM";  break;
    default:        *flo =  150;      *fhi = 2850;      *name = "USB"; break;
  }
  return mode;
}

int main(void) {
  int lat = (int)getenv_ll("SDRFL_LAT", 10);

  if (getenv("SDRFL_TONE")) {
    printf("sdrfl-audioprobe TONE: 700 Hz for 2 s via PipeWire (lat target %d ms)\n", lat);
    if (audio_start(AUDIO_RATE, 1, lat, NULL) != 0) { fprintf(stderr, "audio_start failed\n"); return 2; }
    double ph = 0.0, dph = 2.0 * M_PI * 700.0 / AUDIO_RATE;
    float blk[480];
    for (int b = 0; b < 200; b++) {              /* 200 × 10 ms = 2 s */
      for (int i = 0; i < 480; i++) {
        blk[i] = 0.2f * sinf(ph);
        ph += dph;
        if (ph > 2.0 * M_PI) { ph -= 2.0 * M_PI; }
      }
      audio_push(blk, 480);
      usleep(10000);
    }
    usleep(200000);
    audio_stop();
    return 0;
  }

  const char *ip = getenv("SDRFL_RADIO_IP");
  snprintf(ipaddr_radio, sizeof(ipaddr_radio), "%s", (ip && *ip) ? ip : "192.168.1.247");
  long long freq = getenv_ll("SDRFL_FREQ", 14100000);
  int       rate = (int)getenv_ll("SDRFL_RATE", 768000);
  int       secs = (int)getenv_ll("SDRFL_SECS", 10);
  double    vol  = (double)getenv_ll("SDRFL_VOLUME", -10);
  double    flo, fhi;
  const char *mname;
  int mode = pick_mode(freq, &flo, &fhi, &mname);

  printf("sdrfl-audioprobe: discovering %s ...\n", ipaddr_radio);
  p2_discovery();
  /* P1 (METIS) round only when the pinned IP wasn't answered by P2 — same
   * policy as start_radio (a P2 gate run pays no extra probe time). */
  {
    int need_p1 = 1;
    for (int i = 0; i < devices; i++) {
      if (strcmp(inet_ntoa(discovered[i].network.address.sin_addr), ipaddr_radio) == 0) {
        need_p1 = 0;
        break;
      }
    }
    if (need_p1) { p1_discovery(); }
  }
  if (devices <= 0) { fprintf(stderr, "no radio found\n"); return 1; }
  const DISCOVERED *dev = NULL;
  for (int i = 0; i < devices && !dev; i++) {
    if (strcmp(inet_ntoa(discovered[i].network.address.sin_addr), ipaddr_radio) == 0) {
      dev = &discovered[i];
    }
  }
  if (!dev) { dev = &discovered[0]; }
  int p1 = (dev->protocol == ORIGINAL_PROTOCOL);
  printf("using %s at %s (protocol %d)\n", dev->name,
         inet_ntoa(dev->network.address.sin_addr), p1 ? 1 : 2);
  if (dev->status == 3) {
    fprintf(stderr, "radio is IN USE by another program — close it first\n");
    return 1;
  }
  if (p1 && rate != 48000 && rate != 96000 && rate != 192000 && rate != 384000) {
    rate = 384000;   /* P1 tops out at 384 k (C&C 0x00-C1 bits1:0) */
    printf("P1 radio: sample rate capped to %d Hz\n", rate);
  }
  printf("RX %lld Hz @ %d Hz, mode %s (%.0f..%.0f Hz), vol %.0f dB, audio lat %d ms\n",
         freq, rate, mname, flo, fhi, vol, lat);

  if (audio_start(AUDIO_RATE, 2, lat, NULL) != 0) { fprintf(stderr, "audio_start failed\n"); return 2; }  /* stereo: demod feeds L/R */
  if (demod_create(0, rate, mode, flo, fhi, vol) != 0) { fprintf(stderr, "demod_create failed\n"); audio_stop(); return 3; }
  if ((p1 ? p1_rx_start(dev, freq, rate, feed_cb, NULL)
          : p2_rx_start(dev, freq, rate, feed_cb, NULL)) != 0) {
    fprintf(stderr, "%s failed\n", p1 ? "p1_rx_start" : "p2_rx_start");
    demod_destroy(); audio_stop();
    return 4;
  }

  printf("playing %d s — listen ...\n", secs);
  for (int i = 0; i < secs; i++) {
    usleep(1000000);
    fprintf(stderr, "  t=%2ds  audio peak=%.4f  queued=%d frames (~%.0f ms)  ferr=%d\n",
            i + 1, demod_peak(), audio_queued(),
            1000.0 * audio_queued() / AUDIO_RATE, demod_last_error());
  }

  if (p1) { p1_rx_stop(); } else { p2_rx_stop(); }
  demod_destroy();
  audio_stop();
  return 0;
}
