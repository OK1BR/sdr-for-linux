# Contributing

Thanks for your interest — but please read this before opening anything.

## Pull requests are not accepted

This project does **not** accept pull requests. Every line that lands here
is developed and verified live on real radio hardware, and TX-path code is
subject to strict safety rules ([docs/TX-SAFETY.md](docs/TX-SAFETY.md)) —
a wrong control bit can physically destroy a power amplifier. Unsolicited
PRs will be closed without review, regardless of quality.

## What IS welcome: issues

- **Bug reports** — with your radio model, firmware, distro and steps to
  reproduce. Logs from a terminal run help a lot.
- **Feature requests** — especially from real operating experience
  (contesting, DXing, digimodes).
- **Hardware offers** — the supported-radio whitelist grows only through
  bring-up on real hardware. If you own an Apache Labs / HPSDR Protocol 2
  board that isn't supported yet and can lend it (or run tests with a dummy
  load), open an issue; that's the single most valuable contribution.

## Why so strict?

The project follows piHPSDR's engine philosophy with a deliberately small
surface: vendored, pinned dependencies; no DSP reimplementation; every TX
feature cross-checked against piHPSDR and verified on the air before it
ships. That discipline is incompatible with drive-by code contributions —
but it is exactly why the radio side works. Feedback, testing and hardware
access are where outside help genuinely moves this project forward.

73 de OK1BR
