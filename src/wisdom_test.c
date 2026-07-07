/*
 * sdrfl-wisdom-test — exercise the first-run FFTW wisdom gate WITHOUT the radio.
 *
 * Calls wisdom_ensure() and exits. On the first run (no cache) it shows the
 * progress window and builds the wisdom cache; on a second run it imports the
 * cache instantly. Point it at a throwaway dir so it doesn't touch the real
 * config, and time both runs:
 *
 *   SDRFL_WISDOM_DIR=/tmp/wtest ./build/sdrfl-wisdom-test   # first: builds
 *   SDRFL_WISDOM_DIR=/tmp/wtest ./build/sdrfl-wisdom-test   # second: instant
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <glib.h>
#include <stdio.h>

#include "wisdom_gate.h"

int main(void) {
  gint64 t0 = g_get_monotonic_time();
  printf("wisdom_ensure() ...\n");
  wisdom_ensure();
  double secs = (g_get_monotonic_time() - t0) / 1e6;
  printf("done in %.1f s\n", secs);
  return 0;
}
