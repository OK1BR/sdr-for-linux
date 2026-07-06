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
4. ✅ **WDSP analyzer.** Lean wrapper `src/engine/analyzer.c` (Option B, scope in
   [`WDSP-ANALYZER-SCOPE.md`](WDSP-ANALYZER-SCOPE.md)): `XCreateAnalyzer` /
   `SetAnalyzer` / `Spectrum0` / `GetPixels` only (no demod channel); `on_rx_iq`
   `double` I/Q re-buffered to WDSP's 1024 block. Values mirror `receiver.c` @
   974acba.
5. ✅ **Render.** `GetPixels` `float` dBm → our `panadapter_draw()` (already had a
   float path). Gate `sdrfl-panprobe` — verified **live** on the G1: 14.1 MHz @
   192 kHz, 40 frames, noise floor ~−115 dBm, real 20 m signals rendered (CW
   cluster below centre on the correct side). `SDRFL_SELFTEST` checks the
   analyzer + feed order offline (no radio).

**Milestone 1 done** — live float panadapter on the real radio (P2 RX → WDSP
analyzer → our Cairo renderer, full resolution).

**Milestone 2 done** — demodulation + audio. `src/engine/demod.c` (WDSP channel:
`OpenChannel` + `SetRXA*` mode/filter/AGC + `fexchange0`, second consumer of the
analyzer's 1024-sample block) → `src/engine/audio_pw.c` (native PipeWire
`pw_stream` sink behind an `audio.h` seam; lock-free SPSC ring, RT `on_process`).
Backend + rate decided with Richard: **native PipeWire** (min latency) + **768 kHz
IQ**. Gate `sdrfl-audioprobe` (+ `SDRFL_TONE` offline). Wired into the GUI: the
direct-radio window sees **and** hears. Verified live: FT8 7.074 MHz USB, clean
audio, 0 seq errors, **~15 ms end-to-end latency**. Scope: `docs/AUDIO-SCOPE.md`.

Then: controls, then RX2, then TX/PureSignal.

## NEXT SESSION STARTS HERE — Milestone 3 (on-window controls)

RX is usable (see + hear) but tuning/mode/filter are env vars set at startup.
M3 makes the GTK4 window a real control surface, driving the engine live.

Goal: change VFO frequency, mode, filter, and zoom/pan **from the window**,
re-tuning the running radio without restart.

**Engine hooks to add (small, on top of what M1/M2 built):**
- **Re-tune:** re-send the P2 High-Priority packet with the new DDC frequency
  (`protocol2.c` already builds it — add `p2_set_frequency(hz)` that rebuilds +
  sends High-Priority; the keepalive timer already re-sends). No restart needed.
- **Mode/filter:** `demod_set_mode(mode, flo, fhi)` (already stubbed in demod.h
  intent) → `SetRXAMode` + `RXASetPassband` on the running channel (WDSP setters
  are safe live, receiver.c pattern).
- **Zoom/pan:** re-call `SetAnalyzer` with new `fscLin/fscHin` (see
  `WDSP-ANALYZER-SCOPE.md` §3) — narrows the 768 kHz span to something readable.
  Needs the zoom/pan → afft/clip math from receiver.c:1704-1730.

**GTK4 UI:** scroll/drag on the panadapter to tune (map x-pixel → Hz via the
`cA/cB` mapping), keys or a small header bar for mode/filter/band, a VFO readout
that updates. Keep `panadapter.c`/`waterfall.c` pure-Cairo; add input handling in
`gui.c` (GtkGestureClick/Drag, scroll controller).

**Cleanups worth folding in (from M1/M2 known issues):**
- **AGC vs digital gain:** replace `SDRFL_GAIN` with proper AGC-target calibration
  so levels self-balance (why is WDSP's RXA output ~30× low for us? — investigate
  `SetRXAAGC*` / panel gain scaling).
- **Audio clock drift** (ring 1–21 ms): adaptive resample or drop/insert to track
  the soundcard clock.
- **Absolute dBm panadapter cal:** bake a measured `soffset` instead of auto-range.

## Notes

- The GTK4 UI, `panadapter.c` and `waterfall.c` are already in place (v0). Only
  the **data source** changes: network bytes → WDSP float pixels.
- Keep the network client (`client.c`) — it can remain an optional "remote head".
