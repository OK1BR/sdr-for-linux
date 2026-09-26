# Display rendering — architecture, renderer selection, measurements

Status 2026-09-26. Covers the `SdrflDisplay` widget (GSK snapshot path), the
automatic GSK renderer choice, the diagnostic techniques used to get here, and
what is still open. History: commits `90cd88e` (renderer default) + `fec8131`
(GPU waterfall / snapshot widget, 2026-08-01), then issue #15 (2026-09-26:
columns follow the width, the spectrum body on the GPU, no strip-wide cairo
node).

## Why (the two diagnoses)

**2026-08-01, 60 FPS stutter at ≤ 2048 px.** The spectrum + waterfall
stuttered "like a laggy game", worse the busier the band — audio unaffected
(the engine runs on its own threads). Frame-clock tracing (`GDK_DEBUG=frames`,
see Techniques) showed one frame's paint cost at 10–13 ms against the 16.7 ms
budget with GTK's **CPU cairo renderer**; the tail (p99 ≈ 19.5 ms) missed the
deadline in visible bursts. Fix: the GL renderer + the waterfall as a GPU
texture (below). The display data path itself never lags by design: the GUI
tick always pulls the *latest* analyzer frame (WDSP triple buffer), the
waterfall history is a fixed 256-row bitmap, DX spots are bounded.

**2026-09-26, 30 f/s above ~2100 px (issue #15).** On a 3606 or 5120 px
window the frame clock fell to every second vblank (frame_end 19–22 ms), the
waterfall scrolled at half speed and looked blocky. Two causes:

1. The spectrum strip was ONE cairo node, and GTK rasterizes a cairo node on
   the CPU **at render time**, inside `gsk_gpu_upload_cairo_op`, then uploads
   it — 7 / 10 / 13 ms at 2048 / 3606 / 5120 px for the fill (a per-pixel
   path fill with a 25-stop gradient) and the trace (one stroke per column).
   ⛔ `SDRFL_DRAW_PROF` never saw this: it times only the *recording* into the
   surface `gtk_snapshot_append_cairo()` hands out (0.9 ms). The earlier
   conclusion "the cairo layer costs 0.9 ms, GSK GL burns ~12 ms" was that
   misread — the 12 ms were our own raster.
2. `ENGINE_PIXELS` was a fixed 2048, so the waterfall bitmap and the trace
   were 2048 columns stretched 1.8–2.5×.

Two additional, separate effects seen while measuring, both compositor-side
and rare: single frames held by gnome-shell for seconds (`present=3691 ms` in
the frame log), and the frame clock legitimately stopping while the window is
fully occluded or on another workspace (mutter suspends frame callbacks — the
"frozen when covered" look is benign, and a measurement needs the window on
the visible workspace).

## Renderer selection (gui.c → `g_setenv`)

| Runtime GTK | Default | Why |
|---|---|---|
| ≥ 4.22 | `gl` (GPU) | Live-verified 2026-08-01 (RTX 5070, NVIDIA 610.43, Wayland). The GTK-4.14-era NVIDIA+Wayland GL crash is gone. |
| < 4.22 | `cairo` (CPU) | GL unverified/crashy there; cairo is the proven path. |

- An operator's explicit `GSK_RENDERER` env always wins (`g_setenv` with
  `overwrite=FALSE`).
- The startup log always prints the choice: `renderer: GSK_RENDERER=… (GTK x.y)`.
- ⛔ **The Vulkan renderer was measured and rejected** on NVIDIA 610.43:
  30–36 f/s and p95 35 ms on the same scene where GL holds a steady
  58.5 f/s. Do not flip the default to vulkan without re-measuring.
- GTK ≥ 4.12 is required (meson): `GDK_MEMORY_A8` mask textures and
  `gtk_snapshot_push_mask`.

Why GL wins even though the average frame cost barely moved in 2026-08: it
collapses the *jitter*. Same scene, 60 FPS target, >40 ms hitches: cairo
100–566/min → GL ~2/min; frame_end p99 19.5 → 16.6 ms.

## Columns follow the window (issue #15)

The RX and TX analyzers produce one column per display pixel: `tick_cb` →
`pixels_follow_width()` asks for `widget width × scale factor`, clamped to
[256, `ANALYZER_MAX_PIXELS` = 8192] (our own cap; WDSP allows 16384, the
vendored `SPECTRUM_DATA_SIZE` 4096 stays the network protocol's), debounced
300 ms — `SetAnalyzer` stops and restarts WDSP's dispatcher, never per frame
during a resize — and applied through `analyzer_set_pixels()` +
`tx_run_set_pixels()`. The first frame after a change is dropped (it may
carry the old count's data); the EMAs re-seed on their own width check; the
waterfall **resamples** its history to the new width (nearest column) instead
of clearing it. The initial count is the saved window width.

The spectrum, waterfall and TX-trace EMAs use the wall-clock gap since the
last consumed frame (`ema_factor_dt`, the S-meter idiom): a frame clock at
30 f/s no longer doubles the time constants.

## The `SdrflDisplay` widget (gui.c) — the scene

A custom `GtkWidget` subclass with a `snapshot()` vfunc. Bottom to top, live
RX:

1. **Waterfall texture** — the 256-row ARGB32 history bitmap as a
   `GdkMemoryTexture`, `gtk_snapshot_append_scaled_texture` with
   `GSK_SCALING_FILTER_NEAREST` (crisp streaks). Rebuilt **only when the
   content changed**, keyed on `waterfall_serial()` (waterfall.c stays
   GTK-free). RX and TX each cache one texture (`App.wf_tex[2]`).
2. **Spectrum body** (`snapshot_body`) — a colour node (the palette's floor
   colour), the dB grid lines as 1-px colour nodes, then two mask nodes:
   `mask(fill A8) × vertical gradient` (palette by the dBm at that row,
   alpha 0.55) and `mask(trace A8) × a W×1 colour strip` stretched
   vertically (per-column colour = the brighter neighbouring column, lifted
   50 % to white — the per-segment colouring of the cairo trace).
   panadapter.c computes the masks on the CPU in device pixels
   (`panadapter_body_band` → `panadapter_body_masks`): the polyline runs
   through the column centres, column x carries the half-segments to both
   neighbours, coverage is antialiased per row. The masks cover only the rows
   the polyline spans; below them the fill is a plain gradient node. The GTK
   GPU renderer composes mask nodes on the GPU (`gsk_gpu_mask_op`); fill and
   stroke nodes would NOT help — GTK 4.22 still rasterizes their path on the
   CPU for every changed path (`gsk_gpu_cached_fill_lookup`).
3. **Full-height lines** — colour nodes: the VFO hairline (0.75 px at 60 %,
   pixel-centre snapped), the filter passband fill + edges, the select-mode
   ghost, the frequency grid, the DX-spot ticks; the dashed band edges are
   3-px-wide cairo slivers (dashes need cairo).
4. **Text** — cairo only where there is text: a 230-px top band (ruler
   labels, spot labels, ADC badge, readout block, S-meter) and the 46-px dB
   gutter (`panadapter_draw_db_labels`). `panadapter_set_body(0)` makes
   `panadapter_draw()` paint only its readout; the render test and the status
   screens keep the full cairo draw. DX spots are laid out once per frame on
   a scratch context (`spots_layout`), so labels and ticks can sit in
   different nodes.
5. **Divider** — two colour nodes; **filter-on-waterfall overlay** — a cairo
   node as wide as the passband + hairlines only (`wf_overlay_bounds`).

TX while keyed: the same body under a strip-only cairo node (`draw_tx`:
ruler, TX filter, power/SWR, HUD), the filter carried onto the TX waterfall
as a narrow node, the divider colour nodes. The TX strip node is still
strip-wide (its ruler, HUD and filter would need the same split — not done;
the TX display is not the acceptance case).

## Measurements

**Live, before the fix** (ANAN G2E, GL, GTK 4.22.5, RTX 5070, 60 Hz,
`GDK_DEBUG=frames` + `SDRFL_DRAW_PROF=1`, TX periods excluded):

| window width | frame_end median | interval median | result |
|---|---|---|---|
| 2065 px | 14.8 ms | 19.8 ms | 44 f/s (alternating 60/30) |
| 2419 px | 19.0 ms | 32.9 ms | 30 f/s |
| 3606 px | 18.7 ms | 33.2 ms | 30 f/s |
| 5120 px | 21.8 ms | 33.0 ms | 30 f/s |

**Offline, GL renderer, `gsk_renderer_render_texture`** (scratch harness,
strip 690 px tall, RTX 5070, 40 frames; the readback is ~0.2 ms):

| scene | 2048 px | 3606 px | 5120 px |
|---|---|---|---|
| old: one cairo node with fill + trace | 10.1 ms | 16.5 ms | 23.9 ms |
| GPU body + one strip-wide cairo node | 4.4 | 7.5 | 10.5 |
| GPU body + top band + gutter (shipped) | 3.2 | 5.2 | 7.3 |
| … + the 256-row waterfall texture | 4.1 | 7.7 | 9.3 |
| an EMPTY strip-wide cairo node alone | 2.2 | 3.5 | 5.4 |
| the mask build on the CPU | 0.4 | 0.7 | 1.0 |

Pixel diff old ↔ new (synthetic band, 12 peaks): mean ≈ 1/255 per channel;
the trace sits on the column centre (half a column right of the old
left-edge vertices) and steep runs are the column staircase cairo's
antialiasing also produces.

**Live, after the fix:** pending Richard's run (the acceptance: 60 f/s held
at 5120 px on the G2E).

## Techniques (reusable)

- **Frame-clock trace**: run with `GDK_DEBUG=frames`, pipe through a
  timestamper (`perl -MTime::HiRes=time -ne 'BEGIN{$|=1} printf "%.3f %s",
  time, $_'`). Key fields per frame: `interval` (gap since last frame — the
  stutter metric), `frame_end` (paint cost), `present` (compositor
  presentation latency). Analyze percentiles + counts over minutes; exclude
  TX periods (the `tx: KEY/UNKEY` log lines bracket them). Keep the window on
  the visible workspace — an occluded window has no frame clock.
- **Draw profiler**: `SDRFL_DRAW_PROF=1` prints once a second
  `drawprof: tex=… body=… cairo=… (of which pan=…) ms/frame (N f/s)` — the
  per-section cost of *our* snapshot work. ⛔ `cairo=` is the RECORDING time
  only; the raster of a cairo node happens later, inside GTK's render phase,
  and scales with the node's area. To see it, replay the same drawing into a
  `cairo_image_surface` offline, or benchmark the node tree with the GL
  renderer (next point).
- **Offline GL scene benchmark**: build the node tree with `GtkSnapshot`,
  `gsk_gl_renderer_new()` + `gsk_renderer_realize_for_display()`, time
  `gsk_renderer_render_texture()` over N frames; the numbers match the live
  `frame_end` within a millisecond or two and need no radio. Baseline scenes
  (a bare colour node; an empty cairo node of the same size) locate the cost.
- **Column-change gate**: a scratch harness that feeds a tone into the live
  analyzer and re-pixels it (2048 → 5120 → 3606 → 8192 → 700) checks the peak
  lands on the expected column each time.
- Renderer identity: `GSK_DEBUG=renderer` (which renderer), and
  `/proc/<pid>/maps` (which GL library — `libEGL_nvidia` vs `llvmpipe`)
  when "is this really hardware GL?" is the question.
- Headless GUI smoke (status screens): gtk4-broadwayd + headless Chrome,
  ⛔ under `dbus-run-session` — GApplication is single-instance and a
  second instance on the session bus only *activates* the running one,
  which then opens a second main window inside itself.

## Open

- The live confirmation at 5120 px on the G2E (issue #15 stays open with
  `needs-live-check` until then).
- The TX strip node is still strip-wide (see the scene above).
- The waterfall texture is re-uploaded whole on every content change
  (0.5 / 0.9 / 1.6 ms at 2048 / 3606 / 5120 px). A GL ring-buffer texture
  updated one row per frame (`GdkGLTextureBuilder`) would remove most of it;
  not worth its complexity at these numbers.
