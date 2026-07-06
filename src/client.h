/*
 * client.h — reusable connection layer to a piHPSDR server.
 *
 * Owns the TCP/UDP sockets and a background receive thread that decodes
 * INFO_RX_SPECTRUM frames into a mutex-protected "latest frame". The GUI polls
 * client_latest() from the main thread. Builds on transport.{h,c} +
 * protocol.{h,c}. Does NOT send CMD_SCREEN / CMD_RX_FPS (those clobber the
 * operator's shared server-side display); the server's native width/fps is used.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PIHPSDR_CLIENT_CLIENT_H
#define PIHPSDR_CLIENT_CLIENT_H

#include <stdint.h>

#include "protocol.h"

/* One decoded panadapter frame (RX0). dBm for column i is dbm[i] - 200. */
typedef struct {
  int       width;                    /* number of valid columns (<= 4096) */
  uint8_t   dbm[SPECTRUM_DATA_SIZE];  /* raw column bytes                   */
  long long vfo_a_freq;               /* VFO-A dial frequency, Hz           */
  long long vfo_a_ctun_freq;          /* VFO-A CTUN frequency, Hz           */
  double    s_dbm;                    /* RX level (S-meter), dBm            */
  uint64_t  seq;                      /* bumped once per received frame     */
} ClientFrame;

typedef struct Client Client;

/* Error codes (negative returns from client_connect). */
enum {
  CLIENT_OK              =  0,
  CLIENT_ERR_CONNECT     = -1,
  CLIENT_ERR_VERSION     = -2,
  CLIENT_ERR_PASSWORD    = -3,
  CLIENT_ERR_PROTO       = -4,
};

Client *client_new(const char *host, int port, const char *pwd);

/*
 * TCP connect + handshake (version, KDF, PCM) + UDP registration + ingest of
 * the INFO_* burst to CMD_START_RADIO + enable RX0 spectrum streaming.
 * Blocking (~1s for the KDF). Returns CLIENT_OK or a negative CLIENT_ERR_*.
 */
int client_connect(Client *c);

/* Spawn the background receive thread. Call after a successful client_connect. */
void client_start(Client *c);

/*
 * If a frame newer than *last_seq is available, copy it into *out, update
 * *last_seq and return 1; otherwise return 0. Thread-safe (main thread).
 */
int client_latest(Client *c, ClientFrame *out, uint64_t *last_seq);

/* Relative VFO tune by hz (CMD_MOVE). Server convention: dial -= hz. */
void client_vfo_move(Client *c, int id, long long hz);

/*
 * Request `columns` panadapter columns (64..4096) via CMD_SCREEN. 0 leaves the
 * server's native width untouched. NB: this reconfigures the *shared* server
 * display width (see [[dont-clobber-server-config]]). The client remembers the
 * native width from the first frame and restores it on client_stop().
 * May be called before or after client_connect().
 */
void client_set_columns(Client *c, int columns);

/* Stop streaming, join the thread, close sockets. Safe to call once. */
void client_stop(Client *c);
void client_free(Client *c);

const char *client_strerror(int code);

#endif /* PIHPSDR_CLIENT_CLIENT_H */
