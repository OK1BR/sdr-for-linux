/*
 * sdrfl-txiq-ring-test — OFFLINE gate for the Protocol-2 TX-IQ ring link gate
 * (SDR-4, docs/TX-DESIGN.md §10).
 *
 * The TX-IQ producer (tx_run's feed thread → framer → p2_tx_iq_socket_emit)
 * runs from tx_run_start, i.e. BEFORE p2_rx_start and continuously on P2
 * (the N3 zero stream). The paced sender (consumer) only exists between
 * p2_rx_start and p2_rx_stop. This gate pins the contract that makes that
 * safe:
 *
 *   1. Before the link: emits are refused WITHOUT touching the ring (counted
 *      as pre-link, never as ring-full drops — those are TX-over statistics).
 *   2. After p2_rx_start: the first packets the "radio" sees are exactly the
 *      ones emitted after the link came up, in order, with their own sequence
 *      numbers — no replay of anything from before the link.
 *   3. After p2_rx_stop: emits are refused again (gate closed).
 *   4. A restart in the same process replays NOTHING from the previous link
 *      (head/tail were reset while no producer could be inside the ring) and
 *      then carries the new packets in order.
 *
 * The "radio" is a UDP socket on 127.0.0.1:1029 (TX_IQ_FROM_HOST_PORT) owned
 * by this test; p2_rx_start is pointed at a hand-built DISCOVERED for
 * 127.0.0.1 — loopback only, NO discovery, NO real radio. The start handshake
 * (General / RX-specific / TX-specific / HP run=1) goes to 127.0.0.1:1024-1027
 * where nothing listens; p2_rx_stop's run=0 + drain likewise. Exit 0 = PASS.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <glib.h>

#include "discovered.h"
#include "protocol2.h"

#define TXIQ_PORT 1029          /* TX_IQ_FROM_HOST_PORT (protocol2.c) */
#define PKT_LEN   1444

static int g_checks, g_fail;

static void chk(const char *what, long got, long want) {
  g_checks++;
  if (got != want) {
    printf("  FAIL  %-52s got=%ld want=%ld\n", what, got, want);
    g_fail++;
  } else {
    printf("  ok    %-52s = %ld\n", what, got);
  }
}

/* One fabricated TX-IQ packet: 4-byte BE sequence + a payload stamped with
 * the sequence so a replayed/torn slot would also be caught by content. */
static void emit_seq(uint32_t seq) {
  unsigned char pkt[PKT_LEN];
  memset(pkt, (int)(seq & 0xFF), sizeof pkt);
  pkt[0] = (seq >> 24) & 0xFF; pkt[1] = (seq >> 16) & 0xFF;
  pkt[2] = (seq >>  8) & 0xFF; pkt[3] =  seq        & 0xFF;
  p2_tx_iq_socket_emit(pkt, PKT_LEN, NULL);
}

/* Receive one packet on the fake radio's port 1029. Returns 1 and fills *seq
 * (and checks the payload stamp) on success, 0 on timeout. */
static int recv_seq(int sock, int timeout_ms, uint32_t *seq) {
  struct timeval tv = { .tv_sec = timeout_ms / 1000,
                        .tv_usec = (timeout_ms % 1000) * 1000 };
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  unsigned char buf[2048];
  ssize_t n = recvfrom(sock, buf, sizeof buf, 0, NULL, NULL);
  if (n < 0) { return 0; }
  if (n != PKT_LEN) { printf("  FAIL  packet length %zd (want %d)\n", n, PKT_LEN); g_fail++; return 0; }
  *seq = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
         ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
  /* payload stamp must match the header — a torn slot would not */
  for (int i = 4; i < PKT_LEN; i++) {
    if (buf[i] != (unsigned char)(*seq & 0xFF)) {
      printf("  FAIL  payload stamp mismatch in seq %u at byte %d\n", *seq, i);
      g_fail++;
      break;
    }
  }
  return 1;
}

/* Expect exactly the sequence run first..last in order and nothing else. */
static void expect_run(int sock, const char *what, uint32_t first, uint32_t last) {
  uint32_t want = first, got = 0;
  int in_order = 1, count = 0;
  while (want <= last && recv_seq(sock, 500, &got)) {
    count++;
    if (got != want) { in_order = 0; printf("  note  %s: got seq %u, wanted %u\n", what, got, want); }
    want++;
  }
  char label[96];
  snprintf(label, sizeof label, "%s: packets received", what);
  chk(label, count, (long)(last - first + 1));
  snprintf(label, sizeof label, "%s: in order, own sequence numbers", what);
  chk(label, in_order, 1);
  /* nothing trailing (a replayed stale slot would show up here) */
  int extra = 0;
  while (recv_seq(sock, 60, &got)) { extra++; printf("  note  %s: unexpected trailing seq %u\n", what, got); }
  snprintf(label, sizeof label, "%s: no unexpected packets after", what);
  chk(label, extra, 0);
}

static void expect_silence(int sock, const char *what, int ms) {
  uint32_t got = 0; int n = 0;
  while (recv_seq(sock, ms, &got)) { n++; printf("  note  %s: unexpected seq %u\n", what, got); }
  chk(what, n, 0);
}

static void on_iq(const double *iq, int n, void *u) { (void)iq; (void)n; (void)u; }

/* Phase 5 producer: the shape of tx_run's feed thread — emits continuously
 * (~1 kHz here) with a running sequence, before/during/after the link. */
static volatile gint g_prod_run, g_prod_emitted;
static gpointer producer_thread(gpointer data) {
  (void)data;
  uint32_t seq = 1000;
  while (g_atomic_int_get(&g_prod_run)) {
    emit_seq(seq++);
    g_atomic_int_inc(&g_prod_emitted);
    g_usleep(1000);
  }
  return NULL;
}

int main(void) {
  printf("sdrfl-txiq-ring-test: TX-IQ ring link gate (loopback, no radio)\n");

  /* The fake radio: receive TX-IQ on 127.0.0.1:1029. */
  int radio = socket(AF_INET, SOCK_DGRAM, 0);
  if (radio < 0) { perror("socket"); return 2; }
  int yes = 1;
  setsockopt(radio, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  struct sockaddr_in ra; memset(&ra, 0, sizeof ra);
  ra.sin_family = AF_INET; ra.sin_port = htons(TXIQ_PORT);
  ra.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(radio, (struct sockaddr *)&ra, sizeof ra) < 0) {
    perror("bind 127.0.0.1:1029 (is a simulator running?)");
    return 2;
  }

  /* A hand-built DISCOVERED pointing the P2 link at loopback. */
  DISCOVERED dev; memset(&dev, 0, sizeof dev);
  dev.protocol = NEW_PROTOCOL;            /* P2 — informational here */
  dev.device   = NEW_DEVICE_G1;           /* the G2E's id: DDC0, 1 ADC */
  snprintf(dev.name, sizeof dev.name, "loopback");
  dev.network.address_length = sizeof(struct sockaddr_in);
  dev.network.address.sin_family = AF_INET;
  dev.network.address.sin_port   = htons(1024);
  dev.network.address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  dev.network.interface_length = sizeof(struct sockaddr_in);
  dev.network.interface_address.sin_family = AF_INET;
  dev.network.interface_address.sin_port   = htons(0);    /* ephemeral */
  dev.network.interface_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  int live, queued, prelink, sent, drops;

  /* ---- 1. before the link: gate closed, ring untouched ------------------ */
  printf("[1] before p2_rx_start\n");
  for (uint32_t s = 0; s < 10; s++) { emit_seq(s); }
  {
    unsigned char bad[100]; memset(bad, 0, sizeof bad);
    p2_tx_iq_socket_emit(bad, sizeof bad, NULL);   /* wrong length: ignored */
  }
  p2_txiq_ring_debug(&live, &queued, &prelink, &sent);
  chk("pre-link: gate closed (live)",            live,    0);
  chk("pre-link: ring untouched (queued)",       queued,  0);
  chk("pre-link: refused emits counted",         prelink, 10);
  chk("pre-link: nothing sent",                  sent,    0);
  p2_txiq_ring_stats_take(&drops);
  chk("pre-link: NOT counted as ring-full drops", drops,  0);
  expect_silence(radio, "pre-link: radio port sees nothing", 50);

  /* ---- 2. link up: only post-link packets, in order -------------------- */
  printf("[2] p2_rx_start (loopback handshake ~0.3 s)\n");
  if (p2_rx_start(&dev, 14000000, 192000, on_iq, NULL) != 0) {
    printf("  FAIL  p2_rx_start failed\n"); return 2;
  }
  p2_txiq_ring_debug(&live, &queued, &prelink, &sent);
  chk("link up: gate open (live)",               live,    1);
  chk("link up: ring empty at start",            queued,  0);
  chk("link up: pre-link counter taken by start", prelink, 0);
  chk("link up: nothing sent before first emit", sent,    0);
  expect_silence(radio, "link up: no replay of pre-link packets", 80);
  for (uint32_t s = 100; s <= 109; s++) { emit_seq(s); }
  expect_run(radio, "link up", 100, 109);
  p2_txiq_ring_debug(&live, &queued, &prelink, &sent);
  chk("link up: ring drained (queued)",          queued,  0);
  chk("link up: sent count",                     sent,    10);
  p2_txiq_ring_stats_take(&drops);
  chk("link up: no ring-full drops",             drops,   0);

  /* ---- 3. link down: gate closed again --------------------------------- */
  printf("[3] p2_rx_stop (run=0 + FPGA rest + drain, ~0.3 s)\n");
  p2_rx_stop();
  for (uint32_t s = 300; s < 307; s++) { emit_seq(s); }
  p2_txiq_ring_debug(&live, &queued, &prelink, &sent);
  chk("stopped: gate closed (live)",             live,    0);
  chk("stopped: ring untouched (queued)",        queued,  0);
  chk("stopped: refused emits counted",          prelink, 7);
  p2_txiq_ring_stats_take(&drops);
  chk("stopped: NOT counted as ring-full drops", drops,   0);
  expect_silence(radio, "stopped: radio port sees nothing", 50);

  /* ---- 4. restart in the same process: no stale replay ----------------- */
  printf("[4] p2_rx_start again (restart)\n");
  if (p2_rx_start(&dev, 14000000, 192000, on_iq, NULL) != 0) {
    printf("  FAIL  p2_rx_start (restart) failed\n"); return 2;
  }
  p2_txiq_ring_debug(&live, &queued, &prelink, &sent);
  chk("restart: gate open (live)",               live,    1);
  chk("restart: ring empty at start",            queued,  0);
  chk("restart: sent counter reset",             sent,    0);
  expect_silence(radio, "restart: no replay from the previous link", 80);
  for (uint32_t s = 200; s <= 204; s++) { emit_seq(s); }
  expect_run(radio, "restart", 200, 204);
  p2_rx_stop();
  p2_txiq_ring_debug(&live, NULL, NULL, NULL);
  chk("final: gate closed (live)",               live,    0);

  /* ---- 5. the real shape: a producer LIVE across start and stop --------- */
  /* The feed thread in the app emits at 800 pkt/s before, during and after
   * p2_rx_start, and keeps going through p2_rx_stop. Drive a producer thread
   * at ~1 kHz across both edges and require that what the radio saw is ONE
   * contiguous ascending run — no pre-link or stale slot, no gap, no
   * duplicate — and exactly as many packets as the sender handed to sendto. */
  printf("[5] producer thread live across p2_rx_start / p2_rx_stop\n");
  g_atomic_int_set(&g_prod_run, 1);
  GThread *prod = g_thread_new("producer", producer_thread, NULL);
  g_usleep(30000);                                     /* pre-link emits */
  if (p2_rx_start(&dev, 14000000, 192000, on_iq, NULL) != 0) {
    printf("  FAIL  p2_rx_start (live producer) failed\n"); return 2;
  }
  {
    uint32_t got = 0, first = 0, prev = 0;
    int count = 0, contiguous = 1;
    /* drain while the link runs (~150 ms), so the loopback socket buffer
     * never limits the test; then close the link under the producer and
     * drain whatever the joined sender had already handed to sendto */
    gint64 until = g_get_monotonic_time() + 150000;
    for (;;) {
      int have = recv_seq(radio, 20, &got);
      if (have) {
        if (count == 0) { first = got; }
        else if (got != prev + 1) {
          contiguous = 0;
          printf("  note  live producer: seq %u after %u\n", got, prev);
        }
        prev = got; count++;
      }
      if (g_get_monotonic_time() >= until) { break; }
    }
    p2_rx_stop();                                      /* gate closes under the producer */
    while (recv_seq(radio, 100, &got)) {
      if (count == 0) { first = got; }
      else if (got != prev + 1) {
        contiguous = 0;
        printf("  note  live producer: seq %u after %u (post-stop drain)\n", got, prev);
      }
      prev = got; count++;
    }
    g_usleep(30000);                                   /* post-link emits (refused) */
    g_atomic_int_set(&g_prod_run, 0);
    g_thread_join(prod);
    p2_txiq_ring_debug(&live, &queued, &prelink, &sent);
    printf("  info  live producer: emitted %d, radio saw %d (seq %u..%u), refused after stop %d\n",
           g_atomic_int_get(&g_prod_emitted), count, first, prev, prelink);
    chk("live producer: something got through",        count > 0, 1);
    chk("live producer: one contiguous ascending run",  contiguous, 1);
    chk("live producer: received == handed to sendto",  count, sent);
    chk("live producer: first seq is post-link (>= 1000)", count > 0 && first >= 1000, 1);
    chk("live producer: gate closed after stop (live)", live, 0);
    chk("live producer: post-link emits refused",       prelink > 0, 1);
    p2_txiq_ring_stats_take(&drops);
    chk("live producer: no ring-full drops",            drops, 0);
    /* Packets the producer slipped in between the sender's last send and the
     * gate closing legitimately stay in the ring (the sender exits on
     * p2running=0 without draining) — `queued` may be a few here. The
     * contract is that they are NEVER replayed: the next start resets
     * head/tail before it opens the gate. Prove it with those real leftovers. */
    printf("  info  live producer: %d leftover slot(s) in the ring after stop\n", queued);
    if (p2_rx_start(&dev, 14000000, 192000, on_iq, NULL) != 0) {
      printf("  FAIL  p2_rx_start (after live producer) failed\n"); return 2;
    }
    p2_txiq_ring_debug(&live, &queued, NULL, &sent);
    chk("after live producer: restart finds the ring empty", queued, 0);
    expect_silence(radio, "after live producer: leftovers NOT replayed", 80);
    chk("after live producer: nothing sent on restart", sent, 0);
    p2_rx_stop();
    p2_txiq_ring_debug(&live, NULL, NULL, NULL);
    chk("after live producer: gate closed (live)", live, 0);
  }

  /* ---- 6. inbound: the DUC seq-error tripwire is G2E-only (SDR-8) -------
   * The fake radio sends a High-Priority *status* packet from 127.0.0.1:1025
   * (the listener dispatches by source port) to the host's data socket,
   * learned from the source address of a DUC packet. Bytes 32-35 carry
   * 0x01020304. On the G2E (NEW_DEVICE_G1) the parser must take it as the
   * counter; on the Saturn (NEW_DEVICE_SATURN) those bytes are p2app FIFO
   * telemetry (OutHighPriority.c, protocol V4.3) and must be ignored —
   * W1IZZ's G2 printed ~5 garbage "DUC sequence errors" lines a second. */
  {
    printf("[6] inbound HP status: seq-error counter parsed on G2E only\n");
    int hp = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(hp, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in ha; memset(&ha, 0, sizeof ha);
    ha.sin_family = AF_INET; ha.sin_port = htons(1025);
    ha.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(hp, (struct sockaddr *)&ha, sizeof ha) < 0) {
      perror("bind 127.0.0.1:1025"); return 2;
    }
    unsigned char status[60]; memset(status, 0, sizeof status);
    status[32] = 0x01; status[33] = 0x02; status[34] = 0x03; status[35] = 0x04;

    for (int pass = 0; pass < 2; pass++) {
      dev.device = pass == 0 ? NEW_DEVICE_G1 : NEW_DEVICE_SATURN;
      const char *who = pass == 0 ? "G2E" : "Saturn";
      if (p2_rx_start(&dev, 14000000, 192000, on_iq, NULL) != 0) {
        printf("  FAIL  p2_rx_start (%s) failed\n", who); return 2;
      }
      /* one DUC packet → the source address tells us the host's port */
      emit_seq(5000 + pass);
      struct sockaddr_in host; socklen_t hl = sizeof host;
      unsigned char buf[1500];
      struct pollfd pf = { radio, POLLIN, 0 };
      int got = poll(&pf, 1, 500) > 0 &&
                recvfrom(radio, buf, sizeof buf, 0, (struct sockaddr *)&host, &hl) == 1444;
      char label[96];
      snprintf(label, sizeof label, "inbound %s: learned the host port from a DUC packet", who);
      chk(label, got, 1);
      int have = -1; unsigned last = 0;
      if (got) {
        sendto(hp, status, sizeof status, 0, (struct sockaddr *)&host, hl);
        g_usleep(150000);                       /* listener thread parses it */
        p2_seqerr_debug(&have, &last);
      }
      snprintf(label, sizeof label, "inbound %s: counter %s", who,
               pass == 0 ? "parsed (have=1)" : "IGNORED (have=0)");
      chk(label, have, pass == 0 ? 1 : 0);
      if (pass == 0) {
        chk("inbound G2E: counter value = bytes 32-35 (0x01020304)", (long)last, 0x01020304L);
      }
      p2_rx_stop();
    }
    close(hp);
    dev.device = NEW_DEVICE_G1;
  }

  close(radio);
  printf("sdrfl-txiq-ring-test: %d checks, %d failed — %s\n",
         g_checks, g_fail, g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
