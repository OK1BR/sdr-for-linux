/*
 * sdr-for-linux — TX runtime. See tx_run.h.
 *
 * This is sdrfl-txkey's keying loop (F5, validated live on the ANAN G1) turned
 * into a persistent worker thread that the GUI drives by intent. It never sends
 * control packets itself: keying is expressed by p2_set_tx_state(&state)/NULL,
 * which the engine's single keepalive thread applies atomically (a consistent
 * {MOX, TX_RELAY, ANT, LPF, BPF, attenuators} set per HP datagram). Only the
 * TX-IQ stream (port 1029) is emitted here, on this thread, via the framer — a
 * separate UDP port, exactly as txkey did.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "protocol2.h"
#include "tx.h"
#include "tx_meter.h"
#include "tx_gate.h"
#include "tx_analyzer.h"
#include "tx_run.h"
#include "mic_pw.h"   /* live host-soundcard mic → exciter while MOX is keyed (F6c) */
#include "cw_gen.h"   /* CW Morse envelope → keyed carrier (F6d)                    */
#include "demod.h"    /* DEMOD_* mode ids (match WDSP TXA_*)                        */

#define TX_MODE_IS_CW(m) ((m) == DEMOD_CWL || (m) == DEMOD_CWU)
#define CW_IQ_AMP        0.896   /* DC-signal gain comp for the radio's DUC CFIR (piHPSDR) */
#define CW_CUTOFF_US     (20 * 1000000LL)   /* 20 s continuous-key hardware backstop */
#define CW_PTT_DELAY_US  30000   /* key-on → first RF: MOX must land on the wire first
                                    (piHPSDR cw_keyer_ptt_delay default, radio.c:203) */

#define TX_IQ_RATE   192000  /* WDSP TX channel output rate (matches tx.c)     */
#define TX_IQ_BLOCK  2048    /* IQ pairs per on_tx_iq block (512*192k/48k)     */

#define TX_FLO        150.0    /* TX audio passband low edge  (Hz)                       */
#define TX_FHI        2850.0   /* TX audio passband high edge (Hz)                       */
#define TX_MODE_DFLT  DEMOD_USB /* channel is created with this; mode set per key        */

/* TX bandpass edges in IQ space for a WDSP mode. The SIGN of the passband is the
 * ONLY sideband selector in the TXA chain (SetTXAMode just switches the AM/FM
 * modulators), so LSB must get (-high,-low) — mirrors piHPSDR tx_set_filter
 * (transmitter.c:2161 @974acba): USB (low,high), LSB (-high,-low), AM (-high,high),
 * CW ±150 (WDSP is bypassed for CW; kept for the tidy channel state). */
static void tx_passband(int mode, double *flo, double *fhi) {
  switch (mode) {
  case DEMOD_LSB: *flo = -TX_FHI; *fhi = -TX_FLO; break;
  case DEMOD_CWL:
  case DEMOD_CWU: *flo = -150.0;  *fhi = 150.0;   break;
  case DEMOD_AM:  *flo = -TX_FHI; *fhi = TX_FHI;  break;   /* carrier ± high */
  default:        *flo = TX_FLO;  *fhi = TX_FHI;  break;   /* USB + future DIGU */
  }
}
#define FEED_BLOCK    512      /* mic samples per fexchange0 (matches tx.c TX_BUFSIZE)   */
#define PACE_US       10667    /* 512 samples @ 48 kHz — real-time TX IQ pacing          */
#define GATE_US       50000    /* gate + meter cadence (~20 Hz, like tx_update_display)  */
#define IDLE_US       10000    /* idle poll period                                       */

/* Internal config: like tx_run_cfg but owns the country_key string. */
typedef struct {
  int    pa_enabled, antenna;
  double drive_w, tune_w, pa_calibration;
  int    swr_protect;
  double swr_alarm;
  int    allow_oob, region, mode;
  char   country_key[8];
  double pa_trim[11];
} tx_cfg_i;

static GThread          *s_thread;
static volatile int      s_running;        /* g_atomic */
static volatile int      s_want_mox;       /* g_atomic */
static volatile int      s_want_tune;      /* g_atomic */
static volatile int      s_want_cw;        /* g_atomic: CW break-in wants key (feed thread) */
static volatile int      s_mode;           /* g_atomic: current WDSP/demod mode           */

static cw_gen           *s_cw;             /* CW envelope generator (192k), under s_cw_lock */
static GMutex            s_cw_lock;
static volatile int      s_cw_hang_ms = 250;   /* g_atomic: break-in hang time (ms)         */

static tx_cfg_i          s_cfg;            /* under s_cfg_lock */
static GMutex            s_cfg_lock;
static long long         s_freq;           /* under s_freq_lock */
static GMutex            s_freq_lock;
static tx_run_status     s_status;         /* under s_status_lock */
static GMutex            s_status_lock;

static p2_tx_iq_framer   s_framer;
static int               s_log_ctr;         /* ~1 Hz keyed power log throttle */

/* TX IQ from the WDSP TX channel → frame → port-1029 socket, and → the TX
 * panadapter analyzer (spectrum of what we transmit). */
static void on_tx_iq(const double *iq, int n_pairs, void *u) {
  (void)u;
  p2_tx_iq_framer_push(&s_framer, iq, n_pairs);
  tx_analyzer_feed(iq, n_pairs);
}

static void publish(const tx_run_status *st) {
  g_mutex_lock(&s_status_lock);
  s_status = *st;
  g_mutex_unlock(&s_status_lock);
}

/* One meter+gate slot (~20 Hz). Returns the keyed decision and applies all wire
 * state (p2_set_tx_state) + DSP transitions (tone/run) on key/unkey edges. */
static int gate_slot(int *prev_keyed, int *prev_want, const float *silence,
                     int *keyed_mox, int *keyed_cw) {
  int is_cw     = TX_MODE_IS_CW(g_atomic_int_get(&s_mode));
  int want_cw   = g_atomic_int_get(&s_want_cw);      /* CW break-in (feed thread) */
  /* CW keys through the exciter exactly like MOX (MOX bit + drive, SWR-protected). */
  int want_mox  = g_atomic_int_get(&s_want_mox) || (is_cw && want_cw);
  int want_tune = g_atomic_int_get(&s_want_tune);
  int want      = want_mox || want_tune;

  /* Fresh key press (want rising) → clear any stale trip latch + SWR history, so
   * a new deliberate key starts clean. While the operator keeps holding after a
   * trip (no rising edge) the latch is preserved by tx_gate. */
  if (want && !*prev_want) { tx_gate_reset(); tx_meter_reset(); }

  g_mutex_lock(&s_cfg_lock);  tx_cfg_i cfg = s_cfg;  g_mutex_unlock(&s_cfg_lock);
  g_mutex_lock(&s_freq_lock); long long freq = s_freq; g_mutex_unlock(&s_freq_lock);
  int is_6m = freq >= 50000000LL && freq < 54000000LL;

  tx_gate_cfg gc;
  memset(&gc, 0, sizeof gc);
  gc.pa_enabled      = cfg.pa_enabled;
  gc.band_disable_pa = 0;                 /* per-band disablePA is F6b */
  gc.antenna         = cfg.antenna;
  gc.drive_byte      = tx_calc_drive_byte(cfg.drive_w, cfg.pa_calibration);
  gc.tune_byte       = tx_calc_drive_byte(cfg.tune_w,  cfg.pa_calibration);
  gc.swr_protect     = cfg.swr_protect;
  gc.swr_alarm       = cfg.swr_alarm;
  gc.allow_oob       = cfg.allow_oob;
  gc.region          = (bp_region_t)cfg.region;
  gc.country_key     = cfg.country_key;

  /* One real coupler reading per slot → tx_gate's 2-consecutive SWR filter sees
   * genuine consecutive samples (never a re-run on stale data). The wattmeter-trim
   * curve is (re)installed here so it stays owned by this worker thread. */
  tx_meter_set_trim(cfg.pa_trim);
  p2_telemetry t; p2_get_telemetry(&t);
  tx_meter_update(t.fwd_raw, t.rev_raw, is_6m);

  tx_gate_in in = { .want_mox = want_mox, .want_tune = want_tune, .freq_hz = freq,
                    .swr = tx_meter_swr(), .fwd_w = tx_meter_fwd_w(), .rev_w = tx_meter_rev_w() };
  tx_gate_result r;
  tx_gate_evaluate(&gc, &in, &r);
  int keyed = r.keyed;

  int cw_key = is_cw && r.state.mox && !r.state.tune;   /* MOX-keyed in a CW mode = CW */
  if (keyed && !*prev_keyed) {
    /* KEY ON: bring up the DSP first, then assert the gate-approved wire state.
     * (IQ starts on the next feed; the radio ignores it until MOX/relay land, so
     * no RF can precede a consistent TX state.) CW bypasses WDSP — the feed loop
     * emits the shaped carrier IQ directly; here we just assert the wire state. */
    double flo, fhi;
    tx_passband(cfg.mode, &flo, &fhi);
    tx_dsp_set_mode(cfg.mode, flo, fhi);
    tx_dsp_tune_tone(r.state.tune ? 1 : 0, 0.0);   /* TUNE = post-gen carrier */
    if (r.state.mox && !cw_key) { mic_flush(); }   /* voice: start on fresh mic audio */
    tx_dsp_run(1);
    p2_set_tx_state(&r.state);
    fprintf(stderr, "tx: KEY %s  freq=%lld Hz  PA=%s  ANT%d  drive=%d/255\n",
            r.state.tune ? "TUNE" : (cw_key ? "CW" : "MOX"), freq,
            r.state.pa_enabled ? "ON" : "off", cfg.antenna + 1, r.state.drive);
    fflush(stderr);
  } else if (!keyed && *prev_keyed) {
    /* KEY OFF (operator release OR protection trip): drop MOX first, stop the
     * tone, flush a little audio, then stop the channel. */
    p2_set_tx_state(NULL);
    tx_dsp_tune_tone(0, 0.0);
    for (int i = 0; i < 4; i++) { tx_dsp_feed_mic(silence, FEED_BLOCK); }
    tx_dsp_run(0);
    fprintf(stderr, "tx: UNKEY (%s)\n", (r.reason && r.reason[0]) ? r.reason : "release");
    fflush(stderr);
  } else if (keyed) {
    p2_set_tx_state(&r.state);   /* refresh (frequency/antenna may have changed) */
    if (++s_log_ctr % 20 == 0) {   /* ~1 Hz while keyed */
      fprintf(stderr, "tx: fwd=%.2f W  rev=%.2f W  SWR=%.2f  (fwd_raw=%d rev_raw=%d)\n",
              tx_meter_fwd_w(), tx_meter_rev_w(), tx_meter_swr(), t.fwd_raw, t.rev_raw);
      fflush(stderr);
    }
  }

  tx_run_status st;
  memset(&st, 0, sizeof st);
  st.running = 1;
  st.keyed   = keyed;
  st.tune    = keyed && r.state.tune;
  st.mox     = keyed && r.state.mox;
  st.tripped = r.tripped;
  st.allowed = r.allowed;
  st.high_swr = r.high_swr;
  st.fwd_w   = tx_meter_fwd_w();
  st.rev_w   = tx_meter_rev_w();
  st.swr     = tx_meter_swr();
  if (keyed) { tx_dsp_get_meters(&st.mic_pk, &st.alc_gain); }   /* live WDSP TX level */
  else       { st.mic_pk = -99.0; st.alc_gain = 0.0; }
  g_strlcpy(st.reason, r.reason ? r.reason : "", sizeof st.reason);
  publish(&st);

  if (keyed_mox) { *keyed_mox = st.mox && !is_cw; }   /* voice mic path */
  if (keyed_cw)  { *keyed_cw  = st.mox &&  is_cw; }    /* CW shaped-carrier path */
  *prev_keyed = keyed;
  *prev_want  = want;
  return keyed;
}

static gpointer tx_thread(gpointer u) {
  (void)u;
  float  silence[FEED_BLOCK];
  float  mic[FEED_BLOCK];
  float  cwenv[TX_IQ_BLOCK];       /* CW envelope @192k */
  double cwiq[2 * TX_IQ_BLOCK];    /* CW IQ (I=amp·env, Q=0) */
  memset(silence, 0, sizeof silence);
  int prev_keyed = 0, prev_want = 0, keyed = 0, keyed_mox = 0, keyed_cw = 0;
  gint64 last_gate = 0, next_feed = 0, cw_hang_deadline = 0, cw_key_on = 0;

  while (g_atomic_int_get(&s_running)) {
    gint64 now = g_get_monotonic_time();

    /* CW break-in bookkeeping (feed thread owns cw_gen). While there is queued/
     * unfinished Morse we want to key; after the last element we hold for the hang
     * time. The gate turns this into a real MOX assertion (SWR-protected). */
    int cw_mode = TX_MODE_IS_CW(g_atomic_int_get(&s_mode));
    int cw_content = 0;
    if (cw_mode) {
      g_mutex_lock(&s_cw_lock);
      cw_content = s_cw && !cw_gen_idle(s_cw);
      g_mutex_unlock(&s_cw_lock);
    }
    if (cw_content) { cw_hang_deadline = now + (gint64)g_atomic_int_get(&s_cw_hang_ms) * 1000; }
    g_atomic_int_set(&s_want_cw, cw_mode && (now < cw_hang_deadline) ? 1 : 0);

    if (now - last_gate >= GATE_US) {
      last_gate = now;
      keyed = gate_slot(&prev_keyed, &prev_want, silence, &keyed_mox, &keyed_cw);
      if (keyed && next_feed == 0) { next_feed = g_get_monotonic_time(); }
      if (!keyed) { next_feed = 0; }
    }

    /* 20 s continuous-key hardware backstop: abort the Morse so break-in unkeys. */
    if (keyed_cw) {
      if (cw_key_on == 0) { cw_key_on = now; }
      else if (now - cw_key_on > CW_CUTOFF_US) {
        g_mutex_lock(&s_cw_lock); if (s_cw) { cw_gen_flush(s_cw); } g_mutex_unlock(&s_cw_lock);
        fprintf(stderr, "tx: CW 20 s cutoff — flushed\n"); fflush(stderr);
      }
    } else { cw_key_on = 0; }

    if (keyed) {
      if (keyed_cw) {
        /* CW: pull the shaped envelope and emit it as a keyed carrier IQ
         * (I = amp·env, Q = 0) straight to the framer — no WDSP. During the hang
         * the generator is idle so env = 0 (carrier off, T/R still held).
         * PTT delay: for the first CW_PTT_DELAY_US after key-on emit zeros WITHOUT
         * consuming the Morse queue — the MOX HP packet has just been kicked onto
         * the wire and the FPGA/T-R relay need a beat, else the radio swallows the
         * first dits (a dot @30 WPM is only 40 ms). */
        int rf_hold = (now - cw_key_on) < CW_PTT_DELAY_US;
        g_mutex_lock(&s_cw_lock);
        if (s_cw && !rf_hold) { cw_gen_pull(s_cw, cwenv, TX_IQ_BLOCK); }
        else                  { memset(cwenv, 0, sizeof cwenv); }
        g_mutex_unlock(&s_cw_lock);
        for (int i = 0; i < TX_IQ_BLOCK; i++) {
          cwiq[2 * i]     = CW_IQ_AMP * (double)cwenv[i];
          cwiq[2 * i + 1] = 0.0;
        }
        on_tx_iq(cwiq, TX_IQ_BLOCK, NULL);
      } else if (keyed_mox) {
        /* MOX/voice: pull live mic; pad any underrun with silence so the exciter
         * never stalls (PACE_US real-time cadence must be met every block). */
        int got = mic_pull(mic, FEED_BLOCK);
        for (int i = got; i < FEED_BLOCK; i++) { mic[i] = 0.0f; }
        tx_dsp_feed_mic(mic, FEED_BLOCK);       /* → IQ → framer → port 1029 */
      } else {
        tx_dsp_feed_mic(silence, FEED_BLOCK);   /* TUNE: post-gen carrier, mic muted */
      }
      next_feed += PACE_US;
      gint64 slack = next_feed - g_get_monotonic_time();
      if (slack > 0) { g_usleep((gulong)slack); }
      else           { next_feed = g_get_monotonic_time(); }   /* fell behind → resync */
    } else {
      g_usleep(IDLE_US);
    }
  }
  return NULL;
}

int tx_run_start(long long tx_freq_hz, int pan_pixels, int fps) {
  if (s_thread) { return 0; }   /* already up */

  /* SAFE defaults before the thread can look at the config. */
  g_mutex_lock(&s_cfg_lock);
  memset(&s_cfg, 0, sizeof s_cfg);
  s_cfg.pa_enabled     = 0;      /* RF impossible until the operator enables the PA */
  s_cfg.pa_calibration = 53.0;   /* G1 default (validated); per-band pushed by GUI */
  s_cfg.swr_protect    = 1;
  s_cfg.swr_alarm      = 3.0;
  s_cfg.mode           = TX_MODE_DFLT;
  s_cfg.country_key[0] = '\0';
  for (int i = 0; i < 11; i++) { s_cfg.pa_trim[i] = i * 10.0; }  /* G1 identity curve (PA_100W: piHPSDR radio.c:1330) */
  g_mutex_unlock(&s_cfg_lock);

  g_mutex_lock(&s_freq_lock); s_freq = tx_freq_hz; g_mutex_unlock(&s_freq_lock);

  g_atomic_int_set(&s_want_mox, 0);
  g_atomic_int_set(&s_want_tune, 0);
  g_atomic_int_set(&s_want_cw, 0);
  g_atomic_int_set(&s_mode, TX_MODE_DFLT);

  /* CW envelope generator @192k (the TX IQ rate) — safe defaults; GUI pushes the
   * operator's WPM/weight/ramp. It only makes an envelope; it never keys. */
  g_mutex_lock(&s_cw_lock);
  if (!s_cw) { s_cw = cw_gen_new(TX_IQ_RATE, 20, 50.0, 9.0); }
  else       { cw_gen_flush(s_cw); }
  g_mutex_unlock(&s_cw_lock);

  tx_run_status st; memset(&st, 0, sizeof st); st.running = 1; st.allowed = 1;
  publish(&st);

  /* WDSP TX channel (created stopped) + TX panadapter analyzer + IQ framer to the
   * port-1029 emitter. Both WDSP OpenChannel/XCreateAnalyzer happen here, before
   * RX starts flowing, so they don't race the live RX channel/analyzer. */
  p2_tx_iq_framer_init(&s_framer, p2_tx_iq_socket_emit, NULL);
  double flo, fhi;
  tx_passband(TX_MODE_DFLT, &flo, &fhi);
  if (tx_dsp_create(TX_MODE_DFLT, flo, fhi, on_tx_iq, NULL) != 0) { return -1; }
  tx_analyzer_create(pan_pixels, TX_IQ_RATE, TX_IQ_BLOCK, fps);
  tx_meter_reset();
  tx_gate_reset();

  g_atomic_int_set(&s_running, 1);
  s_thread = g_thread_new("sdrfl-tx", tx_thread, NULL);
  return 0;
}

void tx_run_stop(void) {
  if (!s_thread) { return; }
  g_atomic_int_set(&s_want_mox, 0);
  g_atomic_int_set(&s_want_tune, 0);
  g_atomic_int_set(&s_want_cw, 0);
  g_atomic_int_set(&s_running, 0);
  g_thread_join(s_thread);
  s_thread = NULL;
  p2_set_tx_state(NULL);   /* belt-and-braces: ensure the wire state is RX-only */
  tx_dsp_destroy();
  tx_analyzer_destroy();
  g_mutex_lock(&s_cw_lock);
  cw_gen_free(s_cw); s_cw = NULL;
  g_mutex_unlock(&s_cw_lock);
  tx_run_status st; memset(&st, 0, sizeof st);
  publish(&st);
}

int tx_run_get_pixels(float *out, int pixels) {
  return tx_analyzer_get_pixels(out, pixels);
}

void tx_run_set_cfg(const tx_run_cfg *cfg) {
  if (!cfg) { return; }
  g_mutex_lock(&s_cfg_lock);
  s_cfg.pa_enabled     = cfg->pa_enabled;
  s_cfg.antenna        = cfg->antenna;
  s_cfg.drive_w        = cfg->drive_w;
  s_cfg.tune_w         = cfg->tune_w;
  s_cfg.pa_calibration = cfg->pa_calibration;
  s_cfg.swr_protect    = cfg->swr_protect;
  s_cfg.swr_alarm      = cfg->swr_alarm;
  s_cfg.allow_oob      = cfg->allow_oob;
  s_cfg.region         = cfg->region;
  s_cfg.mode           = cfg->mode;
  g_strlcpy(s_cfg.country_key, cfg->country_key ? cfg->country_key : "", sizeof s_cfg.country_key);
  for (int i = 0; i < 11; i++) { s_cfg.pa_trim[i] = cfg->pa_trim[i]; }
  g_mutex_unlock(&s_cfg_lock);
  g_atomic_int_set(&s_mode, cfg->mode);   /* CW break-in gates on the mode (feed thread) */
}

/* --- CW (F6d) — queue Morse text; break-in in the feed thread does the keying. --- */
void tx_run_cw_send(const char *text) {
  if (!text) { return; }
  g_mutex_lock(&s_cw_lock);
  if (s_cw) { cw_gen_send_text(s_cw, text); }
  g_mutex_unlock(&s_cw_lock);
}

void tx_run_cw_abort(void) {
  g_mutex_lock(&s_cw_lock);
  if (s_cw) { cw_gen_flush(s_cw); }
  g_mutex_unlock(&s_cw_lock);
}

void tx_run_set_cw(int wpm, double weight, double ramp_ms, int hang_ms) {
  g_mutex_lock(&s_cw_lock);
  if (s_cw) { cw_gen_set_speed(s_cw, wpm, weight); cw_gen_set_ramp(s_cw, ramp_ms); }
  g_mutex_unlock(&s_cw_lock);
  g_atomic_int_set(&s_cw_hang_ms, hang_ms > 0 ? hang_ms : 0);
}

void tx_run_set_freq(long long tx_freq_hz) {
  g_mutex_lock(&s_freq_lock); s_freq = tx_freq_hz; g_mutex_unlock(&s_freq_lock);
}

void tx_run_set_mic_gain(double db) { tx_dsp_set_mic_gain(db); }   /* tx_dsp locks internally */

void tx_run_set_comp(int on, double gain_db) { tx_dsp_set_compressor(on, gain_db); }

void tx_run_set_span(double span_hz) { tx_analyzer_set_span(span_hz); }   /* TX zoom (analyzer locks) */

void tx_run_request(int want_mox, int want_tune) {
  g_atomic_int_set(&s_want_mox,  want_mox  ? 1 : 0);
  g_atomic_int_set(&s_want_tune, want_tune ? 1 : 0);
}

void tx_run_get_status(tx_run_status *out) {
  if (!out) { return; }
  g_mutex_lock(&s_status_lock);
  *out = s_status;
  g_mutex_unlock(&s_status_lock);
}
