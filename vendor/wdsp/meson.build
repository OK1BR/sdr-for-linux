# WDSP — vendored DSP library (see VENDOR.md for provenance).
#
# This is piHPSDR's WDSP. We build & link it; we do NOT modify it (warnings are
# silenced, not patched). Source set == the upstream Makefile OBJS list, so the
# generator tool make_zetahat.c is NOT compiled. rnnr.c / sbnr.c pull in the
# vendored rnnoise / libspecbleach headers; emnr.c reads the runtime data files
# `calculus` and `zetaHat.bin` (only when EMNR is active — a later milestone).
#
# WDSP is headless (comm.h pulls no GTK/GLib), which is exactly why the engine
# can live under our GTK4 UI.

wdsp_inc = include_directories('.')

libwdsp = static_library('wdsp',
  [
    'amd.c',
    'ammod.c',
    'amsq.c',
    'analyzer.c',
    'anf.c',
    'anr.c',
    'apfshadow.c',
    'bandpass.c',
    'calcc.c',
    'calculus.c',
    'cblock.c',
    'cfcomp.c',
    'cfir.c',
    'channel.c',
    'cmath.c',
    'compress.c',
    'delay.c',
    'dexp.c',
    'div.c',
    'doublepole.c',
    'eer.c',
    'emnr.c',
    'emph.c',
    'eq.c',
    'fcurve.c',
    'FDnoiseIQ.c',
    'fir.c',
    'firmin.c',
    'fmd.c',
    'fmmod.c',
    'fmsq.c',
    'gain.c',
    'gaussian.c',
    'gen.c',
    'icfir.c',
    'iir.c',
    'impulse_cache.c',
    'iobuffs.c',
    'iqc.c',
    'linux_port.c',
    'lmath.c',
    'main.c',
    'matchedCW.c',
    'meter.c',
    'meterlog10.c',
    'nbp.c',
    'nob.c',
    'nobII.c',
    'osctrl.c',
    'patchpanel.c',
    'resample.c',
    'rmatch.c',
    'rnnr.c',
    'RXA.c',
    'sbnr.c',
    'sender.c',
    'shift.c',
    'siphon.c',
    'slew.c',
    'snb.c',
    'ssql.c',
    'syncbuffs.c',
    'TXA.c',
    'utilities.c',
    'varsamp.c',
    'version.c',
    'wcpAGC.c',
    'wisdom.c',
    'zetaHat.c',
  ],
  include_directories : wdsp_inc,
  # GNU dialect (as upstream builds it): comm.h neutralises Windows __declspec
  # only under `#if defined(linux)`, and the bare `linux` macro exists only in
  # the GNU dialect — strict -std=c11 would leave __declspec unhandled.
  override_options : ['warning_level=0', 'c_std=gnu11'],
  dependencies : [fftw3f_dep, threads_dep, m_dep, rnnoise_dep, specbleach_dep],
  install : false,
)

wdsp_dep = declare_dependency(
  link_with : libwdsp,
  include_directories : wdsp_inc,
  dependencies : [fftw3f_dep, threads_dep, m_dep, rnnoise_dep, specbleach_dep],
)
