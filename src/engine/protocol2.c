/*
 * sdr-for-linux — HPSDR Protocol-2 RX link (headless, GLib-only).
 *
 * Lean RX-only reimplementation of piHPSDR's new_protocol.c @ 974acba
 * (Option B — see docs/P2-RX-SCOPE.md). Line references below (np.c:NNNN) point
 * at that file; the wire-critical byte fills and the 24-bit-BE IQ decode are
 * copied faithfully from it. Absent by construction: TX/DUC, mic, PureSignal,
 * diversity, wideband, RX-audio return, Saturn/XDMA, and all the GTK/global
 * entanglement — we keep our own small state instead.
 *
 * Threading model (simpler than upstream): the outgoing packets are sent only
 * from p2_rx_start() (once, before the timer spawns) and thereafter only from
 * the single keepalive-timer thread, so there is no send-side concurrency and
 * we need no per-packet mutexes. The listener thread decodes IQ inline (one low
 * -rate DDC → cheap) and hands it straight to the callback, so we also skip
 * upstream's per-DDC ring buffers + semaphores.
 */
#include <glib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>

#include "discovered.h"
#include "protocol2.h"
#include "message.h"

/* Ports — from host (we send) / to host (radio sends). See np.h:28-49. */
#define GENERAL_REGISTERS_FROM_HOST_PORT              1024
#define RECEIVER_SPECIFIC_REGISTERS_FROM_HOST_PORT    1025
#define TRANSMITTER_SPECIFIC_REGISTERS_FROM_HOST_PORT 1026
#define HIGH_PRIORITY_FROM_HOST_PORT                  1027
#define COMMAND_RESPONSE_TO_HOST_PORT                 1024
#define HIGH_PRIORITY_TO_HOST_PORT                    1025
#define MIC_LINE_TO_HOST_PORT                         1026
#define RX_IQ_TO_HOST_PORT_0                          1035

#define NET_BUFFER_SIZE 1500
#define GENERAL_LEN       60
#define TX_SPECIFIC_LEN   60
#define RX_SPECIFIC_LEN 1444
#define HIGH_PRIORITY_LEN 1444

/* 4294967296 / 122880000 — DDC NCO phase-word scale (np.c:817, verbatim). */
#define P2_PHASE_SCALE 34.952533333333333333333333333333

/* ---- state (single owner; see threading note above) --------------------- */
static int data_socket = -1;
static struct sockaddr_in base_addr, receiver_addr, transmitter_addr, high_priority_addr;
static socklen_t base_len, receiver_len, transmitter_len, high_priority_len;

static uint32_t general_sequence;
static uint32_t rx_specific_sequence;
static uint32_t tx_specific_sequence;
static uint32_t high_priority_sequence;
static uint32_t ddc_sequence[8];

static volatile int p2running = 0;

static int       cfg_device;
static long long cfg_freq;       /* read by the timer thread, written by p2_set_frequency */
static GMutex    freq_lock;      /* fences cfg_freq across the GUI/timer threads */
static gint      cfg_atten;      /* ADC0 step attenuator dB (0-31), atomic; HP byte 1443 */
static int       cfg_sample_rate;
static int       cfg_ddc;        /* DDC index for this device (0 for G1) */
static p2_iq_cb  cfg_cb;
static void     *cfg_user;

static GThread *listener_tid;
static GThread *timer_tid;

/*
 * DDC index a receiver maps to. HERMES/HERMES2/G1 use DDC0/1; ANGELIA/ORION/
 * ORION2/SATURN use DDC2/3 (np.c:1627-1631, 835-836). The ANAN G1 is HERMES-
 * class → DDC0 → IQ arrives on port 1035.
 */
static int ddc_for_device(int device) {
  if (device == NEW_DEVICE_ANGELIA || device == NEW_DEVICE_ORION ||
      device == NEW_DEVICE_ORION2  || device == NEW_DEVICE_SATURN) {
    return 2;
  }
  return 0;
}

/* ---- outgoing packet builders (no socket; hexdump-testable) -------------- */

/* General packet — np.c:662-716. Phase-word mode + HW timer + Alex-0 enable
 * ([59]=0x01 — G1 has one Alex; ORION2/SATURN would need 0x03). PA[58] stays 0
 * on purpose: one of the three no-TX guarantees (docs/TX-SAFETY.md); piHPSDR
 * sends 1 here when its "PA enable" setting is on, with no effect on RX. */
int p2_build_general(unsigned char *buf) {
  memset(buf, 0, GENERAL_LEN);
  buf[0] = (general_sequence >> 24) & 0xFF;
  buf[1] = (general_sequence >> 16) & 0xFF;
  buf[2] = (general_sequence >>  8) & 0xFF;
  buf[3] = (general_sequence      ) & 0xFF;
  buf[37] = 0x08;  // phase word (not frequency)
  buf[38] = 0x01;  // enable hardware timer
  buf[59] = 0x01;  // enable Alex 0 — the G1's filter board is ALEX (np.c:696);
                   // without this the RX band-pass relays never engage → no signal
  return GENERAL_LEN;
}

/* RX-specific packet — np.c:1609-1711, single-RX subset. n_adc=1; dither/random
 * off; enable one DDC; program its ADC(=0), sample-rate (kHz, BE) and 24 b/s. */
int p2_build_receive_specific(unsigned char *buf, int device, int sample_rate) {
  int ddc = ddc_for_device(device);
  memset(buf, 0, RX_SPECIFIC_LEN);
  buf[0] = (rx_specific_sequence >> 24) & 0xFF;
  buf[1] = (rx_specific_sequence >> 16) & 0xFF;
  buf[2] = (rx_specific_sequence >>  8) & 0xFF;
  buf[3] = (rx_specific_sequence      ) & 0xFF;
  buf[4] = 1;                        // n_adc
  buf[5] = 0;                        // dither  (adc[0..1])
  buf[6] = 0;                        // random  (adc[0..1])
  buf[7] |= (1 << ddc);              // DDC enable bitmap
  buf[17 + ddc * 6] = 0;             // ADC feeding this DDC (ADC0)
  buf[18 + ddc * 6] = ((sample_rate / 1000) >> 8) & 0xFF;  // rate kHz MSB
  buf[19 + ddc * 6] = ((sample_rate / 1000)     ) & 0xFF;  // rate kHz LSB
  buf[22 + ddc * 6] = 24;            // bits per sample
  return RX_SPECIFIC_LEN;
}

/* High-Priority packet — np.c:718-1474, RX subset. Byte[4] is the run bit;
 * bytes[9..12] carry the DDC0 NCO phase (always set — the radio's automatic
 * band filter follows DDC0), and for DDC2-class devices we also write the
 * receiver's own DDC slot. alex0/alex1 (bytes 1432..1435 / 1428..1431) carry the
 * G1's RX BPF + TX LPF + ANT relay bits (see below); TX fields stay 0. */
int p2_build_high_priority(unsigned char *buf, int device, long long freq_hz, int run) {
  int ddc = ddc_for_device(device);
  long long freq = freq_hz;          // calibrated_frequency() with cal=0 is identity
  uint32_t phase = (uint32_t)(((double)freq) * P2_PHASE_SCALE);
  memset(buf, 0, HIGH_PRIORITY_LEN);
  buf[0] = (high_priority_sequence >> 24) & 0xFF;
  buf[1] = (high_priority_sequence >> 16) & 0xFF;
  buf[2] = (high_priority_sequence >>  8) & 0xFF;
  buf[3] = (high_priority_sequence      ) & 0xFF;
  buf[4] = run ? 1 : 0;              // run bit (MOX bit 0x02 never set — RX only)
  buf[ 9] = (phase >> 24) & 0xFF;    // DDC0 phase (band-filter follows this)
  buf[10] = (phase >> 16) & 0xFF;
  buf[11] = (phase >>  8) & 0xFF;
  buf[12] = (phase      ) & 0xFF;
  if (ddc != 0) {                    // ORION-class: also program the real DDC slot
    int off = 9 + ddc * 4;
    buf[off + 0] = buf[ 9];
    buf[off + 1] = buf[10];
    buf[off + 2] = buf[11];
    buf[off + 3] = buf[12];
  }
  /* G1 Alex words — mirror piHPSDR np.c high_priority() for NEW_DEVICE_G1 @ RX:
   *  - RX BPF bit per RX frequency (below);
   *  - ONE TX LPF bit per band (np.c:1244-1259; G1 keys it to the DUC/TX VFO,
   *    for us = the RX frequency — same band, same bit);
   *  - ALEX_TX_ANTENNA_1 (0x01000000): the ANT1 connector relay. Set during RX
   *    too (np.c:1385-1397) — WITHOUT it no antenna is routed to the RX path
   *    and the radio hears only relay leakage (~46 dB down; the "deaf RX" bug).
   *  - alex1 (bytes 1428-1431) mirrors the TX-case word: LPF + ANT1 (we omit
   *    ALEX_TX_RELAY since we never key the PA).
   * TODO: make the antenna (ANT1/2/3) a setting; hardcoded ANT1 for now. */
  uint32_t alex0;
  if      (freq <  1500000LL) { alex0 = 0x00001000; }  // BYPASS_BPF
  else if (freq <  2100000LL) { alex0 = 0x00000040; }  // 160 m
  else if (freq <  5500000LL) { alex0 = 0x00000020; }  // 80/60 m
  else if (freq < 11000000LL) { alex0 = 0x00000010; }  // 40/30 m
  else if (freq < 22000000LL) { alex0 = 0x00000002; }  // 20/15 m
  else if (freq < 35000000LL) { alex0 = 0x00000004; }  // 12/10 m
  else                        { alex0 = 0x00000008; }  // 6 m + preamp
  uint32_t lpf;                        /* TX LPF bank bit (np.c:1244, alex.h) */
  if      (freq > 35600000LL) { lpf = 0x20000000; }    // 6 m bypass LPF
  else if (freq > 24000000LL) { lpf = 0x40000000; }    // 12/10 m
  else if (freq > 16500000LL) { lpf = 0x80000000; }    // 17/15 m
  else if (freq >  8000000LL) { lpf = 0x00100000; }    // 30/20 m
  else if (freq >  5000000LL) { lpf = 0x00200000; }    // 60/40 m
  else if (freq >  2500000LL) { lpf = 0x00400000; }    // 80 m
  else                        { lpf = 0x00800000; }    // 160 m
  alex0 |= lpf | 0x01000000;           /* + ALEX_TX_ANTENNA_1 = ANT1 relay */
  uint32_t alex1 = lpf | 0x01000000;   /* TX-case word: LPF + ANT1, no TX_RELAY */
  buf[1428] = (alex1 >> 24) & 0xFF;
  buf[1429] = (alex1 >> 16) & 0xFF;
  buf[1430] = (alex1 >>  8) & 0xFF;
  buf[1431] = (alex1      ) & 0xFF;
  buf[1432] = (alex0 >> 24) & 0xFF;
  buf[1433] = (alex0 >> 16) & 0xFF;
  buf[1434] = (alex0 >>  8) & 0xFF;
  buf[1435] = (alex0      ) & 0xFF;
  buf[1443] = (unsigned char)g_atomic_int_get(&cfg_atten);  /* ADC0 step attenuator (0-31 dB) */
  return HIGH_PRIORITY_LEN;
}

/* Transmit-specific packet — np.c:1476. We never transmit, but upstream's start
 * handshake sends one, so we send a zeroed (TX-off) copy for parity. */
static int build_transmit_specific(unsigned char *buf) {
  memset(buf, 0, TX_SPECIFIC_LEN);
  buf[0] = (tx_specific_sequence >> 24) & 0xFF;
  buf[1] = (tx_specific_sequence >> 16) & 0xFF;
  buf[2] = (tx_specific_sequence >>  8) & 0xFF;
  buf[3] = (tx_specific_sequence      ) & 0xFF;
  return TX_SPECIFIC_LEN;
}

/* ---- send wrappers (build + sendto + seq++) ----------------------------- */

static void send_packet(const unsigned char *buf, int len,
                        struct sockaddr_in *addr, socklen_t addrlen, const char *what) {
  ssize_t rc = sendto(data_socket, buf, len, 0, (struct sockaddr *)addr, addrlen);
  if (rc < 0) {
    t_perror("p2 sendto");
    t_print("p2: send %s failed\n", what);
    p2running = 0;
  } else if (rc != len) {
    t_print("p2: send %s short: %zd of %d\n", what, rc, len);
  }
}

static void send_general(void) {
  unsigned char buf[GENERAL_LEN];
  int len = p2_build_general(buf);
  send_packet(buf, len, &base_addr, base_len, "general");
  general_sequence++;
}

static void send_receive_specific(void) {
  unsigned char buf[RX_SPECIFIC_LEN];
  int len = p2_build_receive_specific(buf, cfg_device, cfg_sample_rate);
  send_packet(buf, len, &receiver_addr, receiver_len, "rx-specific");
  rx_specific_sequence++;
}

static void send_transmit_specific(void) {
  unsigned char buf[TX_SPECIFIC_LEN];
  int len = build_transmit_specific(buf);
  send_packet(buf, len, &transmitter_addr, transmitter_len, "tx-specific");
  tx_specific_sequence++;
}

static void send_high_priority(int run) {
  unsigned char buf[HIGH_PRIORITY_LEN];
  g_mutex_lock(&freq_lock);
  long long freq = cfg_freq;
  g_mutex_unlock(&freq_lock);
  int len = p2_build_high_priority(buf, cfg_device, freq, run);
  send_packet(buf, len, &high_priority_addr, high_priority_len, "high-priority");
  high_priority_sequence++;
}

/*
 * Re-tune the running DDC. Only stores the new frequency; the keepalive timer
 * pushes it in the next High-Priority packet (<=100 ms), so all wire sends stay
 * on the single timer thread and rapid tuning coalesces into ~10 retunes/s.
 */
void p2_set_frequency(long long freq_hz) {
  g_mutex_lock(&freq_lock);
  cfg_freq = freq_hz;
  g_mutex_unlock(&freq_lock);
}

/* Set the ADC0 step attenuator (0-31 dB); 0 = max sensitivity. Stored atomically;
 * the keepalive timer pushes it in the next High-Priority packet (≤100 ms). */
void p2_set_attenuation(int db) {
  if (db < 0)  { db = 0; }
  if (db > 31) { db = 31; }
  g_atomic_int_set(&cfg_atten, db);
}

/* ---- incoming IQ ---------------------------------------------------------- */

/*
 * Decode one DDC-IQ packet and deliver it. Header is 16 bytes; [14..15] is the
 * sample-count; payload is 6 bytes/sample (24-bit BE signed I then Q). The
 * extraction + 1/2^23 scaling is copied verbatim from process_iq_data
 * (np.c:2446-2459); the hand-off (upstream: rx_add_iq_samples) is our callback.
 */
static void decode_iq(const unsigned char *buffer) {
  int samplesperframe = ((buffer[14] & 0xFF) << 8) + (buffer[15] & 0xFF);
  double iq[2 * (NET_BUFFER_SIZE / 6 + 1)];
  int b = 16;
  if (samplesperframe < 0 || 16 + 6 * samplesperframe > NET_BUFFER_SIZE) {
    t_print("p2: bogus samplesperframe=%d\n", samplesperframe);
    return;
  }
  for (int i = 0; i < samplesperframe; i++) {
    int leftsample, rightsample;
    leftsample   = (int)((signed char) buffer[b++]) << 16;
    leftsample  |= (int)((((unsigned char)buffer[b++]) << 8) & 0xFF00);
    leftsample  |= (int)((unsigned char)buffer[b++] & 0xFF);
    rightsample  = (int)((signed char) buffer[b++]) << 16;
    rightsample |= (int)((((unsigned char)buffer[b++]) << 8) & 0xFF00);
    rightsample |= (int)((unsigned char)buffer[b++] & 0xFF);
    // The "obscure" constant 1.1920928955078125E-7 is 1/(2^23)
    iq[2 * i    ] = (double)leftsample  * 1.1920928955078125E-7;  // I
    iq[2 * i + 1] = (double)rightsample * 1.1920928955078125E-7;  // Q
  }
  if (cfg_cb) { cfg_cb(iq, samplesperframe, cfg_user); }
}

/* Listener thread — np.c:2074-2154. recvfrom loop, dispatch by source port. */
static gpointer listener_thread(gpointer data) {
  (void)data;
  struct sockaddr_in addr;
  socklen_t length = sizeof(addr);
  unsigned char buffer[NET_BUFFER_SIZE];

  int idle_ticks = 0;   /* consecutive 100 ms RCVTIMEO expiries (Thetis-style LOS) */
  while (p2running) {
    int bytesread = recvfrom(data_socket, buffer, NET_BUFFER_SIZE, 0,
                             (struct sockaddr *)&addr, &length);
    if (!p2running) { break; }
    if (bytesread < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {              // RCVTIMEO tick
        if (++idle_ticks == 30) {
          t_print("p2: no packets from the radio for 3 s — link lost?\n");
        }
        continue;
      }
      t_perror("p2 recvfrom");
      p2running = 0;
      break;
    }
    if (idle_ticks >= 30) { t_print("p2: radio traffic resumed\n"); }
    idle_ticks = 0;

    int sourceport = ntohs(addr.sin_port);
    if (sourceport >= RX_IQ_TO_HOST_PORT_0 && sourceport <= RX_IQ_TO_HOST_PORT_0 + 7) {
      int ddc = sourceport - RX_IQ_TO_HOST_PORT_0;
      uint32_t seq = ((uint32_t)(buffer[0] & 0xFF) << 24)
                   + ((uint32_t)(buffer[1] & 0xFF) << 16)
                   + ((uint32_t)(buffer[2] & 0xFF) <<  8)
                   + ((uint32_t)(buffer[3] & 0xFF));
      if (ddc_sequence[ddc] != seq) {
        t_print("p2: DDC(%d) seq error: expected %u got %u\n",
                ddc, ddc_sequence[ddc], seq);
      }
      ddc_sequence[ddc] = seq + 1;
      decode_iq(buffer);
    }
    // Other ports (command-resp 1024, high-priority status 1025, mic 1026) are
    // not needed for the IQ stream — ignore silently for now.
  }
  return NULL;
}

/* Keepalive timer — np.c:2953-3007, same cadence as piHPSDR: HP every 100 ms,
 * RX-spec + TX-spec alternating every 200 ms, General every 800 ms. The
 * periodic (zeroed) TX-specific keeps the radio's TX registers in the known
 * TX-off state even across dropped packets. */
static gpointer timer_thread(gpointer data) {
  (void)data;
  int cycling = 0;
  usleep(100000);
  while (p2running) {
    cycling++;
    switch (cycling) {
    case 1: case 3: case 5: case 7:
      send_transmit_specific();
      send_high_priority(1);
      break;
    case 2: case 4: case 6:
      send_receive_specific();
      send_high_priority(1);
      break;
    case 8:
      send_general();
      send_receive_specific();
      send_high_priority(1);
      cycling = 0;
      break;
    }
    usleep(100000);
  }
  return NULL;
}

/* ---- public API ----------------------------------------------------------- */

int p2_rx_start(const DISCOVERED *dev, long long freq_hz, int sample_rate,
                p2_iq_cb cb, void *user) {
  if (!dev || sample_rate <= 0) { return -1; }

  cfg_device      = dev->device;
  cfg_freq        = freq_hz;
  cfg_sample_rate = sample_rate;
  cfg_cb          = cb;
  cfg_user        = user;
  cfg_ddc         = ddc_for_device(cfg_device);

  data_socket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (data_socket < 0) { t_perror("p2 data socket"); return -2; }

  int optval = 1;
  setsockopt(data_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
  setsockopt(data_socket, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
  optval = 0x80000;
  setsockopt(data_socket, SOL_SOCKET, SO_RCVBUF, &optval, sizeof(optval));
  optval = 0x20000;
  setsockopt(data_socket, SOL_SOCKET, SO_SNDBUF, &optval, sizeof(optval));
#ifdef IPTOS_DSCP_EF
  optval = IPTOS_DSCP_EF;
#else
  optval = 0xB8;
#endif
  setsockopt(data_socket, IPPROTO_IP, IP_TOS, &optval, sizeof(optval));

  if (bind(data_socket, (const struct sockaddr *)&dev->network.interface_address,
           dev->network.interface_length) < 0) {
    t_perror("p2 bind");
    close(data_socket);
    data_socket = -1;
    return -3;
  }

  struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };  // 100 ms RCVTIMEO
  setsockopt(data_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  t_print("p2: socket %d bound to %s:%d\n", data_socket,
          inet_ntoa(dev->network.interface_address.sin_addr),
          ntohs(dev->network.interface_address.sin_port));

  /* Destination addresses: radio IP, one port per packet type. */
  base_addr = receiver_addr = transmitter_addr = high_priority_addr = dev->network.address;
  base_len = receiver_len = transmitter_len = high_priority_len = dev->network.address_length;
  base_addr.sin_port          = htons(GENERAL_REGISTERS_FROM_HOST_PORT);
  receiver_addr.sin_port      = htons(RECEIVER_SPECIFIC_REGISTERS_FROM_HOST_PORT);
  transmitter_addr.sin_port   = htons(TRANSMITTER_SPECIFIC_REGISTERS_FROM_HOST_PORT);
  high_priority_addr.sin_port = htons(HIGH_PRIORITY_FROM_HOST_PORT);

  general_sequence = rx_specific_sequence = tx_specific_sequence = high_priority_sequence = 0;
  memset((void *)ddc_sequence, 0, sizeof(ddc_sequence));

  p2running = 1;
  listener_tid = g_thread_new("p2-listener", listener_thread, NULL);

  /* Start handshake (np.c:1851-1858): General -> RX-spec -> TX-spec -> HP(run=1). */
  send_general();           usleep(100000);
  send_receive_specific();  usleep(50000);
  send_transmit_specific(); usleep(50000);
  send_high_priority(1);    usleep(100000);

  timer_tid = g_thread_new("p2-timer", timer_thread, NULL);

  t_print("p2: started dev=%d ddc=%d @ %lld Hz, %d Hz sample rate\n",
          cfg_device, cfg_ddc, freq_hz, sample_rate);
  return 0;
}

void p2_rx_stop(void) {
  if (data_socket < 0) { return; }
  p2running = 0;
  if (timer_tid)    { g_thread_join(timer_tid);    timer_tid = NULL; }
  if (listener_tid) { g_thread_join(listener_tid); listener_tid = NULL; }

  /* Tell the radio to stop streaming (run=0); threads are gone, no race. */
  send_high_priority(0);

  close(data_socket);
  data_socket = -1;
  t_print("p2: stopped\n");
}
