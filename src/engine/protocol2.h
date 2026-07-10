/*
 * sdr-for-linux — HPSDR Protocol-2 RX link (headless, GLib-only).
 *
 * A lean, RX-only reimplementation of piHPSDR's new_protocol.c @ 974acba
 * (Option B, decided with Richard — see docs/P2-RX-SCOPE.md). It starts one
 * radio over Protocol 2, runs a single DDC (RX1), and delivers the decoded IQ
 * stream to a caller-supplied callback. The wire-critical byte layouts (the
 * three outgoing packets and the 24-bit-BE IQ decode) are copied verbatim from
 * upstream with line references; everything TX / PureSignal / diversity /
 * Saturn is simply absent.
 *
 * The WDSP analyzer feed (piHPSDR's rx_add_iq_samples -> Spectrum0) is the next
 * milestone; here the IQ meets our own callback instead.
 */
#ifndef SDRFL_ENGINE_PROTOCOL2_H
#define SDRFL_ENGINE_PROTOCOL2_H

#include <stdint.h>
#include "discovered.h" /* DISCOVERED, NEW_DEVICE_* */

/*
 * RX IQ callback. `iq` is interleaved I/Q as doubles [I0,Q0,I1,Q1,...] with
 * `n_pairs` sample pairs (I = "left", Q = "right", scaled to ~[-1,1) by 1/2^23,
 * exactly as WDSP is fed upstream). Called from the P2 listener thread — keep
 * it fast (accumulate / hand to a buffer; do not block or do heavy work here).
 */
typedef void (*p2_iq_cb)(const double *iq, int n_pairs, void *user);

/*
 * Start Protocol-2 on `dev` (from discovery), tuning one DDC (RX1) to `freq_hz`
 * at `sample_rate` (Hz; e.g. 192000). Decoded IQ is delivered to `cb`/`user`.
 * Opens the UDP data socket, sends General -> RX-specific -> High-Priority
 * (run=1), and spawns the listener + keepalive-timer threads.
 * Returns 0 on success, negative on error.
 */
int p2_rx_start(const DISCOVERED *dev, long long freq_hz, int sample_rate,
                p2_iq_cb cb, void *user);

/*
 * Stop the radio (High-Priority run=0), join the threads, close the socket.
 * Safe to call once after a successful p2_rx_start().
 */
void p2_rx_stop(void);

/*
 * Re-tune the running DDC to `freq_hz` (thread-safe; call from the GUI thread).
 * Non-blocking: stores the frequency and lets the keepalive timer send it in the
 * next High-Priority packet (<=100 ms). No restart, no send-side concurrency.
 */
void p2_set_frequency(long long freq_hz);

/* Set the ADC0 step attenuator in dB (0-31; 0 = max sensitivity). */
void p2_set_attenuation(int db);

/*
 * Read-only telemetry decoded from the radio's High-Priority *status* packet
 * (port 1025, np.c process_high_priority @ 974acba). RX-relevant fields only —
 * the fwd/rev/exciter power words read ~0 outside TX and are not surfaced here
 * until the TX milestone. Purely inbound: parsing this changes nothing we send.
 */
typedef struct {
  int valid;          /* 1 once at least one status packet has been parsed     */
  int adc0_overload;  /* ADC0 clipped since the last poll (latched, read+clear) */
  int adc1_overload;  /* ADC1 clipped since the last poll (latched, read+clear) */
  int raw_adc0;       /* raw AIN word ADC0 (bytes 57-58) — "PA voltage" per     */
                      /* hpsdrsim; UNCALIBRATED for the G1 (needs a live scale) */
  int raw_adc1;       /* raw AIN word ADC1 (bytes 55-56), uncalibrated          */
  /* TX power sensors (F3, np.c:2652-2667). 16-value moving average of the raw
   * ALEX coupler words; ~0 on RX (no TX). Convert to watts/SWR via tx_meter.c. */
  int fwd_raw;        /* ALEX forward-power word (bytes 14-15), averaged        */
  int rev_raw;        /* ALEX reverse-power word (bytes 22-23), averaged        */
  int exciter_raw;    /* exciter-power word (bytes 6-7), averaged               */
} p2_telemetry;

/*
 * Snapshot the latest telemetry (thread-safe; call from the GUI tick). The two
 * overload flags are latched by the listener and CLEARED by this read, so each
 * reported '1' means "clipped at least once since you last asked" — keep a
 * single consumer. Raw analog words are read non-destructively.
 */
void p2_get_telemetry(p2_telemetry *out);

/*
 * Hardware PTT (footswitch) state as reported by the radio in the HP-status
 * packet, byte [4] bit 0 (np.c:2624). Non-destructive read (a state, not an
 * event) — poll from the tx_run gate slot. 0 until the first status packet.
 * Reading this changes nothing we send; whether the PTT *keys* is decided by
 * tx_gate like any other MOX intent.
 */
int p2_ptt_get(void);

/*
 * Configure the radio's PTT input (TX-specific byte 50, np.c:1553-1558):
 * `enabled` clears the 0x04 "mic PTT disabled" bit while transmitting, so the
 * release of a footswitch is still reported mid-over; `tip` selects the PTT
 * contact on the mic-jack tip (0x08; default ring, piHPSDR default). Stored
 * atomically; the keepalive timer sends it in the next TX-specific packet.
 * With enabled=0 the TX-time packet keeps PTT disabled (today's behavior).
 */
void p2_set_ptt_input(int enabled, int tip);

/*
 * PEP: read the raw per-packet forward-power maximum and decay it ×15/16
 * (piHPSDR alex_forward_max + transmitter.c:580 ballistics, rate-adjusted to
 * the ~20 Hz tx_run slot). Destructive read — single consumer (tx_run) only;
 * the 16-EMA fwd_raw above under-reads voice PEP by ~6 dB, this doesn't.
 */
int p2_tx_fwd_max_take(void);

/*
 * ---- TX byte construction (F1, docs/TX-DESIGN.md) -------------------------
 *
 * ⛔ SAFETY: these structs let the builders CONSTRUCT the TX bytes; they do NOT
 * transmit. The live engine ALWAYS passes NULL / pa_enabled=0 (see the hardcoded
 * off call sites in protocol2.c) → the RX packets stay byte-identical to the
 * verified RX build and no MOX/PA/drive/TX_RELAY can leave the host. Only the
 * offline sdrfl-txprobe gate passes a non-NULL "hot" state, and only to hexdump-
 * verify the layout against piHPSDR — never over a socket. Do NOT wire a non-off
 * state into any live send before the F4/F5 safety milestones (docs/TX-SAFETY.md).
 */

/* High-Priority TX state. tx_freq is the DUC (TX) frequency; drive is the 0-255
 * exciter level (forced 0 unless in_band). antenna 0/1/2 → ALEX_TX_ANTENNA_1/2/3. */
typedef struct {
  int       mox;         /* key the exciter → HP[4] |= 0x02 (non-CW)              */
  int       tune;        /* TUNE active — also keys (drive from the tune source)   */
  int       pa_enabled;  /* PA on for the TX band → gates ALEX_TX_RELAY + atten-31 */
  int       in_band;     /* tx_freq inside a ham band (else drive forced to 0)     */
  int       drive;       /* exciter drive 0-255 → HP[345]                          */
  long long tx_freq;     /* DUC (TX) frequency Hz → HP[329-332] + TX-LPF selection */
  int       antenna;     /* 0/1/2 → ALEX_TX_ANTENNA_1/2/3 (clamped to ANT1 if bad) */
} p2_tx_state;

/*
 * ---- PureSignal (F7 / PS-1, docs/PS-SCOPE.md) ------------------------------
 *
 * PS state for the packet builders. NULL (or enabled=0) → every packet is
 * byte-identical to today's build — that is the PS-off regression guarantee,
 * enforced by sdrfl-txprobe. With enabled=1:
 *  - RX-specific (only while a TX state is applied): DDC0 ← ADC0 = RX feedback,
 *    DDC1 ← pseudo-ADC 1 (TX-DAC loopback), both fixed 192 kHz/24-bit, byte
 *    [1363]=0x02 syncs DDC1 into DDC0's stream (np.c:1649-1668) — one
 *    interleaved stream replaces the normal RX DDC while keyed.
 *  - High-priority: DDC0+DDC1 NCOs forced to the DUC (TX) frequency while
 *    keyed (np.c:871-883; feedback arrives baseband-aligned), ALEX_PS_BIT in
 *    alex1 always / alex0 while keyed (np.c:1034-1038), and — ⛔ the approved
 *    TX-safety delta #1 (PS-SCOPE §6, Richard 2026-07-10) — the ADC0 step
 *    attenuator runs at `attenuation` during PS-TX instead of the forced 31;
 *    ADC1 stays 31. NOTE: piHPSDR writes its HP-packet PS value into [1442]
 *    (the ADC1 slot, np.c:1447-1449), contradicting its authoritative
 *    TX-specific [59]=ADC0 — a dead-code leftover for old firmware. We mirror
 *    the TX-specific mapping into [1443]=ADC0 (documented divergence).
 *  - TX-specific: byte [59] (ADC0) = `attenuation` (np.c:1584-1586).
 */
typedef struct {
  int enabled;      /* PS on (operator setting; also affects RX-time alex1)     */
  int attenuation;  /* ADC0 step attenuator during PS-TX, 0-31 dB               */
  int feedback_ant; /* 0 = internal coupler (no Alex routing bits on the G1
                       family, np.c:1333-1336); 7 = BYPASS (ALEX_RX_ANTENNA_
                       BYPASS). EXT1 does not exist on the G1.                  */
} p2_ps_state;

/*
 * Live PS state (like p2_set_tx_state; NULL = off). Thread-safe; the keepalive
 * timer applies it — RX-specific/HP/TX-specific all pick it up on the next
 * send (kicked immediately on change). ⛔ The attenuation exception only ever
 * reaches the wire while a TX state is also applied, i.e. keyed through
 * tx_gate; with PS off (the default) nothing changes, bit-for-bit.
 */
void p2_set_ps(const p2_ps_state *ps);

/*
 * PS feedback delivery. While the PS-TX DDC config is on the wire, DDC0's
 * port carries interleaved sample pairs: first = DDC0 = RX feedback (coupler),
 * second = DDC1 = TX-DAC loopback (np.c:2554-2571). The listener splits them
 * and calls `cb` from the listener thread with `n_pairs` samples per stream
 * (interleaved complex doubles, 1/2^23 scaled — WDSP pscc's expected form;
 * txfb/rxfb in pscc argument order). Keep the callback fast (accumulate only).
 */
typedef void (*p2_ps_iq_cb)(const double *txfb, const double *rxfb,
                            int n_pairs, void *user);
void p2_set_ps_iq_cb(p2_ps_iq_cb cb, void *user);

/* Pure decoder for one PS-synced DDC packet (offline-testable): de-interleave
 * into txfb/rxfb (each 2*max_pairs doubles), return pair count (≤ max_pairs). */
int p2_ps_decode(const unsigned char *pkt, int len,
                 double *txfb, double *rxfb, int max_pairs);

/* Transmit-specific config (CW keyer + mic/line). NULL → the all-zero TX-off
 * packet the live path sends today (no internal keyer → the FPGA cannot key CW). */
typedef struct {
  int pa_enabled;        /* → step attenuators 58/59 = 31 dB during TX            */
  /* CW keyer — byte 5 bitfield + parameters */
  int cw_internal;       /* 0x02 enable the in-radio (FPGA) keyer                  */
  int cw_reversed;       /* 0x04 dot/dash reversed                                 */
  int cw_mode_b;         /* keyer mode B (0x28) vs mode A (0x08)                   */
  int cw_sidetone_on;    /* 0x10                                                   */
  int cw_spacing;        /* 0x40 strict spacing                                    */
  int cw_breakin;        /* 0x80                                                   */
  int cw_sidetone_vol;   /* byte 6, 0-127                                          */
  int cw_sidetone_freq;  /* bytes 7-8 (Hz, BE)                                     */
  int cw_speed;          /* byte 9 (WPM)                                           */
  int cw_weight;         /* byte 10                                                */
  int cw_hang_ms;        /* bytes 11-12 (BE)                                       */
  int cw_ptt_delay;      /* byte 13 (RF/PTT delay)                                 */
  int cw_ramp_width;     /* byte 17                                                */
  /* mic / line-in / PTT — byte 50 bitfield + byte 51 gain */
  int mic_linein;        /* 0x01 line-in selected (vs mic)                         */
  int mic_boost;         /* 0x02 +20 dB mic boost                                  */
  int mic_ptt_disabled;  /* 0x04                                                   */
  int mic_ptt_tip;       /* 0x08 tip/ring select                                   */
  int mic_bias;          /* 0x10 mic bias                                          */
  int linein_gain_db;    /* byte 51 = (gain+34)*0.6739+0.5, 0-31                   */
} p2_tx_cw;

/*
 * Offline self-test hook: build each outgoing packet into `buf` (>= 1444 bytes)
 * WITHOUT any socket, and return its wire length. Lets a tool hexdump-verify the
 * byte offsets against upstream with no radio. The live engine passes the off
 * arguments (pa_enabled=0, tx=NULL, cw=NULL); the TX arguments are exercised only
 * by sdrfl-txprobe.
 */
int p2_build_general(unsigned char *buf, int pa_enabled);
int p2_build_receive_specific(unsigned char *buf, int device, int sample_rate,
                              const p2_ps_state *ps, int xmit);
int p2_build_high_priority(unsigned char *buf, int device, long long rx_freq_hz,
                           int run, const p2_tx_state *tx, const p2_ps_state *ps);
int p2_build_transmit_specific(unsigned char *buf, const p2_tx_cw *cw,
                               const p2_ps_state *ps);

/*
 * Live TX state for the running link (F5, docs/TX-DESIGN.md). Pass a non-NULL
 * state to make the High-Priority / General / Transmit-specific packets carry TX
 * (MOX, drive, ALEX_TX_RELAY, PA-enable, attenuators→31, nDAC=1); pass NULL to
 * revert to the RX-only off state. Thread-safe; the keepalive timer applies it
 * within ≤100 ms, and while a TX state is set it also refreshes the General
 * (PA-enable) packet every cycle so PA-enable can never lag the relay.
 *
 * ⛔ Passing a keyed state here is what makes the radio transmit. Only the F5+
 * keying path (through tx_gate, into a dummy load, with the full TX-SAFETY
 * checklist satisfied and the operator present) may call this. The GUI app never
 * calls it until the TX control surface is wired.
 */
void p2_set_tx_state(const p2_tx_state *tx);

/*
 * ---- TX IQ path (F2, docs/TX-DESIGN.md) -----------------------------------
 *
 * The mic->DUC IQ (from the WDSP TX channel, src/engine/tx.c) is encoded to the
 * P2 wire form and framed into port-1029 packets. F2 builds and offline-tests
 * this; it is NOT run by the live engine until the F5 keying milestone — nothing
 * calls the framer or the socket emitter in F1-F4, so no TX IQ leaves the host.
 */
#define P2_TX_IQ_SAMPLES 240   /* IQ pairs per TX-IQ packet (1440-byte payload) */

/*
 * Encode `n_pairs` interleaved IQ doubles (~[-1,1)) to the P2 TX wire form: 6
 * bytes per pair — 24-bit big-endian signed I then Q, with piHPSDR's full-scale
 * mapping (np.c:2919). Writes n_pairs*6 bytes to `out`; returns bytes written.
 */
int p2_tx_iq_encode(const double *iq, int n_pairs, unsigned char *out);

/* Emitter for one complete 1444-byte TX-IQ packet (4-byte BE sequence + 1440-byte
 * payload). The framer calls this once per full packet. */
typedef void (*p2_tx_iq_emit)(const unsigned char *pkt, int len, void *user);

/*
 * Stateful framer: accumulates encoded IQ into 240-sample (1440-byte) packets,
 * prepends the running sequence, and emits each full packet via `emit`. Leftover
 * samples are held for the next push. Zero-initialised by p2_tx_iq_framer_init.
 */
typedef struct {
  unsigned char payload[P2_TX_IQ_SAMPLES * 6];
  int           fill;      /* IQ pairs currently in payload (0..239)   */
  uint32_t      seq;       /* next packet sequence number              */
  p2_tx_iq_emit emit;
  void         *user;
} p2_tx_iq_framer;

void p2_tx_iq_framer_init(p2_tx_iq_framer *f, p2_tx_iq_emit emit, void *user);
void p2_tx_iq_framer_push(p2_tx_iq_framer *f, const double *iq, int n_pairs);

/*
 * Dormant live emitter (F5): send one framed packet to the radio's TX-IQ port
 * 1029 over the engine's data socket. NOT called anywhere in F1-F4 — it exists so
 * F5 only has to wire tx.c -> framer(this) and start a feed under the full
 * TX-SAFETY checklist. A no-op if the data socket is not open.
 */
void p2_tx_iq_socket_emit(const unsigned char *pkt, int len, void *user);

#endif /* SDRFL_ENGINE_PROTOCOL2_H */
