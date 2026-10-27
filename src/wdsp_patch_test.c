/*
 * sdrfl-wdsp-patch-test — pins the ONE deliberate local change in the vendored
 * WDSP (vendor/wdsp/VENDOR.md, "Local patches"):
 *
 *   calcc.c  #define PS_NF_MAG_PREFILT_YMAX  4.0   (upstream 2.11: 1.8)
 *
 * The PS 3.0 magnitude pre-filter drops every calibration point whose required
 * correction exceeds this factor; the ANAN G2E needs ~2.1x at low level, so with
 * 1.8 the lowest fifth of the range is left without data and every fit fails
 * BAD_CONDNUM (0/85 live on 2026-10-27). Reported upstream as
 * https://github.com/TAPR/OpenHPSDR-wdsp/issues/10.
 *
 * Why a gate: a re-sync of vendor/wdsp copies upstream verbatim and silently
 * reverts this line. This test reads calcc.c from the source tree and fails
 * when the value is not ours — so the re-sync either re-applies the patch or,
 * once upstream has resolved #10, drops it here AND in VENDOR.md on purpose.
 *
 * Pure file parsing — NO WDSP linked, NO radio. Exit 0 = pass.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SDRFL_CALCC_PATH
#error "SDRFL_CALCC_PATH must point at vendor/wdsp/calcc.c (set by meson.build)"
#endif

#define WANT 4.0
#define MARKER "sdr-for-linux LOCAL PATCH"

int main(void) {
  FILE *f = fopen(SDRFL_CALCC_PATH, "r");
  if (!f) { perror(SDRFL_CALCC_PATH); return 2; }
  char line[1024]; int defines = 0, marker = 0, fail = 0; double value = 0;
  while (fgets(line, sizeof line, f)) {
    if (strstr(line, MARKER)) marker++;
    const char *d = "#define PS_NF_MAG_PREFILT_YMAX";
    if (strncmp(line, d, strlen(d)) == 0) {
      defines++;
      if (sscanf(line + strlen(d), " %lf", &value) != 1) { printf("  FAIL  cannot parse: %s", line); fail++; }
    }
  }
  fclose(f);
  printf("  %s  PS_NF_MAG_PREFILT_YMAX defined exactly once          (%d)\n", defines == 1 ? "ok  " : "FAIL", defines);
  if (defines != 1) fail++;
  printf("  %s  value is the local patch %.1f                     (found %.3g)\n", fabs(value - WANT) < 1e-9 ? "ok  " : "FAIL", WANT, value);
  if (fabs(value - WANT) >= 1e-9) fail++;
  printf("  %s  marker comment present                            (%d)\n", marker >= 1 ? "ok  " : "FAIL", marker);
  if (marker < 1) fail++;
  if (fail) {
    printf("\nsdrfl-wdsp-patch-test: FAIL (%d) — vendor/wdsp/calcc.c no longer carries the local\n"
           "PS_NF_MAG_PREFILT_YMAX patch. A re-sync reverted it: re-apply it, or — if\n"
           "TAPR/OpenHPSDR-wdsp#10 is resolved upstream — drop the patch, this test and the\n"
           "VENDOR.md section together, on purpose.\n", fail);
    return 1;
  }
  printf("sdrfl-wdsp-patch-test: PASS (3 checks)\n");
  return 0;
}
