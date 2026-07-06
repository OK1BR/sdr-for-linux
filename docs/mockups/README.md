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

## Files

| File | What it shows |
|---|---|
| `main_window_libadwaita.c` | Main window: headerbar with big VFO readout + top control strip (mode / filter / AGC / NR·NB·ANF / AF / bands) over a faked panadapter+waterfall. Uses `mock_common.h`. |
| `settings_libadwaita.c` | `AdwPreferencesWindow` — Radio / Audio / Display / DSP / About pages with switch/combo/spin/entry rows. |
| `parametric_eq_libadwaita.c` | **Richard's favourite.** A parametric EQ: a Cairo-drawn log-freq response curve with draggable orange band nodes (drag = freq×gain, live redraw) — inside a libadwaita window. The template for custom instrument widgets. |
| `mock_common.h` | Shared helpers for the main-window mock (fake spectrum draw, control strip, VFO, CSS). |

## Build & run (standalone, needs the radio-free — they are fake, no radio)

```sh
cd docs/mockups
cc main_window_libadwaita.c   -o /tmp/mw  $(pkg-config --cflags --libs libadwaita-1) -lm
cc settings_libadwaita.c      -o /tmp/set $(pkg-config --cflags --libs libadwaita-1)
cc parametric_eq_libadwaita.c -o /tmp/eq  $(pkg-config --cflags --libs libadwaita-1) -lm
GSK_RENDERER=cairo /tmp/eq      # drag the orange nodes
```

`GSK_RENDERER=cairo` avoids the NVIDIA+Wayland GTK4 GL crash (same as the app).
The `GtkImage ... baselines` warnings on stderr are a harmless GTK 4.22 quirk.
