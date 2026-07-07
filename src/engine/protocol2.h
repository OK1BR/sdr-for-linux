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
 * Offline self-test hook: build each outgoing packet into `buf` (>= 1444 bytes)
 * using the given tune/rate, WITHOUT any socket, and return its wire length.
 * Lets a tool hexdump-verify the byte offsets against upstream with no radio.
 * `ddc` is the DDC index for `device` (0 for HERMES-class incl. ANAN G1).
 */
int p2_build_general(unsigned char *buf);
int p2_build_receive_specific(unsigned char *buf, int device, int sample_rate);
int p2_build_high_priority(unsigned char *buf, int device, long long freq_hz, int run);

#endif /* SDRFL_ENGINE_PROTOCOL2_H */
