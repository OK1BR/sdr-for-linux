# SDR for Linux — project context

Instructions and context for Claude Code working in this repo. (Richard's global
`~/.claude/CLAUDE.md` rules also apply — consent before major/irreversible changes, etc.)

## What this is

A modern **GTK4 SDR application** for HPSDR / ANAN radios — a GPLv3 fork-in-spirit
of **piHPSDR**. Reuse piHPSDR's *engine* (HPSDR Protocol 1/2 radio link, **WDSP**
DSP, audio, CAT/rigctl, TCI, TX/PureSignal) and put a **new GTK4 UI** on it.
**Do NOT reimplement DSP — link WDSP.** Grew out of
[`pihpsdr-client`](https://github.com/OK1BR/pihpsdr-client) (a GTK4 *remote head*
for piHPSDR); its pure-Cairo `panadapter`/`waterfall` renderer seeds this UI.

## Status (2026-07-06)

**v0**: seeded from pihpsdr-client. Builds, and renders the panadapter + waterfall
over the piHPSDR **client/server network path** (`src/client.c`).

**Milestones 1 & 2 done** — you can **see and hear** the radio, direct from
the G1, in the GTK4 window. The headless GLib-only engine under `src/engine/`:
builds WDSP (vendored `vendor/wdsp` + rnnoise + libspecbleach), discovers the
radio (`discovery_p2.c`), runs one RX DDC over Protocol 2 and streams IQ
(`protocol2.c` → `on_rx_iq`), feeds the WDSP analyzer for the float panadapter
(`analyzer.c`), **and** a WDSP demod channel (`demod.c`) whose audio goes to a
native PipeWire sink (`audio_pw.c`) at ~15 ms latency. The app (`sdr-for-linux`)
defaults to direct radio; `--server` is the v0 network remote head. Gates:
`sdrfl-wdsp-smoke`, `sdrfl-discover`, `sdrfl-rxprobe`, `sdrfl-panprobe`,
`sdrfl-audioprobe` (all verified live on the ANAN G1 at 192.168.1.247). Scope
docs: [`docs/ENGINE-IMPORT.md`](docs/ENGINE-IMPORT.md), `docs/P2-RX-SCOPE.md`,
`docs/WDSP-ANALYZER-SCOPE.md`, `docs/AUDIO-SCOPE.md`.

**Milestone 3 mostly done (2026-07-07)** — the window is a live **libadwaita**
control surface: scroll/drag tuning, mode toggles + keys, piHPSDR filter presets
with the passband drawn on the spectrum, band buttons, AF volume, a footer zoom
(+/- ×2), a settings dialog (`AdwPreferencesDialog`; live FPS/gain, restart IP/
rate incl. **1536 kHz** P2 max), and config persistence (`~/.config/sdr-for-linux/
config.ini`). Framework decision + throwaway design mockups in `docs/mockups/`.

**Deep-zoom stutter fixed (2026-07-07)** — profiled: it was **not** the FFT
compute (one 262144 transform = 0.5 % of a core) and **not** multithread-able
(a single 1D FFT threads to only ~1.9×, erratically). The stutter was WDSP
re-planning FFTW with `FFTW_PATIENT` whenever a zoom step crossed an FFT-size
band — **26 s** to plan 262144 cold. Fix = FFTW **wisdom**, exactly like
piHPSDR/Thetis: `src/wisdom_gate.c` `wisdom_ensure()` builds the plan cache once
on first run (`~/.config/sdr-for-linux/wdspWisdom00`, ~6 min, progress window)
and imports it instantly ever after (verified: 2nd run 0.0 s). Gate:
`sdrfl-wisdom-test` (offline, no radio). No `vendor/wdsp` edits.

**★ Next session starts with:** Var1/Var2 filters (draggable passband edges),
wire AGC/NR/NB/ANF (still placeholders), off-centre pan. See ENGINE-IMPORT.md
"Milestone 3". Older follow-ups: AGC-target vs `SDRFL_GAIN`, audio clock-drift
smoothing, absolute dBm cal.

## Approach (decided with Richard)

**Full GTK4.** Build a new GTK4 app (this repo) and **import the piHPSDR engine
incrementally, milestone by milestone** — do NOT mechanically port piHPSDR's GTK3
widgets (that path is months of non-compiling limbo). The engine is GLib-based,
and **GLib is shared across GTK3/GTK4**, so engine files compile under GTK4. The
core real work is disentangling the **engine ↔ GUI boundary**.

## Vendoring policy (decided with Richard)

Third-party code we depend on comes in one of two ways, and the line is
deliberate:

- **Domain / niche libraries** (things that could vanish from GitHub or that we
  need a *specific* version of): WDSP, rnnoise, libspecbleach, and — as they are
  imported — piHPSDR engine files. These are **vendored in-tree as source** under
  `vendor/`, **built from that source** (never linked from an external checkout
  or a prebuilt `.a`), and **not modified**. Each `vendor/<lib>/` carries a
  `VENDOR.md` recording upstream origin, the pinned commit, the date vendored,
  what we build vs skip, and **how to check upstream for updates**. Periodically
  re-check upstream and re-sync. The repo must `git clone && meson compile` with
  no external source.
- **Ubiquitous platform libraries** (gtk4, fftw3, opus, openssl, zlib, the
  toolchain/libc): taken from the distribution — they will not disappear, and we
  *want* the distro's security-patched versions. Not vendored; listed as build
  requirements in the README and enforced by meson.

New radios / features: prefer taking proven open source from outside (vendored
per the above) over reimplementing.

## Key technical facts (verified against piHPSDR src @ 974acba)

- The panadapter data is `rx->pixel_samples` — a **`float` array** (`rx->pixels`
  long), filled by the WDSP analyzer. In-process this gives **full float
  resolution, no 1 dB quantisation and no 4096-column cap** (set `rx->pixels`
  freely). This is what our panadapter will render. (v0 currently renders network
  bytes where `dBm = byte − 200`.)
- piHPSDR draws with **Cairo**; our `panadapter.c` / `waterfall.c` are pure Cairo
  → already GTK-version-agnostic.
- **GTK3 and GTK4 cannot coexist in one process.** piHPSDR is GTK3.
- **WDSP** is a separate GPL library (piHPSDR builds it from its `wdsp/` subdir).

## ⛔ TX safety — protect the transceiver. NON-NEGOTIABLE.

A wrong relay/filter state on RX costs sensitivity (see the −45 dB ANT-relay
bug, c4b9243); **on TX it can physically destroy the PA or feed transmit RF
back into the RX input**. Every current and future agent MUST treat the TX
path as hazardous and observe, without exception:

- **Any change touching the TX path** — MOX, PA-enable (general[58]), drive,
  TX-specific packet, or the Alex words (ANT relays, LPF/BPF selection, T/R
  relay, step attenuators) — must be **re-verified against piHPSDR
  (`~/.local/opt/pihpsdr`, @974acba) and [`docs/TX-SAFETY.md`](docs/TX-SAFETY.md)
  every time**, not assumed correct from an earlier session. Repeat the check
  whenever these bytes are edited, moved, or re-ordered.
- **Consistency is atomic**: one high-priority packet always carries a mutually
  consistent {MOX, ANT, LPF, BPF, attenuator} set. Never key with a stale LPF
  or an open antenna relay; never split that state across packets.
- **SWR must be evaluated during TX** (ALEX fwd/rev sensors), and on high SWR
  the software must **automatically reduce drive to zero and/or refuse TX**
  (piHPSDR: alarm ≥ 3.0 in two consecutive readings → drive 0). TX must also
  be refused out-of-band and with PA disabled.
- **Protect the RX input during TX**: both step attenuators to 31 dB while
  transmitting + correct T/R relay state, so transmitted RF can never return
  to the ADC/RX input path.
- TX-capable code lands **only with Richard's explicit consent** and only with
  the full `docs/TX-SAFETY.md` checklist satisfied — it is the acceptance
  criteria list, not a suggestion. Until then the three no-TX guarantees
  (no MOX bit, PA-enable 0, zeroed TX-specific) must never be weakened.

## Roadmap (RX first)

1. GTK4 app → HPSDR discovery → **RX IQ → WDSP analyzer → our panadapter** on the
   real radio (float, full resolution).
2. Demodulation + audio out (WDSP).
3. Control surface in GTK4: VFO, mode, filter, AGC, NR/NB/ANF, zoom/pan, band.
4. RX2; S-meter / TX meters.
5. **TX + PureSignal**, CAT/rigctl, TCI (last, highest risk).

**Next concrete step** (does NOT need the radio free): get **WDSP into the meson
build** + vendor **Protocol-2 discovery** (find the radio on the LAN). See
[`docs/ENGINE-IMPORT.md`](docs/ENGINE-IMPORT.md).

## Hardware & dev/test

- **Radio:** Apache Labs **ANAN G1**, HPSDR **Protocol 2**, at **192.168.1.247**
  on the LAN. Richard's dev machine: 192.168.1.18.
- **piHPSDR source** (the engine reference to import from):
  `/home/rfa/.local/opt/pihpsdr`, git **974acba** (github.com/dl1ycf/pihpsdr),
  `CLIENT_SERVER_VERSION 0x01300005`. A prebuilt `pihpsdr` binary runs there, so
  the build (incl. WDSP) already works on this Arch machine.
- **One owner of the HPSDR hardware at a time.** To develop direct-radio mode,
  piHPSDR must be **disconnected from the radio / closed** first.
- **v0 network test server:** Richard runs piHPSDR in *server mode* on
  `127.0.0.1:50000` (PCM). Password is set in its Server menu; piHPSDR **rejects
  any password shorter than 5 chars** (`server_thread.c:1107`) — current test
  password is **`Test5`**. The app reads it from `$PIHPSDR_PWD`.

## Build & run

```sh
meson setup build && meson compile -C build
# GUI, DIRECT RADIO (default): discovery → P2 RX → WDSP analyzer → panadapter.
# TAKES THE RADIO (piHPSDR must be closed). GSK_RENDERER=cairo avoids the
# NVIDIA+Wayland GTK4 GL crash. Env: SDRFL_RADIO_IP/SDRFL_FREQ/SDRFL_RATE.
GSK_RENDERER=cairo ./build/sdr-for-linux
# GUI, network remote head onto a running piHPSDR server (v0 path):
GSK_RENDERER=cairo PIHPSDR_PWD='Test5' ./build/sdr-for-linux --server 127.0.0.1 50000
# headless engine gates (need the radio free): IQ probe / analyzer→panadapter PNG
./build/sdrfl-rxprobe
RENDER_OUT=/tmp/pan.png ./build/sdrfl-panprobe    # SDRFL_SELFTEST=1 = no radio
# headless network render check (writes a PNG, no window):
PIHPSDR_PWD='Test5' RENDER_OUT=/tmp/pan.png ./build/sdrfl-render-test
```

## Gotchas (learned the hard way in pihpsdr-client)

- **Do NOT send `CMD_SCREEN` / `CMD_RX_FPS`** to the piHPSDR *server*: they
  reconfigure the operator's **shared live display**, and `CMD_SCREEN` physically
  **resizes his piHPSDR window**. Consume the server's native width/fps. (Moot
  once the engine is in-process — you own `rx->pixels` then.)
- **Keepalive:** the server drops a client silent on TCP for 30 s → send
  `CMD_PING` ~1×/s and continuously drain the TCP stream. `client.c` already does.
- The network server serves **one client at a time** — stop the GUI before
  running the render-test.
- After a client disconnects the server needs a moment before it `accept()`s
  again → a transient **"Connection refused"** on immediate reconnect; just retry.
- Never `pkill -f sdr-for-linux` (matches your own shell's command line) and never
  `pkill pihpsdr` (would kill the *radio server*). Kill by exact `comm` or PID.

## Working with Richard

- **Communicate in Czech.** He is **OK1BR**, an experienced radio amateur and
  developer; knows ham terminology. Wants **argumentation and comparison of
  variants**, technical detail. Get consent before major/irreversible steps.
- **Push to GitHub regularly** (`git push origin main`). Repo:
  **OK1BR/sdr-for-linux** (public).
- Sibling repo **OK1BR/pihpsdr-client** — the remote-head client; its code seeded
  this project and it stays as a bonus "remote head".
