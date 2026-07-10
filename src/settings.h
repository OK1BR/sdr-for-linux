/*
 * settings.h — persistent app state (GKeyFile INI at
 * $XDG_CONFIG_HOME/sdr-for-linux/config.ini). Remembers the direct-radio
 * operating state across runs so the app no longer starts at defaults.
 *
 * Precedence is applied by the caller (gui.c): env var > saved value >
 * built-in default. This module only reads/writes the file.
 *
 * UI rule (Richard, 2026-07-09): every setting that only takes effect on the
 * next launch MUST say "· restart to apply" in its own row subtitle — users read
 * the row, not the group header, and an unmarked delayed change reads as broken.
 * Restart-to-apply here: ip, rate, latency, audio_rate, audio_device, mic_device.
 * Everything else applies live.
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
  int       auto_level;/* auto-track the noise floor on the dB axis   */
  int       avg_spec;  /* spectrum-trace averaging time constant (ms) */
  int       avg_wf;    /* waterfall averaging time constant (ms)      */
  int       palette;   /* colour scheme index (waterfall + spectrum)  */
  char      region[8]; /* band-plan region key: "R1" / "R2" / "R3"    */
  char      country[8];/* band-plan national overlay: "" / "CZ" / "US"*/
  int       band_edges;/* draw band-plan edges + band name overlay    */
  int       show_spots;/* draw TCI DX-spot overlay (F6d-2e)           */
  int       spot_ttl;  /* spot lifetime, minutes (contest 1, DX 10)   */
  int       win_w;     /* window width (px, 0 = default)             */
  int       win_h;     /* window height (px, 0 = default)            */
  int       win_max;   /* window maximized                          */
  char      band_levels[512]; /* per-band dB window, "key=hi/lo;..." */
  int       atten;     /* ADC0 step attenuator (dB, 0-31)            */
  int       agc;       /* AGC mode 0=off,1=long,2=slow,3=med,4=fast  */
  double    agc_gain;  /* AGC-T threshold/gain (dB)                  */
  int       filter;    /* filter-preset index in the mode's table (-1 = default) */
  int       nr, nb, anf; /* noise reduction / blanker / auto-notch on-off        */
  int       binaural;    /* binaural stereo audio output on-off                  */
  char      mode_filt[128]; /* per-mode filter memory, "modeid=idx;..."          */
  char      var_filt[512];  /* Var1/Var2 per mode, "modeid/v1lo/v1hi/v2lo/v2hi;.."*/
  /* TX (F6a) — persistent; PA-enable mirrors piHPSDR's persistent PA setting. */
  int       tx_pa;      /* PA enable (0/1) — RF impossible when 0                 */
  int       tx_ant;     /* TX/RX antenna 0/1/2 → ANT1/2/3                         */
  double    tx_drive;   /* MOX/voice power request (W)                            */
  double    tx_digi_max;/* ⛔ drive cap in DIGU/DIGL, W (100 = uncapped)          */
  double    tx_tune;    /* TUNE power request (W)                                 */
  double    tx_swr;     /* SWR trip threshold                                     */
  double    mic_gain;   /* TX mic gain (dB, SSB voice) — SetTXAPanelGain1         */
  int       tx_comp;    /* speech processor (PROC) on/off                         */
  double    tx_comp_db; /* PROC compression (dB, 0-20; 0 = auto-leveler only)     */
  int       tx_gate;    /* mic noise gate (DEXP) on/off                           */
  double    tx_gate_db; /* gate threshold (dBFS, post-mic-gain; -60..-10)         */
  int       tx_ptt;     /* footswitch: radio PTT input keys like MOX (0/1)        */
  int       tx_ptt_tip; /* PTT contact on mic-jack tip (0 = ring, Apache default) */
  int       ps_enable;  /* PureSignal on/off (F7/PS-2)                            */
  int       ps_att;     /* PS ADC0 feedback attenuator during TX (dB, 0-31)       */
  double    ps_setpk;   /* PS SetPk (0.01-1.01; 0 = unset → default 0.2899)       */
  int       tx_mon;     /* TX monitor (self-listen: voice mic / CW sidetone)      */
  double    tx_mon_db;  /* monitor level (dB, -40..0)                             */
  double    tx_flo;     /* TX audio filter low edge (Hz; default 150)             */
  double    tx_fhi;     /* TX audio filter high edge (Hz; default 2850, eSSB up)  */
  int       cw_wpm;     /* CW keyer speed (WPM, F6d-1c)                           */
  int       cw_pitch;   /* CW sidetone pitch (Hz)                                 */
  double    cw_st_db;   /* CW sidetone level (dBFS before monitor gain; -40..0)   */
  int       cw_hang;    /* CW break-in hang (ms)                                  */
  int       tci_enable; /* TCI server on/off (F6d-2a)                             */
  int       tci_port;   /* TCI server port (ExpertSDR default 40001)              */
  int       tci_iq_rate;/* device-global IQ stream rate (F6d-2d; client-set)      */
  double    tx_pan_high, tx_pan_low; /* TX panadapter dB window (0/0 = autofit)   */
  char      pa_cal[256];  /* per-band PA calibration (dB), "6m=53.0;20m=53.0;.." */
  char      pa_trim[256]; /* wattmeter correction curve, 11 pts "p0;p1;..;p10"   */
  int       audio_rate;   /* shared audio sample rate (RX out + TX mic), Hz        */
  char      audio_device[128]; /* RX playback: PW node.name ("" = default)         */
  char      mic_device[128];   /* TX mic capture: PW node.name ("" = default)      */
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
