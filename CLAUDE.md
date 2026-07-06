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
over the piHPSDR **client/server network path** (`src/client.c`). The
direct-to-radio engine is **not yet imported**.

## Approach (decided with Richard)

**Full GTK4.** Build a new GTK4 app (this repo) and **import the piHPSDR engine
incrementally, milestone by milestone** — do NOT mechanically port piHPSDR's GTK3
widgets (that path is months of non-compiling limbo). The engine is GLib-based,
and **GLib is shared across GTK3/GTK4**, so engine files compile under GTK4. The
core real work is disentangling the **engine ↔ GUI boundary**.

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
# GUI (GSK_RENDERER=cairo avoids the NVIDIA+Wayland GTK4 GL crash):
GSK_RENDERER=cairo PIHPSDR_PWD='Test5' ./build/sdr-for-linux
# headless render check (writes a PNG, no window — good for visual checks):
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
