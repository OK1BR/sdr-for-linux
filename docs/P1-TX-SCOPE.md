# Protocol 1 TX — scope for the Hermes Lite 2 (docs/P1-SCOPE.md companion)

⛔ **TX-SAFETY applies in full** (docs/TX-SAFETY.md): every byte below was
read first-hand from piHPSDR old_protocol.c / radio.c / transmitter.c
@974acba on 2026-07-12, with line references, and MUST be re-verified there
whenever the sending code is edited. TX-capable code goes live only through
the phased gates at the bottom, each live step with Richard's explicit
consent. Until the live phase, the three P1 no-TX guarantees stay untouched:
MOX bit never set, drive 0, T/R relay locked RX (0x12-C2 = 0x04).

## 1. The P1 TX wire, byte by byte (piHPSDR @974acba)

- **MOX**: `C0 |= 0x01` on EVERY outgoing EP2 frame while transmitting
  (ozy_send_buffer end, :2769-2789) — both the C0=0x00 frame and the
  round-robin frame. CW: piHPSDR sets MOX too unless the in-FPGA keyer is
  used (`!cw_keyer_internal` → set); we key host-side (cw_gen), so for us
  CW == every other mode here.
- **TX IQ**: rides the SAME EP2 frames that today carry zeros — per sample
  4 B audio + 4 B IQ, 63 samples/frame, 126/packet.
  - Encoding (:1720-1760): 16-bit signed BE I then Q, via
    `(int32)(x * 32766.672 + 32767.5) - 32767` (implicit 0.99999 headroom).
  - ⛔ **HL2 audio bytes are ALWAYS zero** — non-zero "audio" addresses the
    HL2 *extended register* space (:1727-1735, also :1618-1660 for RX)!
  - ⛔ **HL2 CWX guard**: low byte of I *and* Q `& 0xFE` (:1752-1760) — the
    IQ LSBs are CWX keying bits in the HL2 firmware; clearing them avoids
    spurious keying (DAC is 12-bit, no quality loss).
- **Drive is two-component on the HL2** (radio.c:2934-2996): a 16-step
  hardware TX attenuator in 0x12-C1 (`drive_level` ∈ {0,16,…,240} ≈
  −7.5..0 dB in 0.5 dB steps) **plus host-side IQ scaling** (`drive_scale`
  multiplied into every TX IQ sample, `do_scale`). Request levels ≤ 107
  map to attenuator step 0 with pure IQ scaling 0..0.995.
- **0x12-C2, HL2 semantics** (:2230-2255; all Alex/Apollo bits stay 0):
  - `0x08` = **PA enable** (ADDR-9 bit 19) — only when PA enabled + band PA
    allowed;
  - `0x04` = T/R relay locked to RX (bit 18) — the PA-disabled state; TX
    then leaves only the low-power RF1 output (dry-key analogue);
  - `0x10` = TUNE request for the AH4/IO-board ATU (bit 20) — only with the
    ATU option, skip for now.
- **0x2E** (:2536-2549): C3 = PTT hang 20 ms (bits 4:0), C4 = **TX latency
  40 ms** (bits 6:0) — the FPGA buffers 40 ms of TX IQ before RF starts
  (FIFO holds ~75 ms / 3600 samples). Send this register during setup.
- **Pacing** (:250-330): during TX the EP2 stream is clocked by TX IQ
  production at 48 kHz — 126 samples per packet ⇒ nominal 2.625 ms, with a
  host-side FIFO estimate throttling the send delay (0/500/2000 µs). The
  radio reports **TX FIFO underrun/overrun in EP6 addr-0 C3 bits 0xC0**
  (:1268-1290; mask underruns right after the RX→TX transition until the
  FIFO first fills). Underrun = broken RF envelope; overrun = latency
  creep — both must be surfaced.

## 2. Telemetry & protections (EP6 status, 16-sample EMA like piHPSDR)

| addr | C1C2 | C3C4 |
|---|---|---|
| 0 | fw version etc. | C3 bits 0xC0 = TX FIFO underrun (0x80) / overrun (0xC0) |
| 1 | **temperature** (the "exciter power" slot; °C = 0.0795898·raw − 50) | **fwd power raw** (+ max-hold for PEP, :1310) |
| 2 | **rev power raw** | **PA current raw** (mA ≈ 0.505396·raw) |

- **Wattmeter** (transmitter.c:685-693): HL2 branch `c1=3.3, c2=1.5,
  rc2=1.5, fwd_off=6, rev_off=6` — NOTE c2 differs from the 10E profile
  (0.095) by ~16×; a fresh `radio_tx_profile()` entry is mandatory, plus
  live calibration against the external wattmeter at 5 W scale.
- **fwd/rev swap hook** (transmitter.c:701-708): some HL2s have the current
  sense transformer wound backwards — if rev > fwd, piHPSDR swaps the two.
  Port this; harmless on correct units.
- **SWR protection redesign**: same drop-drive/refuse logic as P2
  (tx_meter), but fed from these raw words with the HL2 constants; add a
  **temperature guard** (drop TX above ~60 °C, warn earlier — we already
  decode temperature live) since a 5 W PA has no Alex-style protection
  around it.
- PS far-future note: HL2 P1 TX-DAC feedback peak ≈ 0.230 → GetPk display
  offset 17.0 (transmitter.c:838). Feedback via RX3/RX4.

## 3. What our engine needs (delta from the P2 TX path)

1. **protocol1.c builders** (offline first, txprobe pattern): a
   `p1_tx_state` (mox, pa_enabled, in_band, drive_att, tune) consumed by
   `cc_general`/`cc_round_robin`/frame filler; TX IQ encoder (16-bit BE +
   CWX guard + drive_scale); 0x2E in the round-robin. Live path keeps
   passing the off state until the live phase.
2. **TX IQ feed**: WDSP TX channel output at **48 kHz** for P1 (the P2 path
   runs 192 k to port 1029) + a ring the EP2 sender drains — sender switches
   from zero payload to ring payload while keyed, pacing per §1.
3. **tx_run dispatch**: p1/p2 branch like engine_set_frequency (set_tx_state,
   IQ route, meters source).
4. **Config**: `radio_tx_profile()` HL2 entry (5 W, wattmeter constants
   above, pacal floor TBD live, cfg group `[tx-hl2]`), pa_calibration
   default 40.5 dB (piHPSDR band.c — "the No. 1 problem for new HermesLite
   users is 'no RF output'").
5. **Whitelist last**: `radio_tx_supported()` += HL2 only at the live
   checklist, with Richard present.

## 4. Phased gates (mirrors TX-DESIGN §F1-F6)

| Phase | Content | Gate |
|---|---|---|
| T1 ✅ | Offline builders + `sdrfl-p1txprobe`: hexdump TX-hot frames, assert every byte against §1; assert the OFF state is byte-identical to the RX build | PASS 2026-07-12 (offline) + live RX regression |
| T2 ✅ | Engine: 48 k TX WDSP route (CFIR off — proven by the 0.5000-vs-0.4481 = ×0.896 magnitude in the txdsp gate), EP2 TX-IQ ring, production-paced keyed sender (20 ms zero-IQ keepalive cap), tx_run dispatch (engine_set_tx_state, drive split), CW amp 1.0 on P1 | PASS 2026-07-12: p1txprobe + txprobe + txdsp-test (new P1 section) |
| T3 ✅ | Protections: fwd/rev EMA + PEP max-hold + TX FIFO health in the EP6 parser, thermal trip 60 °C in tx_gate (4 new txgate-test cases), fwd/rev swap hook, HL2 radio_tx_profile (5 W, c2=1.5, offsets 6/6, [tx-hl2], pa_cal floor 25), P1 footswitch PTT | PASS 2026-07-12 (all offline gates + live RX regression) |
| T4 ✅ | Live checklist into the dummy load (through the tuner, external wattmeter at the tuner input); `radio_tx_supported()` += HL2 flipped with Richard's consent | **PASS live 2026-07-12 evening**: TUNE at drive 191-197/255 (first RF from the app on an HL2), 20 m pa_cal calibrated to **28.4 dB** (other bands stay at the safe under-driving 53 default until calibrated), request ≈ external meter, SWR 1.2 into the dummy (rev channel + swap hook verified), temperature stable, **CW via SDC/TCI** (clean per-element envelope in the wire log: dits 64 ms/dahs 139 ms, RF zero between elements, no backwave), **voice MOX** peaks ~4 W. Digi (TCI TX audio) deferred to a Decodium session — same external-source path as CW. |

*Audited & written 2026-07-12; the ENTIRE milestone (T1-T4) landed and was
live-verified the same day — the HL2 is a full RX+TX radio in the app. The
SWR bridge lives on the N2ADR filter board (fitted on Richard's unit).*

## 5. Open items after the live day

- **TX FIFO status semantics (gw 73.2)**: the addr-0 C3 top bits climb on
  BOTH sides ~30/s while the RF is demonstrably clean — they look like
  fill-level watermark indicators, not the latched events piHPSDR's comment
  suggests (measured there on gw 7.2). Verify against the HL2 gateware
  source; counters demoted to SDRFL_LAT_DEBUG until then.
- **Per-band pa_cal**: only 20 m calibrated (28.4 dB); the rest sit at the
  under-driving 53 dB default — calibrate as bands get used.
- **Drive linearity**: same top-end compression as the G2E/10E — covered by
  the planned guided multi-point wattmeter/drive calibration (TODO).
- **Digi TX** (TCI external audio): live check with Decodium pending.
- **PureSignal on P1**: separate milestone — needs the multi-RX P1 link
  (feedback via RX3/RX4), pscc feed, LNA-gain-based auto-attenuate
  (piHPSDR maps 31−att into the 0x14 register during PS-TX), GetPk offset
  17.0 (TX-DAC peak 0.230). No known firmware wedge on P1 (unlike P2 on
  the 10E) — and the same infrastructure is the alternative route to PS
  on the 10E, which also speaks P1.*
