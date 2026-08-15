/*
 * sdr-for-linux — sdrfl-tci-send: a minimal TCI command-line client (dev tool).
 *
 * Connects to the app's TCI server, sends each argument as one text command,
 * prints everything the server says, and exits. Made for the RTTY live gate
 * (keying text without a logger: `sdrfl-tci-send 'rtty_macros:0,TEST DE OK1BR;'`)
 * and generally for poking the TCI surface while log-for-linux integration is
 * developed. Text frames only — no audio/IQ streams.
 *
 *   sdrfl-tci-send [-h host] [-p port] [-w wait_s] 'cmd:args;' ...
 *
 * Defaults: 127.0.0.1:40001, wait 2 s after the last command (collects the
 * echo/broadcast). Exit 0 = connected and sent; 1 = connection failed.
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
static int  s_up, s_done_connecting, s_next;   /* next argv command to send */
static int  s_argc; static char **s_argv;      /* commands (argv slice)     */

static int cb(struct lws *wsi, enum lws_callback_reasons reason,
              void *user, void *in, size_t len) {
  (void)user;
  switch (reason) {
  case LWS_CALLBACK_CLIENT_ESTABLISHED:
    s_wsi = wsi; s_up = 1; s_done_connecting = 1;
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
      if (s_next < s_argc) { lws_callback_on_writable(wsi); }
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
  const char *host = "127.0.0.1";
  int port = 40001, wait_s = 2, i = 1;
  for (; i < argc; i++) {
    if      (!strcmp(argv[i], "-h") && i + 1 < argc) { host   = argv[++i]; }
    else if (!strcmp(argv[i], "-p") && i + 1 < argc) { port   = atoi(argv[++i]); }
    else if (!strcmp(argv[i], "-w") && i + 1 < argc) { wait_s = atoi(argv[++i]); }
    else { break; }
  }
  if (i >= argc) {
    fprintf(stderr, "usage: sdrfl-tci-send [-h host] [-p port] [-w wait_s] 'cmd:args;' ...\n");
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

  struct lws_client_connect_info cc;
  memset(&cc, 0, sizeof cc);
  cc.context = ctx;
  cc.address = host;
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
   * the listen window (echoes/broadcasts land on stdout as they arrive). */
  long deadline_ms = -1;
  while (1) {
    lws_service(ctx, 50);
    if (!s_done_connecting) { continue; }
    if (!s_up) { break; }                          /* failed or server closed */
    if (s_next >= s_argc) {
      if (deadline_ms < 0) { deadline_ms = wait_s * 1000; }
      deadline_ms -= 50;
      if (deadline_ms <= 0) { break; }
    } else {
      lws_callback_on_writable(s_wsi);
    }
  }
  int ok = s_next >= s_argc;
  lws_context_destroy(ctx);
  return ok ? 0 : 1;
}
