# Engine import plan (from piHPSDR)

How the piHPSDR *engine* is brought into `sdr-for-linux`, RX first. Source of
truth: `/home/rfa/.local/opt/pihpsdr` (git `974acba`). Import incrementally —
only what each milestone needs — vendoring files with their GPL headers intact
(same spirit as `vendor/pihpsdr/client_server.h`).

## The engine ↔ GUI boundary (the real work)

piHPSDR's engine and its GTK3 GUI are intertwined: the engine pushes redraws via
`g_idle_add()` onto GTK widgets, reads GUI state (`rx->width`, `rx->pixels`), and
stores results (`rx->pixel_samples`) for the GUI to draw. To reuse the engine
under a GTK4 UI we cut this boundary: the engine stays headless (GLib only — no
GTK), fills `rx->pixel_samples`, and our GTK4 side reads it on a tick. GLib
(`g_idle_add`, `g_timeout_add`, `GThread`) works in both GTK3 and GTK4.

## Relevant piHPSDR files

| Area | Files | Notes |
|---|---|---|
| Discovery | `discovery.c` | UDP broadcast to find the radio on the LAN. |
| **Protocol 2** (our G1) | `new_protocol.c`, `protocols.c` | Start radio, RX IQ stream, command channel. |
| Protocol 1 (other ANANs) | `old_protocol.c` | Later / optional. |
| Radio + RX state | `radio.c`, `receiver.c` | `receiver.c` drives the **WDSP analyzer** (`XCreateAnalyzer`, `SetAnalyzer`, `Spectrum0`) and fills `rx->pixel_samples` (float). Trim to what RX needs. |
| DSP | `../wdsp/` | WDSP library — **build & link, do not modify.** |

## Milestone 1 — RX panadapter on the real radio

1. ✅ **WDSP in the build.** WDSP + rnnoise + libspecbleach vendored in-tree and
   built as meson static libs (`vendor/wdsp`, `vendor/rnnoise`,
   `vendor/libspecbleach`, each with a `VENDOR.md`). Gate: `sdrfl-wdsp-smoke`
   links `libwdsp` and calls a WDSP symbol. Built with `c_std=gnu11` (WDSP needs
   the GNU dialect). Commit `4f06c05`.
2. ✅ **Discovery.** Headless GLib-only engine layer `src/engine/`
   (`discovery_p2.c`, adapted from `new_discovery.c`; `discovered.h` vendored
   verbatim). Gate: `sdrfl-discover` — verified live, found the ANAN G1 at
   192.168.1.247 while piHPSDR was streaming (read-only, does not disturb).
   Commit `2bd329e`.
3. ✅ **Protocol 2 RX.** Lean RX-only `src/engine/protocol2.c` (Option B, scope in
   [`P2-RX-SCOPE.md`](P2-RX-SCOPE.md)): one DDC over P2, IQ → `on_rx_iq` callback.
   Gate `sdrfl-rxprobe` — verified **live** on the G1: 14.1 MHz @ 192 kHz,
   ~191.9 kHz effective rate, IQ RMS ~−59 dBFS, clean sequence. `SDRFL_DRYRUN`
   hexdumps the packets offline. Wire-critical bytes copied verbatim from
   `new_protocol.c` @ 974acba.
4. ⏳ **WDSP analyzer.** *(next)* From `receiver.c`: create the analyzer, feed RX IQ
   (`fexchange0`), pull the panadapter pixels (`Spectrum0` → `rx->pixel_samples`).
   The `on_rx_iq` callback already delivers `double` I/Q — exactly WDSP's input.
5. **Render.** Feed `rx->pixel_samples` (float) into our `panadapter.c` /
   `waterfall.c` (they currently take `dBm` bytes — add a float path). Full
   resolution, no protocol limits.

After M1: demod + audio (`receiver.c` demod path + audio out), then controls,
then RX2, then TX/PureSignal.

## NEXT SESSION STARTS HERE — Milestone 1, step 4 (WDSP analyzer → panadapter)

Steps 1–3 done: WDSP builds, discovery works, and `src/engine/protocol2.c`
delivers live RX IQ via the `on_rx_iq(const double *iq, int n_pairs, void*)`
callback (verified on the G1 with `sdrfl-rxprobe`). Step 4 turns that IQ into
panadapter pixels using the WDSP **analyzer** — no radio needed to write it, only
to see the final picture.

Goal: create a WDSP analyzer channel, feed it the `on_rx_iq` samples, and pull
the spectrum into a `float` pixel array our `panadapter.c` can render.

**Scope to map (from `receiver.c` @ 974acba, RX/analyzer part only):**
- Analyzer lifecycle: `XCreateAnalyzer` + `SetAnalyzer` (see `rx_create_analyzer`
  / the analyzer setup in `receiver.c`) — FFT size, window, overlap, `rx->pixels`.
- Feed path: upstream accumulates IQ into `rx->iq_input_buffer` and calls
  `Spectrum0(1, rx->id, 0, 0, iq)` every `buffer_size` samples (`rx_full_buffer`,
  receiver.c:1307/1342). Our `on_rx_iq` already hands over `double` I/Q — batch it
  to `buffer_size` and call `Spectrum0`.
- Output: `GetPixels(rx->id, 0, rx->pixel_samples, &flag)` → `float` dBm array of
  length `rx->pixels`. That array is the panadapter input (full float resolution,
  no 4096 cap, no 1 dB quantisation — see repo CLAUDE.md).
- Also `fexchange0` for demod later (step: demod+audio) — **not** needed for the
  panadapter; the analyzer (`Spectrum0`/`GetPixels`) is enough for step 4.

**Decide:** keep a minimal in-house receiver-ish state struct (as in step 3), or
start adopting piHPSDR's `RECEIVER` for the WDSP fields (`id`, `pixels`,
`pixel_samples`, `iq_input_buffer`, `buffer_size`, `fft_size`, window). Present
a scope plan + get consent before the import (same as steps 1 & 3).

**Gate idea (`sdrfl-panprobe` or extend render-test):** discover → P2 RX →
analyzer → dump one averaged `pixel_samples` frame to PNG (headless), like the
existing `sdrfl-render-test` but fed by the analyzer instead of the network path.

## Notes

- The GTK4 UI, `panadapter.c` and `waterfall.c` are already in place (v0). Only
  the **data source** changes: network bytes → WDSP float pixels.
- Keep the network client (`client.c`) — it can remain an optional "remote head".
