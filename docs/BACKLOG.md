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
- **Type:** task · **Severity:** high · **Status:** open
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

## Open — bugs

### SDR-1 — Supply voltage reads wrong on an ANAN G2
- **Type:** bug · **Severity:** high · **Status:** done in code (2026-08-23) — G2 mapping unverified on hardware
- **Source:** W1IZZ, GitHub `gh#3` ("G2 testing"), 2026-08-22 — **not answered yet**

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
**Still unverified on a real G2** — expected ≈540 counts at 13.8 V; the
confirmation belongs in the SDR-6 answer (ask W1IZZ for one
`SDRFL_DEBUG_LEVELS=1` line next to his PSU reading).

### SDR-2 — No band buttons for 60 m and 30 m
- **Type:** bug · **Severity:** medium · **Status:** done (2026-08-23)
- **Source:** W1IZZ, GitHub `gh#3`, 2026-08-22 — **not answered yet**

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
- **Type:** bug · **Severity:** low · **Status:** open (diagnose first)
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
not have.** That is the scarce resource in this project — SDR-6 turns it into a
verdict, SDR-1 and SDR-2 are the two defects it already surfaced, and none of
the three has been answered since 2026-08-22. The G2 cannot be called supported
until SDR-6 is closed.
