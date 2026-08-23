# More radios — scope for unlocking further transceivers

Companion to `docs/P2-RX-SCOPE.md` (the P2 link this rides on),
`docs/P1-SCOPE.md` (how a second radio family was brought up) and
`docs/TX-SAFETY.md` (what TX costs before it may be enabled).

**Status: SCOPED 2026-08-20; S1-S5 IMPLEMENTED 2026-08-21** on branch
`g2-rx-bringup` (merged via PR #2, released as v0.4.1), with **no G2 on
hand**. The ANAN G2 / Saturn is enabled for **RX, TX and PureSignal** —
Richard's call the same day: unlock it properly, exactly as piHPSDR has it, so
the remote operator can walk the whole path inside issue #1. Every per-device
value comes from piHPSDR and is pinned by the offline gate `sdrfl-p2dev-test`
(174 checks). **§7 CAME BACK on 2026-08-22 — W1IZZ (gh#3) ran the probe set
twice (dummy load, then antenna) and walked the TX half on his G2; evaluated
2026-08-23 (BACKLOG SDR-6), details in §7.1.** What is done, and what still
is not:

| | |
|---|---|
| S1 device profile + picker | ✅ was already there (`discovery_p2.c:353-356` names it "Saturn/G2"); **live: discovery prints `Saturn/G2 device=10 (4RX) software_version=27(.46) status=2`** |
| S2 wire conditionals (G1 Alex 0+1, G2 n_adc, DDC2, band-pass) | ✅ code + offline gate; ✅ **the three headless probes came back clean (§7.1)** — rxprobe 192.9 kHz effective, panprobe raw floor −143.4 dB with antenna vs −149.4 on the dummy load (antenna noise 6 dB above the ADC floor = the RX path is through, not the −45 dB relay class), audioprobe audible with no errors |
| S3 whitelist += SATURN | ✅ connect + TX + PS; ✅ **live tuning and band relays confirmed indirectly** — he tuned, calibrated nine bands and made a voice QSO (per-band PA calibration only works if the LPF relays follow the band); an explicit "relays click" report was not asked for and did not come |
| G4 "drop the Att control" | ❌ **withdrawn — it was a misreading**, see §1 |
| S4 TX | 🟢 **live-walked by the tester** — PA calibrated per band against an LP-100A into a dummy load (his numbers 43.0–53.0 dB, see §7.1), our wattmeter reading within 1–2 W of the LP-100A, SWR "tracked well" against external equipment, a DX voice QSO barefoot with a good audio report. **Not reported:** the dry-key step as such, the SWR-alarm trip test (§7 step 4), CW. |
| S5 PureSignal | 🟡 "appeared to be working correctly" — no footer numbers (feedback level / state / auto-att) came back, so `ps_setpk` 0.6121 is confirmed only as "does not misbehave". Stays default-OFF like on every model. |

⛔ **"Unlocked" → "first live pass done, not yet measured by us."** Since
2026-08-22 the RX path, the per-band PA calibration, the wattmeter (±1–2 W
against an LP-100A at the levels he ran) and SWR tracking are confirmed by an
external tester on a real G2; PureSignal only by his impression. What is still
piHPSDR's starting value with no measurement behind it: the wattmeter outside
his report, the PS feedback scaling, and the reverse-power side. Say "tested by
one external operator, not calibrated by us" wherever the radio is announced.
The supply-voltage readout was wrong on the G2 (SDR-1, fixed 2026-08-23 from
piHPSDR + Thetis, awaiting his confirmation) and the status parser spammed his
log (SDR-8, fixed the same day).

Everything below is the audit the implementation was taken from; it stays as
written so the next person can re-check our bytes against upstream.

## ⭐⭐ PRIORITY (Richard, 2026-08-21) — this is now the front of the queue

Richard's decision, verbatim in intent: **first unlock the ANAN G2 for that
man, and only then answer him that he can try to test it.** So the order in
§3 is not just a ladder, it is a schedule: **S1 → S2 → S3 ship before anyone
writes into `OK1BR/sdr-for-linux#1`.**

This does **not** relax the whitelist policy in `src/radio_support.h`. It
schedules the work that earns the unlock. Everything the ⛔ note in §5 says
still holds — the volunteer is contacted with a build, a probe and an
expected output, never with a thank-you note and a question mark.

~~⛔ **RX ONLY. TX (S4/S5) is not in this batch and the TX whitelist stays
shut.**~~ **SUPERSEDED the same day (Richard, 2026-08-21): "odemkni správně i
TX, přesně podle toho, jak to mají u piHPSDR… aby nám mohl v rámci toho issue
otestovat celou cestu."** TX and PureSignal are unlocked too. The concern that
produced the RX-only rule was raised and answered, so record both halves:

* **What changed the balance:** the TX audit found nothing left to get wrong
  from here. `p2_build_transmit_specific()` has no device branch and neither
  does upstream's builder; the keyed HP bytes are the G2E's, and upstream puts
  the Saturn in the same class in every TX branch (including `rxant += 100`
  for the PS feedback path). The per-model numbers — PA_100W, the ANAN-7000
  wattmeter branch, `ps_setpk` 0.6121 — are §2's, now in `radio_tx_profile()`.
* **What did NOT change:** a remote volunteer still must not key an
  uncalibrated PA blind. So the safety moved from "refuse" to "start safe and
  say so": `[tx-saturn]` has no stored calibration → PA off, ANT1, 1 W,
  pa_calibration 53 dB (the under-driving direction), and the first keying IS
  the dry-key step. The out-of-band gate, the SWR alarm and the forced 31 dB
  attenuators are model-independent and stay armed. §7 spells out the dummy
  load walk-in he must do, and every announcement must say the wattmeter is
  uncalibrated on this model.

⛔ **Implementation does not happen from the `work` session.** Richard,
2026-08-21: this must be done by the proper project agent. This block is a
handover note and a priority marker — it is not a licence to write code.
(Done 2026-08-21 by the project agent on `g2-rx-bringup`.)

**Definition of done for the reply to `#1`:** a build W1IZZ can install, a
named probe to run, and the output we expect back. Until all three exist,
the issue stays open and unanswered on purpose.

Source of truth for every piHPSDR line reference below: the pinned tree
`/home/rfa/.local/opt/pihpsdr` @ `974acba`, read first-hand on 2026-08-20
(`np.c` = `new_protocol.c`). Our own line references are current as of
that date.

> ⛔ The whitelist policy in `src/radio_support.h` is unchanged and
> non-negotiable: a model is enabled only after its bring-up passed live
> on that physical radio. Nothing in this document adds a device to any
> whitelist. Scoping a radio ≠ supporting it.

## 0. Why the ANAN G2 (Saturn) is the cheapest next radio

Because it is not a new link layer — it is the P2 link we already ship,
plus a device profile. Three facts, all verified:

- **It answers our existing LAN discovery.** piHPSDR maps device ids 1010
  and 1011 to `NEW_DEVICE_SATURN`, name "Saturn/G2" (`new_discovery.c`
  :346-349); our `src/engine/discovery_p2.c:353-356` already does exactly
  the same thing. A G2 on the LAN is found today — the picker just greys
  the row out, because `radio_supported()` refuses it.
- **Our P2 builder already branches on it.** `ddc_for_device()`
  (`protocol2.c:153`) returns DDC2 for ORION/ORION2/SATURN (np.c:1631,
  835-842); the RX band-pass knee table treats G1/ORION2/SATURN as one
  "g2class" (`protocol2.c:350`, np.c:1073-1090).
- **The engine's shape fits.** One DDC, 24-bit IQ, the same high-priority
  and general packets. Nothing about Saturn needs a second transport.

The G2 also drags **ORION2** (ANAN-7000/8000, DLE 7000) along with it:
piHPSDR treats `NEW_DEVICE_ORION2` and `NEW_DEVICE_SATURN` as one case in
almost every branch cited below. One bring-up, two families in reach.

## 1. The actual gaps (what a G2 would hit today)

| # | Gap | Where we are now | piHPSDR reference |
|---|---|---|---|
| G1 | **Alex 0+1 enable.** Saturn/ORION2 need general byte `[59] = 0x03`; a G2E has one Alex board and needs `0x01`. | `protocol2.c:178` hardcodes `0x01` (the comment already flags this) | np.c:693-697 |
| G2 | **Two ADCs.** G2E is `n_adc = 1`; Saturn falls through to the `default:` branch and is `n_adc = 2`. | `protocol2.c:200` hardcodes `buf[4] = 1` | `radio.c`:1564-1584 |
| G3 | **ADC1 band-pass word.** Saturn/ORION2 program band-pass filters for ADC0 *and* ADC1, the second in `alex1`. | we only build `alex0` (`protocol2.c:341-365`) | np.c:1073-1120 |
| G4 | ❌ **WITHDRAWN 2026-08-21 — this row was wrong.** True statement: the Saturn has no *ALEX* attenuator (the 0/10/20/30 dB relay bank in the alex0 word, np.c:991-1010, gated on `have_alex_att`). But our footer "Att" is the **ADC step attenuator** (0-31 dB, HP byte 1443) — and piHPSDR sets `have_rx_att = 1` for `NEW_DEVICE_SATURN` (radio.c:1359-1366), exactly as for the G2E. The control stays; dropping it would have been a regression on a radio we cannot test. We never had an ALEX-attenuator control to remove. | Att (step attenuator) is correct as-is | radio.c:1359-1366, np.c:991 |
| G5 | **XVTR relay + speaker-amp mute** live in high-priority byte 1400 for ORION2/Saturn only. | not built | np.c:939-966 |
| G6 | **RX antenna encoding** — this class adds 100 to `rxant`, and the "new PA board" +1000 path must stay off. | G2E already takes the +100 path | np.c:1288-1300 |
| G7 | **TX LPF is not used on RX** for G1/ORION2/SATURN (the RX signal goes through band-pass, not the TX low-pass). | matches our G2E behaviour | np.c:1224-1232 |

None of G1-G7 is deep work. G1/G2 are one conditional each **and are now
implemented** (`protocol2.c`, `n_adc_for_device()` + the `[59]` branch, gated
by `sdrfl-p2dev-test`); G3 only matters once RX2 exists; G4 turned out to be a
misreading (above); G5 is GUI/HP-level and still open.

## 2. TX profile for the G2/Saturn — ⛔ do not guess these

`radio_tx_profile()` in `src/radio_support.h` would need a fourth entry.
Audited values (the ones that differ from our G2E entry are the point of
this section):

- **PA rating 100 W** — `radio.c`:1306-1308 puts `NEW_DEVICE_SATURN`
  alongside `NEW_DEVICE_G1` at `PA_100W` ("make 100W the default for G2").
- **Wattmeter: the ANAN-7000 branch**, not ours — `transmitter.c`:664-682:
  `constant1 = 5.0`, `constant2 = 0.12`, reverse `0.15` HF / `0.7` on 6 m,
  offsets fwd 32 / rev 28 **when `pa_power == PA_100W`**; the same case
  falls to ANAN-8000 values (c2 0.08, offsets 18/16) otherwise. Note our
  G2E entry uses a *live-calibrated* `c1 = 5.0` where piHPSDR's G1 branch
  says 3.3 — so the Saturn numbers are a starting point for a live
  calibration, never a shipping value.
- **⭐ PureSignal `ps_setpk = 0.6121`, not 0.2899** — `transmitter.c`:1206.
  This is the single largest per-device difference in the TX chain: the
  Saturn TX-DAC feedback peak is roughly double every other P2 radio's.
  The matching PS attenuation offset is **8.5 dB instead of 15.0**
  (`transmitter.c`:842-843). Shipping the generic 0.2899 on a G2 would
  mis-scale the feedback and "very strange things can happen" (upstream's
  own words at :1195-1201).
- **Config group** `"tx-saturn"` — per the existing rule that PA
  calibration is per radio and must never leak between models.

**Implemented 2026-08-21:** all of the above sits in `radio_tx_profile()` as
the `saturn` entry, and TX is unlocked (see §PRIORITY). Two things that entry
buys beyond the numbers themselves: `settings_save()` writes TX-cal keys into
the connected radio's config group, so `[tx-saturn]` stops a Saturn session
from overwriting `[tx]` (the live-calibrated G2E section) — and because that
group starts empty, a first connect comes up PA off, ANT1, 1 W with
pa_calibration at the conservative 53 dB default.

⛔ The one number worth repeating: **`ps_setpk` 0.6121, not 0.2899.** It is
asserted by `sdrfl-p2dev-test` precisely because nobody here can notice it
being wrong.

## 3. Bring-up gates (same ladder as P1/P2, nothing skipped)

| Step | Content | Gate |
|---|---|---|
| S1 ✅ | Device profile + picker: name the row, keep it refused until S2 passes | picker shows "Saturn/G2", still greyed |
| S2 ✅ | RX: G1+G2 conditionals, then the three headless probes | code + `sdrfl-p2dev-test` done here; `sdrfl-rxprobe` / `sdrfl-panprobe` / `sdrfl-audioprobe` **came back from W1IZZ 2026-08-22 (§7.1) — all three clean** |
| S3 ✅ | GUI: connect whitelist += SATURN, ~~Att control dropped (G4)~~, band-pass class confirmed on air | whitelist done; tuning + band relays confirmed by use (nine bands calibrated, voice QSO) — no explicit relay-click report |
| S4 🟢 | TX: `[tx-saturn]` starting at PA off + 1 W; the code half is done and gated offline, the **live half of `docs/TX-SAFETY.md` was walked by the tester** | PA cal per band vs LP-100A, wattmeter ±1–2 W, SWR tracks, voice QSO (§7.1); dry-key / SWR-trip / CW not reported |
| S5 🟡 | PureSignal with `ps_setpk = 0.6121` (the 8.5 dB offset has no counterpart here — we auto-attenuate Thetis-style instead) | PS gates from `docs/PS-SCOPE.md`, after S4 |

S4 and S5 still require the radio to be **in the room with a dummy load and an
operator watching** — that has not changed, only *who* the operator is. We
ship the code and the checklist; the person with the G2 walks it. Until he
does, the model is "unlocked, unproven" and must be described that way.

## 4. The XDMA / on-radio path — deliberately out of scope

The ANAN G2 can run piHPSDR *on its own internal Raspberry Pi*, talking to
the Saturn FPGA over PCIe instead of the LAN: `saturn_discovery()` probes
`/dev/xdma0_user` (`saturnmain.c`:338-341), gated by the "Enable Saturn
XDMA" preference (`protocols.c`:42, 156-160), and the whole P2 packet path
forks to `saturn_handle_*()` calls (np.c:530, 700, 1455, 1589, 1692).

**We do not want this.** `sdr-for-linux` is a desktop application on a
Linux workstation; the LAN P2 path is the one that matches how this
program is used, and it is the path we already have. Written down here
only so nobody re-researches it and concludes it is required. It is not.

## 5. What actually blocks this: hardware

We own an ANAN G2E. We do not own a G2, an ORION2, or anything else on
this list, and every gate from S2 down needs the physical radio.

On 2026-08-17, **W1IZZ (Larry)** confirmed in `OK1BR/sdr-for-linux#1` that
v0.4.0 fixed his monitor problem, and stated he has a Flex 6600, an ANAN
G2E, an **ANAN G2 (black face)** and an ANAN DLE 7000 MKIII, and can
"easily switch radios to test the software". Taken at face value, that
covers exactly the two families this document scopes — the G2 for §1-§3
and the DLE 7000 (ORION2-class) for the follow-on.

> ⛔ **Nothing is asked of him until there is something to test.**
> (Richard, 2026-08-20: a reaction with no substance has no value.) The
> order is: this scope → the S1/S2 code → *then* a concrete request with a
> build, a probe to run and an expected output. Not a thank-you note with
> a question mark.

## 6. Other named candidates

- **ORION2 / ANAN-7000, 8000, DLE 7000** — shares nearly every branch cited
  in §1-§2; realistically a small delta on top of a finished Saturn
  bring-up (the wattmeter split at `pa_power != PA_100W` is the main one).
- **Square SDR** — still the other candidate named in the
  `src/radio_support.h` header policy; not audited here.
- Older P1 boards (HL1, Metis, Hermes) — the P1 link exists since
  `docs/P1-SCOPE.md`, so these are a whitelist + profile question too, but
  they are old hardware with no volunteer attached.

## 7. First live confirmation on a real G2 — the exact ask

S1-S5 shipped without a single live gate, so this is what turns "should work"
into "works". Written down here so the request to whoever has the radio
carries a command and an expected output, never a question mark. **Do the RX
half first and completely** — if the receiver is not right, nothing about the
transmitter's numbers can be trusted either.

⛔ **The probes are dev-tree binaries** (`install : false` in `meson.build`) —
a release AppImage/.deb carries only the GUI. So either the tester builds from
source, or the first pass is GUI-only (which is still worth having: discovery,
picker row, tuning, band relays audible). Decide per tester; do not send a
probe name that their download does not contain.

From a source build (`meson setup build && meson compile -C build`), radio at
`<IP>`, on a band with signals — 40 m at night, 20 m by day:

```sh
# 1. discovery — must NAME the radio, not just find an address
./build/sdrfl-discover
#    expect: "Saturn/G2" ... Protocol 2 ... status 2 (idle)

# 2. RX IQ — the G1/G2 wire conditionals in one number
SDRFL_RADIO_IP=<IP> SDRFL_FREQ=7100000 SDRFL_RATE=192000 SDRFL_SECS=5   ./build/sdrfl-rxprobe
#    expect: "effective rate ~192000 Hz" and "IQ RMS" WELL above 0
#    ⛔ rate right but RMS ~0 / -90 dBFS = the link is fine and the RF path is
#       not: that is the Alex-enable ([59]=0x03) or antenna-relay class of bug,
#       exactly the "deaf RX" signature we hit on the G2E (c4b9243)

# 3. panadapter — the analyzer end to end
SDRFL_RADIO_IP=<IP> SDRFL_FREQ=7100000 RENDER_OUT=/tmp/pan.png   ./build/sdrfl-panprobe
#    expect: dBm floor around -120..-100 with peaks above it; /tmp/pan.png
#            shows a noise floor with signals, not a flat line

# 4. audio
SDRFL_RADIO_IP=<IP> SDRFL_FREQ=7100000 SDRFL_RATE=192000 SDRFL_MODE=cw   ./build/sdrfl-audioprobe
#    expect: audible signals, no dropout messages
```

Then the GUI (`./build/sdr-for-linux`): the picker row must read **"RX only"**
(not "Not supported yet"), tuning across bands must click the band-pass relays
audibly, and **no TX control may be usable** — a toast says TX has not been
brought up for this model. If a TX control CAN be operated, stop and report:
that is a whitelist bug, not a feature.

Sample rates worth one pass each: 192 k and 1536 k (the P2 maximum) — the
n_adc/DDC2 change touches the RX-specific packet both carry.

### ⛔ Then, and only then, the TX half — into a DUMMY LOAD

Same ladder we walked on the G2E, the 10E and the HL2, in this order. Nothing
here may be skipped or reordered, and the first four steps do not put a signal
on an antenna. The radio starts PA off / ANT1 / 1 W / no stored PA calibration
on purpose, so step 1 is safe by construction.

1. **Dry key.** PA still disabled in *Preferences → TX*. Key (MOX or CW) and
   confirm: the app shows TX, the RX mutes, no power is produced, unkeying
   returns to RX cleanly. This proves the T/R sequencing without RF.
   ⚠️ One unknown to watch here: our TX chain is clocked by the radio's own
   mic stream (P2 port 1026). We know the G2E sends it; nobody has checked a
   G2. If the app goes into TX but no audio/IQ flows, look in the terminal for
   `no radio mic clock` and report that line — it is a known failure mode with
   a known fix, not a mystery.
2. **1 W into the dummy load.** Enable the PA, leave drive at 1 W, key on a
   quiet part of a band the load is rated for. Expect a *small* forward
   reading and SWR near 1.0. ⚠️ The wattmeter constants are piHPSDR's
   ANAN-7000 values, never measured on this radio — treat the number as
   indicative and trust an external meter over ours.
3. **PA calibration walk-in, per band.** Raise the requested power in steps,
   comparing against an external meter, and adjust *PA calibration* for that
   band until they agree. Expect the G2's number to land in the 38.8-70 dB
   window (our G2E sits at 38.8-ish; a 100 W-class chain is the same family).
   Work up gradually — never jump to full power on the first key of a band.
4. **SWR behaviour.** With a deliberate mismatch (or simply by watching the
   reverse reading), confirm the alarm fires and drive drops to zero. If the
   protection does NOT act, stop and report — that is a bug, not a quirk.
5. **CW and voice** through the load, then on air at modest power. Listen on a
   second receiver: clean envelope, no backwave, no key clicks.
6. **PureSignal LAST**, once 1-5 are clean. It is off by default. Turn it on,
   key a two-tone or voice over and watch the PS line in the footer: the
   feedback level should settle into the 129-181 window and report
   "correcting". `ps_setpk` is preset to 0.6121 — the Saturn-specific value —
   so if the feedback looks wildly wrong, report the number rather than
   fighting it.

What to send back: which steps passed, the per-band PA-calibration numbers you
ended up with, our reading vs the external meter at 5/10/50/100 W, and
anything the app claimed that the hardware disagreed with.

### 7.1 What came back — W1IZZ, ANAN G2, 2026-08-22 (gh#3), evaluated 2026-08-23

Source-built 0.4.1 (`sdr-for-linux-0.4.1/build`), radio `Saturn/G2 device=10
(4RX) software_version=27(.46)` at 10.0.0.199, host 10.0.0.83 (Linux, iMac).
Two probe sets: first into a **dummy load**, then re-run with an **antenna on
RX1**; both at the probes' defaults (14.100 MHz, he did not set
`SDRFL_RADIO_IP`/`SDRFL_FREQ` — the probes tried 192.168.1.247 first, then
found his radio by broadcast). Then the GUI: PA calibration per band against
an **LP-100A** into the dummy load, SWR checked against external equipment,
PureSignal switched on, a DX voice QSO barefoot.

| probe | expected (§7) | dummy load | antenna | verdict |
|---|---|---|---|---|
| `sdrfl-discover` | names "Saturn/G2", P2, status 2 | ✅ `Saturn/G2 P2 dev=10 … fw=27 status=2 0.000-61.440 MHz` | same | ✅ named, idle |
| `sdrfl-rxprobe` 192 k | rate ~192000, RMS well above 0 | 193012 Hz, RMS −101.5 dBFS | 192892 Hz, RMS **−89.6 dBFS** | ✅ link fine; antenna lifts the RMS 12 dB above the dummy load. ⚠ §7's "−90 dBFS = deaf" heuristic was written from the G2E's −59 dBFS on a busy evening band — the absolute RMS depends on band activity; the panprobe floor comparison below is the proper deaf-RX test |
| `sdrfl-panprobe` 192 k, 40 frames | floor −120..−100 dBm, peaks above it | raw floor(p20) −149.4 dB, peak −137.2 (+12) | raw floor **−143.4 dB**, peak −119.1 (+24) | ✅ the antenna raises the noise floor 6 dB above the ADC floor and signals stand 24 dB above it — a −45 dB relay-class fault would leave the floor at the dummy-load value. Rough cross-check only (different analyzer set-ups — the GUI vs the probe): the G2E's GUI at 1536 k reports soffset 18.1 → raw floor −133.1, which is ≈ −142 dB at the 192 k bin width (−9 dB for 8× narrower bins) — same order as his −143.4, i.e. nothing like a 45 dB deficit |
| `sdrfl-audioprobe` 768 k USB | audible, no dropouts | peak 0.009, queued 15–21 ms, ferr=0 | peak **0.02–0.07**, queued 17–21 ms, ferr=0 | ✅ audio path runs 10 s clean; mic stream (port 1026) DETECTED at 750 pkt/s on both runs — the G2 **does** send the mic clock (§7 step 1's open question is closed) |

So the dummy-load set is usable after all: it is the "RX with nothing on the
antenna" baseline that makes the antenna set readable.

**TX (his prose + the screenshot in gh#3):** per-band PA calibration ended at
160 m 47.4 · 80 m 49.4 · 60 m 46.6 · 40 m 50.0 · **30 m 53.0 (untouched
default — he had no 30 m button, SDR-2)** · 20 m 50.2 · 17 m 49.7 · 15 m 48.2
· 12 m 44.8 · 10 m 45.4 · 6 m 43.0 dB — all inside the 38.8–70 window, below
the 53 dB start (i.e. the default under-drove, as designed), and with the
usual roll-off towards the high bands. "Power readings within 1–2 W of the
LP-100A" — the ANAN-7000 wattmeter branch (c1 5.0, c2 0.12, fwd offset 32)
holds on this radio at the levels he ran (unknown which; barefoot ≤ 100 W).
"SWR also tracked well" against his external equipment. PureSignal "appeared
to be working correctly" — no numbers. A DX contact barefoot, good signal and
audio report (voice). Screenshot: 20 m USB, Filter 2.7k, PS **lit**, S5 −99 dBm
noise floor, drive 100 W / tune 40 W, ANT 1, **Supply 0.10 V** (SDR-1).

**Defects the run surfaced, all ours, all fixed 2026-08-23:** SDR-1 (supply
readout: Saturn's supply is HP-status bytes 57-58 × 0.02553, not the G2E's
55-56 — unverified on his radio until he reads back `SDRFL_DEBUG_LEVELS=1`),
SDR-2 (no 60 m / 30 m buttons), SDR-8 (his logs carry ~5 garbage
`p2: DUC sequence errors` lines a second — Saturn bytes 32-35 are p2app FIFO
telemetry, now parsed on the G2E only). Noted, not fixed: the probes default
to our LAN IP 192.168.1.247 before falling back to broadcast (cosmetic; a
tester following §7 sets `SDRFL_RADIO_IP`).

**What the next round should ask for** (a build from `main` after
2026-08-23, or the next release): one `p2 telemetry:` line from
`SDRFL_DEBUG_LEVELS=1 ./build/sdr-for-linux` next to his PSU's voltage
(expected raw_adc0 ≈ 540 at 13.8 V → footer ≈ 13.8 V); the footer PS line
during a two-tone (feedback level, state, auto-att) so 0.6121 gets a number;
the 30 m (and 60 m) PA calibration now that the buttons exist; and a
confirmation that the `DUC sequence errors` lines are gone from his terminal.
