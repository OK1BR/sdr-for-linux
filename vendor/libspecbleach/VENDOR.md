# Vendored libspecbleach

Spectral-bleaching noise reduction, linked by WDSP's `sbnr.c`. Built from source
in-tree; never linked as a prebuilt `.a` from an external path. Needs `fftw3f`.

## Source of truth

| | |
|---|---|
| Upstream | https://github.com/lucianodato/libspecbleach |
| Upstream commit | `52660fa1dbe41991cf420b9d60648f7563b9717e` (per `README.DL1YCF`) |
| Vendored via | https://github.com/dl1ycf/pihpsdr — subdir `libspecbleach/` |
| piHPSDR commit | `974acbac07fe7dd3e24f28f3956a9ffb3a1ebaf1` (`974acba`, 2026-07-03) |
| Vendored on | 2026-07-06 |
| License | LGPL v2.1 (see `LICENSE`) — GPL-compatible |

## dl1ycf's changes vs lucianodato upstream (per `README.DL1YCF`)

Added a trimmed `Makefile` (+ `README.DL1YCF`) that builds only the files
piHPSDR needs; tests, examples and the upstream meson build were omitted.

## What we build

`meson.build` compiles the **exact `SOURCES` of the upstream `Makefile`** (22 `.c`).
Present in the tree but deliberately **not** built (not in `SOURCES`):
`src/processors/denoiser/spectral_denoiser.c`,
`src/shared/noise_estimation/noise_estimator.c`, `.../noise_profile.c`,
`src/shared/utils/spectral_trailing_buffer.c`.

Built with `fftw3f` + `c_std=gnu11`.

## Check for upstream updates / re-sync

1. `diff -ru vendor/libspecbleach <pihpsdr>/libspecbleach`
   (ignore `Makefile`, `libspecbleach.a`, `*.o`).
2. If changed: re-copy, update commits/date above, re-check the `SOURCES` list in
   `meson.build` against the new `Makefile`, `meson compile`, run the wdsp smoke.
