/*
 * sdr-for-linux — HPSDR Protocol-1 (METIS) RX link (headless, GLib-only).
 *
 * The P1 twin of protocol2.h: a lean, RX-only implementation of piHPSDR's
 * old_protocol.c @ 974acba, scoped to ONE receiver on a Hermes Lite 2
 * (docs/P1-SCOPE.md, R1). Everything is multiplexed on one UDP socket
 * against radio port 1024:
 *
 *   host→radio : EF FE 04 <cmd>  start/stop (bit0 = EP6 IQ stream)
 *                EF FE 01 02 <seq32> + 2×512 B USB frames ("EP2"): 7F 7F 7F
 *                sync, C0..C4 command bytes, 63×(4 B audio + 4 B TX IQ,
 *                all-zero here). The continuous EP2 stream IS the keepalive —
 *                the HL2 watchdog stops streaming ~10 s after the last packet.
 *   radio→host : EF FE 01 06 <seq32> + 2×512 B frames ("EP6"): sync, 5 status
 *                bytes, then 63×(24-bit BE I, 24-bit BE Q, 16-bit mic) at 1 RX.
 *
 * ⛔ No-TX guarantees (the P1 analogue of the three P2 layers, P1-SCOPE §3):
 * the MOX bit (C0[0]) is never set, the drive byte (0x12-C1) is always 0, and
 * on HL-class radios 0x12-C2 = 0x04 locks the T/R relay to RX. There is no
 * TX-capable code in this module at all.
 */
#ifndef SDRFL_ENGINE_PROTOCOL1_H
#define SDRFL_ENGINE_PROTOCOL1_H

#include "discovered.h" /* DISCOVERED, DEVICE_HERMES_LITE2 */

/* RX IQ callback — same contract as p2_iq_cb (interleaved I/Q doubles scaled
 * by 1/2^23, called from the listener thread; keep it fast). */
typedef void (*p1_iq_cb)(const double *iq, int n_pairs, void *user);

/*
 * Start Protocol 1 on `dev` (from p1_discovery), one receiver at `freq_hz`,
 * `sample_rate` ∈ {48000, 96000, 192000, 384000}. Primes the C&C registers,
 * sends the METIS start command, and spawns the EP2 sender (keepalive/C&C
 * round-robin, 1 packet / 2.625 ms) + EP6 listener threads.
 * Returns 0 on success, negative on error (no answer / bad rate).
 */
int p1_rx_start(const DISCOVERED *dev, long long freq_hz, int sample_rate,
                p1_iq_cb cb, void *user);

/* Stop the stream (EF FE 04 00), join threads, close the socket. */
void p1_rx_stop(void);

/* Re-tune RX1 (thread-safe; the sender's next 0x04 frame carries it — and the
 * 0x02 frame keeps the TX NCO on the same value, which is what the HL2
 * gateware's automatic N2ADR filter-board LPF selection tracks). */
void p1_set_frequency(long long freq_hz);

/*
 * HL2 LNA gain in dB, −12..+48 (AD9866 extended mode: C&C 0x14-C4 =
 * 0x40 | (gain+12), old_protocol.c:2292-2308). piHPSDR's calibration point
 * is +14 dB ("essential to have some gain set"). Thread-safe.
 */
void p1_set_gain(int db);

/*
 * ---- TX byte construction (T1, docs/P1-TX-SCOPE.md) ------------------------
 *
 * ⛔ SAFETY: these let the builders CONSTRUCT the TX bytes; they do NOT
 * transmit. The live engine calls them with tx=NULL (see the hardcoded off
 * call sites in protocol1.c) → the wire stays byte-identical to the verified
 * RX build: MOX bit 0 on every frame, drive 0, T/R relay locked RX. Only the
 * offline sdrfl-p1txprobe gate passes a non-NULL "hot" state, and only to
 * hexdump-verify the layout against piHPSDR — never over a socket. Do NOT
 * wire a non-off state into any live send before the T2-T4 phases of
 * docs/P1-TX-SCOPE.md are cleared with Richard.
 */

/* HL2 TX state. drive_att is piHPSDR's drive_level: the 16-step hardware TX
 * attenuator byte {0,16,...,240} of C&C 0x12-C1 (radio.c:2934-2996); the
 * second drive component (IQ scaling) is applied by the TX IQ encoder. */
typedef struct {
  int mox;        /* key the radio → C0 |= 0x01 on EVERY frame (o_p.c:2769-89) */
  int pa_enabled; /* 0x12-C2 = 0x08 (PA on) instead of 0x04 (T/R locked RX)    */
  int in_band;    /* TX freq inside a ham band (else the drive byte is forced 0)*/
  int drive_att;  /* hardware TX attenuator step, 0-240 in 16s (0x12-C1)       */
  int tune;       /* TUNE (AH4/IO-board ATU bit 0x10 NOT sent — no ATU here)   */
} p1_tx_state;

/* Pure frame builders (offline-testable, no socket). tx == NULL → the exact
 * RX-only bytes the live engine sends today (regression guarantee, enforced
 * by sdrfl-p1txprobe). `step` advances the round-robin; returns next step. */
void p1_build_cc_general(unsigned char c[5], int device, int rate_bits,
                         long long freq_hz, const p1_tx_state *tx);
int p1_build_cc_round_robin(unsigned char c[5], int device, long long freq_hz,
                            int lna_gain_db, int step, const p1_tx_state *tx);

/*
 * Encode TX IQ pairs to the EP2 payload form: per sample 4 zero audio bytes
 * (⛔ on the HL2 non-zero audio addresses the EXTENDED REGISTER space —
 * old_protocol.c:1727-1735) + 16-bit signed BE I,Q via piHPSDR's
 * (int32)(x*32766.672+32767.5)-32767 with `scale` (= drive IQ component)
 * pre-applied; on HL-class the low byte of I and Q is masked & 0xFE (⛔ CWX
 * guard, old_protocol.c:1752-1760 — the IQ LSBs are firmware keying bits).
 * Writes n_pairs*8 bytes to `out`, returns bytes written. Pure function.
 */
int p1_tx_iq_encode(const double *iq, int n_pairs, double scale, int device,
                    unsigned char *out);

/* Read-only telemetry decoded from the EP6 status frames (addr 0-2). */
typedef struct {
  int valid;          /* 1 once at least one status frame was parsed          */
  int adc_overload;   /* ADC clipped since last poll (latched, read+clear)    */
  int seq_errors;     /* EP6 sequence gaps seen so far                        */
  int sync_errors;    /* frames dropped for a bad 7F 7F 7F sync               */
  int ptt, dot, dash; /* radio-side key/PTT inputs (state, RX-only: log only) */
  int temp_raw;       /* HL2: addr-1 "exciter power" slot = temperature ADC;
                         °C = 0.0795898*raw − 50 (piHPSDR rx_panadapter.c:884) */
  int current_raw;    /* HL2: addr-2 AIN slot = PA current ADC;
                         mA ≈ 0.505396*raw (rx_panadapter.c:943)              */
} p1_telemetry;

/* Snapshot telemetry (thread-safe). adc_overload is latched and cleared by
 * this read — single consumer. */
void p1_get_telemetry(p1_telemetry *out);

#endif /* SDRFL_ENGINE_PROTOCOL1_H */
