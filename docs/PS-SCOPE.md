# F7 — PureSignal: scope & plan

Goal: TX linearization (adaptive predistortion) on the ANAN G2E over Protocol 2,
using the WDSP PS engine, exactly the way piHPSDR does it. Study phase done
2026-07-10 against piHPSDR @974acba (the G2E is known natively to that revision —
`NEW_DEVICE_G1`, so nothing below is inferred from "similar" boards) and our
vendored WDSP. File:line refs: `pihpsdr/` = `~/.local/opt/pihpsdr/src`,
`wdsp/` = `vendor/wdsp`. Implemented (`src/engine/ps.c`) and live-verified on
the G2E 2026-07-11 and, over Protocol 1, on the HL2 2026-07-12; the
engine-mapping and phased-plan sections (§4, §5) were removed once done (last
full version: commit d889fde).

**Headline findings**

- The whole PS engine is **already compiled into our libwdsp** (`calcc.c` +
  `iqc.c`, vendor/wdsp/meson.build:24,54). Nothing to vendor, nothing to patch.
  Every TXA channel already carries calcc's 4 worker threads today.
- piHPSDR has **no device gate** for PS: the G2E is treated as a fully
  PS-capable single-ADC radio of the Orion2/Saturn Alex family; internal
  feedback needs **no Alex routing bits** at all.
- The one real safety decision: PS requires the **ADC0 step attenuator to run
  at a controlled 0–31 dB value during TX**, replacing our hard "31/31 on TX"
  rule for that one ADC. Thetis documents the same exception (TX-SAFETY.md,
  Thetis cross-check, "TX attenuation lives in TX-specific bytes 57-59 too":
  forced 31 *"when PureSignal off"*). ⛔ Signed off by Richard 2026-07-10 —
  see §6.

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
  1203-1241; G2E is not special-cased → P2 default; verify with `GetPSMaxTX`
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

## 2. The radio side (P2, G2E)

Feedback topology during PS TX (pihpsdr/new_protocol.c:1649-1668):

- **DDC0 = RX feedback** ← ADC0 (the coupler / "internal" path).
- **DDC1 = TX-DAC loopback** ← pseudo-ADC number `n_adc` (= **1** on the G2E,
  single-ADC board).
- Both **fixed 192 kHz / 24-bit**, independent of the RX DDC rate.
- RX-specific packet: DDC0 slot `[17..22]`, DDC1 slot `[23..26]`, sync byte
  `[1363] = 0x02` ("DDC1 synced to DDC0"), enable bit only for DDC0
  (`[7] |= 1`).
- The radio then sends **one interleaved stream on DDC0's port** (1035):
  first sample of each pair = DDC0 = RX-fb, second = DDC1 = TX-fb; 119 pairs
  per 1444-B packet; 24-bit BE, scale 2⁻²³ (new_protocol.c:2525-2589).

On the G2E (Hermes-class DDC layout) the PS pair *is* the normal RX DDC pair —
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
  7 = bypass; EXT1 doesn't exist on the G2E family). G2E decodes with the
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
  **DECISION 2026-07-11 (Richard, after the three-way audit vs Thetis
  @852bf0e):** we implement the **Thetis** semantics instead — stepping on
  every new calibration during *any* keyed non-CW PS TX (voice included),
  accept window **(128, 181]**, one-shot step toward 152.293, `info[4]`>256
  (ADC clip) → slam +31.1 dB (Thetis PSForm.cs:728-784, 1109-1112); the
  too-weak stall detector (4 s without a calibration → hunt from 0 dB)
  stays **two-tone-scoped** — voice pauses must never zero the attenuator.
  The piHPSDR-faithful variant is preserved at git tag
  `ps-auto-att-pihpsdr`. STBL ships 0 in *both* references (the "Thetis
  recipe stbl=1" claim was wrong) → ours defaults 0, Preferences toggle.
- **Two-tone test**: WDSP PostGen, 700+1900 Hz (negated for LSB-family
  modes), each tone at 0.49999 so the pair peaks at full scale
  (transmitter.c:2902-2956). It **keys MOX through the normal gates** and
  runs at the current drive level. Closing the PS menu force-stops it.
- Persisted state: PS enable itself persists and is re-armed after startup,
  plus auto_on/setpk/attenuation/all ps_* params (transmitter.c:356-370).

## 6a. Live verification (2026-07-11, G2E, 20/17/40 m — CLOSED, works)

The Thetis-style implementation (a5ed663) was live-verified end-to-end (2T →
voice → drive changes 10↔41 → band changes mid-QSO → TUNE; ~196 s of keyed PS
TX across 32 overs). What is worth keeping from it:

- Voice calibrates *continuously* (≈3.4 calibrations/s) once auto-att holds
  the level; two-tone is only the initial bootstrap.
- **getpk median 0.290** → the P2 default SetPk **0.2899 is confirmed** for
  the G2E; do not retune.
- Big upward drive jumps go through the clip-slam (fdbk > 256 → 31 dB) and
  reconverge in 2–4 s; a band change mid-voice resolved in one step.
- The overnight voice flip-flop did **not** return with STBL = 0 — it was a
  level problem, not missing stabilization.
- TUNE park: state 0 throughout, cals frozen, instant resume.

Known cosmetic wart (deliberately NOT fixed): ~5/26 steps were chases of
fits completed in quiet passages (fdbk 78–118 at ~0 W fwd) — a down-step
immediately undone. Real Thetis dodges these by accident (its timer sees
only cals landing in the last 10 ms poll ≈ 1 in 10). If it ever annoys:
require 2 consecutive out-of-window cals before stepping. At steady drive
it does not occur.

Remaining PS-4 nice-to-haves (not alpha-blocking): TX-pan tap before
xiqc (toggle), MON feedback display, SaveCorr/RestoreCorr per band,
per-band ps_att memory (Thetis TxAttenData).

## 6b. PureSignal over Protocol 1 (HL2) — 2026-07-12

The WDSP runtime above is protocol-agnostic; only the wire differs. The
P1 wire (feedback via RX3/RX4 at nrx=4, LNA-as-attenuator, PS enable
bit, link restart on the enable edge, SetPk 0.2400, feedback rate = RX
rate, ≤192 kHz) is audited and specified in **docs/P1-TX-SCOPE.md §6**;
ps.c selects the wire via `ps_configure()`. This is also the intended
alternative route to PS on the ANAN 10E (TX-DESIGN §9 lockout).
**Live-verified on the HL2 2026-07-12** (2T + voice into the dummy load:
GetPk confirms SetPk 0.2400, auto-att bidirectional, 14 clean
enable-edge link restarts — results in P1-TX-SCOPE §6);
`radio_ps_supported()` includes the HL2 since that test.

## 6. ⛔ TX-safety deltas — signed off by Richard 2026-07-10 (this section is their record)

1. **ADC0 attenuator during PS TX** = `ps_attenuation` (0–31 dB) instead of
   forced 31; ADC1 stays 31. Exactly the piHPSDR/Thetis exception
   (TX-SAFETY.md, Thetis cross-check, already records it). PS off ⇒ today's 31/31 behavior,
   bit-for-bit.
2. **Two-tone keys the radio** — it must go through `tx_run_request`/tx_gate
   like MOX/TUNE (out-of-band lockout, SWR trip, whitelist, digi cap all
   apply). PS menu-close force-stops it, piHPSDR-style.
3. Feedback DDC retune (NCO = TX freq) rides the same HP packet as MOX —
   atomicity preserved by construction (one builder).
4. Unchanged and re-affirmed: watchdog on, single-sender invariant, SWR trip
   on EMA averages, PEP single consumer, no IQ scaling outside WDSP.
