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

### SDR-19 — Standard VFO A/B (swap, A=B); split removed; the card dissolved into a plain white readout
- **Type:** task · **Severity:** — · **Status:** doing — implemented 2026-09-06 (evening → night) in four live rounds with Richard on the G2E; builds clean, gates pass; final verdict pending
- **Source:** Richard, 2026-09-06 evening, after an afternoon of using SDR-12's
  split: "funkce split jak je teď se mi moc nezdá, asi bych ji spíš předělal na
  standardní funkci VFOA/VFOB … a nebo split tam necháme, ale to VFO B tam
  potřeba bude" — and on the card: "frekvenční kartu hodíme zpět ke straně, kde
  byla původně frekvence … takhle mi tam nějak překáží při provozu".

This **reverses the SDR-12 decision "no active-VFO switching"** taken the same
afternoon. The two framings in the source are not alternatives: the standard
transceiver model is two full VFOs (frequency + mode), **A/B** switches the
active one (what you hear, tune and — without split — transmit on), **A=B**
copies, **SPLIT** = transmit on the *other* VFO. SDR-12's split is exactly the
special case "A active + split on", so nothing was ripped out — B got a life
outside split.

- *Model (gui.c, "swap model"):* `app->freq`/`app->mode` stay the ACTIVE VFO
  (so every tuning/mode/band path in the file is untouched); the other VFO is
  parked in `ofreq`/`omode`; `vfo_b` is only the letter the active one carries.
  `vfo_swap()` = swap the pairs, then re-apply through the existing paths:
  `engine_set_frequency` → `band_apply` FIRST (cur_band, levels, relays,
  band-stack mode) → the VFO's own mode via the mode toggle (⛔ ordering:
  `on_mode_toggled` files the mode under `cur_band`, which only `band_apply`
  advances — the other order stores it under the old band and the next tick's
  band-stack overrides it). Refused while keyed (toast) — RX BPF and TX LPF
  would move together mid-over. `vfo_copy()` = A=B (frequency + mode).
  `tx_vfo_freq()` is unchanged in meaning: `split ? other : active` — the wire
  path and its gates (`sdrfl-p2dev-test`, `sdrfl-p1txprobe`) are untouched.
- *The one constraint that moved:* the other VFO is no longer clamped into the
  active band — a second VFO on another band is the whole point. The rule is
  now "**split requires both VFOs in the same band**": `vfo_other_set()` clamps
  only while split is on; `split_set()` re-seeds an off-band other VFO before
  switching on (default offset per mode, as before) and *says so in a toast*,
  because that overwrites a real VFO memory now; `band_apply()` drops split on
  a band change but no longer clobbers the other VFO; startup restores split
  only same-band. No cross-mode split: the other VFO's mode mirrors the active
  one while split is on (`split_set`, `on_mode_toggled`, `vfo_swap`).
- *TCI:* channel 0 = the active (RX) VFO, channel 1 = the other one, whatever
  letters they carry — so `split_enable` ("TX on channel 1") stays right for
  JTDX/tciadapter regardless of which VFO is active, and skimmer (`dds`) / log
  (`vfo:0,0`) keep reading the RX frequency. `vfo:0,1,f` with split off sets a
  free other VFO (no clamp); JTDX's order (vfo:0,1 then split_enable) lands the
  same as before. `sdrfl-tci-test` (59) unchanged — ⚠ it drives a stub ops
  table, so it says nothing about the real clamp/swap paths.
- *UI — three iterations the same evening.* (1) The card fixed top-left with
  an other-VFO row; Richard's first live look: "dobrý, tak to trochu
  rozložíme" → (2) **the card is gone**: "kartu zruš … nech frekvenci, druhý
  a třetí řádek … S-metr dej zpět do pravého rohu … řádek NR/NB atd. dej
  pryč, to je redundantní zobrazení … všechno ať je bílou barvou". So the
  readout is plain WHITE text at the pre-card place (x = 44 under the ruler):
  frequency · `Hz · VFO A · band · mode` · `MODE · lo–hi Hz (width) · AGC X`
  · **the other VFO row** `VFO B  7.025.000 · CW` (tag `TX B` under split)
  with the **A/B** and **A=B** buttons right after it on the same line — my
  proposal for his open question "jen nevím, kam dáme přepínač A/B a A=B":
  the second VFO's readout and its controls together, like a rig's display,
  and the ~1540 px strip does not grow. No box: the area tunes as before the
  card; only the filter/AGC texts and the two buttons are click targets
  (`readout_target`). S-meter back top-right via `draw_s_meter` (both paths),
  indicator row dropped (the strip buttons show the same state). Active line
  reads `VFO A` / `VFO B`; key **b** = A/B; TX card tag `TX B` / `TX A`, its
  A=B = `vfo_copy`; OVL badge right of the block; `SPLIT_MIN_TOP` = `RO_TOP +
  RO_H + 16` = 156 px (was 230). Identifiers `card_*`/`CARD_*` → `ro_*`/`RO_*`.
- *Round 4 — SPLIT REMOVED (Richard: "zruš tu funkci split" → "necháme jen
  VFO A/B"):* gone from the GUI and settings — `app->split`, the SPLIT strip
  button, key `s`, the red TX filter + TX card, Ctrl+click / Ctrl+drag /
  Ctrl+wheel on the TX VFO, the same-band clamp, re-seed + toasts,
  `tx_vfo_freq()`, `[rx] split` (retired on save), the TCI split ops (table
  entries NULL → `split_enable` echoes false, TX_FREQUENCY = channel 0; the
  generic server backend and `sdrfl-tci-test`'s stub-driven split checks
  stay). `tx_run_set_freq()` always gets the active VFO; the other VFO is a
  memory on any band. The readout's third line also lost its mode label
  ("ten údaj USB je tam redundantní" — the strip's mode buttons show it).
  Consequence named to Richard before he confirmed: LB0EI's pileup click-to-TX
  (SKM-4) has no radio side now.
- *Rounds 5-7 (same night):* the whole readout block +10 % via one factor
  `RO_S` (fonts, line pitch, buttons, `RO_H`); fonts unified with the strip —
  Adwaita Mono only on the big number (tabular digits), Adwaita Sans (`FONT_UI`)
  on lines 2-4 and the A/B, A=B buttons; **lower-sideband filters shown as
  audio Hz, low to high** ("u LSB se blbě ukazují filtry, měly by být stejně
  jako USB — absolutní hodnoty od nejnižšího do nejvyššího"): the readout's
  passband text prints `150–2850 Hz` for LSB/CWL/DIGL, and the Filter dialog
  works in a mirrored DISPLAY space for those modes — graph axis, edge
  handles/labels, drag and the Low/High spin rows all read audio Hz
  (`filt_mirrored` / `filt_disp` = its own inverse / `filt_graph_drange`);
  AM/RTTY stay signed; WDSP-facing `flo/fhi` untouched.
- *Rounds 8-11 (same night, all live-looked-at by Richard, "to je prozatím
  vše"):* readout fonts `RO_S` 1.20 / line pitch `RO_L` 1.26; S-meter sized by
  proportion at `METER_S` 1.06 (1.15 was "trochu moc"): ticks 16 px ≈ the
  strip's 14.7 px Sans + optical compensation, reading 20 px, bar 407 px — and
  its ticks + `S9+20 −53 dBm` reading were bluish-grey/greenish, now WHITE in
  Adwaita Sans (that was the "divné písmo"); the passband dash is spaced
  (`150 – 3050 Hz`); the Filter graph (fill/outline/handles/values/carrier/
  axis) and the AGC gain bar are WHITE with the UI font. Spot labels dodge
  the readout block (measured extent) and the S-meter (scaled macros) —
  verified in code, not with live spots. TX meters still Adwaita Mono (asked,
  no answer yet). ⏳ NOT committed — awaiting Richard's word.
- *Persistence (`[rx]`):* `vfo` (0/1), `vfo_other_freq`, `vfo_other_mode`;
  legacy `freq_b` is read as the fallback for `vfo_other_freq` and removed on
  the next save. Proven headless (isolated XDG, private session bus): new keys
  round-trip, a legacy-only config maps `freq_b` → `vfo_other_freq`.
- *Live checklist for Richard (G2E; ≥ 5 s between relaunches):* (1) A/B across
  bands — relays click, band levels + the other VFO's mode follow, the card
  letter flips; (2) A/B under split — the TX filter jumps to the old RX
  frequency, audio moves to the old TX frequency; (3) split on with the other
  VFO parked off-band — re-seed + toast; (4) Ctrl+click / TX-filter drag still
  move the TX VFO and stop at the band edge; (5) `s`, `b`, A=B; (6) the JTDX or
  tciadapter split pair if handy. Commit after his verdict, as on 6.9.

### SDR-18 — CTUN: the spectrum pans, the RX dial stays — the RX passband becomes a handle
- **Type:** task · **Severity:** — · **Status:** done — live-verified 2026-09-06 by Richard on the G2E ("dobrý, to funguje": the shifter sign discriminator passed)
- **Source:** Richard, 2026-09-06: "ještě by se mi hodila funkce C-TUN… myší budu dál posouvat spektrum, ale naladěná RX zůstane na místě a budu s ní hýbat stejně jako s TX při splitu"
- **Detail:** `src/gui.c` — `centre_hz()` / `centre_x()` / `vfo_x()`, `engine_set_frequency()` / `engine_set_centre()` / `engine_push_rx()` / `ctun_room()`, `in_rx_filter()`, `ctun_set()`; `demod_set_ctun_offset()` in `src/engine/demod.[ch]`; TCI `dds` / `if` / `vfo` in `tci_server.c`

*Model:* `app->freq` stays the RX dial everywhere; `app->centre` is the DDC
centre (== dial unless CTUN). The dial's offset from the centre is applied
in the WDSP RXA shifter exactly like piHPSDR's `rx_set_offset`
(receiver.c:1101: `offset = ctun_frequency − frequency`, added to the CW/RTTY
pitch term in `apply_passband()`; the passband itself stays dial-relative), so
the TCI IQ stream is still the un-shifted DDC. `engine_set_frequency()` is the
one dispatch for every tuning path: Model A → the centre follows the dial;
CTUN → the centre stays while the dial fits, a dial pushed past the span edge
drags the centre along by the overshoot (decision: push, not piHPSDR's
refusal — a wheel that keeps turning should keep tuning), a jump beyond the
span (band button, far spot) recentres. Room = DDC span (`rate`, not the zoomed
window) minus 2 % and the passband, so a shift+drag pan can never make the
next wheel notch jump the centre.

*Interaction:* plain drag pans the DDC centre (clamped so the dial's passband
stays in the span), the RX passband BODY is the dial's handle (`grab` cursor,
lit while hovered/dragged; edges still drag the Var edges — edges win), wheel
tunes the dial as before, select-click / spot click tune without the Model-A
`pan = 0` recentre, double-click on the RX body recentres the span on the
dial. **CTUN** toggle after BIN in the strip (RX-side); off → the DDC hops
back onto the dial (Model A). Persisted `[rx] ctun`; the centre starts on the
dial. VFO card sub-line shows `CTUN`; the card reads the dial through the
frame's `vfo_a_ctun_freq`.

*TCI:* the three now diverge — `dds:0` = DDC centre (what skimmer-for-linux
uses for its waterfall, verified in its tci_test), `if:0,0` = dial − centre,
`vfo:0,0` = dial (what log-for-linux reads); handshake + reporter; **IQ centre
stamps carry the centre**, not the dial. `dds` set = move the centre, the dial
keeps its IF offset (the plain reading of ExpertSDR's VFO = DDS + IF; stated,
not spec-verified); `if` set = dial at centre + off. New ops `get_centre` /
`set_centre` appended last, NULL-safe; `sdrfl-tci-test` +2.

*TX:* nothing to change — `tx_vfo_freq()` reads the dial, so MOX goes out on
the dial (or B under split); unkeyed, the P2 HP builder parks the DUC and
picks the LPF from the DDC centre and the RX BPF/HPF from the centre too,
which is piHPSDR parity (np.c:1169-1211 uses `vfo.frequency`). Two setters
(DDC retune, shifter) are not atomic — a frame of wrong pitch during a fast
pan is possible; piHPSDR has the same.

*Live:* the shifter sign cannot be checked offline; the discriminating test
(CTUN on, listen to a station, drag the spectrum — the audio must not change)
passed on the G2E the same night, Richard's verdict "dobrý, to funguje".

### SDR-17 — Draggable spectrum/waterfall divider
- **Type:** task · **Severity:** — · **Status:** done — live-verified 2026-09-06 by Richard on the G2E ("dobrý")
- **Source:** Richard, 2026-09-06 — "potřebujeme mít možnost volně hejbat s poměrem oken mezi spektrem a vodopádem, aby šlo myší chytnout to rozmezí a posunout ho"; the affordance is the cursor: "uživatel to pozná tak, že se mu tam nabídne šipečka"
- **Detail:** `src/gui.c` — `split_ph()` / `split_clamp()` / `in_split()` / `draw_split_line()`, the left-drag, motion (+ leave) and click handlers; `[display] pan_frac` in `settings.[ch]`

The split was a compile-time constant (`PANADAPTER_FRACTION 0.5`) computed
independently in four places (RX snapshot, RX fallback, TX display, the dB-axis
hit-test). Now ONE helper computes it from `app->pan_frac`, a fraction of the
area height (so a window resize keeps the proportion, as the fixed 0.5 did),
persisted as `[display] pan_frac`. Behaviour:

- **Grab zone** ±5 px around the separator line, live display only (the status
  screens have no separator). Over it the cursor is `ns-resize`; the line lights
  up (passband-edge blue) while hovered or dragged. Left-drag moves it, relative
  to the split at press (no jump on grab); the arrow stays up through a clamped
  drag until release.
- **Clamps:** spectrum ≥ `CARD_TOP + CARD_H + 16` = 230 px — the upper cairo
  node is exactly `ph + 1` tall, so a lower split would silently cut the VFO
  card; waterfall ≥ 48 px. Clamped in pixels first, then stored, so the stored
  fraction is always the one shown and can never collapse to 0.
- **Double-click the line** → back to the default 0.5.
- **Priority:** the divider wins over the zones it overlaps — the dB gutter's
  bottom rows (drag AND double-click autofit) and a passband edge crossing it.
- TX display uses the same split (draggable while keyed too).
- Fixed in passing: `on_motion`'s "nothing here" cursor was NULL, so leaving any
  hover zone in select mode dropped the crosshair; the base is now `crosshair`
  when select mode is on.

Verified: build clean (no warnings), the 11 offline gates, and a headless
broadway run with an isolated config — `pan_frac=0.3` seeded, app started with
the new code, SIGTERM, config re-read: still 0.3 (exit 0). Then the live pass
on the G2E the same evening (arrow cursor, drag, reset, restart) — Richard's OK.

### SDR-14 — Spectrum-centred control surface: the VFO card, then filter/AGC/mode controls on the spectrum
- **Type:** task · **Severity:** — · **Status:** doing — step 1 (the card) implemented 2026-09-06, live look pending; step 2 not started
- **Source:** Richard, 2026-09-06, after the SDR-2 window-width note; inspiration = SmartSDR's per-slice "flag" (the always-visible info panel at the slice's tuning line), explicitly *not* to be copied 1:1
- **Detail:** mockups `~/Downloads/sdr-mockup-vfo-karta-v4.png` (approved) — drawn onto `docs/img/main-window.png`

The direction Richard chose instead of re-flowing the top bar (an adaptive
two-row wrap of both control strips was proposed and declined — the buttons
stay as they are for now): improve the spectrum first, then move the filter
and AGC controls onto it, and only then does the top bar lose those widgets.

**Step 1 — the VFO card (done in code 2026-09-06):** one panel next to the VFO
line replaces the old top-left frequency readout and the top-right S-meter.
400 × 186 px, rounded, dark translucent with a faint blue border, its right
edge 20 px left of the VFO line (Richard: "not glued to the line" — 28 px was
too much, 20 is the approved gap), top just under the ruler; mirrors to the
right of the line when panned so far that it would leave the left edge.
Rows: the frequency (same value, font and size as the readout it replaces);
"Hz · VFO A · band · mode" as before; **mode · passband lo–hi Hz (width) ·
AGC** (the passband is what the blue edges draw — this is where the filter
values ended up after the first attempt, small labels at the foot of the
edges, was rejected as unreadable); the NR/NB/ANF/BIN indicators lit/dimmed;
the S-meter with its scale, palette fill and latched reading. The S-meter
ballistics were extracted into one tick function shared with the legacy
top-right block, which server mode (`--server`) still draws — the card is
radio mode, RX display only. Spot labels dodge the card; a click or drag that
starts inside it is swallowed (the card is not tunable area — step 2 makes it
clickable anyway). Offline: compile + the ten CI gates; the headless lab
cannot show it (no radio → "Not connected" path). **Live eyeball list:** pan
(the card follows the VFO line), zoom, CW mode (values like −250…+250),
drag a Var edge (numbers follow), toggle NR/NB/ANF/BIN, the mirror case.

**Step 2 — filter part (done in code 2026-09-06, same day):** the filter
editor is a libadwaita **dialog** in the style of Preferences (Richard: "not in
Preferences — its own icon next to the menu"), opened by a new header-bar
button left of the hamburger (our own compiled-in symbolic icon
`sdrfl-filter-symbolic`, GResource under `data/`, since the icon theme has no
bandpass glyph — ⛔ the `<file>` needs `alias=`, or the resource path grows an
extra `icons/` component and GTK shows the broken-image placeholder; caught in
the headless lab), by the `app.filter` action, and by a click on the card's
filter row. Contents: a passband graph vs the carrier (drag an edge, drag the
middle to shift, 10 Hz steps, resize/grab cursors), the mode's presets +
Var1/Var2 as grouped toggles, Low/High spin rows. One state path for every
editor (`filter_select()` / `filter_apply_var_edges()`): dropdown, spectrum
edge drag, dialog graph, toggles and spin rows all re-sync each other with
signals muted; a TCI filter set re-syncs the dialog too. Rules landed with it:
**any filter's edges drag on the spectrum** and a dragged preset becomes Var1
seeded with its edges (the preset stays untouched); the card sits on the side
AWAY from the passband (LSB family → right of the VFO line); the VFO line and
the select-mode aim line are **red** hairlines (0.75 px, 55 %, pixel-snapped)
carried down the waterfall, the amber ghost stays for the filter footprint.
Headless lab: dialog opened via `gdbus … Activate filter`, screenshot checked
(layout + icon). Live: Richard's eyeball pass in progress.

**Step 2 — AGC part + the strip (done in code 2026-09-06, same session):**
the AGC dialog in the same style (header-bar button with its own icon
`sdrfl-agc-symbolic`, the card's "AGC …" text, the `app.agc` action):
character toggles Off · Long · Slow · Med · Fast with the WDSP hang/decay
constants of the chosen one as a caption, a LIVE gain bar (new
`demod_agc_meters()`: gain = −RXA_AGC_GAIN as in the `SDRFL_DEBUG_LEVELS`
dump, out = RXA_AGC_AV; 10 Hz while open; scale 0 … AGC-T) and an AGC-T spin
row mirroring the footer slider. One path (`agc_select()` / `agc_gain_set()`)
for the dialog, the footer slider and the per-mode-group restore. Both the
Filter and the AGC dropdowns are GONE from the top bar (Richard), the strip is
a GtkCenterBox — modes left, NR…MON centred on the window, bands right — and
the window's minimum width is now set by the footer (~1540 px), not the strip.
Hover cursors: hand over the card's filter and AGC texts, resize arrow over a
passband edge. Lab-verified (dialogs opened via the actions, icons legible at
16 px after the outline icon was replaced by a solid trapezoid); Richard's live
pass the same afternoon. ⛔ The GResource has no `xml-stripblanks` on purpose:
that would need xmllint at build time (libxml2-utils is not in the CI images).

**Still open:** the mode editor (the mode row stays on the strip for now); the
footer is the next width driver (RX sliders vs TX sliders). The README
screenshot is stale (card, icons, strip); refresh it at the next release pass.

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

### SDR-15 — P2 data socket binds to the interface's link-local address
- **Type:** bug · **Severity:** medium · **Status:** open
- **Source:** Richard's desk, 2026-09-06 — both starts that day logged `p2: socket N bound to 169.254.198.250:0`

`enp134s0f1` carries a link-local 169.254.198.250/16 next to 192.168.1.18/24.
P2 discovery walks the interface addresses in `getifaddrs` order and records
one `discovered[]` entry per address the radio answered on (it answered both);
the picker de-duplicates by the radio's IP and keeps the FIRST entry, which was
the link-local one, and `p2_rx_start` binds the data socket to that entry's
`interface_address`. It works only because the G2E replies to whatever source
address it saw on the same wire. A link-local address is transient (assigned
by NetworkManager/avahi when nothing better is up) — if it disappears mid-
session the socket sits on a dead address and the stream dies. Fix: when
several discovery entries share a radio IP, prefer the one whose interface
address is in the radio's subnet (else any non-169.254 one); the next start
must log `bound to 192.168.1.18`. Sibling symptom on the same LAN: SDR-5.

### SDR-16 — `GtkGizmo (trough) reported min height -2` burst after a dry-key TUNE
- **Type:** bug · **Severity:** low · **Status:** open
- **Source:** `/tmp/sdr-for-linux-run.log`, 2026-09-06 13:45:07 — six warnings in 83 ms, one widget address, right after `tx: KEY TUNE … drive=0/255` / `UNKEY`

One GtkRange trough measured a negative minimum height for a few frames
around a TUNE over with drive 0. Cosmetic (GTK clamps it), but a slider is
being sized from something negative — find which one (a TX HUD / footer
scale that hides or changes range on key?) with `G_DEBUG=fatal-warnings gdb`
next time a dry-key TUNE is on the bench. Not seen in the following two runs.


### SDR-13 — Came up at "RX 1 Hz" after a restart, not at the last tuned frequency
- **Type:** bug · **Severity:** medium · **Status:** done (2026-09-06) — closed as **operator state**, not a defect: the persistence path was read clean, 1 Hz is the tuning clamp, and the next relaunch restored the real last frequency (see the closing note)
- **Source:** Richard's desk, 2026-09-05 21:27 — the SDR was stopped (SIGTERM, clean `p2: stopped`) and relaunched from `build/` for the capture-clock stamp (98c57de); the log's first line read `Using ANAN G2E … RX 1 Hz`
- **Detail:** this entry; the skimmer session write-up (skimmer-for-linux `docs/SCOPE.md`, M8 evening state)

Observation only, cause not yet read from the code. Some 40 minutes before the
restart a TCI client had tuned the radio to 4 Hz by mistake (then restored it
over TCI to 3 520 497 Hz); Richard tuned by hand for a long while after that,
at ~1 Hz around 21:00 and elsewhere later. On relaunch the persisted
frequency was 1 Hz. Either the TCI `vfo:` path (`tci_set_freq` →
`schedule_save`) or the GUI tuning path failed to persist the LAST value, or
the save is debounced and the final GUI tuning before SIGTERM never landed,
or a value ≤ some floor is refused on load. Read `schedule_save` / the
keyfile load in `gui.c` before guessing. Reproduce: tune over TCI, tune by
hand, SIGTERM, relaunch, compare.

**Reading 2026-09-06 (no code touched, no reproduction yet):** the persistence
path has no hole to fall through. Every tuning path — wheel, drag, click-tune,
band button, tune-step snap and the TCI `vfo:` handler `tci_set_freq` — ends in
`schedule_save` (debounced 1 s); `main()` runs an unconditional
`settings_save` after `g_application_run` returns, BEFORE `tci_server_stop` /
`p2_rx_stop`, and the 21:27 predecessor's log carries both `tci: server
stopped` and `p2: stopped (clean …)`, so the exit save ran; `app_to_settings`
copies `app->freq` verbatim; `settings_save` writes through
`g_key_file_save_to_file` (atomic temp + rename) and logs a `g_warning` on
failure — none in the log; `settings_load` takes the saved `freq` as is, no
floor, no refusal. The relaunched instance printed `RX 1 Hz` from that value.
What the code DOES do: every GUI tuning path clamps `if (nf < 1) nf = 1` — so
**1 Hz is exactly the state a drag or wheel sweep below zero lands in**, and
Richard had been sweeping the DC line at ~1 Hz around 21:00 (the waterfall
look). His last frequency before the SIGTERM is not recoverable: the skimmer's
`SKIM_WF_DEBUG` probe logs bin steps, not absolute Hz, and the SDR prints the
frequency only at start. Asked 2026-09-06, Richard does not remember and
noticed no fault. **Working hypothesis: the app faithfully restored where the
radio actually stood.** Not closed — a reading is not a measurement. Recipe,
to run at the next restart that is planned anyway (the live radio is not
stopped for this): note the frequency on screen, tune once over TCI
(`vfo:0,0,<hz>`), tune once by hand, wait > 1 s, SIGTERM, relaunch, compare the
`Using … RX <hz>` line and `[radio] freq` in `config.ini` with the last value
set. Reopen with a cause if they differ; close as operator state if they match.

**Closed 2026-09-06.** Richard's own read: the 1 Hz was a leftover of the
skimmer waterfall test that evening (sweeping the DC line), and he has seen
nothing of the kind since. The next relaunch supplied the missing data point:
the 2026-09-06 13:14 start (same `build/` binary, previous instance stopped
the evening before after hand tuning on 80 m) came up at
`Using ANAN G2E … RX 3528509 Hz` — the real last frequency — and the
debounced save was seen working in the same session (`[radio] freq` followed
his 20 m tuning within seconds). Not the full TCI+hand+SIGTERM recipe, but a
normal stop → start cycle restoring the last value, on top of the code
reading. Reopen only if a start ever lands on a frequency the operator did
not set.

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
- **Type:** bug · **Severity:** low · **Status:** done (2026-08-23) — hardened + instrumented; **live signature confirmed on the G2E 2026-09-06** (see the closing note)
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
baseline's N settles the origin. ~~Not verified on the radio yet.~~

**Live-verified 2026-09-06 (G2E, `build/` at 1f288d9, ~10 min / 300 log
lines):** `p2: DUC sequence-error counter at link start: 1 (before the first
DUC packet)`, then exactly one `p2: DUC sequence errors: 2 (+1)` in the line
right after `p2: started`, and nothing later. That settles the origin of the
old "2": the gateware counts 1 by itself at run=1, before our first packet;
our first DUC packet adds the unavoidable +1. The healthy signature in
`TX-DESIGN.md` §10 stands as written.

### SDR-7 — 60 m defaults to LSB; the band is USB/CWU
- **Type:** bug · **Severity:** low · **Status:** done (2026-09-06) — seed fixed + one-time migration, verified offline on three config shapes (see below)
- **Source:** review of SDR-2, 2026-08-23

The per-band default mode comes from the `< 10 MHz → LSB` rule at the band
table init in `gui.c` (`band_mode` seed), so the first press of the new 60 m
button lands in LSB. piHPSDR's 60 m bandstack entries are USB/CWU
(`band.c`), and 5357 kHz is the FT8 (USB) dial. A default change alone would
not fix a config that already persists `60m=…/0/…` (0 = LSB) — decide how to
treat an already-saved LSB on 60 m (migrate once, or only change the seed).

**Fix (2026-09-06, `gui.c` + `settings.[ch]`):** both — the seed AND a
one-time migration, because a seed change alone leaves every existing config
(Richard's included: `60m=…/0/5359602`) in LSB until the operator happens to
press USB there, and a blanket "LSB on 60 m is refused on load" would break
the persist-everything rule for a mode the operator may pick deliberately.
1. The `BANDS[]` table carries a per-band default mode `dmode` (160/80/40 m
   LSB, 60 m USB, 30 m and up USB — piHPSDR `band.c` seeds every 60 m
   bandstack entry `modeUSB`, our bandplan marks 5354-5366 "USB voice");
   `default_mode_for_freq()` replaces both `< 10 MHz → LSB` sites (the
   per-band seed and the startup-mode fallback). Outside any table entry the
   old by-frequency rule still applies.
2. New `[display] band_levels_v` marker (0 when absent = written before this
   build, 1 = seeded from the table; `app_to_settings` always writes 1). On
   load, `seed_lsb_migrates()`: a saved LSB on a band whose seed changed
   (`lo < 10 MHz && dmode != LSB` — only 60 m today) migrates to `dmode`
   **only while the marker is absent**, with a `settings: … (one-time
   migration)` line; the startup `[rx] mode` gets the same treatment when the
   app last closed on such a band. Once this build has saved the config the
   marker is there and a saved LSB is the operator's own and stays.
   Verified offline (broadway + isolated `XDG_CONFIG_HOME` under
   `dbus-run-session`, `SDRFL_RADIO_IP=192.0.2.1`, SIGTERM exit → save):
   (A) Richard's config copy, marker-less → line printed, `60m=…/1/…` +
   `band_levels_v=1` after exit; (B) marker present + 60 m put back to LSB →
   no line, LSB preserved; (C) marker-less, last closed on 5 359 602 Hz in
   LSB → both lines, `mode=1` + `60m=…/1/…` after exit. The real config and
   the live instance were untouched throughout. Not clicked on a live radio
   (the band button path is unchanged, only its seed value moved).

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
- **Type:** bug · **Severity:** low · **Status:** done in code (2026-09-06) — live check pending (see below)
- **Source:** live test of the 2026-08-23 fixes on the G2E (log `/var/tmp/sdrfl-test-2026-08-23/sdr.err`)

With the mic capture open (voice mode) a TUNE over does not consume the mic
ring (TUNE is a tone), so the ring overflows and the unkey over-stats line
reports tens of thousands of "mic drops" — a number meant to flag lost speech.
Four TUNE overs in that session printed 57 856–88 320; every MOX over printed
nothing (clean). Suppress the mic counters for overs that never read the mic
(TUNE, two-tone, CW, digi), or clear the ring stats at key-down of those.

**Fix (2026-09-06, `tx_run.c`):** at key-on the control thread now records
whether THIS over pulls the mic ring at all — `over_mic = mox && !gen_key &&
!s_ext_src`, the exact condition of the feed thread's live-mic branch
(`keyed_mox && !s_ext_src`). At unkey the mic counters are still read (and so
cleared) every over, but reported only when `over_mic` is set; the IQ-ring
numbers are unaffected. So: TUNE (silence feed), CW/RTTY (generators) and
TCI-fed digi (ext ring) print nothing for the mic; a voice MOX over prints
exactly what it printed before. Two-tone stays reported — it is a MOX over
and the feed does pull the ring under the generator, so its counters are
real. No offline gate compiles `tx_run.c` (radio-bound), so the change is
compile-checked + the ten CI gates only. **Live check, next time the TX
path is keyed anyway:** one TUNE over in a voice mode must print no
`tx: over stats` line; one MOX voice over must behave as before (silent when
clean, the line only on real drops/shorts).

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
- **Type:** idea · **Severity:** — · **Status:** done, then **REMOVED the same night (2026-09-06, SDR-19)** — live-verified by Richard on the G2E in the afternoon, dropped on his call in the evening ("zruš tu funkci split … necháme jen VFO A/B"); what survives is the second VFO (A/B, A=B), the engine's TX-frequency wire path + its gates, and the generic TCI server backend (the GUI passes no split ops now — `split_enable` echoes false, TX_FREQUENCY = channel 0). The pileup click-to-TX this item was filed for (LB0EI, SKM-4) is therefore not available; the implementation block below is history.
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

**Implemented 2026-09-06** (Richard: "chtěl bych funkci split… odlehčená
frekvenční karta a TX filtr… jen ještě nevím, jak budeme mezi kartami
přepínat"; decision after the reference research: **no switching at all**):

- *Model:* VFO A = RX (and TX without split), VFO B = the TX frequency under
  split. `tx_vfo_freq()` is what the TX runtime, the in-band gate, the P2
  DUC/LPF and the P1 TX NCO receive (TX-SAFETY has the wire audit).
- *No active-VFO mode to forget* (against Thetis' ClickTuneMode / SmartSDR's
  active slice): everything acts on A as before; B moves only by touching the
  **TX filter** drawn at B (drag its body, wheel over it — `grab` cursor) or
  with **Ctrl** held (Ctrl+wheel steps B, Ctrl+click = "TX here" = the LB0EI
  pileup click, turning split on if needed, Ctrl+drag drags B).
- *Display:* RED TX filter outline in the spectrum and, with the filter
  overlay, the waterfall (colour language settled the same evening: RX =
  green — passband, VFO line, VFO card, dialog graphs — TX = red; the first
  cut had Thetis' yellow and the red VFO line, "trochu pomíchané");
  CW = a carrier line, voice = the TX audio filter with the sideband's sign,
  data/RTTY = the RX passband. A **lightweight TX card** (250 × 58) at the
  foot of the spectrum beside the TX carrier, on B's side away from A (the
  VFO card owns the top; they only meet with the divider at its 230 px
  minimum): `TX`
  + frequency, `SPLIT +1.00 kHz`, buttons **A=B** and **OFF**. The VFO card's
  indicator row gained `SPLIT` (yellow when on). Key `s` toggles split;
  switching on seeds B = A + 1 kHz (CW) / 5 kHz (voice) / 0 (data) unless B
  is already valid on this band.
- *In-band by construction:* `split_set_b()` clamps B into A's band (a drag
  stops at the edge, a TCI `vfo:0,1` beyond it is echoed back clamped) — so
  the per-band PA calibration keyed on A's band is always B's too;
  `band_apply()` drops split (B := A, toast) on a band change — ExpertSDR's
  "automatic SPLIT disable", hard-wired; startup refuses an off-band B.
  Persisted as `[rx] split` + `freq_b`.
- *TCI:* `vfo:0,1,f` (and piHPSDR's `vfo:1,0,f`) = B, `split_enable:0,x`
  real backend (no longer the echo table), both in the handshake and the
  change reporter, `tx_frequency:` = the TX VFO. JTDX and tciadapter send
  exactly this pair (research 2026-09-06: JTDX `TCITransceiver.cpp:1184/1469`,
  tciadapter `adapter.go:228-255`). `sdrfl-tci-test` +6 checks.
- *Gates:* `sdrfl-p2dev-test` pins DUC/LPF from B and BPF from A (in-band and
  cross-band), `sdrfl-p1txprobe` pins the 0x02 + feedback-DDC frames.
- *Live:* Richard's pass on the G2E the same evening (interaction, SPLIT
  strip button, colours) — "dobrý". The keyed path on B follows the same
  `tx_freq` the wire tests pin; first on-air use will be the proof of that.

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
