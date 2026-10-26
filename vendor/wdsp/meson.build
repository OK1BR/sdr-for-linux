# WDSP — vendored DSP library (see VENDOR.md for provenance).
#
# This is piHPSDR's WDSP (2.11). We build & link it; we do NOT modify it
# (warnings are silenced, not patched). Source set == the upstream Makefile
# OBJS list, generated from it — so the generator/helper tools (make_calculus.c,
# make_interface.c, make_zetahat.c, snoop.c) are NOT compiled. The four
# nnr_model_*.c files are the built-in neural noise-reduction models
# (generated C arrays, ~57 MB of source). sbnr.c pulls in the vendored
# libspecbleach headers; emnr.c carries compiled-in copies of the `calculus`
# and `zetaHat.bin` data (calculus.c / zetaHat.c), the files are optional.
#
# WDSP is headless (comm.h pulls no GTK/GLib), which is exactly why the engine
# can live under our GTK4 UI.

wdsp_inc = include_directories('.')

libwdsp = static_library('wdsp',
  [
    'linux_port.c',
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
    'extrapolate.c',
    'fcurve.c',
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
    'lmath.c',
    'main.c',
    'matchedCW.c',
    'meter.c',
    'meterlog10.c',
    'nbp.c',
    'nnet.c',
    'nnio.c',
    'nnr.c',
    'nnrbypass.c',
    'nnr_model_0.c',
    'nnr_model_1.c',
    'nnr_model_2.c',
    'nnr_model_3.c',
    'nob.c',
    'nobII.c',
    'nurbs.c',
    'nurbs_fit.c',
    'nurbs_spline.c',
    'osctrl.c',
    'patchpanel.c',
    'phrot.c',
    'resample.c',
    'reshb.c',
    'rmatch.c',
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
    'version.c',
    'varsamp.c',
    'wbfm.c',
    'wcpAGC.c',
    'wisdom.c',
    'zetaHat.c',
  ],
  include_directories : wdsp_inc,
  # GNU dialect (as upstream builds it): comm.h neutralises Windows __declspec
  # only under `#if defined(linux)`, and the bare `linux` macro exists only in
  # the GNU dialect — strict -std=c11 would leave __declspec unhandled.
  # -D_GNU_SOURCE is upstream's own CFLAGS (wdsp/Makefile).
  override_options : ['warning_level=0', 'c_std=gnu11'],
  c_args : ['-D_GNU_SOURCE'],
  dependencies : [fftw3f_dep, fftw3_dep, threads_dep, m_dep, specbleach_dep],
  install : false,
)

wdsp_dep = declare_dependency(
  link_with : libwdsp,
  include_directories : wdsp_inc,
  dependencies : [fftw3f_dep, fftw3_dep, threads_dep, m_dep, specbleach_dep],
)
