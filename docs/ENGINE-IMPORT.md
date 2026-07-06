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

1. **WDSP in the build.** Compile `wdsp/` into a static lib via meson (or link
   piHPSDR's build), expose its headers. No radio needed.
2. **Discovery.** Vendor/adapt `discovery.c` → find the ANAN at 192.168.1.247.
   Broadcast only; no radio ownership needed yet.
3. **Protocol 2 RX.** Vendor/adapt `new_protocol.c` enough to: start the radio,
   set one RX (sample rate), and receive RX IQ. **Needs the radio free** — close
   piHPSDR / disconnect it from the radio first (one owner at a time).
4. **WDSP analyzer.** From `receiver.c`: create the analyzer, feed RX IQ
   (`fexchange0`), pull the panadapter pixels (`Spectrum0` → `rx->pixel_samples`).
5. **Render.** Feed `rx->pixel_samples` (float) into our `panadapter.c` /
   `waterfall.c` (they currently take `dBm` bytes — add a float path). Full
   resolution, no protocol limits.

After M1: demod + audio (`receiver.c` demod path + audio out), then controls,
then RX2, then TX/PureSignal.

## Notes

- The GTK4 UI, `panadapter.c` and `waterfall.c` are already in place (v0). Only
  the **data source** changes: network bytes → WDSP float pixels.
- Keep the network client (`client.c`) — it can remain an optional "remote head".
