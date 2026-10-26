/*
 * WDSP build/link smoke check.
 *
 * Not part of the app — a milestone gate. It includes WDSP's umbrella header
 * from *outside* the vendored library and calls into libwdsp, proving that the
 * vendored DSP (WDSP + libspecbleach) compiles, that its headers are
 * usable by our own code, and that the whole thing links. The real analyzer
 * path (XCreateAnalyzer/Spectrum0 -> rx->pixel_samples) lands in a later step.
 */
#include <stdio.h>

#include "comm.h" /* WDSP umbrella header — exercises the vendored headers */

extern int GetWDSPVersion(void);

int main(void) {
  int v = GetWDSPVersion();
  printf("WDSP link OK — GetWDSPVersion() = %d\n", v);
  /* Tripwire: the vendored copy is what VENDOR.md says (2.11 since issue #22). */
  if (v < 211) { printf("FAIL: expected WDSP >= 2.11 (211), got %d\n", v); return 1; }
  return 0;
}
