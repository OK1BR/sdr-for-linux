project('sdr-for-linux', 'c',
  version : '0.0.1',
  license : 'GPL-3.0-or-later',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

add_project_arguments('-D_GNU_SOURCE', language : 'c')

# --- Dependencies -----------------------------------------------------------
# gtk4/opus come in transitively via the vendored piHPSDR headers; openssl is
# the password KDF, zlib decompresses network spectrum. As the piHPSDR engine
# (radio link + WDSP) is imported milestone by milestone, its deps (e.g. the
# WDSP library, portaudio/pulse) will be added here.
cc          = meson.get_compiler('c')
gtk_dep     = dependency('gtk4')
adw_dep     = dependency('libadwaita-1')  # GTK4 control surface (headerbar, prefs, rows)
glib_dep    = dependency('glib-2.0')  # engine is GLib-only (headless), no GTK
opus_dep    = dependency('opus')
openssl_dep = dependency('openssl')
zlib_dep    = dependency('zlib')
# WDSP + libspecbleach need single-precision FFTW; rnnoise/WDSP need pthreads + libm.
# WDSP's analyzer/fir also use the double-precision FFTW (fftw_* symbols).
fftw3f_dep  = dependency('fftw3f')
fftw3_dep   = dependency('fftw3')
# Native PipeWire for the low-latency RX audio sink (src/engine/audio_pw.c).
pipewire_dep = dependency('libpipewire-0.3')
lws_dep      = dependency('libwebsockets')   # TCI server (F6d-2; distro platform lib)
threads_dep = dependency('threads')
m_dep       = cc.find_library('m', required : false)

vendor_inc = include_directories('vendor/pihpsdr')
src_inc    = include_directories('src')
engine_inc = include_directories('src/engine')

# --- Engine (headless, GLib-only; adapted from piHPSDR) ---------------------
# Grows milestone by milestone. First piece: Protocol-2 discovery.
engine_sources = [
  'src/engine/engine_state.c',
  'src/engine/discovery_p2.c',
  'src/engine/protocol2.c',
]
engine_deps = [glib_dep]

common_deps = [gtk_dep, opus_dep, openssl_dep, zlib_dep]

# --- Vendored engine libraries ----------------------------------------------
# Built from source we keep in-tree (never linked from an external checkout);
# each dir's VENDOR.md records upstream origin + pinned commit. Order matters:
# wdsp depends on rnnoise_dep / specbleach_dep declared by the first two.
subdir('vendor/rnnoise')
subdir('vendor/libspecbleach')
subdir('vendor/wdsp')  # declares wdsp_dep

ui_sources = [
  'src/panadapter.c',
  'src/waterfall.c',
]

# --- Application ------------------------------------------------------------
# v0: the GTK4 front-end (panadapter + waterfall) driven, for now, by the
# piHPSDR client/server network path (client.c). The direct-to-radio engine
# (HPSDR Protocol 2 + WDSP) replaces that data source in upcoming milestones.
executable('sdr-for-linux',
  [
    'src/gui.c',
    'src/settings.c',
    'src/bandplan.c',
    'src/wisdom_gate.c',
    'src/picker.c',           # startup radio picker (discovery dialog)
    'src/tci_server.c',       # TCI server — ExpertSDR-compatible control (F6d-2a)
    'src/client.c',
    'src/protocol.c',
    'src/transport.c',
    'src/engine/analyzer.c',
    'src/engine/demod.c',
    'src/engine/audio_pw.c',
    'src/engine/mic_pw.c',    # PipeWire mic capture — SSB TX (F6c)
    'src/engine/tx.c',        # WDSP TX DSP channel (F2)
    'src/engine/tx_meter.c',  # fwd/rev power + SWR (F3)
    'src/engine/tx_gate.c',   # TX safety gate (F4)
    'src/engine/tx_analyzer.c', # TX panadapter analyzer (F6a)
    'src/engine/tx_run.c',    # TX runtime thread — GUI keying (F6a)
    'src/engine/ps.c',        # PureSignal runtime — WDSP calcc wiring (F7/PS-2)
    'src/engine/cw_gen.c',    # CW Morse envelope generator (F6d)
  ] + engine_sources + ui_sources,
  include_directories : [vendor_inc, src_inc, engine_inc],
  # engine (protocol2 + analyzer + demod + PipeWire audio) alongside the v0
  # network client; WDSP's comm.h/wdsp.h need the GNU dialect (as sdrfl-wdsp-smoke).
  dependencies : common_deps + [adw_dep, glib_dep, wdsp_dep, m_dep, pipewire_dep, lws_dep],
  override_options : ['c_std=gnu11'],
  install : false,
)

# First-run FFTW wisdom builder + progress window, exercised without the radio
# (SDRFL_WISDOM_DIR overrides the cache dir so a throwaway build can be tested).
executable('sdrfl-wisdom-test',
  ['src/wisdom_test.c', 'src/wisdom_gate.c'],
  include_directories : [src_inc],
  dependencies : [adw_dep, gtk_dep, glib_dep, wdsp_dep, m_dep],
  override_options : ['c_std=gnu11'],
  install : false,
)

# WDSP build/link smoke check (milestone gate: proves the vendored DSP builds,
# its headers are usable by our code, and libwdsp links end-to-end).
executable('sdrfl-wdsp-smoke',
  ['src/wdsp_smoke.c'],
  dependencies : [wdsp_dep],
  # includes WDSP's comm.h, which needs the GNU dialect (see vendor/wdsp/meson.build)
  override_options : ['c_std=gnu11'],
  install : false,
)

# Headless Protocol-2 discovery probe (finds the radio on the LAN; read-only,
# safe to run alongside another client). Engine's first milestone gate.
executable('sdrfl-discover',
  ['src/discover_main.c'] + engine_sources,
  include_directories : [vendor_inc, engine_inc],
  dependencies : engine_deps,
  install : false,
)

# Headless Protocol-2 RX + IQ probe (starts one DDC, collects IQ, prints stats).
# Milestone-1 step-3 gate. TAKES THE RADIO — piHPSDR must be disconnected.
# SDRFL_DRYRUN=1 builds+hexdumps the packets offline (no socket, no radio).
executable('sdrfl-rxprobe',
  ['src/rxprobe_main.c'] + engine_sources,
  include_directories : [vendor_inc, engine_inc],
  dependencies : engine_deps + [m_dep],
  install : false,
)

# Offline TX byte-layout gate (F1, docs/TX-DESIGN.md). Builds the General/HP/TX-
# specific packets with a hot TX state and asserts every TX byte vs piHPSDR; also
# proves the OFF state carries no MOX/TX_RELAY/drive. NO radio, NO socket.
executable('sdrfl-txprobe',
  ['src/txprobe_main.c'] + engine_sources,
  include_directories : [vendor_inc, engine_inc],
  dependencies : engine_deps + [m_dep],
  install : false,
)

# Headless Protocol-2 RX -> WDSP analyzer -> panadapter PNG. Milestone-1 step-4
# gate. TAKES THE RADIO — piHPSDR must be disconnected. Links the vendored WDSP
# (needs the GNU dialect, like sdrfl-wdsp-smoke) + our pure-Cairo renderer.
executable('sdrfl-panprobe',
  ['src/panprobe_main.c', 'src/engine/analyzer.c'] + engine_sources + ui_sources,
  include_directories : [vendor_inc, src_inc, engine_inc],
  dependencies : [wdsp_dep, gtk_dep, glib_dep, m_dep],
  override_options : ['c_std=gnu11'],
  install : false,
)

# Offline TX DSP + IQ-encoding gate (F2, docs/TX-DESIGN.md). Feeds a tone into the
# WDSP TX channel and checks the SSB IQ output + the 24-bit encoder + the framer.
# Links WDSP (GNU dialect). NO radio, NO socket.
executable('sdrfl-txdsp-test',
  ['src/txdsp_test.c', 'src/engine/tx.c'] + engine_sources,
  include_directories : [vendor_inc, src_inc, engine_inc],
  dependencies : engine_deps + [wdsp_dep, m_dep],
  override_options : ['c_std=gnu11'],
  install : false,
)

# Offline TX metering + SWR gate (F3, docs/TX-DESIGN.md). Pure math on synthetic
# ALEX coupler words — G1 watts + SWR vs piHPSDR. NO radio, NO WDSP.
executable('sdrfl-swr-test',
  ['src/swr_test.c', 'src/engine/tx_meter.c'],
  include_directories : [engine_inc],
  dependencies : [m_dep],
  install : false,
)

# Offline CW timing gate (F6d, docs/TX-DESIGN.md). Proves the Morse envelope
# generator keeps sample-locked rhythm (dot = sr·1.2/WPM; PARIS = 50 dots) at a
# range of speeds. Pure — NO radio, NO WDSP, NO socket.
executable('sdrfl-cw-test',
  ['src/cw_test.c', 'src/engine/cw_gen.c'],
  include_directories : [engine_inc],
  dependencies : [m_dep],
  install : false,
)

# Offline TCI gate (F6d-2a, docs/TCI-SCOPE.md). Starts the TCI server with
# stub ops, connects a real WebSocket client and verifies the handshake,
# control round-trips + broadcasts, and CW queue/stop. NO radio, NO GUI.
executable('sdrfl-tci-test',
  ['src/tci_test.c', 'src/tci_server.c'],
  include_directories : [src_inc],
  dependencies : [glib_dep, lws_dep, wdsp_dep, m_dep],   # wdsp: IQ resampler (2d)
  install : false,
)

# Offline TX safety-gate check (F4, docs/TX-DESIGN.md). Exercises each guard
# (in-band, PA gate, SWR/open-antenna trip + latch, tune-exempt) with synthetic
# inputs. Links the band plan for the in-band check. NO radio, NO socket.
executable('sdrfl-txgate-test',
  ['src/txgate_test.c', 'src/engine/tx_gate.c', 'src/bandplan.c'],
  include_directories : [vendor_inc, src_inc, engine_inc],
  dependencies : [m_dep],
  install : false,
)

# FIRST-KEYING probe (F5, docs/TX-DESIGN.md). KEYS THE RADIO — dummy load on ANT1,
# piHPSDR disconnected, operator present. Safe defaults: PA off, drive 0. Links the
# TX DSP + metering + safety gate + band plan + engine + WDSP.
executable('sdrfl-txkey',
  ['src/txkey_main.c', 'src/engine/tx.c', 'src/engine/tx_meter.c',
   'src/engine/tx_gate.c', 'src/bandplan.c'] + engine_sources,
  include_directories : [vendor_inc, src_inc, engine_inc],
  dependencies : engine_deps + [wdsp_dep, m_dep],
  override_options : ['c_std=gnu11'],
  install : false,
)

# Headless Protocol-2 RX -> WDSP demod -> PipeWire audio. Milestone-2 gate.
# TAKES THE RADIO. SDRFL_TONE=1 plays a test tone with no radio.
executable('sdrfl-audioprobe',
  ['src/audioprobe_main.c', 'src/engine/demod.c', 'src/engine/audio_pw.c'] + engine_sources,
  include_directories : [vendor_inc, src_inc, engine_inc],
  dependencies : engine_deps + [wdsp_dep, m_dep, pipewire_dep],
  override_options : ['c_std=gnu11'],
  install : false,
)

# Mic capture check (F6c-1): default PipeWire source → VU meter. NO radio, NO TX.
executable('sdrfl-micprobe',
  ['src/micprobe_main.c', 'src/engine/mic_pw.c'],
  include_directories : [src_inc, engine_inc],
  dependencies : [m_dep, pipewire_dep],
  override_options : ['c_std=gnu11'],
  install : false,
)

# Headless render check (writes a PNG of one averaged frame; no window).
executable('sdrfl-render-test',
  [
    'src/render_test.c',
    'src/client.c',
    'src/protocol.c',
    'src/transport.c',
  ] + ui_sources,
  include_directories : [vendor_inc, src_inc],
  dependencies : common_deps,
  install : false,
)
