# TX safety — current guarantees + checklist for the TX milestone

Why this doc: the ANT-relay bug (fixed in c4b9243) showed how easy it is to
drive the Alex relays into a state the hardware never expects. On RX that
costs sensitivity; on TX a wrong relay state can cost the PA. This records
(a) why the app is TX-safe *today*, and (b) what MUST be ported from piHPSDR
(src @ 974acba) before we ever key the radio. Nothing on this list is
optional; treat it as the acceptance checklist for the TX milestone.

## Current state: the app cannot transmit (three independent layers)

1. **No MOX** — high-priority byte [4] carries only the run bit; the MOX bit
   (0x02) is never set (`p2_build_high_priority`).
2. **PA disabled** — general packet byte [58] (PA enable) is 0. The firmware
   keeps the PA stage off regardless of other state.
3. **Drive = 0** — the TX-specific packet is all-zero (`build_transmit_specific`),
   so even a keyed exciter would produce no RF.

Hardware PTT / CW key corner case: P2 firmware can key locally from the
PTT/key jacks without any host command. With layers 2+3 no RF is produced,
and since c4b9243 the Alex words always describe a *valid closed* path
(ANT1 relay + correct band LPF — the "Jan 2023 protocol update" TX-case
bits), so even a hardware-keyed radio never sees an open antenna relay.
This matches exactly what piHPSDR sends when "PA enable" is off: piHPSDR
adds ALEX_TX_RELAY to alex1 only `if (!txband->disablePA && pa_enabled)`
(new_protocol.c:1026) — with PA disabled it omits it, and so do we.

Footswitch PTT (the "Footswitch PTT" setting): the pedal state is *read*
from the incoming HP-status packet, byte 4 bit 0 (np.c:2624), and turned
into a MOX **intent** in the tx_run gate slot — voice modes only, and
tx_gate still decides (in-band, PA, SWR trip, whitelist), exactly like the
MOX button. The pedal never keys on its own on the host side; a firmware-
local key with our RX state on the wire produces no RF (layers 2+3 above).
While transmitting with the setting on, TX-specific byte 50 keeps the PTT
input enabled (np.c:1553-1558) so the pedal *release* is still reported
mid-over; with the setting off the TX-time packet keeps PTT disabled and
the status bit is ignored (today's behavior, bit-for-bit).

## Checklist to port before enabling TX (piHPSDR references)

- [ ] **PA gating**: general[58] PA-enable tied to a user setting AND per-band
      `disablePA` (band.c); ALEX_TX_RELAY in alex0/alex1 only when PA enabled
      (new_protocol.c:1019-1032). Drive forced 0 when PA disabled.
- [ ] **ADC protection during TX**: with PA on, set both step attenuators to
      31 dB while transmitting — HP bytes 1442/1443 (new_protocol.c:1076,
      "Upon transmitting with PA enabled, set the attenuators to maximum")
      *and* TX-specific bytes 58/59 (same file, duplicated per the newer
      protocol definition). Protects the ADCs from own RF.
- [ ] **LPF always tracks the TX (DUC) frequency** in both alex words
      (new_protocol.c:1244-1279) and TX antenna bits in alex1 (:1398-1410).
      Never key with a stale LPF after QSY — rebuild the HP packet on every
      frequency/mox change (our builder regenerates the full packet, so MOX
      + relay bits always travel atomically in one packet; keep it that way).
- [ ] **Out-of-band lockout**: refuse MOX outside ham bands unless the user
      explicitly enables OOB TX (piHPSDR: txband->disablePA + "Out of band"
      in vfo.c; general packet resent on band edge crossing, vfo.c:281).
- [ ] **SWR protection**: compute SWR from ALEX fwd/rev sensors each meter
      cycle; if SWR ≥ alarm (default 3.0) in TWO consecutive readings (spike
      filter) and protection is on, force drive to 0 + alarm the user
      (transmitter.c:770-790). Do not trip during TUNE.
- [ ] **Per-band drive limits / PA calibration** (pa_trim table, drive_max,
      separate digi-mode max — radio.c pa_power_list, drive_digi_max).
- [ ] **TX/RX transition silencing** (rx->txrxmax sample muting on the RX
      side, receiver.c rx_add_iq_samples) — audio hygiene, not PA safety,
      but part of the same transition code.
- [ ] **CW**: if the internal keyer/sidetone is enabled via TX-specific,
      verify the attenuator + LPF + lockout rules also hold for
      hardware-keyed CW (firmware keys without a host MOX).

Relay hot-switching itself (RF ramp-down before relay movement) is sequenced
by the P2 gateware, not the host — but ONLY if the host state is consistent:
one high-priority packet must always carry a mutually consistent
{MOX, ANT, LPF, BPF, attenuator} set. Never split that across packets.

## Thetis cross-check (ramdor/Thetis @ 852bf0ef, audited 2026-07-07)

Thetis implements the same protections differently; adopt the best of both:

- [ ] **Open-antenna detection**: during MOX, if `fwd > 10 W && (fwd − rev) < 1 W`
      → treat as open feedline: drive to minimum AND force MOX off + warn
      (console.cs:25968-25988). Catches the "transmitting into an open relay"
      case that SWR alone reports too slowly.
- [ ] **SWR trip needs debouncing**: Thetis trips only after **4 consecutive**
      over-limit polls, then scales drive by `limit/(swr+1)`; optional latch to
      near-zero until un-MOX (console.cs:26046-26069). piHPSDR uses 2
      consecutive readings → drive 0. Either way: never trip on one sample
      (artifacts), never TUNE-trip.
- [ ] **Refuse the T/R relay when PA is disabled** — Thetis guards
      `SetTRXrelay` with `if (!pa_enabled) return` (netInterface.c:374-385).
      Port this: the relay must not move without a PA that can be keyed.
- [ ] **T/R sequencing delays**: TX→RX: stop TX DSP → ~10 ms (in-flight samples
      clear) → retune DDCs → PTT/TR relay off → ~20 ms (hardware settles) →
      re-enable receivers (console.cs:29581-29614). Mirror on RX→TX.
- [ ] **TX attenuation lives in TX-specific bytes 57-59 too** (0-31 dB,
      Thetis default 31; forced 31 when PureSignal off or CW/QSK) — piHPSDR
      duplicates it in HP bytes 1442/1443 for old firmware. Send BOTH.
- [ ] **Fwd/rev power calibration is per-model**: G2E/G2 bridge 0.12 V fwd /
      0.15 V rev (0.7 on 6 m), ref 5.0 V, ADC offsets 32/28
      (console.cs:25009-25017) — needed before SWR numbers mean anything.
- [ ] **Watchdog stays on**: we run General[38]=1 + HP every 100 ms (piHPSDR
      style), so the radio auto-stops streaming AND transmitting if the host
      dies. Thetis runs [38]=0 and relies on its 750 pkt/s audio stream —
      do NOT copy that model; keep the watchdog armed.

## Firmware-side facts (Saturn p2app + FPGA + proto spec v4.4, audited 2026-07-07)

What the radio itself guarantees when the host stops (run=0, watchdog
timeout, or p2app exit) — and what it does NOT:

- Standby drops **only** MOX/TXEN → PTT out, open-collector outputs, and the
  T/R relay (hardware-strobed from TXEN; the client's Alex bit 27 is ignored
  on Saturn — the FPGA replaces it with its own strobe). Anti-stuck-TX is
  therefore firmware-guaranteed. Additionally an FPGA hardware watchdog
  disables TX after 2 s without FIFO activity even if p2app itself hangs.
- **ANT/BPF/LPF relays are NOT released** — they are external latched shift
  registers, holding last state until power-off. That is why our run=0
  shutdown packet carries zeroed Alex words: it parks the RX input
  disconnected from the antenna (static protection). piHPSDR/Thetis do not
  do this (antenna stays latched to the ADC after they exit).
- p2app applies Alex/attenuator fields from a HP packet **even when the run
  bit is 0** — command packets are state images at all times. But p2app
  caches Alex words and skips identical rewrites; a parking zero-write is
  only guaranteed to land if a non-zero word was written earlier in the
  session (our 100 ms keepalives ensure that).
- On a crash/SIGKILL (no park packet) the relays stay latched — same
  behavior as after a piHPSDR crash; only powering the radio off clears it.
