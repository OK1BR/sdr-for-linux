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
#include <string.h>
#include <signal.h>

#include "tci_server.h"

#define TCI_MAX_CLIENTS 8
#define TCI_PROTO_LINE  "protocol:ExpertSDR3,1.9;"   /* clients key on this name */
#define TCI_DEVICE_LINE "device:SDR-for-Linux;"

typedef struct {
  struct lws *wsi;
  int         inuse;
  int         init_sent;     /* handshake block queued                       */
  GQueue     *txq;           /* char* messages waiting for SERVER_WRITEABLE  */
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

static void cli_send(Client *c, const char *msg) {
  if (!c->inuse || !c->txq) { return; }
  g_queue_push_tail(c->txq, g_strdup(msg));
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

/* ---- initial handshake (main thread; ops needed for the state block) ------ */

static gboolean send_initial_idle(gpointer data) {
  Client *c = (Client *)data;
  int lo, hi;
  if (!s_run || !c->inuse) { return G_SOURCE_REMOVE; }
  s_ops.get_filter(&lo, &hi);
  g_mutex_lock(&s_lock);
  cli_send(c, TCI_PROTO_LINE);
  cli_send(c, TCI_DEVICE_LINE);
  cli_send(c, "receive_only:false;");
  cli_send(c, "trx_count:1;");
  cli_send(c, "channel_count:1;");
  cli_send(c, "vfo_limits:0,61440000;");
  g_mutex_unlock(&s_lock);
  tci_sendf(c, "if_limits:-%d,%d;", s_ops.get_rate() / 2, s_ops.get_rate() / 2);
  tci_sendf(c, "modulations_list:am,lsb,usb,cwl,cwu;");
  tci_sendf(c, "mute:%s;", s_ops.get_mute() ? "true" : "false");
  tci_sendf(c, "volume:%d;", (int)(s_ops.get_volume() - 0.5));
  tci_sendf(c, "dds:0,%lld;", s_ops.get_freq());
  tci_sendf(c, "if:0,0,0;");
  tci_sendf(c, "vfo:0,0,%lld;", s_ops.get_freq());
  tci_sendf(c, "modulation:0,%s;", s_ops.get_mode());
  tci_sendf(c, "rx_filter_band:0,%d,%d;", lo, hi);
  tci_sendf(c, "trx:0,%s;", s_ops.get_trx() ? "true" : "false");
  tci_sendf(c, "tune:0,%s;", s_ops.get_tune() ? "true" : "false");
  tci_sendf(c, "drive:0,%d;", (int)(s_ops.get_drive() + 0.5));
  tci_sendf(c, "tune_drive:0,%d;", (int)(s_ops.get_tune_drive() + 0.5));
  tci_sendf(c, "cw_macros_speed:%d;", s_ops.get_cw_speed());
  tci_sendf(c, "cw_macros_delay:%d;", s_cw_delay_ms);
  tci_sendf(c, "tx_enable:0,%s;", s_ops.get_tx_enable() ? "true" : "false");
  tci_sendf(c, "tx_frequency:%lld;", s_ops.get_freq());
  tci_sendf(c, "ready;");
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

/* One parsed command: lowercase name + up to 8 raw args. */
static void tci_exec(Client *c, char *name, char **av, int ac) {
  for (char *p = name; *p; p++) { *p = (char)g_ascii_tolower(*p); }

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
      /* arg3 "tci" = client wants to source TX audio over TCI — that is
       * F6d-2c; refuse the key rather than transmit mic audio he didn't
       * intend. Everything else keys through the normal gate path. */
      if (want && ac > 2 && g_ascii_strcasecmp(av[2], "tci") == 0) {
        fprintf(stderr, "tci: trx source 'tci' not supported yet — refusing key\n");
      } else {
        s_ops.set_trx(want);
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
  } else if (strcmp(name, "start") == 0 || strcmp(name, "stop") == 0) {
    /* device start/stop — we are always running; acknowledge by echo */
    tci_sendf(c, "%s;", name);
  }
  /* anything else: ignored (per spec: invalid commands are ignored) */
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
      s_cli[idx].wsi = wsi;
      s_cli[idx].inuse = 1;
      s_cli[idx].init_sent = 0;
      s_cli[idx].txq = g_queue_new();
    }
    g_mutex_unlock(&s_lock);
    *slot = idx;
    if (idx < 0) { return -1; }             /* table full — refuse */
    fprintf(stderr, "tci: client %d connected\n", idx);
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
    if (lws_frame_is_binary(wsi)) { return 0; }   /* audio/IQ = later phases */
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
    char *msg = NULL;
    int more = 0;
    g_mutex_lock(&s_lock);
    if (c->txq) {
      msg = g_queue_pop_head(c->txq);
      more = !g_queue_is_empty(c->txq);
    }
    g_mutex_unlock(&s_lock);
    if (msg) {
      size_t n = strlen(msg);
      unsigned char *buf = g_malloc(LWS_PRE + n);
      memcpy(buf + LWS_PRE, msg, n);
      int rc = lws_write(wsi, buf + LWS_PRE, n, LWS_WRITE_TEXT);
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
    c->inuse = 0;
    c->wsi = NULL;
    if (c->txq) {
      char *m;
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

static gpointer service_thread(gpointer data) {
  (void)data;
  while (s_run) {
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

/* ---- public API ------------------------------------------------------------ */

int tci_server_start(int port, const TciOps *ops) {
  if (s_run) { return 0; }
  s_ops = *ops;
  memset(&s_cli, 0, sizeof(s_cli));
  memset(&s_last, 0, sizeof(s_last));
  s_cw_delay_ms = 10;

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
  fprintf(stderr, "tci: server up on port %d\n", port);
  return 0;
}

void tci_server_stop(void) {
  if (!s_run) { return; }
  s_run = 0;
  if (s_reporter_id) { g_source_remove(s_reporter_id); s_reporter_id = 0; }
  lws_cancel_service(s_ctx);
  g_thread_join(s_thread);
  s_thread = NULL;
  lws_context_destroy(s_ctx);
  s_ctx = NULL;
  for (int i = 0; i < TCI_MAX_CLIENTS; i++) {
    if (s_cli[i].txq) {
      char *m;
      while ((m = g_queue_pop_head(s_cli[i].txq))) { g_free(m); }
      g_queue_free(s_cli[i].txq);
    }
  }
  memset(&s_cli, 0, sizeof(s_cli));
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
