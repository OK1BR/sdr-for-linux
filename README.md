# SDR for Linux

**A modern GTK4 software-defined-radio application for HPSDR / ANAN radios,
built on the proven piHPSDR engine with a new, high-detail user interface.**

`sdr-for-linux` is a GPLv3 fork-in-spirit of
[piHPSDR](https://github.com/dl1ycf/pihpsdr): it reuses piHPSDR's excellent
*engine* — the HPSDR Protocol 1/2 radio link, WDSP DSP, audio, CAT and TCI — and
replaces the front-end with a beautiful, GPU-friendly GTK4 panadapter, waterfall
and control surface. **No DSP is reimplemented**; WDSP does the heavy lifting.

> Status: **early.** The GTK4 front-end (panadapter + waterfall) already runs.
> The direct-to-radio engine is being imported from piHPSDR milestone by
> milestone, RX first.

## Why

piHPSDR is a first-class radio engine, but its panadapter is drawn in Cairo on
the CPU and its controls are functional rather than beautiful. Rather than a
remote head over the network (which only ever receives *rendered* spectrum), this
project runs the engine **in-process**, so it can render the WDSP analyzer's raw
`float` spectrum at full resolution — no 1 dB quantisation, no fixed column cap —
with a modern, elegant UI.

This grew out of [`pihpsdr-client`](https://github.com/OK1BR/pihpsdr-client), a
GTK4 *remote* head for piHPSDR; its Cairo `panadapter`/`waterfall` renderer is
the seed of this application's UI.

## Architecture (target)

```
  Radio (ANAN G1, HPSDR Protocol 2)
        │  Ethernet (raw IQ)
        ▼
  sdr-for-linux  (single GTK4 app)
  ├─ HPSDR Protocol 1/2 link           ┐
  ├─ WDSP DSP (demod, filters, AGC,    │ imported from piHPSDR
  │   NR, S-meter, panadapter FFT)     │ (the "honest work")
  ├─ audio I/O, TX processing, CAT/TCI ┘
  └─ GTK4 UI: panadapter + waterfall + controls   (new)
```

## Roadmap (RX first)

1. **RX panadapter on real hardware:** GTK4 app → HPSDR discovery → receive RX
   IQ → WDSP analyzer → render `rx->pixel_samples` (float) in the panadapter.
2. Demodulation + audio out (WDSP).
3. Control surface in GTK4: VFO, mode, filter, AGC, NR/NB/ANF, zoom/pan, band.
4. RX2; S-meter / TX meters.
5. **TX + PureSignal**, CAT/rigctl, TCI (last).

## Requirements

- An HPSDR radio (e.g. Apache Labs ANAN)
- Linux + GTK4, WDSP, OpenSSL, zlib, Opus

## Credits

- **piHPSDR** — Christoph van Wüllen (DL1YCF) and John Melton (G0ORX) — the
  engine this project builds on.
- **WDSP** — Warren Pratt (NR0V) — the DSP library.

## License

[GPLv3](LICENSE). Builds on and derives from piHPSDR and WDSP, both GPLv3.

## Author

Richard Fakenberg — **OK1BR**
