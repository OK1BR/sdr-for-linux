/*
 * sdrfl-nnr-test — offline gate for WDSP's neural noise reduction (NNR, the
 * NR slot 3 since the WDSP 2.11 re-sync, issue #22). NO radio, NO audio device.
 *
 * Opens an RXA channel exactly like demod.c (1024-sample IQ blocks at 192 kHz
 * in, dsp = out = the selectable AF rate) for each AF rate we offer (48/96/192
 * kHz) and, for every built-in model slot (0 Standard, 1 Premium, 2 Standard
 * CW, 3 Premium CW):
 *   - SetRXANNRModel must return the slot asked for (the model loaded and
 *     accepted the channel's rate), and the chain output must stay finite and
 *     non-silent with NNR running;
 *   - reports the CPU cost (fexchange0 wall time ÷ audio time) — the number
 *     Richard needs for the Premium-by-default decision — and the tone-to-
 *     total power ratio of a 1 kHz tone in white noise with NNR off vs on
 *     (report only: the voice models are trained on speech, a steady tone is
 *     not a spec for them).
 * Also times OpenChannel: every RXA open parses all four built-in models.
 *
 * Usage: ./build-release/sdrfl-nnr-test   (release build for honest timings)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wdsp.h"

#define IQ_RATE   192000
#define BUFSIZE   1024
#define DSPSIZE   2048
#define TONE_HZ   1000.0
#define SECS      6          /* audio seconds per measurement */
#define SKIP_SECS 1          /* settle time before measuring  */

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* Deterministic Gaussian noise (Box-Muller on a LCG) — same stream every run. */
static unsigned long long rng = 0x9E3779B97F4A7C15ULL;
static double urand(void) { rng = rng * 6364136223846793005ULL + 1442695040888963407ULL; return ((rng >> 11) + 0.5) / 9007199254740992.0; }
static double grand(void) { double u = urand(), v = urand(); return sqrt(-2.0 * log(u)) * cos(2.0 * M_PI * v); }

/* Goertzel power at f over n interleaved-stereo frames (left channel). */
static double goertzel(const double *lr, int n, double f, int rate) {
  double w = 2.0 * M_PI * f / rate, c = 2.0 * cos(w), s1 = 0, s2 = 0;
  for (int i = 0; i < n; i++) { double s0 = lr[2 * i] + c * s1 - s2; s2 = s1; s1 = s0; }
  return (s1 * s1 + s2 * s2 - c * s1 * s2) / ((double)n * n / 4.0);
}

typedef struct { double tone_db, total_db, cpu_pct, rms; int finite; } Meas;

static Meas run_chain(int ch, int out_frames, int arate, int nnr_on, int slot) {
  Meas m = { 0 };
  double *in  = g_new0(double, 2 * BUFSIZE);
  double *out = g_new0(double, 2 * out_frames);
  double ph = 0.0, dph = 2.0 * M_PI * TONE_HZ / IQ_RATE;
  long blocks = (long)SECS * IQ_RATE / BUFSIZE, skip = (long)SKIP_SECS * IQ_RATE / BUFSIZE;
  double tone_acc = 0, total_acc = 0; long meas = 0; double sumsq = 0; long nsamp = 0;
  gint64 busy = 0;
  SetRXANNRRun(ch, nnr_on);
  if (nnr_on) { int got = SetRXANNRModel(ch, slot); CHECK(got == slot, "slot %d at %d Hz: SetRXANNRModel returned %d", slot, arate, got); }
  for (long b = 0; b < blocks; b++) {
    for (int i = 0; i < BUFSIZE; i++) {               /* USB tone at +1 kHz + white noise, −30 dBFS each */
      in[2 * i]     = 0.03 * cos(ph) + 0.03 * grand();
      in[2 * i + 1] = 0.03 * sin(ph) + 0.03 * grand();
      ph += dph; if (ph > 2 * M_PI) { ph -= 2 * M_PI; }
    }
    int err = 0;
    gint64 t0 = g_get_monotonic_time();
    fexchange0(ch, in, out, &err);
    busy += g_get_monotonic_time() - t0;
    if (b < skip) { continue; }
    for (int i = 0; i < 2 * out_frames; i++) { if (!isfinite(out[i])) { m.finite = -1; } sumsq += out[i] * out[i]; }
    nsamp += 2 * out_frames;
    tone_acc  += goertzel(out, out_frames, TONE_HZ, arate);
    double tot = 0; for (int i = 0; i < out_frames; i++) { tot += out[2 * i] * out[2 * i]; }
    total_acc += tot / out_frames;
    meas++;
  }
  m.finite   = m.finite == 0;
  m.rms      = sqrt(sumsq / (double)nsamp);
  m.tone_db  = 10.0 * log10(tone_acc / meas + 1e-30);
  m.total_db = 10.0 * log10(total_acc / meas + 1e-30);
  m.cpu_pct  = 100.0 * (double)busy / (1e6 * SECS);
  g_free(in); g_free(out);
  return m;
}

int main(void) {
  const int rates[] = { 48000, 96000, 192000 };
  /* SDRFL_WISDOM_DIR=<dir with trailing />: import the FFTW wisdom cache first,
   * as the app does (wisdom_ensure) — without it the first OpenChannel pays
   * FFTW_PATIENT planning for every filter size (tens of seconds). */
  const char *wd = getenv("SDRFL_WISDOM_DIR");
  if (wd && *wd) { printf("wisdom: WDSPwisdom(%s) -> %d (0 = imported, 1 = rebuilt)\n", wd, WDSPwisdom((char *)wd)); }
  const char *names[] = { "Standard", "Premium", "Standard CW", "Premium CW" };
  printf("sdrfl-nnr-test — WDSP %d, NNR built-in models at the three AF rates\n", GetWDSPVersion());
  for (unsigned r = 0; r < G_N_ELEMENTS(rates); r++) {
    int arate = rates[r], ch = 0, out_frames = BUFSIZE / (IQ_RATE / arate);
    gint64 t0 = g_get_monotonic_time();
    OpenChannel(ch, BUFSIZE, DSPSIZE, IQ_RATE, arate, arate, 0, 1, 0.010, 0.025, 0.0, 0.010, 1);
    double open_ms = (g_get_monotonic_time() - t0) / 1e3;
    SetRXABandpassRun(ch, 1);
    SetRXAPanelRun(ch, 1);
    SetRXAPanelSelect(ch, 3);
    SetRXAPanelBinaural(ch, 1);     /* copy → L = R */
    SetRXAPanelGain1(ch, 1.0);
    SetRXAMode(ch, 1);              /* USB */
    RXASetPassband(ch, 150.0, 2850.0);
    SetRXAAGCMode(ch, 0);           /* linear chain for the level comparison */
    SetRXANNRMaskFloor(ch, -25.0);
    printf("\n[AF %d Hz] OpenChannel %.0f ms (parses the four models), %d out frames/block\n", arate, open_ms, out_frames);
    Meas off = run_chain(ch, out_frames, arate, 0, 0);
    CHECK(off.finite && off.rms > 1e-6, "NNR off: output not finite/silent at %d Hz", arate);
    printf("  NNR off      : tone %6.1f dB, total %6.1f dB, tone/total %5.1f dB, cpu %5.1f %%\n",
           off.tone_db, off.total_db, off.tone_db - off.total_db, off.cpu_pct);
    for (int slot = 0; slot < 4; slot++) {
      Meas on = run_chain(ch, out_frames, arate, 1, slot);
      CHECK(on.finite, "slot %d at %d Hz: non-finite output", slot, arate);
      CHECK(on.rms > 1e-6, "slot %d at %d Hz: silent output (rms %g)", slot, arate, on.rms);
      printf("  %-12s : tone %6.1f dB, total %6.1f dB, tone/total %5.1f dB, cpu %5.1f %%  (model %d)\n",
             names[slot], on.tone_db, on.total_db, on.tone_db - on.total_db, on.cpu_pct, GetRXANNRModel(ch));
    }
    SetChannelState(ch, 0, 1);
    CloseChannel(ch);
  }
  printf("\n%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
  return fails ? 1 : 0;
}
