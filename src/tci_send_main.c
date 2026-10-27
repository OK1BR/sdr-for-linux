/*
 * sdr-for-linux — sdrfl-tci-send: a minimal TCI command-line client (dev tool).
 *
 * Connects to the app's TCI server, sends each argument as one text command,
 * prints everything the server says, and exits. Made for the RTTY live gate
 * (keying text without a logger: `sdrfl-tci-send 'rtty_macros:0,TEST DE OK1BR;'`)
 * and generally for poking the TCI surface while log-for-linux integration is
 * developed. Text frames only — no audio/IQ streams.
 *
 *   sdrfl-tci-send [-h host] [-p port] [-u socket_path] [-w wait_s] 'cmd:args;' ...
 *
 * Defaults: 127.0.0.1:40001, wait 2 s after the last command (collects the
 * echo/broadcast); -u dials the app's Unix socket instead (issue #46,
 * default path $XDG_RUNTIME_DIR/sdr-for-linux/tci.sock). Exit 0 = connected
 * and sent; 1 = connection failed.
 *
 * ⛔ This tool has no keying authority of its own: whatever it sends lands in
 * the same tci_server → ops → tx_gate path as any TCI client. It is exactly
 * as safe (and exactly as armed) as SDC or a logger on the same port.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <libwebsockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct lws *s_wsi;
static struct lws_context *s_ctx;
static int  s_up, s_done_connecting, s_next;   /* next argv command to send */
static int  s_argc; static char **s_argv;      /* commands (argv slice)     */
static int  s_wait_s;                          /* -w: listen window after the last command */
/* Timers live in lws's own scheduler (lws_sul): lws_service() ignores its
 * timeout argument since v3.2 and blocks until the next scheduled event or
 * network traffic — so a wall-clock deadline checked between service calls
 * never fires against a QUIET server (the offline stub, 2026-10-27: the tool
 * hung after its last command), while a chatty live radio hid that. A sul
 * wakes the loop exactly when the window ends; a second one caps the connect
 * phase so an unanswered dial cannot hang either. */
static lws_sorted_usec_list_t s_sul_wait, s_sul_connect;
static volatile int s_timeout;                 /* -w window (or connect cap) elapsed */
/* ⛔ A sul fires at the TOP of lws_service(), before its poll(); the loop
 * condition is only re-read after lws_service() RETURNS, and with nothing
 * else scheduled that poll() waits "forever" (LWS_POLL_WAIT_LIMIT) — the
 * flag alone hung the tool until the server happened to send something
 * (measured 2026-10-27). lws_cancel_service() pokes the wake pipe, so the
 * poll() returns at once and the loop sees the flag. */
static void sul_timeout(lws_sorted_usec_list_t *sul) {
  (void)sul;
  s_timeout = 1;
  lws_cancel_service(s_ctx);
}

static int cb(struct lws *wsi, enum lws_callback_reasons reason,
              void *user, void *in, size_t len) {
  (void)user;
  switch (reason) {
  case LWS_CALLBACK_CLIENT_ESTABLISHED:
    s_wsi = wsi; s_up = 1; s_done_connecting = 1;
    lws_sul_cancel(&s_sul_connect);
    lws_callback_on_writable(wsi);
    return 0;
  case LWS_CALLBACK_CLIENT_RECEIVE:
    if (!lws_frame_is_binary(wsi)) {
      fwrite(in, 1, len, stdout);
      fputc('\n', stdout);
      fflush(stdout);
    }
    return 0;
  case LWS_CALLBACK_CLIENT_WRITEABLE:
    if (s_next < s_argc) {
      unsigned char buf[LWS_PRE + 512];
      size_t n = strlen(s_argv[s_next]);
      if (n > 512) { n = 512; }
      memcpy(buf + LWS_PRE, s_argv[s_next], n);
      lws_write(wsi, buf + LWS_PRE, n, LWS_WRITE_TEXT);
      fprintf(stderr, ">> %s\n", s_argv[s_next]);
      s_next++;
      if (s_next < s_argc) {
        lws_callback_on_writable(wsi);
      } else {                                 /* last one out: open the -w window */
        lws_sul_schedule(s_ctx, 0, &s_sul_wait, sul_timeout,
                         (lws_usec_t)s_wait_s * LWS_US_PER_SEC);
      }
    }
    return 0;
  case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    fprintf(stderr, "tci-send: connect failed: %s\n", in ? (char *)in : "?");
    s_done_connecting = 1;
    return -1;
  case LWS_CALLBACK_CLIENT_CLOSED:
    s_up = 0;
    return 0;
  default:
    return 0;
  }
}

static const struct lws_protocols protocols[] = {
  { "tci", cb, 0, 8192, 0, NULL, 0 },
  { NULL, NULL, 0, 0, 0, NULL, 0 }
};

int main(int argc, char **argv) {
  const char *host = "127.0.0.1", *sock = NULL;
  int port = 40001, wait_s = 2, i = 1;
  for (; i < argc; i++) {
    if      (!strcmp(argv[i], "-h") && i + 1 < argc) { host   = argv[++i]; }
    else if (!strcmp(argv[i], "-p") && i + 1 < argc) { port   = atoi(argv[++i]); }
    else if (!strcmp(argv[i], "-u") && i + 1 < argc) { sock   = argv[++i]; }
    else if (!strcmp(argv[i], "-w") && i + 1 < argc) { wait_s = atoi(argv[++i]); }
    else { break; }
  }
  if (i >= argc) {
    fprintf(stderr, "usage: sdrfl-tci-send [-h host] [-p port] [-u socket_path] [-w wait_s] 'cmd:args;' ...\n"
                    "  -u  connect over the app's Unix socket instead of TCP\n"
                    "      (default path: $XDG_RUNTIME_DIR/sdr-for-linux/tci.sock)\n");
    return 1;
  }
  s_argv = argv + i;
  s_argc = argc - i;

  lws_set_log_level(LLL_ERR, NULL);
  struct lws_context_creation_info ci;
  memset(&ci, 0, sizeof ci);
  ci.port = CONTEXT_PORT_NO_LISTEN;
  ci.protocols = protocols;
  struct lws_context *ctx = lws_create_context(&ci);
  if (!ctx) { fprintf(stderr, "tci-send: lws context failed\n"); return 1; }

  /* Unix socket (issue #46): lws dials a socket path when the ADDRESS (not
   * the URL path) starts with '+' (lib/core-net/client/connect2.c); port is
   * ignored, the Host: header still needs a name. */
  char uaddr[256];
  if (sock) { snprintf(uaddr, sizeof uaddr, "+%s", sock); host = "localhost"; }
  struct lws_client_connect_info cc;
  memset(&cc, 0, sizeof cc);
  cc.context = ctx;
  cc.address = sock ? uaddr : host;
  cc.port    = port;
  cc.path    = "/";
  cc.host    = host;
  cc.origin  = host;
  cc.protocol = "tci";
  if (!lws_client_connect_via_info(&cc)) {
    fprintf(stderr, "tci-send: connect init failed\n");
    lws_context_destroy(ctx);
    return 1;
  }

  /* Service until connected (or failed), then until all commands are out plus
   * the listen window (echoes/broadcasts land on stdout as they arrive). The
   * window is an lws_sul timer (see the note at s_sul_wait): -w counts real
   * seconds whether the server is chatty (live radio) or silent (stub); a
   * one-shot trx:0,true keyed over TCI unkeys the instant the client
   * disconnects (server safety), so hold it with -w N. The dial itself is
   * capped at 5 s. */
  s_ctx = ctx;
  s_wait_s = wait_s;
  lws_sul_schedule(ctx, 0, &s_sul_connect, sul_timeout, 5 * LWS_US_PER_SEC);
  while (!s_timeout) {
    lws_service(ctx, 0);
    if (!s_done_connecting) { continue; }
    if (!s_up) { break; }                          /* failed or server closed */
    if (s_next < s_argc) { lws_callback_on_writable(s_wsi); }
  }
  if (!s_done_connecting) { fprintf(stderr, "tci-send: connect timed out\n"); }
  int ok = s_next >= s_argc;
  lws_context_destroy(ctx);
  return ok ? 0 : 1;
}
