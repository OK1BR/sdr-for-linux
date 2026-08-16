# RTTY mode — scope plan (zadání, 2026-08-15)

> **⏸ DEFERRED 2026-08-16 (Richard): everything still open below is PARKED
> until later — no RTTY contest is coming up for a long while, so there is
> no way to verify it properly. Not a candidate for the next milestone;
> pick it back up before the next RTTY contest.** The parked list, complete:
>
> - **§8 step 5 remainder (live family gate at the radio, with Richard):**
>   (a) the decode loop — skimmer decoding around our own TX needs BOTH new
>   binaries running at once (sdr `cc470af`: `trx` reports the real keyed
>   state; skimmer `01c72c8`: TX-hold) and a live check that the other
>   station's reply decodes from the 1st character after an over; at the
>   0.36 W test drive our own leak-through sat below the skimmer threshold
>   (T/R relay + 31 dB TX attenuators), so it needs real drive;
>   (b) wattmeter/duty check at 5-10 W (verifies `RTTY_IQ_AMP` at 100 % duty
>   — so far only 1 W: fwd constant, SWR 1.00);
>   (c) SDC compatibility look at `modulations_list` with `rtty` advertised.
> - **Dial convention — UNDECIDED (Richard's call):** we tune the FSK pair
>   CENTRE, the world (Icom/RBN) tunes MARK → a standing 85 Hz offset.
>   Proposal on the table: switch to dial = MARK (touches sdr + skimmer +
>   §7 A here).
> - **§8 step 6 (log-for-linux, its repo):** its steps 1-2 are done
>   (`b7a76b6`, tests 10/10); remaining = the live pass F-key → FSK at the
>   radio, and the scope's end goal — a real on-air RTTY QSO from a macro
>   during the next RTTY contest.
>
> The mode itself is usable and live-proven (first QSOs 2026-08-15; the
> IC-705 decodes us since the wire-conjugation fix 5e7bbcb).

> **STATUS 2026-08-15 (same day): §8 steps 1-4 IMPLEMENTED, offline-gated.**
> Richard confirmed §7 A-E as proposed and green-lit implementation.
> Landed: `rtty_gen.{c,h}` + `sdrfl-rtty-test` (PASS at 48 k + 192 k, in the
> CI gate list together with the previously-missing `sdrfl-cw-test`); the
> tx_run RTTY keyed source; the full mode plumbing (DEMOD_RTTY=12, FILT_RTTY,
> mode strip + `r` hotkey, HUD, AGC digi group, drive clamp, persistence,
> RTTY pitch pref); the TCI `rtty_macros` family (covered by
> `sdrfl-tci-test`, 43 checks PASS). Two corrections found in review and
> applied during implementation: (1) the **is_voice tripwire** — tx_run's
> "not CW and not digi = voice" test had to learn DEMOD_RTTY explicitly or
> mode 12 would have opened the mic path (fixed + commented in gate_slot);
> (2) the §6 parity sweep must be **case-insensitive** (`grep -ni digu`) or
> it misses the C identifiers `DEMOD_DIGU/DEMOD_DIGL` — exactly the
> dangerous sites. Sweep run clean. **Pending: §8 step 5** (live dummy-load
> family gate — skimmer decodes our own TX, wattmeter/duty check, SDC look)
> **and step 6** (log-for-linux, its own repo). (The "no RTTY on-air before
> the live gate" tripwire was lifted the same evening — Richard worked the
> contest live himself; the remainder is now parked, see the block above.)
>
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

Requested by Richard on 2026-08-15, mid-RTTY-contest, the same morning
`skimmer-for-linux` M7 (RTTY decode) went live-verified. Goal: a first-class
**RTTY mode** in this transceiver — its own mode button with its own filter
set, and its own **FSK modulator that generates the TX signal from text
arriving over TCI, exactly the way CW does today** — plus everything around
it so that an F-key macro in `log-for-linux` keys a complete RTTY exchange
on the air. RX **decoding stays in `skimmer-for-linux`** (the family's
decoder, RTTY-capable and live-proven as of today); this app shows no
decoded text, the same division TX-DESIGN §F6d set for CW ("digital keying
from an external program; no in-app text window").

## 0. Milestone gate

`sdrfl-rtty-test` (offline: NO radio, NO socket, NO WDSP; exit 0 = pass):

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
  leading whitespace only when idle — TX-DESIGN §10 tripwire).

**Live gate** (dummy load, operator present, house TX-safety rules): the
family loop — `skimmer-for-linux` in RTTY mode, fed by THIS radio's TCI IQ
stream, decodes our own transmission: text exact, measured shift 170 Hz,
reported frequency = the dial (pair centre), spectrum clean of clicks.
CI: add `sdrfl-rtty-test` to the offline gate list in
`.github/workflows/build.yml` (the CW gate's absence from that list is a
known gap — do not copy it).

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
mark/space on the classic **2125/2295 Hz audio pair**, so any external
audio-fed decoder works out of the box.

**Dial & RX passband.** The dial reads the **FSK pair CENTRE** — what the
skimmer spots, so a clicked spot lands exactly. CW-style RXA shifter
(`apply_passband()` gains an RTTY branch): pair centre → `RTTY_PITCH`
audio (default 2210 Hz = mark 2125 / space 2295). The "DDC centre ==
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

## 3. Reference sequence (one contest QSO, the whole family)

skimmer spots a CQ (pair centre, RTTY, telnet + panadapter label) →
operator clicks the label → this radio tunes dial = pair centre and
relays the click → `log-for-linux` prefills the call → operator hits F2 →
logbook expands `{CALL} 599 {NR}` and sends
`rtty_macros:0, DL1ABC 599 001;` → `tci_server` idle-dispatches to the
GTK loop → `ops.rtty_send` → `rtty_gen` queues → the feed thread sees
content → `tx_gate` keys → mark preamble → ITA2 bits at 45.45 Bd → mark
tail → unkey → back to RX. Esc in the logbook → `rtty_macros_stop;` →
ramp-down within one block.

## 4. log-for-linux contract (its own repo, its own task — the wire contract lives here)

- Sends `rtty_macros:0,<expanded text>;` when its TCI-reported mode is
  RTTY; `rtty_macros_stop;` on Esc/STOP. The macro bar, banks, ESM and
  placeholder expansion are untouched (already mode-agnostic).
- Its TCI mode mapping gains `rtty → RTTY` (today `digu/digl → FT8` and
  no `rtty` case, so the macro gate misroutes RTTY into the CW send).
- **Cut numbers are a CW convention — suppressed when sending RTTY**
  (today they apply on the settings flag with no mode check).
- The keyer-speed UI (PgUp/PgDn → `cw_macros_speed`) stays CW-only.

## 5. New files & build

| File | Role |
|---|---|
| `src/engine/rtty_gen.c` / `.h` | text → ITA2 → 45.45 Bd phase-continuous FSK IQ (cw_gen contract) |
| `src/rtty_test.c` | gate `sdrfl-rtty-test` (§0); meson target + CI gate list |
| touched | `gui.c` (mode strip, 3 converters, FILT_RTTY, HUD dispatch, drive clamp, hotkey), `audioprobe_main.c` (its converter copy), `engine/demod.c` (RTTY shift branch), `engine/tx_run.c/.h` (RTTY keyed source), `tci_server.c` (commands + advert), `docs/TCI-SCOPE.md` (extension paragraph), metainfo/README mode strings |

## 6. Risks & mitigations

- **`modulations_list` with `rtty`**: a third-party TCI client may reject
  the unknown name → appended last; the server already echoes
  `get_mode()` on unsupported sets; verify against SDC in the live pass.
- **P2 CFIR gain at ±85 Hz** — a wrong `RTTY_IQ_AMP` is wrong power →
  measured in the gate + on the wattmeter at the dummy-load gate.
- **100 % duty on the PA** → the digi drive cap applies; add the RTTY
  line to the TX-SAFETY pre-flight notes.
- **Four scattered mode-name converters** + the audioprobe copy — a
  missed one is a silent wrong-mode path; §5 lists all, the
  implementation order ends with a `grep -ni digu` sweep to prove parity
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

**Proposed here — confirm with Richard before implementation:**
- **A.** Dial = FSK pair centre; audio pair on 2125/2295 via the
  LSB-side mapping (`RTTY_PITCH` 2210, advanced preference).
- **B.** `DEMOD_RTTY = 12`, mapped to DIGL at the WDSP boundary.
- **C.** TCI names `rtty_macros` / `rtty_macros_stop`; `rtty` appended
  to `modulations_list`.
- **D.** `FILT_RTTY` ladder as in §2, default 500.
- **E.** 45.45 Bd / 170 Hz fixed (no baud/shift UI).

## 8. Implementation order (after consent)

1. `rtty_gen.{c,h}` + `sdrfl-rtty-test` offline gate (pure code, no app
   changes, CI list).
2. `tx_run`: the RTTY keyed source (preamble/tail, gate, clamp) — builds
   and passes offline.
3. Mode plumbing: demod shift branch, `FILT_RTTY`, mode strip + hotkey +
   converters + HUD + persistence.
4. `tci_server`: `rtty_macros` family + advert; TCI-SCOPE paragraph
   final.
5. Live dummy-load gate: the skimmer decodes our own TX off the TCI IQ
   tap; wattmeter/duty check; SDC compatibility look.
6. `log-for-linux` side per §4 (its own repo) → a real on-air RTTY QSO
   from a macro during the next RTTY contest.
