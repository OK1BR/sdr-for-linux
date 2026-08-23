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
- **Type:** bug | idea | debt · **Severity:** high | medium | low · **Status:** open | doing | done | deferred
- **Source:** who/where/when
- **Detail:** pointer to the full write-up
Short statement of the problem and where in the code it lives.
```

Severity is about the damage, not the effort: `high` = wrong data or something
that leaves the machine wrong; `medium` = gets in the operator's way;
`low` = cosmetic or log noise.

---

## Open — bugs

### SDR-1 — Supply voltage reads wrong on an ANAN G2
- **Type:** bug · **Severity:** high · **Status:** open
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

### SDR-2 — No band buttons for 60 m and 30 m
- **Type:** bug · **Severity:** medium · **Status:** open
- **Source:** W1IZZ, GitHub `gh#3`, 2026-08-22 — **not answered yet**

The tester found no way to switch to 60 m or 30 m, so he could not power-
calibrate them either. **Confirmed in the code:** the band button list at
`gui.c:2963` is 160/80/40/20/17/15/12/10/6, while `bandplan.c:58` and
`bandplan.c:60` both know 60 m and 30 m, with segments and regional limits.
The bands exist everywhere except in the row of buttons.

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
- **Type:** bug · **Severity:** low · **Status:** open
- **Source:** stderr of the YO DX HF runs, 2026-08-22 / 23
- **Detail:** `docs/CONTEST-NOTES-2026-08-22.md` §N2 (day-2 section)

One line, always at stream bring-up, always the same count, then never again
for the rest of the run — and identical on two independent days. Operationally
invisible, but reproducible enough to stop calling it start-up noise. Worth a
look at whether the first DUC sequence numbers are dropped during ramp-up, next
time the P2 TX path is open anyway.

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

Ahead of all of it: **SDR-1 and SDR-2 are an outside tester's findings on
hardware we do not have.** That is the scarce resource in this project, and the
issue has been sitting unanswered since 2026-08-22.
