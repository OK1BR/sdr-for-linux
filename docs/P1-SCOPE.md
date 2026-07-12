# Protocol 1 (METIS) — scope for the Hermes Lite 2 milestone

Companion to `docs/P2-RX-SCOPE.md` (the P2 twin) and `docs/TX-SAFETY.md`.
Everything here was established 2026-07-12 by two first-hand audits:
piHPSDR `old_discovery.c`/`old_protocol.c` @974acba (byte-level wire
reference) and the official HL2 upstream (wiki Protocol page, `hermeslite.py`,
gateware `dsopenhpsdr1.v`). Target radio: **Hermes Lite 2** on the LAN
(192.168.1.21, gateware 73.2, board id 6 → synthetic `DEVICE_HERMES_LITE2`
506; current upstream stable is 74.2).

> ⛔ This milestone is **RX only**. P1 TX (5 W PA, IQ-scaled drive, CW)
> comes much later, behind the full TX-SAFETY process, per-model whitelist
> and Richard's explicit consent — exactly like the G2E/10E bring-ups.

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

## 1. Discovery — DONE (2026-07-12, live-verified)

`src/engine/discovery_p1.c` (adapted from piHPSDR `old_discovery.c` the same
way `discovery_p2.c` adapts `new_discovery.c`; results land in the shared
`discovered[]`). Reply: `[2]` status (2 idle/3 in use), `[3..8]` MAC, `[9]`
gateware major, `[10]` board id; board 6 → composite version
`10*buf[9]+buf[21]`, ≥ 400 ⇒ Hermes Lite 2 (else HL1). MAC-level dedup
against everything already discovered (the same radio answers directed +
per-interface broadcast rounds). Wired into the picker (always), `sdrfl-
discover` (always) and `start_radio` (only when the pinned IP wasn't
answered by P2 — a P2 start pays no extra probe time). Live: HL2 found via
directed UDP, correct name/version/range; picker shows the row greyed
("Not supported yet") until the RX path lands.

HL2 extras deliberately not used yet: discovery also answers on port 1025;
reply bytes carry temperature/fwd/rev/bias telemetry + ADC clip count; the
out-of-band `EF FE 05` command packet (port 1025) can read/write registers
without starting the stream (documented only in gateware + hermeslite.py).

## 2. RX milestone plan (mirrors the P2 bring-up gates)

| Step | Content | Gate |
|---|---|---|
| R1 ✅ | P1 link core: socket, start/stop, EP2 sender thread (1032 B / 2.625 ms pacing, zeroed audio+IQ payload), C&C round-robin builder, EP6 parser (sync hunt, seq check, 63×24-bit IQ → float) | `sdrfl-p1probe` (headless IQ counter, like sdrfl-rxprobe) — PASS live 2026-07-12 |
| R2 ✅ | Feed the existing WDSP analyzer + demod/audio path (they are protocol-agnostic — same `on_rx_iq` contract as P2). Both gates grew the start_radio discovery policy (P2 first, P1 round only when the pinned IP didn't answer), select the radio BY IP (not `discovered[0]` — the broadcast fallback also collects the P2 radios), and branch p1/p2_rx_start on `dev->protocol`; audioprobe caps a >384k rate to P1's maximum | PASS live 2026-07-12: panprobe @192k (40 m band picture, floor −115 dB), audioprobe @384k-capped (CW audio, 0 ferr, queue ≤3 ms) |
| R3 ✅ | GUI integration: whitelist `radio_supported()` += HERMES_LITE2 (RX-only; TX/PS whitelists exclude P1 structurally), one `engine_set_frequency()` dispatch for all 7 tuning paths, footer LNA slider (−12..+48 dB, persisted `[rx] lna`) replacing Att on P1, prefs rate list 48-384 k, 6 m band button greyed (38.4 MHz cap) | PASS live 2026-07-12: tuning/controls + filter-board relays confirmed by Richard. Landed two WIRE FIXES (see §4): the R1 C0 double-shift and the N2ADR OC bits |
| R4 ✅ | Polish: ADC-overload badge ("ADC OVL", single-ADC text) + die temperature (`0.0795898*raw − 50` °C, EMA, footer "Temp" slot replacing "Supply", green <45/amber/red >55 °C) from `p1_get_telemetry` in the GUI tick; TCI server decoupled from the TX runtime (starts for RX-only radios — control/RX-audio/IQ/spots work, TX ops already refuse on `!tx_ready`); LNA slider + PTT-ignore landed in R3/R1 | PASS live 2026-07-12: temperature shown (~33 °C), TCI up on the HL2 |

Reuse from the P2 engine: analyzer, demod, audio_pw, panadapter/waterfall,
picker, settings — the ONLY new code is the P1 link (discovery done + R1)
and small GUI conditionals (rates, gain, no atten).

## 3. HL2 device profile facts (for R3 and far-future TX)

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
- **Far-future TX** (documented now so it isn't re-researched): PA_5W,
  `pa_calibration` default 40.5 dB (piHPSDR band.c — "the No. 1 problem
  for new HermesLite users is 'no RF output'"), drive = 16-step hardware
  attenuator (0x12-C1 high nibble) + host-side IQ scaling (`do_scale`,
  radio.c:2936-2993), TX IQ LSBs cleared (CWX guard), PTT-hang/TX-latency
  via 0x2E (piHPSDR: 20/40 ms), PS feedback = RX3/RX4 (4 receivers).
  Telemetry: temperature + bias current instead of Alex fwd/rev — the SWR
  protection design must be revisited for HL2 (fwd/rev exist as raw 12-bit
  values in responses; needs its own calibration).
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
- `enp134s0f1` carries two IPv4 addresses (LAN + link-local) → per-address
  broadcast rounds duplicate P2 replies in `discovered[]`; P1 dedups by
  MAC, P2 (vendored logic, unmodified) does not — the picker dedups rows,
  `sdrfl-discover` output may show a P2 radio twice. Cosmetic.

*Written 2026-07-12 after the discovery step went live; the ENTIRE RX
milestone (R1-R4) landed and was live-verified the same day. The HL2 is a
supported RX-only radio. Still here for the future: P1 TX (§3 facts
pre-collected; full TX-SAFETY process + Richard's consent required), P1 PS
as the possible 10E-PS route, HL1/Metis whitelisting after a live test.*
