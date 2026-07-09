/*
 * sdrfl-micprobe — headless PipeWire microphone capture check (F6c-1 gate).
 *
 * Opens the default capture source via mic_pw.c and prints a live VU meter
 * (RMS + peak) for a few seconds so you can confirm the host mic path works and
 * gauge levels before wiring it to the exciter.
 *
 *   NO RADIO, NO TX — this only reads the microphone. Speak into the mic and the
 *   bar should move.
 *
 * Env: SDRFL_MIC_SECS (5), SDRFL_MIC_LAT (20 ms capture-latency target),
 *      SDRFL_MIC_RATE (48000), SDRFL_MIC_TARGET (PW node name; empty = default).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mic_pw.h"

#define BLOCK  512

static int env_int(const char *k, int dflt) {
  const char *v = getenv(k);
  return (v && *v) ? atoi(v) : dflt;
}

int main(void) {
  int secs = env_int("SDRFL_MIC_SECS", 5);
  int lat  = env_int("SDRFL_MIC_LAT", 20);
  int rate = env_int("SDRFL_MIC_RATE", 48000);
  const char *target = getenv("SDRFL_MIC_TARGET");   /* NULL/"" = default source */

  printf("=== sdrfl-micprobe — PipeWire mic capture (F6c-1) ===\n");
  printf("Capturing %d s @ %d Hz mono from %s. Speak into the mic.\n\n",
         secs, rate, (target && *target) ? target : "the default source");

  if (mic_start(rate, lat, target) != 0) {
    fprintf(stderr, "mic_start failed — no capture source?\n");
    return 1;
  }

  float buf[BLOCK];
  int   ticks = secs * 20;            /* ~20 prints/s */
  int   got_any = 0;
  double sumsq = 0.0; long nsq = 0;
  for (int k = 0; k < ticks; k++) {
    /* Drain whatever is queued this tick into an RMS accumulator. */
    int n;
    while ((n = mic_pull(buf, BLOCK)) > 0) {
      got_any = 1;
      for (int i = 0; i < n; i++) { sumsq += (double)buf[i] * buf[i]; }
      nsq += n;
      if (n < BLOCK) { break; }
    }
    double rms  = nsq ? sqrt(sumsq / (double)nsq) : 0.0;
    float  peak = mic_peak();
    int bars = (int)(peak * 40.0f + 0.5f);
    if (bars > 40) { bars = 40; }
    char meter[41];
    for (int i = 0; i < 40; i++) { meter[i] = i < bars ? '#' : '.'; }
    meter[40] = '\0';
    printf("\r[%s] peak=%.3f rms=%.3f  q=%d   ", meter, peak, rms, mic_queued());
    fflush(stdout);
    sumsq = 0.0; nsq = 0;
    usleep(50000);
  }

  mic_stop();
  printf("\n\n%s\n", got_any
         ? "PASS — capture stream delivered samples."
         : "WARN — no samples pulled (mic muted / no default source?).");
  return got_any ? 0 : 2;
}
