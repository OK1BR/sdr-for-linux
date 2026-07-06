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
    'src/client.c',
    'src/protocol.c',
    'src/transport.c',
    'src/engine/analyzer.c',
    'src/engine/demod.c',
    'src/engine/audio_pw.c',
  ] + engine_sources + ui_sources,
  include_directories : [vendor_inc, src_inc, engine_inc],
  # engine (protocol2 + analyzer + demod + PipeWire audio) alongside the v0
  # network client; WDSP's comm.h/wdsp.h need the GNU dialect (as sdrfl-wdsp-smoke).
  dependencies : common_deps + [glib_dep, wdsp_dep, m_dep, pipewire_dep],
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

# Headless Protocol-2 RX -> WDSP demod -> PipeWire audio. Milestone-2 gate.
# TAKES THE RADIO. SDRFL_TONE=1 plays a test tone with no radio.
executable('sdrfl-audioprobe',
  ['src/audioprobe_main.c', 'src/engine/demod.c', 'src/engine/audio_pw.c'] + engine_sources,
  include_directories : [vendor_inc, src_inc, engine_inc],
  dependencies : engine_deps + [wdsp_dep, m_dep, pipewire_dep],
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
