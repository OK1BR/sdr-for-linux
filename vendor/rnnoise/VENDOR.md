# Vendored RNNoise

RNNoise (recurrent-neural-net noise suppression) is linked by WDSP's `rnnr.c`.
Built from source in-tree; never linked as a prebuilt `.a` from an external path.

## Source of truth

| | |
|---|---|
| Upstream | https://github.com/xiph/rnnoise (Jean-Marc Valin / Xiph) |
| Vendored via | https://github.com/dl1ycf/pihpsdr — subdir `rnnoise/` |
| piHPSDR commit | `974acbac07fe7dd3e24f28f3956a9ffb3a1ebaf1` (`974acba`, 2026-07-03) |
| Vendored on | 2026-07-06 |
| License | BSD (see `COPYING` / `AUTHORS`) — GPL-compatible |

## dl1ycf's changes vs xiph upstream (per `README.DL1YCF`)

- A trimmed `Makefile` that compiles only what piHPSDR needs.
- The 80 MB trained model `src/rnnoise_data.c` was **split** into six
  `rnnoise_data_{1..6}.h` (+ six tiny `rnnoise_data_{1..6}.c` includers) to stay
  under GitHub's 50 MB file limit and to build on 1 GB RaspberryPis. `rnnoise_data.c`
  is now ~16 KB. This is why our `meson.build` compiles those six files.

## What we build

`meson.build` compiles the **exact `SOURCES` of the upstream `Makefile`** (16 `.c`).
Deliberately **not** compiled — training/dump tools with their own `main()`:
`dump_features.c`, `write_weights.c`, `dump_rnnoise_tables.c`.
(`parse_lpcnet_weights.c` *is* built — its `main()` is `#ifdef`-guarded, as
upstream links it into the library.)

Built with `c_std=gnu11` for consistency with the rest of the vendored engine.
A benign `#warning` notes only SSE/SSE2 are enabled; add `-march=` later if the
neural NR needs the throughput (a milestone-2 concern, not RX panadapter).

## Check for upstream updates / re-sync

1. Compare against the pihpsdr tree: `diff -ru vendor/rnnoise <pihpsdr>/rnnoise`
   (ignore `Makefile`, `librnnoise.a`, `*.o`).
2. If changed: re-copy, update commit/date above, re-check the `SOURCES` list in
   `meson.build` against the new `Makefile`, `meson compile`, run the wdsp smoke.
