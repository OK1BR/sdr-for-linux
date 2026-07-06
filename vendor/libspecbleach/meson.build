# libspecbleach — vendored static library (see VENDOR.md for provenance).
# WDSP's sbnr.c links this for spectral-bleaching noise reduction. Built from the
# exact SOURCES set of the upstream Makefile (some sources present in the tree are
# deliberately NOT built: spectral_denoiser.c, noise_estimator.c, noise_profile.c,
# spectral_trailing_buffer.c). Needs fftw3f. Do NOT modify the sources.

specbleach_public_inc = include_directories('include')

libspecbleach = static_library('specbleach',
  [
    'src/processors/specbleach_adenoiser.c',
    'src/processors/specbleach_denoiser.c',
    'src/processors/adaptivedenoiser/adaptive_denoiser.c',
    'src/shared/gain_estimation/gain_estimators.c',
    'src/shared/noise_estimation/adaptive_noise_estimator.c',
    'src/shared/pre_estimation/absolute_hearing_thresholds.c',
    'src/shared/pre_estimation/critical_bands.c',
    'src/shared/pre_estimation/masking_estimator.c',
    'src/shared/pre_estimation/noise_scaling_criterias.c',
    'src/shared/pre_estimation/spectral_smoother.c',
    'src/shared/pre_estimation/transient_detector.c',
    'src/shared/post_estimation/noise_floor_manager.c',
    'src/shared/post_estimation/postfilter.c',
    'src/shared/post_estimation/spectral_whitening.c',
    'src/shared/utils/denoise_mixer.c',
    'src/shared/utils/general_utils.c',
    'src/shared/utils/spectral_features.c',
    'src/shared/utils/spectral_utils.c',
    'src/shared/stft/stft_processor.c',
    'src/shared/stft/fft_transform.c',
    'src/shared/stft/stft_buffer.c',
    'src/shared/stft/stft_windows.c',
  ],
  include_directories : [
    specbleach_public_inc,
    include_directories('src'),
    include_directories('src/shared'),
  ],
  override_options : ['warning_level=0', 'c_std=gnu11'],
  dependencies : [fftw3f_dep, m_dep],
  install : false,
)

specbleach_dep = declare_dependency(
  link_with : libspecbleach,
  include_directories : specbleach_public_inc,
)
