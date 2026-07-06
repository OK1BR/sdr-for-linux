# rnnoise — vendored static library (see VENDOR.md for provenance).
# WDSP's rnnr.c links this for its RNNoise-based noise reduction. Built from the
# exact SOURCES set of the upstream Makefile (tool programs with their own main()
# — dump_features.c, write_weights.c, dump_rnnoise_tables.c — are NOT compiled).
# Do NOT modify the sources; warnings are silenced rather than patched.

rnnoise_public_inc = include_directories('include')

librnnoise = static_library('rnnoise',
  [
    'src/denoise.c',
    'src/celt_lpc.c',
    'src/kiss_fft.c',
    'src/nnet.c',
    'src/nnet_default.c',
    'src/parse_lpcnet_weights.c',
    'src/pitch.c',
    'src/rnn.c',
    'src/rnnoise_data.c',
    'src/rnnoise_data_1.c',
    'src/rnnoise_data_2.c',
    'src/rnnoise_data_3.c',
    'src/rnnoise_data_4.c',
    'src/rnnoise_data_5.c',
    'src/rnnoise_data_6.c',
    'src/rnnoise_tables.c',
  ],
  include_directories : [rnnoise_public_inc, include_directories('src')],
  override_options : ['warning_level=0', 'c_std=gnu11'],
  dependencies : [threads_dep, m_dep],
  install : false,
)

rnnoise_dep = declare_dependency(
  link_with : librnnoise,
  include_directories : rnnoise_public_inc,
)
