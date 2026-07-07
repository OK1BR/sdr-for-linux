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

## Milestone 3 — on-window controls (mostly DONE, 2026-07-07)

The GTK4 window is a live **libadwaita** control surface (framework decision +
mockups: `docs/mockups/`). Done this session, all live-verified on the ANAN G1:
- **Tuning:** scroll-wheel (Ctrl 10 Hz / Shift 1 kHz / else 100 Hz) + left-drag
  pan → `p2_set_frequency()` (stores freq; keepalive timer sends it ≤100 ms, so
  no send-side concurrency). Model A: VFO = DDC centre = span centre.
- **Mode:** segmented USB/LSB/CWL/CWU/AM toggles + keys u/l/c/a (kept in sync) →
  `demod_set_mode()`.
- **Filter:** piHPSDR named presets per mode (from `filter.c`) in a dropdown →
  `demod_set_passband()` live; **passband drawn on the spectrum** (shaded band +
  edges + VFO centre line, scales with zoom). Var1/Var2 (draggable edges) NOT yet.
- **Band buttons** 160–10 m → jump the VFO.
- **Zoom:** stepped ×2 +/- buttons in a footer (`AdwToolbarView` bottom bar).
  `analyzer_set_zoom()` re-clips `fscLin/fscHin`; FFT grows with zoom (`A_MSIZE`
  262144) for sharp zoom to 128×; 1 Hz PSD norm → zoom-invariant floor.
- **AF volume** slider; **settings dialog** (`AdwPreferencesDialog` from the
  headerbar menu): IP / sample-rate (incl. **1536 kHz**, the P2 ceiling) /
  latency = restart-to-apply; digital gain + **FPS** apply live.
- **Persistence:** `~/.config/sdr-for-linux/config.ini` (GKeyFile) — ip/freq/
  rate/mode/volume/gain/fps/latency; precedence env > saved > default; saved
  debounced (~1 s) + on clean exit.

**★ TOP OF NEXT SESSION — deep-zoom performance.** Zoom still **stutters at the
larger factors** (worst at 1536 kHz; ~1 core already at 1536 k zoom=1). Growing
the FFT to 262144 for a sharp deep zoom is too heavy. **Profile first**: is it the
per-click `SetAnalyzer` reconfig freeze, or the steady large-FFT load each frame?
Then pick — cap `A_MSIZE` lower / accept interpolation past a point / drop fps when
zoomed / narrow the DDC sample-rate instead of the FFT. See `analyzer.c`
`analyzer_set_zoom` + the gui.c footer (`on_zoom_in/out`).

**Other M3 follow-ups:**
- **Var1/Var2 filters** — drag the passband edges on the spectrum (hit-test + drag).
- **AGC / NR / NB / ANF** — still visible placeholders in the top strip; wire to
  WDSP (`SetRXAAGC*`, rnnoise/specbleach).
- **Off-centre pan** (currently pan=0, VFO always centred).

**Cleanups (from M1/M2):** AGC-target vs `SDRFL_GAIN`; audio clock-drift
smoothing; absolute dBm panadapter cal.

## Notes

- The GTK4 UI, `panadapter.c` and `waterfall.c` are already in place (v0). Only
  the **data source** changes: network bytes → WDSP float pixels.
- Keep the network client (`client.c`) — it can remain an optional "remote head".
