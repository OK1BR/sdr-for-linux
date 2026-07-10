# F7 — PureSignal: scope & plan

Goal: TX linearization (adaptive predistortion) on the ANAN G1 over Protocol 2,
using the WDSP PS engine, exactly the way piHPSDR does it. Study phase done
2026-07-10 against piHPSDR @974acba (the G1 is known natively to that revision —
`NEW_DEVICE_G1`, so nothing below is inferred from "similar" boards) and our
vendored WDSP. File:line refs: `pihpsdr/` = `~/.local/opt/pihpsdr/src`,
`wdsp/` = `vendor/wdsp`.

**Headline findings**

- The whole PS engine is **already compiled into our libwdsp** (`calcc.c` +
  `iqc.c`, vendor/wdsp/meson.build:24,54). Nothing to vendor, nothing to patch.
  Every TXA channel already carries calcc's 4 worker threads today.
- piHPSDR has **no device gate** for PS: the G1 is treated as a fully
  PS-capable single-ADC radio of the Orion2/Saturn Alex family; internal
  feedback needs **no Alex routing bits** at all.
- The one real safety decision: PS requires the **ADC0 step attenuator to run
  at a controlled 0–31 dB value during TX**, replacing our hard "31/31 on TX"
  rule for that one ADC. Thetis documents the same exception
  (TX-SAFETY.md:83-85: forced 31 *"when PureSignal off"*). ⛔ Needs Richard's
  explicit sign-off before implementation (see §6).

## 1. The WDSP engine (calcc.c = calibrate, iqc.c = apply)

Apply side: `xiqc` sits **last in the TXA chain** before the CFIR/output
resampler (wdsp/TXA.c:587) — after modulators, compressor, ALC and the
tune/two-tone generator (so two-tone *is* corrected — that's what makes
two-tone calibration work). Per sample it evaluates three cubic splines
indexed by instantaneous envelope and applies a magnitude+phase predistortion
(iqc.c:128-140). When off it is a literal no-op (in==out==midbuff,
iqc.c:201-202). Coefficient swaps are double-buffered and crossfaded over 5 ms
(iqc.c:113-195). `iqc.run` is raised/cleared by calcc itself
(`SetTXAiqcStart/End`) — the host never rewires the chain.

Calibrate side: `pscc(txa_channel, size, txfb, rxfb)` (calcc.c:617) takes one
block of interleaved-complex doubles: `txfb` = the TX-DAC loopback (the ideal
reference), `rxfb` = the amplifier-coupler feedback (the distorted reality).
Block size is free (piHPSDR: 1024 ≈ 5.3 ms @192k); sample rate is whatever the
host declares via `SetPSFeedbackRate` — piHPSDR passes **192000 on P2**
(pihpsdr/radio.c:977). State machine: LRESET→LWAIT→LMOXDELAY→LSETUP→LCOLLECT
(bin samples by TX envelope into 16 intervals × 256 samples)→MOXCHECK→LCALC
(spline fit on a worker thread)→LDELAY→loop (calcc.c:525-799). Calibration
needs the envelope to span all 16 bins — a steady single tone never
calibrates; **two-tone or speech does**. 2 consecutive failed fits → LRESET
(correction dropped, calcc.c:795-796).

Host API actually used by piHPSDR (all take the TXA channel id):

- `SetPSControl(ch, reset, mancal, automode, turnon)` — the only run control
  (start = `(0,0,1,0)` continuous / `(0,1,0,0)` oneshot; stop = `(1,0,0,0)`).
- `SetPSMox(ch, mox)` on every key edge — lock-free (calcc.c:901-911), safe
  from the keying path.
- `SetPSFeedbackRate(ch, 192000)` once at startup.
- `GetPSInfo(ch, int[16])` — status poll. `info[4]` = feedback level
  (`256·hw_scale/rx_scale`, calcc.c:360), `info[5]` = cal counter, `info[6]`
  = sanity bitmask, `info[14]` = correcting (bool), `info[15]` = state.
- `SetPSHWPeak` ("SetPk") — expected full-scale TX envelope, hardware
  specific: P2 default **0.2899**, Saturn 0.6121 (pihpsdr/transmitter.c:
  1203-1241; G1 is not special-cased → P2 default; verify with `GetPSMaxTX`
  ("GetPk") on the first live cal — too small a value causes "very strange
  things" per the piHPSDR comment).
- `tx_ps_setparams` bundle (pihpsdr/transmitter.c:2564-2580) with defaults:
  ints 16, spi 256, map 1, pin 1, ptol 0.8 (0.4 = "relax"), moxdelay 0.2 s,
  loopdelay 0, ampdelay 150 ns via `SetPSTXDelay(1e-9·150)`.

Threading gotchas: the `SetTXAiqcStart/Swap/End` crossfades **busy-wait until
TX samples flow** — a PS reset while receiving needs ~7 dummy 1024-sample
`pscc` calls to flush state (piHPSDR does exactly that,
pihpsdr/transmitter.c:2477-2499); a reset while transmitting needs ~100 ms of
continued TX. `SetPSIntsAndSpi` is a stop-the-world resize — only call outside
TX. Use `pscc` (doubles), not `psccF` (2048-sample cap).

## 2. The radio side (P2, G1)

Feedback topology during PS TX (pihpsdr/new_protocol.c:1649-1668):

- **DDC0 = RX feedback** ← ADC0 (the coupler / "internal" path).
- **DDC1 = TX-DAC loopback** ← pseudo-ADC number `n_adc` (= **1** on the G1,
  single-ADC board).
- Both **fixed 192 kHz / 24-bit**, independent of the RX DDC rate.
- RX-specific packet: DDC0 slot `[17..22]`, DDC1 slot `[23..26]`, sync byte
  `[1363] = 0x02` ("DDC1 synced to DDC0"), enable bit only for DDC0
  (`[7] |= 1`).
- The radio then sends **one interleaved stream on DDC0's port** (1035):
  first sample of each pair = DDC0 = RX-fb, second = DDC1 = TX-fb; 119 pairs
  per 1444-B packet; 24-bit BE, scale 2⁻²³ (new_protocol.c:2525-2589).

On the G1 (Hermes-class DDC layout) the PS pair *is* the normal RX DDC pair —
fine, because non-duplex TX disables the RX DDCs anyway; piHPSDR even ignores
duplex on this family during PS TX (action table case 10110,
new_protocol.c:441-445).

High-priority packet, PS-conditional content:

- **DDC0+DDC1 NCO words are forced to the TX (DUC) frequency** while
  PS-transmitting (new_protocol.c:871-883) — that's what makes the feedback
  arrive baseband-aligned, no rotation needed anywhere.
- `ALEX_PS_BIT` (bit 18, alex.h:94) set in alex1 whenever PS is enabled, and
  in alex0 while PS-transmitting (new_protocol.c:1034-1038).
- Feedback source select: phantom `adc[2].antenna` (0 = internal coupler,
  7 = bypass; EXT1 doesn't exist on the G1 family). G1 decodes with the
  Orion2/Saturn offset (+100), and **case 100 "internal" sets no routing bits
  at all** (new_protocol.c:1288-1354) — the default just works.

Attenuators (the safety-relevant part): the **TX-specific packet is
authoritative** (new_protocol.c:1574-1586):

```
[59] (ADC0) = 31 when PA on … except PS on → transmitter->attenuation (0..31)
[58] (ADC1) = 31 when PA on   (always stays 31)
```

The HP-packet bytes 1442/1443 duplicate this for old firmware — but piHPSDR
puts the PS value in `[1442]`, which is the **ADC1** slot (new_protocol.c:
1447-1449) while `[1443]` stays 31. That contradicts the TX-specific mapping
and looks like a dead-code leftover; **we follow the TX-specific mapping and
mirror it consistently into HP [1443]** (ADC0), noting the divergence here.

Sequencing (pihpsdr/radio.c:2046-2103, transmitter.c:2442-2528):

- Key edge: `SetPSMox(1)` *before* the TX state goes out; zero the feedback
  accumulators (no stale half-buffer); HP packet → RX-specific packet.
- PS enable: set flag → send HP + RX-specific → wait 100 ms for the streams
  to start → `SetPSControl` resume + `tx_ps_setparams`.
- TUNE workaround: PS reset before TUNE, resume after (long-session "broad
  line spectrum" bug in the engine, radio.c:2728-2749).
- CW TX: feedback is *not* fed to pscc (WDSP is bypassed in CW anyway).

## 3. Control layer (piHPSDR ps_menu.c as UX reference)

- **Feedback-level indicator**: `info[4]` ideal 152.3, accept band 140–165
  (±0.7 dB); >181 blue "too strong", >128 green, >90 yellow, else red
  (ps_menu.c:214-238, 321-330). "Correcting" green/red = `info[14]`.
- **Auto-attenuate**: a 100 ms timer that runs *only during the two-tone
  experiment* (ps_menu.c:169-281). On each new calibration, if `info[4]` is
  outside 140–165: `delta = round(20·log10(info[4]/152.293))` dB (±15 dB
  jumps at the <25 / >275 extremes), clamp 0..31, then
  reset → write attenuation → send TX-specific → reset → resume. In normal
  QSO operation the value is static — auto-att is a calibration-time tool.
- **Two-tone test**: WDSP PostGen, 700+1900 Hz (negated for LSB-family
  modes), each tone at 0.49999 so the pair peaks at full scale
  (transmitter.c:2902-2956). It **keys MOX through the normal gates** and
  runs at the current drive level. Closing the PS menu force-stops it.
- Persisted state: PS enable itself persists and is re-armed after startup,
  plus auto_on/setpk/attenuation/all ps_* params (transmitter.c:356-370).

## 4. Mapping onto our engine

Everything lands in already-mapped places; no architectural change:

- **protocol2.c** (single-DDC today, but dispatch is already per-port with
  per-DDC sequence counters, protocol2.c:636-681): add the PS block to
  `p2_build_receive_specific()` (second DDC slot + sync `[1363]` + enable
  bit — builder already addresses slots as `17 + ddc*6`); add the DDC0/1 NCO
  override + `ALEX_PS_BIT` + feedback-ant cases to `p2_build_high_priority()`;
  PS attenuation exception to both attenuator sites (HP :290-299, TX-spec
  :351-353). A feedback-IQ callback next to `on_rx_iq` (the existing
  `decode_iq` drops the DDC index — extend the callback API). All config
  changes ride the existing single-sender timer (state + kick, never a
  packet from another thread — §8 tripwire holds).
- **p2_tx_state** gets a `ps_attenuation` field so {MOX, ANT, LPF, BPF,
  attenuators} stay atomic in one builder pass (TX-SAFETY rule intact).
- **tx_run.c / tx_gate**: `SetPSMox` hooks into the existing key-ON/key-OFF
  edges in `gate_slot()` (tx_run.c:181-215); PS on/off through the cfg path
  (timer re-sends RX-specific every 200 ms already). WDSP TXA channel is id 8.
- **Feedback feed**: accumulate 1024 interleaved pairs (TX-fb / RX-fb split
  exactly like pihpsdr/new_protocol.c:2554-2571) → `pscc(8, 1024, txfb,
  rxfb)`. G1 has no `do_scale` — no IQ rescaling of the DAC feedback.
- **GUI**: PS group in Preferences → Radio → Transmit (enable, feedback ant
  Internal/Bypass, SetPk, relax tolerance, oneshot, manual TX-att spin,
  auto-att toggle); a **two-tone toggle** near TUNE in the footer;
  "Correcting" + feedback-level indicator on the TX panadapter (status
  fields ride `tx_run_status` — single-consumer telemetry rules respected).
  MON (feedback spectrum in the TX panadapter, +15.0 dB empirical offset on
  P2 non-Saturn) is optional polish, not core.
- **Persistence**: `[tx] ps_*` keys in config.ini (enable, setpk, atten,
  auto, ant, ptol, oneshot) — every control persists, per the house rule.

## 5. Phased plan (each phase gated, TX phases live only with consent)

1. **PS-1 plumbing (no RF)**: RX-specific PS block, HP NCO/PS-bit/attenuator
   changes behind a `ps_enabled` flag default-off; feedback demux + counters;
   offline gate extends `sdrfl-tci-test`-style checks (packet bytes vs
   piHPSDR reference values).
2. **PS-2 WDSP wiring**: SetPSFeedbackRate/Control/Mox, pscc feed, GetPSInfo
   into `tx_run_status`; minimal Prefs UI. Gate: with PS off, byte-identical
   packets to today (regression tripwire).
3. **PS-3 calibration UX**: two-tone (through tx_gate!), auto-attenuate state
   machine, indicators, persistence.
4. **PS-4 live**: drive into dummy load, watch feedback level + convergence,
   measure GetPk vs SetPk 0.2899, then on-air two-tone + voice A/B (IMD
   before/after on the IC-705 as off-air monitor). MON display + oneshot +
   SaveCorr/RestoreCorr as follow-ups.

## 6. ⛔ TX-safety deltas (require explicit sign-off, then TX-SAFETY.md update)

1. **ADC0 attenuator during PS TX** = `ps_attenuation` (0–31 dB) instead of
   forced 31; ADC1 stays 31. Exactly the piHPSDR/Thetis exception
   (TX-SAFETY.md:83-85 already records it). PS off ⇒ today's 31/31 behavior,
   bit-for-bit.
2. **Two-tone keys the radio** — it must go through `tx_run_request`/tx_gate
   like MOX/TUNE (out-of-band lockout, SWR trip, whitelist, digi cap all
   apply). PS menu-close force-stops it, piHPSDR-style.
3. Feedback DDC retune (NCO = TX freq) rides the same HP packet as MOX —
   atomicity preserved by construction (one builder).
4. Unchanged and re-affirmed: watchdog on, single-sender invariant, SWR trip
   on EMA averages, PEP single consumer, no IQ scaling outside WDSP.
