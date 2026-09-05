# SDR for Linux — backlog

The single work queue for this app: shortcomings, ideas for new features and
bugs reported from real operation. The `docs/*-SCOPE.md` / `TX-DESIGN.md` set
says what the app **is** and why it is built that way; this file says what is
**queued, in progress or just done**. When the two disagree, that is itself a
backlog item.

(Unlike the two sister apps this repo has no single `SCOPE.md` — design lives
in per-topic documents: `P2-RX-SCOPE.md`, `P1-SCOPE.md`, `P1-TX-SCOPE.md`,
`TX-DESIGN.md`, `TX-SAFETY.md`, `TCI-SCOPE.md`, `RENDERING.md`,
`RADIOS-SCOPE.md`, `PS-SCOPE.md`, `RTTY-SCOPE.md`, `AUDIO-SCOPE.md`,
`WDSP-ANALYZER-SCOPE.md`, `BANDPLAN.md`, `ENGINE-IMPORT.md`.)

## How things get in here

- **Found during a contest or live operation** — first written up in
  `docs/CONTEST-NOTES-<date>.md` (raw observation + analysis, no code touched
  while operating), then triaged into an item here.
- **Reported by someone else** — GitHub issue stays the conversation with the
  reporter; the item here mirrors it and carries the `gh#N` link, so one list
  still shows all the work.
- **Own idea / design gap** — straight in, marked `idea`.

## Item format

```
### SDR-N — one-line title
- **Type:** bug | idea | debt | task · **Severity:** high | medium | low · **Status:** open | doing | done | deferred
- **Source:** who/where/when
- **Detail:** pointer to the full write-up
Short statement of the problem and where in the code it lives.
```

Severity is about the damage, not the effort: `high` = wrong data or something
that leaves the machine wrong; `medium` = gets in the operator's way;
`low` = cosmetic or log noise.

---

## Open — tasks

### SDR-6 — Evaluate W1IZZ's G2 test results and close out the bring-up
- **Type:** task · **Severity:** high · **Status:** done (2026-08-23) — evaluated, fixes tried by Richard on the G2E, released as **v0.4.2**, and the answer posted to `gh#3` with Richard's approval (issuecomment-5388178274). Round 2 came back 2026-08-24, was evaluated in §7.2, released as **v0.5.0**, answered (issuecomment-5414226078) and **`gh#3` was closed as completed 2026-08-25** — a third round (dry-key, SWR-trip, CW, PSU reference) stays optional on his side
- **Source:** W1IZZ, GitHub `gh#3` ("G2 testing"), 2026-08-22
- **Detail:** `docs/RADIOS-SCOPE.md` (S2–S5 status table and its ⛔ note)

This is the return `RADIOS-SCOPE.md` has been waiting for. The G2 / Saturn was
unlocked for RX, TX and PureSignal on 2026-08-21 **with no G2 on hand**: every
per-device value came from piHPSDR and is pinned only by the offline gate
`sdrfl-p2dev-test`. The document says it plainly — *"Unlocked is not verified.
Until §7 comes back from a real G2, this model's wattmeter calibration, PA
calibration and PS feedback scaling are piHPSDR's starting values, not
measurements."* W1IZZ walked that path and sent the results back. Nobody has
gone through them yet.

What he supplied in `gh#3`:

- **`sdr-linux-RX probes.odt`** — the headless probe set, run into a **dummy
  load**. He then noticed the instructions expect an antenna, so this set may
  not be usable as-is; deciding that is part of this item.
- **`Probes with antenna connected.odt`** — the same probes re-run with an
  antenna on RX1.
- **Power calibration settings** and a screenshot of the running app.

What he reports in prose, each of which needs turning into a verdict rather
than being taken on trust: power calibration completed without trouble, the
readings within 1–2 W of an LP-100A on a dummy load; SWR matching external
equipment; PureSignal "appeared to be working correctly"; a DX contact made
barefoot with a good signal report and audio called good. Two things came back
as defects and are already tracked separately — **SDR-1** (supply voltage) and
**SDR-2** (no 60 m / 30 m buttons).

The work:

1. Read both probe documents against the expected output in
   `docs/P2-RX-SCOPE.md` and `docs/ENGINE-IMPORT.md` — sample count, rate and
   RMS — and decide whether the dummy-load set counts for anything.
2. Settle the ⏳ rows in the S2–S5 table: the three headless probes, live
   tuning and filter relays, the live half of `docs/TX-SAFETY.md` §7, and
   whether S5 PureSignal may come off its default-OFF.
3. Decide what his measurements say about the piHPSDR-derived constants —
   wattmeter, PA calibration, `ps_setpk` 0.6121 — and whether the ⛔ note in
   `RADIOS-SCOPE.md` can come down or must stay.
4. Then answer him in the issue, with the verdict and with whatever the next
   step is. Per the standing rule in `RADIOS-SCOPE.md` §5, a volunteer is
   contacted with a build, a probe and an expected output — never with a
   thank-you note and a question mark.

Until this is done the G2 stays "unlocked, not verified", and that wording has
to be used wherever the radio is announced.

**Evaluation (2026-08-23) — full write-up in `docs/RADIOS-SCOPE.md` §7.1:**
1. Both probe documents read against §7. All three probes clean in both sets;
   the dummy-load set IS usable — it is the no-antenna baseline: the antenna
   lifts the rxprobe RMS 12 dB (−101.5 → −89.6 dBFS) and the panprobe raw
   floor 6 dB (−149.4 → −143.4 dB) with signals 24 dB above it, so the RX
   path is through (a −45 dB relay fault would leave the floor at the
   dummy-load value). The G2 sends the mic clock (750 pkt/s). §7's "−90 dBFS
   = deaf" heuristic is retired in favour of the floor comparison.
2. S2 ✅, S3 ✅ (tuning/relays by use), S4 🟢 (PA cal per band vs LP-100A
   43–53 dB, wattmeter ±1–2 W, SWR tracks, voice QSO; dry-key/SWR-trip/CW not
   reported), S5 stays 🟡 (works by impression, no numbers) and default-OFF
   like every model.
3. Constants: the ANAN-7000 wattmeter branch holds to ±1–2 W at his levels;
   PA-cal defaults (53 dB) under-drove as designed; `ps_setpk` 0.6121 is
   "does not misbehave" only. The ⛔ note is reworded, not removed: "tested
   by one external operator, not calibrated by us".
   Defects found and fixed the same day: SDR-1, SDR-2, SDR-8. README,
   CLAUDE.md and the metainfo description updated to the new status.
4. The reply: verdict + the three fixes + a concrete next ask (one
   `SDRFL_DEBUG_LEVELS=1` telemetry line next to his PSU reading, the PS
   footer numbers during a two-tone, 30/60 m PA cal, confirmation the log
   spam is gone). **Not posted, and not before its time** (Richard,
   2026-08-23): first Richard tries the fixes on the G2E himself, then a
   release carries them, and only then is there something he can be asked
   to test — a `main` tip is not a build to hand out.

## Open — bugs

### SDR-1 — Supply voltage reads wrong on an ANAN G2
- **Type:** bug · **Severity:** high · **Status:** done (2026-08-24) — G2 mapping confirmed by W1IZZ's v0.4.2 read-back (raw 544 ≈ expected 540 at a nominal 13.8 V; he did not state his PSU's actual reading)
- **Source:** W1IZZ, GitHub `gh#3` ("G2 testing"), 2026-08-22 — answered 2026-08-23 (issuecomment-5388178274)

An external tester running an ANAN **G2** reports the supply voltage displayed
as roughly a tenth of a volt. Everything else in his run checked out (power
calibration within 1–2 W of an LP-100A, SWR matching external equipment,
PureSignal working, a DX contact made).

Likely cause, from reading the code: `SUPPLY_V_PER_COUNT` at `gui.c:402` is a
single hard-coded constant anchored empirically on **Richard's G2E**
(13.46 V measured against raw_adc1 ≈ 797.5). A different board in the family
can scale that ADC differently, and nothing in the code makes the constant
per-radio. `SDRFL_VOLT_CAL` overrides it at runtime, which is a workaround for
the tester but not a fix.

**Unverified** — no G2 hardware here; this is inference from the code and the
report, not a measurement.

**Resolution (2026-08-23):** not a scale problem but a *source-word* problem.
W1IZZ's "0.10 V" means HP-status bytes 55-56 sit at ~6 counts on a G2 — that
word is the supply only on the G2E (measured live, 13.46 V at 797.5). Both
references agree where the Saturn's supply is: piHPSDR `rx_panadapter.c`
(SATURN/ORION2: `0.02553 × ADC0`, ADC0 = bytes 57-58) and Thetis
`convertToVolts(getUserADC0())` = adc0/4095 × 5 V × 23/1.1. New
`radio_supply_profile()` in `radio_support.h` (source word + V/count per model,
pinned by `sdrfl-p2dev-test`, now 174 checks): G2E unchanged (55-56 ×
13.46/797.5); SATURN + ORION2 = 57-58 × 0.025530; every model without a
documented source (10E/Hermes class, HL2) **hides** the footer readout instead
of extrapolating — note that the ANAN 10E therefore no longer shows a Supply
number. `SDRFL_DEBUG_LEVELS` now dumps all three raw words (49-50 "supply
volts" slot, 55-56, 57-58) so a tester can settle a model against a meter.
~~**Still unverified on a real G2** — expected ≈540 counts at 13.8 V; the
confirmation belongs in the SDR-6 answer (ask W1IZZ for one
`SDRFL_DEBUG_LEVELS=1` line next to his PSU reading).~~
**Round 2 (2026-08-24, gh#3):** W1IZZ's v0.4.2 telemetry line reads
`raw_supply[49-50]=1595 raw_adc1[55-56]=12 raw_adc0[57-58]=544` — 544 ×
0.02553 = 13.89 V, within the expected ≈540 @ 13.8 V, and bytes 55-56 at 12
counts confirm the old G2E word really is dead on a Saturn. He did not write
down his PSU's actual voltage, so the V/count scale is confirmed only against
the nominal expectation, not against a meter; his remaining complaint is the
readout's flicker, which is SDR-11.

### SDR-2 — No band buttons for 60 m and 30 m
- **Type:** bug · **Severity:** medium · **Status:** done (2026-08-23) — used live by W1IZZ 2026-08-24: he PA-calibrated 30 m (50.4 dB) and 60 m (50.0 dB) through the new buttons
- **Source:** W1IZZ, GitHub `gh#3`, 2026-08-22 — answered 2026-08-23 (issuecomment-5388178274)

The tester found no way to switch to 60 m or 30 m, so he could not power-
calibrate them either. **Confirmed in the code:** the band button list at
`gui.c:2963` is 160/80/40/20/17/15/12/10/6, while `bandplan.c:58` and
`bandplan.c:60` both know 60 m and 30 m, with segments and regional limits.
The bands exist everywhere except in the row of buttons.

**Resolution (2026-08-23):** the two entries added to the header-bar band list
(`{"60", 5357000}`, `{"30", 10136000}`, the `BANDS[].dflt` frequencies), nothing
else needed changing. Checked headless (broadway + isolated config): all eleven
buttons lay out, nothing clipped. Side effect worth knowing: the window's
**minimum width grows from 1618 to 1704 px** (two more 43 px buttons) — on a
1680 px-wide screen the window no longer fits unscrolled; a compact band row or
an overflow is a design call for later, not done here. Not clicked on a live
radio (the click path is the shared `on_band_clicked()`). Reviewer note for a
follow-up item: 60 m defaults to LSB by the `< 10 MHz` rule in `gui.c`, while
piHPSDR's 60 m bandstack is USB/CWU and 5357 kHz is the FT8 (USB) dial.

### SDR-3 — GtkImage baseline warnings flood stderr, non-deterministically
- **Type:** bug · **Severity:** low · **Status:** done — diagnosed, upstream GTK/Pango, no code change (2026-08-23)
- **Source:** stderr of the YO DX HF runs, 2026-08-22 / 23
- **Detail:** `docs/CONTEST-NOTES-2026-08-22.md` §N1

65 warnings in one 2.5 h run, each from a different object address — the great
majority of everything the app wrote to stderr. Nothing visibly broken. The
same warning shows up in `skimmer-for-linux` (18×).

Day two: **zero** warnings from the same binary, same GTK 4.22.4 (no package
transaction in between) and a verified-identical environment — so it is not
deterministic across runs. **Do not fix blind:** first a backtrace under `gdb`
with `G_DEBUG=fatal-warnings`, which also settles whether the cause is ours or
a GTK 4.22 regression. The full write-up lives in the skimmer's notes
(`skimmer-for-linux/docs/CONTEST-NOTES-2026-08-22.md` §N2).

**Resolution (2026-08-23, `CONTEST-NOTES-2026-08-22.md` "N1 — rozbor"):** the
numbers above were off — it is 64 warnings, all reporting a baseline of
`-2147483648` (INT_MIN, not −1), from 32 GtkImage addresses twice each, in ONE
70 ms burst mid-run; day 2 has none. That value is the signature of GTK's own
`gtk_image_measure()` (4.22.4 `gtkimage.c`: baseline = size ×
ascent/(ascent+descent) from Pango metrics — zero metrics → NaN → INT_MIN,
caught by `gtksizerequest.c`, clamped, nothing visible). Reproduced
byte-identically with an empty fontset, both in a standalone program and in
our binary; a control run with normal fonts and the same dialogs emits zero;
our CSS cannot cause it (nonexistent family substitutes, only `font-size: 0`
zeroes metrics and we never set it); we create no GtkImage ourselves; upstream
GNOME/gtk#5926 reports the same text from a stale fontconfig cache and the
code is unchanged in GTK main. Conclusion: transient font-metrics failure
inside GTK/Pango, cosmetic, not ours — no code change. Reopen recipe if it
recurs: `G_DEBUG=fatal-warnings gdb -batch -ex run -ex bt --args
./build/sdr-for-linux` for a backtrace of the first occurrence. The sister
note in skimmer-for-linux still carries the old numbers (separate repo).

### SDR-4 — `p2: DUC sequence errors: 2` at every stream start
- **Type:** bug · **Severity:** low · **Status:** done in code (2026-08-23) — hardened + instrumented, live signature pending
- **Source:** stderr of the YO DX HF runs, 2026-08-22 / 23
- **Detail:** `docs/CONTEST-NOTES-2026-08-22.md` §N2 (day-2 section)

One line, always at stream bring-up, always the same count, then never again
for the rest of the run — and identical on two independent days. Operationally
invisible, but reproducible enough to stop calling it start-up noise. Worth a
look at whether the first DUC sequence numbers are dropped during ramp-up, next
time the P2 TX path is open anyway.

**Resolution (2026-08-23):** the code did have a start-up hole — `gui.c` starts
the TX runtime before `p2_rx_start`, the feed thread emits the continuous zero
DUC stream from that moment into the paced-sender ring, and `p2_rx_start`
zeroed the ring's head/tail under that live producer (SPSC torn-state hazard;
pre-link packets discarded). Closed with a `txiq_live` gate in `protocol2.c`
(the emitter refuses packets — counted as "pre-link" — unless the paced sender
exists; the ring is only reset while no producer can be inside it), pinned by
a new offline gate `sdrfl-txiq-ring-test` (loopback "radio" on 127.0.0.1:1029,
41 checks, in CI). Honest caveat from the analysis: by timing the ring was
full at reset in every normal start and a full ring has no torn window, so the
race alone does NOT explain a deterministic "2 (+1)" — one discontinuity per
link start is unavoidable (first DUC packet vs the gateware's stale
`last_sequence_number`), and where the pre-fix "already 1 at first
observation" came from is live-only knowledge. So the listener now prints a
baseline once per link start (`p2: DUC sequence-error counter at link start:
N (before|after the first DUC packet)`). **Live criterion for the next run
against the G2E (TX-DESIGN §10):** at most ONE `DUC sequence errors: N (+1)`
line within ~1 s of `p2: started`, never a second one, never one later; the
baseline's N settles the origin. Not verified on the radio yet.

### SDR-7 — 60 m defaults to LSB; the band is USB/CWU
- **Type:** bug · **Severity:** low · **Status:** open
- **Source:** review of SDR-2, 2026-08-23

The per-band default mode comes from the `< 10 MHz → LSB` rule at the band
table init in `gui.c` (`band_mode` seed), so the first press of the new 60 m
button lands in LSB. piHPSDR's 60 m bandstack entries are USB/CWU
(`band.c`), and 5357 kHz is the FT8 (USB) dial. A default change alone would
not fix a config that already persists `60m=…/0/…` (0 = LSB) — decide how to
treat an already-saved LSB on 60 m (migrate once, or only change the seed).

### SDR-8 — "p2: DUC sequence errors" spam on the ANAN G2 (bytes 32-35 are not a counter there)
- **Type:** bug · **Severity:** low · **Status:** done (2026-08-23) — confirmed live on the G2 by W1IZZ 2026-08-24 ("Sequence errors are gone")
- **Source:** W1IZZ's probe logs in `gh#3` (both ODT sets), found while evaluating SDR-6

Every one of his probe runs printed ~5 lines a second like `p2: DUC sequence
errors: 570431232 (+553588224)` — 32-bit garbage, changing every status
packet. Root cause, from the primary source (laurencebarker/Saturn
`sw_projects/P2_app/OutHighPriority.c`, "protocol V4.3"): on the Saturn the
High-Priority status bytes 30-42 carry FIFO telemetry — byte 30 overflow
bits, 31-32 DDC FIFO depth, 33-34 mic FIFO depth, 35-36 DUC FIFO depth,
37-38 speaker FIFO depth, 39-42 ADC peak holds. The sequence-error counter at
bytes 32-35 is a C10 (G2E) gateware feature and piHPSDR reads none of these
bytes. **Fix:** the parser (and the SDR-4 baseline line) is enabled for
`NEW_DEVICE_G1` only; pinned by phase 6 of `sdrfl-txiq-ring-test` (a fake
HP-status packet from 127.0.0.1:1025 — G2E parses 0x01020304, Saturn ignores;
46 checks; mutation-checked: without the gate the Saturn case fails). Other P2
gateware (10E) stays unparsed until its bytes 32-35 are shown to be that
counter. Idea for later: read the Saturn's V4.3 FIFO words as real telemetry
(DUC FIFO depth + overflow bits = a better tripwire than the G2E's counter).

### SDR-9 — "tx: over stats — mic drops=…" printed after TUNE overs
- **Type:** bug · **Severity:** low · **Status:** open
- **Source:** live test of the 2026-08-23 fixes on the G2E (log `/var/tmp/sdrfl-test-2026-08-23/sdr.err`)

With the mic capture open (voice mode) a TUNE over does not consume the mic
ring (TUNE is a tone), so the ring overflows and the unkey over-stats line
reports tens of thousands of "mic drops" — a number meant to flag lost speech.
Four TUNE overs in that session printed 57 856–88 320; every MOX over printed
nothing (clean). Suppress the mic counters for overs that never read the mic
(TUNE, two-tone, CW, digi), or clear the ring stats at key-down of those.

### SDR-11 — Footer supply readout: noisy last digit, and the green is unreadable in some themes
- **Type:** bug · **Severity:** low · **Status:** done (2026-08-25) — live-verified on the G2E the same day (headless broadway lab against the real radio): the value held a pixel-identical "13,5 V" in theme-foreground white across 5 screenshots over ~20 s while the raw word jittered 797/798, and 13.45 V is the worst case — it sits exactly on a 0.1 V rounding boundary. No Pango/GTK warnings from the new markup. **Light theme verified too** (same lab, `ADW_DEBUG_COLOR_SCHEME=prefer-light` against the live radio): the value renders near-black on the light footer, readable. Follow-up from Richard's own look (2026-08-25): the readout sat glued to the window edge → the Supply/Temp slot now carries an 8 px margin-end. Both the light theme and the margin were then confirmed by Richard on his own desktop the same day. The HL2 die-temperature half (same one-line pattern) is not live-verified.
- **Source:** W1IZZ, `gh#3` round 2 (2026-08-24: "very noisy … maybe some
  averaging and resolution to a tenth of a volt", "don't care for the green
  text"); green removal decided by Richard 2026-08-25 (in-band value should be
  ordinary foreground text like the neighbouring labels — a fixed `#8cf08c` can
  be illegible on a light theme)

Two display-only defects in the same footer slot. (1) The supply EMA was a
fixed 0.1 factor **per frame** — its time constant changed with FPS — and the
label printed hundredths, updating on every 0.01 V change, so the ±1-2-count
raw jitter (±0.03-0.05 V on a Saturn) danced in the last digit. Now the
S-meter idiom: wall-clock EMA (`SUPPLY_AVG_MS` 1500), snap on first sample or
a >1 s gap, label rounded to **0.1 V** and repainted only when that rounded
value changes. (2) The in-band state of both footer readouts (Supply, and the
HL2 die-temperature label) dropped its fixed green and now inherits the theme
foreground; amber/warn and red/fault colours stay. Protection paths are
untouched — the HL2 thermal trip reads raw telemetry in the engine
(`tx_run.c` → `p1_get_tx_meters`), never the GUI's smoothed copy.

## Open — ideas

### SDR-10 — A radio-trained neural NR in the existing NR slot (NR5)
- **Type:** idea · **Severity:** — · **Status:** deferred — a possible direction for later, not queued and not scheduled
- **Source:** own research, 2026-08-25

The NR selector already has the shape this would need. `demod_set_nr()`
(`src/engine/demod.c:458`) cycles off / ANR (LMS) / NR2 (EMNR spectral) / NR3
(RNNoise) / NR4 (libspecbleach), the button in `gui.c:2758` cycles `% 5` and
`gui.c:4720` persists it. Both neural-ish entries are vendored under `vendor/`
and credited in the about dialog. Adding an NR5 is not new plumbing; it is one
more run flag and one more processor in a chain that was designed to hold
several.

**What makes it worth considering at all.** NR3 is RNNoise — trained on human
speech in ordinary noise, which is not what comes out of an HF receiver. The
one project in amateur radio that addresses this directly is **RM Noise**,
whose stated advantage is that its network was trained on radio traffic
specifically, SSB and CW, rather than on office speech (their claim, via
oeradio.at; not measured here). That is the whole delta: same slot in the
chain, a model that has heard the signal it is being asked to clean.

**Why it cannot simply be adopted.** Checked in RM Noise's own documentation:
the client sends the receiver's audio to *their servers*, which denoise it and
send it back; it is Windows 10/11 only, there is no Linux client, the model is
server-side and not downloadable, and an account is required with the callsign
as the username. Every one of those is disqualifying here — this app does not
send what the operator is listening to anywhere, and a core RX feature cannot
depend on someone else's server being up. So the idea is not "integrate RM
Noise", it is "the thing RM Noise does is missing on Linux and nobody has
built it locally".

**The nearest local starting point** is DeepFilterNet2/3, which Intel publishes
in OpenVINO IR form and ships in its Audacity plugin set. It is still a speech
model, so it inherits exactly the mismatch above — it would be a step sideways
from NR3 unless retrained. Whether a radio-trained model could be produced at
all (corpus of off-air recordings, labelled how?) is the real question behind
this item, and it is a much bigger piece of work than wiring a processor in.

Open questions, roughly in the order they would kill or shape it:

1. **Does it damage CW?** Every model named here is a speech enhancer. A CW
   tone is not speech, and a network that has learned to preserve formants may
   attenuate or warp a steady tone. NR3/NR4 already have this exposure; a
   stronger model has more of it. Any such NR must be judged separately on
   CW and on SSB.
2. **Monitor path only.** Whatever this produces is *altered* audio — a
   generative model can invent something that was not transmitted. It may
   feed the operator's ears and never a decoder or anything that gets logged
   or spotted. That boundary is the design constraint, not a detail.
3. **Latency budget.** The chain is block-based at the demod rate; a
   frame-based network adds its own algorithmic delay on top. Break-in CW
   listening sets the ceiling, and it is low.
4. **Interaction with what is already running.** NB (ANB/SNBA), ANF and the
   existing NR stages are in the same path. A learned denoiser downstream of
   a spectral one may fight it.
5. **Where it runs.** The dev machine has an Intel NPU (Core Ultra 7 265,
   `vpu_37xx`) reachable through OpenVINO, attractive because it is a few
   watts and leaves CPU and GPU alone — but **nothing has been compiled or
   measured**, and neither the IR conversion nor operator coverage on that
   generation has been tried. CPU is the baseline. Anything NPU-specific
   stays optional: a build without it must still produce the same app, minus
   this one NR position.
6. **Vendoring and licence.** NR3/NR4 are vendored under `vendor/` with
   credits; a new model would have to fit the same pattern, and model weights
   have their own licensing that is not the same question as code licensing.

Nothing here is on a milestone and nothing about it is decided. If it is ever
picked up, the first step is offline and cheap: run an existing model over
recorded off-air audio — SSB and CW separately — and A/B it against NR2, NR3
and NR4 on the same recording, which is the same corpus discipline
`CONTRIBUTING.md` already asks of outside patches.

### SDR-12 — VFO B + split with a TCI backend (`vfo:0,1,…`, `split_enable`), for pileup click-to-TX
- **Type:** idea · **Severity:** — · **Status:** deferred (not designed; opened so the skimmer's SKM-4 has somewhere to point)
- **Source:** skimmer-for-linux `SKM-4` — Roy Andre Løntjern, LB0EI, 2026-08-29:
  in a split pileup he clicks where the DX was just listening and moves his
  **TX** frequency there while RX stays on the DX. Filed here 2026-09-05 when
  the skimmer's waterfall (M8) was started.
- **Detail:** skimmer-for-linux `docs/SCOPE.md` M8 and `docs/BACKLOG.md` SKM-4

What the skimmer needs from the radio side is a TX frequency it can address
over TCI while the RX frequency stays put — ExpertSDR's shape is VFO B on
channel 1 plus `split_enable`. Read in this tree 2026-09-05, nothing of it
exists yet:

- `src/tci_server.c`, the `vfo`/`dds` handler: `vfo:rx,ch,f` takes the
  frequency from the argument index alone and **ignores the channel** — any
  channel sets the single frequency, and the echo is always `vfo:0,0,…`.
- `split_enable`, `rit_enable/offset`, `xit_enable/offset` sit in the
  backend-less state table (accepted, stored, echoed — the compat echo layer
  from F6d-2b), so a client that sets split gets a confirming broadcast and
  no split.
- The GUI and the engine have no VFO B and no split at all (`grep -i split
  src/gui.c src/engine/*.h` finds only the drive-level and DDC senses of the
  word).

So this is a radio feature first — a second VFO in the engine/GUI (TX on B
while RX on A, or XIT as the minimal form), then the TCI backend for
`vfo:0,1,f` and `split_enable`. Until it exists the skimmer's click can only
tune the one VFO, which is what its station rows already do. Nothing is
promised; the item exists so the two backlogs point at each other.

## Deferred

### SDR-5 — ANAN 10E reports a 169.254.x.x address
- **Type:** bug · **Severity:** unknown · **Status:** deferred to 10E bring-up
- **Detail:** `docs/CONTEST-NOTES-2026-07-11.md` §10b

Left open from the July contest: the 10E is expected to get an address from the
router but announces link-local. Cannot be settled without the radio on the
bench, and the 10E bring-up is the next roadmap item anyway.

## Roadmap

Richard's directive of 2026-07-11 — radio support, in this order, each behind
the whitelist rule (a radio is blocked until it has been tested on real
hardware):

1. **ANAN 10E** — reportedly runs Protocol 2 (earlier research said the 10E
   line was P1-only; confirm from the discovery output at bring-up). P2 exists,
   so the work is Alex/HP bits for a Hermes-class board, PA tables, whitelist
   entry, live RX and TX per the TX-SAFETY checklist. Picks up **SDR-5**.
2. **Hermes Lite 2** — Protocol 1, a whole new milestone (P1 discovery + link
   + HL2 gateware differences). By far the largest piece.
3. **Square SDR** — an HL2 derivative: same P1 base, its own gateware
   deviations to map.

Both radios are physically here, so bring-up on real hardware is possible.

Ahead of all of it: **`gh#3` is an outside tester's whole run on hardware we do
not have.** That is the scarce resource in this project — SDR-6 turned it into
a verdict on 2026-08-23, SDR-1/SDR-2/SDR-8 are the defects it surfaced (all
fixed, tried by Richard on the G2E and released as v0.4.2 the same day), and
the reply is posted (issuecomment-5388178274). The
G2's status is "first live pass done by an external tester, not calibrated by
us" until his next round comes back.
