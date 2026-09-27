# RTTY mode — scope plan (zadání, 2026-08-15)

> **Status 2026-09-28: nothing is parked any more (issue #10).** What was
> deferred on 2026-08-16 was settled by operation, not by a bench pass: two
> contests keyed from the logbook's F-key macros over `rtty_macros` — SARTG
> WW RTTY 2026-08-15/16 (73 QSOs) and CQ WW RTTY 2026-09-26/27 (81 QSOs).
> The numbers below are from the run logs of 2026-09-26 (44 of those QSOs):
>
> - **Decode around our own TX — works.** 169 `KEY RTTY` overs here, 169 TX
>   holds in the skimmer (`trx` = the real keyed state, `cc470af`; skimmer
>   TX-hold, `01c72c8`), at about 80 W. Of the 90 replies that carried our
>   call, 74 had the complete call at their head. The rest lost their first
>   characters — the skimmer's acquisition after an over, tracked as
>   skimmer-for-linux #6.
> - **`RTTY_IQ_AMP` at 100 % duty — consistent with CW.** Internal
>   wattmeter, 458 readings: mean 80.9 W (66.9–84.5), SWR 1.00–1.12; CW at
>   the same drive byte (36/255) reads 72–80 W key-down. Not compared
>   against an external meter. That drive byte is a 50 W set-point at the
>   default `pa_calibration` of 53 dB — the gap to the 80 W read is
>   calibration (issue #8) and the same in both modes.
> - **SDC and `rtty` in `modulations_list` — not checked, not planned.** SDC
>   is not part of the RTTY chain; the contest ran on skimmer-for-linux and
>   log-for-linux.
> - **Dial convention — decided, the dial stays on the pair centre** (§7).

> **⛔ LIVE-CAUGHT LESSON (2026-08-15 evening, first QSO attempts): the HPSDR
> wire IQ convention is spectrally INVERTED — in BOTH directions.** The DDC
> side was long known (tci_server conjugates the RX stream for clients);
> the DUC side was invisible until now because WDSP-produced voice bakes
> the inversion in and CW (Q = 0, real envelope) is immune. Our direct FSK
> was the first asymmetric IQ on the bypass path and went out with mark
> LOW = reversed RTTY: spectrally perfect (KiwiSDR: tones at dial ±85,
> spacing 170), utterly undecodable (IC-705 read nothing; a mark-low
> slicer on the off-air recording read the RYRY test, and the 100 ms
> preamble — mark by definition — sat on dial−85). Fix: tx_run conjugates
> at the wire boundary (Q → −Q); rtty_gen stays in TRUE convention
> (gate-verified), the monitor consumes the TRUE samples. Verified live:
> the IC-705 decodes us after the fix (Richard). **Any future direct-IQ
> synthesis (PSK, future modes) MUST conjugate at the same boundary.**

Requested by Richard on 2026-08-15, mid-RTTY-contest: a first-class **RTTY
mode** in this transceiver — its own mode button with its own filter set, and
its own **FSK modulator that generates the TX signal from text arriving over
TCI, exactly the way CW does** — so that an F-key macro in `log-for-linux`
keys a complete RTTY exchange. RX **decoding stays in `skimmer-for-linux`**;
this app shows no decoded text (the division TX-DESIGN §F6d set for CW).
Implemented and offline-gated the same day; the reference sequence, file list
and implementation order were removed once done (last full version: commit
d889fde).

## 0. The generator contract (gate `sdrfl-rtty-test`)

Offline — NO radio, NO socket, NO WDSP; exit 0 = pass:

- **ITA2 encoder truth on hardcoded bit vectors** (R = 01010, Y = 10101,
  FIGS→1 = 11011→11101) — independent witnesses, not the encode tables
  round-tripping themselves (the trick the skimmer's gate uses);
- text → bit stream: **45.45 Bd carried in SAMPLE COUNTS** (the `cw_gen.h`
  contract) at both runtime IQ rates (P1 48 k, P2 192 k); 1 start + 5 data
  LSB-first + 1.5 stop; automatic FIGS/LTRS insertion consistent with
  unshift-on-space (what the skimmer's decoder assumes);
- generated IQ: **constant envelope**; per-bit instantaneous frequency
  measured from the phase slope = mark +85 Hz / space −85 Hz around 0;
  **phase-continuous across every bit edge** (bounded sample-to-sample
  phase step — an FSK click is a phase jump); amplitude ramps at
  key-on/key-off only;
- steady-**mark preamble** before the first start bit and a ~1-bit mark
  tail before unkey (the FSK convention — receivers sync on idle mark);
- abort mid-message cuts within one block and ramps down (no key click);
- the leading-space idle rule copied from `cw_gen_send_text()` (skip
  leading whitespace only when idle — TX-DESIGN §8 tripwire).

## 1. Headline findings (recon 2026-08-15)

- **WDSP has no FSK anywhere** (`vendor/wdsp/RXA.h`/`TXA.h` end at DRM) —
  but the CW TX path already proves the pattern RTTY needs: a pure
  in-house generator (`src/engine/cw_gen.c`) whose IQ goes **straight to
  the framer, WDSP bypassed** (`tx_run.c:638-682`, `I = amp·env, Q = 0`).
  RTTY TX is that pattern with a frequency toggle instead of an on/off
  envelope. No DSP vendoring question arises for TX: the modulator is an
  NCO plus bit timing — `cw_gen`-class code, not DSP.
- The skimmer sibling **spots the FSK pair CENTRE** and a spot click tunes
  the dial there (`clicked_on_spot` relay, live since 2026-08-01). The
  dial convention below keeps that loop exact to the Hz.
- **ExpertSDR3 TCI has no RTTY text command and no `rtty` modulation
  name** — the whole TCI surface is a family extension (precedent: the
  client-click relay, TCI-SCOPE "Client-originated clicks").
- `log-for-linux` macros are already mode-agnostic text expansion
  (`{CALL}`/`{NR}`/ESM untouched); only its **transport** is CW-hardcoded
  (`cw_macros:0,<text>;`). Its macro mode gate already lets RTTY through —
  which TODAY keys the text as CW. This scope turns that into a real RTTY
  transmission.

## 2. Architecture (proposed)

**Mode id.** `DEMOD_RTTY = 12` — the first id beyond the WDSP enum
(`DEMOD_NMODES` 12 → 13; ids stay sparse, mode-indexed tables in `gui.c`
grow by one; `settings.h mode_filt[128]` holds 13 modes with room). At
every WDSP boundary (`SetRXAMode` in `demod.c`, `tx_passband()` in
`tx_run.c`) the mode maps to **DIGL**. ONE mode, no RTTYU/RTTYL: transmit
is direct FSK (mark = higher RF, always); the LSB-side RX mapping lands
mark/space on the classic **2125/2295 Hz audio pair** at pitch 2210, so any
external audio-fed decoder works out of the box.

**Dial & RX passband.** The dial reads the **FSK pair CENTRE** — what the
skimmer spots, so a clicked spot lands exactly. CW-style RXA shifter
(`apply_passband()` gains an RTTY branch): pair centre → `RTTY_PITCH`
audio (default **800 Hz** = mark 715 / space 885 since the 2026-08-15 live
pass; the classic 2210 = 2125/2295 is a preference away). The "DDC centre ==
reported dds in every mode" invariant is untouched — the offset lives in
the RXA shifter like the CW BFO (TCI-SCOPE, "no IQ phase rotation"). The
GUI passband overlay stays symmetric around the dial, exactly like CW.

**Filters.** New `FILT_RTTY` family, symmetric around the dial:
2.5k, 1.5k, 1.0k, 800, 600, 500, 450, 400, 350, 300 — default **500**
(occupied bandwidth is shift 170 + keying sidebands ≈ 260 Hz minimum;
500 is the contest workhorse, 2.5k/1.5k are look-around widths). Var1/Var2
draggable edges ride the existing machinery unchanged.

**TX modulator.** New `src/engine/rtty_gen.{c,h}` under the `cw_gen`
contract verbatim (pure, offline, timing in sample counts, ⛔ NEVER keys —
it only produces IQ): text → ITA2 → 45.45 Bd bit stream →
phase-continuous NCO toggling ±85 Hz at the runtime IQ rate → interleaved
IQ into `on_tx_iq()`. Constant envelope; raised-cosine amplitude ramp at
key-on/off only; steady-mark preamble through the PTT delay + ~100 ms;
~1-bit mark tail. `RTTY_IQ_AMP` calibrated like `CW_IQ_AMP` — the P2
compensating CFIR's gain at ±85 Hz is NOT its DC gain, so 0.896 must not
be copied blindly (gate measures the generator; the wattmeter check on the
dummy-load gate confirms).

**Keying.** `tx_run` grows the RTTY twin of the CW machinery: generator
content wants TX through the **same `tx_gate`** (no new keying path), the
30 ms PTT-delay hold carries the mark preamble, TX drops at message end
after the mark tail (no hang-time dial — RTTY has no semi-break-in
tradition; Esc/stop aborts within one block), the 20 s cutoff backstop
stays. **100 % duty**: RTTY joins `digi_drive_clamp()` (`tx_digi_max`
cap) in both test sites; the digi clean-chain rule is satisfied by
construction (WDSP bypassed, mic never opens — `mode_is_voice` stays
false).

**TCI surface** (family extension; see the paragraph added to
TCI-SCOPE.md): `rtty_macros:<trx>,<text>;` with the same escaping and
leading-space word-gap semantics as `cw_macros`, and `rtty_macros_stop;`.
`modulations_list` gains `rtty` (appended LAST), `modulation:0,rtty;`
round-trips through the existing echo-back mechanism (an unsupported
client set leaves the client consistent). `TciOps` gains
`rtty_send`/`rtty_stop` **appended at the END of the struct** — the
initializer at `gui.c` is positional. No speed command: 45.45 Bd is a
constant (a future `rtty_macros_baud` is cheap if ever needed).

**UI.** RTTY button in the mode strip (`mids[]`/`mlabels[]`); free hotkey
`r`; the TX progress HUD reuses the CW HUD drawing (dispatch by mode);
AGC joins the existing digi AGC group (no 5th group — less surface).
Preferences → RTTY: pitch (advanced; default 800 since the 2026-08-15
live pass — the classic 2125/2295 fatigues the ears and decode is
IQ-side, so audio pitch is pure comfort; floor 300) + monitor level
(own trim, default −20 dBFS — added after the 2026-08-15 live pass: the
FSK monitor is a CONTINUOUS ~2.2 kHz tone, so sharing the CW sidetone
level blasted at full perceived volume; Richard's request). Shift 170
and 45.45 Bd are constants, not knobs. Persistence (mode, per-mode filter,
band stacking) rides the existing plumbing; the four mode-name converters
(`mode_from_name`, `tci_get_mode`, `tci_set_mode`, and the
`audioprobe_main.c` copy) all learn `rtty`.

## 4. log-for-linux contract (its own repo, its own task — the wire contract lives here)

- Sends `rtty_macros:0,<expanded text>;` when its TCI-reported mode is
  RTTY; `rtty_macros_stop;` on Esc/STOP. The macro bar, banks, ESM and
  placeholder expansion are untouched (already mode-agnostic).
- Its TCI mode mapping gains `rtty → RTTY` (today `digu/digl → FT8` and
  no `rtty` case, so the macro gate misroutes RTTY into the CW send).
- **Cut numbers are a CW convention — suppressed when sending RTTY**
  (today they apply on the settings flag with no mode check).
- The keyer-speed UI (PgUp/PgDn → `cw_macros_speed`) stays CW-only.

## 6. Risks & mitigations

- **`modulations_list` with `rtty`**: a third-party TCI client may reject
  the unknown name → appended last; the server already echoes
  `get_mode()` on unsupported sets. Never checked against SDC (status
  block above).
- **P2 CFIR gain at ±85 Hz** — a wrong `RTTY_IQ_AMP` is wrong power →
  measured in the gate; on the air the level matches CW at the same drive
  byte (status block above).
- **100 % duty on the PA** → the digi drive cap applies; add the RTTY
  line to the TX-SAFETY pre-flight notes.
- **Four scattered mode-name converters** + the audioprobe copy — a
  missed one is a silent wrong-mode path; prove parity with a
  `grep -ni digu` sweep
  (case-INSENSITIVE — a lowercase-only grep misses the C identifiers
  `DEMOD_DIGU/DEMOD_DIGL`, i.e. the mode-test sites like tx_run's
  is_voice, which are precisely the dangerous ones).
- Saved `[rx] mode = 12` read by an OLDER build is unvalidated
  (pre-existing gap) — accepted and noted, not fixed here.

## 7. Decisions

**Given by the assignment (Richard, 2026-08-15):** a real new mode with
its own filter set; TX = own modulator generating the signal from TCI
text the way CW does; the driving use case is a logbook macro keying a
full RTTY exchange; RX text stays out of this app (the skimmer is the
family's decoder).

**Confirmed by Richard 2026-08-15 (A–E as proposed):**
- **A.** Dial = FSK pair centre; audio pair on 2125/2295 via the
  LSB-side mapping (`RTTY_PITCH` — proposed 2210, default 800 since the
  live pass; advanced preference).
- **B.** `DEMOD_RTTY = 12`, mapped to DIGL at the WDSP boundary.
- **C.** TCI names `rtty_macros` / `rtty_macros_stop`; `rtty` appended
  to `modulations_list`.
- **D.** `FILT_RTTY` ladder as in §2, default 500.
- **E.** 45.45 Bd / 170 Hz fixed (no baud/shift UI).

**A stands — decided by Richard 2026-09-28, after two contests on it.** The
alternative was dial = MARK, the convention of Icom and RBN, which reads
85 Hz above our dial. Inside the family nothing sees that offset: the
skimmer spots the pair centre, a spot click tunes the centre, and no app of
the family takes spots from outside (log-for-linux has no cluster client).
A switch would touch three apps, the TX path among them, and change what
every stored frequency means. Should outside spots ever come in or go out,
convert the 85 Hz at that boundary — do not move the dial.
