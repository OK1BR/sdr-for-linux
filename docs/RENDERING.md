# Display rendering — architecture, renderer selection, measurements

Status 2026-08-01. Covers the `SdrflDisplay` widget (GSK snapshot path), the
automatic GSK renderer choice, the diagnostic techniques used to get here, and
the one open item. History: commits `90cd88e` (renderer default) + `fec8131`
(GPU waterfall / snapshot widget).

## Why (the stutter diagnosis)

The spectrum + waterfall stuttered "like a laggy game" at 60 FPS, worse the
busier the band — audio unaffected (the engine runs on its own threads).
Frame-clock tracing (`GDK_DEBUG=frames`, see Techniques) showed the cause:
with everything rendered by GTK's **CPU Cairo renderer**, one frame's paint
cost was 10–13 ms against the 16.7 ms budget of 60 FPS — the tail of that
distribution (p99 ≈ 19.5 ms) regularly missed the deadline, dropping frames
in visible bursts (100–566 misses/min). The display data path itself never
lags by design: the GUI tick always pulls the *latest* analyzer frame (WDSP
triple buffer), the waterfall history is a fixed 256-row bitmap, DX spots are
bounded (MAX_SPOTS + TTL prune).

Two additional, separate effects seen while measuring, both compositor-side
and rare: single frames held by gnome-shell for seconds (`present=3691 ms` in
the frame log), and the frame clock legitimately stopping while the window is
fully occluded (mutter suspends frame callbacks — the "frozen when covered"
look is benign).

## Renderer selection (main.c → `g_setenv`)

| Runtime GTK | Default | Why |
|---|---|---|
| ≥ 4.22 | `gl` (GPU) | Live-verified 2026-08-01 (RTX 5070, NVIDIA 610.43, Wayland). The GTK-4.14-era NVIDIA+Wayland GL crash is gone. |
| < 4.22 (incl. AppImage's bundled 4.14) | `cairo` (CPU) | GL unverified/crashy there; cairo is the proven path. |

- An operator's explicit `GSK_RENDERER` env always wins (`g_setenv` with
  `overwrite=FALSE`).
- The startup log always prints the choice: `renderer: GSK_RENDERER=… (GTK x.y)`.
- ⛔ **The Vulkan renderer was measured and rejected** on NVIDIA 610.43:
  30–36 f/s and p95 35 ms on the same scene where GL holds a steady
  58.5 f/s. Do not flip the default to vulkan without re-measuring.

Why GL wins even though the average frame cost barely moved: it collapses the
*jitter*. Same scene, 60 FPS target, >40 ms hitches: cairo 100–566/min → GL
~2/min; frame_end p99 19.5 → 16.6 ms.

## The `SdrflDisplay` widget (gui.c)

A custom `GtkWidget` subclass with a `snapshot()` vfunc replaced the old
full-window `GtkDrawingArea` draw func. Scene, bottom to top:

1. **Waterfall texture** — the 256-row ARGB32 history bitmap becomes a
   `GdkMemoryTexture`, appended with `gtk_snapshot_append_scaled_texture`
   (`GSK_SCALING_FILTER_NEAREST` — crisp streaks, like piHPSDR). The GPU does
   the scaling; the texture is rebuilt **only when the content changed**,
   keyed on `waterfall_serial()` (waterfall.c stays GTK-free — it exposes the
   surface + a serial bumped on every push/palette switch/resize).
   RX and TX waterfalls each cache one texture (`App.wf_tex[2]`).
2. **Spectrum cairo node** — the existing cairo code (panadapter, scales,
   spots, passband, badges, S-meter, separator) rendered into a node covering
   only the spectrum strip (+1 px separator overlap), not the full window.
3. **Waterfall-overlay cairo node** — filter edges / VFO line / select cursor
   carried down onto the waterfall; appended only when enabled
   (`wf_overlays_wanted()` must exactly match what `draw_wf_overlays()`
   paints).
4. Status screens (no radio / calibrating / network error) and the TX display
   keep a full-surface cairo fallback (`draw_all`), layered over the same
   texture — output is identical to the old draw order by construction.

Measured effect (2048 px, 60 FPS, GL, RTX 5070): the app's own snapshot work
is **~1.1 ms/frame** — texture 0.2 + cairo 0.9, of which `panadapter_draw`
0.8. Scene complexity (a crowded band) now scales a ~1 ms base instead of a
~13 ms one.

## Techniques (reusable)

- **Frame-clock trace**: run with `GDK_DEBUG=frames`, pipe through a
  timestamper (`perl -MTime::HiRes=time -ne 'BEGIN{$|=1} printf "%.3f %s",
  time, $_'`). Key fields per frame: `interval` (gap since last frame — the
  stutter metric), `frame_end` (paint cost), `present` (compositor
  presentation latency). Analyze percentiles + counts over minutes.
- **Draw profiler**: `SDRFL_DRAW_PROF=1` prints once a second
  `drawprof: tex=… cairo=… (of which pan=…) ms/frame (N f/s)` — the
  per-section cost of *our* snapshot, separating it from GTK/driver time.
- Renderer identity: `GSK_DEBUG=renderer` (which renderer), and
  `/proc/<pid>/maps` (which GL library — `libEGL_nvidia` vs `llvmpipe`)
  when "is this really hardware GL?" is the question.

## Open item

With GL, `frame_end` still averages ~13 ms while our snapshot costs ~1.1 ms:
**~12 ms/frame of main-thread CPU is spent inside GSK GL** (upload of the
spectrum cairo node, GL submit, and/or the NVIDIA Wayland swap path, which is
known to busy-wait). It does not drop frames at 60 FPS (58.5 f/s held, ~2
big hitches/min) but it burns most of a core and leaves no headroom. Next
steps: profile the render phase with `perf`; try NVIDIA's `__GL_YIELD=USLEEP`.
Static-layer caching (grid/scales as cached nodes) is **not** the answer —
the whole cairo layer is already down to 0.9 ms.
