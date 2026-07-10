/*
 * gui.c — GTK4 panadapter + waterfall window for sdr-for-linux.
 *
 * Two data sources, same renderer (panadapter.{h,c} / waterfall.{h,c}):
 *   - default: DIRECT RADIO — HPSDR Protocol-2 discovery + RX (engine/protocol2)
 *     feeding the WDSP analyzer (engine/analyzer); full float pixels. TAKES THE
 *     radio (one owner) — piHPSDR must be disconnected.
 *   - `--server [host [port [pwd]]]`: the v0 network path (client.{h,c}) — a
 *     remote head onto a running piHPSDR server.
 *
 * A GdkFrameClock tick pulls the latest frame (analyzer or network) and redraws.
 *
 * Usage:  sdr-for-linux [--server [host [port [pwd]]]]
 *   env:  SDRFL_RADIO_IP, SDRFL_FREQ, SDRFL_RATE, SDRFL_SOFFSET   (radio mode)
 *         PIHPSDR_PWD                                             (server mode)
 * Run with GSK_RENDERER=cairo to avoid the NVIDIA+Wayland GTK4 GL crash.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <gtk/gtk.h>
#include <adwaita.h>
#include <glib-unix.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "client.h"
#include "panadapter.h"
#include "waterfall.h"

#include <strings.h>

#include "discovered.h"
#include "discovery.h"
#include "protocol2.h"
#include "analyzer.h"
#include "demod.h"
#include "audio.h"
#include "mic_pw.h"
#include "settings.h"
#include "bandplan.h"
#include "wisdom_gate.h"
#include "picker.h"
#include "tci_server.h"
#include "tx_run.h"
#include "tx.h"   /* tx_dsp_in_rate() — mic capture rate must match the WDSP TX input */

#define ENGINE_PIXELS 2048
#define ENGINE_FPS    25
/* Frames to average before locking the display offset (~1 s at ENGINE_FPS). */
#define SETTLE_FRAMES 15
/* Where the noise floor lands after auto-levelling (dBm, inside the pan window). */
#define TARGET_FLOOR  -115.0

#define NBANDS 11   /* HF+6m bands we remember per-band dB levels for */

/* After a TX→RX transition, silence the DEMOD input for a short while so the
 * "tail" of the T/R-relay crosstalk doesn't pump the AGC (piHPSDR txrxmax:
 * receiver.c:1355-1369, radio.c:2168-2207 — 31 ms for TUNE). The panadapter
 * keeps its live RX IQ, so only the audio path (AGC) is silenced. */
#define RX_SILENCE_MS 20    /* brief demod-input silence to swallow the literal T/R tail */
#define TX_SETTLE_MS  200   /* keep RX audio muted + ADC-OVL suppressed this long after */
                            /* unkey, so the AGC recovers before the audio fades back in */
static volatile int g_rx_silence;   /* g_atomic: RX IQ pairs still to silence to the demod */

typedef struct {
  int         radio_mode;   /* 1 = direct radio, 0 = network server            */

  /* network (server) path */
  Client     *client;
  int         conn_err;

  /* radio (engine) path */
  int         engine_ok;    /* discovery + RX started                          */
  int         audio_ok;     /* demod + audio sink up                           */
  long long   freq;         /* DDC centre = tuned frequency (Model A)          */
  int         mode;         /* current demod mode (DEMOD_*)                    */
  int         rate;         /* IQ sample rate = full panadapter span (Hz)      */
  double      volume;       /* AF gain (dB)                                    */
  double      gain;         /* digital master gain                            */
  int         atten;        /* ADC0 step attenuator (dB, 0-31)                 */
  int         agc;          /* AGC mode 0=off,1=long,2=slow,3=med,4=fast       */
  double      agc_gain;     /* AGC-T threshold/gain (dB)                       */
  int         nr, nb, anf;  /* noise reduction / blanker / auto-notch on-off   */
  int         binaural;     /* binaural stereo audio output on-off             */
  int         fps;          /* panadapter frame rate                          */
  int         latency;      /* audio target latency (ms)                      */
  int         audio_rate;   /* shared audio sample rate (RX out + TX mic), Hz  */
  char        audio_device[128]; /* RX playback PW node.name ("" = default)     */
  char        mic_device[128]; /* TX mic capture PW node.name ("" = default)   */
  audio_sink  audio_sinks[16]; /* enumerated playback devices (picker)          */
  int         audio_nsink;  /* count in audio_sinks                            */
  mic_source  mic_srcs[16]; /* enumerated capture devices (for the picker)     */
  int         mic_nsrc;     /* count in mic_srcs                               */
  char        radio_ip[64]; /* resolved radio IP (for persistence)            */
  guint       save_timer_id;/* debounced settings save (0 = none pending)     */
  long long   drag_base_freq; /* app->freq at drag-begin (pan is absolute)     */
  int         pixels;
  float       eng_raw[SPECTRUM_DATA_SIZE];
  double      soffset;
  int         soffset_locked;
  int         cal_frames;

  int         connected;    /* data source is up (either path)                 */

  ClientFrame frame;        /* metadata for the readouts (+ dbm on net path)   */
  uint64_t    last_seq;
  int         have_frame;

  /* Time-averaged trace, held in dBm (so the renderer is source-agnostic). */
  float       ema[SPECTRUM_DATA_SIZE];
  int         ema_w;
  float       wf_ema[SPECTRUM_DATA_SIZE];   /* separate averaging for the waterfall */
  int         wf_ema_w;
  int         avg_spec_ms;   /* spectrum-trace averaging time constant (ms)        */
  int         avg_wf_ms;     /* waterfall averaging time constant (ms)             */
  int         palette;       /* colour-scheme index (waterfall + spectrum)         */

  Waterfall  *wf;
  GtkWidget  *area;
  GtkWidget  *mode_btns[DEMOD_NMODES];  /* toggle per DEMOD_* id (keys ↔ buttons) */
  double      zoom;          /* current display zoom (1 = full span)           */
  long long   tune_step;     /* scroll-tuning step (Hz); freq snaps to it       */
  GtkWidget  *step_dd;       /* footer step selector                           */
  GtkWidget  *span_label;    /* footer span readout                            */
  double      pending_zoom;  /* slider target; applied ≤1×/frame in tick_cb    */
  int         zoom_dirty;    /* pending_zoom needs applying                    */
  int         filter_idx;    /* selected preset (0-9) or Var (10=Var1, 11=Var2)  */
  int         filter_by_mode[DEMOD_NMODES]; /* remembered preset per mode (-1=dflt) */
  int         var_low[DEMOD_NMODES][2], var_high[DEMOD_NMODES][2]; /* Var1/2 (Hz)   */
  int         drag_edge;     /* dragging a Var passband edge: 0 none, 1 low, 2 high */
  double      drag_begin_x;  /* pointer x at drag-begin (for edge drag)           */
  double      pan;           /* off-centre pan [-1,1]: 0 = VFO centred (zoom>1 only) */
  int         drag_pan;      /* shift+drag is panning the view (not tuning)        */
  double      drag_base_pan; /* pan captured at drag-begin (for shift+drag)        */
  double      flo, fhi;      /* current passband (Hz, rel. centre) — for drawing */
  GtkWidget  *filter_dd;     /* filter dropdown (repopulated per mode)         */
  gint64      ovl0_until;    /* ADC0-overload badge lit until (monotonic µs)   */
  gint64      ovl1_until;    /* ADC1-overload badge lit until (monotonic µs)   */
  int         tlm_valid;     /* HP-status telemetry seen at least once          */
  double      supply_v;      /* supply voltage (V), EMA-smoothed; 0 = unknown   */
  GtkWidget  *volt_label;    /* footer supply-voltage readout                   */
  double      volt_shown;    /* last value pushed to volt_label (markup throttle)*/
  double      pan_high, pan_low;              /* visible dB window (dBm)          */
  int         drag_gutter;   /* the active drag started in the left dB gutter    */
  double      drag_base_high, drag_base_low;  /* pan window captured at drag-begin */
  /* Right-button drag = zoom (dB range in the gutter, freq span on the body); a
   * right-click with no motion still toggles select mode. */
  int         rdrag_gutter;  /* right-drag started in the dB gutter               */
  double      rdrag_base_high, rdrag_base_low; /* dB window at right-drag begin    */
  double      rdrag_anchor_frac;               /* cursor y fraction at begin (anchor)*/
  double      rdrag_base_zoom;                 /* app->zoom at right-drag begin      */
  int         rdrag_zoomed;                    /* right-drag actually zoomed (vs a click)*/
  GtkWidget  *zoom_scale;    /* footer zoom slider (kept in sync by right-drag)   */
  double      ptr_x, ptr_y;  /* last pointer pos over the area (scroll hit-test)  */
  int         show_db_grid, show_db_scale;     /* horizontal grid / dB labels     */
  int         show_freq_grid, show_freq_scale; /* vertical grid / freq labels      */
  int         show_filter_wf; /* extend filter edges + centre onto the waterfall  */
  int         filter_op;     /* filter-overlay opacity (0-100 %)                  */
  int         select_mode;   /* right-click select cursor: left-click = tune      */
  int         auto_level;    /* auto-track the noise floor on the dB axis          */
  int         bp_region;     /* band-plan region index (0=R1) — see bandplan.h     */
  int         bp_country;    /* band-plan country/overlay index (0=none)           */
  int         show_band_edges; /* draw band-plan edges + band name on the spectrum */
  /* DX spots pushed over TCI (F6d-2e; SDC streams its skimmer decodes). The
   * store lives here (main thread only: TCI ops + draw + click). hit_* are
   * the label rectangles of the last draw, for click-to-tune. */
#define MAX_SPOTS 192
  struct spot {
    char      call[20], mode[12];
    long long hz;
    unsigned  argb;
    gint64    ts;              /* g_get_monotonic_time of (re)announcement    */
    double    hx0, hx1, hy0, hy1;  /* label hit box (px; 0 = not drawn)       */
  }           spots[MAX_SPOTS];
  int         nspots;
  int         show_spots;    /* draw the spot overlay (persisted)              */
  GtkWidget  *win;           /* top-level window (for size persistence)           */
  GtkWidget  *toast_overlay; /* AdwToastOverlay wrapping the content (restart hint) */
  int         restart_pending;     /* a restart-to-apply setting changed this session */
  int         restart_toast_shown; /* a "restart to apply" toast is live (dedupe)   */
  int         win_w, win_h, win_max;  /* remembered window geometry               */
  double      band_high[NBANDS], band_low[NBANDS];  /* per-band dB window          */
  int         band_mode[NBANDS];  /* per-band remembered demod mode (band stacking) */
  long long   band_freq[NBANDS];  /* per-band last frequency                       */
  GtkWidget  *band_btns[NBANDS];  /* band buttons (NULL if no button), for highlight */
  int         cur_band;      /* index into BANDS for app->freq (-1 = out of band) */

  /* TX (F6a) — the runtime lives in engine/tx_run; the GUI only expresses intent
   * (TUNE) and reflects status. MOX waits for the mic path (F6c). */
  int         tx_ready;      /* tx_run started (WDSP TX channel + worker thread up) */
  int         tx_pa_enabled; /* PA enable — RF impossible when 0 (persisted)        */
  int         tx_antenna;    /* TX/RX antenna 0/1/2 → ANT1/2/3 (persisted)          */
  double      tx_drive_w;    /* MOX/voice power request, W (persisted)              */
  double      tx_tune_w;     /* TUNE power request, W (persisted)                   */
  double      tx_swr_alarm;  /* SWR trip threshold (persisted)                      */
  double      tx_mic_gain;   /* TX mic gain, dB — SSB voice (persisted)             */
  int         tx_comp;       /* speech processor (PROC) on/off (persisted)          */
  double      tx_comp_db;    /* PROC compression dB, 0-20 (persisted)               */
  int         tx_gate;       /* mic noise gate (DEXP) on/off (persisted)            */
  double      tx_gate_db;    /* gate threshold dBFS post-mic-gain (persisted)       */
  char        picked_ip[64]; /* radio chosen in the startup picker ("" = none)      */
  int         tci_enable;    /* TCI server on/off (persisted, F6d-2a)               */
  int         tci_port;      /* TCI server port (persisted; ExpertSDR default 40001)*/
  int         tci_iq_rate;   /* device-global IQ stream rate (persisted; client-set)*/
  GtkWidget  *tci_client_row; /* live client list on the TCI prefs page             */
  guint       tci_timer;     /* 1 s refresh for the client row                      */
  int         tx_mon;        /* TX monitor (self-listen) on/off (persisted)         */
  double      tx_mon_db;     /* monitor level dB (persisted)                        */
  double      tx_flo;        /* TX audio filter low edge, Hz (persisted)            */
  double      tx_fhi;        /* TX audio filter high edge, Hz (persisted; eSSB up)  */
  int         cw_wpm;        /* CW keyer speed, WPM (persisted, F6d-1c)             */
  int         cw_pitch;      /* CW sidetone pitch, Hz (persisted)                   */
  double      cw_st_db;      /* CW sidetone level, dBFS (persisted)                 */
  int         cw_hang;       /* CW break-in hang, ms (persisted)                    */
  double      band_pacal[NBANDS]; /* per-band PA calibration, dB (F6b, persisted)   */
  double      pa_trim[11];   /* wattmeter correction curve, 11 pts W (F6b, persist) */
  GtkWidget  *pacal_spin[NBANDS]; /* per-band PA-cal spin buttons in Preferences    */
  GtkWidget  *patrim_spin[11];    /* wattmeter-trim spin buttons in Preferences     */
  GtkWidget  *tune_btn;      /* TUNE toggle                                         */
  GtkWidget  *mox_btn;       /* MOX toggle (disabled until F6c)                     */
  GtkWidget  *tx_label;      /* live TX power/SWR + refusal reason readout          */
  char        tx_reason[64]; /* last refusal/trip reason to flash                   */
  gint64      tx_reason_until;/* monotonic µs until which to show tx_reason         */
  int         tx_keyed_shown;/* last keyed state pushed to the label (repaint gate) */
  double      tx_swr_disp;   /* displayed SWR: held while no power flows            */
  int         mic_open;      /* PipeWire mic capture running (voice modes only, F6c) */
  /* TX panadapter: while keyed we show the transmitted spectrum (24 kHz span,
   * full area, no waterfall) in place of the RX view — like piHPSDR non-duplex. */
  int         tx_display;    /* currently showing the TX panadapter (keyed)         */
  float       tx_raw[SPECTRUM_DATA_SIZE];  /* latest TX analyzer pixels (dB)         */
  float       tx_ema[SPECTRUM_DATA_SIZE];  /* smoothed TX trace (dB)                 */
  int         tx_ema_w;      /* width of tx_ema (0 = no frame yet)                   */
  double      tx_pan_high, tx_pan_low;  /* TX panadapter dB window (manual, draggable)*/
  int         tx_pan_init;   /* one-shot autofit has placed the TX window this run   */
  ClientFrame tx_frame;      /* metadata for the TX readout (carrier freq)           */
  double      tx_fwd_shown;  /* fwd power last drawn in the TX banner                */
  Waterfall  *tx_wf;         /* TX waterfall (transmitted spectrum history)          */
  gint64      tx_settle_until;/* monotonic µs: keep RX audio muted + ADC-OVL badge   */
                             /* suppressed through the TX→RX AGC-recovery window     */
} App;

/* dB-scale limits for the grab-to-move axis. */
#define PAN_HIGH_DEFAULT  -50.0
#define PAN_LOW_DEFAULT  -140.0
#define PAN_RANGE_MIN      20.0    /* min dB window (max vertical zoom-in)  */
#define PAN_RANGE_MAX     160.0    /* max dB window (max zoom-out)          */
#define ZOOM_MAX          128.0    /* max frequency zoom (octave 7)         */
#define PAN_DBM_CEIL       20.0    /* the top of the window can't exceed    */
#define PAN_DBM_FLOOR    -200.0    /* the bottom of the window can't go below */
#define AUTO_FLOOR_FRAC    0.12    /* auto-level: keep the noise floor this far up */

/* HF bands [lo,hi] Hz + a config key. The dB window is remembered per band, so
 * the noise-floor placement follows you across bands. Order defines the
 * persistence layout; the key survives reordering. */
static const struct { long long lo, hi; const char *key; long long dflt; } BANDS[NBANDS] = {
  { 1800000,  2000000, "160m",  1840000 }, { 3500000,  4000000, "80m",  3600000 },
  { 5250000,  5450000, "60m",   5357000 }, { 7000000,  7300000, "40m",  7074000 },
  {10100000, 10150000, "30m",  10136000 }, {14000000, 14350000, "20m", 14074000 },
  {18068000, 18168000, "17m",  18100000 }, {21000000, 21450000, "15m", 21074000 },
  {24890000, 24990000, "12m",  24915000 }, {28000000, 29700000, "10m", 28074000 },
  {50000000, 54000000, "6m",   50313000 },
};

static int band_for_freq(long long f) {
  for (int i = 0; i < NBANDS; i++) { if (f >= BANDS[i].lo && f <= BANDS[i].hi) { return i; } }
  return -1;
}

/* Device-specific TX-calibration DEFAULTS, pulled from piHPSDR for the ANAN G1.
 * These would differ for another radio type (piHPSDR device-switches them in
 * radio.c) — if this app ever supports a non-G1, they must be switched too.
 *
 *   pa_calibration: piHPSDR band.c table = 53.0 dB for every band (the only
 *     device override is HermesLite2 → 40.5; the G1 keeps 53.0). Live-validated
 *     on this G1 (docs/TX-DESIGN.md §5). The 38.8 dB FLOOR (band.c:571-577) is the
 *     safety limit — a lower value raises the drive byte for a given watts request.
 *   pa_trim step: piHPSDR radio.c:1308 sets the G1 to pa_power=PA_100W (100 W
 *     rated), so radio.c:1330 seeds pa_trim[i] = i * 100 * 0.1 = i * 10 W. */
#define PACAL_MIN     38.8
#define PACAL_MAX     70.0
#define PACAL_DEFAULT 53.0     /* G1: piHPSDR band.c table (not HL2's 40.5)        */
#define PATRIM_STEP   10.0     /* G1: PA_100W → 10 W per 10 % step (radio.c:1330)  */
static inline double pacal_clamp(double v) {
  return v < PACAL_MIN ? PACAL_MIN : (v > PACAL_MAX ? PACAL_MAX : v);
}

/* Keep the current band's remembered levels in step with the live window. */
static void pan_store_band(App *app) {
  if (app->cur_band >= 0) {
    app->band_high[app->cur_band] = app->pan_high;
    app->band_low[app->cur_band]  = app->pan_low;
  }
}

/* Light up the button for the current band (none when out of band). The band
 * buttons are toggles so the :checked style matches the mode buttons under the
 * user's theme. set_active fires "toggled" (unconnected), never "clicked", so
 * this never re-triggers a tune. */
static void update_band_highlight(App *app) {
  for (int i = 0; i < NBANDS; i++) {
    if (!app->band_btns[i]) { continue; }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->band_btns[i]), i == app->cur_band);
  }
}

static void tx_push_cfg(App *app);   /* fwd: re-push TX cfg when the band changes */

/* On a band change, load that band's remembered dB window (out-of-band keeps
 * the current one). Called each tick — freq changes via buttons/tune/click. */
static void band_apply(App *app) {
  int b = band_for_freq(app->freq);
  if (b == app->cur_band) { return; }
  app->cur_band = b;
  update_band_highlight(app);
  if (b >= 0) {
    app->pan_high = app->band_high[b];
    app->pan_low  = app->band_low[b];
    /* band stacking: restore the mode last used on this band (activating the
     * grouped toggle drives demod + per-mode filter + stores it back — idempotent). */
    int m = app->band_mode[b];
    if (m >= 0 && m != app->mode && app->mode_btns[m]) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->mode_btns[m]), TRUE);
    }
    if (app->area) { gtk_widget_queue_draw(app->area); }
  }
  /* The new band has its own PA calibration → recompute the drive byte. Guarded
   * internally by tx_ready; no-op until TX is up. */
  tx_push_cfg(app);
}

/* Supply-voltage calibration: V = k * raw_adc1. No G1 divider is documented, so
 * k is anchored empirically — Richard's Microset measured 13.46 V while the G1
 * reported raw_adc1 ~= 797.5 (bytes 55-56). SDRFL_VOLT_CAL overrides k (V per
 * count) for a precise re-trim without a rebuild. */
#define SUPPLY_V_PER_COUNT (13.46 / 797.5)
#define SUPPLY_V_EMA 0.1   /* smooths the ±1-count (~±0.017 V) raw jitter */

/* How long an ADC-overload badge stays lit after a clip (µs). A clip may span
 * just one 50 ms status packet — hold it so it's visible at any frame rate. */
#define ADC_OVL_HOLD_US 700000

#define PANADAPTER_FRACTION 0.5
#define EMA_FACTOR 0.55f   /* network-path trace EMA (fixed) */

/* EMA weight for a time constant `ms` at `fps` frames/s (log-domain, on dBm).
 * ms <= 0 → no averaging (weight 1). */
static float ema_factor_ms(int ms, int fps) {
  if (ms <= 0) { return 1.0f; }
  double dt = 1000.0 / (fps > 0 ? fps : 25);
  double f = 1.0 - exp(-dt / (double)ms);
  if (f > 1.0) { f = 1.0; }
  if (f < 0.01) { f = 0.01; }
  return (float)f;
}

/* Scroll-tuning steps (Hz): the frequency snaps to a multiple of the active one.
 * Labels are NULL-terminated for gtk_drop_down_new_from_strings(). */
static const long long TUNE_STEPS[] = { 1, 10, 100, 1000, 10000 };
static const char * const TUNE_STEP_LABELS[] = { "1 Hz", "10 Hz", "100 Hz", "1 kHz", "10 kHz", NULL };
#define TUNE_STEP_DEFAULT 100

/* The rate the running engine was actually started with — app->rate may
 * diverge live (the preference is restart-to-apply). */
static int s_engine_rate;

static void feed_cb(const double *iq, int n_pairs, void *user) {
  (void)user;
  analyzer_feed(iq, n_pairs);   /* panadapter — always live */
  /* Raw IQ → TCI skimmers (F6d-2d); no-op while nobody iq_start'ed. Fed the
   * real signal even during the post-TX silence window — a skimmer copes. */
  tci_server_iq_push(iq, n_pairs, s_engine_rate);
  /* Anti-AGC-pump: after TX, feed the demod silence for RX_SILENCE_MS so the
   * relay-crosstalk tail can't kick the AGC (the analyzer still gets real IQ). */
  if (g_atomic_int_get(&g_rx_silence) > 0) {
    static const double zbuf[2 * 4096] = { 0 };
    int done = 0;
    while (done < n_pairs) {
      int chunk = n_pairs - done;
      if (chunk > 4096) { chunk = 4096; }
      demod_feed(zbuf, chunk);
      done += chunk;
    }
    g_atomic_int_add(&g_rx_silence, -n_pairs);
  } else {
    demod_feed(iq, n_pairs);    /* audio */
  }
}

/* Filter presets per mode — piHPSDR filter.c @ 974acba (named presets; Var later). */
typedef struct { int low, high; const char *name; } FilterPreset;

#define NPRESET      10     /* fixed presets per mode; Var1/Var2 follow at 10, 11 */
#define FILT_HIT_PX  7.0    /* how near a Var passband edge counts as a grab      */

static const FilterPreset FILT_LSB[] = {
  {-5150,-150,"5.0k"},{-4550,-150,"4.4k"},{-3950,-150,"3.8k"},{-3450,-150,"3.3k"},
  {-3050,-150,"2.9k"},{-2850,-150,"2.7k"},{-2550,-150,"2.4k"},{-2250,-150,"2.1k"},
  {-1950,-150,"1.8k"},{-1150,-150,"1.0k"},
};
static const FilterPreset FILT_USB[] = {
  {150,5150,"5.0k"},{150,4550,"4.4k"},{150,3950,"3.8k"},{150,3450,"3.3k"},
  {150,3050,"2.9k"},{150,2850,"2.7k"},{150,2550,"2.4k"},{150,2250,"2.1k"},
  {150,1950,"1.8k"},{150,1150,"1.0k"},
};
static const FilterPreset FILT_CW[] = {   /* CWL/CWU: symmetric around the CW pitch */
  {-500,500,"1.0k"},{-400,400,"800"},{-375,375,"750"},{-300,300,"600"},
  {-250,250,"500"},{-200,200,"400"},{-125,125,"250"},{-50,50,"100"},
  {-25,25,"50"},{-13,13,"25"},
};
static const FilterPreset FILT_AM[] = {
  {-8000,8000,"16k"},{-6000,6000,"12k"},{-5000,5000,"10k"},{-4000,4000,"8k"},
  {-3300,3300,"6.6k"},{-2600,2600,"5.2k"},{-2000,2000,"4.0k"},{-1550,1550,"3.1k"},
  {-1450,1450,"2.9k"},{-1200,1200,"2.4k"},
};

/* Filter table + count + default index for a mode. */
static const FilterPreset *mode_filters(int mode, int *n, int *deflt) {
  *n = 10;
  switch (mode) {
    case DEMOD_LSB:  *deflt = 5; return FILT_LSB;   /* 2.7k */
    case DEMOD_USB:  *deflt = 5; return FILT_USB;   /* 2.7k */
    case DEMOD_CWL:
    case DEMOD_CWU:  *deflt = 4; return FILT_CW;    /* 500  */
    case DEMOD_AM:   *deflt = 4; return FILT_AM;    /* 6.6k */
    case DEMOD_DIGU: *deflt = 2; return FILT_USB;   /* 3.8k — FT8 slots up to ~3.1 kHz */
    case DEMOD_DIGL: *deflt = 2; return FILT_LSB;   /* 3.8k mirrored                   */
    default:         *deflt = 5; return FILT_USB;
  }
}

/* Low/high (Hz) for a filter index in a mode: fixed preset (<NPRESET) or the
 * editable Var1/Var2 stored in the App. */
static void filter_lohi(App *app, int mode, int idx, int *lo, int *hi) {
  if (idx < NPRESET) {
    int nf, dfl; const FilterPreset *ft = mode_filters(mode, &nf, &dfl);
    if (idx < 0) { idx = dfl; }
    *lo = ft[idx].low; *hi = ft[idx].high;
  } else {
    int v = (idx - NPRESET) & 1;
    *lo = app->var_low[mode][v]; *hi = app->var_high[mode][v];
  }
}

/* Parse a mode name (USB/LSB/CWU/CWL/AM) to a DEMOD_* id; -1 if unknown/NULL. */
static int mode_from_name(const char *m) {
  if      (m && !strcasecmp(m, "usb"))  return DEMOD_USB;
  else if (m && !strcasecmp(m, "lsb"))  return DEMOD_LSB;
  else if (m && !strcasecmp(m, "cwu"))  return DEMOD_CWU;
  else if (m && !strcasecmp(m, "cwl"))  return DEMOD_CWL;
  else if (m && !strcasecmp(m, "am"))   return DEMOD_AM;
  else if (m && !strcasecmp(m, "digu")) return DEMOD_DIGU;
  else if (m && !strcasecmp(m, "digl")) return DEMOD_DIGL;
  return -1;
}

static int cmp_float(const void *a, const void *b) {
  float fa = *(const float *)a, fb = *(const float *)b;
  return (fa > fb) - (fa < fb);
}

/* Horizontal frequency graticule: faint vertical lines on "nice" frequencies
 * across the visible span, labelled (MHz) in a ruler strip at the very top edge
 * (above the big VFO readout). Model A: span centre = VFO = app->freq. */
/* Off-centre pan: the view centre is offset from the VFO by this many Hz when
 * zoomed in and panned (app->pan in [-1,1] of the available slide). */
static double pan_offset_hz(App *app) {
  if (app->zoom <= 1.0) { return 0.0; }
  return app->pan * ((double)app->rate * (1.0 - 1.0 / app->zoom) / 2.0);
}
/* Screen x (px) of the VFO / centre line for area width w (w/2 unless panned). */
static double vfo_x(App *app, int w) {
  double hz_per_px = (double)app->rate / app->zoom / w;
  return (double)w / 2.0 - pan_offset_hz(app) / hz_per_px;
}

static void draw_freq_scale(cairo_t *cr, App *app, int w, int ph) {
  if (w < 2 || app->rate <= 0 || app->zoom <= 0.0) { return; }
  double span      = (double)app->rate / app->zoom;   /* Hz across the width */
  double hz_per_px = span / w;
  double pan_off   = pan_offset_hz(app);
  double left_hz   = (double)app->freq + pan_off - span / 2.0;
  double right_hz  = (double)app->freq + pan_off + span / 2.0;

  /* Nice tick step (1/2/5·10ⁿ) targeting ~110 px between ticks. */
  double raw  = hz_per_px * 110.0;
  double mag  = pow(10.0, floor(log10(raw)));
  double nn   = raw / mag;
  double step = (nn <= 1 ? 1 : nn <= 2 ? 2 : nn <= 5 ? 5 : 10) * mag;
  int dec = step >= 1000000 ? 1 : step >= 100000 ? 2 : step >= 1000 ? 3 : step >= 100 ? 4 : 5;

  cairo_select_font_face(cr, FONT_MONO, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 10.0);
  const double ly = 13.0;   /* label baseline — top edge, above the VFO readout */
  for (double f = ceil(left_hz / step) * step; f <= right_hz; f += step) {
    double x = (f - left_hz) / hz_per_px;
    if (app->show_freq_grid) {
      cairo_set_source_rgba(cr, 0.5, 0.6, 0.7, 0.11);        /* full-height line */
      cairo_set_line_width(cr, 1.0);
      cairo_move_to(cr, x + 0.5, 0); cairo_line_to(cr, x + 0.5, ph);
      cairo_stroke(cr);
    }
    if (!app->show_freq_scale) { continue; }

    char lbl[24];
    snprintf(lbl, sizeof lbl, "%.*f", dec, f / 1e6);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, lbl, &ext);
    double lx = x - ext.width / 2.0;
    if (lx < 2) { lx = 2; }
    if (lx + ext.width > w - 2) { lx = w - 2 - ext.width; }
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.42);          /* legibility pill */
    cairo_rectangle(cr, lx - 3, ly - ext.height - 2, ext.width + 6, ext.height + 5);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.72, 0.82, 0.94, 0.95);
    cairo_move_to(cr, lx, ly);
    cairo_show_text(cr, lbl);
    cairo_set_source_rgba(cr, 0.6, 0.7, 0.85, 0.5);          /* tick under label */
    cairo_move_to(cr, x + 0.5, ly + 4); cairo_line_to(cr, x + 0.5, ly + 9);
    cairo_stroke(cr);
  }
}

/* Band-plan overlay: dashed amber lines at the amateur band edges in view. The
 * band name + recommended mode go in the VFO readout (see bp_mode_at), not on
 * the spectrum. Same freq→x frame as draw_freq_scale. */
static void draw_band_edges(cairo_t *cr, App *app, int w, int ph) {
  if (!app->show_band_edges || w < 2 || app->rate <= 0 || app->zoom <= 0.0) { return; }
  double span      = (double)app->rate / app->zoom;
  double hz_per_px = span / w;
  double pan_off   = pan_offset_hz(app);
  double left_hz   = (double)app->freq + pan_off - span / 2.0;
  double right_hz  = (double)app->freq + pan_off + span / 2.0;

  bp_edge_t edges[32];
  int n = bp_edges((bp_region_t)app->bp_region, bp_country_key(app->bp_country), edges, 32);

  /* Dashed amber band-edge markers (full height). */
  const double edge_dash[] = { 4.0, 3.0 };
  cairo_set_line_width(cr, 1.0);
  cairo_set_dash(cr, edge_dash, 2, 0);
  cairo_set_source_rgba(cr, 1.0, 0.66, 0.2, 0.45);
  for (int i = 0; i < n; i++) {
    for (int e = 0; e < 2; e++) {
      double f = e ? (double)edges[i].hi : (double)edges[i].lo;
      if (f < left_hz || f > right_hz) { continue; }
      double x = floor((f - left_hz) / hz_per_px) + 0.5;
      cairo_move_to(cr, x, 0);
      cairo_line_to(cr, x, ph);
      cairo_stroke(cr);
    }
  }
  cairo_set_dash(cr, NULL, 0, 0);
}

/* DX-spot overlay (F6d-2e): callsign labels in up to three stacked rows under
 * the frequency ruler + a tick down toward the signal, coloured by the
 * client's ARGB. Spots expire after SPOT_TTL (SDC re-announces live ones);
 * each drawn label remembers its hit box for click-to-tune. Same freq→x
 * frame as draw_band_edges. */
#define SPOT_TTL_US (10ll * 60 * 1000000)
static int spot_cmp_hz(const void *a, const void *b) {
  long long d = ((const struct spot *)a)->hz - ((const struct spot *)b)->hz;
  return d < 0 ? -1 : (d > 0 ? 1 : 0);
}
static void draw_spots(cairo_t *cr, App *app, int w, int ph) {
  if (!app->show_spots || app->nspots <= 0 || w < 2 || app->rate <= 0 || app->zoom <= 0.0) { return; }
  gint64 now = g_get_monotonic_time();
  int n = 0;                                   /* prune expired in place */
  for (int i = 0; i < app->nspots; i++) {
    if (now - app->spots[i].ts <= SPOT_TTL_US) {
      if (n != i) { app->spots[n] = app->spots[i]; }
      n++;
    }
  }
  app->nspots = n;
  if (n <= 0) { return; }
  qsort(app->spots, (size_t)n, sizeof(app->spots[0]), spot_cmp_hz);   /* left→right packing */

  double span      = (double)app->rate / app->zoom;
  double hz_per_px = span / w;
  double pan_off   = pan_offset_hz(app);
  double left_hz   = (double)app->freq + pan_off - span / 2.0;
  double right_hz  = left_hz + span;
  double rowend[3] = { -1e9, -1e9, -1e9 };
  const double y0 = 32.0, rh = 17.0;           /* label rows under the ruler */
  cairo_select_font_face(cr, FONT_MONO, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13.0);
  cairo_set_line_width(cr, 1.0);
  for (int i = 0; i < n; i++) {
    struct spot *s = &app->spots[i];
    s->hx0 = s->hx1 = 0.0;
    if ((double)s->hz < left_hz || (double)s->hz > right_hz) { continue; }
    double x = floor(((double)s->hz - left_hz) / hz_per_px) + 0.5;
    double r = ((s->argb >> 16) & 0xffu) / 255.0;
    double g = ((s->argb >>  8) & 0xffu) / 255.0;
    double b = ( s->argb        & 0xffu) / 255.0;
    if (r + g + b < 0.25) { r = 1.0; g = 0.85; b = 0.3; }  /* unset/black → amber */
    cairo_text_extents_t te;
    cairo_text_extents(cr, s->call, &te);
    int row = -1;
    for (int k = 0; k < 3; k++) {
      if (x - 2.0 > rowend[k]) { row = k; break; }
    }
    double ly = y0 + (row < 0 ? 0 : row) * rh;
    if (row >= 0) {                            /* label fits in a free row */
      rowend[row] = x + te.width + 8.0;
      cairo_set_source_rgba(cr, r, g, b, 0.95);
      cairo_move_to(cr, x + 3.0, ly);
      cairo_show_text(cr, s->call);
      s->hx0 = x - 3.0; s->hx1 = x + te.width + 6.0;
      s->hy0 = ly - 13.0; s->hy1 = ly + 4.0;
    }
    cairo_set_source_rgba(cr, r, g, b, 0.5);   /* tick even when the label didn't fit */
    cairo_move_to(cr, x, row >= 0 ? ly + 3.0 : y0);
    cairo_line_to(cr, x, ph * 0.45);
    cairo_stroke(cr);
  }
}

/* Top-right meter geometry — shared by the RX S-meter and the TX power meter so
 * the two read as one family. METER_BY aligns the bar with the frequency readout
 * (its tick labels line up with the big number's top); METER_RM keeps it off the
 * right edge. */
#define METER_BW 384.0
#define METER_BH 24.0
#define METER_BY 54.0
#define METER_RM 28.0   /* right margin */

/* Graphical S-meter, top-right of the panadapter. S1..S9 (6 dB/unit, S9 = -73
 * dBm on HF) then +dB over S9. dBm comes from the WDSP meter (radio) or the
 * server (network). Fill is coloured by the active palette (like spectrum/wf). */
static void draw_s_meter(cairo_t *cr, App *app, int w) {
  const double DBM_MIN = -121.0, DBM_S9 = -73.0, DBM_MAX = -13.0;   /* S1 … S9+60 */
  double dbm = app->frame.s_dbm;
  if (dbm < DBM_MIN) { dbm = DBM_MIN; }
  double span = DBM_MAX - DBM_MIN;
  double bw = METER_BW, bh = METER_BH, bx = w - bw - METER_RM, by = METER_BY;
  double fillw = (dbm - DBM_MIN) / span * bw;
  if (fillw > bw) { fillw = bw; }
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);           /* track */
  cairo_rectangle(cr, bx, by, bw, bh); cairo_fill(cr);
  /* Fill coloured by the active palette (same scheme as the spectrum + waterfall):
   * a horizontal gradient over the whole bar, painted up to the current level. */
  /* The monochrome palettes have a near-black low end, too dark to read as a
   * meter fill, so lift them toward white; the colour palettes read fine as-is. */
  const char *pname = waterfall_palette_name(app->palette);
  double k = (pname && g_str_has_prefix(pname, "Mono")) ? 0.20 : 0.0;
  cairo_pattern_t *grad = cairo_pattern_create_linear(bx, 0, bx + bw, 0);
  for (int s = 0; s <= 12; s++) {
    double t = s / 12.0, r, g, b;
    waterfall_palette_rgb(t, &r, &g, &b);
    r += (1.0 - r) * k; g += (1.0 - g) * k; b += (1.0 - b) * k;
    cairo_pattern_add_color_stop_rgba(grad, t, r, g, b, 0.92);
  }
  cairo_save(cr);
  cairo_rectangle(cr, bx, by, fillw, bh); cairo_clip(cr);
  cairo_set_source(cr, grad);
  cairo_rectangle(cr, bx, by, bw, bh); cairo_fill(cr);
  cairo_restore(cr);
  cairo_pattern_destroy(grad);

  cairo_select_font_face(cr, FONT_MONO, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 15.0);
  const struct { double dbm; const char *l; } mk[] = {
    {-121,"1"},{-109,"3"},{-97,"5"},{-85,"7"},{-73,"9"},{-53,"+20"},{-33,"+40"},{-13,"+60"},
  };
  for (int i = 0; i < 8; i++) {
    double mx = bx + (mk[i].dbm - DBM_MIN) / span * bw;
    cairo_set_source_rgba(cr, 0.7, 0.75, 0.85, 0.6);
    cairo_move_to(cr, mx + 0.5, by - 4); cairo_line_to(cr, mx + 0.5, by); cairo_stroke(cr);
    cairo_text_extents_t ex; cairo_text_extents(cr, mk[i].l, &ex);
    cairo_move_to(cr, mx - ex.width / 2, by - 7); cairo_show_text(cr, mk[i].l);
  }

  char lbl[32];
  double d = app->frame.s_dbm;
  if (d <= DBM_S9) {
    int s = (int)lround((d + 127.0) / 6.0); if (s < 1) { s = 1; }
    snprintf(lbl, sizeof lbl, "S%d   %.0f dBm", s, d);
  } else {
    snprintf(lbl, sizeof lbl, "S9+%d   %.0f dBm", (int)(lround((d - DBM_S9) / 10.0) * 10), d);
  }
  cairo_set_font_size(cr, 19.0);
  cairo_text_extents_t ex; cairo_text_extents(cr, lbl, &ex);
  cairo_set_source_rgba(cr, 0.82, 0.92, 0.78, 0.95);
  cairo_move_to(cr, bx + bw - ex.width, by + bh + 22); cairo_show_text(cr, lbl);
}

/* TX level meter — top-right of the TX panadapter, in the SAME geometry as the RX
 * S-meter so they read as one family. Two stacked bars: Mic input peak (dBFS,
 * -40..0, filled with the ACTIVE PALETTE like the S-meter, with a -6..0 TARGET
 * zone) and ALC gain reduction (dB, amber, 0..-20). WDSP TX meters, valid only
 * while keyed.
 *
 * The -6..0 dBFS zone is where voice PEAKS belong, not a danger zone: the TX
 * chain has no makeup gain with PROC off (the ALC only attenuates), so full PEP
 * needs mic peaks at the top of this bar with the ALC just starting to work. */
static void draw_tx_level_meter(cairo_t *cr, App *app, int w, const tx_run_status *ts) {
  const double MIC_MIN = -40.0, MIC_MAX = 0.0, MIC_TGT = -6.0, ALC_FS = 20.0;
  const double LVL_FS = 8.0;   /* leveler makeup full scale (WDSP top = 8 dB) */
  const double h_mic = 14.0, gap = 3.0, h_lvl = 7.0, h_alc = 7.0;
  double bw = METER_BW, bx = w - bw - METER_RM, by = METER_BY;

  /* Mic input peak (dBFS): palette gradient fill, exactly like draw_s_meter, so the
   * drive bar tracks the colour scheme (Richard's ask). */
  double mic = ts->mic_pk;
  if (mic < MIC_MIN) { mic = MIC_MIN; } else if (mic > MIC_MAX) { mic = MIC_MAX; }
  double micfill = (mic - MIC_MIN) / (MIC_MAX - MIC_MIN) * bw;
  double zx = bx + (MIC_TGT - MIC_MIN) / (MIC_MAX - MIC_MIN) * bw;
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
  cairo_rectangle(cr, bx, by, bw, h_mic); cairo_fill(cr);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.13);   /* target zone: aim peaks here */
  cairo_rectangle(cr, zx, by, bx + bw - zx, h_mic); cairo_fill(cr);
  const char *pname = waterfall_palette_name(app->palette);   /* lift the near-black Mono low end */
  double kk = (pname && g_str_has_prefix(pname, "Mono")) ? 0.20 : 0.0;
  cairo_pattern_t *grad = cairo_pattern_create_linear(bx, 0, bx + bw, 0);
  for (int s = 0; s <= 12; s++) {
    double t = s / 12.0, r, g, b;
    waterfall_palette_rgb(t, &r, &g, &b);
    r += (1.0 - r) * kk; g += (1.0 - g) * kk; b += (1.0 - b) * kk;
    cairo_pattern_add_color_stop_rgba(grad, t, r, g, b, 0.92);
  }
  cairo_save(cr);
  cairo_rectangle(cr, bx, by, micfill, h_mic); cairo_clip(cr);
  cairo_set_source(cr, grad);
  cairo_rectangle(cr, bx, by, bw, h_mic); cairo_fill(cr);
  cairo_restore(cr);
  cairo_pattern_destroy(grad);
  cairo_set_source_rgba(cr, 0.55, 0.95, 0.55, 0.75);          /* target-zone edge (-6 dB) */
  cairo_rectangle(cr, zx, by, 1.0, h_mic); cairo_fill(cr);

  /* Noise-gate overlay on the Mic bar: a blue threshold tick, and when the gate
   * is CLOSED (mic below threshold → mic dropped 20 dB) a blue shade over the
   * sub-threshold region — the operator sees exactly when/where the gate bites. */
  int gate_closed = 0;
  if (app->tx_gate) {
    double gx = bx + (app->tx_gate_db - MIC_MIN) / (MIC_MAX - MIC_MIN) * bw;
    if (gx < bx) { gx = bx; } else if (gx > bx + bw) { gx = bx + bw; }
    gate_closed = ts->mic_pk < app->tx_gate_db;
    if (gate_closed) {
      cairo_set_source_rgba(cr, 0.30, 0.55, 1.0, 0.30);
      cairo_rectangle(cr, bx, by, gx - bx, h_mic); cairo_fill(cr);
    }
    cairo_set_source_rgba(cr, 0.35, 0.65, 1.0, 0.95);   /* gate threshold tick */
    cairo_rectangle(cr, gx, by, 1.5, h_mic); cairo_fill(cr);
  }

  /* Leveler makeup gain (dB, green) — what PROC is ADDING right now (0..+8).
   * Riding high in speech gaps = the noise pump the gate is there to stop. */
  double ly = by + h_mic + gap;
  double lvl = ts->lvlr_gain;
  if (lvl < 0.0) { lvl = 0.0; } else if (lvl > LVL_FS) { lvl = LVL_FS; }
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
  cairo_rectangle(cr, bx, ly, bw, h_lvl); cairo_fill(cr);
  cairo_set_source_rgba(cr, 0.45, 0.85, 0.45, 0.9);
  cairo_rectangle(cr, bx, ly, lvl / LVL_FS * bw, h_lvl); cairo_fill(cr);

  /* ALC gain reduction (dB, amber). alc_gain is ≤ 0; show its magnitude. */
  double ay = ly + h_lvl + gap;
  double alc = -ts->alc_gain;
  if (alc < 0.0) { alc = 0.0; } else if (alc > ALC_FS) { alc = ALC_FS; }
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
  cairo_rectangle(cr, bx, ay, bw, h_alc); cairo_fill(cr);
  cairo_set_source_rgba(cr, 1.0, 0.72, 0.0, 0.9);
  cairo_rectangle(cr, bx, ay, alc / ALC_FS * bw, h_alc); cairo_fill(cr);

  /* Left tags + a numeric readout under the bars (like the S-meter's dBm line). */
  cairo_select_font_face(cr, FONT_MONO, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_source_rgba(cr, 0.72, 0.80, 0.92, 0.8);
  cairo_set_font_size(cr, 11.0);
  cairo_move_to(cr, bx - 32, by + h_mic - 3); cairo_show_text(cr, "Mic");
  cairo_move_to(cr, bx - 32, ly + h_lvl);     cairo_show_text(cr, "Lev");
  cairo_move_to(cr, bx - 32, ay + h_alc);     cairo_show_text(cr, "ALC");
  char lbl[64];
  if (ts->mic_pk <= MIC_MIN) {
    snprintf(lbl, sizeof lbl, "Mic  --   Lev +%.0f   ALC %.0f dB%s",
             ts->lvlr_gain, ts->alc_gain, gate_closed ? "  · GATE" : "");
  } else {
    snprintf(lbl, sizeof lbl, "Mic %+.0f   Lev +%.0f   ALC %.0f dB%s",
             ts->mic_pk, ts->lvlr_gain, ts->alc_gain, gate_closed ? "  · GATE" : "");
  }
  cairo_set_font_size(cr, 15.0);
  cairo_text_extents_t ex; cairo_text_extents(cr, lbl, &ex);
  cairo_set_source_rgba(cr, 0.95, 0.86, 0.62, 0.92);
  cairo_move_to(cr, bx + bw - ex.width, ay + h_alc + 20); cairo_show_text(cr, lbl);
}

/* TX panadapter (F6a): the transmitted spectrum, panadapter (top) + waterfall
 * (bottom), like the RX view — with big red power/SWR numbers and a level meter.
 * The frequency axis MATCHES the RX one (span = rate/zoom), capped at the TX DUC
 * IQ bandwidth (192 kHz) since that's all the TX stream carries. */
#define TX_IQ_BW 192000.0      /* TX DUC IQ bandwidth = widest possible TX span (Hz) */
#define MIC_GAIN_MIN (-12.0)   /* TX mic-gain slider range, dB — piHPSDR sliders.c:643 */
#define MIC_GAIN_MAX  50.0
#define COMP_DB_MIN   0.0      /* PROC compression range, dB (0 = auto-leveler only) */
#define COMP_DB_MAX   20.0
#define GATE_DB_MIN  (-60.0)   /* mic noise-gate threshold range, dBFS (post-gain) */
#define GATE_DB_MAX  (-10.0)
#define GATE_DB_DFLT (-35.0)
#define MON_DB_MIN   (-40.0)   /* TX monitor level range, dB */
#define MON_DB_MAX     0.0
#define MON_DB_DFLT  (-15.0)
#define CW_WPM_DFLT    20      /* CW keyer (F6d-1c): speed, sidetone, break-in hang */
#define CW_PITCH_DFLT  700
#define CW_ST_DB_MIN (-40.0)   /* sidetone level, ABSOLUTE output dBFS (bypasses */
#define CW_ST_DB_MAX    0.0    /* the monitor gain); −20 ≈ piHPSDR vol 50/127    */
#define CW_ST_DB_DFLT (-20.0)
#define CW_HANG_DFLT   250
#define TXF_LO_MIN    20.0     /* TX audio filter edges, Hz (150/2850 default;   */
#define TXF_LO_MAX   500.0     /* high edge up to 6 kHz covers eSSB widths)      */
#define TXF_HI_MIN  1500.0
#define TXF_HI_MAX  6000.0
#define TXF_LO_DFLT  150.0
#define TXF_HI_DFLT 2850.0

/* TX display span (Hz): the RX span (rate/zoom), capped at what the TX IQ holds. */
static double tx_span_hz(App *app) {
  double z = app->zoom > 0.0 ? app->zoom : 1.0;
  double s = (double)app->rate / z;
  return s < TX_IQ_BW ? s : TX_IQ_BW;
}

static void draw_tx(cairo_t *cr, int w, int h, App *app) {
  int n = app->pixels;
  if (app->tx_ema_w != n) {
    panadapter_draw(cr, w, h, NULL, NULL, 0, 1, "TX — keying…", NULL, 0.5);
    return;
  }
  tx_run_status ts; tx_run_get_status(&ts);
  int ph = (int)(h * PANADAPTER_FRACTION);
  if (ph < 1) { ph = 1; }
  /* Fixed, operator-draggable dB window (F6c-3, like RX) — no per-frame auto-range,
   * so the scale no longer jumps. tick_tx one-shot-fits it on the first TX frame. */
  double high = app->tx_pan_high, low = app->tx_pan_low;
  panadapter_set_range(high, low);
  panadapter_set_grid(app->show_db_grid, app->show_db_scale);

  /* Panadapter (top): transmitted spectrum, 24 kHz span, carrier centred. Suppress
   * the built-in white VFO readout — we draw a red power/SWR one instead. */
  cairo_save(cr);
  cairo_rectangle(cr, 0, 0, w, ph);
  cairo_clip(cr);
  panadapter_set_readout(0);
  panadapter_draw(cr, w, ph, &app->tx_frame, app->tx_ema, low, high - low, NULL, NULL, 0.5);
  panadapter_set_readout(1);

  /* Frequency ruler at the TOP (like the RX ruler), now zoom-aware: the span
   * follows app->zoom (base 24 kHz / zoom), with a nice-number kHz tick step. */
  double tx_span = tx_span_hz(app);
  double half_khz = tx_span / 2000.0;
  double pxhz = (double)w / tx_span, cx = w / 2.0;
  /* nice tick step (kHz): ~6 divisions over the span, snapped to 1/2/5·10^k. */
  double raw  = (tx_span / 1000.0) / 6.0;
  double pten = pow(10.0, floor(log10(raw)));
  double frac = raw / pten;
  double step = (frac < 1.5 ? 1.0 : frac < 3.5 ? 2.0 : frac < 7.5 ? 5.0 : 10.0) * pten;
  cairo_select_font_face(cr, FONT_MONO, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 10.0);
  const double ly = 13.0;
  cairo_set_line_width(cr, 1.0);
  for (double khz = -floor(half_khz / step) * step; khz <= half_khz + 1e-6; khz += step) {
    double x = floor(cx + khz * 1000.0 * pxhz) + 0.5;
    if (app->show_freq_grid && fabs(khz) > 1e-6) {
      cairo_set_source_rgba(cr, 0.5, 0.6, 0.7, 0.11);        /* neutral, like the RX ruler */
      cairo_move_to(cr, x, 0); cairo_line_to(cr, x, ph); cairo_stroke(cr);
    }
    char lbl[16];
    if (fabs(khz) < 1e-6) { snprintf(lbl, sizeof lbl, "0"); }
    else { snprintf(lbl, sizeof lbl, "%+g", khz); }
    cairo_text_extents_t ex; cairo_text_extents(cr, lbl, &ex);
    double lx = x - ex.width / 2.0;
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.42);
    cairo_rectangle(cr, lx - 3, ly - ex.height - 2, ex.width + 6, ex.height + 5);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.72, 0.82, 0.94, 0.95);
    cairo_move_to(cr, lx, ly); cairo_show_text(cr, lbl);
  }
  cairo_set_source_rgba(cr, 0.72, 0.82, 0.94, 0.7);
  cairo_move_to(cr, cx + half_khz * 1000.0 * pxhz + 8, ly); cairo_show_text(cr, "kHz");
  cairo_restore(cr);

  /* TX waterfall (bottom): transmitted-spectrum history. */
  if (app->tx_wf) { waterfall_draw(app->tx_wf, cr, 0, ph, w, h - ph); }
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
  cairo_rectangle(cr, 0, ph - 1, w, 2);
  cairo_fill(cr);

  /* Big red power/SWR numbers, top-left — the RX frequency readout's TX sibling.
   * No sub-line: power + SWR appearing is itself the "we're transmitting" cue.
   *
   * DISPLAY smoothing only — the SWR protection (tx_gate) keeps acting on the
   * raw fast values. Power shows ts.fwd_pep_w — true PEP, tracked by the engine
   * from the raw 1 kHz coupler packets with piHPSDR's meter ballistics (instant
   * attack, ~0.5 s decay half-life); the EMA-averaged ts.fwd_w under-reads
   * voice by ~6 dB (measured 4× vs an external PEP wattmeter, 2026-07-10).
   * On a steady carrier both are equal. SWR only updates while real power
   * flows (> 0.5 W) and is lightly smoothed — between syllables the coupler
   * reads ~nothing and the computed SWR is meaningless jitter, so hold the
   * last valid reading instead. */
  if (ts.fwd_w > 0.5) {
    app->tx_swr_disp = app->tx_swr_disp > 0.0
                       ? app->tx_swr_disp + 0.3 * (ts.swr - app->tx_swr_disp)
                       : ts.swr;
  } else if (app->tx_swr_disp <= 0.0) {
    app->tx_swr_disp = ts.swr;      /* nothing held yet (dry key) → show live */
  }
  char big[48];
  snprintf(big, sizeof big, "%.0f W   ·   SWR %.1f", ts.fwd_pep_w, app->tx_swr_disp);
  cairo_select_font_face(cr, FONT_MONO, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 32.0);
  cairo_set_source_rgba(cr, 1.0, 0.34, 0.28, 0.98);
  cairo_move_to(cr, 44, 60); cairo_show_text(cr, big);

  /* High-SWR flag (amber) — lights whenever SWR >= alarm, in TUNE *and* MOX. In
   * MOX this precedes the two-poll trip; in TUNE it is warn-only (no trip) so the
   * operator can watch SWR while tuning an ATU / dialling drive for calibration. */
  if (ts.high_swr) {
    cairo_set_font_size(cr, 18.0);
    cairo_set_source_rgba(cr, 1.0, 0.72, 0.0, 0.98);
    cairo_move_to(cr, 44, 88); cairo_show_text(cr, "⚠ HIGH SWR");
  }

  /* Level meter top-right, where the RX S-meter sits — mic peak + ALC. */
  draw_tx_level_meter(cr, app, w, &ts);
}

static void draw_cb(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data) {
  (void)area;
  App *app = (App *)data;

  panadapter_set_range(app->pan_high, app->pan_low);   /* grab-to-move dB window */
  panadapter_set_grid(app->show_db_grid, app->show_db_scale);

  if (!app->connected) {
    const char *msg = app->radio_mode ? "No radio found on the LAN"
                                       : client_strerror(app->conn_err);
    char buf[160];
    snprintf(buf, sizeof(buf), "Not connected: %s", msg);
    panadapter_draw(cr, w, h, NULL, NULL, 0, 1, buf, NULL, 0.5);
    return;
  }
  if (!app->have_frame) {
    panadapter_draw(cr, w, h, NULL, NULL, 0, 1,
                    app->radio_mode ? "Radio up — calibrating…" : "Connected — waiting for spectrum…", NULL, 0.5);
    return;
  }

  /* While keyed, the whole area is the TX panadapter (no RX trace / waterfall). */
  if (app->tx_display) { draw_tx(cr, w, h, app); return; }

  int ph = (int)(h * PANADAPTER_FRACTION);
  if (ph < 1) ph = 1;

  double low, span;
  waterfall_range(app->wf, &low, &span);

  const float *smoothed = (app->ema_w == app->frame.width) ? app->ema : NULL;
  /* Band + recommended mode for the readout, e.g. "20m · USB" (band-plan on). */
  const char *bname = NULL;
  char bandinfo[48];
  if (app->radio_mode && app->show_band_edges) {
    const char *bn = bp_band_for_freq((bp_region_t)app->bp_region,
                                      bp_country_key(app->bp_country), app->freq, NULL, NULL);
    if (bn) {
      const char *md = bp_mode_at((bp_region_t)app->bp_region,
                                  bp_country_key(app->bp_country), app->freq);
      if (md) { snprintf(bandinfo, sizeof bandinfo, "%s · %s", bn, md); }
      else    { snprintf(bandinfo, sizeof bandinfo, "%s", bn); }
      bname = bandinfo;
    }
  }
  cairo_save(cr);
  cairo_rectangle(cr, 0, 0, w, ph);
  cairo_clip(cr);
  panadapter_draw(cr, w, ph, &app->frame, smoothed, low, span, NULL, bname, vfo_x(app, w) / w);
  if (app->radio_mode && (app->show_freq_grid || app->show_freq_scale)) {
    draw_freq_scale(cr, app, w, ph);
  }
  if (app->radio_mode) {
    draw_band_edges(cr, app, w, ph);
    if (!app->tx_display) { draw_spots(cr, app, w, ph); }  /* RX spectrum only */
  }
  /* Filter passband overlay (Model A: VFO = span centre). Just the fill + the
   * two edges here — the VFO centre is the amber line panadapter.c already draws
   * (no second white centre). Both edges share one colour+alpha (opacity slider)
   * and snap to pixel centres, so they render identically. */
  if (app->radio_mode && app->fhi > app->flo) {
    double op = app->filter_op / 100.0;
    double hz_per_px = (double)app->rate / app->zoom / w;
    double cx = vfo_x(app, w);
    double x0 = floor(cx + app->flo / hz_per_px) + 0.5;
    double x1 = floor(cx + app->fhi / hz_per_px) + 0.5;
    cairo_set_source_rgba(cr, 0.35, 0.75, 1.0, op * 0.22);   /* passband fill */
    cairo_rectangle(cr, x0, 0, x1 - x0, ph);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.55, 0.85, 1.0, op * 0.95);   /* both edges */
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x0, 0); cairo_line_to(cr, x0, ph);
    cairo_move_to(cr, x1, 0); cairo_line_to(cr, x1, ph);
    cairo_stroke(cr);
  }
  /* Select-mode filter cursor: the passband footprint (amber) at the pointer,
   * showing where a left-click would place the filter before it recenters. */
  if (app->select_mode && app->fhi > app->flo && app->ptr_x >= 0 && app->ptr_x <= w) {
    double op = app->filter_op / 100.0;                 /* same transparency as the filter */
    double hz_per_px = (double)app->rate / app->zoom / w;
    double gx0 = floor(app->ptr_x + app->flo / hz_per_px) + 0.5;
    double gx1 = floor(app->ptr_x + app->fhi / hz_per_px) + 0.5;
    double gxc = floor(app->ptr_x) + 0.5;
    cairo_set_source_rgba(cr, 1.0, 0.82, 0.28, op * 0.22);   /* ghost fill  */
    cairo_rectangle(cr, gx0, 0, gx1 - gx0, ph);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1.0, 0.82, 0.28, op * 0.95);   /* ghost edges + aim line */
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, gx0, 0); cairo_line_to(cr, gx0, ph);
    cairo_move_to(cr, gx1, 0); cairo_line_to(cr, gx1, ph);
    cairo_move_to(cr, gxc, 0); cairo_line_to(cr, gxc, ph);
    cairo_stroke(cr);
  }
  cairo_restore(cr);

  /* ADC-overload badge, top-left of the panadapter. Warns the input is
   * clipping → add attenuation. Held ADC_OVL_HOLD_US after the last clip. */
  gint64 now = g_get_monotonic_time();
  int ovl0 = now < app->ovl0_until;
  int ovl1 = now < app->ovl1_until;
  if (ovl0 || ovl1) {
    const char *txt = (ovl0 && ovl1) ? "ADC0+1 OVL" : ovl0 ? "ADC0 OVL" : "ADC1 OVL";
    cairo_select_font_face(cr, FONT_MONO, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12.0);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, txt, &ext);
    const double pad = 5.0, bx = 8.0, by = 28.0;   /* below the freq ruler strip */
    cairo_set_source_rgba(cr, 0.78, 0.06, 0.06, 0.85);   /* red badge */
    cairo_rectangle(cr, bx, by, ext.width + 2 * pad, ext.height + 2 * pad);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.96);
    cairo_move_to(cr, bx + pad - ext.x_bearing, by + pad - ext.y_bearing);
    cairo_show_text(cr, txt);
  }

  draw_s_meter(cr, app, w);

  waterfall_draw(app->wf, cr, 0, ph, w, h - ph);

  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
  cairo_rectangle(cr, 0, ph - 1, w, 2);
  cairo_fill(cr);

  /* Optionally carry the filter (both edges) + the VFO centre line down through
   * the waterfall, so signals line up with the passband over time. Toggleable. */
  if (app->radio_mode && app->show_filter_wf && app->fhi > app->flo) {
    double op = app->filter_op / 100.0;
    double hz_per_px = (double)app->rate / app->zoom / w;
    double cx = vfo_x(app, w);
    double x0 = floor(cx + app->flo / hz_per_px) + 0.5;
    double x1 = floor(cx + app->fhi / hz_per_px) + 0.5;
    double xc = floor(cx) + 0.5;
    cairo_set_source_rgba(cr, 0.35, 0.75, 1.0, op * 0.22);   /* fill — SAME as the spectrum */
    cairo_rectangle(cr, x0, ph, x1 - x0, h - ph);
    cairo_fill(cr);
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 1.0, 0.78, 0.25, 0.45);        /* amber VFO centre — matches panadapter.c */
    cairo_move_to(cr, xc, ph); cairo_line_to(cr, xc, h);
    cairo_stroke(cr);
    cairo_set_source_rgba(cr, 0.55, 0.85, 1.0, op * 0.95);   /* both edges — SAME as the spectrum */
    cairo_move_to(cr, x0, ph); cairo_line_to(cr, x0, h);
    cairo_move_to(cr, x1, ph); cairo_line_to(cr, x1, h);
    cairo_stroke(cr);
  }

  /* Select-mode filter cursor carried onto the waterfall too, but only when the
   * filter is shown there — same "Filter on waterfall" toggle governs both. */
  if (app->select_mode && app->show_filter_wf && app->fhi > app->flo &&
      app->ptr_x >= 0 && app->ptr_x <= w) {
    double op = app->filter_op / 100.0;                 /* same transparency as the filter */
    double hz_per_px = (double)app->rate / app->zoom / w;
    double gx0 = floor(app->ptr_x + app->flo / hz_per_px) + 0.5;
    double gx1 = floor(app->ptr_x + app->fhi / hz_per_px) + 0.5;
    double gxc = floor(app->ptr_x) + 0.5;
    cairo_set_source_rgba(cr, 1.0, 0.82, 0.28, op * 0.22);   /* ghost fill  */
    cairo_rectangle(cr, gx0, ph, gx1 - gx0, h - ph);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1.0, 0.82, 0.28, op * 0.95);   /* ghost edges + aim line */
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, gx0, ph); cairo_line_to(cr, gx0, h);
    cairo_move_to(cr, gx1, ph); cairo_line_to(cr, gx1, h);
    cairo_move_to(cr, gxc, ph); cairo_line_to(cr, gxc, h);
    cairo_stroke(cr);
  }
}

/* Network path: pull a decoded frame and fold it into the dBm EMA. */
static void tick_network(App *app, GtkWidget *widget) {
  if (!client_latest(app->client, &app->frame, &app->last_seq)) { return; }
  const ClientFrame *f = &app->frame;
  if (app->ema_w != f->width) {
    for (int i = 0; i < f->width; i++) { app->ema[i] = (float)f->dbm[i] - 200.0f; }
    app->ema_w = f->width;
  } else {
    for (int i = 0; i < f->width; i++) {
      app->ema[i] += EMA_FACTOR * ((float)f->dbm[i] - 200.0f - app->ema[i]);
    }
  }
  waterfall_push(app->wf, f->dbm, f->width);
  app->have_frame = 1;
  gtk_widget_queue_draw(widget);
}

/* Radio path: pull one analyzer frame, auto-level it into dBm, EMA it. The EMA
 * holds relative dB until the offset locks, then is shifted once into dBm; after
 * that the incoming relative-dB pixels are offset on the fly. */
static void tick_radio(App *app, GtkWidget *widget) {
  if (!analyzer_get_pixels(app->eng_raw, app->pixels)) { return; }
  const float *raw = app->eng_raw;
  int n = app->pixels;

  if (app->ema_w != n) {
    memcpy(app->ema, raw, n * sizeof(float));
    app->ema_w = n;
    app->cal_frames = 0;
  } else {
    /* Analyzer uses 1 Hz PSD norm → floor is zoom-invariant; no compensation. */
    double so = app->soffset_locked ? app->soffset : 0.0;
    float fs = ema_factor_ms(app->avg_spec_ms, app->fps);   /* spectrum averaging */
    for (int i = 0; i < n; i++) {
      app->ema[i] += fs * ((float)(raw[i] + so) - app->ema[i]);
    }
  }
  app->cal_frames++;

  if (!app->soffset_locked) {
    if (app->cal_frames < SETTLE_FRAMES) { return; }   /* still calibrating */
    /* Lock: measure the ~20th-percentile noise floor and shift EMA to dBm. */
    static float sorted[SPECTRUM_DATA_SIZE];
    memcpy(sorted, app->ema, n * sizeof(float));
    qsort(sorted, n, sizeof(float), cmp_float);
    double floor_db = sorted[(int)(n * 0.20)];
    const char *so = getenv("SDRFL_SOFFSET");
    app->soffset = (so && *so) ? strtod(so, NULL) : (TARGET_FLOOR - floor_db);
    for (int i = 0; i < n; i++) { app->ema[i] += (float)app->soffset; }
    app->soffset_locked = 1;
    printf("radio: display locked — soffset=%.1f, floor=%.1f dBm, %d px; rendering\n",
           app->soffset, floor_db + app->soffset, n);
    fflush(stdout);
  }

  /* SDRFL_DEBUG_LEVELS: once per second dump the RAW analyzer levels (no
   * soffset). Note our display norm is 1 Hz PSD (SetDisplaySampleRate=rate);
   * piHPSDR norms per pixel width (SetDisplaySampleRate=width*zoom), so
   * piHPSDR-equivalent dBm = raw + 10*log10(rate/(zoom*n)) ... *n/n_px_pihpsdr
   * — do the conversion offline, here we just log the raw numbers. */
  static int dbg = -1;
  if (dbg < 0) { dbg = getenv("SDRFL_DEBUG_LEVELS") != NULL; }
  if (dbg) {
    static gint64 dbg_next = 0;
    gint64 now = g_get_monotonic_time();
    if (now >= dbg_next) {
      dbg_next = now + G_USEC_PER_SEC;
      static float dsort[SPECTRUM_DATA_SIZE];
      memcpy(dsort, raw, n * sizeof(float));
      qsort(dsort, n, sizeof(float), cmp_float);
      int   pk_i = 0;
      for (int i = 1; i < n; i++) { if (raw[i] > raw[pk_i]) { pk_i = i; } }
      double hz_per_px = (double)app->rate / app->zoom / n;
      printf("spectrum: raw peak=%.1f @%+.1f kHz | floor20=%.1f | median=%.1f "
             "(n=%d, zoom=%g, rate=%d, soffset=%+.1f, pixnorm_delta=%+.1f dB)\n",
             raw[pk_i], (pk_i - n / 2) * hz_per_px / 1000.0,
             dsort[(int)(n * 0.20)], dsort[n / 2],
             n, app->zoom, app->rate, app->soffset,
             10.0 * log10((double)app->rate / (app->zoom * (double)n)));
      fflush(stdout);
    }
  }

  /* Waterfall has its OWN averaging (separate time constant), independent of the
   * trace — on the raw analyzer dBm. */
  if (app->wf_ema_w != n) {
    memcpy(app->wf_ema, raw, n * sizeof(float));
    app->wf_ema_w = n;
  } else {
    float fw = ema_factor_ms(app->avg_wf_ms, app->fps);
    for (int i = 0; i < n; i++) { app->wf_ema[i] += fw * (raw[i] - app->wf_ema[i]); }
  }

  /* EMA now in dBm. Build metadata + waterfall bytes. */
  app->frame.width      = n;
  app->frame.vfo_a_freq = app->freq;
  double peak = app->ema[0];
  static uint8_t bytes[SPECTRUM_DATA_SIZE];
  for (int i = 0; i < n; i++) {
    if (app->ema[i] > peak) { peak = app->ema[i]; }
    double b = (double)app->wf_ema[i] + app->soffset + 200.0;
    bytes[i] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
  }
  app->frame.s_dbm = app->audio_ok ? demod_s_meter() : peak;  /* real WDSP S-meter */
  waterfall_push(app->wf, bytes, n);
  app->have_frame = 1;
  gtk_widget_queue_draw(widget);
}

static void tx_pan_autofit(App *app);   /* fwd: one-shot TX dB-window fit (below) */

/* TX path: pull one TX-analyzer frame (spectrum of what we transmit) and fold it
 * into the TX trace EMA. Draws the TX panadapter over the full area (no RX, no
 * waterfall) while keyed — piHPSDR non-duplex behaviour. */
#define TX_TRACE_CW_MS 12   /* CW TX trace: fast so it tracks keying (SSB keeps the RX avg) */
static void tick_tx(App *app, GtkWidget *widget) {
  int n = app->pixels;
  if (tx_run_get_pixels(app->tx_raw, n)) {
    /* INTERIM: CW needs a fast trace, SSB a smooth one — mode-split for now; a
     * proper tunable per-mode TX averaging is on the TODO list. */
    int cw = (app->mode == DEMOD_CWL || app->mode == DEMOD_CWU);
    float fs = ema_factor_ms(cw ? TX_TRACE_CW_MS : app->avg_spec_ms, app->fps);
    if (app->tx_ema_w != n) {
      memcpy(app->tx_ema, app->tx_raw, n * sizeof(float));
      app->tx_ema_w = n;
    } else {
      for (int i = 0; i < n; i++) { app->tx_ema[i] += fs * (app->tx_raw[i] - app->tx_ema[i]); }
    }
    /* First TX frame of a fresh window: one-shot-fit the dB scale, then leave it
     * fixed and draggable (unless the operator already has a saved TX window). */
    if (!app->tx_pan_init) { tx_pan_autofit(app); app->tx_pan_init = 1; }
    /* Feed the TX waterfall (its own auto-range colours the transmitted spectrum;
     * TX levels aren't dBm-calibrated, so map byte = dB + 200 like the RX path). */
    if (app->tx_wf) {
      static uint8_t bytes[SPECTRUM_DATA_SIZE];
      for (int i = 0; i < n; i++) {
        double b = (double)app->tx_ema[i] + 200.0;
        bytes[i] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
      }
      waterfall_push(app->tx_wf, bytes, n);
    }
  }
  app->tx_frame.width      = n;
  app->tx_frame.vfo_a_freq = app->freq;
  gtk_widget_queue_draw(widget);
}

static void update_span_label(App *app);   /* fwd: zoom slider updates it via tick */
static void update_volt_label(App *app);   /* fwd: defined with the footer controls */
static void on_zoom_changed(GtkRange *r, gpointer data);   /* fwd: footer zoom slider */

/* TX readout (F6a): ONLY a refusal/trip reason, flashed red for a few seconds —
 * the live power/SWR live big on the TX panadapter + the power meter, so this no
 * longer duplicates them (and shows nothing when idle). Called each tick. */
static void update_tx_label(App *app, const tx_run_status *ts, gint64 now) {
  (void)ts;
  if (!app->tx_label) { return; }
  if (app->tx_reason[0] && now < app->tx_reason_until) {
    char m[160];
    snprintf(m, sizeof m, "<span foreground='#f2413d'><b>⚠ %s</b></span>", app->tx_reason);
    gtk_label_set_markup(GTK_LABEL(app->tx_label), m);
  } else {
    gtk_label_set_text(GTK_LABEL(app->tx_label), "");
  }
}

static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
  (void)clock;
  App *app = (App *)data;
  if (app->radio_mode && app->zoom_dirty) {   /* coalesce slider events: ≤1 reconfig/frame */
    analyzer_set_zoom(app->pending_zoom);
    app->zoom = app->pending_zoom;
    app->zoom_dirty = 0;
    if (app->pan != 0.0) { app->pan = 0.0; analyzer_set_pan(0.0); }   /* zoom change recentres */
    if (app->tx_ready) { tx_run_set_span(tx_span_hz(app)); }  /* TX span follows the RX axis */
    update_span_label(app);
  }
  if (app->connected) {
    if (app->radio_mode) {
      band_apply(app);   /* swap in this band's remembered dB levels on QSY */
      if (app->cur_band >= 0) { app->band_freq[app->cur_band] = app->freq; }  /* track freq */
      /* Poll HP-status telemetry once per frame (read-and-clear → single
       * consumer). Latch the overload badges with a hold; raw analog words
       * are parsed but not shown until a live voltage calibration exists. */
      p2_telemetry t;
      p2_get_telemetry(&t);
      gint64 now = g_get_monotonic_time();
      /* Suppress the ADC-overload badge across the TX→RX transition: dropping the
       * step attenuators from 31 dB back to 0 lets the relay-crosstalk tail clip
       * the ADC for a moment — a known transient, not a real RX overload. */
      int ovl_suppress = app->tx_display || (app->tx_settle_until && now < app->tx_settle_until);
      if (!ovl_suppress) {
        if (t.adc0_overload) { app->ovl0_until = now + ADC_OVL_HOLD_US; }
        if (t.adc1_overload) { app->ovl1_until = now + ADC_OVL_HOLD_US; }
      }
      app->tlm_valid = t.valid;
      if (t.valid && t.raw_adc1 > 0) {
        static double kcal = -1.0;
        if (kcal < 0.0) {
          const char *e = getenv("SDRFL_VOLT_CAL");
          kcal = (e && *e) ? atof(e) : SUPPLY_V_PER_COUNT;
        }
        double v = t.raw_adc1 * kcal;
        app->supply_v = (app->supply_v > 0.0)
                        ? app->supply_v + SUPPLY_V_EMA * (v - app->supply_v) : v;
        update_volt_label(app);
      }
      /* TX (F6a): keep the runtime's TX frequency on the VFO and reflect status.
       * If the gate refused or a protection latch tripped while TUNE is held,
       * pop the button back (which fires the handler → unkey) and flash why. */
      if (app->tx_ready) {
        tx_run_set_freq(app->freq);
        tx_run_status ts; tx_run_get_status(&ts);
        if (app->tune_btn && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->tune_btn))
            && (ts.tripped || !ts.allowed)) {
          g_strlcpy(app->tx_reason, ts.reason[0] ? ts.reason : "TX refused", sizeof app->tx_reason);
          app->tx_reason_until = now + 4 * G_USEC_PER_SEC;
          gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->tune_btn), FALSE);
        }
        /* On the RX↔TX transition. Key up: fade RX audio out, start a fresh TX
         * trace. Unkey: keep the audio muted through the AGC-recovery window
         * (TX_SETTLE_MS) and only then fade it back in — otherwise the wound-up
         * AGC pops; also silence the demod input briefly for the literal T/R tail. */
        if (ts.keyed != app->tx_keyed_shown) {
          if (ts.keyed) {
            if (app->audio_ok) { demod_set_mute(1); }
            app->tx_ema_w = 0;
            app->tx_settle_until = 0;
            app->tx_swr_disp = 0.0;                    /* fresh held SWR per over */
          } else {
            if (app->audio_ok) { g_atomic_int_set(&g_rx_silence, app->rate * RX_SILENCE_MS / 1000); }
            app->tx_settle_until = now + (gint64)TX_SETTLE_MS * 1000;
          }
          app->tx_keyed_shown = ts.keyed;
        }
        if (app->tx_settle_until && now >= app->tx_settle_until) {
          if (app->audio_ok) { demod_set_mute(0); }   /* AGC settled → fade back in */
          app->tx_settle_until = 0;
        }
        app->tx_display = ts.keyed;
        update_tx_label(app, &ts, now);
      }
      if (app->tx_display) {
        tick_tx(app, widget);       /* transmitted spectrum, full area */
      } else {
        tick_radio(app, widget);
        if (app->auto_level) {
          /* Track the noise floor on the dB axis, reusing the waterfall's already-
           * smoothed floor. Keep the current range; place the floor AUTO_FLOOR_FRAC
           * up from the bottom. */
          double wl, ws; waterfall_range(app->wf, &wl, &ws); (void)ws;
          double R = app->pan_high - app->pan_low;
          if (R < PAN_RANGE_MIN) { R = PAN_RANGE_MIN; }
          if (R > PAN_RANGE_MAX) { R = PAN_RANGE_MAX; }
          double lo = wl - AUTO_FLOOR_FRAC * R;
          if (lo < PAN_DBM_FLOOR)     { lo = PAN_DBM_FLOOR; }
          if (lo > PAN_DBM_CEIL - R)  { lo = PAN_DBM_CEIL - R; }
          app->pan_low = lo; app->pan_high = lo + R;
        }
      }
    } else {
      tick_network(app, widget);
    }
  }
  return G_SOURCE_CONTINUE;
}

/* ---- persistent state (see settings.h) ----------------------------------- */

static void app_to_settings(const App *app, Settings *s) {
  g_strlcpy(s->ip, app->radio_ip, sizeof(s->ip));
  s->freq   = app->freq;
  s->rate   = app->rate;
  s->mode   = app->mode;
  s->volume  = app->volume;
  s->gain    = app->gain;
  s->fps     = app->fps;
  s->latency = app->latency;
  s->audio_rate = app->audio_rate;
  g_strlcpy(s->audio_device, app->audio_device, sizeof s->audio_device);
  g_strlcpy(s->mic_device, app->mic_device, sizeof s->mic_device);
  s->step    = (int)app->tune_step;
  s->zoom    = app->zoom;
  s->atten   = app->atten;
  s->agc     = app->agc;
  s->agc_gain = app->agc_gain;
  s->filter  = app->filter_idx;
  s->nr      = app->nr;
  s->nb      = app->nb;
  s->anf     = app->anf;
  s->binaural = app->binaural;
  s->tx_pa    = app->tx_pa_enabled;
  s->tx_ant   = app->tx_antenna;
  s->tx_drive = app->tx_drive_w;
  s->tx_tune  = app->tx_tune_w;
  s->tx_swr   = app->tx_swr_alarm;
  s->mic_gain = app->tx_mic_gain;
  s->tx_comp    = app->tx_comp;
  s->tx_comp_db = app->tx_comp_db;
  s->tx_gate    = app->tx_gate;
  s->tx_gate_db = app->tx_gate_db;
  s->tx_mon     = app->tx_mon;
  s->tx_mon_db  = app->tx_mon_db;
  s->tx_flo     = app->tx_flo;
  s->tx_fhi     = app->tx_fhi;
  s->cw_wpm     = app->cw_wpm;
  s->cw_pitch   = app->cw_pitch;
  s->cw_st_db   = app->cw_st_db;
  s->cw_hang    = app->cw_hang;
  s->tci_enable = app->tci_enable;
  s->tci_port   = app->tci_port;
  s->tci_iq_rate = app->tci_iq_rate;
  s->tx_pan_high = app->tx_pan_high;
  s->tx_pan_low  = app->tx_pan_low;
  /* per-mode filter memory → "modeid=idx;..." */
  char *mp = s->mode_filt; size_t mrem = sizeof(s->mode_filt); mp[0] = '\0';
  for (int i = 0; i < DEMOD_NMODES; i++) {
    int n = snprintf(mp, mrem, "%s%d=%d", i ? ";" : "", i, app->filter_by_mode[i]);
    if (n < 0 || (size_t)n >= mrem) { break; }
    mp += n; mrem -= (size_t)n;
  }
  /* Var1/Var2 per mode → "modeid/v1lo/v1hi/v2lo/v2hi;..." */
  char *vp = s->var_filt; size_t vrem = sizeof(s->var_filt); vp[0] = '\0';
  for (int i = 0; i < DEMOD_NMODES; i++) {
    int n = snprintf(vp, vrem, "%s%d/%d/%d/%d/%d", i ? ";" : "", i,
                     app->var_low[i][0], app->var_high[i][0],
                     app->var_low[i][1], app->var_high[i][1]);
    if (n < 0 || (size_t)n >= vrem) { break; }
    vp += n; vrem -= (size_t)n;
  }
  s->pan_high = app->pan_high;
  s->pan_low  = app->pan_low;
  s->db_grid    = app->show_db_grid;
  s->db_scale   = app->show_db_scale;
  s->freq_grid  = app->show_freq_grid;
  s->freq_scale = app->show_freq_scale;
  s->filter_wf  = app->show_filter_wf;
  s->filter_op  = app->filter_op;
  s->auto_level = app->auto_level;
  s->avg_spec   = app->avg_spec_ms;
  s->avg_wf     = app->avg_wf_ms;
  s->palette    = app->palette;
  s->band_edges = app->show_band_edges;
  s->show_spots = app->show_spots;
  g_strlcpy(s->region,  bp_region_key(app->bp_region),   sizeof(s->region));
  g_strlcpy(s->country, bp_country_key(app->bp_country), sizeof(s->country));
  s->win_w   = app->win_w;
  s->win_h   = app->win_h;
  s->win_max = app->win_max;
  /* per-band state → "key=hi/lo/mode;..." (locale-independent '.') */
  char *bp = s->band_levels; size_t brem = sizeof(s->band_levels); bp[0] = '\0';
  for (int i = 0; i < NBANDS; i++) {
    char hbuf[G_ASCII_DTOSTR_BUF_SIZE], lbuf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(hbuf, sizeof hbuf, "%.1f", app->band_high[i]);
    g_ascii_formatd(lbuf, sizeof lbuf, "%.1f", app->band_low[i]);
    int n = snprintf(bp, brem, "%s%s=%s/%s/%d/%lld", i ? ";" : "", BANDS[i].key,
                     hbuf, lbuf, app->band_mode[i], app->band_freq[i]);
    if (n < 0 || (size_t)n >= brem) { break; }
    bp += n; brem -= (size_t)n;
  }
  /* per-band PA calibration → "key=dB;..." (F6b, locale-independent '.') */
  char *cp = s->pa_cal; size_t crem = sizeof(s->pa_cal); cp[0] = '\0';
  for (int i = 0; i < NBANDS; i++) {
    char cbuf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(cbuf, sizeof cbuf, "%.1f", app->band_pacal[i]);
    int n = snprintf(cp, crem, "%s%s=%s", i ? ";" : "", BANDS[i].key, cbuf);
    if (n < 0 || (size_t)n >= crem) { break; }
    cp += n; crem -= (size_t)n;
  }
  /* wattmeter-trim curve → 11 semicolon-separated points (W) */
  char *tp = s->pa_trim; size_t trem = sizeof(s->pa_trim); tp[0] = '\0';
  for (int i = 0; i < 11; i++) {
    char tbuf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(tbuf, sizeof tbuf, "%.3f", app->pa_trim[i]);
    int n = snprintf(tp, trem, "%s%s", i ? ";" : "", tbuf);
    if (n < 0 || (size_t)n >= trem) { break; }
    tp += n; trem -= (size_t)n;
  }
}

static gboolean do_save_cb(gpointer data) {
  App *app = (App *)data;
  app->save_timer_id = 0;
  Settings s;
  app_to_settings(app, &s);
  settings_save(&s);
  return G_SOURCE_REMOVE;
}

/* Debounced save: write ~1 s after the last change (rapid tuning coalesces). */
static void schedule_save(App *app) {
  if (!app->radio_mode) { return; }
  if (app->save_timer_id) { g_source_remove(app->save_timer_id); }
  app->save_timer_id = g_timeout_add_seconds(1, do_save_cb, app);
}

/* Voice/phone modes need the mic; CW and data modes do not. WDSP mode ids — kept
 * broad so future FM/DSB/SAM count as voice the moment they're wired. */
static int mode_is_voice(int mode) {
  switch (mode) {
    case DEMOD_LSB: case DEMOD_USB: case DEMOD_AM:   /* + future FM/DSB/SAM */
      return 1;
    default:                                         /* CWL/CWU + data modes */
      return 0;
  }
}

/* Open/close the host-soundcard mic to match the selected mode (Richard's F6c
 * choice): capture runs into the tx_run ring whenever a voice mode is active and
 * the TX runtime is up, so the exciter has fresh audio the instant MOX is pressed —
 * no warm-up lag. CW/data modes keep the mic closed (no "recording"). The capture
 * rate must equal the WDSP TX input; PipeWire resamples the device for us. The mic
 * only ever reaches the exciter through tx_gate/MOX — dormant until F6c-3 enables
 * the MOX button. Idempotent; safe to call on any mode/engine transition. */
static void tx_update_mic(App *app) {
  int want = app->tx_ready && mode_is_voice(app->mode);
  if (want && !app->mic_open) {
    const char *dev = app->mic_device[0] ? app->mic_device : NULL;
    if (mic_start(tx_dsp_in_rate(), app->latency, dev) == 0) {
      app->mic_open = 1;
      fprintf(stderr, "mic: capture open @ %d Hz (%s) — voice mode\n",
              tx_dsp_in_rate(), dev ? dev : "default source");
    } else {
      fprintf(stderr, "mic_start failed — SSB voice TX will be silent\n");
    }
  } else if (!want && app->mic_open) {
    mic_stop();
    app->mic_open = 0;
    fprintf(stderr, "mic: capture closed — non-voice mode\n");
  }
  /* MOX/voice is only meaningful with the mic open (a voice mode); grey it out for
   * CW/data, exactly like the mic itself. Button may not exist yet during startup. */
  if (app->mox_btn) { gtk_widget_set_sensitive(app->mox_btn, want); }
}

/* Push the operator's persisted TX settings into the runtime. Safe if TX isn't up.
 * pa_calibration follows the CURRENT band (F6b per-band table); out of band it
 * falls back to the default, but the gate refuses OOB keying anyway. Re-called on
 * band change (band_apply) so the drive byte tracks the band's PA calibration. */
static void tx_push_cfg(App *app) {
  if (!app->tx_ready) { return; }
  int b = band_for_freq(app->freq);
  tx_run_cfg c;
  memset(&c, 0, sizeof c);
  c.pa_enabled     = app->tx_pa_enabled;
  c.antenna        = app->tx_antenna;
  c.drive_w        = app->tx_drive_w;
  c.tune_w         = app->tx_tune_w;
  c.pa_calibration = (b >= 0) ? pacal_clamp(app->band_pacal[b]) : PACAL_DEFAULT;
  c.swr_protect    = 1;
  c.swr_alarm      = app->tx_swr_alarm;
  c.allow_oob      = 0;
  c.region         = app->bp_region;
  c.country_key    = bp_country_key(app->bp_country);
  c.mode           = app->mode;
  c.tx_flo         = app->tx_flo;
  c.tx_fhi         = app->tx_fhi;
  for (int i = 0; i < 11; i++) { c.pa_trim[i] = app->pa_trim[i]; }
  tx_run_set_cfg(&c);
}

/* ---- vertical dB axis — grab the scale (left gutter) --------------------- */

static double clampd(double v, double lo, double hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

/* Panadapter pixel height (the top PANADAPTER_FRACTION of the drawing area). */
static double pan_height_px(App *app) {
  return gtk_widget_get_height(app->area) * PANADAPTER_FRACTION;
}

/* Push the TX dB window onto the TX waterfall so it colours to the same manual
 * scale (Richard's F6c-3 ask: the waterfall follows the operator's dB axis). */
static void tx_pan_apply(App *app) {
  waterfall_set_manual_range(app->tx_wf, app->tx_pan_low, app->tx_pan_high - app->tx_pan_low);
}

/* The dB window currently shown: the TX window while keyed (TX panadapter), else
 * the RX window. Lets the shared gutter drag/scroll act on whichever is visible. */
static double pan_win_high(App *app) { return app->tx_display ? app->tx_pan_high : app->pan_high; }
static double pan_win_low (App *app) { return app->tx_display ? app->tx_pan_low  : app->pan_low;  }

/* Store a proposed [high,low] dB window, clamped to a sane range and dBm span
 * (range preserved; the window slides within [FLOOR,CEIL]). Targets the TX window
 * while the TX panadapter is up, else the RX window (per-band). */
static void pan_set_window(App *app, double high, double low) {
  double range = clampd(high - low, PAN_RANGE_MIN, PAN_RANGE_MAX);
  high = clampd(high, PAN_DBM_FLOOR + range, PAN_DBM_CEIL);
  if (app->tx_display) {
    app->tx_pan_high = high;
    app->tx_pan_low  = high - range;
    tx_pan_apply(app);
  } else {
    app->pan_high = high;
    app->pan_low  = high - range;
    pan_store_band(app);   /* remember it for the current band */
  }
}

/* Is the pointer over the left dB gutter (the grab zone for the vertical axis)? */
static int in_gutter(App *app, double x, double y) {
  return x < PANADAPTER_GUTTER_W && y < pan_height_px(app);
}

/* Double-click the scale → auto-fit: put the noise floor near the bottom and
 * keep a readable window, so the trace is well-placed as the floor drifts. */
static void pan_autofit(App *app) {
  int n = app->ema_w;
  if (n < 8) { return; }
  static float srt[SPECTRUM_DATA_SIZE];
  memcpy(srt, app->ema, n * sizeof(float));
  qsort(srt, n, sizeof(float), cmp_float);
  double floor_db = srt[(int)(n * 0.20)];
  double peak_db  = srt[n - 1];
  double low  = floor_db - 10.0;
  double high = peak_db + 12.0;
  if (high - low < 50.0) { high = low + 50.0; }
  pan_set_window(app, high, low);
  schedule_save(app);
  gtk_widget_queue_draw(app->area);
}

/* One-shot fit of the TX dB window to the transmitted spectrum (double-click the
 * gutter while keyed, and once automatically on the first TX frame). Then it stays
 * a fixed, draggable scale. Falls back to a sane window if no TX frame yet. */
static void tx_pan_autofit(App *app) {
  int n = app->tx_ema_w;
  if (n < 8) { app->tx_pan_high = -40.0; app->tx_pan_low = -130.0; tx_pan_apply(app); return; }
  static float srt[SPECTRUM_DATA_SIZE];
  memcpy(srt, app->tx_ema, n * sizeof(float));
  qsort(srt, n, sizeof(float), cmp_float);
  double floor_db = srt[(int)(n * 0.20)];
  double peak_db  = srt[n - 1];
  double low  = floor_db - 10.0;
  double high = peak_db + 12.0;
  if (high - low < 50.0) { high = low + 50.0; }
  app->tx_pan_high = high;
  app->tx_pan_low  = low;
  tx_pan_apply(app);
}

/* Mouse wheel over the panadapter re-tunes the DDC (Model A: the whole span
 * moves, passband stays centred) by the selected step. Zoom lives on the right
 * mouse button (drag) now, not the wheel. Each tune notch snaps to a clean grid
 * multiple in the scroll direction. */
static gboolean on_scroll(GtkEventControllerScroll *ctl, double dx, double dy, gpointer data) {
  (void)dx; (void)ctl;
  App *app = (App *)data;
  if (!app->radio_mode) { return FALSE; }
  if (!app->engine_ok) { return FALSE; }
  int dir = (int)llround(-dy);        /* wheel up (dy < 0) tunes higher */
  if (dir == 0) { return FALSE; }
  long long step = app->tune_step > 0 ? app->tune_step : TUNE_STEP_DEFAULT;
  long long f = app->freq;
  /* Snap to the grid in the scroll direction (floor going up, ceil going down),
   * then move whole steps — so nf is always a multiple of step. */
  long long base = (dir > 0) ? (f / step) * step
                             : ((f + step - 1) / step) * step;
  long long nf = base + (long long)dir * step;
  if (nf < 1) { nf = 1; }
  app->freq = nf;              /* readout follows on the next tick */
  p2_set_frequency(nf);
  schedule_save(app);
  return TRUE;
}

/* Which Var passband edge is near screen-x? 0 none, 1 low, 2 high. Only Var
 * filters (idx >= NPRESET) are draggable. */
static int edge_hit(App *app, double x) {
  if (app->filter_idx < NPRESET || app->fhi <= app->flo) { return 0; }
  int w = gtk_widget_get_width(app->area);
  if (w < 1) { return 0; }
  double hzpp = (double)app->rate / app->zoom / w;
  double cx = vfo_x(app, w);
  if (fabs(x - (cx + app->flo / hzpp)) <= FILT_HIT_PX) { return 1; }
  if (fabs(x - (cx + app->fhi / hzpp)) <= FILT_HIT_PX) { return 2; }
  return 0;
}

/* Left-drag grabs the spectrum and pans the DDC centre (Model A) — UNLESS it
 * started in the dB gutter (slides the level window) or on a Var passband edge
 * (drags that edge). Both pans are absolute from drag-begin (no drift). */
static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer data) {
  App *app = (App *)data;
  GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
  app->drag_gutter = in_gutter(app, x, y);
  app->drag_edge = 0;
  app->drag_pan = 0;
  if (app->drag_gutter) {
    app->drag_base_high = pan_win_high(app);
    app->drag_base_low  = pan_win_low(app);
  } else if ((mods & GDK_SHIFT_MASK) && app->zoom > 1.0) {   /* shift+drag = pan the view */
    app->drag_pan = 1;
    app->drag_base_pan = app->pan;
  } else if (!app->select_mode && (app->drag_edge = edge_hit(app, x))) {
    app->drag_begin_x = x;               /* edge follows the cursor */
  } else {
    app->drag_base_freq = app->freq;
  }
}

static void on_drag_update(GtkGestureDrag *g, double off_x, double off_y, gpointer data) {
  (void)g;
  App *app = (App *)data;
  if (!app->radio_mode) { return; }

  if (app->drag_gutter) {                 /* vertical: slide the dB window */
    if (!app->tx_display && app->auto_level) { return; }  /* RX auto-floor: no manual pan (TX is always manual) */
    double ph = pan_height_px(app);
    if (ph < 1.0) { return; }
    double range = app->drag_base_high - app->drag_base_low;
    double shift = (off_y / ph) * range;  /* drag down → trace & window move down */
    pan_set_window(app, app->drag_base_high + shift, app->drag_base_low + shift);
    gtk_widget_queue_draw(app->area);
    return;
  }

  if (app->drag_pan) {                    /* shift+drag: slide the zoomed view sideways */
    int w = gtk_widget_get_width(app->area);
    double max_off = (double)app->rate * (1.0 - 1.0 / app->zoom) / 2.0;
    if (w < 1 || app->zoom <= 1.0 || max_off <= 0.0) { return; }
    double hzpp = (double)app->rate / app->zoom / w;
    double new_hz = app->drag_base_pan * max_off - off_x * hzpp;   /* drag right → lower freq */
    double np = new_hz / max_off;
    if (np < -1.0) { np = -1.0; }
    if (np >  1.0) { np =  1.0; }
    app->pan = np;
    analyzer_set_pan(np);
    gtk_widget_queue_draw(app->area);
    return;
  }

  if (app->drag_edge) {                   /* drag a Var passband edge */
    int w = gtk_widget_get_width(app->area);
    if (w < 1) { return; }
    double hzpp = (double)app->rate / app->zoom / w;
    int f = (int)lround((app->drag_begin_x + off_x - vfo_x(app, w)) * hzpp);
    int v = (app->filter_idx - NPRESET) & 1;
    const int MINW = 50;                  /* keep a minimum passband width */
    if (app->drag_edge == 1) {            /* low edge */
      if (f > app->fhi - MINW) { f = app->fhi - MINW; }
      if (f < -20000) { f = -20000; }
      app->flo = f; app->var_low[app->mode][v] = f;
    } else {                              /* high edge */
      if (f < app->flo + MINW) { f = app->flo + MINW; }
      if (f > 20000) { f = 20000; }
      app->fhi = f; app->var_high[app->mode][v] = f;
    }
    demod_set_passband(app->flo, app->fhi);
    schedule_save(app);
    gtk_widget_queue_draw(app->area);
    return;
  }

  if (app->select_mode) { return; }   /* select mode: left = click-tune, no pan */
  if (!app->engine_ok) { return; }
  int w = gtk_widget_get_width(app->area);
  if (w < 1) { return; }
  double hz_per_px = (double)app->rate / app->zoom / w;   /* narrower span when zoomed */
  /* Drag right → content follows the cursor → view moves to lower frequency. */
  long long nf = app->drag_base_freq - (long long)llround(off_x * hz_per_px);
  if (nf < 1) { nf = 1; }
  app->freq = nf;             /* readout follows on the next tick */
  p2_set_frequency(nf);
  schedule_save(app);
}

/* Track the pointer so on_scroll (which carries no coords) can hit-test the
 * gutter — and, in select mode, so the filter cursor follows the mouse. */
static void on_motion(GtkEventControllerMotion *m, double x, double y, gpointer data) {
  (void)m;
  App *app = (App *)data;
  app->ptr_x = x;
  app->ptr_y = y;
  if (app->select_mode) { gtk_widget_queue_draw(app->area); return; }
  /* Show a resize cursor over a draggable Var passband edge. */
  static int over = -1;
  int e = (!in_gutter(app, x, y) && edge_hit(app, x)) ? 1 : 0;
  if (e != over) {
    over = e;
    gtk_widget_set_cursor_from_name(app->area, e ? "ew-resize" : NULL);
  }
}

/* Recenter the span on the frequency under x (Model A: not CTUN — the whole
 * span moves so the clicked signal lands at centre). */
static void click_tune(App *app, double x) {
  if (!app->engine_ok) { return; }
  int w = gtk_widget_get_width(app->area);
  if (w < 1) { return; }
  double hz_per_px = (double)app->rate / app->zoom / w;
  long long nf = app->freq + (long long)llround((x - vfo_x(app, w)) * hz_per_px);
  if (nf < 1) { nf = 1; }
  app->freq = nf;
  if (app->pan != 0.0) { app->pan = 0.0; analyzer_set_pan(0.0); }   /* recentre on the signal */
  p2_set_frequency(nf);
  schedule_save(app);
}

/* Left click: double-click the gutter → auto-fit; single click in select mode
 * → tune to the cursor (recenter). */
static void on_pressed(GtkGestureClick *g, int n_press, double x, double y, gpointer data) {
  (void)g;
  App *app = (App *)data;
  if (!app->radio_mode) { return; }
  if (n_press == 2 && in_gutter(app, x, y)) {
    if (app->tx_display) { tx_pan_autofit(app); schedule_save(app); gtk_widget_queue_draw(app->area); }
    else                 { pan_autofit(app); }
    return;
  }
  if (n_press == 2 && !in_gutter(app, x, y) && app->pan != 0.0) {   /* recentre the pan */
    app->pan = 0.0; analyzer_set_pan(0.0); gtk_widget_queue_draw(app->area); return;
  }
  /* Spot label hit → tune exactly to the spot + tell TCI clients (F6d-2e).
   * Checked before select mode so a spot click always wins over recentre. */
  if (n_press == 1 && app->show_spots && !app->tx_display && !in_gutter(app, x, y)) {
    for (int i = 0; i < app->nspots; i++) {
      struct spot *s = &app->spots[i];
      if (s->hx1 > s->hx0 && x >= s->hx0 && x <= s->hx1 && y >= s->hy0 && y <= s->hy1) {
        app->freq = s->hz;                     /* like click_tune, exact Hz */
        if (app->pan != 0.0) { app->pan = 0.0; analyzer_set_pan(0.0); }
        p2_set_frequency(s->hz);
        schedule_save(app);
        tci_server_spot_clicked(s->call, s->hz);
        gtk_widget_queue_draw(app->area);
        return;
      }
    }
  }
  if (app->select_mode && n_press == 1 && !in_gutter(app, x, y)) { click_tune(app, x); }
}

/* Right click toggles select mode (a filter-shaped cursor you aim at a signal;
 * left-click then recenters on it). */
/* Right button = ZOOM by dragging (dB range in the gutter, frequency span on the
 * body). A plain right-click (no drag) still toggles select mode — handled by the
 * click gesture below, gated on rdrag_zoomed so a drag never also toggles. */
static void on_rdrag_begin(GtkGestureDrag *g, double x, double y, gpointer data) {
  (void)g;
  App *app = (App *)data;
  if (!app->radio_mode) { return; }
  app->rdrag_gutter = in_gutter(app, x, y);
  if (app->rdrag_gutter) {
    app->rdrag_base_high = pan_win_high(app);
    app->rdrag_base_low  = pan_win_low(app);
    double ph = pan_height_px(app);
    app->rdrag_anchor_frac = ph > 1.0 ? y / ph : 0.5;   /* hold the grabbed dB fixed */
  } else {
    app->rdrag_base_zoom = app->zoom;
  }
}

static void on_rdrag_update(GtkGestureDrag *g, double off_x, double off_y, gpointer data) {
  (void)g;
  App *app = (App *)data;
  if (!app->radio_mode) { return; }
  app->rdrag_zoomed = 1;                    /* this is a drag, not a click */
  if (app->rdrag_gutter) {                  /* vertical drag → zoom the dB range */
    double ph = pan_height_px(app);
    if (ph < 1.0) { return; }
    double base = app->rdrag_base_high - app->rdrag_base_low;
    double frac = app->rdrag_anchor_frac;
    double anchor = app->rdrag_base_high - frac * base;          /* dB held under cursor */
    double nr = clampd(base * pow(2.0, off_y / ph), PAN_RANGE_MIN, PAN_RANGE_MAX);
    pan_set_window(app, anchor + frac * nr, anchor + frac * nr - nr);   /* up = zoom in */
    schedule_save(app);
    gtk_widget_queue_draw(app->area);
  } else if (app->engine_ok) {              /* horizontal drag → zoom the span (around VFO) */
    /* Snap to the same octave raster as the footer slider (1,2,4,…,128×). */
    int maxoct = (int)lround(log2(ZOOM_MAX));
    int noct = (int)lround(log2(app->rdrag_base_zoom) + off_x / 90.0);   /* ~90 px / octave */
    if (noct < 0) { noct = 0; } else if (noct > maxoct) { noct = maxoct; }
    double nz = pow(2.0, noct);
    if (nz != app->pending_zoom) {
      app->pending_zoom = nz;
      app->zoom_dirty   = 1;                /* applied ≤1×/frame in the tick */
      if (app->zoom_scale) {                /* step the footer slider in lockstep */
        g_signal_handlers_block_by_func(app->zoom_scale, (gpointer)on_zoom_changed, app);
        gtk_range_set_value(GTK_RANGE(app->zoom_scale), noct);
        g_signal_handlers_unblock_by_func(app->zoom_scale, (gpointer)on_zoom_changed, app);
      }
    }
  }
}

static void on_rdrag_end(GtkGestureDrag *g, double off_x, double off_y, gpointer data) {
  (void)g; (void)off_x; (void)off_y; (void)data;
  /* Zoom + footer-slider sync happen live in on_rdrag_update; nothing to finalise. */
}

/* Right press starts fresh; right release with no intervening drag = a click →
 * toggle select mode (the classic right-click behaviour, kept alongside zoom). */
static void on_right_pressed(GtkGestureClick *g, int n_press, double x, double y, gpointer data) {
  (void)g; (void)n_press; (void)x; (void)y;
  ((App *)data)->rdrag_zoomed = 0;
}
static void on_right_released(GtkGestureClick *g, int n_press, double x, double y, gpointer data) {
  (void)g; (void)n_press; (void)x; (void)y;
  App *app = (App *)data;
  if (!app->radio_mode || app->rdrag_zoomed) { return; }   /* a drag, not a click */
  app->select_mode = !app->select_mode;
  gtk_widget_set_cursor_from_name(app->area, app->select_mode ? "crosshair" : NULL);
  gtk_widget_queue_draw(app->area);
}

/* Keys u/l/c/a switch demod mode by activating the matching strip toggle
 * (which drives the engine), so keyboard and buttons stay in sync. */
static gboolean on_key(GtkEventControllerKey *ctl, guint keyval, guint keycode,
                       GdkModifierType state, gpointer data) {
  (void)ctl; (void)keycode;
  App *app = (App *)data;
  if (!app->radio_mode) { return FALSE; }
  if (keyval == GDK_KEY_Escape && app->select_mode) {
    app->select_mode = 0;
    gtk_widget_set_cursor_from_name(app->area, NULL);
    gtk_widget_queue_draw(app->area);
    return TRUE;
  }
  /* Esc aborts any queued/running CW (the CW source is TCI, F6d-2; the F6d-1b
   * 'k' test hotkey is gone — one stray keypress must never key the radio). */
  if (keyval == GDK_KEY_Escape && app->tx_ready) { tx_run_cw_abort(); return TRUE; }
  /* CW dev trigger, ENV-GATED: Ctrl+Shift+K queues a test string, but only when
   * the app was launched with SDRFL_CW_TEST=1 (+ CW mode + TX up). A normal run
   * still has no key that can key the radio — the 45f73ae audit rule holds. */
  if (gdk_keyval_to_lower(keyval) == GDK_KEY_k &&
      (state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK) &&
      app->tx_ready && (app->mode == DEMOD_CWL || app->mode == DEMOD_CWU)) {
    const char *t = g_getenv("SDRFL_CW_TEST");
    if (t && strcmp(t, "1") == 0) { tx_run_cw_send("V V V TEST DE OK1BR "); }
    return TRUE;
  }
  int mode;
  switch (gdk_keyval_to_lower(keyval)) {
    case GDK_KEY_u: mode = DEMOD_USB; break;
    case GDK_KEY_l: mode = DEMOD_LSB; break;
    case GDK_KEY_c: mode = DEMOD_CWU; break;
    case GDK_KEY_a: mode = DEMOD_AM;  break;
    default: return FALSE;
  }
  if (app->mode_btns[mode]) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->mode_btns[mode]), TRUE);
  }
  return TRUE;
}

/* ---- control-strip callbacks (wired to the engine) ----------------------- */

/* Filter dropdown → apply the selected preset's passband live. */
static void on_filter_changed(GtkDropDown *dd, GParamSpec *ps, gpointer data) {
  (void)ps;
  App *app = (App *)data;
  int nf, dfl;
  mode_filters(app->mode, &nf, &dfl);
  guint idx = gtk_drop_down_get_selected(dd);
  if ((int)idx >= nf + 2) { return; }          /* presets + Var1 + Var2 */
  app->filter_idx = (int)idx;
  app->filter_by_mode[app->mode] = (int)idx;   /* remember it for this mode */
  int lo, hi; filter_lohi(app, app->mode, (int)idx, &lo, &hi);
  app->flo = lo; app->fhi = hi;
  demod_set_passband(app->flo, app->fhi);
  schedule_save(app);                        /* persist the chosen filter */
}

/* Rebuild the filter dropdown for the current mode and select filter_idx. */
static void populate_filter_dd(App *app) {
  if (!app->filter_dd) { return; }
  int nf, dfl;
  const FilterPreset *ft = mode_filters(app->mode, &nf, &dfl);
  const char *names[16];
  for (int i = 0; i < nf; i++) { names[i] = ft[i].name; }
  names[nf] = "Var1"; names[nf + 1] = "Var2"; names[nf + 2] = NULL;  /* editable */
  GtkStringList *m = gtk_string_list_new(names);
  g_signal_handlers_block_by_func(app->filter_dd, (gpointer)on_filter_changed, app);
  gtk_drop_down_set_model(GTK_DROP_DOWN(app->filter_dd), G_LIST_MODEL(m));
  gtk_drop_down_set_selected(GTK_DROP_DOWN(app->filter_dd), app->filter_idx);
  g_signal_handlers_unblock_by_func(app->filter_dd, (gpointer)on_filter_changed, app);
  g_object_unref(m);
}

/* Push PROC + mic gate to the TX runtime with the digi override: in DIGU/DIGL
 * the chain must be COMPLETELY clean (Richard's rule — no compressor, leveler,
 * gate or EQ may touch data audio), regardless of the stored voice settings. */
static void tx_apply_proc(App *app) {
  int digi = (app->mode == DEMOD_DIGU || app->mode == DEMOD_DIGL);
  tx_run_set_comp(digi ? 0 : app->tx_comp, app->tx_comp_db);
  tx_run_set_gate(digi ? 0 : app->tx_gate, app->tx_gate_db);
}

static void on_mode_toggled(GtkToggleButton *b, gpointer data) {
  if (!gtk_toggle_button_get_active(b)) { return; }  /* ignore the deselect half */
  App *app = (App *)data;
  int mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "mode"));
  int nf, dfl;
  mode_filters(mode, &nf, &dfl);
  app->mode = mode;
  if (app->cur_band >= 0) { app->band_mode[app->cur_band] = mode; }   /* band stacking */
  /* restore this mode's last-used filter (fall back to its default) */
  int fidx = app->filter_by_mode[mode];
  if (fidx < 0 || fidx >= nf + 2) { fidx = dfl; }
  app->filter_idx = fidx;
  app->filter_by_mode[mode] = fidx;
  int lo, hi; filter_lohi(app, mode, fidx, &lo, &hi);
  app->flo = lo; app->fhi = hi;
  demod_set_mode(mode, app->flo, app->fhi);
  populate_filter_dd(app);               /* refill dropdown for the new mode */
  tx_push_cfg(app);                      /* TX runtime learns the mode (CW break-in gates on it) */
  tx_update_mic(app);                    /* voice mode → mic ready; CW/data → mic closed */
  tx_apply_proc(app);                    /* digi = clean chain (PROC/gate forced off) */
  schedule_save(app);
}

static void on_volume_changed(GtkRange *r, gpointer data) {
  App *app = (App *)data;
  double v = gtk_range_get_value(r);
  demod_set_volume(v);
  app->volume = v;
  schedule_save(app);
}

/* RF front-end: ADC0 step attenuator (0-31 dB; 0 = max sensitivity). */
static void on_atten_changed(GtkRange *r, gpointer data) {
  App *app = (App *)data;
  int db = (int)gtk_range_get_value(r);
  p2_set_attenuation(db);
  app->atten = db;
  schedule_save(app);
}

/* AGC dropdown index → mode; dropdown order is Med,Fast,Slow,Long,Off. */
static const int AGC_MODE_OF_IDX[] = { 3, 4, 2, 1, 0 };

static void on_agc_changed(GtkDropDown *dd, GParamSpec *ps, gpointer data) {
  (void)ps;
  App *app = (App *)data;
  guint i = gtk_drop_down_get_selected(dd);
  if (i >= G_N_ELEMENTS(AGC_MODE_OF_IDX)) { return; }
  app->agc = AGC_MODE_OF_IDX[i];
  demod_set_agc(app->agc);
  schedule_save(app);
}

/* AGC-T threshold/gain slider. */
static void on_agct_changed(GtkRange *r, gpointer data) {
  App *app = (App *)data;
  double g = gtk_range_get_value(r);
  demod_set_agc_gain(g);
  app->agc_gain = g;
  schedule_save(app);
}

/* Noise reduction / blanker / auto-notch toggles. */
/* NR cycles off/NR/NR2/NR3/NR4 (ANR, EMNR, RNNoise, specbleach); NB cycles
 * off/NB/NB2 (ANB, SNBA). A toggle button cycles; the label shows the algorithm
 * (suffix = the mode number for 2+), :checked shows on. */
static void noise_btn_update(GtkButton *b, int mode, const char *base) {
  char lbl[8];
  char suf[2] = "";
  if (mode >= 2 && mode <= 9) { suf[0] = (char)('0' + mode); }   /* single-digit mode suffix */
  snprintf(lbl, sizeof lbl, "%s%s", base, suf);
  gtk_button_set_label(b, lbl);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), mode > 0);   /* fires "toggled", not "clicked" */
}
static void on_nr_clicked(GtkButton *b, gpointer data) {
  App *app = (App *)data;
  app->nr = (app->nr + 1) % 5;   /* off / NR / NR2 / NR3 / NR4 */
  demod_set_nr(app->nr);
  noise_btn_update(b, app->nr, "NR");
  schedule_save(app);
}
static void on_nb_clicked(GtkButton *b, gpointer data) {
  App *app = (App *)data;
  app->nb = (app->nb + 1) % 3;
  demod_set_nb(app->nb);
  noise_btn_update(b, app->nb, "NB");
  schedule_save(app);
}
static void on_anf_toggled(GtkToggleButton *b, gpointer data) {
  App *app = (App *)data;
  app->anf = gtk_toggle_button_get_active(b);
  demod_set_anf(app->anf);
  schedule_save(app);
}
static void on_bin_toggled(GtkToggleButton *b, gpointer data) {
  App *app = (App *)data;
  app->binaural = gtk_toggle_button_get_active(b);
  demod_set_binaural(app->binaural);   /* live: flips the WDSP panel copy */
  schedule_save(app);
}

/* TX key (F6a): both TUNE and MOX route here; we hand the runtime the combined
 * intent and let tx_gate decide whether it actually keys. MOX is disabled until
 * F6c, so only TUNE can set intent today. */
/* TX monitor (self-listen): voice mic / CW sidetone into the host audio.
 * On/off is the header-bar MON toggle (operational control, next to MOX);
 * only the level lives in Preferences. */
static void on_mon_toggled(GtkToggleButton *b, gpointer data) {
  App *app = (App *)data;
  app->tx_mon = gtk_toggle_button_get_active(b) ? 1 : 0;
  tx_run_set_monitor(app->tx_mon);
  schedule_save(app);
}

static void on_tx_key_toggled(GtkToggleButton *b, gpointer data) {
  (void)b;
  App *app = (App *)data;
  if (!app->tx_ready) { return; }
  int mox  = app->mox_btn  && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->mox_btn));
  int tune = app->tune_btn && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->tune_btn));
  tx_run_request(mox, tune);
}

static void on_band_clicked(GtkButton *b, gpointer data) {
  App *app = (App *)data;
  long long f = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "freq"));
  int bi = band_for_freq(f);
  if (bi >= 0 && app->band_freq[bi] > 0) { f = app->band_freq[bi]; }  /* last freq on this band */
  app->freq = f;
  if (app->pan != 0.0) { app->pan = 0.0; analyzer_set_pan(0.0); }   /* new band → recentre */
  p2_set_frequency(f);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), TRUE);  /* stay lit (band_apply fixes others) */
  schedule_save(app);
}

/* Small "label: widget" pair for the strip. */
static GtkWidget *labeled(const char *text, GtkWidget *w) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *l = gtk_label_new(text);
  gtk_widget_add_css_class(l, "dim");
  gtk_box_append(GTK_BOX(box), l);
  gtk_box_append(GTK_BOX(box), w);
  return box;
}

/* Build the horizontal control strip and wire it to the engine. Radio mode. */
static GtkWidget *build_controls(App *app) {
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(bar, "controlbar");

  /* Mode — segmented, grouped; keep a handle per DEMOD id for key sync. */
  static const int         mids[]   = {DEMOD_USB, DEMOD_LSB, DEMOD_CWL, DEMOD_CWU, DEMOD_AM,
                                       DEMOD_DIGU, DEMOD_DIGL};
  static const char *const mlabels[] = {"USB", "LSB", "CWL", "CWU", "AM", "DIGU", "DIGL"};
  GtkWidget *modebox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(modebox, "linked");
  GtkWidget *group = NULL;
  for (int i = 0; i < (int)G_N_ELEMENTS(mids); i++) {
    GtkWidget *b = gtk_toggle_button_new_with_label(mlabels[i]);
    gtk_widget_add_css_class(b, "mode");
    g_object_set_data(G_OBJECT(b), "mode", GINT_TO_POINTER(mids[i]));
    if (!group) { group = b; } else { gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(group)); }
    app->mode_btns[mids[i]] = b;
    gtk_box_append(GTK_BOX(modebox), b);
  }
  if (app->mode_btns[app->mode]) {   /* reflect the resolved startup mode … */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->mode_btns[app->mode]), TRUE);
  }
  for (int i = 0; i < (int)G_N_ELEMENTS(mids); i++) {  /* … then connect, so this doesn't re-fire the engine */
    g_signal_connect(app->mode_btns[mids[i]], "toggled", G_CALLBACK(on_mode_toggled), app);
  }
  gtk_box_append(GTK_BOX(bar), modebox);

  /* Filter — piHPSDR presets for the current mode; repopulated on mode change. */
  app->filter_dd = gtk_drop_down_new(NULL, NULL);
  g_signal_connect(app->filter_dd, "notify::selected", G_CALLBACK(on_filter_changed), app);
  populate_filter_dd(app);
  gtk_box_append(GTK_BOX(bar), labeled("Filter", app->filter_dd));

  /* RF attenuator (front-end gain): ADC0 step attenuator, 0-31 dB, 0 = max sens. */
  GtkWidget *att = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 31, 1);
  gtk_range_set_value(GTK_RANGE(att), app->atten);      /* before wiring: no spurious send */
  gtk_widget_set_size_request(att, 110, -1);
  gtk_scale_set_draw_value(GTK_SCALE(att), TRUE);
  gtk_scale_set_value_pos(GTK_SCALE(att), GTK_POS_RIGHT);
  g_signal_connect(att, "value-changed", G_CALLBACK(on_atten_changed), app);
  gtk_box_append(GTK_BOX(bar), labeled("Att", att));

  /* AGC character + AGC-T (threshold/gain). NR·NB·ANF still placeholders below. */
  GtkWidget *agc_dd = gtk_drop_down_new_from_strings(
      (const char *[]){"Med","Fast","Slow","Long","Off", NULL});
  guint aidx = 0;
  for (guint i = 0; i < G_N_ELEMENTS(AGC_MODE_OF_IDX); i++) {
    if (AGC_MODE_OF_IDX[i] == app->agc) { aidx = i; break; }
  }
  gtk_drop_down_set_selected(GTK_DROP_DOWN(agc_dd), aidx);   /* before wiring: no re-fire */
  g_signal_connect(agc_dd, "notify::selected", G_CALLBACK(on_agc_changed), app);
  gtk_box_append(GTK_BOX(bar), labeled("AGC", agc_dd));

  GtkWidget *agct = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -20, 120, 1);
  gtk_range_set_value(GTK_RANGE(agct), app->agc_gain);       /* before wiring */
  gtk_widget_set_size_request(agct, 110, -1);
  gtk_scale_set_draw_value(GTK_SCALE(agct), TRUE);
  gtk_scale_set_value_pos(GTK_SCALE(agct), GTK_POS_RIGHT);
  g_signal_connect(agct, "value-changed", G_CALLBACK(on_agct_changed), app);
  gtk_box_append(GTK_BOX(bar), labeled("AGC-T", agct));

  GtkWidget *nrbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(nrbox, "linked");
  GtkWidget *nr_b  = gtk_toggle_button_new_with_label("NR");
  GtkWidget *nb_b  = gtk_toggle_button_new_with_label("NB");
  GtkWidget *anf_b = gtk_toggle_button_new_with_label("ANF");
  GtkWidget *bin_b = gtk_toggle_button_new_with_label("BIN");
  noise_btn_update(GTK_BUTTON(nr_b), app->nr, "NR");   /* label + checked (before wiring) */
  noise_btn_update(GTK_BUTTON(nb_b), app->nb, "NB");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(anf_b), app->anf);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bin_b), app->binaural);
  gtk_widget_set_tooltip_text(bin_b, "Binaural stereo audio");
  g_signal_connect(nr_b,  "clicked", G_CALLBACK(on_nr_clicked),  app);   /* cycles off/NR/NR2 */
  g_signal_connect(nb_b,  "clicked", G_CALLBACK(on_nb_clicked),  app);
  g_signal_connect(anf_b, "toggled", G_CALLBACK(on_anf_toggled), app);
  g_signal_connect(bin_b, "toggled", G_CALLBACK(on_bin_toggled), app);
  gtk_box_append(GTK_BOX(nrbox), nr_b);
  gtk_box_append(GTK_BOX(nrbox), nb_b);
  gtk_box_append(GTK_BOX(nrbox), anf_b);
  gtk_box_append(GTK_BOX(nrbox), bin_b);
  gtk_box_append(GTK_BOX(bar), nrbox);

  /* TX: TUNE keys a carrier at the tune power through the tx_gate safety layer
   * (into a dummy load / matched antenna); MOX keys SSB voice from the mic (F6c) —
   * enabled only in a voice mode (mic open), greyed for CW/data. Both go through
   * the same gate. Drive/PA/antenna live in Preferences → TX (like piHPSDR's PA). */
  GtkWidget *txbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(txbox, "linked");
  app->tune_btn = gtk_toggle_button_new_with_label("TUNE");
  app->mox_btn  = gtk_toggle_button_new_with_label("MOX");
  gtk_widget_add_css_class(app->tune_btn, "txkey");
  gtk_widget_add_css_class(app->mox_btn,  "txkey");
  gtk_widget_set_tooltip_text(app->tune_btn,
      "TUNE: key a carrier at the tune power (dummy load / matched antenna)");
  gtk_widget_set_tooltip_text(app->mox_btn,
      "MOX: key SSB voice from the mic (dummy load / matched antenna)");
  gtk_widget_set_sensitive(app->tune_btn, app->tx_ready);     /* only if the runtime is up */
  gtk_widget_set_sensitive(app->mox_btn,                      /* + only in a voice mode */
      app->tx_ready && mode_is_voice(app->mode));
  g_signal_connect(app->tune_btn, "toggled", G_CALLBACK(on_tx_key_toggled), app);
  g_signal_connect(app->mox_btn,  "toggled", G_CALLBACK(on_tx_key_toggled), app);
  gtk_box_append(GTK_BOX(txbox), app->tune_btn);
  gtk_box_append(GTK_BOX(txbox), app->mox_btn);
  /* MON — TX monitor (self-listen) on/off. Operational control → on the bar
   * next to MOX (level stays in Preferences). Deliberately NOT .txkey: red
   * means "transmitting", and MON never keys. Works for CW sidetone too, so
   * gated on tx_ready only, not on a voice mode. */
  GtkWidget *mon_b = gtk_toggle_button_new_with_label("MON");
  gtk_widget_set_tooltip_text(mon_b,
      "Monitor: hear your own transmission (voice mic / CW sidetone)");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mon_b), app->tx_mon != 0);
  gtk_widget_set_sensitive(mon_b, app->tx_ready);
  g_signal_connect(mon_b, "toggled", G_CALLBACK(on_mon_toggled), app);
  gtk_box_append(GTK_BOX(txbox), mon_b);
  gtk_box_append(GTK_BOX(bar), txbox);

  app->tx_label = gtk_label_new("");   /* only flashes a refusal/trip reason; empty otherwise */
  gtk_widget_add_css_class(app->tx_label, "span");
  gtk_box_append(GTK_BOX(bar), app->tx_label);

  /* AF volume — live. */
  GtkWidget *vol = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -40, 0, 1);
  gtk_range_set_value(GTK_RANGE(vol), app->volume);
  gtk_widget_set_size_request(vol, 130, -1);
  gtk_scale_set_draw_value(GTK_SCALE(vol), FALSE);
  g_signal_connect(vol, "value-changed", G_CALLBACK(on_volume_changed), app);
  gtk_box_append(GTK_BOX(bar), labeled("AF", vol));

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(bar), spacer);

  /* Band buttons — jump the VFO. */
  static const struct { const char *l; int f; } bands[] = {
    {"160", 1840000}, {"80", 3600000}, {"40", 7074000}, {"20", 14074000},
    {"17", 18100000}, {"15", 21074000}, {"12", 24915000}, {"10", 28074000},
    {"6", 50313000},
  };
  GtkWidget *bandbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(bandbox, "linked");
  for (int i = 0; i < (int)(sizeof bands / sizeof bands[0]); i++) {
    GtkWidget *b = gtk_toggle_button_new_with_label(bands[i].l);
    gtk_widget_add_css_class(b, "band");
    g_object_set_data(G_OBJECT(b), "freq", GINT_TO_POINTER(bands[i].f));
    int bi = band_for_freq(bands[i].f);
    if (bi >= 0) { app->band_btns[bi] = b; }
    g_signal_connect(b, "clicked", G_CALLBACK(on_band_clicked), app);
    gtk_box_append(GTK_BOX(bandbox), b);
  }
  update_band_highlight(app);   /* reflect the startup band */
  gtk_box_append(GTK_BOX(bar), bandbox);
  return bar;
}

static void update_span_label(App *app) {
  if (!app->span_label) { return; }
  char buf[40];
  snprintf(buf, sizeof buf, "%.1f kHz  ·  %g×", (double)app->rate / app->zoom / 1000.0, app->zoom);
  gtk_label_set_text(GTK_LABEL(app->span_label), buf);
}

/* Footer supply-voltage readout: green in-band, amber on a mild excursion, red
 * on a fault. Throttled to ~0.01 V changes so it doesn't relayout every frame. */
static void update_volt_label(App *app) {
  if (!app->volt_label || app->supply_v <= 0.0) { return; }
  double v = app->supply_v;
  if (app->volt_shown > 0.0 && fabs(v - app->volt_shown) < 0.01) { return; }
  app->volt_shown = v;
  const char *col = (v < 12.0 || v > 15.0) ? "#f2413d"    /* fault */
                  : (v < 12.8 || v > 14.5) ? "#fbb724"    /* warn  */
                  :                          "#8cf08c";   /* ok    */
  char m[96];
  snprintf(m, sizeof m, "<span foreground='%s'><b>%.2f V</b></span>", col, v);
  gtk_label_set_markup(GTK_LABEL(app->volt_label), m);
}

/* Zoom slider snaps to octave detents (1×,2×,…,128×): continuous re-config while
 * dragging looked rough, so reconfig fires only when the detent actually changes
 * (one cheap wisdom-backed SetAnalyzer per notch). */
static void on_zoom_changed(GtkRange *r, gpointer data) {
  App *app = (App *)data;
  double z = pow(2.0, lround(gtk_range_get_value(r)));   /* nearest octave */
  if (z == app->pending_zoom) { return; }
  app->pending_zoom = z;
  app->zoom_dirty = 1;       /* applied on the next frame tick */
  schedule_save(app);        /* debounced; persists the applied app->zoom */
}

/* Bottom bar (AdwToolbarView bottom slot): view/display controls — zoom for now. */
/* Step selector: set the scroll step, and snap the current frequency to the
 * nearest multiple of the new step so switching steps leaves no sub-step units. */
static void on_step_changed(GtkDropDown *dd, GParamSpec *ps, gpointer data) {
  (void)ps;
  App *app = (App *)data;
  guint i = gtk_drop_down_get_selected(dd);
  if (i >= G_N_ELEMENTS(TUNE_STEPS)) { return; }
  long long step = TUNE_STEPS[i];
  app->tune_step = step;
  long long f = app->freq;
  long long snapped = ((f + step / 2) / step) * step;   /* round to nearest */
  if (snapped < 1) { snapped = step; }
  if (snapped != f) {
    app->freq = snapped;                 /* readout follows on the next tick */
    if (app->engine_ok) { p2_set_frequency(snapped); }
  }
  schedule_save(app);
}

/* Show the drive sliders' value with its unit ("50 W"), not just the number. */
static char *fmt_watts(GtkScale *s, double v, gpointer u) {
  (void)s; (void)u;
  return g_strdup_printf("%.0f W", v);
}
static char *fmt_db(GtkScale *s, double v, gpointer u) {
  (void)s; (void)u;
  return g_strdup_printf("%+.0f dB", v);
}

/* TX drive / tune-drive / antenna — operational, so they live on the footer bar
 * (not buried in Preferences). All live into the runtime + persisted. */
static void on_drive_changed(GtkRange *r, gpointer data) {
  App *app = (App *)data;
  app->tx_drive_w = gtk_range_get_value(r);
  tx_push_cfg(app); schedule_save(app);
}
static void on_tune_drive_changed(GtkRange *r, gpointer data) {
  App *app = (App *)data;
  app->tx_tune_w = gtk_range_get_value(r);
  tx_push_cfg(app); schedule_save(app);
}
static void on_antenna_changed(GtkDropDown *dd, GParamSpec *ps, gpointer data) {
  (void)ps;
  App *app = (App *)data;
  app->tx_antenna = (int)gtk_drop_down_get_selected(dd);
  tx_push_cfg(app); schedule_save(app);
}
static void on_mic_gain_changed(GtkRange *r, gpointer data) {
  App *app = (App *)data;
  app->tx_mic_gain = gtk_range_get_value(r);
  tx_run_set_mic_gain(app->tx_mic_gain);   /* live into the WDSP TX panel (SSB voice) */
  schedule_save(app);
}
static void on_comp_toggled(GtkToggleButton *b, gpointer data) {
  App *app = (App *)data;
  app->tx_comp = gtk_toggle_button_get_active(b) ? 1 : 0;
  tx_apply_proc(app);                   /* live (WDSP setters lock); digi = off */
  schedule_save(app);
}
static void on_comp_level_changed(GtkRange *r, gpointer data) {
  App *app = (App *)data;
  app->tx_comp_db = gtk_range_get_value(r);
  tx_apply_proc(app);
  schedule_save(app);
}

static GtkWidget *build_bottom_controls(App *app) {
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(bar, "controlbar");

  GtkWidget *zoom = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 7.0, 1.0); /* octaves 1x..128x */
  gtk_range_set_round_digits(GTK_RANGE(zoom), 0);          /* snap the handle to detents */
  for (int i = 0; i <= 7; i++) { gtk_scale_add_mark(GTK_SCALE(zoom), i, GTK_POS_BOTTOM, NULL); }
  gtk_range_set_value(GTK_RANGE(zoom), log2(app->zoom));   /* reflect saved zoom (before wiring) */
  gtk_widget_set_size_request(zoom, 200, -1);
  gtk_scale_set_draw_value(GTK_SCALE(zoom), FALSE);
  g_signal_connect(zoom, "value-changed", G_CALLBACK(on_zoom_changed), app);
  app->zoom_scale = zoom;   /* right-drag zoom keeps this in sync */
  gtk_box_append(GTK_BOX(bar), labeled("Zoom", zoom));

  app->span_label = gtk_label_new("");
  gtk_widget_add_css_class(app->span_label, "span");
  gtk_box_append(GTK_BOX(bar), app->span_label);
  update_span_label(app);

  GtkWidget *sdd = gtk_drop_down_new_from_strings(TUNE_STEP_LABELS);
  guint sidx = 0;
  for (guint i = 0; i < G_N_ELEMENTS(TUNE_STEPS); i++) {
    if (TUNE_STEPS[i] == app->tune_step) { sidx = i; break; }
  }
  gtk_drop_down_set_selected(GTK_DROP_DOWN(sdd), sidx);  /* set before wiring: no retune on build */
  g_signal_connect(sdd, "notify::selected", G_CALLBACK(on_step_changed), app);
  app->step_dd = sdd;
  gtk_box_append(GTK_BOX(bar), labeled("Step", sdd));

  /* TX drive (MOX/voice, W) + tune drive (W) + antenna — operational TX controls. */
  GtkWidget *drv = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
  gtk_range_set_value(GTK_RANGE(drv), app->tx_drive_w);        /* before wiring: no send */
  gtk_widget_set_size_request(drv, 130, -1);
  gtk_scale_set_draw_value(GTK_SCALE(drv), TRUE);
  gtk_scale_set_value_pos(GTK_SCALE(drv), GTK_POS_RIGHT);
  gtk_scale_set_format_value_func(GTK_SCALE(drv), fmt_watts, NULL, NULL);
  g_signal_connect(drv, "value-changed", G_CALLBACK(on_drive_changed), app);
  gtk_box_append(GTK_BOX(bar), labeled("Drive", drv));

  GtkWidget *tdrv = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
  gtk_range_set_value(GTK_RANGE(tdrv), app->tx_tune_w);
  gtk_widget_set_size_request(tdrv, 120, -1);
  gtk_scale_set_draw_value(GTK_SCALE(tdrv), TRUE);
  gtk_scale_set_value_pos(GTK_SCALE(tdrv), GTK_POS_RIGHT);
  gtk_scale_set_format_value_func(GTK_SCALE(tdrv), fmt_watts, NULL, NULL);
  g_signal_connect(tdrv, "value-changed", G_CALLBACK(on_tune_drive_changed), app);
  gtk_box_append(GTK_BOX(bar), labeled("Tune", tdrv));

  GtkWidget *antdd = gtk_drop_down_new_from_strings((const char *[]){"ANT 1","ANT 2","ANT 3", NULL});
  gtk_drop_down_set_selected(GTK_DROP_DOWN(antdd),
      (app->tx_antenna >= 0 && app->tx_antenna < 3) ? (guint)app->tx_antenna : 0);
  g_signal_connect(antdd, "notify::selected", G_CALLBACK(on_antenna_changed), app);
  gtk_box_append(GTK_BOX(bar), labeled("Ant", antdd));

  /* Mic gain (SSB voice) — operational, so it lives on the footer next to Drive.
   * Live into the WDSP TX panel + persisted; only bites while MOX-keyed (F6c-3). */
  GtkWidget *micg = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, MIC_GAIN_MIN, MIC_GAIN_MAX, 1);
  gtk_range_set_value(GTK_RANGE(micg), app->tx_mic_gain);      /* before wiring: no send */
  gtk_widget_set_size_request(micg, 120, -1);
  gtk_scale_set_draw_value(GTK_SCALE(micg), TRUE);
  gtk_scale_set_value_pos(GTK_SCALE(micg), GTK_POS_RIGHT);
  gtk_scale_set_format_value_func(GTK_SCALE(micg), fmt_db, NULL, NULL);
  g_signal_connect(micg, "value-changed", G_CALLBACK(on_mic_gain_changed), app);
  gtk_box_append(GTK_BOX(bar), labeled("Mic", micg));

  /* Speech processor (PROC): toggle + compression level. Off = the chain has NO
   * makeup gain (voice PEP = mic peaks × mic gain, the ALC only limits); on = the
   * WDSP auto-leveler (+8 dB) + COMP at this level (piHPSDR tx_set_compressor).
   * 0 dB is meaningful — leveler only. */
  GtkWidget *procbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *proct = gtk_toggle_button_new_with_label("PROC");
  gtk_widget_add_css_class(proct, "mode");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(proct), app->tx_comp != 0);
  gtk_widget_set_tooltip_text(proct,
      "Speech processor: auto-leveler (+8 dB) + compression (SSB voice)");
  g_signal_connect(proct, "toggled", G_CALLBACK(on_comp_toggled), app);
  gtk_box_append(GTK_BOX(procbox), proct);
  GtkWidget *procl = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, COMP_DB_MIN, COMP_DB_MAX, 1);
  gtk_range_set_value(GTK_RANGE(procl), app->tx_comp_db);      /* before wiring: no send */
  gtk_widget_set_size_request(procl, 100, -1);
  gtk_scale_set_draw_value(GTK_SCALE(procl), TRUE);
  gtk_scale_set_value_pos(GTK_SCALE(procl), GTK_POS_RIGHT);
  gtk_scale_set_format_value_func(GTK_SCALE(procl), fmt_db, NULL, NULL);
  g_signal_connect(procl, "value-changed", G_CALLBACK(on_comp_level_changed), app);
  gtk_box_append(GTK_BOX(procbox), procl);
  gtk_box_append(GTK_BOX(bar), labeled("Proc", procbox));

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(bar), spacer);

  /* Supply-voltage readout, right-aligned — telemetry lives on the footer. */
  app->volt_label = gtk_label_new("—");
  gtk_widget_add_css_class(app->volt_label, "span");
  gtk_box_append(GTK_BOX(bar), labeled("Supply", app->volt_label));
  return bar;
}

static void css_load(void) {
  GtkCssProvider *p = gtk_css_provider_new();
  const char *c =
    ".controlbar { padding: 7px 10px; }"
    ".dim { opacity: 0.6; font-size: 12px; }"
    ".span { font-family: \"Adwaita Mono\"; opacity: 0.75; }"
    "button.mode, button.band { min-width: 30px; padding-left: 7px; padding-right: 7px; }"
    "button.mode:checked, button.band:checked { background: #1d6fa5; color: #fff; }"
    "button.txkey { min-width: 42px; padding-left: 9px; padding-right: 9px; }"
    "button.txkey:checked { background: #c8321e; color: #fff; }";
  gtk_css_provider_load_from_string(p, c);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
      GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* ---- preferences dialog (AdwPreferencesDialog) --------------------------- */

static const int PREF_RATES[] = {48000, 96000, 192000, 384000, 768000, 1536000};

static char **g_orig_argv;   /* main()'s argv — for the in-app restart */

/* Restart the app in place: spawn a detached relauncher that waits ~5 s (the G1
 * needs that long after we disconnect before it answers discovery again — the
 * restart-pause gotcha) then re-execs this binary with the same args, and quit so
 * main()'s normal cleanup parks the RF path and saves settings. Env is inherited. */
static void do_restart(App *app) {
  (void)app;
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n <= 0) { return; }
  exe[n] = '\0';
  GPtrArray *a = g_ptr_array_new();
  g_ptr_array_add(a, (gpointer)"/bin/sh");
  g_ptr_array_add(a, (gpointer)"-c");
  g_ptr_array_add(a, (gpointer)"sleep 5; exec \"$@\"");
  g_ptr_array_add(a, (gpointer)"sh");        /* $0 for the -c script */
  g_ptr_array_add(a, exe);                   /* $1 → the binary, first of "$@" */
  for (int i = 1; g_orig_argv && g_orig_argv[i]; i++) { g_ptr_array_add(a, g_orig_argv[i]); }
  g_ptr_array_add(a, NULL);
  GError *err = NULL;
  g_spawn_async(NULL, (char **)a->pdata, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &err);
  g_ptr_array_free(a, TRUE);
  if (err) { g_warning("restart: spawn failed: %s", err->message); g_error_free(err); return; }
  g_application_quit(G_APPLICATION(g_application_get_default()));   /* → main() cleanup */
}
static void on_restart_toast_clicked(AdwToast *t, gpointer data) {
  (void)t; do_restart((App *)data);
}
static void on_restart_toast_dismissed(AdwToast *t, gpointer data) {
  (void)t; ((App *)data)->restart_toast_shown = 0;
}
/* Show the non-modal restart toast on the MAIN window. Deduped: only one lives at
 * a time (cleared when dismissed or acted on). Shown when the Preferences dialog
 * CLOSES — a toast added while the modal dialog is up sits behind it, unclickable. */
static void show_restart_toast(App *app) {
  if (!app->toast_overlay || app->restart_toast_shown) { return; }
  app->restart_toast_shown = 1;
  AdwToast *t = adw_toast_new("Some changes apply on restart");
  adw_toast_set_button_label(t, "Restart now");
  adw_toast_set_timeout(t, 0);              /* stay until acted on / dismissed */
  g_signal_connect(t, "button-clicked", G_CALLBACK(on_restart_toast_clicked), app);
  g_signal_connect(t, "dismissed",      G_CALLBACK(on_restart_toast_dismissed), app);
  adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(app->toast_overlay), t);
}
/* A restart-to-apply setting changed: just remember it; the toast pops when the
 * Preferences dialog closes (see on_prefs_closed). */
static void restart_hint(App *app) { app->restart_pending = 1; }
/* Preferences dialog closed → if a restart-to-apply setting changed, offer it now. */
static void on_prefs_closed(AdwDialog *dlg, gpointer data) {
  (void)dlg; App *app = (App *)data;
  if (app->restart_pending) { app->restart_pending = 0; show_restart_toast(app); }
}

static void on_pref_fps(AdwSpinRow *r, GParamSpec *ps, gpointer data) {
  (void)ps;
  App *app = (App *)data;
  app->fps = (int)adw_spin_row_get_value(r);
  analyzer_set_fps(app->fps);   /* live */
  schedule_save(app);
}
static void on_pref_gain(AdwSpinRow *r, GParamSpec *ps, gpointer data) {
  (void)ps;
  App *app = (App *)data;
  app->gain = adw_spin_row_get_value(r);
  demod_set_gain(app->gain);    /* live */
  schedule_save(app);
}
static void on_pref_latency(AdwSpinRow *r, GParamSpec *ps, gpointer data) {
  (void)ps;
  App *app = (App *)data;
  app->latency = (int)adw_spin_row_get_value(r);   /* applied on restart */
  schedule_save(app); restart_hint(app);
}
static void on_pref_pan_high(AdwSpinRow *r, GParamSpec *ps, gpointer data) {
  (void)ps;
  App *app = (App *)data;
  double v = adw_spin_row_get_value(r);            /* dB axis top; keep >= MIN span */
  if (v > app->pan_low + PAN_RANGE_MIN && v <= PAN_DBM_CEIL) {
    app->pan_high = v; pan_store_band(app); schedule_save(app); gtk_widget_queue_draw(app->area);
  }
}
static void on_pref_pan_low(AdwSpinRow *r, GParamSpec *ps, gpointer data) {
  (void)ps;
  App *app = (App *)data;
  double v = adw_spin_row_get_value(r);            /* dB axis bottom */
  if (v < app->pan_high - PAN_RANGE_MIN && v >= PAN_DBM_FLOOR) {
    app->pan_low = v; pan_store_band(app); schedule_save(app); gtk_widget_queue_draw(app->area);
  }
}
static void on_pref_db_grid(AdwSwitchRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->show_db_grid = adw_switch_row_get_active(r);
  schedule_save(app); gtk_widget_queue_draw(app->area);
}
static void on_pref_db_scale(AdwSwitchRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->show_db_scale = adw_switch_row_get_active(r);
  schedule_save(app); gtk_widget_queue_draw(app->area);
}
static void on_pref_freq_grid(AdwSwitchRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->show_freq_grid = adw_switch_row_get_active(r);
  schedule_save(app); gtk_widget_queue_draw(app->area);
}
static void on_pref_freq_scale(AdwSwitchRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->show_freq_scale = adw_switch_row_get_active(r);
  schedule_save(app); gtk_widget_queue_draw(app->area);
}
static void on_pref_filter_wf(AdwSwitchRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->show_filter_wf = adw_switch_row_get_active(r);
  schedule_save(app); gtk_widget_queue_draw(app->area);
}
static void on_pref_filter_op(GtkRange *r, gpointer data) {
  App *app = (App *)data;
  app->filter_op = (int)gtk_range_get_value(r);
  schedule_save(app); gtk_widget_queue_draw(app->area);
}
static void on_pref_auto_level(AdwSwitchRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->auto_level = adw_switch_row_get_active(r);
  schedule_save(app); gtk_widget_queue_draw(app->area);
}
static void on_pref_avg_spec(AdwSpinRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->avg_spec_ms = (int)adw_spin_row_get_value(r);   /* live next frame */
  schedule_save(app);
}
static void on_pref_avg_wf(AdwSpinRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->avg_wf_ms = (int)adw_spin_row_get_value(r);
  schedule_save(app);
}
static void on_pref_palette(AdwComboRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->palette = (int)adw_combo_row_get_selected(r);
  waterfall_set_palette(app->wf, app->palette);   /* recolours the whole waterfall live */
  waterfall_set_palette(app->tx_wf, app->palette);
  schedule_save(app);
  if (app->area) { gtk_widget_queue_draw(app->area); }   /* repaint the spectrum too */
}
static void on_pref_region(AdwComboRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->bp_region = (int)adw_combo_row_get_selected(r);
  schedule_save(app);
  if (app->area) { gtk_widget_queue_draw(app->area); }
}
static void on_pref_country(AdwComboRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->bp_country = (int)adw_combo_row_get_selected(r);
  schedule_save(app);
  if (app->area) { gtk_widget_queue_draw(app->area); }
}
static void on_pref_band_edges(AdwSwitchRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->show_band_edges = adw_switch_row_get_active(r);
  schedule_save(app);
  if (app->area) { gtk_widget_queue_draw(app->area); }
}
static void on_pref_spots(AdwSwitchRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->show_spots = adw_switch_row_get_active(r);
  schedule_save(app);
  if (app->area) { gtk_widget_queue_draw(app->area); }
}
static void on_pref_rate(AdwComboRow *r, GParamSpec *ps, gpointer data) {
  (void)ps;
  App *app = (App *)data;
  guint i = adw_combo_row_get_selected(r);
  if (i < G_N_ELEMENTS(PREF_RATES)) { app->rate = PREF_RATES[i]; schedule_save(app); restart_hint(app); }  /* restart */
}
static void on_pref_ip(GtkEditable *e, gpointer data) {
  App *app = (App *)data;
  g_strlcpy(app->radio_ip, gtk_editable_get_text(e), sizeof(app->radio_ip));   /* restart */
  schedule_save(app); restart_hint(app);
}

/* Audio device + sample-rate selection (restart-to-apply, like the IQ rate). The
 * RX audio rate is the sample rate (Nyquist ceiling), NOT the audio bandwidth —
 * that stays the filter's job. Capped to the IQ rate at apply time. */
static const int AUDIO_RATES[] = {48000, 96000, 192000};
static void on_pref_audio_rate(AdwComboRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  guint i = adw_combo_row_get_selected(r);
  if (i < G_N_ELEMENTS(AUDIO_RATES)) { app->audio_rate = AUDIO_RATES[i]; schedule_save(app); restart_hint(app); }
}
static void on_pref_audio_device(AdwComboRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  guint i = adw_combo_row_get_selected(r);   /* 0 = "Default"; 1.. = audio_sinks[i-1] */
  if (i == 0) { app->audio_device[0] = '\0'; }
  else if ((int)i - 1 < app->audio_nsink) {
    g_strlcpy(app->audio_device, app->audio_sinks[i - 1].name, sizeof app->audio_device);
  }
  schedule_save(app); restart_hint(app);
}
static void on_pref_mic_device(AdwComboRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  guint i = adw_combo_row_get_selected(r);   /* 0 = "Default"; 1.. = mic_srcs[i-1] */
  if (i == 0) { app->mic_device[0] = '\0'; }
  else if ((int)i - 1 < app->mic_nsrc) {
    g_strlcpy(app->mic_device, app->mic_srcs[i - 1].name, sizeof app->mic_device);
  }
  schedule_save(app); restart_hint(app);
}

/* TX preferences (F6a) — all live into the runtime (tx_push_cfg) and persisted. */
static void on_pref_tx_pa(AdwSwitchRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->tx_pa_enabled = adw_switch_row_get_active(r);
  tx_push_cfg(app); schedule_save(app);
}
static void on_pref_tx_swr(AdwSpinRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->tx_swr_alarm = adw_spin_row_get_value(r);
  tx_push_cfg(app); schedule_save(app);
}
/* Mic noise gate (DEXP) — tames the PROC leveler pumping room noise up in the
 * gaps between words. Threshold is on the post-mic-gain signal. Applies live. */
static void on_pref_tx_gate(AdwSwitchRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->tx_gate = adw_switch_row_get_active(r) ? 1 : 0;
  tx_apply_proc(app);                   /* digi = gate stays off */
  schedule_save(app);
}
static void on_pref_tx_gate_db(GtkRange *r, gpointer data) {
  App *app = (App *)data;
  app->tx_gate_db = gtk_range_get_value(r);
  tx_apply_proc(app);
  schedule_save(app);
}
static void on_pref_tx_mon_db(GtkRange *r, gpointer data) {
  App *app = (App *)data;
  app->tx_mon_db = gtk_range_get_value(r);
  demod_set_monitor_gain(app->tx_mon_db);
  schedule_save(app);
}

/* CW (F6d-1c): keyer speed, sidetone + break-in hang → the TX runtime, live.
 * Weight 50 / ramp 9 ms stay fixed (the piHPSDR-validated envelope shape). */
static void cw_push(App *app) {
  tx_run_set_cw(app->cw_wpm, 50.0, 9.0, app->cw_hang);
  tx_run_set_sidetone(app->cw_pitch, app->cw_st_db);
  demod_set_cw_pitch(app->cw_pitch);   /* RX CW BFO offset = the same pitch */
}
static void on_pref_cw_wpm(AdwSpinRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->cw_wpm = (int)adw_spin_row_get_value(r);
  cw_push(app); schedule_save(app);
}
static void on_pref_cw_pitch(GtkRange *r, gpointer data) {
  App *app = (App *)data;
  app->cw_pitch = (int)gtk_range_get_value(r);
  cw_push(app); schedule_save(app);
}
static void on_pref_cw_st_db(GtkRange *r, gpointer data) {
  App *app = (App *)data;
  app->cw_st_db = gtk_range_get_value(r);
  cw_push(app); schedule_save(app);
}
static void on_pref_cw_hang(AdwSpinRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->cw_hang = (int)adw_spin_row_get_value(r);
  cw_push(app); schedule_save(app);
}

/* ---- TCI server glue (F6d-2a) --------------------------------------------
 * The ops run on the GTK main thread (tci_server dispatches there) and reuse
 * the SAME paths as the on-screen controls — keying lands in tx_gate via the
 * MOX/TUNE toggle handlers, never directly. */
static App *tci_app;
static int  tci_muted;

static long long tci_get_freq(void) { return tci_app->freq; }
static void tci_set_freq(long long f) {
  if (f < 1) { return; }
  tci_app->freq = f;                 /* same as the tuning wheel */
  p2_set_frequency(f);
  schedule_save(tci_app);
}
static const char *tci_get_mode(void) {
  switch (tci_app->mode) {
    case DEMOD_LSB:  return "lsb";
    case DEMOD_USB:  return "usb";
    case DEMOD_CWL:  return "cwl";
    case DEMOD_CWU:  return "cw";    /* ExpertSDR calls the USB-side CW "cw" */
    case DEMOD_AM:   return "am";
    case DEMOD_DIGU: return "digu";
    case DEMOD_DIGL: return "digl";
    default:         return "usb";
  }
}
static int tci_set_mode(const char *m) {
  int mode;
  if      (strcmp(m, "lsb") == 0) { mode = DEMOD_LSB; }
  else if (strcmp(m, "usb") == 0) { mode = DEMOD_USB; }
  else if (strcmp(m, "cw") == 0 || strcmp(m, "cwu") == 0) { mode = DEMOD_CWU; }
  else if (strcmp(m, "cwl") == 0) { mode = DEMOD_CWL; }
  else if (strcmp(m, "am") == 0)  { mode = DEMOD_AM; }
  else if (strcmp(m, "digu") == 0) { mode = DEMOD_DIGU; }
  else if (strcmp(m, "digl") == 0) { mode = DEMOD_DIGL; }
  else { return -1; }
  if (tci_app->mode_btns[mode]) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tci_app->mode_btns[mode]), TRUE);
  }
  return 0;
}
static void tci_get_filter(int *lo, int *hi) { *lo = tci_app->flo; *hi = tci_app->fhi; }
static void tci_set_filter(int lo, int hi) {
  if (hi <= lo) { return; }
  tci_app->flo = lo;
  tci_app->fhi = hi;
  demod_set_passband(lo, hi);
  gtk_widget_queue_draw(tci_app->area);
}
static double tci_get_drive(void) { return tci_app->tx_drive_w; }
static void tci_set_drive(double v) {
  tci_app->tx_drive_w = v;
  tx_push_cfg(tci_app);
  schedule_save(tci_app);
}
static double tci_get_tune_drive(void) { return tci_app->tx_tune_w; }
static void tci_set_tune_drive(double v) {
  tci_app->tx_tune_w = v;
  tx_push_cfg(tci_app);
  schedule_save(tci_app);
}
static int tci_get_trx(void) {
  return tci_app->mox_btn &&
         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(tci_app->mox_btn));
}
static int tci_set_trx(int on) {
  if (!tci_app->tx_ready || !tci_app->mox_btn) { return -1; }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tci_app->mox_btn), on ? TRUE : FALSE);
  return 0;
}
static int tci_get_tune(void) {
  return tci_app->tune_btn &&
         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(tci_app->tune_btn));
}
static int tci_set_tune(int on) {
  if (!tci_app->tx_ready || !tci_app->tune_btn) { return -1; }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tci_app->tune_btn), on ? TRUE : FALSE);
  return 0;
}
static double tci_get_volume(void) { return tci_app->volume; }
static void tci_set_volume(double db) {
  if (db < -60.0) { db = -60.0; }
  if (db > 0.0)   { db = 0.0; }
  tci_app->volume = db;
  if (!tci_muted) { demod_set_volume(db); }
  schedule_save(tci_app);
}
static int tci_get_mute(void) { return tci_muted; }
static void tci_set_mute(int on) {
  tci_muted = on ? 1 : 0;
  demod_set_volume(tci_muted ? -60.0 : tci_app->volume);
}
static int tci_get_cw_speed(void) { return tci_app->cw_wpm; }
static void tci_set_cw_speed(int wpm) {
  if (wpm < 5)  { wpm = 5; }
  if (wpm > 60) { wpm = 60; }
  tci_app->cw_wpm = wpm;
  cw_push(tci_app);
  schedule_save(tci_app);
}
static void tci_cw_send(const char *t) {
  /* Queue only in a CW mode — otherwise the text would sit in the generator
   * and key unexpectedly when the operator later switches to CW. */
  if (tci_app->tx_ready &&
      (tci_app->mode == DEMOD_CWL || tci_app->mode == DEMOD_CWU)) {
    tx_run_cw_send(t);
  }
}
static void tci_cw_stop(void) { if (tci_app->tx_ready) { tx_run_cw_abort(); } }
static int tci_get_tx_enable(void) { return tci_app->tx_ready && tci_app->tx_pa_enabled; }
static int tci_get_rate(void) { return tci_app->rate; }
static double tci_get_smeter(void) {
  return tci_app->audio_ok ? demod_s_meter() : -200.0;
}
static int tci_set_tx_src(int on) {
  if (!tci_app->tx_ready) { return -1; }
  tx_run_set_ext_source(on);
  return 0;
}
static void tci_tx_audio_push(const float *m, int n) { tx_run_ext_push(m, n); }
static void tci_iq_rate_changed(int rate) {
  tci_app->tci_iq_rate = rate;
  schedule_save(tci_app);
}
/* DX spots from TCI clients (F6d-2e). Main thread (tci_exec dispatch). */
static void tci_spot_add(const char *call, const char *mode, long long hz,
                         unsigned argb, const char *text) {
  (void)text;
  App *app = tci_app;
  if (!call || !*call || hz <= 0) { return; }
  struct spot *sl = NULL;
  for (int i = 0; i < app->nspots; i++) {          /* re-announce → refresh */
    if (strcmp(app->spots[i].call, call) == 0) { sl = &app->spots[i]; break; }
  }
  if (!sl) {
    if (app->nspots < MAX_SPOTS) { sl = &app->spots[app->nspots++]; }
    else {                                          /* full → evict the oldest */
      sl = &app->spots[0];
      for (int i = 1; i < MAX_SPOTS; i++) {
        if (app->spots[i].ts < sl->ts) { sl = &app->spots[i]; }
      }
    }
    memset(sl, 0, sizeof *sl);
  }
  g_strlcpy(sl->call, call, sizeof sl->call);
  g_strlcpy(sl->mode, mode ? mode : "", sizeof sl->mode);
  sl->hz = hz;
  sl->argb = argb;
  sl->ts = g_get_monotonic_time();
  if (app->show_spots && app->area) { gtk_widget_queue_draw(app->area); }
}
static void tci_spot_del(const char *call) {
  App *app = tci_app;
  if (!call) { return; }
  for (int i = 0; i < app->nspots; i++) {
    if (strcmp(app->spots[i].call, call) == 0) {
      app->spots[i] = app->spots[--app->nspots];   /* order restored at draw */
      if (app->area) { gtk_widget_queue_draw(app->area); }
      return;
    }
  }
}
static void tci_spot_clear(void) {
  tci_app->nspots = 0;
  if (tci_app->area) { gtk_widget_queue_draw(tci_app->area); }
}
static void tci_get_tx_meters(double *mic_db, double *rms_w, double *pep_w, double *swr) {
  tx_run_status ts;
  memset(&ts, 0, sizeof(ts));
  if (tci_app->tx_ready) { tx_run_get_status(&ts); }
  *mic_db = ts.mic_pk;
  *rms_w  = ts.fwd_w;
  *pep_w  = ts.fwd_pep_w;
  *swr    = ts.swr;
}

static const TciOps TCI_OPS = {
  tci_get_freq, tci_set_freq, tci_get_mode, tci_set_mode,
  tci_get_filter, tci_set_filter, tci_get_drive, tci_set_drive,
  tci_get_tune_drive, tci_set_tune_drive, tci_get_trx, tci_set_trx,
  tci_get_tune, tci_set_tune, tci_get_volume, tci_set_volume,
  tci_get_mute, tci_set_mute, tci_get_cw_speed, tci_set_cw_speed,
  tci_cw_send, tci_cw_stop, tci_get_tx_enable, tci_get_rate,
  tci_get_smeter, tci_get_tx_meters, tci_set_tx_src, tci_tx_audio_push,
  tci_iq_rate_changed, tci_spot_add, tci_spot_del, tci_spot_clear,
};

/* Start/stop the TCI server to match app->tci_enable (prefs + startup). */
static void tci_apply(App *app) {
  tci_app = app;
  if (app->tci_enable && !tci_server_running()) {
    if (tci_server_start(app->tci_port, &TCI_OPS) != 0) {
      fprintf(stderr, "tci: start failed on port %d\n", app->tci_port);
    } else {
      tci_server_set_iq_rate(app->tci_iq_rate);     /* persisted device IQ rate */
      demod_set_audio_tap(tci_server_audio_push);   /* RX audio → TCI (F6d-2b) */
      tx_run_set_ext_notify(tci_server_tx_chrono);  /* TX pacing → TCI (F6d-2c) */
    }
  } else if (!app->tci_enable && tci_server_running()) {
    demod_set_audio_tap(NULL);
    tx_run_set_ext_notify(NULL);
    tx_run_set_ext_source(0);                       /* back to the mic         */
    tci_server_stop();
  }
}

static void on_pref_tci_enable(AdwSwitchRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->tci_enable = adw_switch_row_get_active(r);
  tci_apply(app);
  schedule_save(app);
}
static void on_pref_tci_port(AdwSpinRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->tci_port = (int)adw_spin_row_get_value(r);
  if (tci_server_running()) {          /* live port move: restart the server */
    tci_server_stop();
    tci_apply(app);
  }
  schedule_save(app);
}

/* Live "who is connected" row on the TCI page (1 s refresh while it exists). */
static gboolean tci_clients_tick(gpointer data) {
  App *app = (App *)data;
  if (!app->tci_client_row) { app->tci_timer = 0; return G_SOURCE_REMOVE; }
  if (!tci_server_running()) {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(app->tci_client_row), "Server is off");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(app->tci_client_row), "");
    return G_SOURCE_CONTINUE;
  }
  int n = tci_server_clients();
  char t[48];
  if (n == 0) { g_strlcpy(t, "No clients connected", sizeof(t)); }
  else { snprintf(t, sizeof(t), "%d client%s connected", n, n == 1 ? "" : "s"); }
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(app->tci_client_row), t);
  GString *s = g_string_new(NULL);
  for (int i = 0; i < TCI_SERVER_MAX_CLIENTS; i++) {
    char info[192];
    if (tci_server_client_info(i, info, sizeof(info))) {
      if (s->len) { g_string_append_c(s, '\n'); }
      g_string_append(s, info);
    }
  }
  adw_action_row_set_subtitle(ADW_ACTION_ROW(app->tci_client_row), s->str);
  g_string_free(s, TRUE);
  return G_SOURCE_CONTINUE;
}
static void on_tci_row_destroy(GtkWidget *w, gpointer data) {
  (void)w;
  ((App *)data)->tci_client_row = NULL;
}
/* TX audio filter edges (Hz). Pushed via tx_push_cfg; applies live even while
 * keyed (gate_slot re-asserts the passband each slot, WDSP no-ops if unchanged). */
static void on_pref_tx_flo(AdwSpinRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->tx_flo = adw_spin_row_get_value(r);
  tx_push_cfg(app); schedule_save(app);
}
static void on_pref_tx_fhi(AdwSpinRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  app->tx_fhi = adw_spin_row_get_value(r);
  tx_push_cfg(app); schedule_save(app);
}
/* Per-band PA calibration (F6b): band index carried on the row. Clamped to the
 * safe [38.8,70.0] range; re-pushed so the current band's drive byte updates. */
static void on_pref_pacal(AdwSpinRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "band"));
  if (i < 0 || i >= NBANDS) { return; }
  app->band_pacal[i] = pacal_clamp(adw_spin_row_get_value(r));
  tx_push_cfg(app); schedule_save(app);
}
/* Wattmeter-trim point (F6b): point index 1..10 carried on the row (0 is fixed). */
static void on_pref_patrim(AdwSpinRow *r, GParamSpec *ps, gpointer data) {
  (void)ps; App *app = (App *)data;
  int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "pt"));
  if (i < 1 || i > 10) { return; }
  double v = adw_spin_row_get_value(r);
  app->pa_trim[i] = v < 0.0 ? 0.0 : v;
  tx_push_cfg(app); schedule_save(app);
}
static void on_pref_patrim_reset(GtkButton *b, gpointer data) {
  (void)b; App *app = (App *)data;
  for (int i = 0; i < 11; i++) {
    app->pa_trim[i] = i * PATRIM_STEP;                       /* identity curve */
    if (i >= 1 && app->patrim_spin[i]) {
      adw_spin_row_set_value(ADW_SPIN_ROW(app->patrim_spin[i]), app->pa_trim[i]);
    }
  }
  tx_push_cfg(app); schedule_save(app);
}

static GtkWidget *pref_spin(const char *title, const char *subtitle,
                            double lo, double hi, double val, GCallback cb, App *app) {
  GtkAdjustment *a = gtk_adjustment_new(val, lo, hi, 1, 1, 0);
  GtkWidget *row = g_object_new(ADW_TYPE_SPIN_ROW, "title", title, "subtitle", subtitle,
                                "adjustment", a, "digits", 0, NULL);
  g_signal_connect(row, "notify::value", cb, app);   /* connect after ctor → no spurious fire */
  return row;
}

static GtkWidget *pref_switch(const char *title, const char *subtitle,
                              int active, GCallback cb, App *app) {
  GtkWidget *row = g_object_new(ADW_TYPE_SWITCH_ROW, "title", title, "subtitle", subtitle,
                                "active", active ? TRUE : FALSE, NULL);
  g_signal_connect(row, "notify::active", cb, app);
  return row;
}

/* Live value readout on a slider row ("700 Hz", "-20 dB") — the label carries
 * its printf format as object data; connected before the user callback. */
static void pref_slider_lbl(GtkRange *r, gpointer data) {
  GtkLabel *lbl = GTK_LABEL(data);
  const char *fmt = g_object_get_data(G_OBJECT(lbl), "fmt");
  char buf[32];
  snprintf(buf, sizeof buf, fmt, gtk_range_get_value(r));
  gtk_label_set_text(lbl, buf);
}

static GtkWidget *pref_slider(const char *title, const char *subtitle,
                              double lo, double hi, double val, const char *fmt,
                              GCallback cb, App *app) {
  GtkWidget *row = g_object_new(ADW_TYPE_ACTION_ROW, "title", title, "subtitle", subtitle, NULL);
  GtkWidget *sc = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, lo, hi, 1);
  gtk_range_set_value(GTK_RANGE(sc), val);
  gtk_widget_set_size_request(sc, 170, -1);
  gtk_widget_set_valign(sc, GTK_ALIGN_CENTER);
  gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
  GtkWidget *lbl = gtk_label_new(NULL);
  gtk_label_set_width_chars(GTK_LABEL(lbl), 7);
  gtk_label_set_xalign(GTK_LABEL(lbl), 1.0);
  gtk_widget_add_css_class(lbl, "numeric");   /* tabular figures — no jitter */
  g_object_set_data_full(G_OBJECT(lbl), "fmt", g_strdup(fmt), g_free);
  pref_slider_lbl(GTK_RANGE(sc), lbl);        /* initial text */
  g_signal_connect(sc, "value-changed", G_CALLBACK(pref_slider_lbl), lbl);
  g_signal_connect(sc, "value-changed", cb, app);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), sc);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), lbl);
  return row;
}

/* One per-band PA-calibration spin row (dB), band index stored for the callback. */
static GtkWidget *pacal_row(App *app, int band) {
  GtkAdjustment *a = gtk_adjustment_new(pacal_clamp(app->band_pacal[band]),
                                        PACAL_MIN, PACAL_MAX, 0.1, 1.0, 0);
  char sub[48];
  snprintf(sub, sizeof sub, "%lld–%lld MHz",
           BANDS[band].lo / 1000000, BANDS[band].hi / 1000000);
  GtkWidget *row = g_object_new(ADW_TYPE_SPIN_ROW, "title", BANDS[band].key,
                                "subtitle", sub, "adjustment", a, "digits", 1, NULL);
  g_object_set_data(G_OBJECT(row), "band", GINT_TO_POINTER(band));
  g_signal_connect(row, "notify::value", G_CALLBACK(on_pref_pacal), app);
  app->pacal_spin[band] = row;
  return row;
}

/* One wattmeter-trim spin row: title = true watts (fixed grid), value = the raw
 * meter reading at that power (the adjustable breakpoint). Point index 1..10. */
static GtkWidget *patrim_row(App *app, int i) {
  GtkAdjustment *a = gtk_adjustment_new(app->pa_trim[i], 0.0, 300.0, 0.5, 5.0, 0);
  char title[16];
  snprintf(title, sizeof title, "%d W", (int)(i * PATRIM_STEP));
  GtkWidget *row = g_object_new(ADW_TYPE_SPIN_ROW, "title", title,
                                "subtitle", "meter reading at this true power",
                                "adjustment", a, "digits", 1, NULL);
  g_object_set_data(G_OBJECT(row), "pt", GINT_TO_POINTER(i));
  g_signal_connect(row, "notify::value", G_CALLBACK(on_pref_patrim), app);
  app->patrim_spin[i] = row;
  return row;
}

static AdwDialog *build_prefs(App *app) {
  AdwPreferencesDialog *dlg = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());

  /* Radio — all restart-to-apply. */
  AdwPreferencesPage *p = ADW_PREFERENCES_PAGE(g_object_new(ADW_TYPE_PREFERENCES_PAGE,
      "title", "Radio", "icon-name", "network-workgroup-symbolic", NULL));
  AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(g_object_new(ADW_TYPE_PREFERENCES_GROUP,
      "title", "Connection", "description", "Applies on restart", NULL));
  GtkWidget *ip = g_object_new(ADW_TYPE_ENTRY_ROW, "title",
      "Radio IP address (empty = pick at startup) · restart to apply", NULL);
  gtk_editable_set_text(GTK_EDITABLE(ip), app->radio_ip);
  g_signal_connect(ip, "changed", G_CALLBACK(on_pref_ip), app);
  adw_preferences_group_add(g, ip);
  GtkStringList *rm = gtk_string_list_new((const char *[]){"48 kHz","96 kHz","192 kHz","384 kHz","768 kHz","1536 kHz", NULL});
  guint ri = 2;
  for (guint i = 0; i < G_N_ELEMENTS(PREF_RATES); i++) { if (PREF_RATES[i] == app->rate) { ri = i; } }
  GtkWidget *rate = g_object_new(ADW_TYPE_COMBO_ROW, "title", "Sample rate",
      "subtitle", "IQ span · restart to apply", "model", rm, "selected", ri, NULL);
  g_signal_connect(rate, "notify::selected", G_CALLBACK(on_pref_rate), app);
  adw_preferences_group_add(g, rate);
  adw_preferences_page_add(p, g);

  /* TX — keying, power + safety (F6a). Lives on the Radio page (like piHPSDR's
   * PA-enable in the Radio menu) so the page switcher stays in the header rather
   * than dropping to a bottom bar. PA-enable persists across restarts. */
  g = ADW_PREFERENCES_GROUP(g_object_new(ADW_TYPE_PREFERENCES_GROUP, "title", "Transmit",
      "description", "TUNE keys into a dummy load / matched antenna. MOX/voice needs the mic path (F6c).", NULL));
  adw_preferences_group_add(g, pref_switch("PA enable", "Off = dry key only (T/R relay, no RF)",
      app->tx_pa_enabled, G_CALLBACK(on_pref_tx_pa), app));
  adw_preferences_group_add(g, pref_spin("SWR alarm", "Trip: drop MOX + refuse re-key",
      2, 5, app->tx_swr_alarm, G_CALLBACK(on_pref_tx_swr), app));
  adw_preferences_group_add(g, pref_switch("Mic noise gate",
      "Mutes room noise between words (−20 dB) — pair with PROC · live",
      app->tx_gate, G_CALLBACK(on_pref_tx_gate), app));
  adw_preferences_group_add(g, pref_slider("Gate threshold",
      "dBFS after Mic gain; open above, −20 dB below · live",
      GATE_DB_MIN, GATE_DB_MAX, app->tx_gate_db, "%.0f dB", G_CALLBACK(on_pref_tx_gate_db), app));
  adw_preferences_group_add(g, pref_slider("Monitor level",
      "dB into the RX audio output · live",
      MON_DB_MIN, MON_DB_MAX, app->tx_mon_db, "%.0f dB", G_CALLBACK(on_pref_tx_mon_db), app));
  adw_preferences_group_add(g, pref_spin("TX filter low",
      "Hz · voice audio low edge (default 150) · live",
      TXF_LO_MIN, TXF_LO_MAX, app->tx_flo, G_CALLBACK(on_pref_tx_flo), app));
  adw_preferences_group_add(g, pref_spin("TX filter high",
      "Hz · voice audio high edge (2850 default; 3500-6000 = eSSB — mind the band plan) · live",
      TXF_HI_MIN, TXF_HI_MAX, app->tx_fhi, G_CALLBACK(on_pref_tx_fhi), app));
  /* F6b — per-band PA calibration table (like piHPSDR's PA-calibration menu). The
   * 38.8 dB floor is the safety limit; 53 dB is the validated G1 default. */
  GtkWidget *pacal_exp = g_object_new(ADW_TYPE_EXPANDER_ROW, "title", "PA calibration (per band)",
      "subtitle", "dB · higher = less drive for the same power request", NULL);
  for (int i = 0; i < NBANDS; i++) {
    adw_expander_row_add_row(ADW_EXPANDER_ROW(pacal_exp), pacal_row(app, i));
  }
  adw_preferences_group_add(g, pacal_exp);
  /* F6b — wattmeter correction curve (like piHPSDR's Watt-meter calibration): map
   * the raw meter reading at each true power onto true watts. Default = linear. */
  GtkWidget *wm_exp = g_object_new(ADW_TYPE_EXPANDER_ROW, "title", "Wattmeter calibration",
      "subtitle", "correct the displayed power against an external wattmeter", NULL);
  for (int i = 1; i <= 10; i++) {
    adw_expander_row_add_row(ADW_EXPANDER_ROW(wm_exp), patrim_row(app, i));
  }
  GtkWidget *reset_row = g_object_new(ADW_TYPE_ACTION_ROW, "title", "Reset to linear",
      "subtitle", "restore the identity (uncalibrated) curve", NULL);
  GtkWidget *reset_btn = gtk_button_new_with_label("Reset");
  gtk_widget_set_valign(reset_btn, GTK_ALIGN_CENTER);
  g_signal_connect(reset_btn, "clicked", G_CALLBACK(on_pref_patrim_reset), app);
  adw_action_row_add_suffix(ADW_ACTION_ROW(reset_row), reset_btn);
  adw_expander_row_add_row(ADW_EXPANDER_ROW(wm_exp), reset_row);
  adw_preferences_group_add(g, wm_exp);
  adw_preferences_page_add(p, g);
  adw_preferences_dialog_add(dlg, p);   /* Drive / Tune drive / Antenna live on the footer bar */

  /* CW — own page (F6d-1c): keyer + sidetone, like piHPSDR's CW menu. All live. */
  p = ADW_PREFERENCES_PAGE(g_object_new(ADW_TYPE_PREFERENCES_PAGE,
      "title", "CW", "icon-name", "input-keyboard-symbolic", NULL));
  g = ADW_PREFERENCES_GROUP(g_object_new(ADW_TYPE_PREFERENCES_GROUP,
      "title", "Keyer &amp; sidetone", NULL));
  adw_preferences_group_add(g, pref_spin("Keyer speed",
      "WPM · queued CW (dev trigger / TCI) · live",
      5, 60, app->cw_wpm, G_CALLBACK(on_pref_cw_wpm), app));
  adw_preferences_group_add(g, pref_slider("Sidetone pitch",
      "monitor tone (default 700 Hz) · live",
      200, 1200, app->cw_pitch, "%.0f Hz", G_CALLBACK(on_pref_cw_pitch), app));
  adw_preferences_group_add(g, pref_slider("Sidetone level",
      "absolute dBFS, independent of Monitor level · −20 ≈ piHPSDR default · live",
      CW_ST_DB_MIN, CW_ST_DB_MAX, app->cw_st_db, "%.0f dB", G_CALLBACK(on_pref_cw_st_db), app));
  adw_preferences_group_add(g, pref_spin("Break-in hang",
      "ms · T/R hold after the last element (piHPSDR default 500) · live",
      0, 1000, app->cw_hang, G_CALLBACK(on_pref_cw_hang), app));
  adw_preferences_page_add(p, g);
  adw_preferences_dialog_add(dlg, p);

  /* TCI — own page (F6d-2): server switch + a live list of connected clients. */
  p = ADW_PREFERENCES_PAGE(g_object_new(ADW_TYPE_PREFERENCES_PAGE,
      "title", "TCI", "icon-name", "network-transmit-receive-symbolic", NULL));
  g = ADW_PREFERENCES_GROUP(g_object_new(ADW_TYPE_PREFERENCES_GROUP, "title", "Server",
      "description", "ExpertSDR-compatible control server (frequency/mode, CW keying, RX audio; TX audio comes later)", NULL));
  adw_preferences_group_add(g, pref_switch("TCI server",
      "WebSocket for SDC, loggers, Decodium… · live",
      app->tci_enable, G_CALLBACK(on_pref_tci_enable), app));
  adw_preferences_group_add(g, pref_spin("TCI port",
      "ExpertSDR default 40001 · applies on server toggle",
      1024, 65535, app->tci_port, G_CALLBACK(on_pref_tci_port), app));
  adw_preferences_page_add(p, g);
  g = ADW_PREFERENCES_GROUP(g_object_new(ADW_TYPE_PREFERENCES_GROUP, "title", "Clients", NULL));
  app->tci_client_row = g_object_new(ADW_TYPE_ACTION_ROW, "title", "—",
      "use-markup", FALSE, NULL);   /* user-agents may contain markup chars */
  g_signal_connect(app->tci_client_row, "destroy", G_CALLBACK(on_tci_row_destroy), app);
  adw_preferences_group_add(g, app->tci_client_row);
  adw_preferences_page_add(p, g);
  adw_preferences_dialog_add(dlg, p);
  tci_clients_tick(app);            /* fill immediately, then 1 s refresh */
  if (!app->tci_timer) { app->tci_timer = g_timeout_add(1000, tci_clients_tick, app); }
  /* Wide enough that AdwPreferencesDialog keeps the page switcher in the header
   * (top) instead of dropping it to a bottom bar — it falls back to the bottom
   * bar when the six pages don't fit across the top. */
  adw_dialog_set_content_width(ADW_DIALOG(dlg), 1000);
  adw_dialog_set_content_height(ADW_DIALOG(dlg), 700);

  /* Audio — ALL audio settings in one place (Richard's ask, 2026-07-10):
   * devices + shared sample rate + gain/latency. One sample rate for the RX
   * output stream (a soundcard runs at a single rate); separate RX playback and
   * TX capture DEVICES. Sample rate is the Nyquist/stream rate, independent of
   * the audio bandwidth (the filter). Rate + devices are restart-to-apply. */
  p = ADW_PREFERENCES_PAGE(g_object_new(ADW_TYPE_PREFERENCES_PAGE,
      "title", "Audio", "icon-name", "audio-speakers-symbolic", NULL));
  g = ADW_PREFERENCES_GROUP(g_object_new(ADW_TYPE_PREFERENCES_GROUP,
      "title", "Devices and rate", NULL));
  { GtkStringList *m = gtk_string_list_new(NULL);
    guint sel = 0;
    for (guint i = 0; i < G_N_ELEMENTS(AUDIO_RATES); i++) {
      char lbl[16]; snprintf(lbl, sizeof lbl, "%d kHz", AUDIO_RATES[i] / 1000);
      gtk_string_list_append(m, lbl);
      if (AUDIO_RATES[i] == app->audio_rate) { sel = i; }
    }
    GtkWidget *row = g_object_new(ADW_TYPE_COMBO_ROW, "title", "Sample rate",
        "subtitle", "RX output, ≤ IQ rate; the AF band is filter-limited, above "
                    "192 kHz is only fatter samples (mic is fixed 48 kHz by WDSP) "
                    "· restart to apply",
        "model", m, "selected", sel, NULL);
    g_signal_connect(row, "notify::selected", G_CALLBACK(on_pref_audio_rate), app);
    adw_preferences_group_add(g, row);
  }
  { app->audio_nsink = audio_list_sinks(app->audio_sinks, (int)G_N_ELEMENTS(app->audio_sinks));
    GtkStringList *m = gtk_string_list_new(NULL);
    gtk_string_list_append(m, "Default (auto)");
    guint sel = 0;
    for (int i = 0; i < app->audio_nsink; i++) {
      gtk_string_list_append(m, app->audio_sinks[i].desc);
      if (app->audio_device[0] && strcmp(app->audio_sinks[i].name, app->audio_device) == 0) { sel = (guint)(i + 1); }
    }
    GtkWidget *row = g_object_new(ADW_TYPE_COMBO_ROW, "title", "RX output device",
        "subtitle", "receive-audio soundcard · restart to apply", "model", m, "selected", sel, NULL);
    g_signal_connect(row, "notify::selected", G_CALLBACK(on_pref_audio_device), app);
    adw_preferences_group_add(g, row);
  }
  { app->mic_nsrc = mic_list_sources(app->mic_srcs, (int)G_N_ELEMENTS(app->mic_srcs));
    GtkStringList *m = gtk_string_list_new(NULL);
    gtk_string_list_append(m, "Default (auto)");
    guint sel = 0;
    for (int i = 0; i < app->mic_nsrc; i++) {
      gtk_string_list_append(m, app->mic_srcs[i].desc);
      if (app->mic_device[0] && strcmp(app->mic_srcs[i].name, app->mic_device) == 0) { sel = (guint)(i + 1); }
    }
    GtkWidget *row = g_object_new(ADW_TYPE_COMBO_ROW, "title", "TX mic device",
        "subtitle", "SSB capture soundcard (F6c) · restart to apply", "model", m, "selected", sel, NULL);
    g_signal_connect(row, "notify::selected", G_CALLBACK(on_pref_mic_device), app);
    adw_preferences_group_add(g, row);
  }
  adw_preferences_page_add(p, g);
  g = ADW_PREFERENCES_GROUP(g_object_new(ADW_TYPE_PREFERENCES_GROUP,
      "title", "Levels and latency", NULL));
  adw_preferences_group_add(g, pref_spin("Digital master gain",
      "× on the demodulated audio after WDSP; 1 = calibrated default · live",
      1, 32, app->gain, G_CALLBACK(on_pref_gain), app));
  adw_preferences_group_add(g, pref_spin("Audio latency",
      "ms, PipeWire quantum for RX output AND TX mic capture · restart to apply",
      5, 100, app->latency, G_CALLBACK(on_pref_latency), app));
  adw_preferences_page_add(p, g);
  adw_preferences_dialog_add(dlg, p);

  /* Spectrum — how the trace + waterfall look (fps live). */
  p = ADW_PREFERENCES_PAGE(g_object_new(ADW_TYPE_PREFERENCES_PAGE,
      "title", "Spectrum", "icon-name", "video-display-symbolic", NULL));
  g = ADW_PREFERENCES_GROUP(g_object_new(ADW_TYPE_PREFERENCES_GROUP, "title", "Levels", NULL));
  adw_preferences_group_add(g, pref_spin("Frame rate", "fps · applies live",
      10, 60, app->fps, G_CALLBACK(on_pref_fps), app));
  adw_preferences_group_add(g, pref_spin("Scale top", "dBm · or drag the dB scale",
      -30, 20, app->pan_high, G_CALLBACK(on_pref_pan_high), app));
  adw_preferences_group_add(g, pref_spin("Scale bottom", "dBm · scroll the scale to zoom",
      -200, -40, app->pan_low, G_CALLBACK(on_pref_pan_low), app));
  adw_preferences_group_add(g, pref_switch("Auto level", "Track the noise floor (like the waterfall)",
      app->auto_level, G_CALLBACK(on_pref_auto_level), app));
  adw_preferences_page_add(p, g);

  /* Averaging — spectrum and waterfall independently (ms time constant). */
  g = ADW_PREFERENCES_GROUP(g_object_new(ADW_TYPE_PREFERENCES_GROUP, "title", "Averaging",
      "description", "Time constant in ms · 0 = none", NULL));
  adw_preferences_group_add(g, pref_spin("Spectrum", "ms · trace smoothing",
      0, 1000, app->avg_spec_ms, G_CALLBACK(on_pref_avg_spec), app));
  adw_preferences_group_add(g, pref_spin("Waterfall", "ms · waterfall smoothing",
      0, 1000, app->avg_wf_ms, G_CALLBACK(on_pref_avg_wf), app));
  adw_preferences_page_add(p, g);

  /* Colour scheme — one palette drives both the waterfall and the spectrum. */
  g = ADW_PREFERENCES_GROUP(g_object_new(ADW_TYPE_PREFERENCES_GROUP, "title", "Colours", NULL));
  int npal = waterfall_palette_count();
  const char **pnames = g_new0(const char *, npal + 1);
  for (int i = 0; i < npal; i++) { pnames[i] = waterfall_palette_name(i); }
  GtkStringList *pl = gtk_string_list_new(pnames);
  g_free(pnames);
  guint psel = (app->palette >= 0 && app->palette < npal) ? (guint)app->palette : 0;
  GtkWidget *pal = g_object_new(ADW_TYPE_COMBO_ROW, "title", "Colour scheme",
      "subtitle", "Waterfall + spectrum palette", "model", pl, "selected", psel, NULL);
  g_signal_connect(pal, "notify::selected", G_CALLBACK(on_pref_palette), app);
  adw_preferences_group_add(g, pal);
  adw_preferences_page_add(p, g);
  adw_preferences_dialog_add(dlg, p);

  /* Overlays — grid, filter + band plan drawn on top of the spectrum. */
  p = ADW_PREFERENCES_PAGE(g_object_new(ADW_TYPE_PREFERENCES_PAGE,
      "title", "Overlays", "icon-name", "view-list-symbolic", NULL));
  g = ADW_PREFERENCES_GROUP(g_object_new(ADW_TYPE_PREFERENCES_GROUP, "title", "Grid &amp; scales", NULL));
  adw_preferences_group_add(g, pref_switch("dB grid", "Horizontal level lines",
      app->show_db_grid, G_CALLBACK(on_pref_db_grid), app));
  adw_preferences_group_add(g, pref_switch("dB scale", "Left dBm labels",
      app->show_db_scale, G_CALLBACK(on_pref_db_scale), app));
  adw_preferences_group_add(g, pref_switch("Frequency grid", "Vertical frequency lines",
      app->show_freq_grid, G_CALLBACK(on_pref_freq_grid), app));
  adw_preferences_group_add(g, pref_switch("Frequency scale", "Top frequency labels",
      app->show_freq_scale, G_CALLBACK(on_pref_freq_scale), app));
  adw_preferences_page_add(p, g);

  /* Filter overlay. */
  g = ADW_PREFERENCES_GROUP(g_object_new(ADW_TYPE_PREFERENCES_GROUP, "title", "Filter", NULL));
  adw_preferences_group_add(g, pref_switch("Filter on waterfall", "Extend passband + centre down",
      app->show_filter_wf, G_CALLBACK(on_pref_filter_wf), app));
  adw_preferences_group_add(g, pref_slider("Filter opacity", "Passband overlay transparency",
      0, 100, app->filter_op, "%.0f %%", G_CALLBACK(on_pref_filter_op), app));
  adw_preferences_page_add(p, g);

  /* Band plan — region + national overlay drive the band-edge overlay. */
  g = ADW_PREFERENCES_GROUP(g_object_new(ADW_TYPE_PREFERENCES_GROUP, "title", "Band plan", NULL));
  int nreg = bp_region_count();
  const char **rnames = g_new0(const char *, nreg + 1);
  for (int i = 0; i < nreg; i++) { rnames[i] = bp_region_name(i); }
  GtkStringList *rl = gtk_string_list_new(rnames);
  g_free(rnames);
  guint rsel = (app->bp_region >= 0 && app->bp_region < nreg) ? (guint)app->bp_region : 0;
  GtkWidget *reg = g_object_new(ADW_TYPE_COMBO_ROW, "title", "Region",
      "subtitle", "IARU region for band edges", "model", rl, "selected", rsel, NULL);
  g_signal_connect(reg, "notify::selected", G_CALLBACK(on_pref_region), app);
  adw_preferences_group_add(g, reg);
  int ncty = bp_country_count();
  const char **cnames = g_new0(const char *, ncty + 1);
  for (int i = 0; i < ncty; i++) { cnames[i] = bp_country_name(i); }
  GtkStringList *cl = gtk_string_list_new(cnames);
  g_free(cnames);
  guint csel = (app->bp_country >= 0 && app->bp_country < ncty) ? (guint)app->bp_country : 0;
  GtkWidget *cty = g_object_new(ADW_TYPE_COMBO_ROW, "title", "Country",
      "subtitle", "National allocation overrides", "model", cl, "selected", csel, NULL);
  g_signal_connect(cty, "notify::selected", G_CALLBACK(on_pref_country), app);
  adw_preferences_group_add(g, cty);
  adw_preferences_group_add(g, pref_switch("Show band plan", "Edges, usage segments + band name",
      app->show_band_edges, G_CALLBACK(on_pref_band_edges), app));
  adw_preferences_group_add(g, pref_switch("DX spots (TCI)",
      "Callsigns from a connected skimmer/cluster client (SDC); click a spot to tune",
      app->show_spots, G_CALLBACK(on_pref_spots), app));
  adw_preferences_page_add(p, g);
  adw_preferences_dialog_add(dlg, p);

  return ADW_DIALOG(dlg);
}

static void act_prefs(GSimpleAction *a, GVariant *param, gpointer data) {
  (void)a; (void)param;
  App *app = (App *)data;
  GtkWindow *win = gtk_application_get_active_window(
      GTK_APPLICATION(g_application_get_default()));
  AdwDialog *dlg = build_prefs(app);
  g_signal_connect(dlg, "closed", G_CALLBACK(on_prefs_closed), app);   /* restart toast after close */
  adw_dialog_present(dlg, GTK_WIDGET(win));
}

/* Persist window geometry as the user resizes / maximizes (debounced). GTK4
 * keeps default-width/height at the current *restore* size, so this survives
 * maximize too. */
static void on_win_size(GObject *win, GParamSpec *ps, gpointer data) {
  (void)ps;
  App *app = (App *)data;
  int w = 0, h = 0;
  gtk_window_get_default_size(GTK_WINDOW(win), &w, &h);
  if (w > 0 && h > 0) { app->win_w = w; app->win_h = h; }
  app->win_max = gtk_window_is_maximized(GTK_WINDOW(win)) ? 1 : 0;
  schedule_save(app);
}

static void on_activate(GtkApplication *gtkapp, gpointer data) {
  App *app = (App *)data;
  css_load();

  GtkWidget *win = adw_application_window_new(gtkapp);
  app->win = win;
  gtk_window_set_title(GTK_WINDOW(win),
                       app->radio_mode ? "SDR for Linux — radio" : "SDR for Linux — server");
  int ww = app->win_w > 0 ? app->win_w : 1320;
  int wh = app->win_h > 0 ? app->win_h : 720;
  gtk_window_set_default_size(GTK_WINDOW(win), ww, wh);
  if (app->win_max) { gtk_window_maximize(GTK_WINDOW(win)); }
  g_signal_connect(win, "notify::default-width",  G_CALLBACK(on_win_size), app);
  g_signal_connect(win, "notify::default-height", G_CALLBACK(on_win_size), app);
  g_signal_connect(win, "notify::maximized",      G_CALLBACK(on_win_size), app);

  GtkWidget *header = adw_header_bar_new();
  GtkWidget *status = gtk_label_new(app->radio_mode ? "●  ANAN G1" : "●  server");
  gtk_widget_add_css_class(status, "dim");
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), status);
  if (app->radio_mode) {
    GSimpleAction *pa = g_simple_action_new("preferences", NULL);
    g_signal_connect(pa, "activate", G_CALLBACK(act_prefs), app);
    g_action_map_add_action(G_ACTION_MAP(gtkapp), G_ACTION(pa));
    g_object_unref(pa);

    GMenu *m = g_menu_new();
    g_menu_append(m, "Preferences", "app.preferences");
    GtkWidget *menu = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu), G_MENU_MODEL(m));
    g_object_unref(m);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu);
  }

  GtkWidget *tv = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);

  app->area = gtk_drawing_area_new();
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app->area), draw_cb, app, NULL);
  gtk_widget_set_vexpand(app->area, TRUE);

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  if (app->radio_mode) {
    gtk_box_append(GTK_BOX(content), build_controls(app));
    gtk_box_append(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  }
  gtk_box_append(GTK_BOX(content), app->area);
  if (app->radio_mode) {   /* thin line under the waterfall too, mirroring the top */
    gtk_box_append(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  }
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), content);
  if (app->radio_mode) {
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(tv), build_bottom_controls(app));
  }
  app->toast_overlay = adw_toast_overlay_new();
  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(app->toast_overlay), tv);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), app->toast_overlay);

  /* Input controllers (self-gate on radio mode): wheel tunes, drag pans, u/l/c/a mode. */
  GtkEventControllerScroll *scroll = GTK_EVENT_CONTROLLER_SCROLL(
      gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL));
  g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), app);
  gtk_widget_add_controller(app->area, GTK_EVENT_CONTROLLER(scroll));

  GtkGesture *drag = gtk_gesture_drag_new();   /* left button by default */
  g_signal_connect(drag, "drag-begin",  G_CALLBACK(on_drag_begin),  app);
  g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), app);
  gtk_widget_add_controller(app->area, GTK_EVENT_CONTROLLER(drag));

  GtkEventControllerMotion *motion =
      GTK_EVENT_CONTROLLER_MOTION(gtk_event_controller_motion_new());
  g_signal_connect(motion, "motion", G_CALLBACK(on_motion), app);
  gtk_widget_add_controller(app->area, GTK_EVENT_CONTROLLER(motion));

  GtkGesture *click = gtk_gesture_click_new();   /* left: dbl-click gutter → auto-fit,
                                                  * single click (select mode) → tune */
  g_signal_connect(click, "pressed", G_CALLBACK(on_pressed), app);
  gtk_widget_add_controller(app->area, GTK_EVENT_CONTROLLER(click));

  GtkGesture *rdrag = gtk_gesture_drag_new();    /* right drag = zoom (dB range / span) */
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rdrag), GDK_BUTTON_SECONDARY);
  g_signal_connect(rdrag, "drag-begin",  G_CALLBACK(on_rdrag_begin),  app);
  g_signal_connect(rdrag, "drag-update", G_CALLBACK(on_rdrag_update), app);
  g_signal_connect(rdrag, "drag-end",    G_CALLBACK(on_rdrag_end),    app);
  gtk_widget_add_controller(app->area, GTK_EVENT_CONTROLLER(rdrag));

  GtkGesture *rclick = gtk_gesture_click_new();  /* right click (no drag) = toggle select mode */
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), GDK_BUTTON_SECONDARY);
  g_signal_connect(rclick, "pressed",  G_CALLBACK(on_right_pressed),  app);
  g_signal_connect(rclick, "released", G_CALLBACK(on_right_released), app);
  gtk_widget_add_controller(app->area, GTK_EVENT_CONTROLLER(rclick));

  GtkEventControllerKey *keys =
      GTK_EVENT_CONTROLLER_KEY(gtk_event_controller_key_new());
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), app);
  gtk_widget_add_controller(win, GTK_EVENT_CONTROLLER(keys));

  gtk_widget_add_tick_callback(app->area, tick_cb, app, NULL);
  gtk_window_present(GTK_WINDOW(win));
}

/* Bring up the direct-radio engine: discover → analyzer → P2 RX.
 * State precedence: env var > saved config > built-in default. */
static void start_radio(App *app) {
  Settings st = { .freq = 14100000, .rate = 192000, .mode = -1,
                  .volume = -10.0, .gain = 1.0, .fps = 25, .latency = 10,
                  .step = TUNE_STEP_DEFAULT, .zoom = 1.0, .atten = 0,
                  .agc = 3, .agc_gain = 80.0, .filter = -1,
                  .pan_high = PAN_HIGH_DEFAULT, .pan_low = PAN_LOW_DEFAULT,
                  .db_grid = 1, .db_scale = 1, .freq_grid = 1, .freq_scale = 1,
                  .filter_wf = 1, .filter_op = 60, .avg_spec = -1, .avg_wf = -1,
                  .palette = 0, .band_edges = 1, .show_spots = 1,
                  .tx_pa = 0, .tx_ant = 0, .tx_drive = 25.0, .tx_tune = 10.0, .tx_swr = 3.0,
                  .mic_gain = 0.0, .audio_rate = 48000,
                  .tx_gate_db = GATE_DB_DFLT, .tx_mon_db = MON_DB_DFLT,
                  .tx_flo = TXF_LO_DFLT, .tx_fhi = TXF_HI_DFLT,
                  .cw_wpm = CW_WPM_DFLT, .cw_pitch = CW_PITCH_DFLT,
                  .cw_st_db = CW_ST_DB_DFLT, .cw_hang = CW_HANG_DFLT,
                  .tci_enable = 0, .tci_port = 40001, .tci_iq_rate = 48000 };
  g_strlcpy(st.ip, "", sizeof(st.ip));   /* no radio default: the picker (or a
                                            saved config) provides the IP; empty
                                            = discovery falls back to broadcast */
  g_strlcpy(st.region,  "R1", sizeof(st.region));   /* IARU R1 default; country = none */
  g_strlcpy(st.country, "",   sizeof(st.country));
  if (settings_load(&st)) { printf("settings: loaded %s\n", settings_path()); }

  const char *e;
  if ((e = getenv("SDRFL_RADIO_IP")) && *e) { g_strlcpy(st.ip, e, sizeof(st.ip)); }
  if (app->picked_ip[0]) { g_strlcpy(st.ip, app->picked_ip, sizeof(st.ip)); }  /* picker's choice */
  if ((e = getenv("SDRFL_FREQ"))     && *e) { st.freq = strtoll(e, NULL, 10); }
  if ((e = getenv("SDRFL_RATE"))     && *e) { st.rate = atoi(e); }
  if ((e = getenv("SDRFL_VOLUME"))   && *e) { st.volume = atof(e); }
  if ((e = getenv("SDRFL_GAIN"))     && *e) { st.gain = atof(e); }
  if ((e = getenv("SDRFL_FPS"))      && *e) { st.fps = atoi(e); }
  if ((e = getenv("SDRFL_LAT"))      && *e) { st.latency = atoi(e); }
  /* Mode: env SDRFL_MODE > saved mode (if valid) > by-band default. */
  int mode = mode_from_name(getenv("SDRFL_MODE"));
  if (mode < 0) { mode = st.mode; }
  if (mode < 0) { mode = (st.freq < 10000000) ? DEMOD_LSB : DEMOD_USB; }

  snprintf(ipaddr_radio, sizeof(ipaddr_radio), "%s", st.ip);
  g_strlcpy(app->radio_ip, st.ip, sizeof(app->radio_ip));
  app->freq   = st.freq;
  app->rate   = st.rate;
  app->mode   = mode;
  app->volume = st.volume;
  app->gain   = st.gain;
  app->atten  = st.atten < 0 ? 0 : (st.atten > 31 ? 31 : st.atten);
  app->agc    = st.agc < 0 ? 0 : (st.agc > 4 ? 4 : st.agc);
  app->agc_gain = st.agc_gain < -20.0 ? -20.0 : (st.agc_gain > 120.0 ? 120.0 : st.agc_gain);
  app->nr     = st.nr < 0 ? 0 : (st.nr > 4 ? 4 : st.nr);   /* 0 off /1 NR /2 NR2 /3 NR3 /4 NR4 */
  app->nb     = st.nb < 0 ? 0 : (st.nb > 2 ? 2 : st.nb);   /* 0 off / 1 NB / 2 NB2 */
  app->anf    = st.anf ? 1 : 0;
  app->binaural = st.binaural ? 1 : 0;
  /* TX (F6a): PA-enable persists (mirrors piHPSDR). Clamp the rest to safe ranges. */
  app->tx_pa_enabled = st.tx_pa ? 1 : 0;
  app->tx_antenna    = st.tx_ant < 0 ? 0 : (st.tx_ant > 2 ? 2 : st.tx_ant);
  app->tx_drive_w    = st.tx_drive < 0.0 ? 0.0 : (st.tx_drive > 100.0 ? 100.0 : st.tx_drive);
  app->tx_tune_w     = st.tx_tune  < 0.0 ? 0.0 : (st.tx_tune  > 100.0 ? 100.0 : st.tx_tune);
  app->tx_swr_alarm  = st.tx_swr   < 2.0 ? 2.0 : (st.tx_swr   > 5.0   ? 5.0   : st.tx_swr);
  app->tx_mic_gain   = st.mic_gain < MIC_GAIN_MIN ? MIC_GAIN_MIN : (st.mic_gain > MIC_GAIN_MAX ? MIC_GAIN_MAX : st.mic_gain);
  app->tx_comp       = st.tx_comp ? 1 : 0;
  app->tx_comp_db    = st.tx_comp_db < COMP_DB_MIN ? COMP_DB_MIN : (st.tx_comp_db > COMP_DB_MAX ? COMP_DB_MAX : st.tx_comp_db);
  app->tx_gate       = st.tx_gate ? 1 : 0;
  app->tx_gate_db    = st.tx_gate_db < GATE_DB_MIN ? GATE_DB_MIN : (st.tx_gate_db > GATE_DB_MAX ? GATE_DB_MAX : st.tx_gate_db);
  app->tx_mon        = st.tx_mon ? 1 : 0;
  app->tx_mon_db     = st.tx_mon_db < MON_DB_MIN ? MON_DB_MIN : (st.tx_mon_db > MON_DB_MAX ? MON_DB_MAX : st.tx_mon_db);
  app->tx_flo        = st.tx_flo < TXF_LO_MIN ? TXF_LO_MIN : (st.tx_flo > TXF_LO_MAX ? TXF_LO_MAX : st.tx_flo);
  app->tx_fhi        = st.tx_fhi < TXF_HI_MIN ? TXF_HI_MIN : (st.tx_fhi > TXF_HI_MAX ? TXF_HI_MAX : st.tx_fhi);
  app->cw_wpm        = st.cw_wpm   < 5   ? 5   : (st.cw_wpm   > 60   ? 60   : st.cw_wpm);
  app->cw_pitch      = st.cw_pitch < 200 ? 200 : (st.cw_pitch > 1200 ? 1200 : st.cw_pitch);
  app->cw_st_db      = st.cw_st_db < CW_ST_DB_MIN ? CW_ST_DB_MIN : (st.cw_st_db > CW_ST_DB_MAX ? CW_ST_DB_MAX : st.cw_st_db);
  app->cw_hang       = st.cw_hang  < 0   ? 0   : (st.cw_hang  > 1000 ? 1000 : st.cw_hang);
  app->tci_enable    = st.tci_enable ? 1 : 0;
  app->tci_port      = st.tci_port < 1024 ? 40001 : (st.tci_port > 65535 ? 40001 : st.tci_port);
  app->tci_iq_rate   = (st.tci_iq_rate == 96000 || st.tci_iq_rate == 192000 ||
                        st.tci_iq_rate == 384000) ? st.tci_iq_rate : 48000;
  app->fps    = st.fps;
  app->latency = st.latency;
  /* Clamp to the supported 48-192 k window — a stale >192 k value from an older
   * config would otherwise still apply (the AF band is filter-limited; higher
   * rates are only fatter samples — Richard's call, 2026-07-10). */
  app->audio_rate = st.audio_rate >= 48000 ? st.audio_rate : 48000;
  if (app->audio_rate > 192000) { app->audio_rate = 192000; }
  g_strlcpy(app->audio_device, st.audio_device, sizeof app->audio_device);
  g_strlcpy(app->mic_device, st.mic_device, sizeof app->mic_device);
  /* Snap the saved zoom to the nearest octave detent in [1, ZOOM_MAX]. */
  app->zoom = st.zoom;
  if (!(app->zoom >= 1.0))  { app->zoom = 1.0; }      /* NaN or < 1 */
  if (app->zoom > ZOOM_MAX) { app->zoom = ZOOM_MAX; }
  app->zoom = pow(2.0, lround(log2(app->zoom)));
  app->pending_zoom = app->zoom;
  /* dB window: validate the saved pair, else fall back to the default. */
  app->cur_band = -1;   /* so pan_set_window's pan_store_band is a no-op until set */
  if (st.pan_high > st.pan_low && st.pan_high <= PAN_DBM_CEIL && st.pan_low >= PAN_DBM_FLOOR) {
    pan_set_window(app, st.pan_high, st.pan_low);
  } else {
    app->pan_high = PAN_HIGH_DEFAULT;
    app->pan_low  = PAN_LOW_DEFAULT;
  }
  /* TX dB window: a saved pair wins (the operator's manual scale); else defaults +
   * a one-shot autofit on the first TX frame (tx_pan_init left 0). tx_wf exists by
   * now (created in main before start_radio), so colour it to this window. */
  if (st.tx_pan_high > st.tx_pan_low && st.tx_pan_high <= PAN_DBM_CEIL && st.tx_pan_low >= PAN_DBM_FLOOR) {
    app->tx_pan_high = st.tx_pan_high; app->tx_pan_low = st.tx_pan_low; app->tx_pan_init = 1;
  } else {
    app->tx_pan_high = -40.0; app->tx_pan_low = -130.0; app->tx_pan_init = 0;
  }
  tx_pan_apply(app);
  /* Per-band dB windows: default every band to the global window, then apply the
   * saved "key=hi/lo;..." overrides, and load the band app->freq is in. */
  for (int i = 0; i < NBANDS; i++) {
    app->band_high[i] = app->pan_high;
    app->band_low[i]  = app->pan_low;
    app->band_mode[i] = (BANDS[i].lo < 10000000) ? DEMOD_LSB : DEMOD_USB;  /* default */
    app->band_freq[i] = BANDS[i].dflt;
  }
  char blbuf[512];
  g_strlcpy(blbuf, st.band_levels, sizeof blbuf);
  char *blsv = NULL;
  for (char *tok = strtok_r(blbuf, ";", &blsv); tok; tok = strtok_r(NULL, ";", &blsv)) {
    char *eq = strchr(tok, '=');
    if (!eq) { continue; }
    *eq = '\0';
    char *rest = eq + 1;                       /* "hi/lo/mode/freq" (mode,freq optional) */
    char *s1 = strchr(rest, '/');
    if (!s1) { continue; }
    *s1 = '\0';
    char *lostr = s1 + 1;
    int md = -1; long long bf = -1;
    char *s2 = strchr(lostr, '/');             /* mode */
    if (s2) {
      *s2 = '\0';
      char *modestr = s2 + 1;
      char *s3 = strchr(modestr, '/');         /* freq */
      if (s3) { *s3 = '\0'; bf = atoll(s3 + 1); }
      md = atoi(modestr);
    }
    double hi = g_ascii_strtod(rest,  NULL);   /* locale-independent */
    double lo = g_ascii_strtod(lostr, NULL);
    if (hi > lo) {
      for (int i = 0; i < NBANDS; i++) {
        if (strcmp(tok, BANDS[i].key) == 0) {
          app->band_high[i] = hi; app->band_low[i] = lo;
          if (md >= 0 && md < DEMOD_NMODES) { app->band_mode[i] = md; }
          if (bf >= BANDS[i].lo && bf <= BANDS[i].hi) { app->band_freq[i] = bf; }
          break;
        }
      }
    }
  }
  /* Per-band PA calibration (F6b): default every band to 53 dB, then apply saved
   * "key=dB;..." overrides (clamped to the safe [38.8,70.0] range). */
  for (int i = 0; i < NBANDS; i++) { app->band_pacal[i] = PACAL_DEFAULT; }
  char pcbuf[256];
  g_strlcpy(pcbuf, st.pa_cal, sizeof pcbuf);
  char *pcsv = NULL;
  for (char *tok = strtok_r(pcbuf, ";", &pcsv); tok; tok = strtok_r(NULL, ";", &pcsv)) {
    char *eq = strchr(tok, '=');
    if (!eq) { continue; }
    *eq = '\0';
    double db = pacal_clamp(g_ascii_strtod(eq + 1, NULL));
    for (int i = 0; i < NBANDS; i++) {
      if (strcmp(tok, BANDS[i].key) == 0) { app->band_pacal[i] = db; break; }
    }
  }
  /* Wattmeter-trim curve (F6b): default identity, then apply the saved 11 points. */
  for (int i = 0; i < 11; i++) { app->pa_trim[i] = i * PATRIM_STEP; }
  char ptbuf[256];
  g_strlcpy(ptbuf, st.pa_trim, sizeof ptbuf);
  char *ptsv = NULL; int pti = 0;
  for (char *tok = strtok_r(ptbuf, ";", &ptsv); tok && pti < 11; tok = strtok_r(NULL, ";", &ptsv)) {
    double v = g_ascii_strtod(tok, NULL);
    if (v < 0.0) { v = 0.0; }
    app->pa_trim[pti++] = v;
  }
  app->pa_trim[0] = 0.0;   /* the origin is fixed */

  app->cur_band = band_for_freq(app->freq);
  if (app->cur_band >= 0) {
    app->pan_high = app->band_high[app->cur_band];
    app->pan_low  = app->band_low[app->cur_band];
    app->band_mode[app->cur_band] = app->mode;   /* current band matches startup mode */
    app->band_freq[app->cur_band] = app->freq;
  }
  app->ptr_x = 1e9;   /* until the pointer moves, wheel = tune (not gutter zoom) */
  app->show_db_grid    = st.db_grid    ? 1 : 0;
  app->show_db_scale   = st.db_scale   ? 1 : 0;
  app->show_freq_grid  = st.freq_grid  ? 1 : 0;
  app->show_freq_scale = st.freq_scale ? 1 : 0;
  app->show_filter_wf  = st.filter_wf  ? 1 : 0;
  app->filter_op = (st.filter_op < 0) ? 0 : (st.filter_op > 100 ? 100 : st.filter_op);
  app->auto_level = st.auto_level ? 1 : 0;
  app->avg_spec_ms = (st.avg_spec < 0) ? 150 : (st.avg_spec > 2000 ? 2000 : st.avg_spec);
  app->avg_wf_ms   = (st.avg_wf   < 0) ?  40 : (st.avg_wf   > 2000 ? 2000 : st.avg_wf);
  app->palette = (st.palette < 0 || st.palette >= waterfall_palette_count()) ? 0 : st.palette;
  waterfall_set_palette(app->wf, app->palette);   /* app->wf created in main() before activation */
  waterfall_set_palette(app->tx_wf, app->palette);
  app->show_band_edges = st.band_edges ? 1 : 0;
  app->show_spots      = st.show_spots ? 1 : 0;
  { int r = bp_region_from_key(st.region);  app->bp_region  = r >= 0 ? r : 0; }
  { int c = bp_country_from_key(st.country); app->bp_country = c >= 0 ? c : 0; }
  app->win_w   = st.win_w   > 0 ? st.win_w : 1320;
  app->win_h   = st.win_h   > 0 ? st.win_h : 720;
  app->win_max = st.win_max ? 1 : 0;
  app->pixels = ENGINE_PIXELS;
  app->tune_step = TUNE_STEP_DEFAULT;   /* keep only known step values */
  for (guint i = 0; i < G_N_ELEMENTS(TUNE_STEPS); i++) {
    if (TUNE_STEPS[i] == st.step) { app->tune_step = st.step; break; }
  }
  int rate    = st.rate;

  printf("Discovering radio at %s ...\n", ipaddr_radio);
  p2_discovery();
  if (devices <= 0) { fprintf(stderr, "no radio found\n"); return; }
  const DISCOVERED *dev = &discovered[selected_device];
  if (dev->status == 3) {   /* P2 discovery reply byte[4]: 2 = idle, 3 = streaming */
    fprintf(stderr, "radio at %s is IN USE by another program (piHPSDR?) — close it first\n",
            inet_ntoa(dev->network.address.sin_addr));
    return;
  }
  printf("Using %s at %s — RX %lld Hz @ %d Hz\n", dev->name,
         inet_ntoa(dev->network.address.sin_addr), app->freq, rate);

  if (analyzer_create(0, app->pixels, rate, app->fps) != 0) {
    fprintf(stderr, "analyzer_create failed\n");
    return;
  }
  if (app->zoom != 1.0) { analyzer_set_zoom(app->zoom); }   /* restore saved zoom */

  /* Audio: WDSP demod → native PipeWire sink. */
  int nf, dfl;
  mode_filters(mode, &nf, &dfl);
  /* Var1/Var2 editable filters: seed per mode (Var1 = default preset, Var2 =
   * widest), then apply saved "modeid/v1lo/v1hi/v2lo/v2hi;..." overrides. */
  for (int i = 0; i < DEMOD_NMODES; i++) {
    int nn, dd; const FilterPreset *f = mode_filters(i, &nn, &dd);
    app->var_low[i][0] = f[dd].low; app->var_high[i][0] = f[dd].high;
    app->var_low[i][1] = f[0].low;  app->var_high[i][1] = f[0].high;
  }
  char vfbuf[512];
  g_strlcpy(vfbuf, st.var_filt, sizeof vfbuf);
  char *vfsv = NULL;
  for (char *tok = strtok_r(vfbuf, ";", &vfsv); tok; tok = strtok_r(NULL, ";", &vfsv)) {
    int mo, a, b, c, d;
    if (sscanf(tok, "%d/%d/%d/%d/%d", &mo, &a, &b, &c, &d) == 5 && mo >= 0 && mo < DEMOD_NMODES && b > a && d > c) {
      app->var_low[mo][0] = a; app->var_high[mo][0] = b;
      app->var_low[mo][1] = c; app->var_high[mo][1] = d;
    }
  }
  app->filter_idx = (st.filter >= 0 && st.filter < nf + 2) ? st.filter : dfl;  /* saved or default */
  /* Per-mode filter memory: seed every mode with its default, the current mode
   * with the loaded filter, then apply the saved "id=idx;..." overrides. */
  for (int i = 0; i < DEMOD_NMODES; i++) { int nn, dd; mode_filters(i, &nn, &dd); app->filter_by_mode[i] = dd; }
  app->filter_by_mode[mode] = app->filter_idx;
  char mfbuf[128];
  g_strlcpy(mfbuf, st.mode_filt, sizeof mfbuf);
  char *mfsv = NULL;
  for (char *tok = strtok_r(mfbuf, ";", &mfsv); tok; tok = strtok_r(NULL, ";", &mfsv)) {
    int mid, fi;
    if (sscanf(tok, "%d=%d", &mid, &fi) == 2 && mid >= 0 && mid < DEMOD_NMODES) {
      int nn, dd; mode_filters(mid, &nn, &dd);
      if (fi >= 0 && fi < nn + 2) { app->filter_by_mode[mid] = fi; }
    }
  }
  app->filter_idx = app->filter_by_mode[mode];
  { int lo, hi; filter_lohi(app, mode, app->filter_idx, &lo, &hi); app->flo = lo; app->fhi = hi; }
  /* RX audio output rate: an exact divisor of the IQ rate, ≤ it (WDSP downsamples
   * IQ→audio). Compute once so the sink and the demod channel agree. */
  int arate = app->audio_rate >= 48000 ? app->audio_rate : 48000;
  if (arate > rate) { arate = rate; }
  { int asc = rate / arate; if (asc < 1) { asc = 1; } arate = rate / asc; }
  demod_set_audio_rate(arate);
  if (audio_start(arate, 2, app->latency, app->audio_device[0] ? app->audio_device : NULL) == 0 &&
      demod_create(0, rate, mode, app->flo, app->fhi, app->volume) == 0) {
    demod_set_gain(app->gain);
    demod_set_agc(app->agc);            /* saved AGC character + threshold */
    demod_set_agc_gain(app->agc_gain);
    demod_set_nr(app->nr);              /* saved NR/NB/ANF */
    demod_set_nb(app->nb);
    demod_set_anf(app->anf);
    demod_set_binaural(app->binaural);
    app->audio_ok = 1;
  } else {
    fprintf(stderr, "audio/demod init failed — panadapter only\n");
  }

  /* TX runtime (F6a): open the WDSP TX channel and spawn the idle worker thread
   * BEFORE RX starts flowing, so OpenChannel doesn't race the live RX channel.
   * The GUI only ever requests TUNE; all keying goes through tx_gate. Starts with
   * a safe config (PA off, drive 0); tx_push_cfg applies the operator's settings. */
  if (tx_run_start(app->freq, app->pixels, app->fps) == 0) {
    app->tx_ready = 1;
    tx_push_cfg(app);
    tx_run_set_mic_gain(app->tx_mic_gain);   /* persisted SSB mic gain into the TX panel */
    tx_apply_proc(app);   /* persisted PROC + mic gate (forced OFF in digi modes) */
    tx_run_set_monitor(app->tx_mon);                  /* persisted TX monitor (self-listen) */
    demod_set_monitor_gain(app->tx_mon_db);
    tx_run_set_span(tx_span_hz(app));  /* TX span ← saved zoom, matching the RX axis */
    cw_push(app);   /* persisted CW keyer speed, hang + sidetone (F6d-1c) */
    tci_apply(app); /* persisted TCI server (F6d-2a; off by default)      */
    tx_update_mic(app);   /* open the mic now if we start in a voice mode (no warm-up lag) */
  } else {
    fprintf(stderr, "TX runtime init failed — RX only\n");
  }

  p2_set_attenuation(app->atten);   /* front-end gain: goes out in the first HP packet */
  s_engine_rate = rate;
  if (p2_rx_start(dev, app->freq, rate, feed_cb, NULL) != 0) {
    fprintf(stderr, "p2_rx_start failed\n");
    if (app->tx_ready) { tx_run_stop(); app->tx_ready = 0; }
    if (app->mic_open) { mic_stop(); app->mic_open = 0; }
    if (app->audio_ok) { demod_destroy(); audio_stop(); }
    analyzer_destroy();
    return;
  }
  app->engine_ok = 1;
  app->connected = 1;
}

/* SIGINT/SIGTERM → quit the run loop so main()'s cleanup (p2_rx_stop) runs and
 * the radio is released — otherwise Ctrl-C would leave it streaming. */
static gboolean on_signal(gpointer data) {
  g_application_quit(G_APPLICATION(data));
  return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
  g_orig_argv = argv;   /* for the in-app "Restart now" toast */
  App app;
  memset(&app, 0, sizeof(app));
  app.wf = waterfall_new();
  app.tx_wf = waterfall_new();   /* TX waterfall (transmitted spectrum, F6a) */

  int server_mode = (argc > 1) && (strcmp(argv[1], "--server") == 0);

  if (server_mode) {
    const char *host = (argc > 2) ? argv[2] : "127.0.0.1";
    int         port = (argc > 3) ? atoi(argv[3]) : 50000;
    const char *pwd  = (argc > 4) ? argv[4] : getenv("PIHPSDR_PWD");
    if (!pwd) { pwd = ""; }
    app.radio_mode = 0;
    app.client = client_new(host, port, pwd);
    app.conn_err = client_connect(app.client);
    if (app.conn_err == CLIENT_OK) {
      app.connected = 1;
      client_start(app.client);
      printf("Connected to %s:%d — streaming.\n", host, port);
    } else {
      fprintf(stderr, "connect failed: %s\n", client_strerror(app.conn_err));
    }
  } else {
    app.radio_mode = 1;
    /* Radio picker (Zeus/piHPSDR-style): broadcast-discover the LAN, let the
     * operator choose. Skipped when SDRFL_RADIO_IP pins the radio, so scripts
     * and gates keep their non-interactive path. */
    const char *eip = getenv("SDRFL_RADIO_IP");
    if (!(eip && *eip)) {
      Settings pst; memset(&pst, 0, sizeof(pst));
      settings_load(&pst);                       /* only for the last-used IP */
      if (!picker_run(pst.ip, app.picked_ip, sizeof(app.picked_ip))) {
        waterfall_free(app.wf);                  /* closed the picker = quit */
        waterfall_free(app.tx_wf);
        return 0;
      }
    }
    wisdom_ensure();      /* first run: build FFTW wisdom (progress window) so the
                             analyzer's PATIENT plans don't freeze on deep zoom */
    start_radio(&app);
  }

  AdwApplication *gtkapp = adw_application_new("cz.ok1br.sdr_for_linux",
                                               G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(gtkapp, "activate", G_CALLBACK(on_activate), &app);
  g_unix_signal_add(SIGINT, on_signal, gtkapp);
  g_unix_signal_add(SIGTERM, on_signal, gtkapp);
  int status = g_application_run(G_APPLICATION(gtkapp), 0, NULL);

  g_object_unref(gtkapp);
  if (app.radio_mode) {
    if (app.save_timer_id) { g_source_remove(app.save_timer_id); app.save_timer_id = 0; }
    if (app.rate > 0) { Settings s; app_to_settings(&app, &s); settings_save(&s); }
    tci_server_stop();                     /* drop TCI clients before the engine goes away */
    if (app.tx_ready) { tx_run_stop(); }   /* unkey + stop TX thread before the socket closes */
    if (app.mic_open) { mic_stop(); app.mic_open = 0; }
    if (app.engine_ok) { p2_rx_stop(); }
    if (app.audio_ok)  { demod_destroy(); audio_stop(); }
    if (app.engine_ok) { analyzer_destroy(); }
  } else {
    client_free(app.client);
  }
  waterfall_free(app.wf);
  waterfall_free(app.tx_wf);
  return status;
}
