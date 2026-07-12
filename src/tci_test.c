/*
 * sdrfl-tci-test — offline gate for the TCI server (F6d-2a). No radio, no GUI.
 *
 * Starts tci_server on 127.0.0.1:40123 with stub ops, connects a real
 * WebSocket client (libwebsockets), and verifies:
 *   - the init handshake arrives and ends with "ready;" (protocol line first),
 *   - set commands round-trip: vfo / modulation / drive / cw_macros_speed
 *     land in the stub AND are broadcast back,
 *   - cw_macros text reaches the stub with reserved chars un-escaped,
 *   - cw_macros_stop aborts, trx with source "tci" is refused (F6d-2c),
 *     plain trx keys the stub.
 *
 * The server dispatches commands to the GLib default main context, so the
 * test pumps it; the LWS client runs in its own thread.
 */
#include <glib.h>
#include <libwebsockets.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tci_server.h"

/* ---- stub radio state ------------------------------------------------------ */

static struct {
  long long freq;
  char      mode[16];
  int       flo, fhi;
  double    drive, tdrive, vol;
  int       trx, tune, mute, wpm;
  char      cw_text[256];
  int       cw_stopped;
  int       tx_src;         /* TCI is the TX audio source                  */
  int       txa_samples;    /* TX audio samples received                   */
  char      spot_call[20];  /* last spot received (F6d-2e)                 */
  long long spot_hz;
  unsigned  spot_argb;
  int       spot_deleted, spot_cleared;
} S = { 14100000, "usb", 150, 2850, 25, 10, -12, 0, 0, 0, 20, "", 0, 0, 0,
        "", 0, 0, 0, 0 };

static long long s_get_freq(void) { return S.freq; }
static void s_set_freq(long long f) { S.freq = f; }
static const char *s_get_mode(void) { return S.mode; }
static int s_set_mode(const char *m) { g_strlcpy(S.mode, m, sizeof(S.mode)); return 0; }
static void s_get_filter(int *lo, int *hi) { *lo = S.flo; *hi = S.fhi; }
static void s_set_filter(int lo, int hi) { S.flo = lo; S.fhi = hi; }
static double s_get_drive(void) { return S.drive; }
static void s_set_drive(double v) { S.drive = v; }
static double s_get_tdrive(void) { return S.tdrive; }
static void s_set_tdrive(double v) { S.tdrive = v; }
static int s_get_trx(void) { return S.trx; }
static int s_set_trx(int on) { S.trx = on; return 0; }
static int s_get_tune(void) { return S.tune; }
static int s_set_tune(int on) { S.tune = on; return 0; }
static double s_get_vol(void) { return S.vol; }
static void s_set_vol(double v) { S.vol = v; }
static int s_get_mute(void) { return S.mute; }
static void s_set_mute(int on) { S.mute = on; }
static int s_get_wpm(void) { return S.wpm; }
static void s_set_wpm(int w) { S.wpm = w; }
static void s_cw_send(const char *t) { g_strlcat(S.cw_text, t, sizeof(S.cw_text)); }
static void s_cw_stop(void) { S.cw_stopped = 1; }
static int s_get_txen(void) { return 1; }
static int s_get_rate(void) { return 192000; }
static double s_get_smeter(void) { return -73.5; }
static void s_get_txm(double *mic, double *rms, double *pep, double *swr) {
  *mic = -20.0; *rms = 47.4; *pep = 67.5; *swr = 1.7;
}
static int s_set_tx_src(int on) { S.tx_src = on; return 0; }
static void s_txa_push(const float *m, int n) { (void)m; S.txa_samples += n; }
static void s_spot_add(const char *call, const char *mode, long long hz,
                       unsigned argb, const char *text) {
  (void)mode; (void)text;
  g_strlcpy(S.spot_call, call, sizeof(S.spot_call));
  S.spot_hz = hz;
  S.spot_argb = argb;
}
static void s_spot_del(const char *call) { (void)call; S.spot_deleted = 1; }
static void s_spot_clear(void) { S.spot_cleared = 1; }

static const TciOps STUB_OPS = {
  s_get_freq, s_set_freq, s_get_mode, s_set_mode, s_get_filter, s_set_filter,
  s_get_drive, s_set_drive, s_get_tdrive, s_set_tdrive, s_get_trx, s_set_trx,
  s_get_tune, s_set_tune, s_get_vol, s_set_vol, s_get_mute, s_set_mute,
  s_get_wpm, s_set_wpm, s_cw_send, s_cw_stop, s_get_txen, s_get_rate,
  s_get_smeter, s_get_txm, s_set_tx_src, s_txa_push,
  NULL,                                    /* iq_rate_changed: no persistence */
  s_spot_add, s_spot_del, s_spot_clear,
};

/* ---- LWS test client -------------------------------------------------------- */

static GMutex      c_lock;
static GString    *c_rx;          /* text the client received                 */
static GByteArray *c_bin;         /* binary frames (audio Stream blocks)      */
static struct lws *c_wsi;
static struct lws_context *c_ctx;
static volatile int c_up, c_run = 1, c_send_pending, c_bin_pending;
static char        c_out[512];
static uint8_t     c_bin_out[4096];
static size_t      c_bin_len;

static int client_cb(struct lws *wsi, enum lws_callback_reasons reason,
                     void *user, void *in, size_t len) {
  (void)user;
  switch (reason) {
  case LWS_CALLBACK_CLIENT_ESTABLISHED:
    c_wsi = wsi;
    c_up = 1;
    return 0;
  case LWS_CALLBACK_CLIENT_RECEIVE:
    g_mutex_lock(&c_lock);
    if (lws_frame_is_binary(wsi)) {
      g_byte_array_append(c_bin, (const guint8 *)in, (guint)len);
    } else {
      g_string_append_len(c_rx, (const char *)in, (gssize)len);
    }
    g_mutex_unlock(&c_lock);
    return 0;
  case LWS_CALLBACK_CLIENT_WRITEABLE:
    if (c_send_pending) {
      unsigned char buf[LWS_PRE + sizeof(c_out)];
      size_t n = strlen(c_out);
      memcpy(buf + LWS_PRE, c_out, n);
      lws_write(wsi, buf + LWS_PRE, n, LWS_WRITE_TEXT);
      c_send_pending = 0;
    }
    if (c_bin_pending) {
      unsigned char buf[LWS_PRE + sizeof(c_bin_out)];
      memcpy(buf + LWS_PRE, c_bin_out, c_bin_len);
      lws_write(wsi, buf + LWS_PRE, c_bin_len, LWS_WRITE_BINARY);
      c_bin_pending = 0;
    }
    return 0;
  case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    fprintf(stderr, "client: connection error: %s\n", in ? (char *)in : "?");
    c_run = 0;
    return -1;
  case LWS_CALLBACK_CLOSED_CLIENT_HTTP:
  case LWS_CALLBACK_CLIENT_CLOSED:
    c_up = 0;
    return 0;
  default:
    return 0;
  }
}

static const struct lws_protocols c_protocols[] = {
  { "tci", client_cb, 0, 8192, 0, NULL, 0 },
  { NULL, NULL, 0, 0, 0, NULL, 0 }
};

static gpointer client_thread(gpointer data) {
  struct lws_context *ctx = (struct lws_context *)data;
  while (c_run) {
    if ((c_send_pending || c_bin_pending) && c_wsi) { lws_callback_on_writable(c_wsi); }
    lws_service(ctx, 0);
    g_usleep(1000);
  }
  return NULL;
}

static void client_send(const char *msg) {
  g_strlcpy(c_out, msg, sizeof(c_out));
  c_send_pending = 1;
  /* lws_service blocks on socket events — wake the client loop so the
   * pending write goes out even when the connection is otherwise quiet. */
  if (c_ctx) { lws_cancel_service(c_ctx); }
}

static void client_send_bin(const void *data, size_t len) {
  if (len > sizeof(c_bin_out)) { return; }
  memcpy(c_bin_out, data, len);
  c_bin_len = len;
  c_bin_pending = 1;
  if (c_ctx) { lws_cancel_service(c_ctx); }
}

/* ---- helpers ---------------------------------------------------------------- */

static int rx_contains(const char *needle) {
  g_mutex_lock(&c_lock);
  int hit = strstr(c_rx->str, needle) != NULL;
  g_mutex_unlock(&c_lock);
  return hit;
}

/* Pump the GLib main context (server command dispatch) until cond() or timeout. */
static int wait_for(int (*cond)(void), int ms) {
  for (int i = 0; i < ms; i++) {
    while (g_main_context_iteration(NULL, FALSE)) {}
    if (cond()) { return 1; }
    g_usleep(1000);
  }
  return 0;
}

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

static int cond_ready(void)   { return rx_contains("ready;"); }
static int cond_freq(void)    { return S.freq == 7020000; }
static int cond_mode(void)    { return strcmp(S.mode, "cw") == 0; }
static int cond_drive(void)   { return (int)(S.drive + 0.5) == 42; }
static int cond_wpm(void)     { return S.wpm == 31; }
static int cond_cwtext(void)  { return strstr(S.cw_text, "TEST: DE OK1BR") != NULL; }
static int cond_stopped(void) { return S.cw_stopped; }
static int cond_trx_on(void)  { return S.trx == 1; }
static int cond_trx_off(void) { return S.trx == 0; }
static int cond_tci_keyed(void)   { return S.trx == 1 && S.tx_src == 1; }
static int cond_tci_unkeyed(void) { return S.trx == 0 && S.tx_src == 0; }
static int cond_txa(void)         { return S.txa_samples >= 512; }
static int cond_chrono(void) {
  g_mutex_lock(&c_lock);
  int hit = 0;
  if (c_bin->len >= 64) {
    const guint32 *h = (const guint32 *)c_bin->data;
    hit = (h[6] == 3 && h[5] == 512);            /* TX_CHRONO for 512 samples */
  }
  g_mutex_unlock(&c_lock);
  return hit;
}
static int cond_bc(void)      { return rx_contains("vfo:0,0,7020000;") && rx_contains("modulation:0,cw;"); }

int main(void) {
  printf("=== TCI server gate (offline) ===\n");
  c_rx = g_string_new(NULL);
  c_bin = g_byte_array_new();

  if (tci_server_start(40123, &STUB_OPS) != 0) {
    printf("FAIL — server would not start on :40123\n");
    return 1;
  }

  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));
  lws_set_log_level(LLL_ERR, NULL);
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.protocols = c_protocols;
  info.gid = -1;
  info.uid = -1;
  struct lws_context *cctx = lws_create_context(&info);
  c_ctx = cctx;

  struct lws_client_connect_info ci;
  memset(&ci, 0, sizeof(ci));
  ci.context = cctx;
  ci.address = "127.0.0.1";
  ci.port = 40123;
  ci.path = "/";
  ci.host = "127.0.0.1";
  ci.origin = "127.0.0.1";
  ci.protocol = "tci";
  lws_client_connect_via_info(&ci);
  GThread *ct = g_thread_new("tci-test-client", client_thread, cctx);

  check("client connects + full handshake ends with ready;", wait_for(cond_ready, 3000));
  check("handshake announces protocol:ExpertSDR3", rx_contains("protocol:ExpertSDR3,1.9;"));
  check("handshake carries device + vfo + modulation state",
        rx_contains("device:") && rx_contains("vfo:0,0,14100000;") && rx_contains("modulation:0,usb;"));
  check("handshake announces the device IQ rate", rx_contains("iq_samplerate:48000;"));

  client_send("vfo:0,0,7020000;modulation:0,CW;drive:0,42;cw_macros_speed:31;");
  check("vfo set lands in the radio (7.020 MHz)", wait_for(cond_freq, 2000));
  check("modulation set lands (cw, case-insensitive)", wait_for(cond_mode, 1000));
  check("drive set lands (42)", wait_for(cond_drive, 1000));
  check("cw speed set lands (31 WPM)", wait_for(cond_wpm, 1000));
  check("server broadcasts the new state back", wait_for(cond_bc, 2000));

  client_send("cw_macros:0,TEST^ DE OK1BR;");
  check("cw_macros queues text with ^ unescaped to :", wait_for(cond_cwtext, 2000));

  client_send("cw_macros_stop;");
  check("cw_macros_stop aborts", wait_for(cond_stopped, 2000));

  client_send("trx:0,true;");
  check("plain trx keys through the ops path", wait_for(cond_trx_on, 2000));
  client_send("trx:0,false;");
  check("trx unkey lands", wait_for(cond_trx_off, 2000));

  /* ---- RX audio stream (F6d-2b): subscribe at 12 kHz mono int-block 512,
   * feed the 48 k tap like the demod would, expect Stream blocks back. */
  client_send("audio_samplerate:12000;audio_stream_channels:1;audio_start:0;");
  g_usleep(300 * 1000);
  while (g_main_context_iteration(NULL, FALSE)) {}
  float ramp[480];
  for (int i = 0; i < 480; i++) { ramp[i] = (float)(i % 96) / 96.0f - 0.5f; }
  int got_audio = 0;
  for (int ms = 0; ms < 3000 && !got_audio; ms += 10) {
    tci_server_audio_push(ramp, 480);        /* 10 ms of 48 k mono           */
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_mutex_lock(&c_lock);
    got_audio = c_bin->len >= 64 + 512 * 4;  /* header + one 512-scalar block */
    g_mutex_unlock(&c_lock);
    g_usleep(10 * 1000);
  }
  check("audio_start delivers a binary Stream block", got_audio);
  /* ⛔ ACK echo (2026-07-12): without the confirmation Decodium retries
   * audio_start forever — and every retry used to reset the accumulator,
   * chopping 20-40 ms out of the running stream (broken FT4/FT8 decodes). */
  check("audio_start is echoed back (ACK)", rx_contains("audio_start:0;"));
  if (got_audio) {
    g_mutex_lock(&c_lock);
    const guint32 *h = (const guint32 *)c_bin->data;
    g_mutex_unlock(&c_lock);
    check("stream header: rate 12000",       h[1] == 12000);
    check("stream header: format float32",   h[2] == 3);
    check("stream header: length 512",       h[5] == 512);
    check("stream header: type RX_AUDIO(1)", h[6] == 1);
    check("stream header: 1 channel",        h[7] == 1);
  } else {
    checks += 5; fails += 5;
    printf("  FAIL stream header checks skipped (no audio arrived)\n");
  }
  client_send("audio_stop:0;");
  int got_stop_ack = 0;
  for (int ms = 0; ms < 1000 && !got_stop_ack; ms += 10) {
    while (g_main_context_iteration(NULL, FALSE)) {}
    got_stop_ack = rx_contains("audio_stop:0;");
    g_usleep(10 * 1000);
  }
  check("audio_stop is echoed back (ACK)", got_stop_ack);

  /* Sensors (2b): subscribe at the fastest cadence, expect both streams. */
  client_send("rx_sensors_enable:true,100;tx_sensors_enable:true,100;");
  int got_sens = 0;
  for (int ms = 0; ms < 2000 && !got_sens; ms += 10) {
    while (g_main_context_iteration(NULL, FALSE)) {}
    got_sens = rx_contains("rx_channel_sensors:0,0,-73.5;") &&
               rx_contains("tx_sensors:0,-20.0,47.4,67.5,1.70;");
    g_usleep(10 * 1000);
  }
  check("rx_channel_sensors + tx_sensors flow at the asked cadence", got_sens);

  /* Sensors OFF before the IQ test: their 100 ms text traffic wakes the
   * blocking lws_service loop and would mask a missing IQ wakeup kick —
   * exactly the bug SDC found live (IQ only flushed on dds broadcasts). */
  client_send("rx_sensors_enable:false;tx_sensors_enable:false;");
  g_usleep(300 * 1000);
  while (g_main_context_iteration(NULL, FALSE)) {}

  /* ---- IQ stream (F6d-2d): subscribe at 48 k (stub engine rate 192 k → ÷4
   * WDSP decimation), feed a +12 kHz complex tone. ExpertSDR's IQ
   * orientation is the CONJUGATE of the DDC feed (live-verified with
   * SDC/CW Skimmer), so the tone must appear at −12 kHz on the wire and
   * nothing at +12 kHz. 12 kHz sits on an exact DFT bin of the 2048-frame
   * block (512 cycles). */
  g_mutex_lock(&c_lock);
  g_byte_array_set_size(c_bin, 0);
  g_mutex_unlock(&c_lock);
  client_send("iq_samplerate:48000;iq_start:0;");
  int got_iq_echo = 0;
  for (int ms = 0; ms < 2000 && !got_iq_echo; ms += 10) {
    while (g_main_context_iteration(NULL, FALSE)) {}
    got_iq_echo = rx_contains("iq_samplerate:48000;") && rx_contains("iq_start:0;");
    g_usleep(10 * 1000);
  }
  check("iq_samplerate + iq_start echo back", got_iq_echo);
  {
    static double tone[2 * 1920];
    double ph = 0.0;
    const double dph = 2.0 * G_PI * 12000.0 / 192000.0;
    const size_t need = 64 + 2048 * 2 * sizeof(float);   /* header + 1 block */
    int got_iq = 0;
    for (int ms = 0; ms < 5000 && !got_iq; ms += 10) {
      for (int i = 0; i < 1920; i++) {                   /* 10 ms of 192 k IQ */
        tone[2 * i]     = 0.5 * cos(ph);
        tone[2 * i + 1] = 0.5 * sin(ph);
        ph += dph;
        if (ph > G_PI) { ph -= 2.0 * G_PI; }
      }
      tci_server_iq_push(tone, 1920, 192000);
      while (g_main_context_iteration(NULL, FALSE)) {}
      g_mutex_lock(&c_lock);
      got_iq = c_bin->len >= need;
      g_mutex_unlock(&c_lock);
      g_usleep(10 * 1000);
    }
    check("iq_start delivers a type-0 Stream block", got_iq);
    if (got_iq) {
      g_mutex_lock(&c_lock);
      guint8 *copy = g_memdup2(c_bin->data, need);
      g_mutex_unlock(&c_lock);
      const guint32 *h = (const guint32 *)copy;
      check("IQ header: rate 48000",       h[1] == 48000);
      check("IQ header: format float32",   h[2] == 3);
      check("IQ header: length 4096",      h[5] == 4096);
      check("IQ header: type IQ(0)",       h[6] == 0);
      check("IQ header: 2 channels",       h[7] == 2);
      /* Correlate the payload against e^{∓j2π·12k·t}: the −12 kHz bin must
       * carry ~the full 0.5 amplitude (conjugated wire orientation), the
       * +12 kHz image ~nothing. */
      const float *p = (const float *)(copy + 64);
      double pr = 0, pi = 0, nr = 0, ni = 0;
      for (int i = 0; i < 2048; i++) {
        double a = 2.0 * G_PI * 12000.0 * (double)i / 48000.0;
        double ci = p[2 * i], cq = p[2 * i + 1];
        pr += ci * cos(a) + cq * sin(a);
        pi += cq * cos(a) - ci * sin(a);
        nr += ci * cos(a) - cq * sin(a);
        ni += cq * cos(a) + ci * sin(a);
      }
      double pos = sqrt(pr * pr + pi * pi) / 2048.0;
      double neg = sqrt(nr * nr + ni * ni) / 2048.0;
      check("tone lands at −12 kHz, amp ~0.5 (ExpertSDR = conjugated DDC)",
            neg > 0.4 && neg < 0.6);
      check("+12 kHz image suppressed > 40 dB", pos < neg * 0.01);
      g_free(copy);
    } else {
      checks += 7; fails += 7;
      printf("  FAIL IQ header/content checks skipped (no IQ arrived)\n");
    }
  }
  client_send("iq_stop:0;");
  {
    int got_stop = 0;
    for (int ms = 0; ms < 2000 && !got_stop; ms += 10) {
      while (g_main_context_iteration(NULL, FALSE)) {}
      got_stop = rx_contains("iq_stop:0;");
      g_usleep(10 * 1000);
    }
    check("iq_stop echoes back", got_stop);
  }

  /* Sticky device rate: an accepted iq_samplerate becomes the default new
   * clients inherit — SDC sends it only at its own startup and expects it
   * to survive reconnects (the second live SDC bug). */
  client_send("iq_samplerate:96000;");
  {
    int sticky = 0;
    for (int ms = 0; ms < 2000 && !sticky; ms += 10) {
      while (g_main_context_iteration(NULL, FALSE)) {}
      sticky = rx_contains("iq_samplerate:96000;") && tci_server_get_iq_rate() == 96000;
      g_usleep(10 * 1000);
    }
    check("accepted iq_samplerate sticks as the device default", sticky);
  }

  /* ---- DX spots (F6d-2e): client pushes a spot (SDC skimmer style), the
   * ops table receives it; a panadapter click broadcasts back. */
  client_send("spot:LZ2PP,CW,14010840,4278255615,test;spot_delete:LZ2PP;spot_clear;");
  {
    int got_spot = 0;
    for (int ms = 0; ms < 2000 && !got_spot; ms += 10) {
      while (g_main_context_iteration(NULL, FALSE)) {}
      got_spot = strcmp(S.spot_call, "LZ2PP") == 0 && S.spot_hz == 14010840 &&
                 S.spot_argb == 4278255615u && S.spot_deleted && S.spot_cleared;
      g_usleep(10 * 1000);
    }
    check("spot + spot_delete + spot_clear land in the ops", got_spot);
  }
  tci_server_spot_clicked("LZ2PP", 14010840);
  {
    int got_click = 0;
    for (int ms = 0; ms < 2000 && !got_click; ms += 10) {
      while (g_main_context_iteration(NULL, FALSE)) {}
      got_click = rx_contains("rx_clicked_on_spot:0,0,LZ2PP,14010840;") &&
                  rx_contains("clicked_on_spot:LZ2PP,14010840;");
      g_usleep(10 * 1000);
    }
    check("spot click broadcasts rx_clicked_on_spot (+legacy)", got_click);
  }

  /* ---- TX audio over TCI (F6d-2c): key with source tci, get the chrono
   * clock, answer with a TX_AUDIO block, unkey reverts to mic. Runs LAST so
   * its binary frames don't pollute the RX-audio header checks above. */
  client_send("trx:0,true,tci;");
  check("trx source 'tci' keys with the TCI TX source", wait_for(cond_tci_keyed, 2000));
  g_mutex_lock(&c_lock);
  g_byte_array_set_size(c_bin, 0);
  g_mutex_unlock(&c_lock);
  tci_server_tx_chrono(512);                    /* the TX feed thread's clock */
  check("TX_CHRONO frame reaches the TX owner", wait_for(cond_chrono, 2000));
  {
    uint32_t hdr[16];
    float pay[512];
    uint8_t frame[64 + sizeof(pay)];
    memset(hdr, 0, sizeof(hdr));
    hdr[1] = 48000; hdr[2] = 3; hdr[5] = 512; hdr[6] = 2; hdr[7] = 1;
    for (int i = 0; i < 512; i++) { pay[i] = 0.25f; }
    memcpy(frame, hdr, 64);
    memcpy(frame + 64, pay, sizeof(pay));
    client_send_bin(frame, sizeof(frame));
  }
  check("TX_AUDIO block lands in the exciter ring", wait_for(cond_txa, 3000));
  client_send("trx:0,false;");
  check("unkey reverts the TX source to mic", wait_for(cond_tci_unkeyed, 2000));

  c_run = 0;
  g_thread_join(ct);
  lws_context_destroy(cctx);
  tci_server_stop();

  printf("\n=== %d checks, %d failures ===\n", checks, fails);
  if (fails) {
    g_mutex_lock(&c_lock);
    printf("client received:\n%s\n", c_rx->str);
    g_mutex_unlock(&c_lock);
    printf("FAIL\n");
    return 1;
  }
  printf("PASS — TCI handshake, control round-trips and CW queueing all behave.\n");
  return 0;
}
