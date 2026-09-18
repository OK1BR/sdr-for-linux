# WDSP analyzer → panadapter: call reference

How WDSP's **analyzer** turns RX IQ into panadapter pixels — the call sequence,
the meaning of `SetAnalyzer`'s 19 positional arguments (the code calls it with
bare values) and the non-obvious rules around it. Reference values from piHPSDR
`receiver.c` @ 974acba; WDSP API from `vendor/wdsp/`. Implemented as
`src/engine/analyzer.c`. Originally the 2026-07-06 scope plan — its gate,
proposed API, file list, risks and implementation order were removed once done
(last full version: commit 6a48d13). Where the code has moved on since, the
value is marked **[code today: …]**.

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

## 3. The WDSP call sequence (values from receiver.c:1647-2052)

**Create (once):**
```c
XCreateAnalyzer(id, &rc, M_SIZE, 1, 1, NULL);   // rc==0 on success
```
- `M_SIZE` was first chosen as 16384 (zoom = 1, low memory; 262144 costs
  hundreds of MB of FFTW buffers). **[code today: `A_MSIZE 262144`, as in
  piHPSDR — raised for deep zoom; 16384 is the smallest FFT size
  `clamp_afft()` picks.]**

**Configure (once now; re-call only if pixels/rate/fps change) — 19 args:**
| arg | value | meaning |
|---|---|---|
| disp | `id` | analyzer id (0 = RX1) |
| n_pixout | 1 | one detector/output slot |
| n_fft | 1 | no spur elimination |
| typ | 1 | complex I&Q input |
| flp | `{0}` | low-side LO (no flip) |
| sz | `afft_size` | FFT size **[code today: picked per zoom by `clamp_afft()`, 16384…`A_MSIZE`]** |
| bf_sz | 1024 | **samples per `Spectrum0` call** (our re-buffer target) |
| win_type | 5 | Kaiser window |
| pi | 14.0 | Kaiser β |
| ovrlp | `max(0, ceil(sz - sample_rate/fps))` | FFT overlap |
| clp | 0 | no per-FFT bin clip |
| fscLin/fscHin | 0.0 / 0.0 | full span **[code today: computed from zoom and pan — the bins clipped from each end]** |
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
SetDisplayNumAverage  (id, 0, max(2, min(60, fps*t)));   // t = avg_time (0.12 s in piHPSDR; code today: 0.030)
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
