# Milestone 1 · Step 3 — Protocol-2 RX + IQ stream: scope plan

Scope for importing the **RX half** of piHPSDR's Protocol-2 link into
`sdr-for-linux`. Get the ANAN G1 started over P2, run **one** DDC (receiver), and
deliver the RX **IQ stream** into a buffer we own. Source of truth:
`/home/rfa/.local/opt/pihpsdr` @ `974acba`. All upstream line numbers below are in
`src/new_protocol.c` unless another file is named.

Prepared by mapping `new_protocol.c` / `new_protocol.h` and the state it reads
(`radio.h`, `receiver.h`, `adc.h`, `vfo.h`, `discovered.h`). **No code written
yet — this is for consent before the import** (same gate as WDSP).

---

## 0. Milestone gate

`sdrfl-rxprobe` (headless, like `sdrfl-discover`): **discover → start radio → set
one RX (192 kHz @ 14.1 MHz) → collect IQ ~1 s → print sample count, effective
rate, RMS, a few samples.** Proves live IQ end-to-end. This gate needs **no
WDSP** — the seam where piHPSDR hands samples to WDSP is replaced by our own
callback that just accumulates statistics. Step 4 later swaps that callback for
the WDSP analyzer feed.

---

## 1. Headline finding — this is smaller than the 3007 lines suggest

`new_protocol.c` is globals-heavy but the **RX-only** path touches very little of
it:

- It reads exactly **two `RECEIVER` fields** to build packets: `adc` and
  `sample_rate` (`receiver.h:47,64`). The `RECEIVER*` is otherwise passed
  **opaquely** to WDSP via `rx_add_iq_samples()` — which we replace.
- From `radio` it reads **only `radio->network.{interface_address,
  interface_length, address, address_length}`** (socket setup, ~600–652) — all
  already populated by our discovery.
- The rest is flat scalars in `radio.c` (`device`, `protocol`, `receivers`,
  `n_adc`, …) and `adc[0]` / `vfo[0]`, most of which are **stub-to-0** for RX.
- Everything TX / PureSignal / diversity / duplex / Saturn-XDMA collapses to dead
  code once `radio_is_transmitting()` is a constant `0` and
  `have_saturn_xdma = 0`.

So the RX engine is a few hundred lines of live logic wrapped in a lot of TX/PS
code we never reach.

---

## 2. Architectural decision (needs Richard's OK) — how to import

Two ways to bring in the P2 RX link. They differ in how faithfully we copy
`new_protocol.c`.

### Option A — vendor `new_protocol.c` near-verbatim, then stub + trim
Copy the file (GPL header intact, like `discovery_p2.c`), provide ~25 globals and
a dozen stub functions (`band_get_band`, `vfo_get_tx_vfo/mode`, `rx_add_iq_samples`,
`saturn_*`, `tx_add_*`, `update_action_table`…), `#define`/delete the TX/PS/DIV/
Saturn branches.
- **+** Maximum wire fidelity by construction; trivial re-sync diff against upstream.
- **−** Carries ~2000 lines of unreachable TX/PS/mic/audio/Saturn code plus a
  large scaffold of stub globals whose only purpose is to make dead branches
  compile. Fragile and confusing: a reader can't tell live code from ballast.

### Option B — write a lean RX-only `protocol2.c`, copy the wire-critical parts verbatim  ⟵ recommended
A new ~450-line `src/engine/protocol2.c` that implements **only** the RX path:
one UDP socket, the three outgoing packets (General / RX-specific / High-Priority),
the listener thread, the 24-bit IQ decode, and the keepalive timer. The
**byte-for-byte wire-critical pieces are copied verbatim** from upstream with
line-referenced comments:
- the packet byte-fills (offsets + big-endian shifts) for the three packets,
- the DDC phase-word computation (`freq × 2³²/122 880 000`, line 817),
- the 24-bit-BE → float sample decode (`process_iq_data`, lines 2446–2459).

Our own minimal state struct replaces the global soup; the WDSP hand-off
(`rx_add_iq_samples`, line 2458) becomes our `on_rx_iq()` callback.
- **+** Every line is live RX code; no stub scaffold; matches the doc's existing
  intent ("get IQ out via a callback/ring buffer", "avoid receiver.c's WDSP+GTK
  entanglement"). Small enough to audit against upstream offset-by-offset.
- **−** Hand-written, so a wrong offset is on us — mitigated by copying the
  wire-critical bytes verbatim and diffing hexdumps against a piHPSDR capture.

**Recommendation: Option B.** The discovery import was a clean adapt because
discovery reads almost no state; `new_protocol.c` is the opposite — faithfully
adapting it means importing a TX engine we then smother in stubs. B keeps exactly
the bytes that must match the wire verbatim, and nothing else. Re-sync is still
tractable because the verbatim blocks are small and line-referenced.

> **DECIDED (Richard, 2026-07-06): Option B.** Lean RX-only `protocol2.c`;
> wire-critical bytes copied verbatim with upstream line references; validated by
> hexdump vs. a Wireshark capture of piHPSDR starting the same radio.

---

## 3. The wire protocol we must reproduce

### 3.1 Ports (`new_protocol.h:28-49`)
One UDP socket, bound to `radio->network.interface_address`; destination differs
only by port (radio IP constant). Source port of an incoming packet selects its type.

| Dir | Purpose | Port | RX-only? |
|---|---|---|---|
| → radio | General registers | 1024 | **keep** |
| → radio | RX-specific (DDC enable + rate) | 1025 | **keep** |
| → radio | High-Priority (run bit + DDC freq) | 1027 | **keep** |
| → radio | TX-specific / audio / TX-IQ | 1026/1028/1029 | drop |
| ← radio | RX IQ, DDC0 | **1035** | **keep** |
| ← radio | RX IQ, DDC1..7 | 1036–1042 | keep DDC1 only if RX2 |
| ← radio | High-Priority status (PTT/overload) | 1025 | defer (S-meter later) |
| ← radio | command-resp / mic / wideband | 1024/1026/1027 | drop |

**G1 gotcha:** the ANAN G1 is classed with HERMES, not Saturn/ORION2 — so **RX1 =
DDC0 = port 1035** (not DDC2). Comment at `new_protocol.c:376`; G1 absent from the
`ddc = 2+i` remap at 1630. Get this wrong → listening on a silent socket.

### 3.2 Outgoing packets (byte offsets to copy verbatim)

**General** — `new_protocol_general()` 662–716, port 1024, 60 bytes:
- `[0..3]` seq (BE); `[37]=0x08` (phase-word mode); `[38]=0x01` (enable HW timer).
- `[58]` PA-enable, `[59]` ALEX-enable → **0 for RX**.

**RX-specific** — `new_protocol_receive_specific()` 1609–1711, port 1025, 1444 bytes:
- `[0..3]` seq; `[4]=n_adc` (=1); `[5]` dither, `[6]` random (=0).
- `[7]` = **DDC-enable bitmap** → `0x01` (DDC0).
- Per-DDC 6-byte block at `17 + ddc*6`: `+0`=ADC (=0), `+1/+2`=`sample_rate/1000`
  (kHz, BE), `+5`=bits-per-sample (=24).
- Drop the PureSignal (1649) and diversity (1670) branches entirely.

**High-Priority** — `new_protocol_high_priority()` 718–1474, port 1027, 1444 bytes:
- `[0..3]` seq; **`[4]` = run byte** → `1` (MOX bit `0x02` stays clear).
- **DDC0 NCO phase = `[9..12]`** (BE), DDC1 = `[13..16]` if RX2.
  Phase = `(uint32_t)((vfo[id].frequency - vfo[id].lo) × 2³²/122 880 000)`
  (line 784+817). `lo=0` without a transverter but the subtraction is unconditional.
- Everything else (DUC phase 329, drive 345, ALEX words 1428/1432, attenuators
  1442/1443) → **0** for a bare G1 RX. (On ALEX radios `[1432..1435]`=alex0 selects
  RX band-pass relays; for the G1 RMS-only probe, default relays are fine — noted
  as a minor risk, §6.)

### 3.3 Send order + keepalive
Start handshake (`new_protocol_menu_start()` 1781, order at 1851–1862):
**General(1024) → RX-specific(1025) → High-Priority(1027, run=1).** Then a **timer
thread** (2953) re-sends General / RX-specific / High-Priority on a rolling
schedule — this **is** the P2 keepalive (no separate ping). We reproduce that
timer; drop its TX-specific re-send. Sequence counters are per-packet-type
`uint32_t`, incremented after each send, all zeroed at start.

### 3.4 Incoming IQ path
`listener thread` (2074) `recvfrom()` on the socket, dispatch by source port
(2115). For port 1035+ddc: sequence-check (`ddc_sequence[]`, 2301–2312; mismatch
logs + counts, does **not** drop) → hand the buffer to the decode.

**IQ decode** — `process_iq_data()` 2403–2460. Packet = 16-byte header + payload:
- `[14..15]` = samples-per-frame (normally 238, BE).
- payload from offset 16, **6 bytes/sample**: 3-byte BE signed I ("left") then
  3-byte BE signed Q ("right"), scaled by `1/2²³` (`1.1920928955078125e-7`) → float
  in ~[-1,1). **These ~15 lines (2446–2459) are copied verbatim.**
- Hand-off seam at line **2458**: `rx_add_iq_samples(rx, i, q)` → **replaced by our
  `on_rx_iq(rx_id, i, q)`** (or a batched `on_rx_iq_block(const float *iq, int n)`
  for efficiency). For `sdrfl-rxprobe` this callback accumulates count + RMS.

Ignore for RX-only: command-response (1024), mic (1026), wideband (1027, not even
handled upstream). High-Priority-status (1025) carries ADC-overload + PTT — **defer**
(useful for the S-meter later, not for IQ).

---

## 4. Minimal state layer to provide

Not piHPSDR's `receiver.h`/`radio.h` (those drag WDSP+GTK). A small
`engine_state.h`/`.c` (extend the existing `engine_state.c`) exposes just:

| State | Value for single-RX G1 |
|---|---|
| `radio->network.{…}` | from discovery (already populated) |
| `protocol` | `NEW_PROTOCOL` (1) |
| `device` | `NEW_DEVICE_G1` (1020) — drives DDC0/1 mapping + RX BPF switch |
| `receivers` | 1 |
| `n_adc` | 1 |
| `receiver[0].adc` | 0 |
| `receiver[0].sample_rate` | 192000 (probe) |
| `adc[0].{antenna,attenuation}` | band RX ant, 0 dB; `dither=random=filter_bypass=0` |
| `vfo[0].{frequency,lo}` | 14 100 000 ; 0 |
| stub-to-0 | `diversity_enabled, duplex, hpsdr_ptt, have_saturn_xdma, mox, frequency_calibration, can_transmit` → `radio_is_transmitting()==0` |
| stub fns | `band_get_band()` → `{OCrx:0, freqMin:0, freqMax:LLONG_MAX, disablePA:0}`; `vfo_get_tx_vfo()→0`; `vfo_get_tx_mode()→` any RX mode |

`calibrated_frequency()` (radio.h:108, inline) with `frequency_calibration=0` is
the identity — reimplement as a one-liner.

---

## 5. New files & build

| File | Role |
|---|---|
| `src/engine/protocol2.c` / `protocol2.h` | P2 RX link: socket, 3 send packets, listener, IQ decode, timer keepalive; public API `p2_rx_start(dev, freq, rate, on_iq_cb)` / `p2_rx_stop()`. |
| `src/engine/engine_state.{c,h}` | extend with the minimal radio/receiver/adc/vfo state + stubs from §4. |
| `src/rxprobe_main.c` | gate: discover → `p2_rx_start` @ 192 k/14.1 MHz → 1 s → print stats. |
| `meson.build` | add `engine_sources += protocol2.c`; new `sdrfl-rxprobe` target (mirror `sdrfl-discover`). |

No WDSP dependency for this gate (`engine_deps = [glib_dep, threads_dep]`).

## 6. Risks & mitigations
- **Wrong byte offset / phase word → silent socket.** Copy wire-critical bytes
  verbatim; diff a `sdrfl-rxprobe` hexdump against a Wireshark capture of piHPSDR
  starting the same radio, before trusting the decode.
- **G1 DDC/port assumption.** Asserted: DDC0/port 1035 (§3.1). Verify in the live
  test that packets actually arrive on 1035.
- **ALEX RX relays** (`filter_board`/alex0). For the RMS probe, default relays
  suffice; if IQ RMS is implausibly low, set `filter_board` + `adc[0].antenna` to
  the G1's real values.
- **Radio ownership.** The live test opens the P2 data path → **takes the radio
  from piHPSDR** (one owner). Everything through step 5 is offline (build, hexdump
  vs. capture). **Ask Richard to free the radio before the live test — do not kill
  his piHPSDR** (his explicit instruction, 2026-07-06).

## 7. Offline vs. live
- **Offline (no radio):** §2 decision, all of `protocol2.c` + `engine_state` +
  `rxprobe_main` + meson, compile, and a static hexdump self-check of the built
  packets. Can also diff against a passively-captured piHPSDR-start Wireshark trace
  without owning the radio.
- **Live (needs free radio):** run `sdrfl-rxprobe` against 192.168.1.247 — the one
  step that needs Richard to free the TRX.

## 8. Implementation order (after consent)
1. Extend `engine_state.{c,h}` with the §4 state + stubs.
2. `protocol2.c`: socket setup + the three send packets; static hexdump self-test.
3. Listener thread + 24-bit IQ decode + `on_rx_iq` callback; timer keepalive.
4. `rxprobe_main.c` + meson `sdrfl-rxprobe`; compile clean.
5. **(live, radio free)** run the probe → sample count / rate / RMS → done.

Then Step 4 swaps `on_rx_iq` for the WDSP analyzer (`Spectrum0` → `pixel_samples`).
