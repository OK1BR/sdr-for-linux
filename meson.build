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
gtk_dep     = dependency('gtk4')
opus_dep    = dependency('opus')
openssl_dep = dependency('openssl')
zlib_dep    = dependency('zlib')

vendor_inc = include_directories('vendor/pihpsdr')
src_inc    = include_directories('src')

common_deps = [gtk_dep, opus_dep, openssl_dep, zlib_dep]

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
  ] + ui_sources,
  include_directories : [vendor_inc, src_inc],
  dependencies : common_deps,
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
