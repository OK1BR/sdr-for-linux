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
 * we need no per-packet mutexes. A TX-state change (p2_set_tx_state) does not
 * send either — it only kicks the timer awake so IT applies the change
 * immediately (see kick_cond). The listener thread decodes IQ inline (one low
 * -rate DDC → cheap) and hands it straight to the callback, so we also skip
 * upstream's per-DDC ring buffers + semaphores.
 */
#include <glib.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
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
#define TX_IQ_FROM_HOST_PORT                          1029  /* F2/F5: TX IQ to radio */
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

/* Inbound telemetry from the HP-status packet (port 1025). Written by the
 * listener thread, read by the GUI via p2_get_telemetry(); shared as atomics
 * (same pattern as cfg_atten). Overload bits are latched here and cleared on
 * read. Read-only path — nothing here ever feeds a packet back to the radio. */
static volatile gint tlm_valid    = 0;
static volatile gint tlm_adc0_ovl = 0;   /* latched, read-and-cleared */
static volatile gint tlm_adc1_ovl = 0;
static volatile gint tlm_raw_adc0 = 0;
static volatile gint tlm_raw_adc1 = 0;
/* TX power sensors (F3) — 16-value moving averages of the ALEX coupler words.
 * The exposed values are read by the GUI via atomics; the *_acc accumulators are
 * touched only by the listener thread (single writer, no atomics needed). */
static volatile gint tlm_fwd      = 0;
static volatile gint tlm_rev      = 0;
static volatile gint tlm_exciter  = 0;
static int fwd_acc = 0, rev_acc = 0, ex_acc = 0;
/* PEP tracker (piHPSDR alex_forward_max, radio.h:221 "// PEP"): max of the RAW
 * per-packet forward word, NOT of the 16-EMA. The SSB envelope fluctuates at
 * audio rate, so the EMA reads near the envelope *average* — squared into watts
 * that's ~6 dB under PEP on speech (the "4× menší než wattmetr" bug). During TX
 * HP packets arrive ~1 kHz, so the raw max IS the PEP. Decayed by the consumer,
 * p2_tx_fwd_max_take(). */
static volatile gint tlm_fwd_max  = 0;
/* Hardware PTT (footswitch): state of HP-status byte [4] bit 0 (np.c:2624).
 * Plain state mirror — no latch; the tx_run gate polls it at ~20 Hz. */
static volatile gint tlm_ptt      = 0;
/* PTT input config for TX-specific byte 50 (np.c:1553-1558), set by the GUI. */
static volatile gint cfg_ptt_enable = 0;   /* keep PTT readable during TX      */
static volatile gint cfg_ptt_tip    = 0;   /* PTT on mic-jack tip (vs ring)    */

static int       cfg_device;
static long long cfg_freq;       /* read by the timer thread, written by p2_set_frequency */
static GMutex    freq_lock;      /* fences cfg_freq across the GUI/timer threads */
static gint      cfg_atten;      /* ADC0 step attenuator dB (0-31), atomic; HP byte 1443 */
static p2_tx_state cfg_tx;        /* live TX state (F5); applied only when cfg_tx_on */
static int         cfg_tx_on;     /* 0 = RX-only (the default/off), 1 = apply cfg_tx */
static GMutex      tx_state_lock; /* fences cfg_tx across the GUI/timer threads */
/* PureSignal (PS-1): live PS state, fenced by tx_state_lock like cfg_tx. Off by
 * default → every packet byte-identical to the PS-less build. `ps_txmode` tells
 * the listener that DDC0's stream is the interleaved feedback pair, not RX IQ;
 * it is flipped ONLY by the timer thread when it sends the matching RX-specific
 * config (single writer). A few in-flight packets may hit the old decoder on
 * the flip — harmless (piHPSDR has the same window; WDSP gates use by mox). */
static p2_ps_state cfg_ps;
static int         cfg_ps_on;
static volatile gint ps_txmode;
static p2_ps_iq_cb cfg_ps_cb;
static void       *cfg_ps_cb_user;
/* TX-state kick: p2_set_tx_state() signals this when the state CHANGES, and the
 * keepalive timer wakes early and applies it to the wire immediately (piHPSDR
 * parity: schedule_high_priority() sends synchronously on every MOX/drive/freq
 * change, new_protocol.c:335). Sends still happen ONLY on the timer thread —
 * the kick just shortens its sleep, preserving the single-sender invariant. */
static GMutex      kick_lock;
static GCond       kick_cond;
static int         kick_pending;
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
 * ([59]=0x01 — G1 has one Alex; ORION2/SATURN would need 0x03). Byte [58] is the
 * firmware PA-enable (np.c:679-685): piHPSDR sends 1 when its "PA enable" setting
 * is on AND the TX band's disablePA is clear. The LIVE engine hardcodes
 * pa_enabled=0 here (send_general) — one of the three no-TX guarantees
 * (docs/TX-SAFETY.md); only sdrfl-txprobe passes 1, offline, to verify the byte. */
int p2_build_general(unsigned char *buf, int pa_enabled) {
  memset(buf, 0, GENERAL_LEN);
  buf[0] = (general_sequence >> 24) & 0xFF;
  buf[1] = (general_sequence >> 16) & 0xFF;
  buf[2] = (general_sequence >>  8) & 0xFF;
  buf[3] = (general_sequence      ) & 0xFF;
  buf[37] = 0x08;  // phase word (not frequency)
  buf[38] = 0x01;  // enable hardware timer
  buf[58] = pa_enabled ? 0x01 : 0x00;  // PA enable (np.c:684). LIVE = 0 (no-TX guarantee)
  buf[59] = 0x01;  // enable Alex 0 — the G1's filter board is ALEX (np.c:696);
                   // without this the RX band-pass relays never engage → no signal
  return GENERAL_LEN;
}

/* RX-specific packet — np.c:1609-1711, single-RX subset. n_adc=1; dither/random
 * off; enable one DDC; program its ADC(=0), sample-rate (kHz, BE) and 24 b/s.
 *
 * PureSignal (PS-1): with `ps->enabled && xmit`, the PS feedback pair replaces
 * the normal RX DDC config (np.c:1649-1668) — on the G1 the PS pair *is* the
 * RX DDC pair (Hermes-class layout), which is fine: non-duplex TX doesn't need
 * the RX stream, and piHPSDR ignores duplex on this family during PS TX
 * (action-table case 10110, np.c:441-445). ps==NULL / enabled=0 / !xmit →
 * byte-identical to today's packet. */
int p2_build_receive_specific(unsigned char *buf, int device, int sample_rate,
                              const p2_ps_state *ps, int xmit) {
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

  if (ps && ps->enabled && xmit) {
    /* PS feedback pair (np.c:1649-1668): DDC0 ← ADC0 = analog (coupler) RX
     * feedback; DDC1 ← "ADC number n_adc" = 1 on the G1 = the TX-DAC loopback
     * pseudo-ADC. Both FIXED 192 kHz / 24-bit regardless of the RX rate.
     * [1363] = 0x02 = "DDC1 synchronized into DDC0" → one interleaved stream
     * on DDC0's port; enable bitmap is exactly DDC0 (piHPSDR sends 0x01 in
     * non-duplex PS TX). Overwrites the generic single-DDC block above. */
    buf[7]    = 0x01;                // enable DDC0 only (DDC1 rides the sync)
    buf[17]   = 0;                   // DDC0 ← ADC0 (rx feedback)
    buf[18]   = 0;  buf[19] = 192;   // DDC0 rate = 192 kHz, fixed
    buf[22]   = 24;
    buf[23]   = 1;                   // DDC1 ← pseudo-ADC n_adc = 1 (TX DAC)
    buf[24]   = 0;  buf[25] = 192;   // DDC1 rate = 192 kHz, fixed
    buf[26]   = 24;
    buf[1363] = 0x02;                // sync bitmap: DDC1 → DDC0
  }
  return RX_SPECIFIC_LEN;
}

/* High-Priority packet — np.c:718-1474. Byte[4] carries the run bit (0x01) and,
 * when transmitting, the MOX bit (0x02); bytes[9..12] carry the DDC0 NCO phase
 * (the radio's automatic band filter follows DDC0). alex0/alex1 (bytes 1432..1435
 * / 1428..1431) carry the G1's RX BPF + TX LPF + ANT relay + (TX) T/R relay bits.
 *
 * `tx` is the transmit state (docs/TX-DESIGN.md §F1). The LIVE engine ALWAYS
 * passes tx=NULL → xmit/pa_on are 0, every TX-only byte below is 0, and the
 * packet is byte-identical to the verified RX build (no MOX, no TX_RELAY, drive
 * 0). Only sdrfl-txprobe passes a non-NULL state, offline, to verify the layout.
 * When tx==NULL the DUC / TX-LPF frequency falls back to the RX frequency. */
int p2_build_high_priority(unsigned char *buf, int device, long long rx_freq_hz,
                           int run, const p2_tx_state *tx, const p2_ps_state *ps) {
  int ddc = ddc_for_device(device);
  long long rx_freq = rx_freq_hz;    // calibrated_frequency() with cal=0 is identity

  /* TX gating — all 0 when tx==NULL (the live path). */
  int ps_on   = ps && ps->enabled;                 // PureSignal enabled (PS-1)
  int xmit    = tx && (tx->mox || tx->tune);       // keyed?
  int pa_on   = tx && tx->pa_enabled;              // PA enabled for the TX band?
  long long tx_freq = tx ? tx->tx_freq : rx_freq;  // DUC (TX) freq; RX freq if no split
  int antenna = tx ? tx->antenna : 0;              // 0/1/2 → ANT1/2/3
  if (antenna < 0 || antenna > 2) { antenna = 0; } // paranoia (np.c:1371): never open relay

  uint32_t rx_phase = (uint32_t)(((double)rx_freq) * P2_PHASE_SCALE);
  memset(buf, 0, HIGH_PRIORITY_LEN);
  buf[0] = (high_priority_sequence >> 24) & 0xFF;
  buf[1] = (high_priority_sequence >> 16) & 0xFF;
  buf[2] = (high_priority_sequence >>  8) & 0xFF;
  buf[3] = (high_priority_sequence      ) & 0xFF;
  buf[4] = run ? 1 : 0;              // run bit
  if (run && xmit) { buf[4] |= 0x02; } // MOX — only with the run bit set, so a stopped/
                                     // park packet (run=0) can never carry MOX (safety).
                                     // (np.c:775; internal-keyer CW suppression is F6.)
  buf[ 9] = (rx_phase >> 24) & 0xFF; // DDC0 phase (band-filter follows this)
  buf[10] = (rx_phase >> 16) & 0xFF;
  buf[11] = (rx_phase >>  8) & 0xFF;
  buf[12] = (rx_phase      ) & 0xFF;
  if (ddc != 0) {                    // ORION-class: also program the real DDC slot
    int off = 9 + ddc * 4;
    buf[off + 0] = buf[ 9];
    buf[off + 1] = buf[10];
    buf[off + 2] = buf[11];
    buf[off + 3] = buf[12];
  }

  /* DUC (TX NCO) phase — bytes 329-332 (np.c:866). Written only when keyed, so
   * the RX packet keeps these 0 (piHPSDR writes it always; we defer that to F5
   * to keep the RX build byte-identical). */
  if (xmit) {
    uint32_t duc = (uint32_t)(((double)tx_freq) * P2_PHASE_SCALE);
    buf[329] = (duc >> 24) & 0xFF;
    buf[330] = (duc >> 16) & 0xFF;
    buf[331] = (duc >>  8) & 0xFF;
    buf[332] = (duc      ) & 0xFF;
    /* PureSignal: both feedback DDCs (DDC0 bytes 9-12, DDC1 bytes 13-16) sit
     * exactly on the DUC frequency while PS-transmitting (np.c:871-883) — the
     * feedback then arrives baseband-aligned, no rotation anywhere. Overwrites
     * the DDC0 RX phase written above. */
    if (ps_on) {
      buf[ 9] = buf[13] = (duc >> 24) & 0xFF;
      buf[10] = buf[14] = (duc >> 16) & 0xFF;
      buf[11] = buf[15] = (duc >>  8) & 0xFF;
      buf[12] = buf[16] = (duc      ) & 0xFF;
    }
  }

  /* Exciter drive — byte 345 (np.c:896-900). 0 unless keyed AND in-band (the
   * fast off-band kill). Live: xmit=0 → 0. */
  {
    int drive = (xmit && tx->in_band) ? tx->drive : 0;
    if (drive < 0)   { drive = 0; }
    if (drive > 255) { drive = 255; }
    buf[345] = (unsigned char)drive;
  }
  /* G1 Alex words — np.c high_priority() for NEW_DEVICE_G1:
   *  - RX BPF bit from the RX frequency (below);
   *  - TX LPF bit from the DUC (TX) frequency = tx_freq (= RX freq when not
   *    split), np.c:1244/1263;
   *  - ALEX_TX_ANTENNA_n (0x01/02/04 000000): the antenna connector relay. Set
   *    during RX too (np.c:1385-1397) — WITHOUT it no antenna is routed to the RX
   *    path and the radio hears only relay leakage (~46 dB down; the "deaf RX"
   *    bug, c4b9243);
   *  - ALEX_TX_RELAY (0x08000000, T/R to TX): alex0 only when keyed, alex1 always
   *    — and ONLY when the PA is enabled (np.c:1024-1032). Live (tx=NULL) →
   *    pa_on=0 → never emitted (a no-TX guarantee).
   *
   * run=0 (shutdown) PARKS the RF path instead: both Alex words stay all-zero,
   * which de-energizes the ANT/BPF/LPF relays and leaves the RX input
   * disconnected from the antenna (static protection, requested by Richard).
   * This works because p2app applies the Alex fields even with the run bit
   * clear (Saturn P2_app/InHighPriority.c:170-208), while the firmware's own
   * standby only releases PTT/OC + the T/R relay (openHPSDR proto spec v4.4)
   * — piHPSDR/Thetis leave the antenna latched to the ADC after exit; we
   * deliberately drop it. (The preceding 100 ms keepalives sent non-zero
   * words, so p2app's write-if-changed cache never swallows this zero.) */
  uint32_t alex0 = 0, alex1 = 0;
  if (run) {
    if      (rx_freq <  1500000LL) { alex0 = 0x00001000; }  // BYPASS_BPF
    else if (rx_freq <  2100000LL) { alex0 = 0x00000040; }  // 160 m
    else if (rx_freq <  5500000LL) { alex0 = 0x00000020; }  // 80/60 m
    else if (rx_freq < 11000000LL) { alex0 = 0x00000010; }  // 40/30 m
    else if (rx_freq < 22000000LL) { alex0 = 0x00000002; }  // 20/15 m
    else if (rx_freq < 35000000LL) { alex0 = 0x00000004; }  // 12/10 m
    else                           { alex0 = 0x00000008; }  // 6 m + preamp
    uint32_t lpf;                      /* TX LPF bank bit from tx_freq (np.c:1244) */
    if      (tx_freq > 35600000LL) { lpf = 0x20000000; }    // 6 m bypass LPF
    else if (tx_freq > 24000000LL) { lpf = 0x40000000; }    // 12/10 m
    else if (tx_freq > 16500000LL) { lpf = 0x80000000; }    // 17/15 m
    else if (tx_freq >  8000000LL) { lpf = 0x00100000; }    // 30/20 m
    else if (tx_freq >  5000000LL) { lpf = 0x00200000; }    // 60/40 m
    else if (tx_freq >  2500000LL) { lpf = 0x00400000; }    // 80 m
    else                           { lpf = 0x00800000; }    // 160 m
    uint32_t ant = 0x01000000u << antenna;   /* ALEX_TX_ANTENNA_1/2/3 */
    alex0 |= lpf | ant;
    alex1  = lpf | ant;
    if (pa_on) {                       /* T/R relay to TX — only with PA enabled */
      if (xmit) { alex0 |= 0x08000000u; }    // ALEX_TX_RELAY (alex0 only when keyed)
      alex1 |= 0x08000000u;                  // TX-case word carries it always
    }
    if (ps_on) {                       /* PureSignal (np.c:1034-1038, alex.h:94) */
      alex1 |= 0x00040000u;                  // ALEX_PS_BIT whenever PS is enabled
      if (xmit) {
        alex0 |= 0x00040000u;                // ...and in alex0 while PS-transmitting
        /* Feedback source (np.c:1288-1354, G1 = Orion2/Saturn decode +100):
         * internal coupler (0/100) needs NO routing bits; BYPASS (7/107) routes
         * the RX path around the BPFs. EXT1 does not exist on the G1 family. */
        if (ps->feedback_ant == 7) { alex0 |= 0x00000800u; }  // ALEX_RX_ANTENNA_BYPASS
      }
    }
  }
  buf[1428] = (alex1 >> 24) & 0xFF;
  buf[1429] = (alex1 >> 16) & 0xFF;
  buf[1430] = (alex1 >>  8) & 0xFF;
  buf[1431] = (alex1      ) & 0xFF;
  buf[1432] = (alex0 >> 24) & 0xFF;
  buf[1433] = (alex0 >> 16) & 0xFF;
  buf[1434] = (alex0 >>  8) & 0xFF;
  buf[1435] = (alex0      ) & 0xFF;

  /* Step attenuators — ADC0 (byte 1443) = configured value; ADC1 (byte 1442) 0.
   * On TX with PA both forced to 31 dB to protect the RX ADC (np.c:1442-1445).
   * Live: xmit && pa_on is false → the RX attenuation is preserved unchanged.
   *
   * ⛔ PureSignal exception — the approved TX-safety delta #1 (PS-SCOPE §6,
   * Richard 2026-07-10): during PS-TX the ADC0 (feedback) attenuator runs at
   * the PS value so the coupler feedback hits the ADC in range; ADC1 stays 31.
   * piHPSDR puts its HP-packet PS value into [1442] = the ADC1 slot
   * (np.c:1447-1449), contradicting its authoritative TX-specific [59] = ADC0
   * — a dead-code leftover for pre-2019 firmware. We mirror the TX-specific
   * mapping into [1443] = ADC0 (divergence documented in PS-SCOPE §2). */
  {
    int atten0 = g_atomic_int_get(&cfg_atten);
    int atten1 = 0;
    if (xmit && pa_on) { atten0 = 31; atten1 = 31; }
    if (xmit && ps_on) {
      int a = ps->attenuation;
      if (a < 0)  { a = 0; }
      if (a > 31) { a = 31; }
      atten0 = a;
    }
    buf[1442] = (unsigned char)atten1;
    buf[1443] = (unsigned char)atten0;
  }
  return HIGH_PRIORITY_LEN;
}

/* Transmit-specific packet — np.c:1476-1607. Carries DAC count + the CW keyer
 * and mic/line-in configuration (no MOX, no drive — those are HP-only).
 *
 * ⛔ cw==NULL builds the ALL-ZERO packet the LIVE engine sends today: nDAC=0 and,
 * crucially, byte[5]=0 → the in-radio CW keyer is DISABLED, so the FPGA cannot
 * key CW locally from the key jack even if a paddle is plugged in. The live path
 * (send_transmit_specific) always passes NULL. Only sdrfl-txprobe passes a
 * non-NULL config, offline, to verify the byte layout — never wire a non-NULL
 * config into a live send before F6 (docs/TX-DESIGN.md, docs/TX-SAFETY.md). */
int p2_build_transmit_specific(unsigned char *buf, const p2_tx_cw *cw,
                               const p2_ps_state *ps) {
  memset(buf, 0, TX_SPECIFIC_LEN);
  buf[0] = (tx_specific_sequence >> 24) & 0xFF;
  buf[1] = (tx_specific_sequence >> 16) & 0xFF;
  buf[2] = (tx_specific_sequence >>  8) & 0xFF;
  buf[3] = (tx_specific_sequence      ) & 0xFF;
  if (!cw) { return TX_SPECIFIC_LEN; }   /* live TX-off: all-zero, no keyer enable */

  buf[4] = 1;                            // number of DACs (np.c:1486)

  /* CW keyer config — byte 5 bitfield (np.c:1489-1519). */
  unsigned char cwb = 0;
  if (cw->cw_internal)    { cwb |= 0x02; }   // enable in-radio keyer
  if (cw->cw_reversed)    { cwb |= 0x04; }
  cwb |= cw->cw_mode_b ? 0x28 : 0x08;        // keyer mode B (0x28) vs A (0x08)
  if (cw->cw_sidetone_on) { cwb |= 0x10; }
  if (cw->cw_spacing)     { cwb |= 0x40; }
  if (cw->cw_breakin)     { cwb |= 0x80; }
  buf[5]  = cwb;
  buf[6]  = (unsigned char)(cw->cw_sidetone_vol & 0x7F);      // sidetone volume
  buf[7]  = (cw->cw_sidetone_freq >> 8) & 0xFF;               // sidetone freq (BE)
  buf[8]  = (cw->cw_sidetone_freq     ) & 0xFF;
  buf[9]  = (unsigned char)(cw->cw_speed  & 0xFF);            // keyer speed (WPM)
  buf[10] = (unsigned char)(cw->cw_weight & 0xFF);            // keyer weight
  buf[11] = (cw->cw_hang_ms >> 8) & 0xFF;                     // hang time (BE, ms)
  buf[12] = (cw->cw_hang_ms     ) & 0xFF;
  buf[13] = (unsigned char)(cw->cw_ptt_delay  & 0xFF);        // RF/PTT delay
  buf[17] = (unsigned char)(cw->cw_ramp_width & 0xFF);        // CW ramp width

  /* Mic / line-in / PTT config — byte 50 bitfield + byte 51 gain (np.c:1543-1572). */
  unsigned char mic = 0;
  if (cw->mic_linein)       { mic |= 0x01; }
  if (cw->mic_boost)        { mic |= 0x02; }
  if (cw->mic_ptt_disabled) { mic |= 0x04; }
  if (cw->mic_ptt_tip)      { mic |= 0x08; }
  if (cw->mic_bias)         { mic |= 0x10; }
  buf[50] = mic;
  buf[51] = (unsigned char)((int)((cw->linein_gain_db + 34.0) * 0.6739 + 0.5) & 0xFF);

  /* Step attenuators during TX — 31 dB both when the PA is enabled (np.c:1579),
   * protecting the RX ADC (duplicated from the HP packet for old firmware).
   * ⛔ PureSignal exception (approved delta #1, PS-SCOPE §6): byte [59] = ADC0
   * = the feedback attenuator runs at the PS value (np.c:1584-1586, the
   * authoritative site per the protocol comment there); [58] = ADC1 stays 31. */
  if (cw->pa_enabled) { buf[58] = 31; buf[59] = 31; }
  if (ps && ps->enabled) {
    int a = ps->attenuation;
    if (a < 0)  { a = 0; }
    if (a > 31) { a = 31; }
    buf[59] = (unsigned char)a;
  }
  return TX_SPECIFIC_LEN;
}

/* ---- TX IQ path (F2, docs/TX-DESIGN.md) --------------------------------- */

/* Encode one IQ pair to 6 wire bytes: 24-bit BE signed I then Q, with piHPSDR's
 * full-scale mapping (np.c:2919). The 8388523.114 factor bakes in ~0.99999
 * headroom; the -8388607 recenters the 0..2^24 result to signed 24-bit. */
static void encode_iq_pair(double I, double Q, unsigned char *b) {
  int is = (int)(I * 8388523.114 + 8388607.5) - 8388607;
  int qs = (int)(Q * 8388523.114 + 8388607.5) - 8388607;
  b[0] = (is >> 16) & 0xFF; b[1] = (is >> 8) & 0xFF; b[2] = is & 0xFF;
  b[3] = (qs >> 16) & 0xFF; b[4] = (qs >> 8) & 0xFF; b[5] = qs & 0xFF;
}

int p2_tx_iq_encode(const double *iq, int n_pairs, unsigned char *out) {
  for (int i = 0; i < n_pairs; i++) {
    encode_iq_pair(iq[2 * i], iq[2 * i + 1], out + i * 6);
  }
  return n_pairs * 6;
}

void p2_tx_iq_framer_init(p2_tx_iq_framer *f, p2_tx_iq_emit emit, void *user) {
  memset(f, 0, sizeof(*f));
  f->emit = emit;
  f->user = user;
}

void p2_tx_iq_framer_push(p2_tx_iq_framer *f, const double *iq, int n_pairs) {
  for (int i = 0; i < n_pairs; i++) {
    encode_iq_pair(iq[2 * i], iq[2 * i + 1], f->payload + f->fill * 6);
    f->fill++;
    if (f->fill >= P2_TX_IQ_SAMPLES) {
      unsigned char pkt[4 + P2_TX_IQ_SAMPLES * 6];   /* 1444 */
      pkt[0] = (f->seq >> 24) & 0xFF;
      pkt[1] = (f->seq >> 16) & 0xFF;
      pkt[2] = (f->seq >>  8) & 0xFF;
      pkt[3] = (f->seq      ) & 0xFF;
      memcpy(pkt + 4, f->payload, P2_TX_IQ_SAMPLES * 6);
      if (f->emit) { f->emit(pkt, (int)sizeof(pkt), f->user); }
      f->seq++;
      f->fill = 0;
    }
  }
}

/* Dormant live emitter (F5): send to the radio's TX-IQ port 1029 via the engine
 * data socket. NOT called in F1-F4. */
void p2_tx_iq_socket_emit(const unsigned char *pkt, int len, void *user) {
  (void)user;
  if (data_socket < 0) { return; }
  struct sockaddr_in a = base_addr;      /* radio IP (set by p2_rx_start) */
  a.sin_port = htons(TX_IQ_FROM_HOST_PORT);
  sendto(data_socket, pkt, len, 0, (struct sockaddr *)&a, base_len);
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
  g_mutex_lock(&tx_state_lock);
  int pa = cfg_tx_on ? cfg_tx.pa_enabled : 0;   /* PA-enable only from a live TX state */
  g_mutex_unlock(&tx_state_lock);
  int len = p2_build_general(buf, pa);
  send_packet(buf, len, &base_addr, base_len, "general");
  general_sequence++;
}

static void send_receive_specific(void) {
  unsigned char buf[RX_SPECIFIC_LEN];
  g_mutex_lock(&tx_state_lock);
  p2_ps_state ps = cfg_ps;
  int ps_on = cfg_ps_on;
  int xmit  = cfg_tx_on;             /* a TX state applied == keyed through tx_gate */
  g_mutex_unlock(&tx_state_lock);
  int ps_tx = ps_on && xmit;
  int len = p2_build_receive_specific(buf, cfg_device, cfg_sample_rate,
                                      ps_on ? &ps : NULL, xmit);
  /* Flip the listener's DDC0 interpretation together with the config that
   * causes it (timer thread = the single writer of both). */
  g_atomic_int_set(&ps_txmode, ps_tx);
  send_packet(buf, len, &receiver_addr, receiver_len, "rx-specific");
  rx_specific_sequence++;
}

static void send_transmit_specific(void) {
  unsigned char buf[TX_SPECIFIC_LEN];
  g_mutex_lock(&tx_state_lock);
  int on = cfg_tx_on;
  int pa = cfg_tx.pa_enabled;
  g_mutex_unlock(&tx_state_lock);
  g_mutex_lock(&tx_state_lock);
  p2_ps_state ps = cfg_ps;
  int ps_on = cfg_ps_on;
  g_mutex_unlock(&tx_state_lock);
  int len;
  if (on) {
    /* Minimal TX-specific for keying: nDAC=1 + attenuators (with PA). NO internal
     * CW keyer (the FPGA must never key CW here). Mic PTT stays disabled UNLESS
     * the operator enabled the footswitch — then the input must stay live during
     * TX so the pedal *release* is still reported in HP-status (np.c:1553-1558;
     * piHPSDR keeps the user's PTT-enable across RX/TX the same way). On RX the
     * all-zero packet below leaves the PTT input enabled, which is what makes
     * the pedal readable before keying — unchanged, verified bytes. */
    p2_tx_cw cw;
    memset(&cw, 0, sizeof(cw));
    cw.pa_enabled       = pa;
    cw.mic_ptt_disabled = !g_atomic_int_get(&cfg_ptt_enable);
    cw.mic_ptt_tip      =  g_atomic_int_get(&cfg_ptt_tip);
    len = p2_build_transmit_specific(buf, &cw, ps_on ? &ps : NULL);
  } else {
    len = p2_build_transmit_specific(buf, NULL, NULL);  /* all-zero (RX, no keyer) */
  }
  send_packet(buf, len, &transmitter_addr, transmitter_len, "tx-specific");
  tx_specific_sequence++;
}

static void send_high_priority(int run) {
  unsigned char buf[HIGH_PRIORITY_LEN];
  g_mutex_lock(&freq_lock);
  long long freq = cfg_freq;
  g_mutex_unlock(&freq_lock);
  g_mutex_lock(&tx_state_lock);
  int on = cfg_tx_on;
  p2_tx_state tx = cfg_tx;
  p2_ps_state ps = cfg_ps;
  int ps_on = cfg_ps_on;
  g_mutex_unlock(&tx_state_lock);
  if (on && tx.tx_freq == 0) { tx.tx_freq = freq; }  /* default TX freq = RX freq (no split) */
  int len = p2_build_high_priority(buf, cfg_device, freq, run, on ? &tx : NULL,
                                   ps_on ? &ps : NULL);
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

void p2_set_tx_state(const p2_tx_state *tx) {
  int changed;
  g_mutex_lock(&tx_state_lock);
  if (tx) {
    /* memcmp is safe: both sides originate from memset-zeroed structs
     * (tx_gate_evaluate zeroes out->state; cfg_tx is zeroed below/at start). */
    changed = !cfg_tx_on || memcmp(&cfg_tx, tx, sizeof(cfg_tx)) != 0;
    cfg_tx = *tx;
    cfg_tx_on = 1;
  } else {
    changed = cfg_tx_on != 0;
    memset(&cfg_tx, 0, sizeof(cfg_tx));
    cfg_tx_on = 0;
  }
  g_mutex_unlock(&tx_state_lock);

  /* Key/unkey/drive/QSY/SWR-trip edge → wake the keepalive timer so the new
   * state lands on the wire NOW, not up to 100 ms later. An unchanged refresh
   * (the ~20 Hz keyed re-assert) does not kick — no extra wire traffic. */
  if (changed) {
    g_mutex_lock(&kick_lock);
    kick_pending = 1;
    g_cond_signal(&kick_cond);
    g_mutex_unlock(&kick_lock);
  }
}

void p2_set_ps(const p2_ps_state *ps) {
  int changed;
  g_mutex_lock(&tx_state_lock);
  if (ps) {
    changed = !cfg_ps_on || memcmp(&cfg_ps, ps, sizeof(cfg_ps)) != 0;
    cfg_ps = *ps;
    cfg_ps_on = 1;
  } else {
    changed = cfg_ps_on != 0;
    memset(&cfg_ps, 0, sizeof(cfg_ps));
    cfg_ps_on = 0;
  }
  g_mutex_unlock(&tx_state_lock);
  if (changed) {                     /* PS on/off/att edge → wire it now */
    g_mutex_lock(&kick_lock);
    kick_pending = 1;
    g_cond_signal(&kick_cond);
    g_mutex_unlock(&kick_lock);
  }
}

void p2_set_ps_iq_cb(p2_ps_iq_cb cb, void *user) {
  cfg_ps_cb_user = user;
  cfg_ps_cb = cb;                    /* set user first; cb is the enable flag */
}

void p2_get_telemetry(p2_telemetry *out) {
  if (!out) { return; }
  out->valid    = g_atomic_int_get(&tlm_valid);
  out->raw_adc0 = g_atomic_int_get(&tlm_raw_adc0);
  out->raw_adc1 = g_atomic_int_get(&tlm_raw_adc1);
  out->fwd_raw     = g_atomic_int_get(&tlm_fwd);
  out->rev_raw     = g_atomic_int_get(&tlm_rev);
  out->exciter_raw = g_atomic_int_get(&tlm_exciter);
  /* read-and-clear: g_atomic_int_and returns the value before the AND */
  out->adc0_overload = g_atomic_int_and(&tlm_adc0_ovl, 0);
  out->adc1_overload = g_atomic_int_and(&tlm_adc1_ovl, 0);
}

int p2_ptt_get(void) {
  return g_atomic_int_get(&tlm_ptt);
}

void p2_set_ptt_input(int enabled, int tip) {
  g_atomic_int_set(&cfg_ptt_enable, enabled ? 1 : 0);
  g_atomic_int_set(&cfg_ptt_tip,    tip     ? 1 : 0);
}

/* Read the PEP tracker and decay it — piHPSDR transmitter.c:578-580 verbatim,
 * except the factor: piHPSDR decays ×7/8 per ~10 Hz meter update; our caller
 * (the tx_run gate slot) runs at ~20 Hz, so ×15/16 gives the same ~0.5 s
 * half-life. MUST be called from exactly ONE place (tx_run) — every call
 * decays the peak, so a second reader would double the decay rate. */
int p2_tx_fwd_max_take(void) {
  int m = g_atomic_int_get(&tlm_fwd_max);
  g_atomic_int_set(&tlm_fwd_max, (m * 15) / 16);
  return m;
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

/* De-interleave one PS-synced DDC0 packet (np.c process_ps_iq_data:2525-2589).
 * [14..15] counts TOTAL samples across both DDCs; the pairs come in DDC-number
 * order: first = DDC0 = analog RX feedback, second = DDC1 = TX-DAC loopback.
 * Same 24-bit-BE decode + 1/2^23 scale as decode_iq. Pure → offline-testable. */
int p2_ps_decode(const unsigned char *pkt, int len,
                 double *txfb, double *rxfb, int max_pairs) {
  if (len < 16) { return 0; }
  int total = ((pkt[14] & 0xFF) << 8) + (pkt[15] & 0xFF);
  int pairs = total / 2;
  if (total < 0 || 16 + 6 * total > len) { return 0; }
  if (pairs > max_pairs) { pairs = max_pairs; }
  int b = 16;
  for (int i = 0; i < pairs; i++) {
    double s[4];                       /* I0 Q0 (DDC0=rxfb)  I1 Q1 (DDC1=txfb) */
    for (int k = 0; k < 4; k++) {
      int v;
      v  = (int)((signed char) pkt[b++]) << 16;
      v |= (int)((((unsigned char)pkt[b++]) << 8) & 0xFF00);
      v |= (int)((unsigned char)pkt[b++] & 0xFF);
      s[k] = (double)v * 1.1920928955078125E-7;   /* 1/2^23 */
    }
    rxfb[2 * i] = s[0];  rxfb[2 * i + 1] = s[1];
    txfb[2 * i] = s[2];  txfb[2 * i + 1] = s[3];
  }
  return pairs;
}

/* Listener-side PS hand-off: split the interleaved stream and deliver both
 * feedback buffers (pscc argument order: tx first) to the registered consumer. */
static void decode_ps_iq(const unsigned char *buffer, int len) {
  /* 119 pairs per full 1444-byte packet (238 samples × 6 B + 16 B header) */
  double txfb[2 * (NET_BUFFER_SIZE / 12 + 1)];
  double rxfb[2 * (NET_BUFFER_SIZE / 12 + 1)];
  int pairs = p2_ps_decode(buffer, len, txfb, rxfb, NET_BUFFER_SIZE / 12 + 1);
  p2_ps_iq_cb cb = cfg_ps_cb;
  if (pairs > 0 && cb) { cb(txfb, rxfb, pairs, cfg_ps_cb_user); }

  /* Keep the RX/audio chain ticking: during PS-TX the RX DDC carries feedback,
   * so demod_feed would starve — and with it the TX-monitor ring drain and the
   * audio sink (the monitor is mixed in AFTER the RX-on-TX mute, demod.c:239).
   * Feed zero IQ at the configured RX rate, paced by this feedback stream
   * (each 192 kHz feedback pair ⇔ rate/192k RX samples of real time). The RX
   * is muted while keyed, so the zeros are inaudible by construction. */
  if (pairs > 0 && cfg_cb) {
    static const double zeros[2 * 8 * (NET_BUFFER_SIZE / 12 + 1)];  /* zeroed BSS */
    int ratio = cfg_sample_rate / 192000;
    if (ratio < 1) { ratio = 1; }
    if (ratio > 8) { ratio = 8; }          /* 1536k max ⇒ 8× (buffer bound)     */
    cfg_cb(zeros, pairs * ratio, cfg_user);
  }
}

/* Decode the radio's High-Priority *status* packet (port 1025) — the RX-useful
 * subset of np.c process_high_priority @ 974acba. We take only what makes sense
 * on RX: the ADC-overload flags (byte 5) and the two raw analog words the
 * firmware fills continuously (ADC0 @57-58 = "PA voltage for others" per
 * hpsdrsim, ADC1 @55-56). The fwd/rev/exciter power words (14-15/22-23/6-7)
 * read ~0 outside TX, so we deliberately ignore them until the TX milestone.
 * Nothing here is ever echoed back to the radio. */
static void parse_high_priority_status(const unsigned char *buf, int len) {
  if (len < 60) { return; }               /* need through the analog words + byte 59 */

  /* byte 4 bit 0: hardware PTT input — the footswitch (np.c:2624). Mirrored as
   * plain state; the tx_run gate turns it into a MOX intent when the operator
   * enabled the footswitch setting. Bits 1/2 (CW dot/dash) stay unused — no
   * physical-key support by decision. */
  g_atomic_int_set(&tlm_ptt, buf[4] & 0x01);

  /* byte 5: ADC overload (np.c:2642-2643). Latch — a clip may last one 50 ms
   * status packet, shorter than a slow GUI frame; the reader clears it. */
  if (buf[5] & 0x01) { g_atomic_int_or(&tlm_adc0_ovl, 1); }
  if (buf[5] & 0x02) { g_atomic_int_or(&tlm_adc1_ovl, 1); }

  /* raw analog words (np.c:2661-2664), big-endian 16-bit. Uncalibrated: the
   * raw->volts scale is model-specific and NOT known for the G1 (neither
   * piHPSDR nor Thetis calibrate it) — surface raw, calibrate live later. */
  g_atomic_int_set(&tlm_raw_adc1, ((buf[55] & 0xFF) << 8) | (buf[56] & 0xFF));
  g_atomic_int_set(&tlm_raw_adc0, ((buf[57] & 0xFF) << 8) | (buf[58] & 0xFF));

  /* TX power sensors (np.c:2652-2667): exciter (6/7), forward (14/15), reverse
   * (22/23), each a 16-value moving average `acc = 15*acc/16 + val; v = acc/16`.
   * ~0 on RX. Read-only — tx_meter.c turns these into watts + SWR. */
  int pw;
  pw = ((buf[ 6] & 0xFF) << 8) | (buf[ 7] & 0xFF);  ex_acc  = (15 * ex_acc)  / 16 + pw;
  pw = ((buf[14] & 0xFF) << 8) | (buf[15] & 0xFF);  fwd_acc = (15 * fwd_acc) / 16 + pw;
  /* PEP: raw pre-EMA max (np.c:2657). Benign race with the decay in
   * p2_tx_fwd_max_take() — worst case one peak is overwritten by the decayed
   * value and re-arms on the next syllable; piHPSDR has the same. */
  if (pw > g_atomic_int_get(&tlm_fwd_max)) { g_atomic_int_set(&tlm_fwd_max, pw); }
  pw = ((buf[22] & 0xFF) << 8) | (buf[23] & 0xFF);  rev_acc = (15 * rev_acc) / 16 + pw;
  g_atomic_int_set(&tlm_exciter, ex_acc  / 16);
  g_atomic_int_set(&tlm_fwd,     fwd_acc / 16);
  g_atomic_int_set(&tlm_rev,     rev_acc / 16);

  g_atomic_int_set(&tlm_valid, 1);

  /* SDRFL_DEBUG_LEVELS: ~1 Hz dump of the raw telemetry (status packets arrive
   * ~20/s on RX). Lets a live session read the raw ADC words against a meter. */
  static int dbg = -1;
  if (dbg < 0) { dbg = getenv("SDRFL_DEBUG_LEVELS") != NULL; }
  if (dbg) {
    static int n = 0;
    if ((n++ % 20) == 0) {
      t_print("p2 telemetry: ADC ovl0=%d ovl1=%d  raw_adc0=%d raw_adc1=%d  "
              "fwd=%d rev=%d exc=%d\n",
              (buf[5] & 0x01), (buf[5] & 0x02) >> 1,
              ((buf[57] & 0xFF) << 8) | (buf[58] & 0xFF),
              ((buf[55] & 0xFF) << 8) | (buf[56] & 0xFF),
              g_atomic_int_get(&tlm_fwd), g_atomic_int_get(&tlm_rev),
              g_atomic_int_get(&tlm_exciter));
    }
  }
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
      /* PS-TX window: DDC0 carries the interleaved feedback pair, not RX IQ
       * (flag flipped by the timer thread with the matching RX-specific). */
      if (ddc == 0 && g_atomic_int_get(&ps_txmode)) {
        decode_ps_iq(buffer, bytesread);
      } else {
        decode_iq(buffer);
      }
    } else if (sourceport == HIGH_PRIORITY_TO_HOST_PORT) {
      parse_high_priority_status(buffer, bytesread);
    }
    // Remaining ports (command-resp 1024, mic 1026) are not needed for RX —
    // ignore silently.
  }
  return NULL;
}

/* One keepalive tick: sleep up to 100 ms, but wake early on a TX-state kick and
 * apply the new state to the wire immediately. Send order on a kick: General
 * (PA enable) and TX-specific first, the High-Priority with MOX/relays last, so
 * MOX never lands ahead of a consistent PA/DAC config; on unkey the same order
 * also drops PA-enable right away instead of at the next 800 ms General. Runs
 * on the timer thread only — the single-sender invariant holds. */
static void timer_wait_or_kick(void) {
  gint64 deadline = g_get_monotonic_time() + 100000;
  g_mutex_lock(&kick_lock);
  while (!kick_pending) {
    if (!g_cond_wait_until(&kick_cond, &kick_lock, deadline)) { break; }  /* tick */
  }
  int kicked = kick_pending;
  kick_pending = 0;
  g_mutex_unlock(&kick_lock);

  if (kicked && p2running) {
    send_general();
    send_transmit_specific();
    send_high_priority(1);
  }
}

/* Keepalive timer — np.c:2953-3007, same cadence as piHPSDR: HP every 100 ms,
 * RX-spec + TX-spec alternating every 200 ms, General every 800 ms. The
 * periodic (zeroed) TX-specific keeps the radio's TX registers in the known
 * TX-off state even across dropped packets. TX-state changes additionally
 * short-circuit the sleep via timer_wait_or_kick (immediate key/unkey). */
static gpointer timer_thread(gpointer data) {
  (void)data;
  int cycling = 0;
  timer_wait_or_kick();
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
    /* While a live TX state is set, refresh General (PA-enable) every cycle so it
     * can never lag the HP relay/MOX (case 8 already sent it this cycle). */
    g_mutex_lock(&tx_state_lock);
    int tx_active = cfg_tx_on;
    g_mutex_unlock(&tx_state_lock);
    if (tx_active && cycling != 0) { send_general(); }
    timer_wait_or_kick();
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

  /* clear any stale telemetry from a previous session */
  g_atomic_int_set(&tlm_valid, 0);
  g_atomic_int_set(&tlm_adc0_ovl, 0);
  g_atomic_int_set(&tlm_adc1_ovl, 0);
  g_atomic_int_set(&tlm_raw_adc0, 0);
  g_atomic_int_set(&tlm_raw_adc1, 0);
  g_atomic_int_set(&tlm_fwd, 0);
  g_atomic_int_set(&tlm_rev, 0);
  g_atomic_int_set(&tlm_exciter, 0);
  g_atomic_int_set(&tlm_ptt, 0);         /* footswitch state re-learned from status */
  fwd_acc = rev_acc = ex_acc = 0;
  g_mutex_lock(&tx_state_lock);          /* start RX-only: no latched TX state */
  memset(&cfg_tx, 0, sizeof(cfg_tx));
  cfg_tx_on = 0;
  /* cfg_ps deliberately SURVIVES the (re)start — it is operator config like
   * cfg_atten, not keyed state. Clearing it here silently disarmed PS whenever
   * the GUI applied settings before p2_rx_start (the "PS dead unless you
   * re-toggle something" bug, 2026-07-11). The attenuator exception only
   * reaches the wire inside a keyed TX state anyway. */
  g_atomic_int_set(&ps_txmode, 0);       /* listener routing state — reset     */
  g_mutex_unlock(&tx_state_lock);
  g_mutex_lock(&kick_lock);              /* drop a stale kick from a prior session */
  kick_pending = 0;
  g_mutex_unlock(&kick_lock);
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
  g_mutex_lock(&kick_lock);   /* wake the timer out of its tick wait (no sends: */
  kick_pending = 1;           /* the kick path re-checks p2running)             */
  g_cond_signal(&kick_cond);
  g_mutex_unlock(&kick_lock);
  if (timer_tid)    { g_thread_join(timer_tid);    timer_tid = NULL; }
  if (listener_tid) { g_thread_join(listener_tid); listener_tid = NULL; }

  /* Safety: the shutdown packet must NEVER carry a keyed TX state — force
   * RX-off before parking, whatever the caller left set. */
  g_mutex_lock(&tx_state_lock);
  memset(&cfg_tx, 0, sizeof(cfg_tx));
  cfg_tx_on = 0;
  g_mutex_unlock(&tx_state_lock);

  /* Stop streaming AND park the RF path: run=0 packets carry zeroed Alex
   * words, dropping the ANT/BPF/LPF relays so the RX input sits disconnected
   * from the antenna while no host runs (see p2_build_high_priority). */
  send_high_priority(0);

  /* CLEAN DISCONNECT (mirror piHPSDR new_protocol_menu_stop:1747-1775). Do NOT
   * slam the socket shut right after run=0: give the FPGA ~200 ms to act on it,
   * then drain the RX data still in flight. Otherwise the radio keeps streaming
   * into a closed socket, gets ICMP "port unreachable", and its P2 session hangs
   * — after which it refuses the next discovery ("no radio found") until it is
   * power-cycled. This is what a rapid close/reopen was tripping. */
  usleep(200000);
  {
    unsigned char drain[NET_BUFFER_SIZE];
    struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
    fd_set fds;
    while (1) {
      FD_ZERO(&fds);
      FD_SET(data_socket, &fds);
      if (select(data_socket + 1, &fds, NULL, NULL, &tv) <= 0) { break; }
      if (recvfrom(data_socket, drain, sizeof drain, 0, NULL, NULL) <= 0) { break; }
    }
  }

  close(data_socket);
  data_socket = -1;
  t_print("p2: stopped (clean: run=0, FPGA rest, drained)\n");
}
