# TX design — architecture, P2 wire map, and phased plan

Companion to [`docs/TX-SAFETY.md`](TX-SAFETY.md) (the safety **checklist** — the
acceptance criteria) and the repo `CLAUDE.md` TX-safety section. This doc is the
**how**: the byte-level Protocol-2 TX wire format, the DSP/audio subsystem we must
build, the safety model, and the phased plan that keeps RF impossible until the
last gate.

All references are to piHPSDR src at `~/.local/opt/pihpsdr/src` @ **974acba**
(`np.c` = `new_protocol.c`, `tx.c` = `transmitter.c`). Target radio: **ANAN G2E**
(`NEW_DEVICE_G1`, device id 20 — piHPSDR sources call the G2E "G1"; grep for G1
when cross-checking upstream, and do NOT confuse it with the Saturn/G2.
HERMES-class, single ADC, Protocol 2). Verified 2026-07-08 by
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
| Metering + SWR | fwd/rev from status packet, G2E calibration | `tx.c:645`, `tx.c:725` |
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

## 2. TX wire format (G2E, verified)

A keyed, in-band, PA-enabled, non-CW, non-PS transmit on the wire:

### General packet (60 B, port 1024) — `np.c:662`
| Byte | Value | Condition |
|---|---|---|
| 58 | `0x01` PA-enable | `pa_enabled && !txband->disablePA` (else 0); caches `local_pa_enable` |
| 59 | `0x01` | Alex-0 enable (G2E); `0x03` only for Orion2/Saturn |

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
- **G2E: no IQ scaling** (`do_scale = 0`) — send `drive_level` in HP[345], leave IQ
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
samples) → (G2E: unity gain) → 24-bit-BE packetiser → port 1029.

**TUNE** injects a tone via the *post* generator instead of the mic:
`SetTXAPostGenToneFreq(id,0)` (carrier at dial freq), `…ToneMag(id,0.99999)`
(full-scale), `…Mode(id,0)`, `…Run(id,1)` (`tx.c:2872`). Output power is set by
`tune_drive → drive_level → HP[345]`, **not** by the tone magnitude. TUNE needs
**no mic path** — which is exactly why it is the safest first RF.

Drive → byte: `calcLevel(d)` (`radio.c:2879`):
`target_dbm = 10·log10(d·1000) − band->pa_calibration;
volts = min(sqrt(10^(dbm/10)·0.05)/0.8, 1); level = volts·(1/0.98)·255`.
G2E uses `drive` for MOX/voice and **`tune_drive` (default 10)** for TUNE
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
  `[6/7]`. G2E calibration: `C1=3.3, C2=0.12, rC2=0.15` (`0.7` on 6 m),
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
| **F3** | fwd/rev/exciter parse (read-only) + G2E watts/SWR (`tx_meter`); `sdrfl-swr-test` | ✅ done, 8/8 (4f80abd) |
| **F4** | Safety gate `tx_gate` (in-band, PA gate, SWR/open-ant shutdown, tune-exempt); `sdrfl-txgate-test` | ✅ done, 12/12 (f11a0c4) |
| **F5** | **FIRST KEYING** via headless `sdrfl-txkey` — TUNE into a dummy load through `tx_gate` | ✅ **done, keyed live** (d3776e5) |
| **F6** | TX into the GUI app: **a)** controls+meter · **b)** cal settings · **c)** mic/SSB · **d)** CW | 🟢 **F6a+F6b done + live-keyed; F6c-1/2 (mic capture + mode-gated wiring) done, MOX enable = F6c-3** |
| **F7** | PureSignal (predistortion) — optional, later | — |

**F6a — done, live-validated on the G2E (OK1BR, 2026-07-08/09).** The GUI app keys
RF through the safety gate into a matched load; verified across 20/40 m (~8 W for a
10 W request — the known low-drive sensor under-read, F6b territory — rev 0, SWR 1.00,
no trips). What landed:
- **TX runtime** (`src/engine/tx_run.[ch]`) — a dedicated worker thread running the
  F5 keying loop (real-time IQ feed → port 1029, `tx_meter` + `tx_gate` at ~20 Hz,
  `p2_set_tx_state`). **Why a thread, not the GUI tick:** the tick is a
  `GdkFrameClock` callback and pauses when the window is occluded — the SWR/open-ant
  guard must never pause while the keepalive holds MOX. Control packets still go
  only via the engine keepalive thread (`p2_set_tx_state` just stores state — and,
  since the TX-audit fixes, kicks the keepalive awake on a state CHANGE so a
  key/unkey/drive/QSY/SWR-trip edge hits the wire immediately, piHPSDR
  `schedule_high_priority` parity, instead of ≤100 ms later); only
  the port-1029 IQ is emitted from the TX thread.
- **TX panadapter** (`src/engine/tx_analyzer.[ch]`) — a 2nd WDSP analyzer (disp 1),
  24 kHz span, mirroring piHPSDR `tx_set_analyzer`. While keyed the whole area shows
  the **transmitted** spectrum (panadapter + a TX waterfall), replacing the RX view —
  piHPSDR non-duplex behaviour. Big red power/SWR numbers, top ±kHz ruler.
- **RX↔TX audio** (piHPSDR-faithful): RX audio **muted while keyed** (`demod_set_mute`,
  ramped, no click), kept muted ~200 ms after unkey through the AGC recovery, plus a
  ~20 ms demod-input silence for the T/R crosstalk tail (piHPSDR `txrxmax` anti-pump).
  ADC-overload badge suppressed across the transition (a known transient).
- **Controls:** **TUNE** button keys via `tx_gate`; **MOX** present but disabled until
  F6c (SSB with silence ≈ no RF). Footer bar: **Drive** / **Tune** sliders (watts) +
  **antenna** ANT1/2/3. **Preferences → Radio → Transmit:** PA-enable (persistent,
  mirrors piHPSDR) + SWR alarm. A refuse/trip pops TUNE back and flashes the reason.
  `pa_calibration` fixed at the validated 53 dB (per-band table = F6b).
- Also: unified the canvas font to **Adwaita Mono** (the generic Cairo "monospace"
  resolved to a serif Courier clone) to match the Adwaita Sans UI.
- **F6b — done + live-keyed on the G2E (2026-07-09).** Both piHPSDR calibration
  knobs, taken in their common/default form (Richard's call: start from the proven
  version, refine later). HF calibration confirmed accurate live; 6 m over-reads
  ~25 % (safe direction) → per-band nonlinear calibration is the next milestone:
  - **Per-band `pa_calibration` table** — `App.band_pacal[NBANDS]`, default 53 dB
    (piHPSDR's, live-validated on this G2E), **clamped to the safe [38.8, 70.0]
    range** (the 38.8 dB floor is the safety limit — a lower value raises the
    drive byte for a given watts request, band.c:571-577). Flows through
    `tx_run_cfg.pa_calibration` for the **current** band and is **re-pushed on
    every band change** (`band_apply` → `tx_push_cfg`) so the drive byte tracks
    the band's PA gain. Editable per band in Preferences → Radio → Transmit.
  - **Wattmeter correction curve** — global 11-point `pa_trim` (raw meter reading
    → true watts at 0,10,…,100 W), piHPSDR `compute_power`, applied to **both**
    fwd and rev in `tx_meter`. **Default = linear (identity)**, so out of the box
    the meter keeps our live-validated constants (`G1_C1 = 5.0`, not piHPSDR's
    3.3 — we keep the measured value and let the curve refine on top, rather than
    regress the meter). Pushed through `tx_run_cfg.pa_trim`, installed in the TX
    worker thread each meter slot (no cross-thread torn reads). Editable + "reset
    to linear" in Preferences.
  - **First-run defaults are the piHPSDR G2E values** (they are device-specific —
    piHPSDR switches them per radio in radio.c): `pa_calibration` 53 dB/band
    (band.c table; only HL2 overrides to 40.5), and `pa_trim` = identity for a
    100 W rating (the G2E is `pa_power=PA_100W`, radio.c:1308/1330 → `i*10 W`). A
    non-G2E port must switch these like piHPSDR does.
  - Both persist in `config.ini` (`[tx] pa_cal`, `[tx] pa_trim`). Offline gates
    `sdrfl-txgate-test` (15/15) and `sdrfl-swr-test` (8/8) pass; identity curve
    reproduces the pre-F6b watts (47.34 W @ raw 2000) exactly.
  - **6m added across the app** (G2E does 6 m; we'd overlooked it) — `BANDS[]`
    50–54 MHz, footer band button, per-band dB window/stacking/pa_cal. The RF
    path was already 6 m-ready (RX BPF `alex0=0x08`+preamp, TX 6 m bypass LPF,
    `tx_meter` 6 m rconstant, band-plan 6 m).
  - **Full-power TUNE** — the TUNE drive slider now spans 0–100 W like Drive (was
    capped 30 W): a wattmeter-calibration pass needs a full-range carrier. With
    that, the safety gate was tightened (docs/TX-SAFETY.md): the **open-antenna
    test is now active during TUNE too** (a full-power carrier into an open port is
    never legitimate; trips after two polls), while **high-SWR still does not trip
    during TUNE** (deliberate ATU-mismatch tuning) but now raises a warn-only flag
    (`tx_gate_result.high_swr` → amber "⚠ HIGH SWR" on the TX panadapter, in TUNE
    and MOX). New gate cases in `sdrfl-txgate-test`.
  - **Live-validated on the G2E (OK1BR, 2026-07-09).** 20 m TUNE into a dummy load,
    swept to full power (drive 41 → app 108 W), SWR ~1.05, no false trips,
    open-antenna guard silent into the matched load. **The power + SWR readout
    matched Richard's tuner wattmeter on 20 m** — i.e. the default calibration
    (`C1 = 5.0` + identity `pa_trim`) is already accurate on HF, vindicating
    keeping our measured `C1` over piHPSDR's 3.3. 6 m keys fine and the sensor
    reads (drive 51 → app ~19 W; the 6 m PA is much weaker per drive), but the app
    **over-reads ~25 % on 6 m** vs the external meter — a **safe-direction** error
    (true power lower than shown → SWR protection stays conservative).
  - **★ NEXT / known limitation — per-band, nonlinear wattmeter calibration.** The
    6 m +25 % is the ceiling of the piHPSDR model we cloned: a **single global**
    `pa_trim` curve + one forward constant for all bands. Richard's requirements
    for the proper fix (its own milestone, needs a real calibration workflow):
    the coupler response is **nonlinear** (diode detectors) and **per-band**, and
    the **reverse** sensor needs its own calibration for reflected power / SWR
    under a real mismatch (today rev is ~0 into a dummy load). Target model:
    **per-band** correction curves (`pa_trim[NBANDS][11]`) for **fwd *and* rev**,
    generalising today's global curve. Do NOT hard-code a 6 m constant — it would
    be throwaway. Needs a guided per-band calibration pass and a known-mismatch
    load for the rev curve.
- **F6c** — Mic path for SSB voice → **host soundcard (PipeWire)**, same as RX
  (Richard's call; not the radio's mic jack). Enables MOX. TUNE needs neither.
  - **F6c-1 done + live-validated** — `src/engine/mic_pw.[ch]` PipeWire capture
    (mirror of `audio_pw.c`, `PW_DIRECTION_INPUT`, mono, SPSC ring, mic_pull for
    the TX feed) + `sdrfl-micprobe` VU meter. Reads the mic live (SPL Marc One,
    peak 0.16). **`PW_STREAM_FLAG_AUTOCONNECT` did NOT reliably hit the default
    mic** — it landed on a silent node; pinning `PW_KEY_TARGET_OBJECT` to the
    source node fixes it. So `mic_start(rate, lat, target)` takes an explicit node.
  - **Device + sample-rate picker done** — the settings dialog enumerates PW
    capture sources (`mic_list_sources`) and persists the chosen node
    (`mic_device`) + shared audio rate; restart-to-apply. Note the WDSP TX input
    is fixed at 48 kHz (`tx_dsp_in_rate()`), so the mic is *captured* at 48 kHz
    regardless of the RX-output rate — PipeWire resamples the device for us.
  - **F6c-2 done (offline-verified)** — live mic wired into the TX worker.
    `tx_run.c`'s feed loop now pulls `mic_pull()` and feeds it to
    `tx_dsp_feed_mic()` **only while MOX is keyed** (`keyed_mox`), padding any
    underrun with silence so the 10.667 ms real-time cadence never stalls; TUNE
    still feeds a post-gen carrier (mic muted). `mic_flush()` fires on the MOX
    key-down edge so voice starts fresh, not on the stale idle backlog.
    **Mic lifecycle is mode-gated** (Richard's call): the GUI opens the capture
    at TX-runtime start / on switching *into* a voice mode (USB/LSB/AM — future
    FM/DSB/SAM) so there's no warm-up lag, and closes it for CW/data modes (no
    "recording" while listening). `gui.c` `mode_is_voice()`/`tx_update_mic()`.
    The mic can only reach the exciter through `tx_gate`/MOX, and the MOX button
    is still disabled — so this is **dormant until F6c-3**. Ring producer drops
    on full (never blocks), so an undrained mic while MOX-off is harmless.
    Offline: builds, all TX gates pass; live mic-opens-per-mode + first voice
    are the F6c-3 test (needs the radio free).
  - **F6c-3a done (offline-verified)** — the non-hazardous GUI, MOX still
    disabled. **Mic-gain** slider on the footer next to Drive (`fmt_db`,
    −12…+40 dB, default 0; `Settings.mic_gain` persisted; live via
    `tx_run_set_mic_gain` → `SetTXAPanelGain1`). **TX level meter** top-right of
    the TX panadapter, in the RX S-meter's geometry: Mic-input-peak bar (dBFS,
    green with a red clip zone past −6 dB) + ALC-gain-reduction bar (amber) —
    Richard's "level bars now, ALC later", both from `GetTXAMeter` (`TXA_MIC_PK`
    / `TXA_ALC_GAIN`) published into `tx_run_status.mic_pk/alc_gain` from the TX
    thread. Meaningful only while keyed. Builds; all TX gates pass.
  - **F6c-3b — MOX enabled; first voice keyed — WORKS (ANAN G2E, OK1BR,
    2026-07-09).** MOX is enabled only in a voice mode (`tx_update_mic` greys it for
    CW/data alongside the mic) — same `tx_gate` path as TUNE, SWR protection ACTIVE
    for MOX. Live checks on the G2E: mic lifecycle RX-only (USB → `mic: capture open
    @ 48000 Hz … voice mode`; CWU → mic stays closed), then **first voice into a
    50 Ω dummy load on ANT1**: keyed clean (`KEY MOX PA=ON ANT1 drive=35/255`),
    SWR 1.00, and — after raising Mic gain — produced real SSB power that tracks the
    voice.
    **★ Gotcha found live:** at the default `mic_gain = 0 dB` the output was ~0 W
    (a few 0.07 W blips on peaks, `fwd_raw` 9→121). The host mic chain (SPL Marc One
    preamp) is quiet, so the WDSP TXA input sat far below full scale → the SSB IQ was
    ~-30 dB down. Same log had TUNE at drive 30 = 53 W, proving the RF path was fine
    — purely a mic-level/gain-staging issue; the footer Mic slider fixes it.
    **Still open:** audio *quality* / over-drive (needs a monitor RX — judge from the
    ALC meter + TX-panadapter splatter meanwhile), and whether to raise the default
    mic gain. Phase-1 stays plain SSB (ALC only; no speech compressor/EQ).
- **F6d** — CW (Morse) transmit. **Digital keying from an external contest/logging
  program (TCI, Richard's choice); no physical paddle, no in-app text window.**
  - **Approach = host-generated shaped carrier** (piHPSDR's CAT-CW path). The
    radio's FPGA internal keyer is PADDLE-ONLY (reads the key jack, can't be driven
    digitally), so digital CW must be host-shaped: `I = 0.896·envelope, Q = 0` on
    the TX-IQ port, MOX/drive/ANT/LPF exactly as SSB/TUNE (already verified) — WDSP
    not involved. The FPGA keyer + the P2 host-CW HP[5] mode stay OFF. Timing is
    carried in SAMPLE COUNTS (locked to the radio's IQ clock), so the rhythm is
    jitter-free at any WPM. See the memory note `cw-research-f6d`.
  - **F6d-1a done (offline-verified)** — `src/engine/cw_gen.[ch]`: a pure Morse
    envelope generator (text → sample-accurate keyed envelope with a raised-cosine
    ramp, capped inside a dot at high WPM). Gate `sdrfl-cw-test` proves the timing
    with no radio: dot = sr·1.2/WPM and the **PARIS word rate = exactly 50 dots**
    (0 sample error) at 12/20/25/30/40 WPM. NEVER keys — pure generator.
  - **F6d-1b done (offline-verified) — cw_gen wired into the TX runtime.** In a CW
    mode, a break-in state machine in the tx_run feed thread derives "want key" from
    the generator's activity (content → key; hang after the last element), the gate
    turns it into a real MOX assertion (drive, ANT, LPF, atten-31, SWR protection —
    identical to voice MOX, already verified), and the feed loop emits the shaped
    carrier IQ directly (`I = 0.896·env, Q = 0`, no WDSP). 20 s continuous-key
    cutoff. API: `tx_run_cw_send/abort/set_cw`. **Dev trigger for live tests,
    ENV-GATED:** with `SDRFL_CW_TEST=1` in the environment, **Ctrl+Shift+K** in a
    CW mode queues "V V V TEST DE OK1BR" (Esc aborts). Without the env var no
    key can key the radio (the plain-'k' hotkey was removed by the 2026-07-09
    audit); the real CW source is TCI (F6d-2). Builds; all offline gates pass (cw
    timing, txprobe OFF-state clean, txgate, swr, txdsp). **Live-verified
    2026-07-10** into a 50 Ω dummy load: first dit intact (kick + 30 ms RF hold),
    break-in feel OK, wattmeter + SWR sane.
  - **F6d-1c done — CW controls.** Preferences → Radio → "CW" group, all live +
    persisted: keyer speed (WPM), sidetone pitch (Hz), sidetone level, break-in
    hang (ms). The sidetone level is its OWN trim (dBFS before the shared
    Monitor level), default −20 dBFS ≈ piHPSDR's sidetone volume 50/127
    (0.00196·vol·env, transmitter.c:1491) — the first live test played the
    sidetone at FULL scale through the voice-calibrated monitor gain and
    audibly overdrove the speaker.
  - **F6d-2** — **TCI** server endpoint as the CW source (bootstraps a slice of the
    otherwise-deferred TCI transport); the contest program sends CW over TCI.

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

## 7. F5 live-keying results + calibration (ANAN G2E, OK1BR, 2026-07-08)

First keying done with `sdrfl-txkey` into a 50 Ω dummy load on ANT1 (20 m), piHPSDR
disconnected, operator at the wattmeter. Staged, and each keying watched:

| Step | Sent | Result |
|---|---|---|
| Dry key | PA **off**, drive 0 | T/R relay clicked (radio entered TX), **wattmeter 0** — keying mechanism proven with no RF |
| First RF | PA on, byte 5 | fwd sensor 0.12 W (wattmeter didn't resolve it) |
| Ramp | byte 20 | **wattmeter 16 W** |
| Watts path | request 10 W (pa_cal 53 → byte 16) | **wattmeter 10 W**, SWR 1.00 |

**Calibration findings (this G2E):**
- **`pa_calibration` = 53 dB (the piHPSDR default) is CORRECT here.** The watts path
  (`tx_calc_drive_byte` = calcLevel) is accurate: request 10 W → 10 W measured. The
  byte→watts curve is nonlinear at low drive (~byte³), so calcLevel is approximate.
- **Forward-power sensor was 2.3× low** with the Thetis constant `C1 = 3.3`; the fix
  is `C1 = 5.0` in `tx_meter.c` (this G2E's slow-ADC ref is 5.0 V; `(5.0/3.3)² = 2.29
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

## 8. F6c-4 — the voice chain completed (TX-path audit + live USB session, 2026-07-09/10)

A full audit of the TX path against piHPSDR @974acba (SSB focus) plus a live
USB-into-dummy-load session produced this wave. Commits 9f29479..63468b0; every
item below is offline-gated by `sdrfl-txdsp-test` (18 checks) unless noted.

**Root cause of "USB voice puts out ~no power":** the TX chain is UNITY GAIN end
to end and the WDSP ALC only attenuates (`out_targ=1.0, max_gain=1.0`), exactly
like piHPSDR — so voice PEP = (mic peak × mic gain)² × drive. A studio-staged
interface at −20 dBFS peaks → milliwatts. Verified on air: fwd_raw sat at the
sensor pedestal until the makeup gain below existed.

What landed (⚠ = regression tripwire — do not undo casually):

- **PROC (speech processor)** — `tx_dsp_set_compressor()`: WDSP COMP 0-20 dB +
  auto-leveler (attack 1 / decay 500 / top 8 dB) + CESSB above 5.5 dB, exact
  piHPSDR `tx_set_compressor` semantics. ⚠ This is the ONLY makeup gain in the
  chain. Never "fix" voice level by scaling IQ samples or drive instead.
- **Mic noise gate** — `tx_dsp_set_gate()`: WDSP TXA AMSQ, depth −20 dB
  (piHPSDR DEXP default), operator threshold (post-mic-gain dBFS). ⚠ AMSQ sits
  BEFORE the leveler in the TXA chain — that ordering is what stops the leveler
  pumping room noise up in speech gaps. Gate depth is verified offline
  (−46 dBFS tone drops exactly 20.0 dB; above-threshold passes unity).
- **Per-mode TX passband** — `tx_passband()`: ⚠ in WDSP TXA the SIGN of the
  bandpass is the ONLY sideband selector for SSB (SetTXAMode just switches
  AM/FM modulators). LSB = (−high,−low). The fixed positive F6a passband was
  transmitting USB in LSB mode. Edges are an operator setting ([tx]
  filt_lo/filt_hi, 20-500 / 1500-6000 Hz — eSSB by raising the high edge),
  applied live even while keyed (WDSP setters no-op when unchanged).
- **Keepalive kick** — `p2_set_tx_state()` signals a GCond on a real state
  change; the keepalive timer wakes and sends General + TX-spec + HP
  immediately (piHPSDR `schedule_high_priority` parity; was ≤150 ms of
  key/unkey/SWR-trip latency). ⚠ All sends STILL happen only on the timer
  thread — the kick only shortens its sleep. Never send packets from another
  thread; the single-sender invariant is what keeps protocol2.c mutex-free.
- **CW RF hold** — 30 ms of zero envelope after break-in key-on WITHOUT
  consuming the Morse queue (piHPSDR `cw_keyer_ptt_delay` default), so the MOX
  HP packet + T/R relay land before the first dit (a dot @30 WPM is 40 ms).
- **TX monitor** — voice mic / CW 700 Hz sidetone (shaped by the SAME envelope
  that keys RF) → `demod_monitor_push()`: lock-free SPSC ring, mixed into the
  sink AFTER the RX-on-TX mute gain. ⚠ demod is the only audio-sink producer;
  the monitor must go through this ring, never push to the sink directly.
- **TX meter** — Lev bar (TXA_LVLR_GAIN, the pump made visible), gate
  threshold tick + GATE state, ALC bar; Mic −6..0 dBFS is a TARGET zone (full
  PEP needs peaks at the top), not a keep-out.
- **True-PEP wattmeter** (post-§8 follow-up, 2026-07-10) — the 16-EMA coupler
  word reads the voice envelope *average*; squared into watts that's ~6 dB
  under PEP (measured: 4× vs an external PEP meter, while TUNE matched).
  `protocol2.c` now also tracks the RAW pre-EMA forward max at the ~1 kHz TX
  packet rate (piHPSDR `alex_forward_max`, np.c:2657) and the big GUI number
  shows watts from it (piHPSDR metermode 0, transmitter.c:760-766). Verified
  live: steady 1 kHz tone through the mic reads identical on the tuner's
  wattmeter; speech/hum legitimately reads above the analog needle (crest
  factor). ⚠ `p2_tx_fwd_max_take()` decays the max ×15/16 per call — exactly
  ONE consumer (the tx_run slot), a second reader doubles the decay. ⚠ SWR +
  tx_gate stay on the EMA averages (piHPSDR computes SWR *before* the PEP
  substitution) — never feed the PEP value into the gate.
- Removed the `'k'` CW test hotkey (one keypress = real RF); Esc still aborts.
- Audio prefs consolidated on the Audio page; **AF output caps at 192 kHz by
  decision** (the AF band is filter-limited; >192 k is only fatter samples —
  see the audio-rate discussion, 2026-07-10). ⚠ Don't re-add higher AF rates,
  and don't run RXA DSP above 192 k (fixed-length filters lose selectivity).
- **Digi drive cap** (2026-07-10) — DIGU/DIGL run 100 % duty (FT8 halves the
  PA's safe dissipation vs SSB peaks): `tx_digi_max_w` clamps the drive, ⚠ at
  ALL of piHPSDR's three points (every drive set incl. TCI, entering a digi
  mode, lowering the cap — radio.c:3293 / transmitter.c:2857 / tx_menu.c:253)
  PLUS a belt-and-braces clamp in tx_push_cfg so no future drive writer can
  bypass it. The STORED drive is clamped (slider shows the truth, piHPSDR
  parity). TUNE drive is deliberately NOT capped (parity again — tune is a
  bounded manual carrier). Prefs → TX "Digi max drive", persisted
  `[tx] drive_digi_max`, default 100 = uncapped.

**Contest-earned tripwires (2026-07-11, first contest deployment — ~130 CW
QSOs; details in CONTEST-NOTES-2026-07-11.md):**
- ⚠ **The TCI IQ stream keeps carrying the real signal during CW TX** (the
  tap in `on_rx_iq` sits BEFORE the mute/silence branch). CW Skimmer decoding
  the operator's own outgoing Morse is contractual behaviour (note #6) —
  never zero-feed or pause the IQ tap while keyed.
- ⚠ `spot_delete`/`spot_clear` handling is contractual (note #9) — deleting a
  call in SDC's spot list removes it from our spectrum; covered by
  `sdrfl-tci-test`.
- ⚠ The SWR 2-consecutive filter advances on GENUINE coupler readings only
  (`tx_gate_in.stale_reading`); edge-triggered gate runs re-use the last
  sample and must not count (covered by `sdrfl-txgate-test`).
- ⚠ The RX-on-TX mute is keyed off `tx_run_keyed()` in the IQ router; the GUI
  frame tick must NOT re-arm `g_rx_silence` (a late second silence window
  punches a hole into recovering RX audio) — backstop unmute only.
- ⚠ `cw_gen_send_text()` skips LEADING whitespace only when the generator is
  idle; mid-queue leading spaces are genuine word gaps between queued macros.

**Live-verified operator config (OK1BR, USB, dummy load, 2026-07-10):**
`mic_gain=11 dB, gate=1 @ −29.5 dBFS, comp(PROC)=off, filt 40-4000 Hz,
drive_w=63` — voice peaks 1-24 W on the averaged wattmeter with SWR 1.00,
gate visibly closing in speech gaps (GATE indicator).

**PROC live-verified (same day, later):** A/B mid-transmission (PROC applies
live), leveler + COMP at 7 dB — power density audibly up, monitor sound
credible ("chová se mnohem lépe než Zeus SDR"), gate threshold did NOT need
re-tuning (room noise stayed below −29.5 dBFS in speech gaps). Operator keeps
PROC on at 7 dB: `comp=1, comp_db=7`.

*Written F0, updated through F6c-4, 2026-07-10. Byte offsets cross-verified
against piHPSDR @974acba by first-hand read (MOX/`TX_RELAY`/atten/SWR) +
three-way audit, then validated by live keying into a dummy load (§7, §8).*

---

## 9. Per-radio TX profile — ANAN 10E (HERMES2) bring-up (2026-07-12)

Second TX-capable model. Two independent first-hand audits of piHPSDR
@974acba (device config + Alex/SWR paths) established that **the entire TX
wire path is device-independent between the G2E and the 10E** — MOX bit,
drive byte 345, DUC-frequency LPF knees (np.c:1244-1277, no device
conditional), ANT1/2/3 bits, `TX_RELAY` gating, atten-31-on-TX (HP 1442/1443
+ TXspec 58/59), the whole TX-specific/CW packet, General[59]=0x01, DDC0/1
topology, and the P2 PureSignal feedback mapping (DDC0 + DDC1←ADC n_adc,
np.c:1649-1667 — the `anan10E` flag is **Protocol-1 only**, it never appears
in new_protocol.c). The differences are a small per-radio profile,
`radio_tx_profile()` in `radio_support.h`:

| Item | G2E | ANAN 10E (HERMES2) | piHPSDR anchor |
|---|---|---|---|
| PA rating | 100 W | **10 W** | radio.c:1292-1296 `PA_10W` |
| Wattmeter c1/c2 | 5.0*/0.12 | 3.3/0.095 | tx.c:622-634 vs :645-662 |
| rev c2 HF / 6 m | 0.15/0.70 | 0.095/0.5 | ditto |
| fwd/rev ADC offsets | 48/42 | 6/3 | ditto |
| RX filter on TX | ANAN7000 BPF | classic HPF knees (unchanged while keyed) | np.c:1043-1210 |
| config group | `[tx]` (legacy) | `[tx-hermes2]` | — |

*\* our live-calibrated value (§7); piHPSDR says 3.3 for both.*

What the rating drives: Drive/Tune slider max, digi cap bounds, the
wattmeter-trim grid (`pa_trim[i] = i·rating/10`, meter full scale), and the
**open-antenna guard**, which is now scaled — fwd > 10 % of rating with
(fwd−rev) < 1 % of rating (the Thetis 10 W/1 W constants assumed a 100 W PA
and would never arm on a 10 W radio; G2E behaviour is bit-identical).

**⛔ TX calibration is per radio** (`Settings.tx_group`): `pa_enable`,
`antenna`, `drive_w`, `drive_digi_max`, `tune_w`, `pa_cal`, `pa_trim` live in
the connected radio's config group and never leak between models — a 10 W
calibration applied to a 100 W PA (or vice versa) yields a wildly wrong drive
byte. `settings_save` pre-loads the existing file so the other radios' groups
survive every save. A first-ever connect of a non-G2E radio starts from that
radio's SAFE defaults: **PA off, ANT1, 1 W drive/tune** — i.e. exactly the
dry-key step of the live checklist. `pa_calibration` default stays 53.0 dB
(piHPSDR's generic band table; only the P1 HL2 overrides it).

Offline gates: `sdrfl-txprobe` gained a HERMES2 section (keyed 40 m packet:
TX bits byte-identical, Hermes HPF 0x20 vs G2E BPF 0x10), `sdrfl-txgate-test`
the 10 W open-antenna scaling cases. **Live checklist PASSED 2026-07-12**
(dry key → 1 W → per-band PA cal 40 m 33.6 / 20 m 32.8 dB against the
external wattmeter, app meter agreeing → SWR 1.05 → CW break-in → TCI/SDC
skimming with a 384 kHz IQ client).

**⛔ PureSignal is LOCKED OUT on Hermes-class (`radio_ps_supported()`, G2E
only).** Live, twice: keying the 10E with PS enabled kills the radio mid-TX
("no packets from the radio for 3 s"), network stack gone (no ARP) until a
power cycle.

**Root cause (piHPSDR + Thetis audits, 2026-07-12).** Not a firmware
limitation, a *sequencing* one:
- piHPSDR reads the discovery P2-protocol-version byte (buffer[12]; 10E=31,
  G2E=44) and throws it away ("Info not yet made use of"); PS has no
  device/firmware gating at all and its P2 wire bytes match ours — piHPSDR
  would wedge a 10E over P2 exactly the same way. Its July-2023 "PURESIGNAL
  now works with ANAN-10E/100B" changelog is the **P1** `anan10E` path
  (feedback via fixed RX1/RX2 channels, no DDC sync; P1 PS toggle does a
  full protocol stop/restart because HPSDR firmwares historically hang on
  live stream reconfig — piHPSDR OZY comment).
- Thetis 2.10.3.15 **supports P2 PS on the 10E** with the byte-identical
  feedback config (enable DDC0 only, sync [1363]=0x02, DDC1←ADC1, both
  192k, n_adc=1) and hard-gates HermesII on **fw >= 10.3** ("Invalid
  Firmware! Requires 10.3 or greater") — 10.3 is exactly the P2-PS-capable
  minimum. The working difference is ORDER: at key-down Thetis sends the DDC
  reconfig (CmdRx) *before* the HP packet that raises PTT; at key-up it
  restores the RX DDC config *before* dropping PTT; rate changes outside TX
  are quiesced (DDCs disabled + 20 ms). On the 10E it also sacrifices both
  DDCs (disables both WDSP RX channels during PS TX) and forces RX2 rate =
  RX1. Our keepalive can emit the MOX HP up to 200 ms before the PS
  RX-specific → fw 10.3 switches sync mode mid-transmission and wedges; the
  G2E's 4.4-class firmware tolerates the loose ordering.

Lifting the lockout = implement the Thetis ordering (PS RX-specific before
MOX on key-down, RX restore before un-MOX on key-up, ± quiesce) and re-test
live — each failed attempt costs a power cycle. Alternative for the 10E:
PureSignal over Protocol 1 once P1 lands (HL2 milestone).
