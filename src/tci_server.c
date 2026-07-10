/*
 * TCI server — see tci_server.h. LWS plumbing follows the proven piHPSDR
 * pattern (src/tci.c @974acba): "chat"/"superchat"/"tci" subprotocols, a
 * 1 ms service loop with a pending-writable flag, per-client send queues,
 * commands executed on the GTK main loop. Command semantics re-checked
 * against the official spec (docs/TCI-SCOPE.md digest).
 */
#include <glib.h>
#include <libwebsockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <stdatomic.h>

#include "tci_server.h"
#include "wdsp.h"                     /* create_resample: IQ decimation (2d) */

#define TCI_MAX_CLIENTS 8
#define TCI_PROTO_LINE  "protocol:ExpertSDR3,1.9;"   /* clients key on this name */
#define TCI_DEVICE_LINE "device:SDR-for-Linux;"

/* One queued outgoing WebSocket message (text or binary). */
typedef struct {
  int    binary;
  size_t len;
  uint8_t data[];
} Msg;

/* Official Stream block header (spec 3.4): 16 × u32 then payload. */
#define TCI_HDR_U32S     16
#define TCI_IQ_STREAM    0            /* StreamType                          */
#define TCI_RX_AUDIO     1
#define TCI_TX_AUDIO     2
#define TCI_TX_CHRONO    3
#define TCI_FMT_INT16    0            /* SampleType                          */
#define TCI_FMT_FLOAT32  3

typedef struct {
  struct lws *wsi;
  int         inuse;
  int         init_sent;     /* handshake block queued                       */
  GQueue     *txq;           /* Msg* waiting for SERVER_WRITEABLE            */
  char        peer[48];      /* client ip:port (identification for the GUI)  */
  char        agent[96];     /* HTTP User-Agent from the WS handshake ("" =?)*/
  /* RX audio stream subscription (F6d-2b); state under s_lock. */
  int         au_sub;        /* audio_start'ed                               */
  int         au_rate;       /* 8000/12000/24000/48000                       */
  int         au_fmt;        /* TCI_FMT_INT16 / TCI_FMT_FLOAT32              */
  int         au_ch;         /* 1 or 2 (mono duplicated)                     */
  int         au_block;      /* Stream.length: scalar samples per packet     */
  int         au_block_set;  /* client pinned au_block explicitly            */
  double      au_acc;        /* 48 k → au_rate boxcar decimator              */
  int         au_acccnt;
  float       au_buf[2048];  /* mono frames at au_rate awaiting a packet     */
  int         au_fill;
  /* Sensor notifications (RX_SENSORS_ENABLE / TX_SENSORS_ENABLE). */
  int         rxsens_on, rxsens_ms;
  int         txsens_on, txsens_ms;
  gint64      rxsens_next, txsens_next;   /* g_get_monotonic_time deadlines  */
  /* TX audio over TCI (F6d-2c): this client sources the exciter audio. */
  int         tx_owner;
  uint8_t     bin_buf[16384];             /* WebSocket fragment reassembly   */
  size_t      bin_fill;
  /* IQ stream subscription (F6d-2d); state under s_lock. The resampler is
   * created lazily by the pump (LWS thread) and torn down under s_lock. */
  int         iq_sub;        /* iq_start'ed                                  */
  int         iq_rate;       /* 48000/96000/192000/384000                    */
  RESAMPLE    iq_rs;         /* WDSP resampler (NULL = none/bypass)          */
  int         iq_rs_in;      /* rates iq_rs was planned for                  */
  int         iq_rs_out;
  double     *iq_out;        /* resampler output (heap while subscribed)     */
  float       iq_blk[2 * 2048];  /* IQ frames accumulated for one block      */
  int         iq_fill;       /* frames in iq_blk                             */
} Client;

static Client              s_cli[TCI_MAX_CLIENTS];
static GMutex              s_lock;        /* clients table + queues          */
static struct lws_context *s_ctx;
static GThread            *s_thread;
static volatile int        s_run;
static volatile int        s_writable;    /* wake flag → callback_on_writable */
static TciOps              s_ops;
static guint               s_reporter_id;
static int                 s_cw_delay_ms = 10;  /* cw_macros_delay (stored)  */
static int                 s_debug;       /* SDRFL_TCI_DEBUG=1: log commands  */

/* RX audio ring: producer = demod tap (feed thread), consumer = LWS thread. */
#define AU_RING 32768u
#define AU_MASK (AU_RING - 1u)
static float            au_ring[AU_RING];
static _Atomic unsigned au_head, au_tail;
static _Atomic int      s_au_active;      /* # of audio_start'ed clients      */

/* Raw DDC IQ ring (F6d-2d): producer = P2 feed (radio thread), consumer =
 * LWS thread. Counted in complex pairs; 2^17 pairs = 85 ms @ 1536 k. */
#define IQ_RING       131072u
#define IQ_MASK       (IQ_RING - 1u)
#define IQ_CHUNK      1024            /* xresample input block, pairs        */
#define IQ_BLK_FRAMES 2048            /* frames per outgoing Stream block    */
#define IQ_OUT_MAX    (IQ_CHUNK * 8 + 16)  /* worst upsample 48 k → 384 k    */
static double           iq_ring[2 * IQ_RING];
static _Atomic unsigned iq_head, iq_tail;
static _Atomic int      s_iq_active;      /* # of iq_start'ed clients         */
static _Atomic int      s_iq_in_rate;     /* engine rate seen by the pusher   */
static double           s_iq_chunk[2 * IQ_CHUNK]; /* shared resampler input   */
/* Live-debug knobs (deskHPSDR precedent: some TCI clients expect the
 * opposite complex spectral orientation): swap I/Q, negate Q. */
static int              s_iq_swap, s_iq_conj;

/* Last state broadcast by the reporter (main thread only). */
static struct {
  long long freq;
  char      mode[16];
  int       flo, fhi;
  int       trx, tune, mute, wpm, txen;
  double    drive, tdrive, vol;
  int       valid;
} s_last;

/* ---- send path (any thread; queue under s_lock, LWS thread writes) ------- */

static void cli_send_msg(Client *c, int binary, const void *data, size_t len) {
  if (!c->inuse || !c->txq) { return; }
  Msg *m = g_malloc(sizeof(Msg) + len);
  m->binary = binary;
  m->len = len;
  memcpy(m->data, data, len);
  g_queue_push_tail(c->txq, m);
  if (s_debug && !binary) {
    fprintf(stderr, "tci> %.*s\n", (int)len, (const char *)data);
  }
}

static void cli_send(Client *c, const char *msg) {
  cli_send_msg(c, 0, msg, strlen(msg));
}

static void tci_broadcast(const char *msg) {
  g_mutex_lock(&s_lock);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) { cli_send(&s_cli[i], msg); }
  g_mutex_unlock(&s_lock);
  s_writable = 1;
  if (s_ctx) { lws_cancel_service(s_ctx); }
}

static void tci_sendf(Client *c, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void tci_sendf(Client *c, const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_mutex_lock(&s_lock);
  cli_send(c, buf);
  g_mutex_unlock(&s_lock);
  s_writable = 1;
  if (s_ctx) { lws_cancel_service(s_ctx); }
}

static void tci_broadcastf(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void tci_broadcastf(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  tci_broadcast(buf);
}

/* ---- state snapshot + reporter (main thread) ------------------------------ */

/* Broadcast every state item that changed since the last look. This is how
 * GUI-initiated changes (tuning wheel, mode buttons) reach TCI clients
 * without instrumenting every GUI path — the server polls its own ops. */
/* Sensor notifications (fast timer, 100 ms): per-client cadence. Getters run
 * on the main thread like every ops call. */
static gboolean tci_sensors_tick(gpointer data) {
  (void)data;
  if (!s_run) { return G_SOURCE_REMOVE; }
  gint64 now = g_get_monotonic_time();
  double sm = 0.0, mic = 0.0, rms = 0.0, pep = 0.0, swr = 0.0;
  int have_sm = 0, have_tx = 0;
  g_mutex_lock(&s_lock);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) {
    Client *c = &s_cli[i];
    if (!c->inuse) { continue; }
    if (c->rxsens_on && now >= c->rxsens_next) {
      if (!have_sm) { g_mutex_unlock(&s_lock); sm = s_ops.get_smeter(); g_mutex_lock(&s_lock); have_sm = 1; }
      /* g_ascii_formatd: the app runs in the user's locale (cs_CZ prints a
       * DECIMAL COMMA) but ',' is a reserved TCI separator — always '.'. */
      char m[96], v[G_ASCII_DTOSTR_BUF_SIZE];
      g_ascii_formatd(v, sizeof(v), "%.1f", sm);
      snprintf(m, sizeof(m), "rx_channel_sensors:0,0,%s;", v);
      cli_send(c, m);
      c->rxsens_next = now + (gint64)c->rxsens_ms * 1000;
    }
    if (c->txsens_on && now >= c->txsens_next) {
      if (!have_tx) { g_mutex_unlock(&s_lock); s_ops.get_tx_meters(&mic, &rms, &pep, &swr); g_mutex_lock(&s_lock); have_tx = 1; }
      char m[192];
      char v1[G_ASCII_DTOSTR_BUF_SIZE], v2[G_ASCII_DTOSTR_BUF_SIZE];
      char v3[G_ASCII_DTOSTR_BUF_SIZE], v4[G_ASCII_DTOSTR_BUF_SIZE];
      g_ascii_formatd(v1, sizeof(v1), "%.1f", mic);
      g_ascii_formatd(v2, sizeof(v2), "%.1f", rms);
      g_ascii_formatd(v3, sizeof(v3), "%.1f", pep);
      g_ascii_formatd(v4, sizeof(v4), "%.2f", swr);
      snprintf(m, sizeof(m), "tx_sensors:0,%s,%s,%s,%s;", v1, v2, v3, v4);
      cli_send(c, m);
      c->txsens_next = now + (gint64)c->txsens_ms * 1000;
    }
  }
  g_mutex_unlock(&s_lock);
  if (have_sm || have_tx) {
    s_writable = 1;
    if (s_ctx) { lws_cancel_service(s_ctx); }
  }
  return G_SOURCE_CONTINUE;
}

static gboolean tci_reporter(gpointer data) {
  (void)data;
  if (!s_run) { s_reporter_id = 0; return G_SOURCE_REMOVE; }
  long long f = s_ops.get_freq();
  const char *m = s_ops.get_mode();
  int lo, hi;
  s_ops.get_filter(&lo, &hi);
  int trx = s_ops.get_trx(), tune = s_ops.get_tune();
  int mute = s_ops.get_mute(), wpm = s_ops.get_cw_speed();
  int txen = s_ops.get_tx_enable();
  double drv = s_ops.get_drive(), tdrv = s_ops.get_tune_drive();
  double vol = s_ops.get_volume();

  if (!s_last.valid || f != s_last.freq) {
    tci_broadcastf("dds:0,%lld;", f);
    tci_broadcastf("vfo:0,0,%lld;", f);
    tci_broadcastf("tx_frequency:%lld;", f);
  }
  if (!s_last.valid || strcmp(m, s_last.mode) != 0) { tci_broadcastf("modulation:0,%s;", m); }
  if (!s_last.valid || lo != s_last.flo || hi != s_last.fhi) {
    tci_broadcastf("rx_filter_band:0,%d,%d;", lo, hi);
  }
  if (!s_last.valid || trx != s_last.trx)   { tci_broadcastf("trx:0,%s;", trx ? "true" : "false"); }
  if (!s_last.valid || tune != s_last.tune) { tci_broadcastf("tune:0,%s;", tune ? "true" : "false"); }
  if (!s_last.valid || mute != s_last.mute) { tci_broadcastf("mute:%s;", mute ? "true" : "false"); }
  if (!s_last.valid || wpm != s_last.wpm)   { tci_broadcastf("cw_macros_speed:%d;", wpm); }
  if (!s_last.valid || txen != s_last.txen) { tci_broadcastf("tx_enable:0,%s;", txen ? "true" : "false"); }
  if (!s_last.valid || drv != s_last.drive)   { tci_broadcastf("drive:0,%d;", (int)(drv + 0.5)); }
  if (!s_last.valid || tdrv != s_last.tdrive) { tci_broadcastf("tune_drive:0,%d;", (int)(tdrv + 0.5)); }
  if (!s_last.valid || vol != s_last.vol)     { tci_broadcastf("volume:%d;", (int)(vol - 0.5)); }

  s_last.freq = f;
  g_strlcpy(s_last.mode, m, sizeof(s_last.mode));
  s_last.flo = lo; s_last.fhi = hi;
  s_last.trx = trx; s_last.tune = tune; s_last.mute = mute;
  s_last.wpm = wpm; s_last.txen = txen;
  s_last.drive = drv; s_last.tdrive = tdrv; s_last.vol = vol;
  s_last.valid = 1;
  return G_SOURCE_CONTINUE;
}

/* Bidirectional commands accepted + echoed (the TCI synchronizer contract:
 * every set MUST come back as a broadcast, else clients stall waiting) that
 * have no real backend yet. The stored args replay in the handshake. */
static struct {
  const char *name;
  const char *dflt;
  char        val[64];
} s_echo[] = {
  { "split_enable",  "0,false", "" }, { "lock",          "0,false", "" },
  { "rit_enable",    "0,false", "" }, { "rit_offset",    "0,0",     "" },
  { "xit_enable",    "0,false", "" }, { "xit_offset",    "0,0",     "" },
  { "sql_enable",    "0,false", "" }, { "sql_level",     "0,-80",   "" },
  { "rx_nb_enable",  "0,false", "" }, { "rx_nr_enable",  "0,false", "" },
  { "rx_anf_enable", "0,false", "" }, { "rx_apf_enable", "0,false", "" },
  { "rx_bin_enable", "0,false", "" }, { "rx_nf_enable",  "0,false", "" },
  { "rx_dse_enable", "0,false", "" }, { "rx_mute",       "0,false", "" },
  { "agc_mode",      "0,fast",  "" }, { "agc_gain",      "0,80",    "" },
};

static void echo_reset(void) {
  for (guint i = 0; i < G_N_ELEMENTS(s_echo); i++) {
    g_strlcpy(s_echo[i].val, s_echo[i].dflt, sizeof(s_echo[i].val));
  }
}

/* Try the echo table; returns 1 when the command was consumed. */
static int echo_exec(const char *name, char **av, int ac) {
  for (guint i = 0; i < G_N_ELEMENTS(s_echo); i++) {
    if (strcmp(name, s_echo[i].name) != 0) { continue; }
    if (ac > 0 && av[0][0]) {                /* set: join args + store */
      char joined[64] = "";
      for (int a = 0; a < ac; a++) {
        if (a) { g_strlcat(joined, ",", sizeof(joined)); }
        g_strlcat(joined, av[a], sizeof(joined));
      }
      g_strlcpy(s_echo[i].val, joined, sizeof(s_echo[i].val));
    }
    tci_broadcastf("%s:%s;", s_echo[i].name, s_echo[i].val);
    return 1;
  }
  return 0;
}

/* ---- initial handshake (main thread; ops needed for the state block) ------ */

static gboolean send_initial_idle(gpointer data) {
  Client *c = (Client *)data;
  int lo, hi;
  if (!s_run || !c->inuse) { return G_SOURCE_REMOVE; }
  s_ops.get_filter(&lo, &hi);
  long long f = s_ops.get_freq();
  const char *mode = s_ops.get_mode();
  g_mutex_lock(&s_lock);
  cli_send(c, TCI_PROTO_LINE);
  cli_send(c, TCI_DEVICE_LINE);
  cli_send(c, "receive_only:false;");
  cli_send(c, "trx_count:1;");
  /* piHPSDR/ExpertSDR spell it "channels_count" (not the spec's CHANNEL_COUNT)
   * and report 2 A/B channels per receiver — Decodium waits for that. */
  cli_send(c, "channels_count:2;");
  cli_send(c, "vfo_limits:0,61440000;");
  g_mutex_unlock(&s_lock);
  tci_sendf(c, "if_limits:-%d,%d;", s_ops.get_rate() / 2, s_ops.get_rate() / 2);
  tci_sendf(c, "modulations_list:am,lsb,usb,cw,cwl,cwu,digu,digl;");
  tci_sendf(c, "mute:%s;", s_ops.get_mute() ? "true" : "false");
  tci_sendf(c, "volume:%d;", (int)(s_ops.get_volume() - 0.5));
  tci_sendf(c, "dds:0,%lld;", f);
  /* Both channels (A/B) of receiver 0: IF offset 0, VFO on the same freq —
   * piHPSDR emits the full per-channel set before the client proceeds. */
  tci_sendf(c, "if:0,0,0;");
  tci_sendf(c, "if:0,1,0;");
  tci_sendf(c, "vfo:0,0,%lld;", f);
  tci_sendf(c, "vfo:0,1,%lld;", f);
  tci_sendf(c, "modulation:0,%s;", mode);
  tci_sendf(c, "rx_filter_band:0,%d,%d;", lo, hi);
  tci_sendf(c, "rx_enable:0,true;");         /* receiver 0 is running — Decodium gates audio on this */
  /* Full state block like piHPSDR: locks, squelch, noise, RIT/XIT, split,
   * volumes, AGC — clients wait for the lot before they start operating. */
  for (guint i = 0; i < G_N_ELEMENTS(s_echo); i++) {
    tci_sendf(c, "%s:%s;", s_echo[i].name, s_echo[i].val);
  }
  tci_sendf(c, "rx_volume:0,0,%d;", (int)(s_ops.get_volume() - 0.5));
  tci_sendf(c, "rx_volume:0,1,%d;", (int)(s_ops.get_volume() - 0.5));
  tci_sendf(c, "trx:0,%s;", s_ops.get_trx() ? "true" : "false");
  tci_sendf(c, "tune:0,%s;", s_ops.get_tune() ? "true" : "false");
  tci_sendf(c, "drive:0,%d;", (int)(s_ops.get_drive() + 0.5));
  tci_sendf(c, "tune_drive:0,%d;", (int)(s_ops.get_tune_drive() + 0.5));
  tci_sendf(c, "cw_macros_speed:%d;", s_ops.get_cw_speed());
  tci_sendf(c, "cw_macros_delay:%d;", s_cw_delay_ms);
  tci_sendf(c, "cw_keyer_speed:%d;", s_ops.get_cw_speed());
  tci_sendf(c, "tx_enable:0,%s;", s_ops.get_tx_enable() ? "true" : "false");
  tci_sendf(c, "tx_frequency:%lld;", f);
  tci_sendf(c, "ready;");
  tci_sendf(c, "start;");                    /* piHPSDR ends with start; — device is running */
  return G_SOURCE_REMOVE;
}

/* ---- command execution (main thread) -------------------------------------- */

/* Un-escape a CW text argument: '^' → ':', '~' → ',', '*' → ';' (spec 3.2.1). */
static void cw_unescape(char *s) {
  for (; *s; s++) {
    if (*s == '^') { *s = ':'; }
    else if (*s == '~') { *s = ','; }
    else if (*s == '*') { *s = ';'; }
  }
}

static int arg_bool(const char *a) { return g_ascii_strcasecmp(a, "true") == 0; }

/* Drop a client's IQ subscription + resampler (caller holds s_lock). Safe
 * against the pump: it only touches iq_rs/iq_out under the same lock. */
static void iq_unsub(Client *c) {
  if (c->iq_sub) { c->iq_sub = 0; atomic_fetch_sub(&s_iq_active, 1); }
  if (c->iq_rs)  { destroy_resample(c->iq_rs); c->iq_rs = NULL; }
  if (c->iq_out) { g_free(c->iq_out); c->iq_out = NULL; }
  c->iq_fill = 0;
}

/* One parsed command: lowercase name + up to 8 raw args. */
static void tci_exec(Client *c, char *name, char **av, int ac) {
  for (char *p = name; *p; p++) { *p = (char)g_ascii_tolower(*p); }
  if (s_debug) {
    fprintf(stderr, "tci< %s", name);
    for (int i = 0; i < ac; i++) { fprintf(stderr, "%c%s", i ? ',' : ':', av[i]); }
    fprintf(stderr, ";\n");
  }

  if (strcmp(name, "vfo") == 0 || strcmp(name, "dds") == 0) {
    int isvfo = (name[0] == 'v');
    int fidx = isvfo ? 2 : 1;               /* vfo:rx,ch,f / dds:rx,f        */
    if (ac > fidx && av[fidx][0]) {
      long long f = g_ascii_strtoll(av[fidx], NULL, 10);
      if (f > 0) { s_ops.set_freq(f); }
      tci_broadcastf("dds:0,%lld;", s_ops.get_freq());
      tci_broadcastf("vfo:0,0,%lld;", s_ops.get_freq());
    } else if (isvfo) {
      tci_sendf(c, "vfo:0,0,%lld;", s_ops.get_freq());
    } else {
      tci_sendf(c, "dds:0,%lld;", s_ops.get_freq());
    }
  } else if (strcmp(name, "if") == 0) {
    /* No CTUN yet: IF stays 0; an IF set retunes by the offset instead. */
    if (ac > 2 && av[2][0]) {
      long long off = g_ascii_strtoll(av[2], NULL, 10);
      if (off != 0) { s_ops.set_freq(s_ops.get_freq() + off); }
      tci_broadcastf("vfo:0,0,%lld;", s_ops.get_freq());
    }
    tci_sendf(c, "if:0,0,0;");
  } else if (strcmp(name, "modulation") == 0) {
    if (ac > 1 && av[1][0]) {
      char m[16];
      g_strlcpy(m, av[1], sizeof(m));
      for (char *p = m; *p; p++) { *p = (char)g_ascii_tolower(*p); }
      s_ops.set_mode(m);                     /* unsupported → state unchanged */
      tci_broadcastf("modulation:0,%s;", s_ops.get_mode());
    } else {
      tci_sendf(c, "modulation:0,%s;", s_ops.get_mode());
    }
  } else if (strcmp(name, "rx_filter_band") == 0) {
    if (ac > 2 && av[1][0] && av[2][0]) {
      s_ops.set_filter(atoi(av[1]), atoi(av[2]));
    }
    int lo, hi;
    s_ops.get_filter(&lo, &hi);
    tci_broadcastf("rx_filter_band:0,%d,%d;", lo, hi);
  } else if (strcmp(name, "trx") == 0) {
    if (ac > 1 && av[1][0]) {
      int want = arg_bool(av[1]);
      int src_tci = (want && ac > 2 && g_ascii_strcasecmp(av[2], "tci") == 0);
      if (want) {
        if (src_tci) {
          /* TX audio over TCI: single owner; source switches BEFORE the key
           * so no mic audio can leak, and everything still goes via tx_gate. */
          int busy = 0;
          g_mutex_lock(&s_lock);
          for (int i = 0; i < TCI_MAX_CLIENTS; i++) {
            if (s_cli[i].inuse && s_cli[i].tx_owner && &s_cli[i] != c) { busy = 1; }
          }
          if (!busy) { c->tx_owner = 1; }
          g_mutex_unlock(&s_lock);
          if (busy) {
            fprintf(stderr, "tci: trx tci refused — another client owns TX audio\n");
          } else if (s_ops.set_tx_source_tci(1) != 0 || s_ops.set_trx(1) != 0) {
            g_mutex_lock(&s_lock);
            c->tx_owner = 0;
            g_mutex_unlock(&s_lock);
            s_ops.set_tx_source_tci(0);
            fprintf(stderr, "tci: trx tci key refused by the gate\n");
          }
        } else {
          s_ops.set_trx(1);              /* mic-sourced MOX, like the GUI key */
        }
      } else {
        s_ops.set_trx(0);
        int was;
        g_mutex_lock(&s_lock);
        was = c->tx_owner;
        c->tx_owner = 0;
        g_mutex_unlock(&s_lock);
        if (was) { s_ops.set_tx_source_tci(0); }
      }
      tci_broadcastf("trx:0,%s;", s_ops.get_trx() ? "true" : "false");
    } else {
      tci_sendf(c, "trx:0,%s;", s_ops.get_trx() ? "true" : "false");
    }
  } else if (strcmp(name, "tune") == 0) {
    if (ac > 1 && av[1][0]) {
      s_ops.set_tune(arg_bool(av[1]));
      tci_broadcastf("tune:0,%s;", s_ops.get_tune() ? "true" : "false");
    } else {
      tci_sendf(c, "tune:0,%s;", s_ops.get_tune() ? "true" : "false");
    }
  } else if (strcmp(name, "drive") == 0 || strcmp(name, "tune_drive") == 0) {
    int istune = (name[0] == 't');
    if (ac > 1 && av[1][0]) {
      double v = g_ascii_strtod(av[1], NULL);
      if (v < 0) { v = 0; } if (v > 100) { v = 100; }
      if (istune) { s_ops.set_tune_drive(v); } else { s_ops.set_drive(v); }
    }
    double v = istune ? s_ops.get_tune_drive() : s_ops.get_drive();
    tci_broadcastf("%s:0,%d;", istune ? "tune_drive" : "drive", (int)(v + 0.5));
  } else if (strcmp(name, "volume") == 0) {
    if (ac > 0 && av[0][0]) { s_ops.set_volume(g_ascii_strtod(av[0], NULL)); }
    tci_broadcastf("volume:%d;", (int)(s_ops.get_volume() - 0.5));
  } else if (strcmp(name, "mute") == 0) {
    if (ac > 0 && av[0][0]) { s_ops.set_mute(arg_bool(av[0])); }
    tci_broadcastf("mute:%s;", s_ops.get_mute() ? "true" : "false");
  } else if (strcmp(name, "cw_macros_speed") == 0 || strcmp(name, "cw_keyer_speed") == 0) {
    if (ac > 0 && av[0][0]) { s_ops.set_cw_speed(atoi(av[0])); }
    tci_broadcastf("cw_macros_speed:%d;", s_ops.get_cw_speed());
  } else if (strcmp(name, "cw_macros_speed_up") == 0) {
    if (ac > 0 && av[0][0]) { s_ops.set_cw_speed(s_ops.get_cw_speed() + atoi(av[0])); }
    tci_broadcastf("cw_macros_speed:%d;", s_ops.get_cw_speed());
  } else if (strcmp(name, "cw_macros_speed_down") == 0) {
    if (ac > 0 && av[0][0]) { s_ops.set_cw_speed(s_ops.get_cw_speed() - atoi(av[0])); }
    tci_broadcastf("cw_macros_speed:%d;", s_ops.get_cw_speed());
  } else if (strcmp(name, "cw_macros_delay") == 0) {
    if (ac > 0 && av[0][0]) { s_cw_delay_ms = atoi(av[0]); }
    tci_broadcastf("cw_macros_delay:%d;", s_cw_delay_ms);
  } else if (strcmp(name, "cw_macros") == 0) {
    if (ac > 1 && av[1][0]) {
      char *txt = g_strdup(av[1]);
      cw_unescape(txt);
      s_ops.cw_send(txt);
      g_free(txt);
    }
  } else if (strcmp(name, "cw_msg") == 0) {
    /* Basic cw_msg: prefix + callsign (with $N repeats) + suffix as one
     * queued text. Live callsign correction = later (needs queue editing). */
    if (ac > 3) {
      char call[128] = "";
      const char *dollar = strchr(av[2], '$');
      int rep = dollar ? atoi(dollar + 1) : 1;
      if (rep < 1) { rep = 1; }
      if (rep > 5) { rep = 5; }
      char base[64];
      g_strlcpy(base, av[2], sizeof(base));
      char *d = strchr(base, '$');
      if (d) { *d = 0; }
      for (int i = 0; i < rep; i++) {
        g_strlcat(call, base, sizeof(call));
        g_strlcat(call, " ", sizeof(call));
      }
      char *txt = g_strdup_printf("%s %s %s", av[1], call, av[3]);
      cw_unescape(txt);
      s_ops.cw_send(txt);
      g_free(txt);
      tci_broadcastf("callsign_send:%s;", base);
    }
  } else if (strcmp(name, "cw_macros_stop") == 0) {
    s_ops.cw_stop();
  } else if (strcmp(name, "digu_offset") == 0 || strcmp(name, "digl_offset") == 0) {
    /* Digimode display offsets: stored + echoed so clients see a consistent
     * value; we do not shift the VFO (no CTUN yet — audio carries the full
     * passband either way). */
    static int s_dig_off[2];
    int isl = (name[3] == 'l');
    if (ac > 0 && av[0][0]) { s_dig_off[isl] = atoi(av[0]); }
    tci_broadcastf("%s:%d;", isl ? "digl_offset" : "digu_offset", s_dig_off[isl]);
  } else if (strcmp(name, "rx_sensors_enable") == 0 || strcmp(name, "tx_sensors_enable") == 0) {
    int istx = (name[0] == 't');
    if (ac > 0 && av[0][0]) {
      int on = arg_bool(av[0]);
      int ms = (ac > 1 && av[1][0]) ? atoi(av[1]) : 500;
      if (ms < 100)  { ms = 100; }
      if (ms > 1000) { ms = 1000; }
      g_mutex_lock(&s_lock);
      if (istx) { c->txsens_on = on; c->txsens_ms = ms; c->txsens_next = 0; }
      else      { c->rxsens_on = on; c->rxsens_ms = ms; c->rxsens_next = 0; }
      g_mutex_unlock(&s_lock);
    }
  } else if (strcmp(name, "audio_samplerate") == 0) {
    if (ac > 0 && av[0][0]) {
      int r = atoi(av[0]);
      if (r == 8000 || r == 12000 || r == 24000 || r == 48000) {
        g_mutex_lock(&s_lock);
        c->au_rate = r;
        if (!c->au_block_set) {              /* spec default block per rate  */
          c->au_block = r == 48000 ? 2048 : r == 24000 ? 1024 : r == 12000 ? 512 : 256;
        }
        c->au_acc = 0; c->au_acccnt = 0; c->au_fill = 0;
        g_mutex_unlock(&s_lock);
      }
    }
    tci_sendf(c, "audio_samplerate:%d;", c->au_rate);
  } else if (strcmp(name, "audio_start") == 0) {
    g_mutex_lock(&s_lock);
    if (!c->au_sub) { c->au_sub = 1; atomic_fetch_add(&s_au_active, 1); }
    c->au_acc = 0; c->au_acccnt = 0; c->au_fill = 0;
    g_mutex_unlock(&s_lock);
    fprintf(stderr, "tci: audio_start — %d Hz, fmt %d, %d ch, block %d\n",
            c->au_rate, c->au_fmt, c->au_ch, c->au_block);
  } else if (strcmp(name, "audio_stop") == 0) {
    g_mutex_lock(&s_lock);
    if (c->au_sub) { c->au_sub = 0; atomic_fetch_sub(&s_au_active, 1); }
    g_mutex_unlock(&s_lock);
    fprintf(stderr, "tci: audio_stop\n");
  } else if (strcmp(name, "audio_stream_sample_type") == 0) {
    if (ac > 0 && av[0][0]) {
      g_mutex_lock(&s_lock);
      /* int24/int32 fall back to float32 — nothing common asks for them */
      c->au_fmt = g_ascii_strcasecmp(av[0], "int16") == 0 ? TCI_FMT_INT16
                                                          : TCI_FMT_FLOAT32;
      g_mutex_unlock(&s_lock);
    }
  } else if (strcmp(name, "audio_stream_channels") == 0) {
    if (ac > 0 && av[0][0]) {
      g_mutex_lock(&s_lock);
      c->au_ch = atoi(av[0]) == 1 ? 1 : 2;
      g_mutex_unlock(&s_lock);
    }
  } else if (strcmp(name, "audio_stream_samples") == 0) {
    if (ac > 0 && av[0][0]) {
      int n = atoi(av[0]);
      if (n < 100)  { n = 100; }
      if (n > 2048) { n = 2048; }
      g_mutex_lock(&s_lock);
      c->au_block = n;
      c->au_block_set = 1;
      g_mutex_unlock(&s_lock);
    }
  } else if (strcmp(name, "iq_samplerate") == 0) {
    if (ac > 0 && av[0][0]) {
      int r = atoi(av[0]);
      if (r == 48000 || r == 96000 || r == 192000 || r == 384000) {
        g_mutex_lock(&s_lock);
        c->iq_rate = r;
        /* rate change → drop the plan; the pump re-plans on the next chunk */
        if (c->iq_rs) { destroy_resample(c->iq_rs); c->iq_rs = NULL; }
        c->iq_fill = 0;
        g_mutex_unlock(&s_lock);
      }
    }
    tci_sendf(c, "iq_samplerate:%d;", c->iq_rate);
  } else if (strcmp(name, "iq_start") == 0) {
    /* single receiver: anything but id 0 is ignored (deskHPSDR does the same) */
    if (ac > 0 && av[0][0] && atoi(av[0]) != 0) { return; }
    g_mutex_lock(&s_lock);
    if (!c->iq_sub) { c->iq_sub = 1; atomic_fetch_add(&s_iq_active, 1); }
    c->iq_fill = 0;
    g_mutex_unlock(&s_lock);
    fprintf(stderr, "tci: iq_start — %d Hz float32\n", c->iq_rate);
    tci_sendf(c, "iq_start:0;");
  } else if (strcmp(name, "iq_stop") == 0) {
    if (ac > 0 && av[0][0] && atoi(av[0]) != 0) { return; }
    g_mutex_lock(&s_lock);
    iq_unsub(c);
    g_mutex_unlock(&s_lock);
    fprintf(stderr, "tci: iq_stop\n");
    tci_sendf(c, "iq_stop:0;");
  } else if (strcmp(name, "start") == 0 || strcmp(name, "stop") == 0) {
    /* device start/stop — we are always running; acknowledge by echo */
    tci_sendf(c, "%s;", name);
  } else if (echo_exec(name, av, ac)) {
    /* bidirectional command without a backend yet: stored + broadcast back */
  }
  /* anything else: ignored (per spec: invalid commands are ignored) */
}

/* TX-owner client vanished (disconnect) — unkey + revert to mic, on the main
 * thread. A dangling key with no audio source must never stay up. */
static gboolean tx_owner_lost_idle(gpointer data) {
  (void)data;
  if (s_run) {
    s_ops.set_trx(0);
    s_ops.set_tx_source_tci(0);
    tci_broadcastf("trx:0,%s;", s_ops.get_trx() ? "true" : "false");
    fprintf(stderr, "tci: TX-owner client disconnected — unkeyed\n");
  }
  return G_SOURCE_REMOVE;
}

/* TX_AUDIO_STREAM block from the TX-owner client (TCI service thread):
 * convert to mono 48 k float and push into the exciter ring. */
static void tci_handle_binary(Client *c, const uint8_t *d, size_t len) {
  if (!c->tx_owner || len < TCI_HDR_U32S * 4) { return; }
  const uint32_t *h = (const uint32_t *)d;
  if (h[6] != TCI_TX_AUDIO) { return; }
  int rate = (int)h[1];
  int fmt  = (int)h[2];
  int ch   = h[7] ? (int)h[7] : 1;
  uint32_t scalars = h[5];
  const uint8_t *pl = d + TCI_HDR_U32S * 4;
  size_t ssz = (fmt == TCI_FMT_INT16) ? 2 : 4;
  if (fmt != TCI_FMT_INT16 && fmt != TCI_FMT_FLOAT32) { return; }
  size_t avail = (len - TCI_HDR_U32S * 4) / ssz;
  if (scalars > avail) { scalars = (uint32_t)avail; }
  int frames = (int)(scalars / (uint32_t)ch);
  int up = (rate == 48000) ? 1 : (rate == 24000) ? 2
         : (rate == 12000) ? 4 : (rate == 8000)  ? 6 : 0;
  if (!up || frames <= 0) { return; }
  float buf[2048];
  int n = 0;
  for (int i = 0; i < frames; i++) {
    float v = (fmt == TCI_FMT_INT16)
                ? (float)((const int16_t *)pl)[i * ch] / 32768.0f
                : ((const float *)pl)[i * ch];        /* left channel */
    for (int u = 0; u < up; u++) {                    /* naive upsample */
      buf[n++] = v;
      if (n == (int)G_N_ELEMENTS(buf)) { s_ops.tx_audio_push(buf, n); n = 0; }
    }
  }
  if (n) { s_ops.tx_audio_push(buf, n); }
}

typedef struct {
  Client *client;
  char    msg[1024];
} Payload;

/* Split a text frame into `name:args;` commands and execute each. */
static gboolean process_payload_idle(gpointer data) {
  Payload *p = (Payload *)data;
  if (s_run && p->client->inuse) {
    char *s = p->msg;
    char *semi;
    while ((semi = strchr(s, ';')) != NULL) {
      *semi = 0;
      char *args = strchr(s, ':');
      char *av[8];
      int ac = 0;
      if (args) {
        *args++ = 0;
        while (ac < 8 && args) {
          av[ac] = args;
          args = strchr(args, ',');
          if (args) { *args++ = 0; }
          ac++;
        }
      }
      g_strstrip(s);
      if (*s) { tci_exec(p->client, s, av, ac); }
      s = semi + 1;
    }
  }
  g_free(p);
  return G_SOURCE_REMOVE;
}

/* ---- LWS glue (LWS service thread) ---------------------------------------- */

static int lws_cb(struct lws *wsi, enum lws_callback_reasons reason,
                  void *user, void *in, size_t len) {
  int *slot = (int *)user;

  switch (reason) {
  case LWS_CALLBACK_ESTABLISHED: {
    int idx = -1;
    g_mutex_lock(&s_lock);
    for (int i = 0; i < TCI_MAX_CLIENTS; i++) {
      if (!s_cli[i].inuse) { idx = i; break; }
    }
    if (idx >= 0) {
      Client *nc = &s_cli[idx];
      memset(nc, 0, sizeof(*nc));
      nc->wsi = wsi;
      nc->inuse = 1;
      nc->txq = g_queue_new();
      nc->au_rate = 48000;             /* spec defaults: float32, 2 ch, 2048 */
      nc->au_fmt = TCI_FMT_FLOAT32;
      nc->au_ch = 2;
      nc->au_block = 2048;
      nc->iq_rate = 48000;
      lws_get_peer_simple(wsi, nc->peer, sizeof(nc->peer));
      lws_hdr_copy(wsi, nc->agent, sizeof(nc->agent), WSI_TOKEN_HTTP_USER_AGENT);
    }
    g_mutex_unlock(&s_lock);
    *slot = idx;
    if (idx < 0) { return -1; }             /* table full — refuse */
    fprintf(stderr, "tci: client %d connected (%s%s%s)\n", idx, s_cli[idx].peer,
            s_cli[idx].agent[0] ? " · " : "", s_cli[idx].agent);
    g_idle_add(send_initial_idle, &s_cli[idx]);
    return 0;
  }

  case LWS_CALLBACK_RECEIVE:
  case LWS_CALLBACK_SERVER_WRITEABLE:
  case LWS_CALLBACK_CLOSED:
    if (!slot || *slot < 0 || *slot >= TCI_MAX_CLIENTS || !s_cli[*slot].inuse) { return 0; }
    break;

  default:
    return 0;
  }

  Client *c = &s_cli[*slot];

  switch (reason) {
  case LWS_CALLBACK_RECEIVE:
    if (lws_frame_is_binary(wsi)) {
      /* Reassemble WebSocket fragments up to one Stream block, then parse. */
      if (c->bin_fill + len <= sizeof(c->bin_buf)) {
        memcpy(c->bin_buf + c->bin_fill, in, len);
        c->bin_fill += len;
      } else {
        c->bin_fill = 0;                           /* oversize → drop        */
        return 0;
      }
      if (!lws_is_final_fragment(wsi)) { return 0; }
      tci_handle_binary(c, c->bin_buf, c->bin_fill);
      c->bin_fill = 0;
      return 0;
    }
    {
      Payload *p = g_new0(Payload, 1);
      size_t n = len < sizeof(p->msg) - 1 ? len : sizeof(p->msg) - 1;
      memcpy(p->msg, in, n);
      p->msg[n] = 0;
      p->client = c;
      g_idle_add(process_payload_idle, p);
    }
    return 0;

  case LWS_CALLBACK_SERVER_WRITEABLE: {
    Msg *msg = NULL;
    int more = 0;
    g_mutex_lock(&s_lock);
    if (c->txq) {
      msg = g_queue_pop_head(c->txq);
      more = !g_queue_is_empty(c->txq);
    }
    g_mutex_unlock(&s_lock);
    if (msg) {
      unsigned char *buf = g_malloc(LWS_PRE + msg->len);
      memcpy(buf + LWS_PRE, msg->data, msg->len);
      int rc = lws_write(wsi, buf + LWS_PRE, msg->len,
                         msg->binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT);
      g_free(buf);
      g_free(msg);
      if (rc < 0) { return -1; }
    }
    if (more) { lws_callback_on_writable(wsi); }
    return 0;
  }

  case LWS_CALLBACK_CLOSED:
    fprintf(stderr, "tci: client %d disconnected\n", *slot);
    g_mutex_lock(&s_lock);
    if (c->tx_owner) {                   /* ⛔ never leave a key without owner */
      c->tx_owner = 0;
      g_idle_add(tx_owner_lost_idle, NULL);
    }
    if (c->au_sub) { atomic_fetch_sub(&s_au_active, 1); }
    iq_unsub(c);
    c->inuse = 0;
    c->wsi = NULL;
    if (c->txq) {
      Msg *m;
      while ((m = g_queue_pop_head(c->txq))) { g_free(m); }
      g_queue_free(c->txq);
      c->txq = NULL;
    }
    g_mutex_unlock(&s_lock);
    return 0;

  default:
    return 0;
  }
}

static const struct lws_protocols s_protocols[] = {
  { "chat",      lws_cb, sizeof(int), 8192, 0, NULL, 0 },
  { "superchat", lws_cb, sizeof(int), 8192, 0, NULL, 0 },
  { "tci",       lws_cb, sizeof(int), 8192, 0, NULL, 0 },
  { NULL, NULL, 0, 0, 0, NULL, 0 }
};

/* Emit one RX-audio Stream block for a client (caller holds s_lock). The
 * mono frames in au_buf are duplicated onto 2 channels when asked for. */
static void au_emit(Client *c) {
  int frames = c->au_fill;
  int scalars = frames * c->au_ch;
  size_t ssize = (c->au_fmt == TCI_FMT_INT16) ? 2 : 4;
  size_t len = TCI_HDR_U32S * 4 + (size_t)scalars * ssize;
  uint8_t *blk = g_malloc0(len);
  uint32_t *h = (uint32_t *)blk;
  h[0] = 0;                       /* receiver                                */
  h[1] = (uint32_t)c->au_rate;
  h[2] = (uint32_t)c->au_fmt;
  h[3] = 0;                       /* codec                                   */
  h[4] = 0;                       /* crc                                     */
  h[5] = (uint32_t)scalars;       /* length: scalar sample count             */
  h[6] = TCI_RX_AUDIO;
  h[7] = (uint32_t)c->au_ch;
  if (c->au_fmt == TCI_FMT_INT16) {
    int16_t *p = (int16_t *)(blk + TCI_HDR_U32S * 4);
    for (int i = 0; i < frames; i++) {
      int16_t v = (int16_t)(c->au_buf[i] * 32767.0f);
      for (int ch = 0; ch < c->au_ch; ch++) { *p++ = v; }
    }
  } else {
    float *p = (float *)(blk + TCI_HDR_U32S * 4);
    for (int i = 0; i < frames; i++) {
      for (int ch = 0; ch < c->au_ch; ch++) { *p++ = c->au_buf[i]; }
    }
  }
  cli_send_msg(c, 1, blk, len);
  g_free(blk);
  c->au_fill = 0;
}

/* Drain the 48 k tap ring and fan out to subscribed clients (LWS thread). */
static void au_pump(void) {
  float chunk[2048];
  for (;;) {
    unsigned t = atomic_load_explicit(&au_tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&au_head, memory_order_acquire);
    unsigned avail = h - t;
    if (avail == 0) { return; }
    int n = avail < 2048 ? (int)avail : 2048;
    for (int i = 0; i < n; i++) { chunk[i] = au_ring[(t + (unsigned)i) & AU_MASK]; }
    atomic_store_explicit(&au_tail, t + (unsigned)n, memory_order_release);

    g_mutex_lock(&s_lock);
    for (int ci = 0; ci < TCI_MAX_CLIENTS; ci++) {
      Client *c = &s_cli[ci];
      if (!c->inuse || !c->au_sub) { continue; }
      int dec = 48000 / (c->au_rate > 0 ? c->au_rate : 48000);
      if (dec < 1) { dec = 1; }
      int fpp = c->au_block / c->au_ch;      /* mono frames per packet       */
      if (fpp < 1) { fpp = 1; }
      if (fpp > (int)G_N_ELEMENTS(c->au_buf)) { fpp = (int)G_N_ELEMENTS(c->au_buf); }
      for (int i = 0; i < n; i++) {
        c->au_acc += chunk[i];
        if (++c->au_acccnt >= dec) {
          c->au_buf[c->au_fill++] = (float)(c->au_acc / dec);
          c->au_acc = 0.0;
          c->au_acccnt = 0;
          if (c->au_fill >= fpp) { au_emit(c); }
        }
      }
      if (c->wsi && c->txq && !g_queue_is_empty(c->txq)) {
        lws_callback_on_writable(c->wsi);
      }
    }
    g_mutex_unlock(&s_lock);
  }
}

/* Emit one IQ Stream block for a client (caller holds s_lock). */
static void iq_emit(Client *c) {
  int frames = c->iq_fill;
  size_t len = TCI_HDR_U32S * 4 + (size_t)frames * 2 * sizeof(float);
  uint8_t *blk = g_malloc0(len);
  uint32_t *h = (uint32_t *)blk;
  h[0] = 0;                       /* receiver                                */
  h[1] = (uint32_t)c->iq_rate;
  h[2] = TCI_FMT_FLOAT32;
  h[5] = (uint32_t)(frames * 2);  /* length: scalar sample count             */
  h[6] = TCI_IQ_STREAM;
  h[7] = 2;                       /* I + Q                                   */
  memcpy(blk + TCI_HDR_U32S * 4, c->iq_blk, (size_t)frames * 2 * sizeof(float));
  cli_send_msg(c, 1, blk, len);
  g_free(blk);
  c->iq_fill = 0;
}

/* Drain the DDC-rate IQ ring in fixed chunks and fan out to subscribed
 * clients (LWS thread): per-client WDSP decimation to iq_rate, float32
 * conversion, block accumulation. Bypass when the rates already match. */
static void iq_pump(void) {
  int in_rate = atomic_load_explicit(&s_iq_in_rate, memory_order_relaxed);
  if (in_rate <= 0) { return; }
  for (;;) {
    unsigned t = atomic_load_explicit(&iq_tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&iq_head, memory_order_acquire);
    if (h - t < IQ_CHUNK) { return; }
    for (int i = 0; i < IQ_CHUNK; i++) {
      unsigned idx = (t + (unsigned)i) & IQ_MASK;
      s_iq_chunk[2 * i]     = iq_ring[2 * idx];
      s_iq_chunk[2 * i + 1] = iq_ring[2 * idx + 1];
    }
    atomic_store_explicit(&iq_tail, t + IQ_CHUNK, memory_order_release);

    g_mutex_lock(&s_lock);
    for (int ci = 0; ci < TCI_MAX_CLIENTS; ci++) {
      Client *c = &s_cli[ci];
      if (!c->inuse || !c->iq_sub) { continue; }
      const double *src = s_iq_chunk;
      int n_out = IQ_CHUNK;
      if (c->iq_rate != in_rate) {
        if (!c->iq_rs || c->iq_rs_in != in_rate || c->iq_rs_out != c->iq_rate) {
          if (c->iq_rs) { destroy_resample(c->iq_rs); }
          if (!c->iq_out) { c->iq_out = g_new(double, 2 * IQ_OUT_MAX); }
          /* fc/ncoef 0 = auto anti-alias at 0.45 × min-rate (piHPSDR usage) */
          c->iq_rs = create_resample(1, IQ_CHUNK, s_iq_chunk, c->iq_out,
                                     in_rate, c->iq_rate, 0.0, 0, 1.0);
          c->iq_rs_in = in_rate;
          c->iq_rs_out = c->iq_rate;
        }
        n_out = xresample(c->iq_rs);
        src = c->iq_out;
      }
      for (int i = 0; i < n_out; i++) {
        float fi = (float)src[2 * i], fq = (float)src[2 * i + 1];
        if (s_iq_swap) { float t2 = fi; fi = fq; fq = t2; }
        if (s_iq_conj) { fq = -fq; }
        c->iq_blk[2 * c->iq_fill]     = fi;
        c->iq_blk[2 * c->iq_fill + 1] = fq;
        if (++c->iq_fill >= IQ_BLK_FRAMES) { iq_emit(c); }
      }
      if (c->wsi && c->txq && !g_queue_is_empty(c->txq)) {
        lws_callback_on_writable(c->wsi);
      }
    }
    g_mutex_unlock(&s_lock);
  }
}

static gpointer service_thread(gpointer data) {
  (void)data;
  while (s_run) {
    if (atomic_load(&s_au_active) > 0) { au_pump(); }
    if (atomic_load(&s_iq_active) > 0) { iq_pump(); }
    if (s_writable) {
      s_writable = 0;
      g_mutex_lock(&s_lock);
      for (int i = 0; i < TCI_MAX_CLIENTS; i++) {
        if (s_cli[i].inuse && s_cli[i].wsi && s_cli[i].txq && !g_queue_is_empty(s_cli[i].txq)) {
          lws_callback_on_writable(s_cli[i].wsi);
        }
      }
      g_mutex_unlock(&s_lock);
    }
    lws_service(s_ctx, 0);
    g_usleep(1000);
  }
  return NULL;
}

void tci_server_audio_push(const float *mono48k, int n) {
  if (!s_run || atomic_load_explicit(&s_au_active, memory_order_relaxed) <= 0) { return; }
  unsigned h = atomic_load_explicit(&au_head, memory_order_relaxed);
  unsigned t = atomic_load_explicit(&au_tail, memory_order_acquire);
  for (int i = 0; i < n; i++) {
    if (h - t >= AU_RING) { break; }          /* full → drop the tail        */
    au_ring[h & AU_MASK] = mono48k[i];
    h++;
  }
  atomic_store_explicit(&au_head, h, memory_order_release);
  /* lws_service blocks until a socket event — kick it so the service loop
   * comes around to au_pump (piHPSDR's tci_audio_wakeup does the same). */
  if (s_ctx) { lws_cancel_service(s_ctx); }
}

void tci_server_iq_push(const double *iq, int n_pairs, int rate) {
  if (!s_run || atomic_load_explicit(&s_iq_active, memory_order_relaxed) <= 0) { return; }
  atomic_store_explicit(&s_iq_in_rate, rate, memory_order_relaxed);
  unsigned h = atomic_load_explicit(&iq_head, memory_order_relaxed);
  unsigned t = atomic_load_explicit(&iq_tail, memory_order_acquire);
  for (int i = 0; i < n_pairs; i++) {
    if (h - t >= IQ_RING) { break; }          /* full → drop                 */
    unsigned idx = h & IQ_MASK;
    iq_ring[2 * idx]     = iq[2 * i];
    iq_ring[2 * idx + 1] = iq[2 * i + 1];
    h++;
  }
  atomic_store_explicit(&iq_head, h, memory_order_release);
  /* lws_service BLOCKS until a socket event — kick it like the audio path
   * does, or the stream only flushes when unrelated traffic (a dds
   * broadcast) happens to wake the loop. Found live with SDC: the IQ
   * "jumped" only on frequency changes. */
  if (s_ctx) { lws_cancel_service(s_ctx); }
}

void tci_server_tx_chrono(int nsamples) {
  if (!s_run || nsamples <= 0) { return; }
  uint32_t hdr[TCI_HDR_U32S];
  memset(hdr, 0, sizeof(hdr));
  hdr[1] = 48000;                      /* the exciter consumes 48 k mono      */
  hdr[2] = TCI_FMT_FLOAT32;
  hdr[5] = (uint32_t)nsamples;
  hdr[6] = TCI_TX_CHRONO;
  hdr[7] = 1;
  g_mutex_lock(&s_lock);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) {
    if (s_cli[i].inuse && s_cli[i].tx_owner) {
      cli_send_msg(&s_cli[i], 1, hdr, sizeof(hdr));
    }
  }
  g_mutex_unlock(&s_lock);
  s_writable = 1;
  if (s_ctx) { lws_cancel_service(s_ctx); }
}

/* ---- public API ------------------------------------------------------------ */

int tci_server_start(int port, const TciOps *ops) {
  if (s_run) { return 0; }
  s_ops = *ops;
  memset(&s_cli, 0, sizeof(s_cli));
  memset(&s_last, 0, sizeof(s_last));
  s_cw_delay_ms = 10;
  s_debug = g_getenv("SDRFL_TCI_DEBUG") != NULL;
  echo_reset();
  atomic_store(&s_au_active, 0);
  atomic_store(&au_head, 0u);
  atomic_store(&au_tail, 0u);
  atomic_store(&s_iq_active, 0);
  atomic_store(&iq_head, 0u);
  atomic_store(&iq_tail, 0u);
  atomic_store(&s_iq_in_rate, 0);
  s_iq_swap = g_getenv("SDRFL_TCI_IQ_SWAP") != NULL;
  s_iq_conj = g_getenv("SDRFL_TCI_IQ_CONJ") != NULL;

  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));
  signal(SIGPIPE, SIG_IGN);
  lws_set_log_level(LLL_ERR, NULL);
  info.port = port;
  info.protocols = s_protocols;
  info.gid = -1;
  info.uid = -1;
  s_ctx = lws_create_context(&info);
  if (!s_ctx) {
    fprintf(stderr, "tci: lws_create_context failed (port %d busy?)\n", port);
    return -1;
  }
  s_run = 1;
  s_thread = g_thread_new("sdrfl-tci", service_thread, NULL);
  s_reporter_id = g_timeout_add(500, tci_reporter, NULL);
  g_timeout_add(100, tci_sensors_tick, NULL);   /* self-removes when !s_run */
  fprintf(stderr, "tci: server up on port %d\n", port);
  return 0;
}

void tci_server_stop(void) {
  if (!s_run) { return; }
  /* ⛔ if a TCI client owns the key, unkey + revert to mic before teardown */
  int owned = 0;
  g_mutex_lock(&s_lock);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) {
    if (s_cli[i].inuse && s_cli[i].tx_owner) { s_cli[i].tx_owner = 0; owned = 1; }
  }
  g_mutex_unlock(&s_lock);
  if (owned) {
    s_ops.set_trx(0);
    s_ops.set_tx_source_tci(0);
    fprintf(stderr, "tci: server stopping while keyed via TCI — unkeyed\n");
  }
  s_run = 0;
  if (s_reporter_id) { g_source_remove(s_reporter_id); s_reporter_id = 0; }
  lws_cancel_service(s_ctx);
  g_thread_join(s_thread);
  s_thread = NULL;
  lws_context_destroy(s_ctx);
  s_ctx = NULL;
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) {
    if (s_cli[i].txq) {
      Msg *m;
      while ((m = g_queue_pop_head(s_cli[i].txq))) { g_free(m); }
      g_queue_free(s_cli[i].txq);
    }
    if (s_cli[i].iq_rs)  { destroy_resample(s_cli[i].iq_rs); }
    if (s_cli[i].iq_out) { g_free(s_cli[i].iq_out); }
  }
  memset(&s_cli, 0, sizeof(s_cli));
  atomic_store(&s_au_active, 0);
  atomic_store(&s_iq_active, 0);
  fprintf(stderr, "tci: server stopped\n");
}

int tci_server_running(void) { return s_run; }

int tci_server_clients(void) {
  int n = 0;
  g_mutex_lock(&s_lock);
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) { n += s_cli[i].inuse ? 1 : 0; }
  g_mutex_unlock(&s_lock);
  return n;
}

int tci_server_client_info(int i, char *buf, int len) {
  if (i < 0 || i >= TCI_MAX_CLIENTS) { return 0; }
  int ok = 0;
  g_mutex_lock(&s_lock);
  if (s_cli[i].inuse) {
    snprintf(buf, (size_t)len, "%s%s%s%s%s",
             s_cli[i].peer[0] ? s_cli[i].peer : "?",
             s_cli[i].agent[0] ? " · " : "", s_cli[i].agent,
             s_cli[i].au_sub ? " · audio" : "",
             s_cli[i].iq_sub ? " · iq" : "");
    ok = 1;
  }
  g_mutex_unlock(&s_lock);
  return ok;
}
