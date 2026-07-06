/*
 * transport.c — see transport.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "transport.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* Resolve host:port into a single IPv4 sockaddr_in. Returns 0 on success. */
static int resolve_v4(const char *host, int port, struct sockaddr_in *out) {
  struct addrinfo hints;
  struct addrinfo *res = NULL;
  char portstr[16];

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;      /* the wire protocol / server bind is IPv4 */
  hints.ai_socktype = SOCK_STREAM;
  snprintf(portstr, sizeof(portstr), "%d", port);

  int rc = getaddrinfo(host, portstr, &hints, &res);
  if (rc != 0 || res == NULL) {
    fprintf(stderr, "resolve %s:%d failed: %s\n", host, port, gai_strerror(rc));
    return -1;
  }

  memcpy(out, res->ai_addr, sizeof(struct sockaddr_in));
  freeaddrinfo(res);
  return 0;
}

int tp_tcp_connect(const char *host, int port, int timeout_ms) {
  struct sockaddr_in addr;
  if (resolve_v4(host, port, &addr) != 0) {
    return -1;
  }

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket(tcp)");
    return -1;
  }

  /* Non-blocking connect with select() timeout. */
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (rc < 0 && errno == EINPROGRESS) {
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    rc = select(fd + 1, NULL, &wset, NULL, &tv);
    if (rc == 0) {
      fprintf(stderr, "connect %s:%d timed out\n", host, port);
      close(fd);
      return -2;
    }
    if (rc < 0) {
      perror("select(connect)");
      close(fd);
      return -1;
    }
    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
    if (soerr != 0) {
      fprintf(stderr, "connect %s:%d: %s\n", host, port, strerror(soerr));
      close(fd);
      return -1;
    }
  } else if (rc < 0) {
    perror("connect(tcp)");
    close(fd);
    return -1;
  }

  /* Restore blocking mode for the simple recv/send loops. */
  fcntl(fd, F_SETFL, flags);

  /* Control traffic is latency-sensitive and small; disable Nagle. */
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  return fd;
}

int tp_udp_connect(const char *host, int port) {
  struct sockaddr_in addr;
  if (resolve_v4(host, port, &addr) != 0) {
    return -1;
  }

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    perror("socket(udp)");
    return -1;
  }

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("connect(udp)");
    close(fd);
    return -1;
  }

  return fd;
}

int tp_recv_all(int fd, void *buf, int bytes) {
  char *p = (char *)buf;
  int got = 0;

  while (got < bytes) {
    ssize_t rc = recv(fd, p + got, (size_t)(bytes - got), 0);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("recv");
      return -1;
    }
    if (rc == 0) {
      /* Peer closed the connection. */
      fprintf(stderr, "recv: connection closed (%d/%d bytes)\n", got, bytes);
      return -1;
    }
    got += (int)rc;
  }

  return got;
}

int tp_send_all(int fd, const void *buf, int bytes) {
  const char *p = (const char *)buf;
  int sent = 0;

  while (sent < bytes) {
    ssize_t rc = send(fd, p + sent, (size_t)(bytes - sent), 0);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("send");
      return -1;
    }
    sent += (int)rc;
  }

  return sent;
}
