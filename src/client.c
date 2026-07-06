/*
 * client.c — see client.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "client.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <zlib.h>
#include <glib.h>

#include "transport.h"

static const uint8_t SYNCBYTES[4] = { 0xFA, 0xFA, 0xAF, 0xAF };

struct Client {
  char host[256];
  int  port;
  char pwd[128];

  int  tcp;
  int  udp;

  GThread *rx_thread_h;   /* UDP: spectrum + keepalive ping */
  GThread *tcp_thread_h;  /* TCP: drains echoes/pongs so the server never blocks */
  GMutex   lock;          /* protects `latest`            */
  GMutex   send_lock;     /* serialises TCP sends across threads */
  ClientFrame latest;
  volatile gboolean running;

  int      columns;         /* requested CMD_SCREEN width, 0 = native */
  int      native_width;    /* width of the first frame (server's own) */
  int      applied_columns; /* last width we requested via CMD_SCREEN  */
};

/* All TCP sends funnel through here (multiple threads send). */
static int client_send(Client *c, const void *buf, int n) {
  if (c->tcp < 0) {
    return -1;
  }
  g_mutex_lock(&c->send_lock);
  int rc = tp_send_all(c->tcp, buf, n);
  g_mutex_unlock(&c->send_lock);
  return rc;
}

Client *client_new(const char *host, int port, const char *pwd) {
  Client *c = g_new0(Client, 1);
  snprintf(c->host, sizeof(c->host), "%s", host ? host : "127.0.0.1");
  c->port = port;
  snprintf(c->pwd, sizeof(c->pwd), "%s", pwd ? pwd : "");
  c->tcp = -1;
  c->udp = -1;
  g_mutex_init(&c->lock);
  g_mutex_init(&c->send_lock);
  return c;
}

/* ---- handshake helpers (no stdout; return codes) --------------------- */

static int read_header(int tcp, HEADER *h) {
  if (tp_recv_all(tcp, h, sizeof(HEADER)) < 0) {
    return -1;
  }
  if (memcmp(h->sync, SYNCBYTES, 4) == 0) {
    return 0;
  }
  int matched = 0;
  while (matched != 4) {
    uint8_t ch;
    if (tp_recv_all(tcp, &ch, 1) < 0) {
      return -1;
    }
    if (ch == SYNCBYTES[matched]) {
      matched++;
    } else {
      matched = (ch == SYNCBYTES[0]) ? 1 : 0;
    }
  }
  memcpy(h->sync, SYNCBYTES, 4);
  if (tp_recv_all(tcp, (char *)h + 4, sizeof(HEADER) - 4) < 0) {
    return -1;
  }
  return 0;
}

static int do_handshake(Client *c, uint8_t udp_hash[SHA512_DIGEST_LENGTH]) {
  uint8_t s[SHA512_DIGEST_LENGTH];
  uint8_t ver[4];

  if (tp_recv_all(c->tcp, ver, 4) < 0) {
    return CLIENT_ERR_PROTO;
  }
  uint32_t got = ((uint32_t)ver[0] << 24) | ((uint32_t)ver[1] << 16) |
                 ((uint32_t)ver[2] << 8) | (uint32_t)ver[3];
  if (got != CLIENT_SERVER_VERSION) {
    return CLIENT_ERR_VERSION;
  }

  if (tp_recv_all(c->tcp, s, SHA512_DIGEST_LENGTH) < 0) {
    return CLIENT_ERR_PROTO;
  }
  cs_pwd_hash(s, c->pwd, udp_hash);
  if (tp_send_all(c->tcp, udp_hash, SHA512_DIGEST_LENGTH) < 0) {
    return CLIENT_ERR_PROTO;
  }

  uint8_t accept = 0;
  if (tp_recv_all(c->tcp, &accept, 1) < 0) {
    return CLIENT_ERR_PROTO;
  }
  if (accept != 0x7F) {
    return CLIENT_ERR_PASSWORD;
  }

  uint8_t comp = 0x40 | 0; /* propose PCM */
  if (tp_send_all(c->tcp, &comp, 1) < 0 || tp_recv_all(c->tcp, &comp, 1) < 0) {
    return CLIENT_ERR_PROTO;
  }
  return CLIENT_OK;
}

/* Read TCP messages until CMD_START_RADIO, consuming each payload by size. */
static int ingest_until_start(Client *c) {
  uint8_t payload[8192];
  for (;;) {
    HEADER h;
    if (read_header(c->tcp, &h) < 0) {
      return CLIENT_ERR_PROTO;
    }
    uint16_t type = from_16(h.data_type);
    int psize = cs_tcp_payload_size(type);
    if (psize > 0) {
      if (psize > (int)sizeof(payload) ||
          tp_recv_all(c->tcp, payload, psize) < 0) {
        return CLIENT_ERR_PROTO;
      }
    }
    if (type == CMD_START_RADIO) {
      return CLIENT_OK;
    }
  }
}

static int cl_send_rxspectrum(Client *c, int id, int state) {
  HEADER h;
  SYNC(h.sync);
  h.data_type = to_16(CMD_RX_SPECTRUM);
  h.b1 = id;
  h.b2 = state;
  h.s1 = 0;
  h.s2 = 0;
  return client_send(c, &h, sizeof(h));
}

/* Request a panadapter width (columns) from the server. */
static int cl_send_screen(Client *c, int hstack, int width) {
  HEADER h;
  SYNC(h.sync);
  h.data_type = to_16(CMD_SCREEN);
  h.b1 = hstack;
  h.b2 = 0;
  h.s1 = to_16(width);
  h.s2 = 0;
  return client_send(c, &h, sizeof(h));
}

/* Keepalive. The server drops a client that is silent on TCP for 30 s. */
static void cl_send_ping(Client *c) {
  HEADER h;
  SYNC(h.sync);
  h.data_type = to_16(CMD_PING);
  h.b1 = 0;
  h.b2 = 0;
  h.s1 = 0;
  h.s2 = 0;
  client_send(c, &h, sizeof(h));
}

int client_connect(Client *c) {
  c->tcp = tp_tcp_connect(c->host, c->port, 5000);
  if (c->tcp < 0) {
    return CLIENT_ERR_CONNECT;
  }

  uint8_t udp_hash[SHA512_DIGEST_LENGTH];
  int rc = do_handshake(c, udp_hash);
  if (rc != CLIENT_OK) {
    close(c->tcp);
    c->tcp = -1;
    return rc;
  }

  c->udp = tp_udp_connect(c->host, c->port);
  if (c->udp < 0) {
    close(c->tcp);
    c->tcp = -1;
    return CLIENT_ERR_CONNECT;
  }
  usleep(100000);
  if (send(c->udp, udp_hash, SHA512_DIGEST_LENGTH, 0) < 0) {
    close(c->tcp);
    close(c->udp);
    c->tcp = c->udp = -1;
    return CLIENT_ERR_PROTO;
  }

  rc = ingest_until_start(c);
  if (rc != CLIENT_OK) {
    close(c->tcp);
    close(c->udp);
    c->tcp = c->udp = -1;
    return rc;
  }

  cl_send_rxspectrum(c, 0, 1);
  return CLIENT_OK;
}

/* ---- receive thread -------------------------------------------------- */

/* Decode one INFO_RX_SPECTRUM datagram into `f`. Returns 0 on success. */
static int decode_spectrum(const uint8_t *buf, int len, ClientFrame *f) {
  if (len < (int)(sizeof(SPECTRUM_DATA) - SPECTRUM_DATA_SIZE)) {
    return -1;
  }
  const SPECTRUM_DATA *sd = (const SPECTRUM_DATA *)buf;
  int width = from_16(sd->width);
  if (width < 0 || width > SPECTRUM_DATA_SIZE) {
    return -1;
  }

  if (sd->compressed) {
    uLongf dst = SPECTRUM_DATA_SIZE;
    if (uncompress(f->dbm, &dst, sd->sample, from_16(sd->compressed_width)) != Z_OK ||
        (int)dst != width) {
      return -1;
    }
  } else {
    memcpy(f->dbm, sd->sample, width);
  }

  f->width           = width;
  f->vfo_a_freq      = (long long)from_64(sd->vfo_a_freq);
  f->vfo_a_ctun_freq = (long long)from_64(sd->vfo_a_ctun_freq);
  f->s_dbm           = from_double(sd->rxlvl);
  return 0;
}

static long mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* UDP: decode spectrum into the latest frame; also send a ~1s keepalive ping. */
static gpointer rx_thread(gpointer data) {
  Client *c = (Client *)data;
  static uint8_t dg[sizeof(SPECTRUM_DATA) + 64];
  ClientFrame tmp;
  memset(&tmp, 0, sizeof(tmp));

  /* 1s recv timeout so we still ping / re-check `running` when UDP is quiet. */
  struct timeval tv = { 1, 0 };
  setsockopt(c->udp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  long last_ping = mono_ms();

  while (c->running) {
    if (mono_ms() - last_ping >= 1000) {
      cl_send_ping(c);
      last_ping = mono_ms();
    }

    ssize_t n = recv(c->udp, dg, sizeof(dg), 0);
    if (n < (int)sizeof(HEADER)) {
      continue; /* timeout or too short */
    }
    HEADER *h = (HEADER *)dg;
    if (memcmp(h->sync, SYNCBYTES, 4) != 0) {
      continue;
    }
    if (from_16(h->data_type) != INFO_RX_SPECTRUM) {
      continue;
    }
    if (decode_spectrum(dg, (int)n, &tmp) != 0) {
      continue;
    }

    /* The first frame is at the server's native width; remember it so we can
     * restore it on stop, then apply the requested column count (if any). */
    if (c->native_width == 0) {
      c->native_width = tmp.width;
    }
    if (c->columns > 0 && c->applied_columns != c->columns) {
      cl_send_screen(c, 0, c->columns);
      c->applied_columns = c->columns;
    }

    g_mutex_lock(&c->lock);
    uint64_t seq = c->latest.seq + 1;
    memcpy(&c->latest, &tmp, sizeof(ClientFrame));
    c->latest.seq = seq;
    g_mutex_unlock(&c->lock);
  }
  return NULL;
}

/*
 * TCP: continuously drain the stream the server sends us (command echoes,
 * CMD_PONG replies to our pings, INFO_* updates). If we did not, our receive
 * buffer would fill and the server's send_tcp would back up. We consume each
 * message by its type-implied size; state updates beyond the spectrum stream
 * are not needed yet, so payloads are discarded.
 */
static gpointer tcp_thread(gpointer data) {
  Client *c = (Client *)data;
  static uint8_t payload[8192];

  while (c->running) {
    HEADER h;
    if (read_header(c->tcp, &h) < 0) {
      c->running = FALSE; /* server closed / error */
      break;
    }
    uint16_t type = from_16(h.data_type);
    int psize = cs_tcp_payload_size(type);
    if (psize > 0) {
      if (psize > (int)sizeof(payload) || tp_recv_all(c->tcp, payload, psize) < 0) {
        c->running = FALSE;
        break;
      }
    }
  }
  return NULL;
}

void client_start(Client *c) {
  c->running = TRUE;
  c->tcp_thread_h = g_thread_new("pihpsdr-tcp", tcp_thread, c);
  c->rx_thread_h  = g_thread_new("pihpsdr-rx",  rx_thread,  c);
}

int client_latest(Client *c, ClientFrame *out, uint64_t *last_seq) {
  int have = 0;
  g_mutex_lock(&c->lock);
  if (c->latest.seq != *last_seq) {
    memcpy(out, &c->latest, sizeof(ClientFrame));
    *last_seq = c->latest.seq;
    have = 1;
  }
  g_mutex_unlock(&c->lock);
  return have;
}

void client_vfo_move(Client *c, int id, long long hz) {
  U64_COMMAND cmd;
  SYNC(cmd.header.sync);
  cmd.header.data_type = to_16(CMD_MOVE);
  cmd.header.b1 = id;
  cmd.header.b2 = 0;
  cmd.header.s1 = 0;
  cmd.header.s2 = 0;
  cmd.u64 = to_64(hz);
  client_send(c, &cmd, sizeof(cmd));
}

void client_set_columns(Client *c, int columns) {
  if (columns > 0) {
    if (columns < 64) {
      columns = 64;
    }
    if (columns > SPECTRUM_DATA_SIZE) {
      columns = SPECTRUM_DATA_SIZE;
    }
  }
  c->columns = columns;
  /* If already streaming, the rx thread applies it on the next frame. */
}

void client_stop(Client *c) {
  if (!c->running && c->tcp < 0) {
    return;
  }
  c->running = FALSE;

  /* Join the UDP thread first so it stops sending pings / screen requests. */
  if (c->rx_thread_h) {
    g_thread_join(c->rx_thread_h);
    c->rx_thread_h = NULL;
  }

  if (c->tcp >= 0) {
    /* Restore the operator's native display width if we changed it. */
    if (c->applied_columns > 0 && c->native_width > 0 &&
        c->applied_columns != c->native_width) {
      cl_send_screen(c, 0, c->native_width);
    }
    cl_send_rxspectrum(c, 0, 0);       /* stop streaming to us */
    shutdown(c->tcp, SHUT_RDWR);        /* unblock the TCP reader */
  }

  if (c->tcp_thread_h) {
    g_thread_join(c->tcp_thread_h);
    c->tcp_thread_h = NULL;
  }

  if (c->tcp >= 0) {
    close(c->tcp);
    c->tcp = -1;
  }
  if (c->udp >= 0) {
    close(c->udp);
    c->udp = -1;
  }
}

void client_free(Client *c) {
  if (!c) {
    return;
  }
  client_stop(c);
  g_mutex_clear(&c->lock);
  g_mutex_clear(&c->send_lock);
  g_free(c);
}

const char *client_strerror(int code) {
  switch (code) {
    case CLIENT_OK:           return "ok";
    case CLIENT_ERR_CONNECT:  return "cannot connect to server";
    case CLIENT_ERR_VERSION:  return "protocol version mismatch (re-vendor client_server.h)";
    case CLIENT_ERR_PASSWORD: return "server rejected password";
    case CLIENT_ERR_PROTO:    return "protocol error";
    default:                  return "unknown error";
  }
}
