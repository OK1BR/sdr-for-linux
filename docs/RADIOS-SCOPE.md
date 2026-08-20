# More radios — scope for unlocking further transceivers

Companion to `docs/P2-RX-SCOPE.md` (the P2 link this rides on),
`docs/P1-SCOPE.md` (how a second radio family was brought up) and
`docs/TX-SAFETY.md` (what TX costs before it may be enabled).

**Status: SCOPED 2026-08-20 — no code written, no radio on hand.** This
document exists so the next bring-up starts from an audit instead of from
scratch, and so the hardware question can be asked with a concrete plan
attached.

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
| G4 | **No Alex attenuators.** ANAN-7000/8000 and Saturn have none — the footer Att control must go away on this class (the way the HL2 got an LNA slider instead). | Att is unconditional today | np.c:993 |
| G5 | **XVTR relay + speaker-amp mute** live in high-priority byte 1400 for ORION2/Saturn only. | not built | np.c:939-966 |
| G6 | **RX antenna encoding** — this class adds 100 to `rxant`, and the "new PA board" +1000 path must stay off. | G2E already takes the +100 path | np.c:1288-1300 |
| G7 | **TX LPF is not used on RX** for G1/ORION2/SATURN (the RX signal goes through band-pass, not the TX low-pass). | matches our G2E behaviour | np.c:1224-1232 |

None of G1-G7 is deep work. G1/G2 are one conditional each; G3 only
matters once RX2 exists; G4-G5 are GUI-level.

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

## 3. Bring-up gates (same ladder as P1/P2, nothing skipped)

| Step | Content | Gate |
|---|---|---|
| S1 | Device profile + picker: name the row, keep it refused until S2 passes | picker shows "Saturn/G2", still greyed |
| S2 | RX: G1+G2 conditionals, then the three headless probes | `sdrfl-rxprobe` / `sdrfl-panprobe` / `sdrfl-audioprobe` live on the radio |
| S3 | GUI: connect whitelist += SATURN (RX only), Att control dropped (G4), band-pass class confirmed on air | live tuning + filter relays confirmed by the operator |
| S4 | ⛔ TX: full `docs/TX-SAFETY.md` checklist into a dummy load on that physical radio, `[tx-saturn]` starting at PA off + 1 W | dry-key → 1 W → walk-in, per TX-DESIGN §7/§8 |
| S5 | PureSignal with `ps_setpk = 0.6121` + the 8.5 dB offset | PS gates from `docs/PS-SCOPE.md` |

S4 and S5 require the radio to be **in the room with a dummy load and an
operator watching**. They are not remote-testable, and no amount of
volunteer goodwill changes that.

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
