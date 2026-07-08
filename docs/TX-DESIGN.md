# TX design — architecture, P2 wire map, and phased plan

Companion to [`docs/TX-SAFETY.md`](TX-SAFETY.md) (the safety **checklist** — the
acceptance criteria) and the repo `CLAUDE.md` TX-safety section. This doc is the
**how**: the byte-level Protocol-2 TX wire format, the DSP/audio subsystem we must
build, the safety model, and the phased plan that keeps RF impossible until the
last gate.

All references are to piHPSDR src at `~/.local/opt/pihpsdr/src` @ **974acba**
(`np.c` = `new_protocol.c`, `tx.c` = `transmitter.c`). Target radio: **ANAN G1**
(`NEW_DEVICE_G1`, HERMES-class, single ADC, Protocol 2). Verified 2026-07-08 by
first-hand read of the four hazardous mechanisms plus a three-way cross-check of
the full TX path.

> ⛔ This document does not authorise TX. Keying the radio happens only at **F5**,
> into a dummy load, with Richard present, and only after a fresh explicit "ano"
> and the whole `TX-SAFETY.md` checklist green.

---

## 0. Current state — the engine is RX-only

`src/engine/protocol2.c` implements RX only. Three independent layers guarantee it
cannot transmit, and all three hold today:

1. **No MOX** — HP `byte[4]` carries only the run bit; `0x02` (MOX) never set.
2. **PA disabled** — General `byte[58] = 0` (memset, never written).
3. **Drive = 0** — `build_transmit_specific()` is all-zero; there is no DUC, no
   mic ingest, no WDSP TX channel.

**TX is therefore a new subsystem, not a byte tweak.** What must be built:

| Subsystem | Summary | piHPSDR anchor |
|---|---|---|
| WDSP TX channel | `OpenChannel(id,512,2048, 48k→96k→192k, type=1)` + TXA blocks | `tx.c:1307` |
| Mic→DUC IQ chain | mic 48k mono → `fexchange0` → 192k IQ (2048) → 24-bit BE | `tx.c:1572`, `np.c:2896` |
| TX-IQ UDP tx | new outbound stream, **port 1029**, sender thread + FIFO pacing | `np.h:35`, `np.c:1987` |
| TX bytes in packets | MOX, DUC phase, drive, Alex TX bits, attenuators | this doc §2 |
| Metering + SWR | fwd/rev from status packet, G1 calibration | `tx.c:645`, `tx.c:725` |
| Safety layer | in-band, PA gate, drive-0, SWR shutdown, atten-31, T/R gate | this doc §4 |
| Control + UI | MOX/TUNE/PTT, two drive sliders, antenna, CW keyer | this doc §5 |

---

## 1. Protocol-2 ports (recap)

From host (we send): General **1024**, RX-specific **1025**, TX-specific **1026**,
High-Priority **1027**, Audio-to-radio **1028**, **TX-IQ 1029**.
Radio→host: cmd-resp 1024, **HP-status 1025** (we already parse this — ADC OVL +
fwd/rev/exciter power + supply), mic/line 1026, RX-IQ 1035+.

New for TX: we must open the **1029** outbound path and start reading the fwd/rev
words already arriving on 1025.

---

## 2. TX wire format (G1, verified)

A keyed, in-band, PA-enabled, non-CW, non-PS transmit on the wire:

### General packet (60 B, port 1024) — `np.c:662`
| Byte | Value | Condition |
|---|---|---|
| 58 | `0x01` PA-enable | `pa_enabled && !txband->disablePA` (else 0); caches `local_pa_enable` |
| 59 | `0x01` | Alex-0 enable (G1); `0x03` only for Orion2/Saturn |

`local_pa_enable` is the single gate re-derived identically in the HP packet and
consumed by both HP and TX-specific attenuator logic — the packets agree by
construction.

### High-Priority packet (1444 B, port 1027) — `np.c:718`
| Byte(s) | Field | Notes |
|---|---|---|
| 4 | bit0 run, **bit1 `0x02` = MOX** | MOX set when `xmit` and (non-CW **or** tune/CAT-CW/MIDI-CW/`!cw_keyer_internal`/twotone/`hpsdr_ptt`). Plain internal-keyer CW does **not** set MOX — the FPGA keys locally (`np.c:755-776`). |
| 9-12 | DDC0 (RX1) phase | as today |
| 13-16 | DDC1 phase | RX2 only |
| **329-332** | **DUC (TX NCO) phase** | `calibrated_frequency(txfreq − lo) × 34.9525…`; `txfreq` from `vfo_get_tx_vfo()`, honours CTUN + XIT (`np.c:849-869`) |
| **345** | **drive level (0-255)** | `= drive_level` if in-band, else **0** (`np.c:896-900`) — fast off-band kill, effective even for FPGA-initiated CW |
| 1401 | OC outputs | `OCtx<<1`, `|OCtune<<1` during tune |
| **1428-1431** | **alex1** (BE) | TX-case word: always carries `TX_RELAY`(if PA), TX antenna, LPF-from-DUC |
| **1432-1435** | **alex0** (BE) | RX word + (when `xmit`) `TX_RELAY`(if PA), TX antenna, LPF-from-DUC; also RX BPF from `rxvfo` |
| **1442 / 1443** | ADC1 / ADC0 step attenuator | forced **31 / 31** when `xmit && local_pa_enable` (`np.c:1442-1445`) — protects RX ADC |

Alex bits (`alex.h`): `TX_RELAY 0x08000000` (only if `!disablePA && pa_enabled`,
alex0 gated on `xmit`, alex1 always — `np.c:1024-1032`); `TX_ANTENNA_1/2/3
0x01/02/04 000000`; TX LPF tracks **DUC** freq (`>35.6M→0x20000000 6m`,
`>24M→0x40000000 12/10`, `>16.5M→0x80000000 17/15`, `>8M→0x00100000 30/20`,
`>5M→0x00200000 60/40`, `>2.5M→0x00400000 80`, else `0x00800000 160`); RX BPF
`ALEX_ANAN7000_RX_*` from `rxvfo`. **TX antenna paranoia:** an illegal antenna
index is forced to ANT1 so one of ANT1/2/3 is always closed — never TX into an
open relay (`np.c:1371-1375`).

Our current RX builder already emits the correct alex0/alex1 (LPF + ANT1, **no**
`TX_RELAY`) and attenuator byte — the TX path adds `TX_RELAY` (gated), the DUC
phase, the drive byte, the MOX bit, and the atten-31-on-TX override.

### Transmit-specific packet (60 B, port 1026) — `np.c:1476`
| Byte | Field |
|---|---|
| 4 | number of DACs = `1` |
| 5 | CW config bitfield: `0x02` internal-keyer enable, `0x04` reversed, `0x08` mode-A / `0x28` mode-B, `0x10` sidetone, `0x40` strict spacing, `0x80` break-in |
| 6 | sidetone volume (0-127) |
| 7-8 | sidetone frequency (BE, Hz) |
| 9 | keyer speed (WPM) |
| 10 | keyer weight |
| 11-12 | hang time (BE, ms) |
| 13 | RF/PTT delay (clamped `900/speed`) |
| 17 | CW ramp width |
| 50 | mic/line/PTT config: `0x01` line-in, `0x02` mic boost, `0x04` mic-PTT-disabled, `0x08` tip/ring, `0x10` bias, `0x20` G2 XLR |
| 51 | line-in gain `(int)((gain+34)*0.6739+0.5)`, 0-31 |
| 58 / 59 | ADC1 / ADC0 attenuator during TX → **31 / 31** when `local_pa_enable` |

No MOX and no drive live here — those are HP-only.

### TX-IQ packet (1444 B, port 1029) — `np.c:1987`, `np.c:2896`
- WDSP TXA emits doubles I/Q @ 192 kHz. Convert to **signed 24-bit big-endian**:
  `is = (int)(I × 8388523.114 + 8388607.5) − 8388607` (same for Q; the
  `8388523.114` factor bakes in ~0.99999 headroom).
- 6 bytes/sample (I hi/mid/lo, Q hi/mid/lo); **240 samples = 1440 B** payload +
  4-byte BE sequence = 1444 B/packet. Ideal cadence one packet / 1250 µs, paced by
  a software FIFO estimator to avoid overrunning the FPGA TX FIFO.
- **G1: no IQ scaling** (`do_scale = 0`) — send `drive_level` in HP[345], leave IQ
  at unity gain (`radio.c:2914`).

---

## 3. TX DSP / audio subsystem

WDSP TX channel — `tx_create_transmitter()` `tx.c:938`, `OpenChannel` `tx.c:1307`:
```
OpenChannel(id, buffer=512, dsp=2048, in=48000, dsp=96000, out=192000,
            type=1 /*transmit*/, state=0, 0.010,0.025,0.0,0.010, 1);
```
Fixed post-open (`tx.c:1323-1336`): `SetTXABandpassWindow=1` (7-term BH) + run;
`SetTXACFIRRun(id, NEW_PROTOCOL)` — **P2 needs the compensating FIR**;
`SetTXAALC*` (attack 1 / decay 10) + `SetTXAALCSt(id,1)` — **ALC always on**;
`SetTXAPanelRun=1`, `SetTXAPanelSelect=2`. Default SSB passband **150–2850 Hz**.

Signal chain (voice): mic 48k mono (`I=sample, Q=0`) → DEXP (outside WDSP) →
`fexchange0(id,…)` → inside TXA: PanelGain(mic) → EQ → Leveler → CFC/COMP →
PHROT → modulator (`SetTXAMode`) → 7-term-BH bandpass → ALC → CFIR → 192k IQ (2048
samples) → (G1: unity gain) → 24-bit-BE packetiser → port 1029.

**TUNE** injects a tone via the *post* generator instead of the mic:
`SetTXAPostGenToneFreq(id,0)` (carrier at dial freq), `…ToneMag(id,0.99999)`
(full-scale), `…Mode(id,0)`, `…Run(id,1)` (`tx.c:2872`). Output power is set by
`tune_drive → drive_level → HP[345]`, **not** by the tone magnitude. TUNE needs
**no mic path** — which is exactly why it is the safest first RF.

Drive → byte: `calcLevel(d)` (`radio.c:2879`):
`target_dbm = 10·log10(d·1000) − band->pa_calibration;
volts = min(sqrt(10^(dbm/10)·0.05)/0.8, 1); level = volts·(1/0.98)·255`.
G1 uses `drive` for MOX/voice and **`tune_drive` (default 10)** for TUNE
(`radio.c:2905`). `drive_digi_max` clamps DIGU/DIGL.

---

## 4. Safety model (our policy — stricter than piHPSDR where noted)

Verified guards to port, and where we deliberately diverge:

- **PA gating** — General[58] and `TX_RELAY` gated by `pa_enabled &&
  !txband->disablePA`. T/R relay never thrown to TX with PA disabled ("safety
  belt", `np.c:1017-1032`).
- **Out-of-band → drive 0** on every HP build (`np.c:896`), fed by our band-plan
  `bp_band_for_freq` / `bp_edges`. TX refused out-of-band and on bandGen/WWV/AIR
  unless the user explicitly enables OOB TX (`band.c:683`).
- **ADC protection** — both step attenuators → 31 dB on TX with PA, in HP
  1442/1443 **and** TX-spec 58/59 (`np.c:1442`, `np.c:1579`).
- **Atomicity** — one HP datagram always carries a mutually consistent
  `{MOX, TX_RELAY, TX antenna, LPF, BPF, attenuators}`; our builder already
  regenerates the whole packet each send, so MOX can never travel with a stale
  LPF or open ANT relay. Keep it that way.
- **Watchdog armed** — General[38]=1 + HP every 100 ms (already done) → the radio
  auto-stops streaming *and* TX if the host dies. Do **not** adopt Thetis's
  watchdog-off model.
- **SWR / fwd-rev** — from status packet: fwd `[14/15]`, rev `[22/23]`, exciter
  `[6/7]`. G1 calibration: `C1=3.3, C2=0.12, rC2=0.15` (`0.7` on 6 m),
  `fwd_off=48, rev_off=42` (`tx.c:645`). `gamma=sqrt(rev/fwd)` (clamp 0.95),
  `swr = 0.7·(1+γ)/(1−γ) + 0.3·swr`, alarm default **3.0**.

### ⚠ Our SWR policy (Richard's decision, 2026-07-08) — **differs from piHPSDR**

piHPSDR ships SWR protection **OFF by default** and, when on, cuts **drive only**
(TX stays keyed). We go stricter:

- **MOX / voice:** high SWR (≥ alarm, **2 consecutive** readings — spike filter)
  → drive 0 **AND drop MOX** AND refuse re-key until the operator releases. Plus
  **Thetis open-antenna detection**: `fwd > 10 W && (fwd − rev) < 1 W` → same trip
  (catches TX into an open feedline faster than SWR alone).
- **TUNE:** SWR protection **suppressed** (`!tx->tune`, `tx.c:779`). This is not a
  loophole — tuning an ATU deliberately drives into a mismatch so the tuner can
  find a match, at the limited `tune_drive`. Without this, an ATU can never be
  tuned.

### Two separate drive controls (Richard's decision)

- **Drive** — normal MOX/voice power (persistent).
- **Tune drive** — separate, bounded, his ~10–15 W (persistent). ATU tuning needs
  real power into a deliberate mismatch. Mirrors `tx->drive` vs `tx->tune_drive`.

### Antenna (Richard's decision)

**ANT1/2/3 as a persistent setting from the start** (shared RX/TX), not hardcoded
ANT1. Feeds the TX-antenna bits in alex0/alex1 and the existing RX ANT relay.

---

## 5. Phased plan — RF impossible until F5

Each phase is independently testable offline or with the PA disabled; keying
happens only at F5.

| Phase | Content | Status (commit) |
|---|---|---|
| **F0** | This design doc + safety checklist mapping | ✅ done (a2d32df) |
| **F1** | TX bytes in the builders, **MOX/PA/drive hard-0**; offline `sdrfl-txprobe` | ✅ done, 41/41 (2eff0e9) |
| **F2** | WDSP TX channel + mic→DUC IQ chain + port-1029 framer (dormant); `sdrfl-txdsp-test` | ✅ done, 12/12 (2c36926) |
| **F3** | fwd/rev/exciter parse (read-only) + G1 watts/SWR (`tx_meter`); `sdrfl-swr-test` | ✅ done, 8/8 (4f80abd) |
| **F4** | Safety gate `tx_gate` (in-band, PA gate, SWR/open-ant shutdown, tune-exempt); `sdrfl-txgate-test` | ✅ done, 12/12 (f11a0c4) |
| **F5** | **FIRST KEYING** via headless `sdrfl-txkey` — TUNE into a dummy load through `tx_gate` | ✅ **done, keyed live** (d3776e5) |
| **F6** | TX into the GUI app: **a)** controls+meter · **b)** cal settings · **c)** mic/SSB · **d)** CW | ⏳ next (F6a first) |
| **F7** | PureSignal (predistortion) — optional, later | — |

**F6 breakdown (next; F6a chosen to start):**
- **F6a** — GUI TX controls: link tx.c/tx_meter/tx_gate into the app; **MOX + TUNE
  buttons**, **two drive sliders (Drive + Tune drive) in WATTS** (via
  `tx_calc_drive_byte`), **TX meter (fwd power + SWR)**. TUNE-first, then MOX. This
  is where the GUI app becomes TX-capable (until now only `sdrfl-txkey` can key).
- **F6b** — Settings: **per-band** `pa_calibration` + per-radio forward-power
  calibration (multi-point) in `config.ini` + the settings dialog.
- **F6c** — Mic path for SSB voice: radio mic jack (P2 mic-to-host 1026) vs host
  soundcard (PipeWire) → `tx_dsp`. TUNE needs neither.
- **F6d** — CW keyer, sidetone, break-in/QSK.

---

## 6. TX-SAFETY.md checklist → where it lands

| Checklist item (`TX-SAFETY.md`) | Phase |
|---|---|
| PA gating (General[58] + `TX_RELAY` gated, drive-0 if PA off) | F1/F4 |
| ADC protection (atten → 31 on TX) | F1/F4 |
| LPF tracks DUC freq in both alex words | F1 |
| Out-of-band lockout (band-plan fed) | F4 |
| SWR protection (2-consec, drop-MOX, not during tune) | F3/F4 |
| Open-antenna detection (Thetis) | F3/F4 |
| Per-band drive limits / PA calibration | F6 (cal live) |
| TX/RX transition muting (`txrxmax`) | F6 |
| CW rules hold for hardware-keyed CW | F6 |

---

## 7. F5 live-keying results + calibration (ANAN G1, OK1BR, 2026-07-08)

First keying done with `sdrfl-txkey` into a 50 Ω dummy load on ANT1 (20 m), piHPSDR
disconnected, operator at the wattmeter. Staged, and each keying watched:

| Step | Sent | Result |
|---|---|---|
| Dry key | PA **off**, drive 0 | T/R relay clicked (radio entered TX), **wattmeter 0** — keying mechanism proven with no RF |
| First RF | PA on, byte 5 | fwd sensor 0.12 W (wattmeter didn't resolve it) |
| Ramp | byte 20 | **wattmeter 16 W** |
| Watts path | request 10 W (pa_cal 53 → byte 16) | **wattmeter 10 W**, SWR 1.00 |

**Calibration findings (this G1):**
- **`pa_calibration` = 53 dB (the piHPSDR default) is CORRECT here.** The watts path
  (`tx_calc_drive_byte` = calcLevel) is accurate: request 10 W → 10 W measured. The
  byte→watts curve is nonlinear at low drive (~byte³), so calcLevel is approximate.
- **Forward-power sensor was 2.3× low** with the Thetis constant `C1 = 3.3`; the fix
  is `C1 = 5.0` in `tx_meter.c` (this G1's slow-ADC ref is 5.0 V; `(5.0/3.3)² = 2.29
  ≈ 2.26` measured; scales fwd+rev together so **SWR is unchanged**). Still ~18 % low
  at 10 W → refine with a multi-point per-radio calibration in **F6b**.

**Safety lessons (do not forget):**
- **Never trust the uncalibrated sensor to lower a safety margin.** The agent
  wrongly derived `pa_cal = 44` from the under-reading sensor; the auto-mode
  classifier **blocked** that keying and was right — 44 would have sent ~25 W for a
  "10 W" request. Keep `pa_cal` at the safe default and let the operator confirm any
  value that raises drive.
- **Raw drive bytes are dangerous** — rated 100 W ≈ byte 51 at `pa_cal 53`, so a
  byte near 255 ≈ 25× overdrive. Drive the PA only through `tx_calc_drive_byte`
  (watts). The `sdrfl-txkey` raw-byte ramp exists only for calibration, hard-capped.
- `pa_calibration` is **per-band** — F6b must store it per band.

---

*Written F0, updated through F5, 2026-07-08. Byte offsets cross-verified against
piHPSDR @974acba by first-hand read (MOX/`TX_RELAY`/atten/SWR) + three-way audit,
then validated by live keying into a dummy load (§7).*
