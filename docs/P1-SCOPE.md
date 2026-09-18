# Protocol 1 (METIS) — scope for the Hermes Lite 2 milestone

Companion to `docs/P2-RX-SCOPE.md` (the P2 twin) and `docs/TX-SAFETY.md`.
Everything here was established 2026-07-12 by two first-hand audits:
piHPSDR `old_discovery.c`/`old_protocol.c` @974acba (byte-level wire
reference) and the official HL2 upstream (wiki Protocol page, `hermeslite.py`,
gateware `dsopenhpsdr1.v`). Target radio: **Hermes Lite 2** on the LAN
(192.168.1.21, gateware 73.2, board id 6 → synthetic `DEVICE_HERMES_LITE2`
506; current upstream stable is 74.2).

> **Status:** the whole RX milestone (R1–R4) landed and was live-verified on
> 2026-07-12; TX and PureSignal over P1 followed the same day —
> `docs/P1-TX-SCOPE.md`. Still ahead: whitelisting HL1 / original Metis
> boards after a live test. Originally a scope plan — the per-step gate
> narration was removed once done (last full version: commit 39bd8a1).

## 0. What P1 is, in one paragraph

One UDP socket, radio port 1024, everything multiplexed: discovery
(`EF FE 02`), start/stop (`EF FE 04 <cmd>`, bit0 = IQ stream), and data
(`EF FE 01 <ep> <seq32>` + two 512-byte "USB frames" per 1032-byte packet).
Host→radio frames (EP2) carry 3-byte sync `7F 7F 7F`, 5 C&C bytes (C0
address/MOX + C1-C4 payload) and 63×(4B headphone audio + 4B TX IQ); the C&C
round-robin over C0 addresses replaces P2's dedicated packet types.
Radio→host (EP6) frames mirror that: sync, 5 status bytes, then per-RX
24-bit BE I/Q groups + 16-bit mic, 63 samples/frame at 1 RX. There is no
keepalive packet: **the continuous EP2 stream is the keepalive** (HL2
watchdog stops streaming + TX ~10 s after the last host packet).

## 1. Discovery

`src/engine/discovery_p1.c` (adapted from piHPSDR `old_discovery.c` the same
way `discovery_p2.c` adapts `new_discovery.c`; results land in the shared
`discovered[]`). Reply: `[2]` status (2 idle/3 in use), `[3..8]` MAC, `[9]`
gateware major, `[10]` board id; board 6 → composite version
`10*buf[9]+buf[21]`, ≥ 400 ⇒ Hermes Lite 2 (else HL1). MAC-level dedup
against everything already discovered (the same radio answers directed +
per-interface broadcast rounds). Wired into the picker (always), `sdrfl-
discover` (always) and `start_radio` (only when the pinned IP wasn't
answered by P2 — a P2 start pays no extra probe time).

HL2 extras deliberately not used yet: discovery also answers on port 1025;
reply bytes carry temperature/fwd/rev/bias telemetry + ADC clip count; the
out-of-band `EF FE 05` command packet (port 1025) can read/write registers
without starting the stream (documented only in gateware + hermeslite.py).

## 2. The RX path (R1–R4, done 2026-07-12)

P1 link core in `src/engine/protocol1.c`: one socket, start/stop, an EP2
sender thread on the fixed 1032 B / 2.625 ms grid (zeroed audio + IQ payload
while not keyed), the C&C round-robin builder and the EP6 parser (sync hunt,
sequence check, 63×24-bit IQ → float). Analyzer, demod and audio are
protocol-agnostic — the same `on_rx_iq` contract as P2. Start policy: P2
discovery first, the P1 round only when the pinned IP did not answer; the
radio is selected BY IP, never `discovered[0]`. GUI: one
`engine_set_frequency()` dispatch for all tuning paths, a footer LNA slider
(−12..+48 dB, persisted `[rx] lna`) instead of Att, rates 48–384 k, the 6 m
band button greyed (38.4 MHz ceiling), the "ADC OVL" badge and the die
temperature (`0.0795898·raw − 50` °C) from `p1_get_telemetry`; the TCI server
also starts for RX-only radios. Gates: `sdrfl-p1probe`, `sdrfl-panprobe`,
`sdrfl-audioprobe`.

## 3. HL2 device profile facts

- **Gain, not attenuator**: single AD9866 LNA setting −12..+48 dB, sent as
  `0x40 | (gain+12)` in C&C 0x14-C4 (extended mode). piHPSDR calibration
  point: `rx_gain_calibration = 14` dB. "It is essential to have some gain
  set" — default +14 dB. No dither/random, no Alex attenuators, `n_adc=1`.
- **12-bit ADC** (AD9866, 76.8 MHz), max 38.4 MHz. No official full-scale
  dBm figure upstream — calibrate the panadapter offset live like we did
  for the G2E/10E.
- **Rates**: 48/96/192/384 k only (C&C 0x00 C1 bits1:0), one rate for all
  receivers. Main gateware has 4 hardware RX (discovery byte 0x13 says).
- **RX-only no-TX guarantees on HL2** (the P1 analogue of our three P2
  layers): MOX bit (C0[0]) never set, drive byte (0x12-C1) 0, and **0x12-C2
  = 0x04 = "T/R relay locked to RX"** (HL2-specific bit; piHPSDR
  old_protocol.c:2243-2248). No PA-enable general byte exists in P1.
- **RQST/ACK extension**: C0 bit7 = request on writes, ACK frames echo
  register + data (C0 bit7 set in EP6 status — the EP6 parser MUST skip
  dot/dash bits in ACK frames). Rules: at most one RQST outstanding, at
  most every other frame. Not needed for R1-R3.

## 4. Gotchas collected up-front

Two wire lessons earned during R3 (both live-diagnosed on the HL2 with the
N2ADR filter board, 2026-07-12):

- **⛔ C0 values are FINAL bytes, not addresses.** piHPSDR's 0x02/0x04/0x12/
  0x14/… constants already carry the register address in bits [7:1] (bit 0 =
  MOX). R1 shipped them shifted once more — RX *appeared* fine only because
  the mislabeled "TX frequency" frame (0x02<<1 = 0x04) landed on RX1's NCO;
  in reality the TX NCO, LNA gain, T/R-relay lock and PWM config never
  reached the radio. Symptoms that unmasked it: filter-board relays silent,
  LNA slider inert. When touching cc_round_robin, re-verify the C0 bytes
  against old_protocol.c line by line, never against our own comments.
- **The N2ADR filter board is host-driven, not gateware-automatic.** The
  HL2's J16 open-collector outputs drive the LPF relays and the HOST must
  select them per band via the OC bits in the C0=0x00 frame (`OCrx << 1`
  into C2, old_protocol.c:1916). piHPSDR defaults HL-class radios to
  filter_board=N2ADR with per-band values 160m=1, 80m=66, 60/40m=68,
  30/20m=72, 17/15m=80, 12/10m=96 (radio.c:2443-2471). Our
  `n2adr_oc_bits()` maps the RX frequency to those values with next-higher-
  LPF thresholds between ham bands. No board fitted → pins drive nothing.
- **⛔ Keyed EP2 sends go on the fixed 2.625 ms grid, never
  production-paced** (P1-TX-SCOPE §1, live-diagnosed 2026-07-12): bursting
  one WDSP block as 8 back-to-back packets oscillates the HL2 TX FIFO at
  the ~47 Hz block rate = a mains-like buzz on ALL TX audio. Corollary:
  **TX bring-up checklists must include off-air listening** — the wire
  log looked clean for a whole day while every mode buzzed.

- The same UDP socket carries everything; the radio sends EP6 to whatever
  host port sent the start command — bind once, keep it.
- EP6 sequence number check: tolerate seq 0 (radio restart), else log gaps.
- Status frames: register = `(C0 >> 3) & 0x1F`; C0 bit0 = PTT, bit1 dash,
  bit2 dot — but NOT in HL2 ACK frames (bit7 set). PTT/dot/dash edges are
  radio-initiated (hardware key at the radio); an RX-only client just logs
  them.
- Host must keep sending EP2 even with nothing to say (watchdog ~10 s +
  the C&C round-robin lives there). Stop = `EF FE 04 00`.
- TCP mode (1032-byte-everything, port 1024) exists upstream; we skip it.
- Startup sequence (piHPSDR `old_protocol_run`): prime 2×2 zero frames
  (C0=0x00 + freq), 20 ms apart, then start, then expect the first EP6
  within the retry loop (10 tries).
