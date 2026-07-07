/*
 * settings.h — persistent app state (GKeyFile INI at
 * $XDG_CONFIG_HOME/sdr-for-linux/config.ini). Remembers the direct-radio
 * operating state across runs so the app no longer starts at defaults.
 *
 * Precedence is applied by the caller (gui.c): env var > saved value >
 * built-in default. This module only reads/writes the file.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SDRFL_SETTINGS_H
#define SDRFL_SETTINGS_H

typedef struct {
  char      ip[64];    /* radio IP                                   */
  long long freq;      /* tuned DDC centre (Hz)                      */
  int       rate;      /* IQ sample rate (Hz) = panadapter span      */
  int       mode;      /* demod mode (DEMOD_*)                       */
  double    volume;    /* AF gain (dB)                               */
  double    gain;      /* digital master gain                        */
  int       fps;       /* panadapter frame rate                      */
  int       latency;   /* audio target latency (ms)                  */
  int       step;      /* scroll-tuning step (Hz)                    */
  double    zoom;      /* panadapter zoom factor (1 = full span)     */
  double    pan_high;  /* dB scale top (dBm)                         */
  double    pan_low;   /* dB scale bottom (dBm)                      */
  int       db_grid;   /* show horizontal (dB) grid lines            */
  int       db_scale;  /* show dB scale labels (left)                */
  int       freq_grid; /* show vertical (frequency) grid lines       */
  int       freq_scale;/* show frequency scale labels (top)          */
  int       filter_wf; /* draw filter + centre on the waterfall too  */
  int       filter_op; /* filter-overlay opacity (0-100 %)           */
  int       win_w;     /* window width (px, 0 = default)             */
  int       win_h;     /* window height (px, 0 = default)            */
  int       win_max;   /* window maximized                          */
  char      band_levels[512]; /* per-band dB window, "key=hi/lo;..." */
  int       atten;     /* ADC0 step attenuator (dB, 0-31)            */
  int       agc;       /* AGC mode 0=off,1=long,2=slow,3=med,4=fast  */
  double    agc_gain;  /* AGC-T threshold/gain (dB)                  */
  int       filter;    /* filter-preset index in the mode's table (-1 = default) */
  int       nr, nb, anf; /* noise reduction / blanker / auto-notch on-off        */
} Settings;

/*
 * Overlay saved values onto *s (pre-fill it with defaults first). Only keys
 * actually present in the file are written; missing keys keep their incoming
 * value. Returns 1 if the config file existed and parsed, 0 otherwise.
 */
int settings_load(Settings *s);

/* Write *s to the config file, creating the directory if needed. Returns 0 on ok. */
int settings_save(const Settings *s);

/* Absolute path of the config file (static buffer; valid for the process). */
const char *settings_path(void);

#endif /* SDRFL_SETTINGS_H */
