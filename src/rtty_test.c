/*
 * sdr-for-linux — offline RTTY generator gate (docs/RTTY-SCOPE.md §0).
 *
 * Proves rtty_gen produces decodable, clean 45.45 Bd / 170 Hz Baudot FSK
 * WITHOUT a radio:
 *   1. ITA2 encoder truth on hardcoded bit vectors (R=01010, Y=10101,
 *      FIGS→1 = 11011→11101, LSB transmitted first) + FIGS/LTRS insertion
 *      with unshift-on-space — independent witnesses, not the encode tables
 *      round-tripping themselves.
 *   2. Full-chain round trip at BOTH runtime IQ rates (P1 48 k, P2 192 k):
 *      generated IQ → an in-test FSK slicer + asynchronous UART framer +
 *      ITA2 decoder (an independent implementation) → exact text equality.
 *   3. Signal hygiene: constant unit envelope between the key-on/key-off
 *      ramps, phase-continuous across every bit edge, per-bit instantaneous
 *      frequency = mark +85 / space −85 Hz, steady-mark preamble before the
 *      first start bit, ~1-bit mark tail before unkey.
 *   4. Abort ramps down within one block; leading-space idle rule
 *      (cw_gen_send_text parity, TX-DESIGN §10); HUD progress bookkeeping.
 * NO radio, NO socket, NO WDSP. Exit 0 = pass.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtty_gen.h"

#define BAUD  45.45
#define SHIFT 170.0

static int fails;
static void chk(const char *what, long got, long want, long tol) {
  long d = got - want; if (d < 0) { d = -d; }
  int ok = d <= tol;
  printf("  %-52s got=%ld want=%ld (tol %ld)  %s\n", what, got, want, tol,
         ok ? "ok" : "FAIL");
  if (!ok) { fails++; }
}
static void chks(const char *what, const char *got, const char *want) {
  int ok = strcmp(got, want) == 0;
  printf("  %-52s got=\"%s\" want=\"%s\"  %s\n", what, got, want, ok ? "ok" : "FAIL");
  if (!ok) { fails++; }
}

/* --- independent witness: FSK slicer + UART framer + ITA2 decoder ---------- */

/* Same table as the family decoder (skimmer-for-linux decode_rtty.c); its
 * agreement with the generator is proven by the hardcoded vectors in part 1,
 * not by this copy. */
static const char W_LTRS[32] = {
  0,   'E', '\n', 'A', ' ', 'S', 'I', 'U',
  0,   'D', 'R',  'J', 'N', 'F', 'C', 'K',
  'T', 'Z', 'L',  'W', 'H', 'Y', 'P', 'Q',
  'O', 'B', 'G',  0,   'M', 'X', 'V', 0,
};
static const char W_FIGS[32] = {
  0,   '3', '\n', '-', ' ', 0,   '8', '7',
  0,   '$', '4',  '\'', ',', '!', ':', '(',
  '5', '"', ')',  '2', '#', '6', '0', '1',
  '9', '?', '&',  0,   '.', '/', ';', 0,
};

typedef struct {
  const double *iq;   /* interleaved pairs */
  long          n;    /* pairs */
  int           sr;
} Wit;

static double w_mag(const Wit *w, long i) {
  return hypot(w->iq[2 * i], w->iq[2 * i + 1]);
}
/* Instantaneous frequency at sample i (Hz), from the phase step i-1 → i. */
static double w_freq(const Wit *w, long i) {
  double a = atan2(w->iq[2 * i + 1], w->iq[2 * i]);
  double b = atan2(w->iq[2 * i - 1], w->iq[2 * i - 2]);
  double d = a - b;
  while (d >  M_PI) { d -= 2.0 * M_PI; }
  while (d < -M_PI) { d += 2.0 * M_PI; }
  return d * (double)w->sr / (2.0 * M_PI);
}
static int w_mark(const Wit *w, long i) { return w_freq(w, i) > 0.0; }

/* Decode every framed character; also record each start-edge position.
 * Returns chars written to out. Frames with a bad stop bit count as fails. */
static int w_decode(const Wit *w, char *out, int outlen,
                    long *edges, int maxedges, int *nedges) {
  double spb = (double)w->sr / BAUD;
  int shift = 0, no = 0, ne = 0;
  long i = 1;
  while (i < w->n) {
    /* hunt: keyed mark → space transition = a start edge */
    if (w_mag(w, i) < 0.5 || !(!w_mark(w, i) && w_mark(w, i - 1))) { i++; continue; }
    long e = i;
    if (ne < maxedges) { edges[ne] = e; }
    ne++;
    /* confirm the start bit + sample data/stop at bit centres */
    if (w_mag(w, e + (long)(0.5 * spb)) < 0.5 || w_mark(w, e + (long)(0.5 * spb))) {
      i++; continue;                       /* not a clean start — keep hunting */
    }
    unsigned code = 0;
    for (int b = 0; b < 5; b++) {
      long p = e + (long)((1.5 + b) * spb);
      if (p >= w->n) { break; }
      if (w_mark(w, p)) { code |= 1u << b; }
    }
    long ps = e + (long)(6.75 * spb);      /* stop-bit centre */
    if (ps < w->n && !w_mark(w, ps)) {
      printf("  witness: BAD STOP at sample %ld  FAIL\n", ps); fails++;
    }
    if (code == 0x1F)      { shift = 0; }
    else if (code == 0x1B) { shift = 1; }
    else {
      char c = shift ? W_FIGS[code] : W_LTRS[code];
      if (code == 0x04) { shift = 0; }     /* unshift-on-space */
      if (c && no < outlen - 1) { out[no++] = c; }
    }
    i = e + (long)(7.45 * spb);            /* resume just before the next edge */
  }
  out[no] = '\0';
  if (nedges) { *nedges = ne; }
  return no;
}

/* Pull the generator dry (guarded) into a malloc'd buffer; returns pairs. */
static long drain(rtty_gen *g, double **out, int block) {
  long cap = 1 << 22, n = 0;               /* 4 M pairs ≈ 22 s @ 192k */
  double *buf = malloc((size_t)cap * 2 * sizeof(double));
  if (!buf) { printf("oom\n"); exit(2); }
  int guard = 0;
  while (!rtty_gen_idle(g) && guard++ < 100000) {
    if (n + block > cap) { break; }
    rtty_gen_pull(g, buf + 2 * n, block);
    n += block;
  }
  *out = buf;
  return n;
}

static void bits_of(unsigned char code, char *s) {   /* LSB first, as transmitted */
  for (int b = 0; b < 5; b++) { s[b] = (char)('0' + ((code >> b) & 1)); }
  s[5] = '\0';
}

int main(void) {
  /* --- 1. ITA2 encoder truth (hardcoded independent vectors) -------------- */
  {
    unsigned char c[2]; char bs[6]; int figs = 0;
    chk("encode R: one code", rtty_encode_char('R', &figs, c), 1, 0);
    bits_of(c[0], bs); chks("encode R bits (LSB first)", bs, "01010");
    chk("encode Y: one code", rtty_encode_char('Y', &figs, c), 1, 0);
    bits_of(c[0], bs); chks("encode Y bits", bs, "10101");
    chk("encode '1' from LTRS: two codes", rtty_encode_char('1', &figs, c), 2, 0);
    bits_of(c[0], bs); chks("encode '1' shift char = FIGS", bs, "11011");
    bits_of(c[1], bs); chks("encode '1' code", bs, "11101");
    chk("shift state now FIGS", figs, 1, 0);
    chk("encode '9' inside FIGS: one code", rtty_encode_char('9', &figs, c), 1, 0);
    chk("encode space: one code", rtty_encode_char(' ', &figs, c), 1, 0);
    chk("space code = 0x04", c[0], 0x04, 0);
    chk("unshift-on-space: state back to LTRS", figs, 0, 0);
    chk("encode '1' again re-emits FIGS", rtty_encode_char('1', &figs, c), 2, 0);
    figs = 1;
    chk("encode A from FIGS: two codes", rtty_encode_char('A', &figs, c), 2, 0);
    chk("A shift char = LTRS", c[0], 0x1F, 0);
    chk("A code = 0x03", c[1], 0x03, 0);
    chk("unknown char skipped", rtty_encode_char('%', &figs, c), 0, 0);
  }

  /* --- 2 + 3. Round trip + signal hygiene at both runtime IQ rates -------- */
  const int rates[]  = { 48000, 192000 };
  const int blocks[] = { 512,   2048   };   /* the tx_run block sizes */
  for (unsigned k = 0; k < sizeof(rates) / sizeof(rates[0]); k++) {
    int sr = rates[k], block = blocks[k];
    double spb = (double)sr / BAUD;
    printf("--- IQ rate %d (block %d) ---\n", sr, block);
    rtty_gen *g = rtty_gen_new(sr);
    if (!g) { printf("rtty_gen_new failed\n"); return 2; }

    rtty_gen_send_text(g, "CQ TEST DE OK1BR 599 001");
    double *iq; long n = drain(g, &iq, block);
    Wit w = { iq, n, sr };

    char txt[128]; long edges[512]; int ne;
    w_decode(&w, txt, sizeof txt, edges, 512, &ne);
    chks("round trip text", txt, "CQ TEST DE OK1BR 599 001");

    /* keyed extent + preamble: first/last sample with envelope > 0.5 */
    long first = -1, last = -1;
    for (long i = 0; i < n; i++) {
      if (w_mag(&w, i) > 0.5) { if (first < 0) { first = i; } last = i; }
    }
    chk("keyed from the start (preamble, not silence)", first < (long)spb, 1, 0);
    /* preamble: solid mark from key-on to the first start edge, ~100 ms */
    long pre = edges[0] - first;
    chk("mark preamble before the first start bit", pre, sr / 10, sr / 100);
    int pre_ok = 1;
    for (long i = first + sr / 200; i < edges[0]; i++) {   /* skip the ramp */
      if (!w_mark(&w, i)) { pre_ok = 0; break; }
    }
    chk("preamble is steady MARK", pre_ok, 1, 0);
    double fpre = 0.0; long npre = 0;
    for (long i = first + sr / 200; i < edges[0]; i++) { fpre += w_freq(&w, i); npre++; }
    chk("preamble frequency = +85 Hz (mark = higher RF)",
        lround(fpre / (double)npre), 85, 1);

    /* per-bit frequency: a start bit is space — measure one mid-bit */
    double fsp = 0.0;
    for (int s = 0; s < 8; s++) { fsp += w_freq(&w, edges[0] + (long)(0.4 * spb) + s); }
    chk("start-bit frequency = -85 Hz (space)", lround(fsp / 8.0), -85, 1);

    /* constant envelope between the ramps; phase step bounded (no clicks) */
    long ramp = sr / 100;                       /* > the 5 ms ramp */
    int env_ok = 1, ph_ok = 1;
    double wmax = 2.0 * M_PI * (SHIFT / 2.0 + 2.0) / (double)sr;
    for (long i = first + ramp; i <= last - ramp; i++) {
      double m = w_mag(&w, i);
      if (m < 0.999 || m > 1.001) { env_ok = 0; }
      double a = atan2(w.iq[2*i+1], w.iq[2*i]), b = atan2(w.iq[2*i-1], w.iq[2*i-2]);
      double d = a - b;
      while (d >  M_PI) { d -= 2.0 * M_PI; }
      while (d < -M_PI) { d += 2.0 * M_PI; }
      if (fabs(d) > wmax) { ph_ok = 0; }
    }
    chk("constant unit envelope between ramps", env_ok, 1, 0);
    chk("phase-continuous across every bit edge", ph_ok, 1, 0);

    /* char cadence: consecutive start edges of butted chars = 7.5 bits */
    long d01 = edges[1] - edges[0];
    chk("char frame = 7.5 bits (start..start)", d01, lround(7.5 * spb), 2);

    /* mark tail: the text ends in '1' (10111, LSB first) whose last SPACE bit
     * is b3 — after it come b4 (1) + stop (1.5) + the ~1-bit tail = 3.5 bits
     * of solid mark before the envelope ramps down. */
    long last_space = -1;
    for (long i = last; i > first; i--) {
      if (w_mag(&w, i) > 0.5 && !w_mark(&w, i)) { last_space = i; break; }
    }
    chk("mark tail after the last space bit (b4+stop+~1 bit)",
        last - last_space, lround(3.5 * spb), lround(spb / 2.0));

    if (!rtty_gen_idle(g)) { printf("  not idle after drain  FAIL\n"); fails++; }
    free(iq);

    /* leading-space idle rule + mid-queue space kept ----------------------- */
    rtty_gen_send_text(g, "  A");
    rtty_gen_send_text(g, " B");            /* queue busy → the space is real */
    n = drain(g, &iq, block);
    w.iq = iq; w.n = n;
    w_decode(&w, txt, sizeof txt, edges, 512, &ne);
    chks("leading space skipped when idle, kept mid-queue", txt, "A B");
    free(iq);

    /* abort: flush must settle the envelope within ONE block --------------- */
    rtty_gen_send_text(g, "CQCQCQCQCQCQCQCQ");
    double *b1 = malloc((size_t)block * 2 * sizeof(double));
    rtty_gen_pull(g, b1, block);            /* into the preamble/first bits */
    rtty_gen_pull(g, b1, block);
    rtty_gen_flush(g);
    rtty_gen_pull(g, b1, block);
    int step_ok = 1;
    for (int i = 1; i < block; i++) {
      double m1 = hypot(b1[2*i], b1[2*i+1]), m0 = hypot(b1[2*i-2], b1[2*i-1]);
      if (fabs(m1 - m0) > 0.02) { step_ok = 0; }
    }
    chk("abort ramps down within one block (idle)", rtty_gen_idle(g), 1, 0);
    chk("abort has no envelope step (no key click)", step_ok, 1, 0);
    free(b1);

    rtty_gen_free(g);
  }

  /* --- 4. HUD progress bookkeeping (cw_gen_progress contract) -------------- */
  {
    int sr = 48000;
    double spb = (double)sr / BAUD;
    rtty_gen *g = rtty_gen_new(sr);
    char buf[64]; int cur = -1;
    rtty_gen_send_text(g, " AB 599");
    long qn = rtty_gen_progress(g, buf, sizeof buf, &cur);
    chks("progress: queued text (shifts invisible)", buf, "AB 599");
    chk("progress: playhead at 0", cur, 0, 0);
    double *tmp = malloc(4096 * 2 * sizeof(double));
    long pulled = 0;
    /* preamble (100 ms) + LTRS + A = 0.1·sr + 2×7.5 bits; pull past A */
    long tgt = sr / 10 + (long)(15.5 * spb);
    while (pulled < tgt) {
      long c = tgt - pulled; if (c > 4096) { c = 4096; }
      rtty_gen_pull(g, tmp, (int)c); pulled += c;
    }
    rtty_gen_progress(g, buf, sizeof buf, &cur);
    chk("progress: 'A' done after its frame", cur, 1, 0);
    rtty_gen_flush(g);
    rtty_gen_progress(g, buf, sizeof buf, &cur);
    chk("progress: flush clears the text", (long)strlen(buf), 0, 0);
    (void)qn;
    free(tmp);
    rtty_gen_free(g);
  }

  printf("\n=== RTTY generator: %s ===\n", fails ? "FAIL" : "PASS");
  if (!fails) {
    printf("PASS — ITA2 truth vectors hold, and the generated FSK decodes "
           "exactly through an independent slicer at 48 k and 192 k with a "
           "constant, phase-continuous envelope.\n");
  }
  return fails ? 1 : 0;
}
