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
#define PS_NRX            4           /* PS: feedback hard-wired to RX3/RX4 on the
                                         HL2 → 4 receivers (old_protocol.c:1090)   */

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

/* PureSignal (P1-TX-SCOPE §6). s_ps under s_tx_lock (snapshotted per build
 * round together with the TX state); s_nrx/s_spf are LATCHED at p1_rx_start
 * — nrx never changes on a running link (piHPSDR restarts the protocol). */
static p1_ps_state      s_ps;           /* under s_tx_lock                    */
static int              s_ps_on;        /* under s_tx_lock                    */
static int              s_nrx = 1;      /* latched receiver count             */
static p1_ps_iq_cb      s_ps_cb;        /* set before start (single writer)   */
static void            *s_ps_user;

/* telemetry (listener writes, GUI reads) */
static GMutex           s_tel_lock;
static p1_telemetry     s_tel;
static int              s_fwd_acc, s_rev_acc;  /* 16-sample EMA accumulators */
static int              s_fwd_max;             /* PEP max-hold (take-decays)  */

/* ---- live TX state + IQ ring (T2, docs/P1-TX-SCOPE.md) --------------------
 * ⛔ s_tx_on with a mox state is the ONLY thing that puts the MOX bit on the
 * wire. It is set exclusively by p1_set_tx_state (tx_run's gate path); the
 * RX-only build never calls that, so everything below stays inert. */
#define TXRING_SAMPLES  8192          /* power of two, ~170 ms @ 48 k          */
#define TXRING_BYTES    (TXRING_SAMPLES * 8)
#define PKT_SAMPLES     126           /* TX IQ samples per EP2 packet (2×63)   */
static GMutex           s_tx_lock;
static GCond            s_tx_cond;    /* signalled by p1_tx_iq_push            */
static p1_tx_state      s_tx_state;   /* valid while s_tx_on                   */
static int              s_tx_on;
static double           s_tx_scale;   /* drive IQ component (p1_drive_split)   */
static unsigned char    s_txring[TXRING_BYTES];
static unsigned int     s_txr_in, s_txr_out;   /* free-running sample counters */
static int              s_txr_drops;  /* ring-full pushes (diagnostic)         */
static int              s_txr_under;  /* keyed packets sent with zero IQ       */

/* ---- EP2 frame builders --------------------------------------------------- */

/* Write one 512-byte USB frame: sync + C0..C4 + sample payload. `c` points at
 * 5 bytes C0..C4 (the builders own the MOX bit). `payload` is 504 bytes of
 * encoded 63×(4 B audio + 4 B TX IQ), or NULL for the all-zero RX payload. */
static void put_frame(unsigned char *f, const unsigned char *c,
                      const unsigned char *payload) {
  f[0] = 0x7F; f[1] = 0x7F; f[2] = 0x7F;       /* sync (old_protocol.c:70)    */
  memcpy(f + 3, c, 5);
  if (payload) {
    memcpy(f + 8, payload, FRAME_LEN - 8);
  } else {
    /* zero payload: the HL2 has no audio codec and discards audio; the
     * all-zero first sample pair doubles as "extended address 0 = none"
     * (P1-SCOPE §2). Zero TX IQ = no RF either. */
    memset(f + 8, 0, FRAME_LEN - 8);
  }
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
                         long long freq_hz, const p1_tx_state *tx, int nrx) {
  c[0] = 0x00;
  c[1] = (unsigned char)rate_bits;   /* C1 bits1:0 sample rate                */
  c[2] = 0x00;
  if (device == DEVICE_HERMES_LITE || device == DEVICE_HERMES_LITE2) {
    c[2] = (unsigned char)(n2adr_oc_bits(freq_hz) << 1); /* filter-board relays
                                        (N2ADR OCtx == OCrx, radio.c:2443)    */
  }
  c[3] = 0x00;                       /* no atten/dither/random/preamp         */
  c[4] = (unsigned char)(0x04 |      /* duplex ALWAYS (old_protocol.c:2022)   */
         (((nrx - 1) & 0x07) << 3)); /* 0..7 → 1..8 receivers (o_p.c:2036)    */
  if (tx && tx->mox) { c[0] |= 0x01; }  /* ⛔ MOX rides EVERY frame
                                           (old_protocol.c:2769-2789)         */
}

/* Copy the applied TX state for one build round (NULL when off). */
static const p1_tx_state *tx_snapshot(p1_tx_state *buf) {
  const p1_tx_state *tx = NULL;
  g_mutex_lock(&s_tx_lock);
  if (s_tx_on) { *buf = s_tx_state; tx = buf; }
  g_mutex_unlock(&s_tx_lock);
  return tx;
}

/* Ditto for the PS wire state. */
static const p1_ps_state *ps_snapshot(p1_ps_state *buf) {
  const p1_ps_state *ps = NULL;
  g_mutex_lock(&s_tx_lock);
  if (s_ps_on) { *buf = s_ps; ps = buf; }
  g_mutex_unlock(&s_tx_lock);
  return ps;
}

static void cc_general(unsigned char *c) {
  long long f;
  p1_tx_state txb;
  g_mutex_lock(&s_freq_lock); f = s_freq_hz; g_mutex_unlock(&s_freq_lock);
  /* ⛔ tx is non-NULL only after p1_set_tx_state — never in the RX-only build
   * (radio_tx_supported excludes P1 until the T4 live checklist). */
  p1_build_cc_general(c, s_device, s_rate_bits, f, tx_snapshot(&txb), s_nrx);
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
/* HL2 "rxgain while transmitting" (old_protocol.c 0x14 :2288-2308 and 0x1C
 * :2372-2390): with PS the AD9866 LNA is the feedback attenuator —
 * 31 − attenuation on a 0..60 scale. Only ever called with ps->enabled;
 * the PS-off builds never reach it (byte-identical regression). */
static int hl2_ps_rxgain(int lna_gain_db, int transmitting,
                         const p1_ps_state *ps) {
  int g;
  if (transmitting) {
    g = 31 - ps->attenuation;         /* the PS branch always wins over the
                                         PA-enable "rxgain = 0" branch        */
  } else {
    g = lna_gain_db + 12;             /* −12..+48 → 0..60 (RX as today)       */
  }
  if (g < 0)  { g = 0; }
  if (g > 60) { g = 60; }
  return g;
}

int p1_build_cc_round_robin(unsigned char c[5], int device, long long freq_hz,
                            int lna_gain_db, int step, const p1_tx_state *tx,
                            const p1_ps_state *ps, int nrx) {
  int hl = (device == DEVICE_HERMES_LITE || device == DEVICE_HERMES_LITE2);
  int ps_on = (ps && ps->enabled);
  memset(c, 0, 5);

  /* Layout: step 0 = TX NCO, steps 1..nrx = per-RX NCO, then the fixed
   * registers. nrx = 1 reproduces the T4-verified 11-step cycle exactly. */
  int reg = (step <= nrx) ? -1 : step - nrx - 1;

  if (step == 0) {                    /* 0x02: TX (DUC) frequency — kept on   */
    c[0] = 0x02;                      /* the RX frequency like piHPSDR on RX  */
    be32(c + 1, (guint32)freq_hz);    /* (old_protocol.c:2108; HL2 gateware
                                         tracks nothing from it, but the DUC
                                         must sit on the VFO before any TX)   */
  } else if (step <= nrx) {           /* 0x04+2·chan: DDC frequencies         */
    c[0] = (unsigned char)(0x04 + 2 * (step - 1));  /* (old_protocol.c:2120)  */
    be32(c + 1, (guint32)freq_hz);    /* chan ≥ 2 (RX3/RX4 feedback) wants the
                                         DUC frequency (channel_freq :1015-30)
                                         — same value here: dial == DUC       */
  } else {
    switch (reg) {
    case 0:                           /* 0x12: drive + HL2 relay/PA           */
      c[0] = 0x12;                    /* (old_protocol.c:2163)                */
      /* Drive byte = the HL2 16-step hardware TX attenuator (radio.c:2934-96);
       * 0 with no TX state, and forced 0 out of band (old_protocol.c:2159). */
      c[1] = (unsigned char)((tx && tx->in_band) ? (tx->drive_att & 0xF0) : 0);
      if (hl) {
        if (tx && tx->pa_enabled) {
          c[2] = 0x08;                /* ⛔ PA enable (ADDR-9 bit 19,          */
        } else {                      /*    old_protocol.c:2238-2242)         */
          c[2] = 0x04;                /* ⛔ T/R relay locked to RX — the       */
        }                             /*    PA-off/dry-key state (:2243-2248) */
      }
      break;

    case 1:                           /* 0x14: HL2 LNA gain (extended mode)   */
      c[0] = 0x14;                    /* (old_protocol.c:2258)                */
      if (hl) {
        if (ps_on) {
          c[2] = 0x40;                /* PS enable bit (old_protocol.c:2284)  */
          c[4] = (unsigned char)(0x40 |
                 hl2_ps_rxgain(lna_gain_db, tx && tx->mox, ps));
        } else {
          int g = lna_gain_db;        /* PS off: byte-identical to the        */
          if (g < -12) { g = -12; }   /* T4-verified build                    */
          if (g >  48) { g =  48; }
          c[4] = (unsigned char)(0x40 | (g + 12)); /* old_protocol.c:2292-2308 */
        }
      } else {
        c[4] = 0x20;                  /* classic: atten enable bit, 0 dB      */
      }
      break;

    case 2: c[0] = 0x16; break;       /* ADC1 att / CW reversed — zeros       */

    case 3:                           /* 0x1C: RX ADC select + TX att         */
      c[0] = 0x1C;
      if (hl && ps_on) {
        /* "ADC0 attenuator while transmitting": bit7 = enable TX att, bit6 =
         * 6-bit range. Static config (not mox-conditional in piHPSDR,
         * old_protocol.c:2372-2390) — with PS on the TX gain IS 31−att. */
        c[3] = (unsigned char)(0xC0 | hl2_ps_rxgain(lna_gain_db, 1, ps));
      }                               /* PS off: zeros, as verified in T4     */
      break;

    case 4: c[0] = 0x1E; break;       /* internal CW keyer DISABLED (bit0=0)  */
    case 5: c[0] = 0x20; break;       /* CW hang/sidetone — zeros             */

    case 6:                           /* 0x22: PWM min/max "harmless" values  */
      c[0] = 0x22;                    /* (old_protocol.c:2442-2450)           */
      c[1] = 25; c[2] = 0; c[3] = 100; c[4] = 0;
      break;

    case 7:                           /* 0x2E: HL2 PTT hang + TX FIFO latency */
      c[0] = 0x2E;                    /* (old_protocol.c:2536-2549)           */
      if (hl) {
        c[3] = 20;                    /* PTT hang 20 ms (bits 4:0)            */
        c[4] = 40;                    /* TX latency 40 ms (bits 6:0) — the    */
      }                               /* FPGA buffers this much TX IQ         */
      break;

    default: c[0] = 0x24; break;      /* Orion-II Alex2 — zeros, end of cycle */
    }
  }

  if (tx && tx->mox) { c[0] |= 0x01; }  /* ⛔ MOX rides EVERY frame            */
  return (step + 1) % (nrx + 10);
}

static int cc_round_robin(unsigned char *c, int step) {
  long long freq;
  p1_tx_state txb;
  p1_ps_state psb;
  g_mutex_lock(&s_freq_lock); freq = s_freq_hz; g_mutex_unlock(&s_freq_lock);
  /* ⛔ tx handling identical to cc_general(). */
  return p1_build_cc_round_robin(c, s_device, freq,
                                 g_atomic_int_get(&s_gain_db), step,
                                 tx_snapshot(&txb), ps_snapshot(&psb), s_nrx);
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

/* Drive split: classic 0-255 request → HL2 hardware attenuator step + IQ
 * scale. Thresholds and factors verbatim piHPSDR radio.c:2934-2996. */
void p1_drive_split(int level, int *att_step, double *iq_scale) {
  static const struct { int min; int att; double k; } T[] = {
    {241, 240, 0.0039215}, {228, 224, 0.0041539}, {215, 208, 0.0044000},
    {203, 192, 0.0046607}, {192, 176, 0.0049369}, {181, 160, 0.0052295},
    {171, 144, 0.0055393}, {161, 128, 0.0058675}, {152, 112, 0.0062152},
    {144,  96, 0.0065835}, {136,  80, 0.0069736}, {128,  64, 0.0073868},
    {121,  48, 0.0078245}, {114,  32, 0.0082881}, {108,  16, 0.0087793},
    {  0,   0, 0.0092995},
  };

  if (level < 0)   { level = 0; }

  if (level > 255) { level = 255; }

  for (unsigned i = 0; i < G_N_ELEMENTS(T); i++) {
    if (level >= T[i].min) {
      *att_step = T[i].att;
      *iq_scale = (double)level * T[i].k;
      return;
    }
  }
}

/* ⛔ Live TX state — tx_run's gate path only (contract in protocol1.h). */
void p1_set_tx_state(const p1_tx_state *tx, double iq_scale) {
  g_mutex_lock(&s_tx_lock);

  if (tx) {
    int key_edge = !s_tx_on || (tx->mox && !s_tx_state.mox);
    s_tx_state = *tx;
    s_tx_scale = iq_scale;
    s_tx_on    = 1;

    if (key_edge) {              /* fresh over: drop stale ring content for  */
      s_txr_out = s_txr_in;      /* minimum latency (piHPSDR txring_flag)    */
    }
  } else {
    s_tx_on    = 0;
    s_tx_scale = 0.0;
  }

  g_cond_signal(&s_tx_cond);     /* wake the sender for the state edge */
  g_mutex_unlock(&s_tx_lock);
}

void p1_tx_iq_push(const double *iq, int n_pairs) {
  unsigned char enc[256 * 8];

  while (n_pairs > 0) {
    int chunk = n_pairs > 256 ? 256 : n_pairs;
    double scale;
    g_mutex_lock(&s_tx_lock);
    scale = s_tx_on ? s_tx_scale : 0.0;
    g_mutex_unlock(&s_tx_lock);
    p1_tx_iq_encode(iq, chunk, scale, s_device, enc);

    g_mutex_lock(&s_tx_lock);
    unsigned int used = s_txr_in - s_txr_out;

    if (used + (unsigned)chunk > TXRING_SAMPLES) {
      s_txr_drops++;                       /* producer ahead — drop, keep RF   */
    } else {
      for (int i = 0; i < chunk; i++) {
        memcpy(s_txring + ((s_txr_in + i) % TXRING_SAMPLES) * 8, enc + i * 8, 8);
      }

      s_txr_in += (unsigned)chunk;
      g_cond_signal(&s_tx_cond);
    }

    g_mutex_unlock(&s_tx_lock);
    iq      += 2 * chunk;
    n_pairs -= chunk;
  }
}

/* Build + send one 1032-byte EP2 packet (old_protocol.c metis_write :2835).
 * `payload` = 1008 bytes of encoded TX IQ (two frames) or NULL for zeros. */
static void send_ep2(int *rr_step, const unsigned char *payload) {
  unsigned char pkt[EP2_PACKET_LEN];
  unsigned char cc[5];
  pkt[0] = 0xEF; pkt[1] = 0xFE; pkt[2] = 0x01; pkt[3] = 0x02;   /* EP2 */
  be32(pkt + 4, s_tx_seq++);
  cc_general(cc);                          /* frame 1: always C0=0x00         */
  put_frame(pkt + 8, cc, payload);
  *rr_step = cc_round_robin(cc, *rr_step); /* frame 2: round-robin register   */
  put_frame(pkt + 520, cc, payload ? payload + 504 : NULL);

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
  unsigned char payload[PKT_SAMPLES * 8];

  while (g_atomic_int_get(&s_running)) {
    /* Keyed? → production-paced like piHPSDR (old_protocol.c:250-330): one
     * packet per 126 ring samples, clocked by the WDSP/CW producer. A 20 ms
     * cap sends a zero-IQ keepalive so C&C and the HL2 watchdog never starve
     * (the FPGA's 40 ms TX-latency buffer rides over the gap). */
    g_mutex_lock(&s_tx_lock);
    int keyed = s_tx_on && s_tx_state.mox;

    if (keyed) {
      gint64 until = g_get_monotonic_time() + 20000;

      while (g_atomic_int_get(&s_running) && s_tx_on && s_tx_state.mox &&
             s_txr_in - s_txr_out < PKT_SAMPLES) {
        if (!g_cond_wait_until(&s_tx_cond, &s_tx_lock, until)) { break; }
      }

      int have = (s_txr_in - s_txr_out >= PKT_SAMPLES);

      if (have) {
        for (int i = 0; i < PKT_SAMPLES; i++) {
          memcpy(payload + i * 8,
                 s_txring + ((s_txr_out + i) % TXRING_SAMPLES) * 8, 8);
        }

        s_txr_out += PKT_SAMPLES;
      } else {
        s_txr_under++;
      }

      g_mutex_unlock(&s_tx_lock);
      send_ep2(&rr, have ? payload : NULL);
      next = g_get_monotonic_time() + PACKET_US;   /* re-arm the idle timer */
      continue;
    }

    g_mutex_unlock(&s_tx_lock);
    send_ep2(&rr, NULL);
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

/* Sample payload of one EP6 frame (pure — contract in protocol1.h): per
 * sample nrx×(24-bit BE I + Q) then 16-bit mic (skipped); chan 0 → rx1 and,
 * at nrx == 4, chan 2 → rxfb (coupler) / chan 3 → txfb (TX-DAC loopback) —
 * the PS demux of old_protocol.c process_ozy_byte:1449-1471. */
int p1_ep6_parse_samples(const unsigned char *f, int nrx,
                         double *rx1, double *txfb, double *rxfb) {
  if (f[0] != 0x7F || f[1] != 0x7F || f[2] != 0x7F) { return -1; }

  if (nrx < 1) { nrx = 1; }

  int spf = (FRAME_LEN - 8) / (6 * nrx + 2);   /* old_protocol.c:1416 */
  const unsigned char *p = f + 8;

  for (int i = 0; i < spf; i++) {
    for (int ch = 0; ch < nrx; ch++) {
      double *dst = NULL;

      if (ch == 0)                                  { dst = rx1  + 2 * i; }
      else if (nrx == PS_NRX && ch == 2 && rxfb)    { dst = rxfb + 2 * i; }
      else if (nrx == PS_NRX && ch == 3 && txfb)    { dst = txfb + 2 * i; }

      if (dst) {
        gint32 iv = (gint32)(((guint32)p[0] << 24) | ((guint32)p[1] << 16) | ((guint32)p[2] << 8)) >> 8;
        gint32 qv = (gint32)(((guint32)p[3] << 24) | ((guint32)p[4] << 16) | ((guint32)p[5] << 8)) >> 8;
        dst[0] = (double)iv * IQ_SCALE;
        dst[1] = (double)qv * IQ_SCALE;
      }

      p += 6;
    }

    p += 2;                                     /* mic sample (unused)  */
  }

  return spf;
}

/* Parse one 512-byte EP6 frame: sync, 5 status bytes, per-RX IQ+mic groups.
 * Returns pairs written per stream (2 doubles per pair) or -1 on bad sync. */
static int parse_frame(const unsigned char *f, double *rx1,
                       double *txfb, double *rxfb) {
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

      {
        /* TX FIFO health, C3 bits 0xC0 (old_protocol.c:1268-1290): during RX
         * clear the mask; after a key-on ignore "underrun" until the FIFO
         * first fills (the 40 ms latency buffer starts empty). */
        static int fifo_filled;
        int keyed;
        g_mutex_lock(&s_tx_lock);
        keyed = s_tx_on && s_tx_state.mox;
        g_mutex_unlock(&s_tx_lock);

        if (!keyed) {
          fifo_filled = 0;
        } else {
          if ((f[6] & 0xC0) != 0x80) { fifo_filled = 1; }

          if ((f[6] & 0xC0) == 0x80 && fifo_filled) { s_tel.tx_fifo_under++; }

          if ((f[6] & 0xC0) == 0xC0) { s_tel.tx_fifo_over++; }
        }
      }
      break;

    case 1:                               /* exciter-power slot = HL2 temp      */
      s_tel.temp_raw = (f[4] << 8) | f[5];

      {                                   /* C3C4 = fwd power: 16-EMA + PEP max */
        int val = (f[6] << 8) | f[7];     /* (old_protocol.c:1305-1311)         */
        s_fwd_acc = (15 * s_fwd_acc) / 16 + val;
        s_tel.fwd_raw = s_fwd_acc / 16;

        if (val > s_fwd_max) { s_fwd_max = val; }
      }
      break;

    case 2: {                             /* C1C2 = rev power (o_p.c:1314-1317) */
      int val = (f[4] << 8) | f[5];
      s_rev_acc = (15 * s_rev_acc) / 16 + val;
      s_tel.rev_raw = s_rev_acc / 16;
      s_tel.current_raw = (f[6] << 8) | f[7];  /* AIN slot = PA current (:943)  */
      break;
    }

    default:
      break;
    }

    g_mutex_unlock(&s_tel_lock);
  }

  return p1_ep6_parse_samples(f, s_nrx, rx1, txfb, rxfb);
}

static gpointer listener_thread(gpointer data) {
  (void)data;
  unsigned char pkt[2048];
  /* Both frames of a packet; sized for the nrx=1 maximum (63 — larger nrx
   * yields fewer samples/frame, never more). */
  double rx1 [2 * SAMPLES_PER_FRAME * 2];
  double txfb[2 * SAMPLES_PER_FRAME * 2];
  double rxfb[2 * SAMPLES_PER_FRAME * 2];
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
      int r = parse_frame(pkt + fo, rx1 + 2 * pairs,
                          txfb + 2 * pairs, rxfb + 2 * pairs);

      if (r < 0) {
        g_mutex_lock(&s_tel_lock);
        s_tel.sync_errors++;
        g_mutex_unlock(&s_tel_lock);
        continue;
      }

      pairs += r;
    }

    if (pairs > 0 && s_cb) { s_cb(rx1, pairs, s_user); }

    /* PS feedback pair → ps.c (which gates on keyed-non-CW itself, exactly
     * like the P2 path). Only ever non-NULL samples at nrx == 4. */
    if (pairs > 0 && s_nrx == PS_NRX && s_ps_cb) {
      s_ps_cb(txfb, rxfb, pairs, s_ps_user);
    }
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

  /* PureSignal: latch the receiver count for this link's lifetime (piHPSDR
   * tx_ps_onoff restarts the whole protocol to change it — the GUI mirrors
   * that by restarting us on the PS-enable edge). */
  g_mutex_lock(&s_tx_lock);
  s_nrx = s_ps_on ? PS_NRX : 1;
  g_mutex_unlock(&s_tx_lock);

  if (s_nrx > 1 && sample_rate > 192000) {
    /* ⛔ 4×RX EP6 @384k ≈ 83 Mbit/s — saturates the HL2's 100BASE-T
     * (P1-TX-SCOPE §6). The GUI refuses this combination too. */
    t_print("p1: PS needs 4 receivers — capped at 192 kHz (got %d)\n",
            sample_rate);
    return -1;
  }
  g_mutex_lock(&s_tx_lock);                /* fresh link: TX state off, ring   */
  s_tx_on = 0; s_tx_scale = 0.0;           /* empty (⛔ RX-only until keyed    */
  s_txr_in = s_txr_out = 0;                /* through tx_run's gate)           */
  s_txr_drops = s_txr_under = 0;
  g_mutex_unlock(&s_tx_lock);
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
    send_ep2(&rr, NULL);                  /* frames: 0x00 + 0x02 (TX freq)    */
    g_usleep(20000);
    send_ep2(&rr, NULL);                  /* frames: 0x00 + 0x04 (RX1 freq)   */
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

  p1_set_tx_state(NULL, 0.0);              /* ⛔ belt-and-braces: unkey first  */
  g_atomic_int_set(&s_running, 0);
  g_mutex_lock(&s_tx_lock);                /* wake a sender blocked on the    */
  g_cond_signal(&s_tx_cond);               /* TX ring cond                    */
  g_mutex_unlock(&s_tx_lock);

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

void p1_get_tx_meters(int *fwd_raw, int *rev_raw, double *temp_c,
                      int *fifo_under, int *fifo_over) {
  g_mutex_lock(&s_tel_lock);

  if (fwd_raw)    { *fwd_raw    = s_tel.fwd_raw; }

  if (rev_raw)    { *rev_raw    = s_tel.rev_raw; }

  if (temp_c)     { *temp_c     = s_tel.temp_raw > 0
                                  ? 0.0795898 * s_tel.temp_raw - 50.0 : 0.0; }

  if (fifo_under) { *fifo_under = s_tel.tx_fifo_under; }

  if (fifo_over)  { *fifo_over  = s_tel.tx_fifo_over; }

  g_mutex_unlock(&s_tel_lock);
}

int p1_tx_fwd_max_take(void) {
  g_mutex_lock(&s_tel_lock);
  int v = s_fwd_max;
  s_fwd_max = (s_fwd_max * 15) / 16;       /* p2_tx_fwd_max_take ballistics */
  g_mutex_unlock(&s_tel_lock);
  return v;
}

int p1_ptt_get(void) {
  g_mutex_lock(&s_tel_lock);
  int v = s_tel.ptt;
  g_mutex_unlock(&s_tel_lock);
  return v;
}

/* ---- PureSignal wire state (P1-TX-SCOPE §6) -------------------------------- */

void p1_set_ps(const p1_ps_state *ps) {
  g_mutex_lock(&s_tx_lock);

  if (ps) {
    s_ps = *ps;

    if (s_ps.attenuation < 0)  { s_ps.attenuation = 0; }

    if (s_ps.attenuation > 31) { s_ps.attenuation = 31; }

    s_ps_on = 1;
  } else {
    memset(&s_ps, 0, sizeof(s_ps));
    s_ps_on = 0;
  }

  g_mutex_unlock(&s_tx_lock);
}

void p1_set_ps_iq_cb(p1_ps_iq_cb cb, void *user) {
  s_ps_user = user;
  s_ps_cb   = cb;
}

int p1_running_nrx(void) {
  return (s_sock >= 0) ? s_nrx : 1;
}
