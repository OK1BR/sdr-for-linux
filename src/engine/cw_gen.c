/*
 * sdr-for-linux — CW (Morse) envelope generator. See cw_gen.h.
 *
 * Timing is entirely in SAMPLE COUNTS: send_text() turns characters into a FIFO of
 * (keydown, length-in-samples) segments using the standard Morse unit grid
 * (dot=1, dash=3, intra-char gap=1, inter-char=3, inter-word=7). pull() walks that
 * FIFO one sample at a time and applies a raised-cosine rise/fall, so element edges
 * land on exact sample boundaries — the caller pulls in lockstep with the radio's
 * fixed-rate TX-IQ clock, giving jitter-free rhythm at any WPM. PURE: no radio, no
 * threads, no WDSP.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "cw_gen.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct { int down; int len; } seg;
typedef struct { char ch; long long end; } cwch;   /* queued char + schedule end pos */

struct cw_gen {
  int    sr;
  int    dot;        /* dot length, samples */
  int    dash;       /* dash length, samples */
  double weight;
  double ramp_ms;
  int    ramp_n;     /* rise/fall length, samples (<= dot) */

  seg   *q;          /* segment FIFO ring */
  int    cap, head, tail;   /* head = next to pop, tail = next to push */

  int    cur_down, cur_rem; /* segment currently being emitted */
  int    have_cur;
  double ramp_pos;   /* 0..ramp_n */

  /* Progress bookkeeping for the CW TX HUD (contest note #7): the sample clock
   * advanced by pull(), the schedule end position advanced by push_seg, and a
   * ring of the CURRENT over's characters annotated with where in the schedule
   * each one finishes sounding (a send from idle starts a new over and clears
   * the ring). Chars with end <= clock have been sent. Display only — nothing
   * here feeds back into the envelope. */
  long long clock;      /* samples emitted since creation                     */
  long long sched_end;  /* clock position where the queued schedule ends      */
  cwch  *cq;            /* queued-text ring */
  int    ccap, chead, ctail;
};

/* ASCII -> Morse (dots/dashes). Index 32..90 (space..Z); lowercase folded up. */
static const char *morse_of(char c) {
  switch (toupper((unsigned char)c)) {
    case 'A': return ".-";     case 'B': return "-...";   case 'C': return "-.-.";
    case 'D': return "-..";    case 'E': return ".";      case 'F': return "..-.";
    case 'G': return "--.";    case 'H': return "....";   case 'I': return "..";
    case 'J': return ".---";   case 'K': return "-.-";    case 'L': return ".-..";
    case 'M': return "--";     case 'N': return "-.";     case 'O': return "---";
    case 'P': return ".--.";   case 'Q': return "--.-";   case 'R': return ".-.";
    case 'S': return "...";    case 'T': return "-";      case 'U': return "..-";
    case 'V': return "...-";   case 'W': return ".--";    case 'X': return "-..-";
    case 'Y': return "-.--";   case 'Z': return "--..";
    case '0': return "-----";  case '1': return ".----";  case '2': return "..---";
    case '3': return "...--";  case '4': return "....-";  case '5': return ".....";
    case '6': return "-....";  case '7': return "--...";  case '8': return "---..";
    case '9': return "----.";
    case '.': return ".-.-.-"; case ',': return "--..--"; case '?': return "..--..";
    case '/': return "-..-.";  case '=': return "-...-";  case '+': return ".-.-.";
    case '-': return "-....-"; case '(': return "-.--.";  case ')': return "-.--.-";
    case ':': return "---..."; case '\'': return ".----."; case '@': return ".--.-.";
    default:  return NULL;     /* unknown -> skipped */
  }
}

static void set_timing(cw_gen *g, int wpm, double weight) {
  if (wpm < 1)  { wpm = 1; }
  if (wpm > 60) { wpm = 60; }
  g->weight = weight;
  g->dot  = (int)lround((double)g->sr * 1.2 / (double)wpm);   /* 1200/wpm ms */
  if (g->dot < 1) { g->dot = 1; }
  g->dash = (int)lround((double)g->dot * 3.0 * weight / 50.0); /* 3:1 at weight 50 */
  if (g->dash < 1) { g->dash = 1; }
  /* Keep the ramp inside a dot so dots still reach full amplitude at high WPM. */
  int rn = (int)lround((double)g->sr * g->ramp_ms / 1000.0);
  int cap = (int)(0.9 * (double)g->dot);
  g->ramp_n = rn < 0 ? 0 : (rn > cap ? cap : rn);
}

cw_gen *cw_gen_new(int sample_rate, int wpm, double weight, double ramp_ms) {
  if (sample_rate < 1000) { return NULL; }
  cw_gen *g = calloc(1, sizeof *g);
  if (!g) { return NULL; }
  g->sr = sample_rate;
  g->ramp_ms = ramp_ms < 0.0 ? 0.0 : ramp_ms;
  g->cap = 256;
  g->q = malloc((size_t)g->cap * sizeof(seg));
  if (!g->q) { free(g); return NULL; }
  g->ccap = 256;
  g->cq = malloc((size_t)g->ccap * sizeof(cwch));
  if (!g->cq) { free(g->q); free(g); return NULL; }
  set_timing(g, wpm, weight);
  return g;
}

void cw_gen_set_speed(cw_gen *g, int wpm, double weight) { if (g) { set_timing(g, wpm, weight); } }
void cw_gen_set_ramp(cw_gen *g, double ramp_ms) {
  if (!g) { return; }
  g->ramp_ms = ramp_ms < 0.0 ? 0.0 : ramp_ms;
  int rn = (int)lround((double)g->sr * g->ramp_ms / 1000.0);
  int cap = (int)(0.9 * (double)g->dot);
  g->ramp_n = rn < 0 ? 0 : (rn > cap ? cap : rn);
}
int cw_gen_dot_samples(const cw_gen *g) { return g ? g->dot : 0; }

void cw_gen_free(cw_gen *g) { if (g) { free(g->q); free(g->cq); free(g); } }

static void push_seg(cw_gen *g, int down, int len) {
  if (len <= 0) { return; }
  int next = (g->tail + 1) % g->cap;
  if (next == g->head) {                     /* ring full → grow */
    int ncap = g->cap * 2;
    seg *nq = malloc((size_t)ncap * sizeof(seg));
    if (!nq) { return; }                     /* drop on OOM (never keys extra) */
    int n = 0;
    for (int i = g->head; i != g->tail; i = (i + 1) % g->cap) { nq[n++] = g->q[i]; }
    free(g->q);
    g->q = nq; g->cap = ncap; g->head = 0; g->tail = n;
    next = (g->tail + 1) % g->cap;
  }
  g->q[g->tail].down = down;
  g->q[g->tail].len  = len;
  g->tail = next;
  g->sched_end += len;
}

/* Record a queued char at the current schedule end. Ring full → drop the oldest
 * entry (it is almost certainly sent history; the schedule is NOT affected). */
static void push_char(cw_gen *g, char ch) {
  int next = (g->ctail + 1) % g->ccap;
  if (next == g->chead) { g->chead = (g->chead + 1) % g->ccap; }
  g->cq[g->ctail].ch  = ch;
  g->cq[g->ctail].end = g->sched_end;
  g->ctail = next;
}

void cw_gen_send_text(cw_gen *g, const char *text) {
  if (!g || !text) { return; }
  int pending = 0;   /* gap units to insert before the next mark */
  /* A send starting from IDLE skips leading whitespace: the word gap it would
   * key has already elapsed as real silence. Without this, SDC macros (which
   * arrive with a leading space) keyed 7 dots of dead air — 280 ms @ 30 WPM —
   * at the START of every over (measured, contest note #3). When the queue is
   * still busy the leading space is a genuine word gap and is kept. */
  int at_start = cw_gen_idle(g);
  /* Progress clock: a schedule queued onto an emptied generator starts sounding
   * at the CURRENT sample position, not where the last schedule ended. A send
   * from idle is a NEW over — drop the previous over's char record, so the HUD
   * shows only the sequence being transmitted now (Richard, live 2026-07-11).
   * When the queue is still busy the incoming text APPENDS to the same over. */
  if (at_start) { g->chead = g->ctail = 0; }
  if (g->sched_end < g->clock) { g->sched_end = g->clock; }
  for (const char *p = text; *p; p++) {
    if (*p == ' ' || *p == '\t' || *p == '\n') {
      if (!at_start && pending < 7) { pending = 7; push_char(g, ' '); }
      continue;
    }
    const char *m = morse_of(*p);
    if (!m) { continue; }
    for (int i = 0; m[i]; i++) {
      int gap = (i == 0) ? pending : 1;                 /* char/word gap, else intra-char */
      if (gap > 0) { push_seg(g, 0, gap * g->dot); }
      push_seg(g, 1, m[i] == '-' ? g->dash : g->dot);   /* the mark */
      pending = 0;
    }
    push_char(g, (char)toupper((unsigned char)*p));     /* ends at its last mark */
    pending = 3;   /* default inter-character gap after a completed char */
    at_start = 0;
  }
}

void cw_gen_flush(cw_gen *g) {
  if (!g) { return; }
  g->head = g->tail = 0; g->have_cur = 0; g->cur_rem = 0;
  g->chead = g->ctail = 0; g->sched_end = g->clock;
}

int cw_gen_pull(cw_gen *g, float *out, int n) {
  if (!g || !out || n <= 0) { return 0; }
  int keydown = 0;
  for (int i = 0; i < n; i++) {
    if (!g->have_cur) {
      if (g->head != g->tail) {                 /* pop next segment */
        g->cur_down = g->q[g->head].down;
        g->cur_rem  = g->q[g->head].len;
        g->head = (g->head + 1) % g->cap;
        g->have_cur = 1;
      } else {
        g->cur_down = 0;                         /* nothing queued → rest (ramp down) */
      }
    }
    int target = g->have_cur ? g->cur_down : 0;
    if (g->ramp_n <= 0) {
      g->ramp_pos = target ? 1.0 : 0.0;
    } else if (target && g->ramp_pos < g->ramp_n) {
      g->ramp_pos += 1.0;
    } else if (!target && g->ramp_pos > 0.0) {
      g->ramp_pos -= 1.0;
    }
    double frac = g->ramp_n > 0 ? g->ramp_pos / (double)g->ramp_n : g->ramp_pos;
    double env  = 0.5 - 0.5 * cos(M_PI * frac);   /* raised cosine, 0..1 */
    out[i] = (float)env;
    if (target) { keydown++; }
    if (g->have_cur && --g->cur_rem <= 0) { g->have_cur = 0; }
  }
  g->clock += n;
  return keydown;
}

int cw_gen_progress(cw_gen *g, char *buf, int buflen, int *cur) {
  if (cur) { *cur = 0; }
  if (buf && buflen > 0) { buf[0] = '\0'; }
  if (!g || !buf || buflen < 2) { return 0; }
  int sent = 0;                          /* chars fully sounded (end <= clock) */
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

int cw_gen_idle(const cw_gen *g) {
  if (!g) { return 1; }
  return (g->head == g->tail) && !g->have_cur && g->ramp_pos <= 0.0;
}
