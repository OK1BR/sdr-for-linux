/*
 * sdr-for-linux — RTTY (Baudot FSK) IQ generator. See rtty_gen.h.
 *
 * Timing is entirely in SAMPLE COUNTS: send_text() turns characters into a
 * FIFO of (mark, length-in-samples) segments — 1 start (space) + 5 data
 * (LSB first) + 1.5 stop (mark) per ITA2 code at 45.45 Bd, the fractional
 * samples-per-bit carried by a running accumulator so the long-run baud rate
 * is exact at any IQ rate. pull() walks the FIFO one sample at a time,
 * advancing ONE phase accumulator whose frequency toggles ±85 Hz — the FSK is
 * phase-continuous by construction — under a raised-cosine envelope that
 * moves only at key-on/key-off. PURE: no radio, no threads, no WDSP.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rtty_gen.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RTTY_BAUD        45.45   /* fixed by decision (RTTY-SCOPE §7E)          */
#define RTTY_SHIFT_HZ    170.0   /* mark = +85 Hz, space = −85 Hz around 0      */
#define RTTY_RAMP_MS     5.0     /* key-on/off raised-cosine amplitude ramp     */
#define RTTY_PREAMBLE_MS 100.0   /* steady mark before the first start bit      */
#define RTTY_TAIL_BITS   1.0     /* steady mark after the last stop bit         */

#define ITA2_FIGS 0x1B
#define ITA2_LTRS 0x1F
#define ITA2_SP   0x04
#define ITA2_LF   0x02

/* ITA2 (CCITT-2), code LSB = FIRST transmitted bit; 0 = unassigned. The exact
 * tables skimmer-for-linux decodes (decode_rtty.c) — US-TTY figures, the ham
 * convention. */
static const char ita2_ltrs[32] = {
  0,   'E', '\n', 'A', ' ', 'S', 'I', 'U',
  0,   'D', 'R',  'J', 'N', 'F', 'C', 'K',
  'T', 'Z', 'L',  'W', 'H', 'Y', 'P', 'Q',
  'O', 'B', 'G',  0,   'M', 'X', 'V', 0,
};
static const char ita2_figs[32] = {
  0,   '3', '\n', '-', ' ', 0,   '8', '7',
  0,   '$', '4',  '\'', ',', '!', ':', '(',
  '5', '"', ')',  '2', '#', '6', '0', '1',
  '9', '?', '&',  0,   '.', '/', ';', 0,
};

int rtty_encode_char(char c, int *figs, unsigned char out[2]) {
  int n = 0;
  char u = (char)toupper((unsigned char)c);
  if (u == '\t') { u = ' '; }
  if (u == ' ') {              /* valid in both shifts; unshift-on-space */
    out[n++] = ITA2_SP;
    if (figs) { *figs = 0; }
    return n;
  }
  if (u == '\n') { out[n++] = ITA2_LF; return n; }   /* both shifts, no change */
  for (int i = 0; i < 32; i++) {
    if (ita2_ltrs[i] == u && u != 0) {
      if (figs && *figs) { out[n++] = ITA2_LTRS; *figs = 0; }
      out[n++] = (unsigned char)i;
      return n;
    }
  }
  for (int i = 0; i < 32; i++) {
    if (ita2_figs[i] == u && u != 0) {
      if (figs && !*figs) { out[n++] = ITA2_FIGS; *figs = 1; }
      out[n++] = (unsigned char)i;
      return n;
    }
  }
  return 0;                    /* unmappable → skipped (cw_gen parity) */
}

typedef struct { unsigned char mark; int len; } rseg;
typedef struct { char ch; long long end; } rch;   /* queued char + schedule end */

struct rtty_gen {
  int    sr;
  double spb;        /* samples per bit (fractional)                    */
  double bitfrac;    /* running fractional-sample accumulator           */
  int    ramp_n;     /* rise/fall length, samples                       */
  int    preamble_n; /* steady-mark preamble, samples                   */
  int    figs;       /* encoder shift state (1 = FIGS)                  */

  rseg  *q;          /* segment FIFO ring                               */
  int    cap, head, tail;

  unsigned char cur_mark;
  int    cur_rem, have_cur;
  int    tail_rem;   /* mark-tail samples left once the queue drained   */
  double ramp_pos;   /* 0..ramp_n                                       */
  double ph;         /* NCO phase (radians) — NEVER reset while running */

  /* Progress bookkeeping (HUD; the cw_gen scheme verbatim). */
  long long clock;
  long long sched_end;
  rch   *cq;
  int    ccap, chead, ctail;
};

rtty_gen *rtty_gen_new(int sample_rate) {
  if (sample_rate < 8000) { return NULL; }
  rtty_gen *g = calloc(1, sizeof *g);
  if (!g) { return NULL; }
  g->sr  = sample_rate;
  g->spb = (double)sample_rate / RTTY_BAUD;
  g->ramp_n     = (int)lround((double)sample_rate * RTTY_RAMP_MS / 1000.0);
  g->preamble_n = (int)lround((double)sample_rate * RTTY_PREAMBLE_MS / 1000.0);
  g->cap = 256;
  g->q = malloc((size_t)g->cap * sizeof(rseg));
  if (!g->q) { free(g); return NULL; }
  g->ccap = 256;
  g->cq = malloc((size_t)g->ccap * sizeof(rch));
  if (!g->cq) { free(g->q); free(g); return NULL; }
  return g;
}

void rtty_gen_free(rtty_gen *g) { if (g) { free(g->q); free(g->cq); free(g); } }

static void push_seg(rtty_gen *g, int mark, int len) {
  if (len <= 0) { return; }
  int next = (g->tail + 1) % g->cap;
  if (next == g->head) {                     /* ring full → grow */
    int ncap = g->cap * 2;
    rseg *nq = malloc((size_t)ncap * sizeof(rseg));
    if (!nq) { return; }                     /* drop on OOM (never keys extra) */
    int n = 0;
    for (int i = g->head; i != g->tail; i = (i + 1) % g->cap) { nq[n++] = g->q[i]; }
    free(g->q);
    g->q = nq; g->cap = ncap; g->head = 0; g->tail = n;
    next = (g->tail + 1) % g->cap;
  }
  g->q[g->tail].mark = (unsigned char)(mark ? 1 : 0);
  g->q[g->tail].len  = len;
  g->tail = next;
  g->sched_end += len;
}

/* Push `nbits` bit-lengths of one polarity, exact through the accumulator. */
static void push_bits(rtty_gen *g, int mark, double nbits) {
  double want = nbits * g->spb + g->bitfrac;
  int len = (int)want;
  g->bitfrac = want - (double)len;
  push_seg(g, mark, len);
}

/* One framed ITA2 code: start (space) + 5 data LSB-first + 1.5 stop (mark).
 * Consecutive equal-polarity bits are pushed as one segment (fewer segments,
 * identical waveform). */
static void push_code(rtty_gen *g, unsigned char code) {
  double run = 1.0;            /* pending run of equal bits, starts with start */
  int    run_mark = 0;         /* start bit = space                            */
  for (int b = 0; b < 5; b++) {
    int m = (code >> b) & 1;
    if (m == run_mark) { run += 1.0; continue; }
    push_bits(g, run_mark, run);
    run = 1.0; run_mark = m;
  }
  if (run_mark) { run += 1.5; push_bits(g, 1, run); }        /* data run + stop */
  else          { push_bits(g, 0, run); push_bits(g, 1, 1.5); }
}

static void push_char_rec(rtty_gen *g, char ch) {
  int next = (g->ctail + 1) % g->ccap;
  if (next == g->chead) { g->chead = (g->chead + 1) % g->ccap; }
  g->cq[g->ctail].ch  = ch;
  g->cq[g->ctail].end = g->sched_end;
  g->ctail = next;
}

void rtty_gen_send_text(rtty_gen *g, const char *text) {
  if (!g || !text) { return; }
  /* A send starting from IDLE: skip leading whitespace (the gap already
   * elapsed as real silence — TX-DESIGN §10 tripwire), drop the previous
   * over's HUD record, and open the over with the steady-mark preamble plus
   * one LTRS (parks the receiver's shift; ours resets with it). Mid-queue
   * sends append — whitespace there is genuine (transmitted space chars). */
  int at_start = rtty_gen_idle(g);
  if (at_start) { g->chead = g->ctail = 0; }
  if (g->sched_end < g->clock) { g->sched_end = g->clock; }
  for (const char *p = text; *p; p++) {
    if (at_start && (*p == ' ' || *p == '\t' || *p == '\n')) { continue; }
    unsigned char codes[2];
    int figs = g->figs;
    int nc = rtty_encode_char(*p, &figs, codes);
    if (!nc) { continue; }
    if (at_start) {
      push_seg(g, 1, g->preamble_n);
      g->figs = 0;
      figs = 0;                              /* re-encode against LTRS state */
      nc = rtty_encode_char(*p, &figs, codes);
      push_code(g, ITA2_LTRS);
      at_start = 0;
    }
    g->figs = figs;
    for (int i = 0; i < nc; i++) { push_code(g, codes[i]); }
    push_char_rec(g, (char)toupper((unsigned char)*p));
  }
}

void rtty_gen_flush(rtty_gen *g) {
  if (!g) { return; }
  g->head = g->tail = 0; g->have_cur = 0; g->cur_rem = 0;
  g->tail_rem = 0; g->bitfrac = 0.0;
  g->chead = g->ctail = 0; g->sched_end = g->clock;
}

int rtty_gen_pull(rtty_gen *g, double *iq, int n_pairs) {
  if (!g || !iq || n_pairs <= 0) { return 0; }
  int sending = 0;
  double w_mark  = 2.0 * M_PI * ( RTTY_SHIFT_HZ / 2.0) / (double)g->sr;
  double w_space = 2.0 * M_PI * (-RTTY_SHIFT_HZ / 2.0) / (double)g->sr;
  for (int i = 0; i < n_pairs; i++) {
    if (!g->have_cur && g->head != g->tail) {   /* pop next segment */
      g->cur_mark = g->q[g->head].mark;
      g->cur_rem  = g->q[g->head].len;
      g->head = (g->head + 1) % g->cap;
      g->have_cur = 1;
      g->tail_rem = 0;
    }
    int target, mark;
    if (g->have_cur)          { target = 1; mark = g->cur_mark; }
    else if (g->tail_rem > 0) { target = 1; mark = 1; g->tail_rem--; }
    else                      { target = 0; mark = 1; }
    if (g->ramp_n <= 0) {
      g->ramp_pos = target ? 1.0 : 0.0;
    } else if (target && g->ramp_pos < g->ramp_n) {
      g->ramp_pos += 1.0;
    } else if (!target && g->ramp_pos > 0.0) {
      g->ramp_pos -= 1.0;
    }
    double frac = g->ramp_n > 0 ? g->ramp_pos / (double)g->ramp_n : g->ramp_pos;
    double env  = 0.5 - 0.5 * cos(M_PI * frac);   /* raised cosine, 0..1 */
    g->ph += mark ? w_mark : w_space;
    if (g->ph >  M_PI) { g->ph -= 2.0 * M_PI; }
    if (g->ph < -M_PI) { g->ph += 2.0 * M_PI; }
    iq[2 * i]     = env * cos(g->ph);
    iq[2 * i + 1] = env * sin(g->ph);
    if (target) { sending++; }
    if (g->have_cur && --g->cur_rem <= 0) {
      g->have_cur = 0;
      if (g->head == g->tail) {               /* queue drained → mark tail */
        g->tail_rem = (int)lround(RTTY_TAIL_BITS * g->spb);
      }
    }
  }
  g->clock += n_pairs;
  return sending;
}

int rtty_gen_progress(rtty_gen *g, char *buf, int buflen, int *cur) {
  if (cur) { *cur = 0; }
  if (buf && buflen > 0) { buf[0] = '\0'; }
  if (!g || !buf || buflen < 2) { return 0; }
  int sent = 0;
  for (int i = g->chead; i != g->ctail; i = (i + 1) % g->ccap) {
    if (g->cq[i].end <= g->clock) { sent++; } else { break; }
  }
  int n = 0;
  for (int i = g->chead; i != g->ctail && n < buflen - 1; i = (i + 1) % g->ccap) {
    buf[n++] = g->cq[i].ch;
  }
  buf[n] = '\0';
  if (cur) { *cur = sent < n ? sent : n; }
  return n;
}

int rtty_gen_idle(const rtty_gen *g) {
  if (!g) { return 1; }
  return (g->head == g->tail) && !g->have_cur && g->tail_rem <= 0 &&
         g->ramp_pos <= 0.0;
}
