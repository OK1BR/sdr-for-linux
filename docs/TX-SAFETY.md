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
