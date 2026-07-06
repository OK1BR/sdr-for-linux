# Milestone 1 · Step 4 — WDSP analyzer → panadapter: scope plan

Turn the live RX IQ (delivered by `src/engine/protocol2.c`'s `on_rx_iq` callback,
step 3) into panadapter pixels using WDSP's **analyzer**. Source of truth for the
reference values: piHPSDR `receiver.c` @ 974acba; WDSP API from our vendored
`vendor/wdsp/`. No code written yet — consent before the import (as with steps
1 & 3).

---

## 0. Milestone gate

`sdrfl-panprobe` (headless PNG, like the existing `sdrfl-render-test`): discover
→ P2 RX (192 kHz @ 14.1 MHz) → feed IQ to the analyzer → poll `GetPixels` →
render `pixel_samples` (float) through our existing `panadapter_draw()` into a
PNG. Proves IQ→analyzer→our-renderer end-to-end. Needs the radio free (takes the
TRX) only for the final picture; everything else is offline.

---

## 1. Headline finding — the analyzer is small and self-contained

The WDSP **analyzer** (`disp`) is independent of the WDSP **channel** (`ch`, the
demod/`fexchange0` side). For a panadapter we need **only** four analyzer calls
plus the `SetDisplay*` config — no `OpenChannel`, no `fexchange0`, no channel
buffers. Data types line up with what we already have:

- `Spectrum0(run, disp, ss, LO, double *pbuff)` takes **interleaved `double` I/Q**
  — exactly what our `on_rx_iq(const double *iq, int n_pairs)` delivers.
- `GetPixels(disp, pixout, float *pix, int *flag)` writes **`float` dBm-ish** —
  exactly what our `panadapter_draw(..., const float *dbm, ...)` consumes.

So step 4 is a thin WDSP wrapper + a re-buffer + a render loop. No piHPSDR
`receiver.c`/`receiver.h` import (they drag the whole demod/AGC/GTK receiver).

---

## 2. Architecture (proposed) — a lean `analyzer` module

Consistent with step 3's Option B: a small in-house module holding its own state,
**not** piHPSDR's `RECEIVER` struct.

`src/engine/analyzer.c` / `analyzer.h` — a thin analyzer wrapper:

```c
int  analyzer_create(int id, int pixels, int sample_rate, int fps);
void analyzer_feed(const double *iq, int n_pairs);   // re-buffers → Spectrum0
int  analyzer_get_pixels(float *out, int pixels);    // GetPixels; returns fresh?
void analyzer_destroy(void);
```

Internal state: `id`, `pixels`, `sample_rate`, `fps`, `afft_size`, a `double`
accumulator (`2*bf_sz`) + fill counter, and a mutex fencing feed vs.
create/destroy. `analyzer_feed` maps piHPSDR's `rx_add_iq_samples` +
`rx_full_buffer`: append incoming pairs into the accumulator and, each time it
reaches `bf_sz` (1024) pairs, call `Spectrum0` and reset (carrying the remainder,
since our blocks are ~238 pairs and don't divide 1024 evenly).

Wiring: the gate passes a small feed callback to `p2_rx_start` that forwards to
`analyzer_feed`; a render loop polls `analyzer_get_pixels` at `fps`.

---

## 3. The WDSP call sequence (values from receiver.c:1647-2052)

**Create (once):**
```c
XCreateAnalyzer(id, &rc, M_SIZE, 1, 1, NULL);   // rc==0 on success
```
- `M_SIZE` = **16384** (not piHPSDR's 262144). With zoom=1 the FFT size is
  16384; 262144 only buys future zoom and costs hundreds of MB of FFTW buffers.
  Raising it later is a one-line change. *(decision A below)*

**Configure (once now; re-call only if pixels/rate/fps change) — 19 args:**
| arg | value | meaning |
|---|---|---|
| disp | `id` | analyzer id (0 = RX1) |
| n_pixout | 1 | one detector/output slot |
| n_fft | 1 | no spur elimination |
| typ | 1 | complex I&Q input |
| flp | `{0}` | low-side LO (no flip) |
| sz | `afft_size` (16384) | FFT size |
| bf_sz | 1024 | **samples per `Spectrum0` call** (our re-buffer target) |
| win_type | 5 | Kaiser window |
| pi | 14.0 | Kaiser β |
| ovrlp | `max(0, ceil(sz - sample_rate/fps))` | FFT overlap |
| clp | 0 | no per-FFT bin clip |
| fscLin/fscHin | 0.0 / 0.0 | full span (no zoom/pan) |
| n_pix | `pixels` | output pixel count (our column count) |
| n_stch | 1 | one sub-span |
| calset | 0 | no calibration set |
| fmin/fmax | 0.0 / 0.0 | calibration off |
| max_w | `afft_size + min(0.1*rate, 0.1*afft_size*fps)` | ring write-ahead |

Then:
```c
SetDisplayDetectorMode(id, 0, DETECTOR_MODE_PEAK);      // 0 — panadapter peaks
SetDisplayAverageMode (id, 0, AVERAGE_MODE_NONE);        // switch-artifact guard
SetDisplayAverageMode (id, 0, AVERAGE_MODE_LOG_RECURSIVE);// 3 — smooth trace
SetDisplayNumAverage  (id, 0, max(2, min(60, fps*t)));   // t = avg_time (0.12 s)
SetDisplayAvBackmult  (id, 0, exp(-1.0/(fps*t)));
SetDisplaySampleRate  (id, sample_rate);                 // bandwidth norm
SetDisplayNormOneHz   (id, 0, 1);
```

**Feed (per 1024 pairs, from the P2 listener thread):**
```c
Spectrum0(1, id, 0, 0, accumulator);   // accumulator = 2048 doubles (I,Q,I,Q,…)
```

**Read (at fps, from the render/main thread):**
```c
GetPixels(id, 0, pixel_samples, &flag);  // flag==1 ⇒ fresh frame → render
```

**Destroy (shutdown):** `DestroyAnalyzer(id)` (piHPSDR never calls it, but WDSP
supports it and we should, to be clean).

---

## 4. Things to get right

- **Interleave / spectrum orientation.** WDSP's `Spectrum0` reads `I=pbuff[2i+1]`,
  `Q=pbuff[2i]` internally, while our `on_rx_iq` stores `I=[2i]`, `Q=[2i+1]`
  (same layout piHPSDR feeds). We reproduce piHPSDR's feed **verbatim** (buffer
  1:1 into `Spectrum0`) and get piHPSDR's result; **verify the spectrum isn't
  mirrored** on the live picture (a real signal must land on the correct side of
  centre). If mirrored, that's a known one-line swap.
- **Units / display calibration.** `GetPixels` output is **relative dB**, not
  dBm. Our `panadapter.c` fixes its window to −50…−140 dBm. Plan: in the gate,
  print min/median/max of `pixel_samples`, then set one scalar `soffset` so the
  noise floor lands sensibly in that window (piHPSDR folds gain-cal/atten/preamp
  into the same `soffset`; we start with a single measured constant). No absolute
  dBm calibration yet — that's a later refinement.
- **Threading.** Feed (`Spectrum0`, listener thread) and read (`GetPixels`,
  render thread) need **no shared lock** — WDSP's triple-buffer decouples them.
  Only `analyzer_create`/`analyzer_destroy` are fenced against the feed by our
  module mutex. (piHPSDR does exactly this: no lock on `GetPixels`.)
- **Feed rate vs frames.** `Spectrum0` just buffers; WDSP's own dispatcher +
  worker threads run the FFT and produce ~`fps` averaged frames/s regardless of
  our 1024-block feed cadence. We poll `GetPixels` at `fps` and skip when flag=0.

---

## 5. New files & build

| File | Role |
|---|---|
| `src/engine/analyzer.c` / `analyzer.h` | thin WDSP analyzer wrapper (§2). |
| `src/panprobe_main.c` | gate: discover → P2 RX → analyzer → `panadapter_draw` → PNG. |
| `meson.build` | new `sdrfl-panprobe` target: `panprobe_main.c` + `analyzer.c` + `engine_sources` + `ui_sources`; deps `wdsp_dep, gtk_dep (cairo), glib_dep, m_dep`; `override_options: ['c_std=gnu11']` (WDSP's `comm.h` needs the GNU dialect, like `sdrfl-wdsp-smoke`). |

The gate builds a minimal `ClientFrame` (width=pixels, `vfo_a_freq`=freq, `s_dbm`)
to feed `panadapter_draw`'s readouts, and hands it the float `pixel_samples` for
the trace. Waterfall (bytes) is optional — a float→byte convert if we want it in
the PNG; panadapter is the primary target.

## 6. Risks & mitigations
- **Spectrum mirrored / off-frequency.** Verify on the live PNG against a known
  signal; one-line I/Q swap if needed (§4).
- **Levels off-window (blank or clipped trace).** The `soffset` measurement step
  (§4) handles it; the gate prints the raw pixel range so we can set it.
- **`XCreateAnalyzer` memory.** `M_SIZE=16384` keeps it modest; note the tradeoff
  for future zoom.
- **Radio ownership.** The final PNG needs the radio free — ask Richard, don't
  kill his piHPSDR. All the build/wrapper work is offline.

## 7. Decisions (confirmed with Richard 2026-07-06)
- **A. `M_SIZE` = 16384** (zoom=1, low memory). Raising it later for zoom is a
  one-liner.
- **B. Lean in-house `analyzer` module** (own state), consistent with step 3.
- **C. Gate as a new `sdrfl-panprobe`** (PNG); the network render-test stays intact.

## 8. Implementation order (after consent)
1. `analyzer.{c,h}`: create/configure/destroy + a static self-check that it
   builds and links WDSP (offline, no radio).
2. `analyzer_feed` re-buffer (238→1024) + `Spectrum0`; `analyzer_get_pixels`.
3. `panprobe_main.c` + meson target; compile clean.
4. **(live, radio free)** run → measure pixel range, set `soffset`, write PNG,
   eyeball the panadapter (signal on the right frequency, sane noise floor).

Then Milestone 1 is essentially done (live float panadapter on the real radio);
next milestones: demod+audio, then the GTK4 control surface.
