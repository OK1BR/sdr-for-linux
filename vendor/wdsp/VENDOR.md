# Vendored WDSP

WDSP is the DSP engine we build & link — **we do not reimplement DSP**, and we
**do not modify** these sources (warnings are silenced in `meson.build`, never
patched). We keep the full source in-tree rather than linking a prebuilt
`libwdsp.a` from an external checkout, so the build never depends on anything
that can disappear.

## Source of truth

| | |
|---|---|
| Author | WDSP by Warren Pratt (NR0V); Linux/Android port by John Melton (g0orx/n6lyt), maintained by Christoph van Wüllen (DL1YCF) |
| Upstream of upstream | https://github.com/TAPR/OpenHPSDR-wdsp (release notes in its README) |
| Vendored via | https://github.com/dl1ycf/pihpsdr — subdir `wdsp/` |
| WDSP version | **2.11** (`GetWDSPVersion()` = 211; TAPR release 2026-10-15) |
| piHPSDR commit | `3b63530267d7ea4f855a6788ba61f6aaddd25b5a` (`3b63530`, 2026-10-19; the last commit touching `wdsp/` is `a91e638`, same day) |
| Vendored on | 2026-10-26 (issue #22; previously 1.29 from `974acba`, vendored 2026-07-06) |
| License | GPL (see `COPYING`) — compatible with this project's GPL-3.0-or-later |

The `.c`/`.h` and data files here are a verbatim copy of `pihpsdr/wdsp/` at that
commit (`git archive 3b63530 wdsp`).

## What we build

`meson.build` compiles the **exact object set of the upstream `Makefile`
(`OBJS`)** — 82 `.c`, the list is generated from the Makefile, not hand-kept.
Deliberately **not** compiled (not in `OBJS`):

- `make_calculus.c`, `make_interface.c`, `make_zetahat.c` — generator tools
  (they produce `calculus.c`, `wdsp.h`, `zetaHat.c`).
- `snoop.c` — a developer tap.

We build via meson (static lib `libwdsp`), **not** the vendored `Makefile` (kept
only as the upstream reference for the source list).

## What 2.x changed for us (re-sync 2026-10-26, issue #22)

- **PureSignal 3.0** (NURBS calibrator): `SetPSMapMode`, `SetPSPinMode`,
  `SetPSPtol`, `SetPSStabilize`, `SetPSIntsAndSpi` are gone from the API.
  piHPSDR 3.1 sets exactly `SetPSHWPeak` / `SetPSMoxDelay` / `SetPSTXDelay` /
  `SetPSLoopDelay`; so does `src/engine/ps.c`. The `GetPSInfo` layout we read
  (info[4] feedback, [5] calibration count, [6] sanity bits, [14] correcting,
  [15] state with LRESET = 0) is unchanged.
- **RNNoise is gone** (`rnnr.c` removed upstream; piHPSDR 3.1 dropped its
  vendored `rnnoise/`, and so did we). The NR3 slot is now WDSP's own neural
  **NNR** (`SetRXANNRRun/Model/MaskFloor`), with four built-in models compiled
  from `nnr_model_0..3.c` (~57 MB of generated C: 0 Standard, 1 Premium, 2/3
  the CW-trained variants; `nnet.c` loads an external `wdsp_nnr_N.bin` from the
  working directory instead when one exists). The models compile in well under
  a second each (24 MB → 4.7 MB object, 72 MB RSS). Every RXA `OpenChannel`
  parses all four (prints ~28 `nnio:`/`nnet:` lines on stdout — WDSP's
  `dprintf` is `printf`). CPU, release build, per RX channel, independent of
  the AF rate: Standard ≈ 10 %, Premium ≈ 20 % of one core (`sdrfl-nnr-test`).
- **`SetChannelState(ch, 0, 1)` before `CloseChannel(ch)`** (2.11 rule): done
  in `demod_destroy()` and `tx_dsp_destroy()`. With the feed already stopped it
  times out after 100 ms (channel.c), no deadlock.
- **Wisdom**: the cache file is now `wdspWisdom01` (was `wdspWisdom00`), the
  plan set is c2c fwd/bwd/bwd+1 then r2c **and c2r** for 64..262144 (one more
  plan per size than 1.29), and `WDSPwisdom()` returns -1 on a bad directory.
  `src/wisdom_gate.c` follows all three; a fresh cache is built once per FFTW
  build on the first start after the update.
- `wdsp.h` is now auto-generated (`make_interface.c`) and carries the enums;
  `wdsp_align.h` is new. `comm.h` still neutralises `__declspec` only under
  `#if defined(linux) || defined(__APPLE__)` — the GNU dialect stays required.
- `emnr.c` now has compiled-in copies of the `calculus` and `zetaHat.bin` data
  (`calculus.c`, `zetaHat.c`); the data files are optional (read only when a
  path is configured), so nothing has to be installed next to the binary.
- New in 2.x and not yet used by us (tickets): free-curve EQ/CFC/post-EQ
  (#27, the EQ milestone), phase rotator + optimizer (#23), CW APF (#24), FM
  (#39, deferred), uSlew (#37), varsamp (#38), VOX in DEXP (#33), NNR tuning
  setters beyond model/floor (#28).

## Gotchas encoded in the build

- **GNU dialect required.** `comm.h` only neutralises Windows `__declspec` under
  `#if defined(linux) || defined(__APPLE__)`, and the bare `linux` macro exists
  only in the GNU C dialect. So `libwdsp` (and any TU that includes WDSP
  headers) is built with `c_std=gnu11`; strict `-std=c11` produces hundreds of
  parse errors. Our own GTK4 UI stays c11 and must reach WDSP through an engine
  wrapper, not by including WDSP headers directly. `-D_GNU_SOURCE` is
  upstream's own CFLAG and is passed too.
- **WDSP is headless** — `comm.h` pulls no GTK/GLib. That is exactly why the
  engine can live under our GTK4 UI.
- libspecbleach — `sbnr.c` (`#include <specbleach_adenoiser.h>`) links the
  sibling vendored lib; its include dir comes in via `specbleach_dep`.

## Check for upstream updates / re-sync

dl1ycf updates WDSP a few times a year (TAPR releases: 2.00 2026-07-01, 2.10
2026-09-04, 2.11 2026-10-15). To check and re-sync:

1. `git -C ~/.local/opt/pihpsdr fetch origin && git -C ~/.local/opt/pihpsdr log --oneline origin/master -- wdsp/`
   (⛔ leave that checkout's working tree on `974acba` — ~40 source comments
   cite its line numbers; read newer trees with `git show <rev>:path`).
2. `git -C ~/.local/opt/pihpsdr archive <rev> wdsp | tar -x -C /var/tmp/<scratch>`,
   then `diff -ru vendor/wdsp /var/tmp/<scratch>/wdsp` (ignore `meson.build`,
   `VENDOR.md`).
3. If changed: re-copy the sources (old tree to the trash), update the
   commit/date/version above, **regenerate the source list in `meson.build`
   from the new `Makefile` `OBJS`**, diff `wdsp.h` against the previous one for
   every function `src/` calls (names AND signatures), `meson compile`, and run
   `./build/sdrfl-wdsp-smoke` (pins the version) plus the offline gates
   (`sdrfl-txdsp-test`, `sdrfl-p2dev-test`, `sdrfl-nnr-test`,
   `sdrfl-wisdom-test` in a throwaway `SDRFL_WISDOM_DIR`). A re-sync that
   touches calcc/TXA is a TX-path change: `docs/TX-SAFETY.md` applies, the live
   pass on the radio (RX, TX into the dummy load, PureSignal recalibration) is
   part of the ticket.
