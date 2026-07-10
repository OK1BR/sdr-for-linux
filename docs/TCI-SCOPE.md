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
  **Still pending in 2b:** RX_CHANNEL_SENSORS (S-meter), TX_SENSORS,
  LINE_OUT stream.
- **F6d-2c — TX audio (digital TX).** TRX:0,true,**tci** → mic path switches
  to the TCI ring: emit TX_CHRONO pacing, accept TX_AUDIO_STREAM, resample to
  the 48 k TX input. Needs **DIGU/DIGL modes** (flat TX audio path, no
  proc/gate/leveler bends the data tones — ties into the data-mode TODO) +
  DIGU_OFFSET/DIGL_OFFSET. ⚠ full TX-SAFETY checklist re-verification.
- **F6d-2d — IQ stream (skimmer).** IQ_START/STOP + IQ_SAMPLERATE 48–384k,
  decimated from the 1536k DDC feed; CW Skimmer via SDC is the acceptance
  test.
- **F6d-2e — spots.** SPOT/SPOT_DELETE/SPOT_CLEAR drawn on the panadapter
  (bandplan-overlay infra reused; callsign labels), RX_CLICKED_ON_SPOT back
  to clients on click. SDC/cluster integration.

## Safety (unchanged, non-negotiable)

TCI is **just another keying requester**: every TRX/TUNE/CW path lands in the
same tx_gate (in-band, PA, SWR trip, atten-31, atomic HP state) — no TCI
command may bypass it. The GUI stays the master: a local unkey/Esc always
wins; `TX_ENABLE:0,false;` is reported when the gate refuses. TCI server off
by default until F6d-2a is live-verified.
