# Milestone 2 — demodulation + audio: scope plan

Demodulate the live RX IQ and play it. The same IQ that feeds the analyzer
(`protocol2.c`'s `on_rx_iq`) now also feeds a WDSP **demod channel**
(`fexchange0`), whose 48 kHz audio goes to a PulseAudio sink. Reference values
from piHPSDR `receiver.c` @ 974acba; audio pattern from its `pulseaudio.c`. No
code written yet — consent before the import (as with M1).

---

## 0. Milestone gate

Two gates:
- `sdrfl-audioprobe` (headless): discover → P2 RX → demod one signal → play a few
  seconds through PulseAudio. Proves IQ → demod → audio end-to-end.
- Then wire audio into the GUI so the live window **plays** the tuned signal.

Needs the radio free (takes the TRX). The demod code is offline; only hearing it
needs the radio.

---

## 1. Headline finding — demod reuses the analyzer's feed

The WDSP **demod channel** is the same `disp`/channel id as the analyzer, driven
by the **same 1024-sample IQ block**. piHPSDR's `rx_full_buffer` (receiver.c:1307)
does, per block, on one `iq_input_buffer`: `fexchange0` (→ audio) **and**
`Spectrum0` (→ panadapter). So demod is a *second consumer* of the feed we
already have — add `OpenChannel` + a handful of `SetRXA*` setters + `fexchange0`,
and route the audio out. No new IQ plumbing.

Audio: **PulseAudio simple API (`libpulse-simple`)** — piHPSDR's own default Linux
backend, works on Arch/PipeWire via `pipewire-pulse`, 48 kHz float32, blocking
`pa_simple_write` = natural pacing. Ubiquitous distro lib (not vendored, per
policy). *(decision A below.)*

---

## 2. Architecture (proposed) — two lean modules + an audio thread

Consistent with M1's Option B: small in-house modules, own state.

`src/engine/demod.c` / `demod.h` — WDSP channel wrapper:
```c
int  demod_create(int id, int in_rate, int mode, double flo, double fhi, double volume);
void demod_feed(const double *iq, int n_pairs);  // accumulate 1024 → fexchange0 → audio
void demod_set_mode(int mode, double flo, double fhi);
void demod_destroy(void);
```
`demod_feed` mirrors `analyzer_feed`: accumulate incoming pairs, and every 1024
call `fexchange0(id, iq_buf, audio_buf, &err)` → `output_samples`(=256 @192k)
interleaved L/R doubles → convert to float → hand to the audio sink.

`src/engine/audio_pa.c` / `audio.h` — PulseAudio sink **with its own thread**:
```c
int  audio_start(int rate, int channels);
void audio_push(const float *interleaved, int frames);  // non-blocking (ring)
void audio_stop(void);
```
`audio_push` drops into a lock-light ring buffer (non-blocking; on overrun drop
oldest). A drain thread pops the ring and does the **blocking** `pa_simple_write`.

**Threading (the crux).** `on_rx_iq` runs on the P2 listener thread. There:
- `analyzer_feed` → `Spectrum0` (memcpy, non-blocking) — already fine.
- `demod_feed` → `fexchange0` (blocks only on WDSP's DSP compute, ~ms) — fine.
- audio: `demod_feed` calls `audio_push` (non-blocking). The **blocking**
  `pa_simple_write` runs on the audio thread, NOT the listener — otherwise
  realtime audio backpressure would stall `recvfrom` and drop IQ packets.

So: listener stays real-time (recvfrom + decode + Spectrum0 + fexchange0 + ring
push); the audio thread absorbs the 48 kHz pacing.

---

## 3. WDSP demod call sequence (values from receiver.c)

**Create (once):**
```c
OpenChannel(id, 1024, 2048, in_rate, 48000, 48000, 0, 1, 0.010,0.025,0.0,0.010, 1);
SetRXABandpassRun(id, 1);
SetRXAPanelRun(id, 1);
SetRXAPanelSelect(id, 3);            // use both I and Q
SetRXAMode(id, mode);               // LSB0 USB1 CWL3 CWU4 AM6 …
RXASetPassband(id, flo, fhi);       // SSB e.g. 150..2850; CW offset by ±sidetone
SetRXAAGCMode(id, 5);               // medium AGC on
SetRXAAGCAttack(id, 2); SetRXAAGCDecay(id, 250);
SetRXAAGCHang(id, 0); SetRXAAGCSlope(id, 35); SetRXAAGCTop(id, 80.0);
SetRXAPanelGain1(id, pow(10, 0.05*volume));   // volume dB (−20 → ~0.1)
// channel already runs (OpenChannel state=1)
```
`output_samples = 1024 / (in_rate/48000)` = 256 @192k; `audio_output_buffer` is
`2*output_samples` doubles. `fexchange0` blocks (bfo=1) until a full audio block
is ready, then writes interleaved L/R (mono demod → L=R).

**Mode/filter defaults for a first listen:** pick from frequency — **LSB below
10 MHz, USB at/above** (ham convention) — with `SDRFL_MODE` to override (SSB/CW).
SSB passband from the standard table (USB 150..2850, LSB −2850..−150); CW =
SSB-demod with the passband shifted by ±`sidetone` (e.g. 600 Hz) so the beat note
is audible. Skip NB/NR/notch/squelch for v1 (all default off).

**Destroy:** `CloseChannel(id)`.

---

## 4. Audio sink (PulseAudio simple)

- `pa_simple_new(NULL, "sdr-for-linux", PA_STREAM_PLAYBACK, NULL, "RX", &ss, NULL,
  &attr, &err)` with `ss = {PA_SAMPLE_FLOAT32NE, 48000, channels}`. Pass `NULL`
  device → the user's default sink (PipeWire routes it, respects per-app volume).
- `attr.tlength = ~200 ms` target latency (fine for RX), like piHPSDR
  (`AUDIO_LAT_TARGET`).
- Mono is enough for SSB/CW: open `channels=1` and push the mono demod, or open
  stereo and duplicate L=R (piHPSDR duplicates). Plan: **mono** (simpler, half the
  data).
- Drain thread: pop ring → `pa_simple_write(handle, buf, frames*sizeof(float))`
  (blocks = pacing). Optional drift guard later (piHPSDR drops blocks when latency
  exceeds a max); skip for v1.

---

## 5. New files & build

| File | Role |
|---|---|
| `src/engine/demod.c` / `demod.h` | WDSP channel wrapper (OpenChannel + setters + fexchange0). |
| `src/engine/audio_pa.c` / `audio.h` | PulseAudio-simple sink + drain thread + ring. |
| `src/audioprobe_main.c` | headless gate: discover → P2 RX → demod → play. |
| `meson.build` | `libpulse-simple` dep; add `demod.c`/`audio_pa.c` to the engine + app; new `sdrfl-audioprobe` target (gnu11, wdsp_dep + pulse). |
| GUI wiring | `gui.c`/`start_radio`: also `demod_create` + `audio_start`, and `feed_cb` calls `demod_feed` alongside `analyzer_feed`; cleanup stops both. |

## 6. Risks & mitigations
- **Audio stalls the listener → IQ packet loss.** Mitigated by the audio thread +
  non-blocking `audio_push` (§2). Only `fexchange0` (short compute) stays inline.
- **Clock drift** (radio 48k vs sound card): ring absorbs jitter; if it grows,
  drop oldest. A latency-based block-drop (piHPSDR's trick) is a later refinement.
- **Wrong mode/sideband = garbled/silent audio.** Default by band + `SDRFL_MODE`;
  verify by ear on a known SSB/CW signal.
- **Radio ownership.** Hearing it needs the TRX free — ask Richard, don't kill
  piHPSDR. Build/wrapper work is offline.

## 7. Decisions (confirmed with Richard 2026-07-06)
- **A. Audio backend = NATIVE PipeWire (`pw_stream`)** — goal is minimum latency.
  `pw_thread_loop` + `pw_stream` with `PW_KEY_NODE_LATENCY` low (small quantum),
  on-demand `on_process` callback (no blocking write). Target ~15–30 ms
  end-to-end. Audio sink is where most latency lives, so this is the payoff.
  The `audio_pw.c` module hides all of it behind the `audio_start/push/stop` seam.
- **A2. IQ sample rate = 768 kHz** — block latency 1024/768k = 1.3 ms (vs 5.3 ms
  at 192k). Costs ~4.6 MB/s on the LAN and more CPU; fall back to 384k if the LAN
  or CPU struggles. Panadapter span becomes 768 kHz (wide; zoom comes later).
- **B. Lean in-house `demod` + `audio_pw` modules** (own state), like M1.
- **C. Gate = new `sdrfl-audioprobe`** then GUI wiring. Mono audio for v1.

### Threading with native PipeWire
- `demod_feed` (P2 listener thread): `fexchange0` → mono float → `audio_push`
  into a **lock-free SPSC ring** (non-blocking; drop/overwrite on overflow).
- `on_process` (PipeWire RT thread): pop the ring into the PW buffer; on underrun
  emit silence. Never lock in the RT callback. Ring kept short (a few quanta) to
  hold latency down.

## 8. Implementation order (after consent)
1. `audio_pa.{c,h}`: PulseAudio sink + drain thread + ring; a tiny tone self-test
   (play a sine, no radio) to prove sound comes out.
2. `demod.{c,h}`: OpenChannel + setters + `fexchange0` → audio_push.
3. `audioprobe_main.c` + meson; compile clean.
4. **(live, radio free)** run → hear a signal; verify mode/sideband.
5. Wire into `gui.c` so the live window plays what it shows.

Then RX is a usable receiver (see + hear). Next: on-window controls (VFO/mode/
filter/zoom), then RX2, then TX.
