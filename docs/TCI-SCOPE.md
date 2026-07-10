# F6d-2 — TCI server: scope & plan

Goal: an ExpertSDR-compatible **TCI server** inside sdr-for-linux, so third-party
software keys CW, decodes digital modes and runs skimmers against our radio —
Richard's concrete clients: **Decodium** (digital modes, needs RX+TX audio over
TCI), **SDC connectors** (UT4LW; CW keyer, skimmer, spots), contest loggers.

## The protocol (official spec, read first-hand)

Source of truth: **"TCI Protocol.pdf" ver. 2.0, 12 Jan 2024**, Expert
Electronics, github.com/ExpertSDR3/TCI (MIT). Read in full 2026-07-10; the
local copy lives outside the repo (452 kB, 41 pp) — re-download from upstream
if needed.

- **Transport:** WebSocket server (full duplex over TCP). *Text frames* carry
  ASCII commands `name:arg1,arg2;` (case-insensitive, reserved chars `:,;`);
  *binary frames* carry audio/IQ blocks. The server pushes every state change
  to all clients (no polling); clients are synchronized by the server.
- **Command classes:** initialization (sent on connect: `VFO_LIMITS`,
  `IF_LIMITS`, `TRX_COUNT`, `CHANNEL_COUNT`, `DEVICE`, `RECEIVE_ONLY`,
  `MODULATIONS_LIST`, `PROTOCOL:<name>,<ver>`, `READY;`), bidirectional
  (`DDS` center freq, `IF` offset-in-panorama, `VFO`, `MODULATION`, `TRX`
  (arg3 `tci` = TX audio source!), `TUNE`, `DRIVE`, `TUNE_DRIVE`, `RIT/XIT`,
  `SPLIT_ENABLE`, `RX_FILTER_BAND`, `CW_MACROS_SPEED/_DELAY`,
  `CW_KEYER_SPEED`, `VOLUME`, `MUTE`, `RX_MUTE`, `AGC_MODE/GAIN`,
  `RX_NB/BIN/NR/ANC/ANF/APF/DSE/NF_ENABLE`, `SQL_ENABLE/LEVEL`, `LOCK`,
  `DIGL_OFFSET`, `DIGU_OFFSET`, `MON_VOLUME`, `MON_ENABLE`…), unidirectional
  (`TX_ENABLE`, `SPOT`/`SPOT_DELETE`/`SPOT_CLEAR`, `IQ_SAMPLERATE`
  48/96/192/384k, `AUDIO_SAMPLERATE` 8/12/24/48k, `IQ_START/STOP`,
  `AUDIO_START/STOP`, `LINE_OUT_*` incl. recorder,
  `AUDIO_STREAM_SAMPLE_TYPE` (int16/24/32/float32, default float32),
  `AUDIO_STREAM_CHANNELS` (1/2), `AUDIO_STREAM_SAMPLES` (100–2048),
  `TX_STREAM_AUDIO_BUFFERING` (50–500 ms, default 50)), notifications
  (`TX_FREQUENCY`, `TX_FOOTSWITCH`, `RX_CLICKED_ON_SPOT`, `KEYER` (straight
  key with per-character ms timing), `RX_SENSORS_ENABLE`/`RX_CHANNEL_SENSORS`
  (S-meter dBm), `TX_SENSORS_ENABLE`/`TX_SENSORS` (mic dBm, RMS W, **peak W**,
  SWR), `VFO_LOCK`, `APP_FOCUS`/`SET_IN_FOCUS`).
- **CW:** `cw_macros:<trx>,<text>;` queues text on air (`|SK|` = merged abbrev,
  `>`/`<` = ±5 WPM inline, `^~*` escape `:,;`); `cw_terminal:true;` holds TX
  after the queue drains (`cw_macros_empty;` notifies); `cw_macros_stop;`
  aborts. `cw_msg:<trx>,<prefix>,<callsign>,<suffix>;` additionally allows
  **live callsign correction** while sending (`cw_msg:<newcall>;` until the
  callsign is on air, then `callsign_send:<final>;`), `$2` = repeat callsign.
  CW_MSG preempts CW_MACROS; the CW queue and speed belong to the server.
- **Streams (binary):** fixed header `{u32 receiver, sample_rate, format,
  codec=0, crc=0, length /*samples*/, type, channels, reserv[8]}` + payload.
  `type`: IQ=0, RX_AUDIO=1, TX_AUDIO=2, **TX_CHRONO=3**, LINEOUT=4.
  RX audio duplicates IQ flow but with client-set rate/format/blocksize; in
  DIGL/DIGU 2 ch = complex, 1 ch = real. **TX audio is server-paced:** the
  server emits TX_CHRONO with `length` samples requested; the client answers
  with a TX_AUDIO_STREAM block (silence/no answer allowed). Per-rate default
  block sizes 48k→2048 … 8k→256; client may lower (≥10 ms recommended).
- **Server etiquette:** parameter changed by server or a client is
  monopolized for 200 ms; ExpertSDR3 always outranks clients. Announce via
  `PROTOCOL:ExpertSDR3,1.9;` — clients key on that name, keep it verbatim
  (piHPSDR does the same).

## Existing proven code: piHPSDR @974acba

`src/tci.c` (3875 lines) + `src/tci_audio.[ch]` implement nearly the whole
protocol **including RX/TX audio streams, IQ streams, cw_msg callsign
correction, DIGU/DIGL offsets** on GLib + **libwebsockets** (default port
40001). Per the project vendoring/import policy (prefer proven outside code;
import engine files milestone by milestone) the plan is to **import and adapt
tci.c**, not rewrite it. The adaptation is the real work: it is welded to
piHPSDR globals (`vfo[]`, `receiver[]`, `transmitter`, `schedule_*`) that must
be remapped onto our engine API (p2 freq, demod, tx_run, tx_meter, settings).
New distro dependency: **libwebsockets** (packaged on Arch/Debian/Fedora;
platform-library category — do not vendor).

## Phases (each independently testable; RX-side needs no keying)

- **F6d-2a — server + control + CW. IMPLEMENTED (offline-verified 2026-07-10;
  live test with a real client pending).** `src/tci_server.[ch]` — own code on
  the piHPSDR LWS pattern (chat/superchat/tci subprotocols, 1 ms service loop,
  per-client queues, commands g_idle_add-dispatched to the GTK main loop, a
  500 ms reporter broadcasts state diffs so GUI-side changes reach clients
  without instrumenting the GUI). Ops table in gui.c reuses the on-screen
  control paths — TRX/TUNE toggle the real MOX/TUNE buttons (→ tx_gate),
  cw_macros → tx_run_cw_send **only in a CW mode** (queued text must never
  key later as a surprise), `trx:0,true,tci` (TCI audio source) is refused
  until F6d-2c. Covered: handshake (protocol:ExpertSDR3,1.9 … ready;),
  dds/if/vfo, modulation (ExpertSDR "cw" ↔ our CWU; cwl kept), rx_filter_band,
  drive/tune_drive, volume/mute, cw_macros(+escapes)/_stop/_speed(_up/_down)/
  _delay, basic cw_msg ($N repeats; live callsign correction NOT yet),
  tx_enable/tx_frequency. Prefs: Radio → TCI (switch live, port 40001,
  persisted `[tx] tci/tci_port`, off by default). Gate: `sdrfl-tci-test`
  (13 checks) — real WebSocket client against the server with stub ops.
- **F6d-2b — RX audio. AUDIO STREAM IMPLEMENTED (offline-verified 2026-07-10).**
  Demod tap: volume-compensated mono (pre-mute/pre-monitor — decode must not
  depend on the volume knob), fixed 48 kHz, → SPSC ring → LWS thread fans out
  per client (boxcar to 8/12/24/48 k, float32/int16, 1/2 ch, block size per
  spec defaults or AUDIO_STREAM_SAMPLES), official Stream header (type=1).
  `tci_server_audio_push` also kicks lws_cancel_service — lws_service blocks
  otherwise and audio never pumps (the piHPSDR tci_audio_wakeup trick).
  Commands: audio_samplerate/_start/_stop/_stream_sample_type/_channels/
  _samples; SDRFL_TCI_DEBUG=1 logs every received command. Gate extended to
  19 checks (12 kHz mono subscription delivers correct Stream blocks).
  **Sensors done too (LIVE with Decodium 2026-07-10):** RX_CHANNEL_SENSORS
  (WDSP S-meter dBm) + TX_SENSORS (mic dBFS, RMS W, PEP W, SWR from
  tx_run_status) on a 100 ms timer honouring each client's
  RX/TX_SENSORS_ENABLE cadence (100–1000 ms). Decodium end-to-end verified:
  RX audio decodes, frequency syncs both ways. Compat lessons that unblocked
  it (each one stalled the client silently): `channels_count:2` (piHPSDR
  spelling + A/B count, not the spec's CHANNEL_COUNT), `rx_enable:0,true;`,
  per-channel if/vfo state, the full piHPSDR-style init block, `start;`
  after `ready;`, and an **echo layer** — every bidirectional set MUST come
  back as a broadcast, so backend-less commands (split/RIT/XIT/squelch/
  noise/agc/rx_mute…) are accepted, stored and echoed. **Still pending in
  2b:** LINE_OUT stream. ⚠ Locale rule: protocol floats are formatted with
  g_ascii_formatd — the GTK app runs in the user's locale (cs_CZ = decimal
  COMMA) and ',' is a reserved TCI separator.
- **F6d-2c — TX audio (digital TX). LIVE-VERIFIED ON AIR 2026-07-10:** first
  complete FT8 QSO made through the app (Decodium → TCI → DIGU, 20 m);
  PSK Reporter shows OK1BR received by 113 reporters in 32 countries within
  24 h. Some QSOs unfinished — attributed to band congestion; watch-items:
  exact system clock (FT8 periods) and key-to-audio latency (~42 ms prime,
  should be negligible). `trx:0,true,tci` switches the exciter input from
  the mic to a TCI SPSC ring (tx_run_set_ext_source/ext_push) **before** the
  key goes down, keys through the same tx_gate as the GUI MOX (SWR trip,
  in-band, PA, power caps — nothing bypassed), and the TX feed loop's block
  cadence drives TX_CHRONO frames to the owning client (48 k float32 mono,
  512/block, 4-block prime on key-on). Binary TX_AUDIO_STREAM blocks are
  reassembled from WS fragments, int16/float32 + 1-2 ch + 8-48 k accepted
  (left channel, naive upsample). Safety: single TX owner; owner disconnect,
  server stop or gate refusal ⇒ immediate unkey + revert to mic. ⛔ Richard's
  clean-chain rule enforced in the GUI: in DIGU/DIGL, PROC/leveler + DEXP
  gate are forced OFF regardless of the stored voice settings (tx_apply_proc)
  and the mic stays closed (mode_is_voice). Gate: 23 checks incl. the full
  key→chrono→audio→unkey round-trip. TODO: drive_digi_max-style power cap
  for 100 % duty modes (piHPSDR has one, default uncapped).
- **F6d-2d — IQ stream (skimmer). IMPLEMENTED (offline-verified 2026-07-10;
  SDC/CW Skimmer live test pending).** Note: piHPSDR's tci.c has iq_start/
  iq_stop as EMPTY STUBS — the working reference here is **deskHPSDR**
  (github.com/dl1bz/deskhpsdr, tci.c), which however streams at the
  receiver's native rate and *retunes the whole radio* to the client's rate.
  We keep the DDC at the operator's rate (1536k panadapter untouched) and
  **decimate per client with the WDSP resampler** (vendored; create_resample
  fc/ncoef auto → anti-alias FIR at 0.45×min-rate, same call piHPSDR's soapy
  path uses). Flow: gui.c feed_cb (raw P2 IQ, real signal even in the post-TX
  silence window) → tci_server_iq_push (SPSC ring, 2^17 pairs = 85 ms @
  1536k, no-op while nobody subscribed) → LWS-thread pump drains 1024-pair
  chunks → per-client xresample (bypass when rates match, re-planned on any
  rate change) → float32 blocks of 2048 frames, Stream header type=0, 2 ch,
  length = frames×2 (the convention Decodium already accepted for audio).
  Commands: iq_samplerate {48,96,192,384}k validated + echoed, iq_start:0 /
  iq_stop:0 echoed (deskHPSDR semantics; non-zero receiver ids ignored).
  No CW ±sidetone phase rotation (deskHPSDR needs it for piHPSDR's CW-BFO
  convention; our DDC centre == reported dds in every mode — revisit if the
  deferred CW BFO offset lands). Debug knobs for a client expecting the
  mirrored spectrum: SDRFL_TCI_IQ_SWAP / SDRFL_TCI_IQ_CONJ (deskHPSDR has the
  same as options). Gate extended to 33 checks: echo round-trips, header
  fields, and a +12 kHz complex tone through the 192k→48k decimation —
  correct frequency, amplitude ~0.5, −12 kHz image < −40 dB (catches a
  swapped/conjugated stream). Prefs TCI page tags streaming clients "· iq".
- **F6d-2e — spots.** SPOT/SPOT_DELETE/SPOT_CLEAR drawn on the panadapter
  (bandplan-overlay infra reused; callsign labels), RX_CLICKED_ON_SPOT back
  to clients on click. SDC/cluster integration.

## Safety (unchanged, non-negotiable)

TCI is **just another keying requester**: every TRX/TUNE/CW path lands in the
same tx_gate (in-band, PA, SWR trip, atten-31, atomic HP state) — no TCI
command may bypass it. The GUI stays the master: a local unkey/Esc always
wins; `TX_ENABLE:0,false;` is reported when the gate refuses. TCI server off
by default until F6d-2a is live-verified.
