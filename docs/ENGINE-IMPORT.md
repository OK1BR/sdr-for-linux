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
3. ⏳ **Protocol 2 RX.** *(next — needs the radio free for the live test)*
4. **WDSP analyzer.** From `receiver.c`: create the analyzer, feed RX IQ
   (`fexchange0`), pull the panadapter pixels (`Spectrum0` → `rx->pixel_samples`).
5. **Render.** Feed `rx->pixel_samples` (float) into our `panadapter.c` /
   `waterfall.c` (they currently take `dBm` bytes — add a float path). Full
   resolution, no protocol limits.

After M1: demod + audio (`receiver.c` demod path + audio out), then controls,
then RX2, then TX/PureSignal.

## NEXT SESSION STARTS HERE — Milestone 1, step 3 (Protocol-2 RX + IQ)

Goal: start the G1 over Protocol 2, run **one** DDC (receiver), and get the RX
**IQ stream** into a buffer we own. Get consent + present a scope plan before the
big import (same as WDSP). Most of this is offline; only the live IQ test needs
the radio.

**⚠️ Radio ownership.** The live test opens the P2 data path → takes the radio
from piHPSDR (one owner at a time). Richard listens via piHPSDR in the
background — **ask him to free the radio before the live test; do not kill his
piHPSDR yourself** (his explicit instruction, 2026-07-06). Offline build work
needs no radio.

**Scope to vendor/adapt into `src/engine/` (RX-only, trim hard):**
- `new_protocol.c` / `new_protocol.h` — P2 link. Keep: build+send the **General
  packet** (start radio, sample rate), the **High-Priority packet** (run bit, RX
  DDC freq), the **RX-specific/DDC-command** packets, and the **receive path**
  for DDC IQ data (port ~1035). Drop: TX/DUC, mic, PureSignal, wideband, all but
  one DDC. Two UDP streams (command + data), sequence numbers, big-endian.
- Minimal `radio`/`adc`/`receiver` state the above reads (`rx->sample_rate`,
  `rx->id`, ddc/adc mappings). Define in `engine_state.c` (extend it). Avoid
  pulling receiver.c's WDSP+GTK entanglement yet — for step 3 just get IQ out via
  a callback/ring buffer, e.g. `void on_rx_iq(const float *iq, int n_samples)`.
- Reuse the established patterns: `message.h` shim, GLib-only, headless.

**Milestone gate (`sdrfl-rxprobe`, headless):** discover → start radio → set one
RX (e.g. 192 kHz @ 14.1 MHz) → collect IQ ~1 s → print sample count, effective
rate, RMS / a few samples. Proves live IQ end-to-end. (Then step 4 feeds this IQ
to the WDSP analyzer.)

**Watch out:** `new_protocol.c` is large and globals-heavy (`radio`,
`receiver[]`, `transmitter`, `adc[]`); expect to stub/trim a lot. P2 needs the
radio's MAC/addr from discovery (`discovered[selected_device]`) — already have
it. Endianness + exact packet offsets: copy faithfully from upstream, verify
against `new_protocol.c` @ 974acba.

## Notes

- The GTK4 UI, `panadapter.c` and `waterfall.c` are already in place (v0). Only
  the **data source** changes: network bytes → WDSP float pixels.
- Keep the network client (`client.c`) — it can remain an optional "remote head".
