/*
 * sdr-for-linux — HPSDR Protocol-1 (METIS) RX link. See protocol1.h.
 *
 * Byte layouts follow piHPSDR old_protocol.c @ 974acba (first-hand audit
 * 2026-07-12, docs/P1-SCOPE.md) with line references at each site. RX-only:
 * one receiver, HL2-focused (the only P1 radio we bring up first). The
 * C&C round-robin, frame layout and start sequence mirror old_protocol.c;
 * TX, diversity, multi-RX and the audio-codec path are simply absent.
 *
 * Threading mirrors protocol2.c: ONE sender thread owns every outgoing
 * packet (fixed 2.625 ms cadence — the packet cadence is also the radio's
 * watchdog keepalive), one listener thread owns the socket reads. Frequency
 * and gain land in atomics that the sender picks up on its next round.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#include "protocol1.h"
#include "message.h"

#define P1_PORT           1024
#define EP2_PACKET_LEN    1032
#define EP6_PACKET_LEN    1032
#define FRAME_LEN         512
#define SAMPLES_PER_FRAME 63          /* 1 RX: (512-8)/(6+2) (old_protocol.c:1416) */
#define PACKET_US         2625        /* 126 audio samples @48 k per EP2 packet    */
#define IQ_SCALE          1.1920928955078125E-7   /* 2^-23 (old_protocol.c:1433)   */

static int              s_sock = -1;
static struct sockaddr_in s_radio;
static int              s_running;      /* g_atomic */
static GThread         *s_sender;
static GThread         *s_listener;
static int              s_device;
static int              s_rate_bits;    /* C&C 0x00-C1 bits1:0 (old_protocol.c:84-87) */
static GMutex           s_freq_lock;
static long long        s_freq_hz;
static volatile int     s_gain_db = 14; /* HL2 LNA, −12..+48 (piHPSDR default cal point) */
static p1_iq_cb         s_cb;
static void            *s_user;
static guint32          s_tx_seq;       /* EP2 sequence */

/* telemetry (listener writes, GUI reads) */
static GMutex           s_tel_lock;
static p1_telemetry     s_tel;

/* ---- EP2 frame builders --------------------------------------------------- */

/* Write one 512-byte USB frame: sync + C0..C4 + zeroed sample payload.
 * `c` points at 5 bytes C0..C4. The MOX bit (C0[0]) is NEVER set here. */
static void put_frame(unsigned char *f, const unsigned char *c) {
  memset(f, 0, FRAME_LEN);
  f[0] = 0x7F; f[1] = 0x7F; f[2] = 0x7F;       /* sync (old_protocol.c:70)    */
  memcpy(f + 3, c, 5);
  /* payload stays zero: 63×(4 B headphone audio + 4 B TX IQ). The HL2 has no
   * audio codec and discards audio; the all-zero first sample pair doubles as
   * "extended address 0 = none" (P1-SCOPE §2). Zero TX IQ = no drive either. */
}

static void be32(unsigned char *p, guint32 v) {
  p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
  p[2] = (v >>  8) & 0xFF; p[3] = v & 0xFF;
}

/* N2ADR companion filter board: the HL2's J16 open-collector outputs drive
 * the board's LPF relays, and piHPSDR selects them PER BAND via the OC bits
 * in the C0=0x00 frame (N2ADR is its default filter board for HL-class
 * radios; values = radio.c radio_n2adr_oc_settings :2443-2471, sent at
 * old_protocol.c:1916 as OCrx << 1). There is NO gateware automatism — the
 * host must set these or the relays never move. Between ham bands we pick
 * the next-higher LPF (a low-pass only needs its cutoff above the RX
 * frequency); inside every ham band the value matches piHPSDR exactly.
 * With no filter board fitted the J16 pins drive nothing — harmless. */
static unsigned char n2adr_oc_bits(long long f) {
  if (f <  2500000) { return  1; }   /* 160m           */
  if (f <  4700000) { return 66; }   /* 80m            */
  if (f <  7500000) { return 68; }   /* 60m + 40m      */
  if (f < 14500000) { return 72; }   /* 30m + 20m      */
  if (f < 21600000) { return 80; }   /* 17m + 15m      */
  return 96;                         /* 12m + 10m      */
}

/* C0=0x00 frame: rate + OC/filter board + duplex (old_protocol.c:1810-2096).
 * Pure builder — tx=NULL is the RX-only regression state (sdrfl-p1txprobe). */
void p1_build_cc_general(unsigned char c[5], int device, int rate_bits,
                         long long freq_hz, const p1_tx_state *tx) {
  c[0] = 0x00;
  c[1] = (unsigned char)rate_bits;   /* C1 bits1:0 sample rate                */
  c[2] = 0x00;
  if (device == DEVICE_HERMES_LITE || device == DEVICE_HERMES_LITE2) {
    c[2] = (unsigned char)(n2adr_oc_bits(freq_hz) << 1); /* filter-board relays
                                        (N2ADR OCtx == OCrx, radio.c:2443)    */
  }
  c[3] = 0x00;                       /* no atten/dither/random/preamp         */
  c[4] = 0x04;                       /* duplex ALWAYS (old_protocol.c:2022) |
                                        (nrx-1)<<3 = 0 for one receiver      */
  if (tx && tx->mox) { c[0] |= 0x01; }  /* ⛔ MOX rides EVERY frame
                                           (old_protocol.c:2769-2789)         */
}

static void cc_general(unsigned char *c) {
  long long f;
  g_mutex_lock(&s_freq_lock); f = s_freq_hz; g_mutex_unlock(&s_freq_lock);
  /* ⛔ tx = NULL hardcoded: the live link is RX-only until the T2-T4 phases
   * of docs/P1-TX-SCOPE.md are cleared with Richard. */
  p1_build_cc_general(c, s_device, s_rate_bits, f, NULL);
}

/* The round-robin C&C registers (old_protocol.c p1_command_loop :2106-2765),
 * reduced to the RX-only/HL2 set from the P1-SCOPE "minimum viable" list.
 *
 * ⛔ The 0x.. values below are the FINAL C0 bytes exactly as piHPSDR sends
 * them — the register address is already in bits [7:1] and bit 0 is MOX
 * (never set here: every constant is even). R1 shipped these shifted once
 * more (`0x02 << 1` …) — RX still worked because the mislabeled "TX" frame
 * happened to land on RX1's NCO, but the TX NCO (filter-board tracking),
 * the LNA gain and the T/R-relay lock never reached the radio. Verified
 * byte-for-byte against old_protocol.c a second time on the fix. */
int p1_build_cc_round_robin(unsigned char c[5], int device, long long freq_hz,
                            int lna_gain_db, int step, const p1_tx_state *tx) {
  int hl = (device == DEVICE_HERMES_LITE || device == DEVICE_HERMES_LITE2);
  memset(c, 0, 5);

  switch (step) {
  case 0:                             /* 0x02: TX (DUC) frequency — kept on   */
    c[0] = 0x02;                      /* the RX frequency like piHPSDR on RX  */
    be32(c + 1, (guint32)freq_hz);    /* (old_protocol.c:2108; HL2 gateware
                                         tracks nothing from it, but the DUC
                                         must sit on the VFO before any TX)   */
    break;

  case 1:                             /* 0x04: RX1 DDC frequency              */
    c[0] = 0x04;                      /* (old_protocol.c:2120)                */
    be32(c + 1, (guint32)freq_hz);
    break;

  case 2:                             /* 0x12: drive + HL2 relay/PA           */
    c[0] = 0x12;                      /* (old_protocol.c:2163)                */
    /* Drive byte = the HL2 16-step hardware TX attenuator (radio.c:2934-96);
     * 0 with no TX state, and forced 0 out of band (old_protocol.c:2159). */
    c[1] = (unsigned char)((tx && tx->in_band) ? (tx->drive_att & 0xF0) : 0);
    if (hl) {
      if (tx && tx->pa_enabled) {
        c[2] = 0x08;                  /* ⛔ PA enable (ADDR-9 bit 19,          */
      } else {                        /*    old_protocol.c:2238-2242)         */
        c[2] = 0x04;                  /* ⛔ T/R relay locked to RX — the       */
      }                               /*    PA-off/dry-key state (:2243-2248) */
    }
    break;

  case 3:                             /* 0x14: HL2 LNA gain (extended mode)   */
    c[0] = 0x14;                      /* (old_protocol.c:2258)                */
    if (hl) {
      int g = lna_gain_db;
      if (g < -12) { g = -12; }
      if (g >  48) { g =  48; }
      c[4] = (unsigned char)(0x40 | (g + 12));   /* old_protocol.c:2292-2308  */
    } else {
      c[4] = 0x20;                    /* classic: atten enable bit, 0 dB      */
    }
    break;

  case 4: c[0] = 0x16; break;         /* ADC1 att / CW reversed — zeros       */
  case 5: c[0] = 0x1C; break;         /* RX1←ADC0, TX att — zeros             */
  case 6: c[0] = 0x1E; break;         /* internal CW keyer DISABLED (bit0=0)  */
  case 7: c[0] = 0x20; break;         /* CW hang/sidetone — zeros             */

  case 8:                             /* 0x22: PWM min/max "harmless" values  */
    c[0] = 0x22;                      /* (old_protocol.c:2442-2450)           */
    c[1] = 25; c[2] = 0; c[3] = 100; c[4] = 0;
    break;

  case 9:                             /* 0x2E: HL2 PTT hang + TX FIFO latency */
    c[0] = 0x2E;                      /* (old_protocol.c:2536-2549)           */
    if (hl) {
      c[3] = 20;                      /* PTT hang 20 ms (bits 4:0)            */
      c[4] = 40;                      /* TX latency 40 ms (bits 6:0) — the    */
    }                                 /* FPGA buffers this much TX IQ         */
    break;

  default: c[0] = 0x24; break;        /* Orion-II Alex2 — zeros, end of cycle */
  }

  if (tx && tx->mox) { c[0] |= 0x01; }  /* ⛔ MOX rides EVERY frame            */
  return (step + 1) % 11;
}

static int cc_round_robin(unsigned char *c, int step) {
  long long freq;
  g_mutex_lock(&s_freq_lock); freq = s_freq_hz; g_mutex_unlock(&s_freq_lock);
  /* ⛔ tx = NULL hardcoded — see cc_general(). */
  return p1_build_cc_round_robin(c, s_device, freq,
                                 g_atomic_int_get(&s_gain_db), step, NULL);
}

/* TX IQ → EP2 payload (pure; contract in protocol1.h — audio slots zero,
 * 16-bit BE with piHPSDR's scaling, HL-class CWX LSB guard). */
int p1_tx_iq_encode(const double *iq, int n_pairs, double scale, int device,
                    unsigned char *out) {
  int hl = (device == DEVICE_HERMES_LITE || device == DEVICE_HERMES_LITE2);
  unsigned char lsb_mask = hl ? 0xFE : 0xFF;   /* ⛔ CWX guard (o_p.c:1752-60) */
  unsigned char *p = out;

  for (int i = 0; i < n_pairs; i++) {
    /* old_protocol.c:1720-1760 — the int arithmetic below is verbatim
     * piHPSDR (implicit ×0.99999 headroom); scale = the IQ half of the
     * two-component HL2 drive (radio.c drive_scale). */
    gint32 is = (gint32)(iq[2 * i]     * scale * 32766.672 + 32767.5) - 32767;
    gint32 qs = (gint32)(iq[2 * i + 1] * scale * 32766.672 + 32767.5) - 32767;
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;   /* ⛔ audio = extended addrs    */
    *p++ = (unsigned char)((is >> 8) & 0xFF);
    *p++ = (unsigned char)( is       & lsb_mask);
    *p++ = (unsigned char)((qs >> 8) & 0xFF);
    *p++ = (unsigned char)( qs       & lsb_mask);
  }

  return n_pairs * 8;
}

/* Build + send one 1032-byte EP2 packet (old_protocol.c metis_write :2835). */
static void send_ep2(int *rr_step) {
  unsigned char pkt[EP2_PACKET_LEN];
  unsigned char cc[5];
  pkt[0] = 0xEF; pkt[1] = 0xFE; pkt[2] = 0x01; pkt[3] = 0x02;   /* EP2 */
  be32(pkt + 4, s_tx_seq++);
  cc_general(cc);                          /* frame 1: always C0=0x00         */
  put_frame(pkt + 8, cc);
  *rr_step = cc_round_robin(cc, *rr_step); /* frame 2: round-robin register   */
  put_frame(pkt + 520, cc);

  if (sendto(s_sock, pkt, sizeof(pkt), 0,
             (struct sockaddr *)&s_radio, sizeof(s_radio)) < 0) {
    if (g_atomic_int_get(&s_running)) { t_perror("p1: sendto(EP2)"); }
  }
}

/* METIS start/stop: EF FE 04 <cmd>, 64 bytes (old_protocol.c:2927-2976). */
static void metis_start_stop(int cmd) {
  unsigned char buf[64];
  memset(buf, 0, sizeof(buf));
  buf[0] = 0xEF; buf[1] = 0xFE; buf[2] = 0x04;
  buf[3] = (unsigned char)cmd;             /* bit0 = EP6 IQ; wideband unused  */

  if (sendto(s_sock, buf, sizeof(buf), 0,
             (struct sockaddr *)&s_radio, sizeof(s_radio)) < 0) {
    t_perror("p1: sendto(start/stop)");
  }
}

/* ---- sender thread: keepalive + C&C round-robin --------------------------- */

static gpointer sender_thread(gpointer data) {
  (void)data;
  int rr = 0;
  gint64 next = g_get_monotonic_time();

  while (g_atomic_int_get(&s_running)) {
    send_ep2(&rr);
    next += PACKET_US;
    gint64 now = g_get_monotonic_time();

    if (next > now) {
      g_usleep((gulong)(next - now));
    } else if (now - next > 250000) {
      next = now;                          /* fell far behind (suspend) — resync */
    }
  }

  return NULL;
}

/* ---- listener thread: EP6 parse ------------------------------------------- */

/* Parse one 512-byte EP6 frame: sync, 5 status bytes, 63 IQ+mic groups.
 * Returns pairs written to iq (2 doubles per pair) or -1 on bad sync. */
static int parse_frame(const unsigned char *f, double *iq) {
  if (f[0] != 0x7F || f[1] != 0x7F || f[2] != 0x7F) { return -1; }

  const unsigned char c0 = f[3];

  if ((c0 & 0x80) == 0) {                 /* HL2 ACK frames: skip status decode */
    int reg = (c0 >> 3) & 0x1F;           /* old_protocol.c:1207               */
    g_mutex_lock(&s_tel_lock);
    s_tel.valid = 1;
    s_tel.ptt  = c0 & 0x01;               /* :1151                              */
    s_tel.dash = (c0 >> 1) & 0x01;        /* :1191                              */
    s_tel.dot  = (c0 >> 2) & 0x01;        /* :1192                              */

    switch (reg) {
    case 0:                               /* C1 bit0 = ADC overflow (:1209)     */
      if (f[4] & 0x01) { s_tel.adc_overload = 1; }
      break;

    case 1:                               /* exciter-power slot = HL2 temp      */
      s_tel.temp_raw = (f[4] << 8) | f[5];
      break;

    case 2:                               /* AIN slot = HL2 PA current (:943)   */
      s_tel.current_raw = (f[6] << 8) | f[7];
      break;

    default:
      break;
    }

    g_mutex_unlock(&s_tel_lock);
  }

  /* 63 groups: 3 B I + 3 B Q (24-bit signed BE) + 2 B mic (skipped) —
   * old_protocol.c:1416-1433. */
  const unsigned char *p = f + 8;

  for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
    gint32 iv = (gint32)(((guint32)p[0] << 24) | ((guint32)p[1] << 16) | ((guint32)p[2] << 8)) >> 8;
    gint32 qv = (gint32)(((guint32)p[3] << 24) | ((guint32)p[4] << 16) | ((guint32)p[5] << 8)) >> 8;
    iq[2 * i]     = (double)iv * IQ_SCALE;
    iq[2 * i + 1] = (double)qv * IQ_SCALE;
    p += 8;
  }

  return SAMPLES_PER_FRAME;
}

static gpointer listener_thread(gpointer data) {
  (void)data;
  unsigned char pkt[2048];
  double iq[2 * SAMPLES_PER_FRAME * 2];   /* both frames of a packet */
  guint32 expect_seq = 0;
  int have_seq = 0;
  struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
  setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  while (g_atomic_int_get(&s_running)) {
    int n = recv(s_sock, pkt, sizeof(pkt), 0);

    if (n < 0) { continue; }              /* timeout — loop for the stop flag  */

    if (n != EP6_PACKET_LEN) { continue; }

    if (pkt[0] != 0xEF || pkt[1] != 0xFE || pkt[2] != 0x01 || pkt[3] != 0x06) {
      continue;                           /* old_protocol.c metis_read :837    */
    }

    guint32 seq = ((guint32)pkt[4] << 24) | (pkt[5] << 16) | (pkt[6] << 8) | pkt[7];

    if (have_seq && seq != expect_seq && seq != 0) {   /* seq 0 = radio restart */
      g_mutex_lock(&s_tel_lock);
      s_tel.seq_errors++;
      g_mutex_unlock(&s_tel_lock);
    }

    expect_seq = seq + 1;
    have_seq = 1;
    int pairs = 0;

    for (int fo = 8; fo <= 520; fo += 512) {
      int r = parse_frame(pkt + fo, iq + 2 * pairs);

      if (r < 0) {
        g_mutex_lock(&s_tel_lock);
        s_tel.sync_errors++;
        g_mutex_unlock(&s_tel_lock);
        continue;
      }

      pairs += r;
    }

    if (pairs > 0 && s_cb) { s_cb(iq, pairs, s_user); }
  }

  return NULL;
}

/* ---- public API ------------------------------------------------------------ */

int p1_rx_start(const DISCOVERED *dev, long long freq_hz, int sample_rate,
                p1_iq_cb cb, void *user) {
  if (!dev || dev->protocol != ORIGINAL_PROTOCOL) { return -1; }

  switch (sample_rate) {                   /* old_protocol.c:84-87 */
  case 48000:  s_rate_bits = 0x00; break;
  case 96000:  s_rate_bits = 0x01; break;
  case 192000: s_rate_bits = 0x02; break;
  case 384000: s_rate_bits = 0x03; break;
  default:
    t_print("p1: unsupported sample rate %d (48/96/192/384 k only)\n", sample_rate);
    return -1;
  }

  s_device  = dev->device;
  s_cb      = cb;
  s_user    = user;
  s_tx_seq  = 0;
  g_mutex_lock(&s_freq_lock); s_freq_hz = freq_hz; g_mutex_unlock(&s_freq_lock);
  memset(&s_tel, 0, sizeof(s_tel));
  memcpy(&s_radio, &dev->network.address, sizeof(s_radio));
  s_radio.sin_family = AF_INET;
  s_radio.sin_port   = htons(P1_PORT);

  s_sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);

  if (s_sock < 0) { t_perror("p1: socket"); return -1; }

  struct sockaddr_in local = {0};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(0);

  if (bind(s_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
    t_perror("p1: bind");
    close(s_sock); s_sock = -1;
    return -1;
  }

  /* Start sequence (old_protocol.c old_protocol_run :2898-2924): prime the
   * C&C registers with a couple of EP2 packets, then the start command, then
   * expect the first EP6 packet. Retry a few times. */
  g_atomic_int_set(&s_running, 1);
  struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };
  setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  int ok = 0;

  for (int try = 0; try < 10 && !ok; try++) {
    int rr = 0;
    send_ep2(&rr);                        /* frames: 0x00 + 0x02 (TX freq)    */
    g_usleep(20000);
    send_ep2(&rr);                        /* frames: 0x00 + 0x04 (RX1 freq)   */
    g_usleep(20000);
    metis_start_stop(1);
    unsigned char probe[2048];
    int n = recv(s_sock, probe, sizeof(probe), 0);

    if (n == EP6_PACKET_LEN && probe[0] == 0xEF && probe[1] == 0xFE &&
        probe[2] == 0x01 && probe[3] == 0x06) {
      ok = 1;
    }
  }

  if (!ok) {
    t_print("p1: radio at %s did not start (no EP6 after 10 tries)\n",
            inet_ntoa(s_radio.sin_addr));
    metis_start_stop(0);
    g_atomic_int_set(&s_running, 0);
    close(s_sock); s_sock = -1;
    return -1;
  }

  t_print("p1: started dev=%d @ %lld Hz, %d Hz sample rate (radio %s)\n",
          s_device, freq_hz, sample_rate, inet_ntoa(s_radio.sin_addr));
  s_listener = g_thread_new("sdrfl-p1-listen", listener_thread, NULL);
  s_sender   = g_thread_new("sdrfl-p1-send",   sender_thread,   NULL);
  return 0;
}

void p1_rx_stop(void) {
  if (s_sock < 0) { return; }

  g_atomic_int_set(&s_running, 0);

  if (s_sender)   { g_thread_join(s_sender);   s_sender = NULL; }

  if (s_listener) { g_thread_join(s_listener); s_listener = NULL; }

  metis_start_stop(0);                     /* stop EP6 (old_protocol.c:2967)  */
  close(s_sock);
  s_sock = -1;
  s_cb = NULL;
  t_print("p1: stopped (clean: EP6 off)\n");
}

void p1_set_frequency(long long freq_hz) {
  g_mutex_lock(&s_freq_lock);
  s_freq_hz = freq_hz;
  g_mutex_unlock(&s_freq_lock);
}

void p1_set_gain(int db) {
  if (db < -12) { db = -12; }

  if (db >  48) { db =  48; }

  g_atomic_int_set(&s_gain_db, db);
}

void p1_get_telemetry(p1_telemetry *out) {
  g_mutex_lock(&s_tel_lock);
  *out = s_tel;
  s_tel.adc_overload = 0;                  /* latched: read + clear */
  g_mutex_unlock(&s_tel_lock);
}
