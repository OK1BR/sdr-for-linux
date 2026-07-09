/*
 * sdr-for-linux — offline CW timing gate (F6d, docs/TX-DESIGN.md).
 *
 * Proves the CW envelope generator keeps correct Morse rhythm WITHOUT a radio:
 *   1. dot length = sr·1.2/WPM for a range of speeds (the sample-clocked timing).
 *   2. the "PARIS" word rate: the distance between successive word starts is
 *      exactly 50 dot-units (the classic WPM definition) at every speed — this
 *      exercises intra-char / inter-char / inter-word gaps end to end.
 *   3. the envelope stays in [0,1] and settles to idle.
 * NO radio, NO socket, NO WDSP. Exit 0 = pass.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cw_gen.h"

#define SR 192000   /* TX IQ rate */

static int fails;
static void chk(const char *what, long got, long want, long tol) {
  long d = got - want; if (d < 0) { d = -d; }
  int ok = d <= tol;
  printf("  %-42s got=%ld want=%ld (tol %ld)  %s\n", what, got, want, tol, ok ? "ok" : "FAIL");
  if (!ok) { fails++; }
}

/* Pull the whole schedule in chunks; record the sample index of the first `max`
 * upward envelope crossings of 0.5 (mark onsets). Returns the number found. */
static int mark_onsets(cw_gen *g, long *onset, int max) {
  float buf[4096];
  double prev = 0.0;
  long pos = 0;
  int found = 0;
  int guard = 0;
  while (!cw_gen_idle(g) && guard++ < 100000) {
    int kd = cw_gen_pull(g, buf, 4096);
    (void)kd;
    for (int i = 0; i < 4096; i++) {
      double e = buf[i];
      if (e < 0.0 || e > 1.0001) { printf("  envelope out of range: %f\n", e); fails++; }
      if (prev < 0.5 && e >= 0.5 && found < max) { onset[found++] = pos + i; }
      prev = e;
    }
    pos += 4096;
  }
  return found;
}

int main(void) {
  const int wpms[] = { 12, 20, 25, 30, 40 };
  for (unsigned k = 0; k < sizeof(wpms) / sizeof(wpms[0]); k++) {
    int wpm = wpms[k];
    cw_gen *g = cw_gen_new(SR, wpm, 50.0, 9.0);
    if (!g) { printf("cw_gen_new failed\n"); return 2; }

    long dot = cw_gen_dot_samples(g);
    long want_dot = lround((double)SR * 1.2 / (double)wpm);
    char lbl[64];
    snprintf(lbl, sizeof lbl, "dot samples @ %d WPM", wpm);
    chk(lbl, dot, want_dot, 0);

    /* PARIS word rate: onset[0] = word-1 first mark, onset[14] = word-2 first mark
     * (PARIS has 14 marks). Their distance must be one word = 50 dots. */
    cw_gen_send_text(g, "PARIS PARIS");
    long on[16];
    int nf = mark_onsets(g, on, 16);
    snprintf(lbl, sizeof lbl, "PARIS marks seen @ %d WPM", wpm);
    chk(lbl, nf, 15, 1);   /* 14 in word 1 + first of word 2 */
    if (nf >= 15) {
      snprintf(lbl, sizeof lbl, "word period @ %d WPM (=50 dots)", wpm);
      chk(lbl, on[14] - on[0], 50 * dot, 2);   /* ±2 samples rounding */
    }
    if (!cw_gen_idle(g)) { printf("  not idle after drain @ %d WPM  FAIL\n", wpm); fails++; }
    cw_gen_free(g);
  }

  printf("\n=== CW timing: %s ===\n", fails ? "FAIL" : "PASS");
  if (!fails) {
    printf("PASS — Morse rhythm is sample-locked: dot = sr·1.2/WPM and the PARIS "
           "word rate is exactly 50 dots at every speed.\n");
  }
  return fails ? 1 : 0;
}
