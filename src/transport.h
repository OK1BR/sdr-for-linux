/*
 * transport.h — low-level TCP/UDP transport for the piHPSDR client/server link.
 *
 * Mirrors piHPSDR's recv_tcp()/send_tcp() loop semantics (client_server.c): a
 * read/write is not complete until the exact requested byte count has moved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PIHPSDR_CLIENT_TRANSPORT_H
#define PIHPSDR_CLIENT_TRANSPORT_H

#include <stddef.h>

/*
 * Connect a TCP stream socket to host:port with a connect() timeout.
 * Returns the socket fd (>= 0), or -1 on error / -2 on timeout.
 */
int tp_tcp_connect(const char *host, int port, int timeout_ms);

/*
 * Create a UDP datagram socket and connect() it to the same host:port, so that
 * subsequent send()/recv() go to/from the server without an address argument.
 * Returns the socket fd (>= 0) or -1 on error.
 */
int tp_udp_connect(const char *host, int port);

/*
 * Read exactly `bytes` bytes into `buf`, looping over short reads.
 * Returns `bytes` on success, or -1 on error / EOF / peer death.
 */
int tp_recv_all(int fd, void *buf, int bytes);

/*
 * Send exactly `bytes` bytes from `buf`, looping over short writes.
 * Returns `bytes` on success, or -1 on error.
 */
int tp_send_all(int fd, const void *buf, int bytes);

#endif /* PIHPSDR_CLIENT_TRANSPORT_H */
