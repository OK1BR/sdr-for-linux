# Vendored WDSP

WDSP is the DSP engine we build & link — **we do not reimplement DSP**, and we
**do not modify** these sources (warnings are silenced in `meson.build`, never
patched). We keep the full source in-tree rather than linking a prebuilt
`libwdsp.a` from an external checkout, so the build never depends on anything
that can disappear.

## Source of truth

| | |
|---|---|
| Author | WDSP by Warren Pratt (NR0V); Linux/Android port by John Melton (g0orx/n6lyt) |
| Vendored via | https://github.com/dl1ycf/pihpsdr — subdir `wdsp/` |
| piHPSDR commit | `974acbac07fe7dd3e24f28f3956a9ffb3a1ebaf1` (`974acba`, 2026-07-03) |
| Vendored on | 2026-07-06 |
| License | GPL (see `COPYING`) — compatible with this project's GPL-3.0-or-later |

The `.c`/`.h` here are a verbatim copy of `pihpsdr/wdsp/` at that commit.

## What we build

`meson.build` compiles the **exact object set of the upstream `Makefile` (`OBJS`)**
— 69 `.c`. Deliberately **not** compiled:

- `make_zetahat.c` — a generator tool (not in `OBJS`), reads `zetaHat.bin`.

We build via meson (static lib `libwdsp`), **not** the vendored `Makefile` (kept
only as the upstream reference for the source list).

## Gotchas encoded in the build

- **GNU dialect required.** `comm.h` only neutralises Windows `__declspec` under
  `#if defined(linux)`, and the bare `linux` macro exists only in the GNU C
  dialect. So `libwdsp` (and any TU that includes WDSP headers) is built with
  `c_std=gnu11`; strict `-std=c11` produces hundreds of parse errors. Our own
  GTK4 UI stays c11 and must reach WDSP through an engine wrapper, not by
  including WDSP headers directly.
- **WDSP is headless** — `comm.h` pulls no GTK/GLib. That is exactly why the
  engine can live under our GTK4 UI.
- **Runtime data files** `calculus` and `zetaHat.bin` are read by `emnr.c`
  (`fopen("calculus","rb")` from the CWD; `zetaHat.bin` via a path prefix). Only
  the **EMNR** noise-reduction path touches them — irrelevant to the RX
  panadapter (milestone 1), but when EMNR is wired up we must make these findable
  at runtime. They are vendored here alongside the source.
- rnnoise / libspecbleach — `rnnr.c` (`#include "rnnoise.h"`) and `sbnr.c`
  (`#include <specbleach_adenoiser.h>`) link the sibling vendored libs; their
  include dirs come in via `rnnoise_dep` / `specbleach_dep`.

## Check for upstream updates / re-sync

dl1ycf occasionally updates WDSP. To check and re-sync:

1. `git -C /path/to/pihpsdr fetch && git -C /path/to/pihpsdr log --oneline wdsp/`
2. `diff -ru vendor/wdsp <pihpsdr>/wdsp` (ignore `Makefile`, `libwdsp.a`, `*.o`).
3. If changed: re-copy the sources, update the commit/date above, re-check the
   `OBJS` list in `meson.build` against the new `Makefile`, `meson compile`, and
   run `./build/sdrfl-wdsp-smoke`.
