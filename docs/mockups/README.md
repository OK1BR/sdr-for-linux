# UI design mockups — the agreed control-surface direction

Non-built **design references** for the Milestone 3+ GTK4 control surface.
These are *throwaway visual mockups* (no engine, no radio) kept as the record of
the UI direction decided with Richard on 2026-07-07. They are **not part of the
meson build**; compile them standalone (commands below) to look at them again.

## Decision: **libadwaita** (not pure GTK4)

We built the same layout twice — once in pure GTK4 + CSS, once in libadwaita —
and compared them live. Findings:

- For the **dense instrument panel** (control strip + panadapter), the two look
  nearly identical → libadwaita adds little there.
- For **settings / dialogs**, libadwaita is clearly nicer and far less code
  (`AdwPreferencesWindow` + `AdwSwitchRow`/`AdwComboRow`/`AdwSpinRow`, with a
  view-switcher and built-in search) vs hand-rolling `GtkListBox` boxed-lists.
- **Custom graphics are unaffected.** libadwaita is *additive* — it is built on
  GTK4, so plain `GtkDrawingArea` + Cairo + `GtkGestureDrag` work unchanged
  inside an Adwaita window. The parametric-EQ mockup proves it.

**Cost accepted:** `adw_init()` themes the whole process (Adwaita stylesheet),
so a libadwaita app does **not** honor a custom GTK theme (only light/dark +
accent). Shared-look with Richard's desktop theme is a later, separate concern.

Chosen shape: **libadwaita**, `AdwApplicationWindow` + `AdwHeaderBar`, a **top
control strip** over the Cairo panadapter/waterfall. First controls: mode,
filter, AGC, NR/NB/ANF, AF volume, band buttons.

## What is kept

`parametric_eq_libadwaita.c` — **Richard's favourite** and the UI seed for the
RX + TX equalizer milestone: a Cairo-drawn log-frequency response curve with
draggable band nodes (drag = freq × gain, live redraw) inside a libadwaita
window; the template for custom instrument widgets. Not part of the meson
build:

```sh
cd docs/mockups
cc parametric_eq_libadwaita.c -o /tmp/eq $(pkg-config --cflags --libs libadwaita-1) -lm
/tmp/eq      # drag the orange nodes
```

The main-window and settings mockups were removed on 2026-09-18 — that UI has
long existed in `src/gui.c` (last versions: commit 6a48d13).
