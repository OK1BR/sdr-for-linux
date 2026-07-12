/*
 * sdr-for-linux — TX runtime. See tx_run.h.
 *
 * This is sdrfl-txkey's keying loop (F5, validated live on the ANAN G2E) turned
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
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#include "protocol1.h"
#include "protocol2.h"
#include "tx.h"
#include "tx_meter.h"
#include "tx_gate.h"
#include "tx_analyzer.h"
#include "tx_run.h"
#include "mic_pw.h"   /* live host-soundcard mic → exciter while MOX is keyed (F6c) */
#include "cw_gen.h"   /* CW Morse envelope → keyed carrier (F6d)                    */
#include "ps.h"       /* PureSignal runtime — key hand-off + status (F7/PS-2)       */
#include "demod.h"    /* DEMOD_* mode ids + demod_monitor_push (TX monitor)         */

/* SDRFL_LAT_DEBUG=1 → one-line monotonic-µs event marks for the latency audit
 * (contest notes #3/#4/#5). No behaviour change; zero cost when off. */
static int lat_on(void) {
  static int v = -1;
  if (v < 0) { const char *e = g_getenv("SDRFL_LAT_DEBUG"); v = (e && e[0] == '1'); }
  return v;
}
#define LAT(...) do { if (lat_on()) { \
    fprintf(stderr, "LAT %lld ", (long long)g_get_monotonic_time()); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } } while (0)

#define CW_SIDETONE_HZ  700     /* sidetone pitch default, Hz — tx_run_set_sidetone      */
#define CW_SIDETONE_CDB (-2000) /* sidetone level default (dB × 100): −20 dBFS ≈ piHPSDR's
                                   default sidetone volume 50/127 (0.00196·vol·env peaks
                                   at 0.098, transmitter.c:1491) — NOT full scale, which
                                   overdrives the speaker through the voice monitor gain */

#define TX_MODE_IS_CW(m) ((m) == DEMOD_CWL || (m) == DEMOD_CWU)
/* CW carrier amplitude: on P2 the TXA chain's compensating CFIR attenuates a
 * DC signal to 0.896 of full scale, so the direct-synthesized carrier must
 * match it (piHPSDR transmitter.c:1737-1747). P1 has no CFIR → 1.0 (:1722-32). */
#define CW_IQ_AMP        (s_p1 ? 1.0 : 0.896)
#define CW_CUTOFF_US     (20 * 1000000LL)   /* 20 s continuous-key hardware backstop */
#define CW_PTT_DELAY_US  30000   /* key-on → first RF: MOX must land on the wire first
                                    (piHPSDR cw_keyer_ptt_delay default, radio.c:203) */

/* WDSP TX channel output rate + CW/sidetone block size — protocol-dependent
 * since HL2 TX (T2): P2 = 192 k / 2048-pair blocks, P1 = 48 k / 512. The
 * defines are the P2 values and the ARRAY BOUND (maximum); the runtime values
 * live in s_iq_rate/s_iq_block, set by tx_run_start. */
#define TX_IQ_RATE   192000  /* P2 rate; also matches tx.c's P2 chain          */
#define TX_IQ_BLOCK  2048    /* max IQ pairs per on_tx_iq block (P2 512*192/48)*/
static int s_p1;             /* Protocol-1 radio (HL2) — set by tx_run_start   */
static int s_iq_rate  = TX_IQ_RATE;
static int s_iq_block = TX_IQ_BLOCK;

#define TX_FLO        150.0    /* default TX audio passband low edge  (Hz)               */
#define TX_FHI        2850.0   /* default TX audio passband high edge (Hz)               */
#define TX_MODE_DFLT  DEMOD_USB /* channel is created with this; mode set per key        */

/* TX bandpass edges in IQ space for a WDSP mode, from the operator's audio edges
 * (positive lo<hi, e.g. 150/2850 or an eSSB 50/4000). The SIGN of the passband is
 * the ONLY sideband selector in the TXA chain (SetTXAMode just switches the AM/FM
 * modulators), so LSB must get (-high,-low) — mirrors piHPSDR tx_set_filter
 * (transmitter.c:2161 @974acba): USB (low,high), LSB (-high,-low), AM (-high,high),
 * CW ±150 (WDSP is bypassed for CW; kept for the tidy channel state). */
static void tx_passband(int mode, double lo, double hi, double *flo, double *fhi) {
  if (!(lo > 0.0) || !(hi > lo + 100.0)) { lo = TX_FLO; hi = TX_FHI; }  /* sane fallback */
  switch (mode) {
  case DEMOD_LSB:
  case DEMOD_DIGL: *flo = -hi;    *fhi = -lo;   break;  /* piHPSDR: LSB+DIGL mirror */
  case DEMOD_CWL:
  case DEMOD_CWU:  *flo = -150.0; *fhi = 150.0; break;
  case DEMOD_AM:   *flo = -hi;    *fhi = hi;    break;  /* carrier ± high */
  default:         *flo = lo;     *fhi = hi;    break;  /* USB + DIGU     */
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
  double pa_watts;         /* PA rating (open-antenna scaling; 0 = 100 W) */
  int    allow_oob, region, mode;
  int    ptt_enabled;      /* footswitch (radio PTT input) may raise MOX intent */
  char   country_key[8];
  double pa_trim[11];
  double tx_flo, tx_fhi;   /* TX audio passband edges (Hz, positive lo<hi) */
} tx_cfg_i;

static GThread          *s_thread;
static volatile int      s_running;        /* g_atomic */
static volatile int      s_want_mox;       /* g_atomic */
static volatile int      s_want_tune;      /* g_atomic */
static volatile int      s_want_tt;        /* g_atomic: two-tone test wants key (⛔ delta #2:
                                              keys via tx_gate like MOX, PS-SCOPE §6) */
static volatile int      s_want_cw;        /* g_atomic: CW break-in wants key (feed thread) */
static volatile int      s_mode;           /* g_atomic: current WDSP/demod mode           */
static volatile int      s_monitor;        /* g_atomic: TX monitor (self-listen) on/off   */
static volatile int      s_mon_raw;        /* g_atomic: monitor source — 0 = processed TX
                                              audio (WDSP output, faithful to the air),
                                              1 = raw mic (zero latency, piHPSDR style)   */
static int               s_mon_skip;       /* feed-thread only: on_tx_iq call is CW —
                                              the sidetone branch monitors instead        */

/* SDRFL_TX_DUMP=<dir>: append raw float32 mono streams for TX-audio forensics
 * — mic48k.f32 (the mic exactly as fed to WDSP, 48 kHz) and txre.f32 (Re of
 * the WDSP IQ output = the transmitted sideband audio, at the DUC rate).
 * Feed-thread only, opened on first use, flushed at UNKEY. Debug tool. */
static FILE *s_dump_mic;
static FILE *s_dump_tx;
static int   s_dump_init;
static void dump_ensure(void) {
  const char *d;
  s_dump_init = 1;
  if (!(d = g_getenv("SDRFL_TX_DUMP")) || !d[0]) { return; }
  char p[512];
  g_snprintf(p, sizeof p, "%s/mic48k.f32", d);
  s_dump_mic = fopen(p, "wb");
  g_snprintf(p, sizeof p, "%s/txre.f32", d);
  s_dump_tx = fopen(p, "wb");
  fprintf(stderr, "tx: DUMP active — %s/{mic48k,txre}.f32\n", d);
}

/* External TX audio (TCI, F6d-2c): mono 48 k SPSC ring — producer = the TCI
 * service thread (tx_run_ext_push), consumer = the feed loop while keyed with
 * the external source selected. ⛔ Richard's digi rule: this audio is CLEAN —
 * the GUI forces PROC/leveler/gate OFF in DIGU/DIGL, nothing may bend it. */
#define EXT_FRAMES 16384u
#define EXT_MASK   (EXT_FRAMES - 1u)
static float            ext_ring[EXT_FRAMES];
static _Atomic unsigned ext_head, ext_tail;
static volatile int     s_ext_src;              /* g_atomic: TCI feeds TX audio  */
static void (*_Atomic   s_ext_cb)(int nsamples); /* chrono clock (feed thread)   */
/* Digi TX-audio meter (display only, contest note #7): peak + clip count since
 * the last meter slot. Producer = the TCI service thread (tx_run_ext_push),
 * consumer = gate_slot (exchange-to-zero). The audio itself is untouched —
 * ⛔ the digi clean-chain rule holds. */
static _Atomic unsigned s_ext_pk_u;     /* peak |sample| × 1e6 */
static _Atomic int      s_ext_clip_n;   /* samples at/above full scale */

static cw_gen           *s_cw;             /* CW envelope generator (192k), under s_cw_lock */
static GMutex            s_cw_lock;
static volatile int      s_cw_hang_ms = 250;   /* g_atomic: break-in hang time (ms)         */
static gint64            s_cw_hang_deadline;   /* break-in hang end (monotonic µs); written
                                                  by the feed thread under s_cw_lock, read
                                                  by tx_run_cw_progress under the same lock
                                                  (the feed thread reads its own writes)    */
static volatile int      s_st_hz  = CW_SIDETONE_HZ;  /* g_atomic: sidetone pitch (Hz)       */
static volatile int      s_st_cdb = CW_SIDETONE_CDB; /* g_atomic: sidetone level (dB × 100) */

static tx_cfg_i          s_cfg;            /* under s_cfg_lock */
static GMutex            s_cfg_lock;
static long long         s_freq;           /* under s_freq_lock */
static GMutex            s_freq_lock;
static tx_run_status     s_status;         /* under s_status_lock */
static GMutex            s_status_lock;

static p2_tx_iq_framer   s_framer;
static int               s_log_ctr;         /* ~1 Hz keyed power log throttle */

/* TX IQ from the WDSP TX channel → the protocol's wire path (P2: frame →
 * port-1029 socket; P1: EP2 payload ring), and → the TX panadapter analyzer
 * (spectrum of what we transmit). */
static void on_tx_iq(const double *iq, int n_pairs, void *u) {
  (void)u;

  if (s_p1) { p1_tx_iq_push(iq, n_pairs); }
  else      { p2_tx_iq_framer_push(&s_framer, iq, n_pairs); }

  tx_analyzer_feed(iq, n_pairs);

  if (s_dump_tx && !s_mon_skip) {        /* forensics: the wire-bound audio */
    static float db[TX_IQ_BLOCK];
    int nd = n_pairs > TX_IQ_BLOCK ? TX_IQ_BLOCK : n_pairs;
    for (int i = 0; i < nd; i++) { db[i] = (float)iq[2 * i]; }
    fwrite(db, sizeof(float), (size_t)nd, s_dump_tx);
  }

  /* Faithful TX monitor: Re(IQ) of the WDSP output IS the processed sideband
   * audio — exactly what keys the exciter, gate/PROC/TX-filter/ALC included
   * (both sidebands: a real signal's spectrum is conjugate-symmetric). Only
   * when the monitor source is "processed"; CW is skipped (its sidetone
   * branch already plays the keying envelope). The one-pole DC block kills
   * the TUNE carrier (0 Hz) so TUNE stays silent like before. The IQ here is
   * pre drive-scaling on both protocols, so the level does not follow Drive. */
  if (g_atomic_int_get(&s_monitor) && !g_atomic_int_get(&s_mon_raw) && !s_mon_skip) {
    static float  mb[TX_IQ_BLOCK];
    static double x1, y1;
    double r = 1.0 - 125.6 / (double)s_iq_rate;    /* ~20 Hz cutoff */
    if (n_pairs > TX_IQ_BLOCK) { n_pairs = TX_IQ_BLOCK; }
    for (int i = 0; i < n_pairs; i++) {
      double x = iq[2 * i];
      double y = x - x1 + r * y1;
      x1 = x; y1 = y;
      mb[i] = (float)y;
    }
    demod_monitor_absolute(0);
    demod_monitor_push(mb, n_pairs, s_iq_rate);
  }
}

/* ⛔ Keying dispatch — the ONE place both protocols' wire state is applied.
 * The generic p2_tx_state is the master; on P1 it maps to p1_tx_state with
 * the drive split into the HL2's HW-attenuator + IQ-scale pair (P1-TX-SCOPE
 * §1). NULL unkeys both. Antenna/tx_freq have no P1 equivalent (no Alex; the
 * 0x02 register already tracks the VFO). */
static void engine_set_tx_state(const p2_tx_state *tx) {
  if (!s_p1) {
    p2_set_tx_state(tx);
    return;
  }

  if (!tx) {
    p1_set_tx_state(NULL, 0.0);
    return;
  }

  p1_tx_state s;
  double scale;
  memset(&s, 0, sizeof s);
  s.mox        = tx->mox || tx->tune;   /* P1: TUNE keys via the same MOX bit */
  s.pa_enabled = tx->pa_enabled;
  s.in_band    = tx->in_band;
  s.tune       = tx->tune;
  p1_drive_split(tx->in_band ? tx->drive : 0, &s.drive_att, &scale);
  p1_set_tx_state(&s, scale);
}

static void publish(const tx_run_status *st) {
  g_mutex_lock(&s_status_lock);
  s_status = *st;
  g_mutex_unlock(&s_status_lock);
}

/* Lock-free keyed flag for per-IQ-block consumers (the GUI's RX-audio router
 * keys the RX-on-TX mute off this — polling the mutexed status from the GUI
 * frame tick added ~110 ms to every unkey, contest note #5). */
static volatile int s_keyed_pub;
int tx_run_keyed(void) { return g_atomic_int_get(&s_keyed_pub); }

/* One meter+gate slot (~20 Hz, plus an immediate run on any keying-intent
 * edge). Returns the keyed decision and applies all wire state
 * (p2_set_tx_state) + DSP transitions (tone/run) on key/unkey edges.
 * fresh_meter: only the regular 50 ms slots read the coupler — an
 * edge-triggered run must not feed a duplicate sample into the SWR
 * 2-consecutive-readings filter (and p2_tx_fwd_max_take() is decay-on-read,
 * single consumer). Edge runs evaluate the gate on the meter's last state. */
static int gate_slot(int *prev_keyed, int *prev_want, const float *silence,
                     int *keyed_mox, int *keyed_cw, int fresh_meter) {
  g_mutex_lock(&s_cfg_lock);  tx_cfg_i cfg = s_cfg;  g_mutex_unlock(&s_cfg_lock);

  int is_cw     = TX_MODE_IS_CW(g_atomic_int_get(&s_mode));
  int want_cw   = g_atomic_int_get(&s_want_cw);      /* CW break-in (feed thread) */
  /* Footswitch (radio PTT input, HP-status byte 4 bit 0): a remote MOX button.
   * Voice modes only — in CW the break-in keys, in DIGU/DIGL the TCI client
   * keys (and the pedal would put mic audio on a digi frequency). Same intent
   * OR as the GUI button; tx_gate still decides whether it actually keys. */
  int is_voice  = !is_cw && cfg.mode != DEMOD_DIGU && cfg.mode != DEMOD_DIGL;
  int want_ptt  = cfg.ptt_enabled && is_voice &&
                  (s_p1 ? p1_ptt_get() : p2_ptt_get());
  /* Two-tone test (PS calibration / IMD check) — keys exactly like MOX through
   * the gate (⛔ approved delta #2); excluded in CW (the feed loop would emit
   * a CW carrier instead of the WDSP chain). */
  int want_tt   = g_atomic_int_get(&s_want_tt) && !is_cw;
  /* CW keys through the exciter exactly like MOX (MOX bit + drive, SWR-protected). */
  int want_mox  = g_atomic_int_get(&s_want_mox) || want_ptt || want_tt || (is_cw && want_cw);
  int want_tune = g_atomic_int_get(&s_want_tune);
  int want      = want_mox || want_tune;

  /* Fresh key press (want rising) → clear any stale trip latch + SWR history, so
   * a new deliberate key starts clean. While the operator keeps holding after a
   * trip (no rising edge) the latch is preserved by tx_gate. */
  if (want && !*prev_want) { tx_gate_reset(); tx_meter_reset(); }
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
  gc.pa_watts        = cfg.pa_watts;
  gc.allow_oob       = cfg.allow_oob;
  gc.region          = (bp_region_t)cfg.region;
  gc.country_key     = cfg.country_key;
  gc.temp_limit_c    = s_p1 ? 60.0 : 0.0;   /* HL2 thermal trip (P1-TX-SCOPE §2) */

  /* One real coupler reading per slot → tx_gate's 2-consecutive SWR filter sees
   * genuine consecutive samples (never a re-run on stale data). The wattmeter-trim
   * curve is (re)installed here so it stays owned by this worker thread. */
  p2_telemetry t; memset(&t, 0, sizeof t);
  static double s_temp_c;                /* last P1 die temperature (this thread) */
  if (fresh_meter) {
    tx_meter_set_trim(cfg.pa_trim);
    if (s_p1) {
      int fwd = 0, rev = 0, fu = 0, fo = 0;
      p1_get_tx_meters(&fwd, &rev, &s_temp_c, &fu, &fo);
      /* HL2s with a reverse-wound current-sense transformer report fwd/rev
       * swapped — piHPSDR swaps them back (transmitter.c:701-708); harmless
       * on correct units (during TX fwd >> rev). */
      if (rev > fwd) { int tmp = fwd; fwd = rev; rev = tmp; }
      tx_meter_update(fwd, rev, p1_tx_fwd_max_take(), is_6m);
      t.fwd_raw = fwd; t.rev_raw = rev;  /* keyed-log lines below reuse t */
      /* TX FIFO health counters. ⚠ OPEN QUESTION (first live TX 2026-07-12):
       * they climb ~30/s on BOTH sides while the RF is demonstrably clean
       * (steady carrier, clean CW envelope) — on gw 73.2 the C3 top bits look
       * like fill-level watermark indicators rather than latched events.
       * Verify against the HL2 gateware source before trusting them; until
       * then they are SDRFL_LAT_DEBUG diagnostics only. */
      static int pu, po;
      if (lat_on() && *prev_keyed && (fu != pu || fo != po)) {
        fprintf(stderr, "tx: HL2 TX-FIFO under=%d over=%d\n", fu, fo); fflush(stderr);
      }
      pu = fu; po = fo;
    } else {
      p2_get_telemetry(&t);
      tx_meter_update(t.fwd_raw, t.rev_raw, p2_tx_fwd_max_take(), is_6m);
    }
    if (lat_on()) {                   /* RF-on-the-coupler edges (radio side) */
      static double lat_prev_fwd;
      double fw = tx_meter_fwd_w();
      if (fw >= 1.0 && lat_prev_fwd < 1.0) { LAT("fwd_rf %.1f W", fw); }
      if (fw <  1.0 && lat_prev_fwd >= 1.0) { LAT("fwd_drop"); }
      lat_prev_fwd = fw;
    }
  }

  tx_gate_in in = { .want_mox = want_mox, .want_tune = want_tune, .freq_hz = freq,
                    .swr = tx_meter_swr(), .fwd_w = tx_meter_fwd_w(), .rev_w = tx_meter_rev_w(),
                    .stale_reading = !fresh_meter, .temp_c = s_temp_c };
  tx_gate_result r;
  tx_gate_evaluate(&gc, &in, &r);
  int keyed = r.keyed;

  int cw_key = is_cw && r.state.mox && !r.state.tune;   /* MOX-keyed in a CW mode = CW */
  double flo, fhi;
  tx_passband(cfg.mode, cfg.tx_flo, cfg.tx_fhi, &flo, &fhi);
  /* Two-tone generator follows the keyed state (single tx thread → static is
   * safe). 700+1900 Hz, negated for the LSB family (piHPSDR transmitter.c:
   * 2918-2928). Toggleable mid-over: the operator A/Bs PureSignal against a
   * steady spectrum. */
  static int tt_applied;
  double tt_sign = (cfg.mode == DEMOD_LSB || cfg.mode == DEMOD_DIGL) ? -1.0 : 1.0;
  int tt_want_now = want_tt && keyed && r.state.mox && !r.state.tune && !cw_key;
  if (keyed && !*prev_keyed) {
    /* KEY ON: bring up the DSP first, then assert the gate-approved wire state.
     * (IQ starts on the next feed; the radio ignores it until MOX/relay land, so
     * no RF can precede a consistent TX state.) CW bypasses WDSP — the feed loop
     * emits the shaped carrier IQ directly; here we just assert the wire state. */
    tx_dsp_set_mode(cfg.mode, flo, fhi);
    tx_dsp_tune_tone(r.state.tune ? 1 : 0, 0.0);   /* TUNE = post-gen carrier */
    if (tt_want_now) { tx_dsp_two_tone(1, tt_sign * 700.0, tt_sign * 1900.0); }
    tt_applied = tt_want_now;
    if (r.state.mox && !cw_key) {
      if (g_atomic_int_get(&s_ext_src)) {
        /* TCI audio: drop stale ring content + prime the client's sender with
         * a few blocks of lead (TX_STREAM_AUDIO_BUFFERING is 50 ms default). */
        atomic_store_explicit(&ext_tail,
            atomic_load_explicit(&ext_head, memory_order_acquire),
            memory_order_release);
        void (*ecb)(int) = atomic_load_explicit(&s_ext_cb, memory_order_acquire);
        if (ecb) { ecb(4 * FEED_BLOCK); }
      } else {
        mic_flush();                      /* voice: start on fresh mic audio */
      }
    }
    tx_dsp_run(1);
    engine_set_tx_state(&r.state);
    g_atomic_int_set(&s_keyed_pub, 1);   /* RX mute router keys off this NOW */
    LAT("key_on %s", r.state.tune ? "TUNE" : (cw_key ? "CW" : "MOX"));
    fprintf(stderr, "tx: KEY %s  freq=%lld Hz  PA=%s  ANT%d  drive=%d/255\n",
            r.state.tune ? "TUNE" : (cw_key ? "CW" : "MOX"), freq,
            r.state.pa_enabled ? "ON" : "off", cfg.antenna + 1, r.state.drive);
    fflush(stderr);

    /* Zero the transport counters at key-on so the UNKEY stats line shows
     * THIS over only — while idle the mic ring legitimately overflows (the
     * capture runs, nobody pulls, mic_flush discards the backlog anyway). */
    {
      int d0, s0;
      mic_stats_take(&d0, &s0);

      if (s_p1) { p1_tx_ring_stats_take(&d0, &s0); }
    }
  } else if (!keyed && *prev_keyed) {
    /* KEY OFF (operator release OR protection trip): drop MOX first, stop the
     * tone, flush a little audio, then stop the channel. */
    engine_set_tx_state(NULL);
    /* Unkey flag FIRST: the WDSP flush below blocks ~100 ms (measured) and the
     * RX-audio settle window must not wait for it — it starts at the T/R drop. */
    g_atomic_int_set(&s_keyed_pub, 0);
    LAT("key_off");
    tx_dsp_tune_tone(0, 0.0);
    if (tt_applied) { tx_dsp_two_tone(0, 0.0, 0.0); tt_applied = 0; }
    for (int i = 0; i < 4; i++) { tx_dsp_feed_mic(silence, FEED_BLOCK); }
    tx_dsp_run(0);
    fprintf(stderr, "tx: UNKEY (%s)\n", (r.reason && r.reason[0]) ? r.reason : "release");

    /* Per-over audio-transport health (buzz/click forensics, 2026-07-12):
     * silent when everything is clean. mic shorts/drops cover the capture
     * ring on both protocols; the IQ-ring numbers are P1's EP2 sender. */
    {
      int md, ms, ru = 0, rd = 0;
      mic_stats_take(&md, &ms);

      if (s_p1) { p1_tx_ring_stats_take(&ru, &rd); }
      else      { p2_txiq_ring_stats_take(&rd); }

      if (md || ms || ru || rd) {
        fprintf(stderr, "tx: over stats — mic drops=%d shorts=%d, "
                "iq ring under=%d drops=%d\n", md, ms, ru, rd);
      }

      if (s_dump_mic) { fflush(s_dump_mic); }
      if (s_dump_tx)  { fflush(s_dump_tx); }
    }

    fflush(stderr);
  } else if (keyed) {
    engine_set_tx_state(&r.state);   /* refresh (frequency/antenna may have changed) */
    tx_dsp_set_mode(cfg.mode, flo, fhi);   /* live TX-filter change while keyed —
                                              WDSP setters no-op when unchanged */
    if (tt_want_now != tt_applied) {       /* two-tone toggled mid-over */
      tx_dsp_two_tone(tt_want_now, tt_sign * 700.0, tt_sign * 1900.0);
      tt_applied = tt_want_now;
    }
    if (fresh_meter && ++s_log_ctr % 20 == 0) {   /* ~1 Hz while keyed (regular slots) */
      ps_status pl; ps_get_status(&pl);
      if (pl.on) {
        fprintf(stderr, "tx: fwd=%.2f W  rev=%.2f W  SWR=%.2f  (fwd_raw=%d rev_raw=%d)"
                "  PS: state=%d fdbk=%d cals=%d sln=0x%x getpk=%.3f %s\n",
                tx_meter_fwd_w(), tx_meter_rev_w(), tx_meter_swr(), t.fwd_raw, t.rev_raw,
                pl.state, pl.fdbk, pl.cals, pl.sln, pl.getpk,
                pl.correcting ? "CORRECTING" : "-");
      } else {
        fprintf(stderr, "tx: fwd=%.2f W  rev=%.2f W  SWR=%.2f  (fwd_raw=%d rev_raw=%d)\n",
                tx_meter_fwd_w(), tx_meter_rev_w(), tx_meter_swr(), t.fwd_raw, t.rev_raw);
      }
      fflush(stderr);
    }
  }

  /* PureSignal key hand-off: MOX/TUNE keyed but NOT the CW carrier (WDSP is
   * bypassed in CW → feedback is meaningless, piHPSDR transmitter.c:2114-2120).
   * Every slot — ps_key/ps_tune edge-detect internally; SetPSMox is lock-free.
   * TUNE parks PS (reset, resumed after — piHPSDR radio.c:2728). */
  ps_tune(keyed && r.state.tune);
  ps_key(keyed && !cw_key);
  ps_auto_tick(keyed && !cw_key, tt_applied);   /* Thetis: att steps on any TX;
                                                   stall detector 2T-only */

  tx_run_status st;
  memset(&st, 0, sizeof st);
  st.running = 1;
  st.keyed   = keyed;
  st.tune    = keyed && r.state.tune;
  st.mox     = keyed && r.state.mox;
  st.tripped = r.tripped;
  st.allowed = r.allowed;
  st.high_swr = r.high_swr;
  st.fwd_w     = tx_meter_fwd_w();
  st.fwd_pep_w = tx_meter_fwd_pep_w();
  st.rev_w     = tx_meter_rev_w();
  st.swr       = tx_meter_swr();
  if (keyed) { tx_dsp_get_meters(&st.mic_pk, &st.alc_gain, &st.lvlr_gain); }  /* live WDSP TX level */
  else       { st.mic_pk = -99.0; st.alc_gain = 0.0; st.lvlr_gain = 0.0; }
  /* Digi (TCI) TX-audio level: peak over the samples pushed since the last
   * slot (meter only — the audio is untouched, ⛔ digi clean-chain rule). */
  unsigned epk = atomic_exchange_explicit(&s_ext_pk_u, 0u, memory_order_relaxed);
  int      ecl = atomic_exchange_explicit(&s_ext_clip_n, 0, memory_order_relaxed);
  st.ext_pk   = epk > 0u ? 20.0 * log10((double)epk / 1000000.0) : -99.0;
  st.ext_clip = ecl > 0;
  ps_status pss; ps_get_status(&pss);          /* zeros when PS is off */
  st.ps_on         = pss.on;
  st.ps_correcting = pss.correcting;
  st.ps_state      = pss.state;
  st.ps_fdbk       = pss.fdbk;
  st.ps_att        = pss.att;
  g_strlcpy(st.reason, r.reason ? r.reason : "", sizeof st.reason);
  if (lat_on() && keyed != *prev_keyed) { LAT("keyed_pub %d", keyed); }
  g_atomic_int_set(&s_keyed_pub, keyed);
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
  float  cwenv[TX_IQ_BLOCK];       /* CW envelope @ the TX IQ rate (max bound) */
  double cwiq[2 * TX_IQ_BLOCK];    /* CW IQ (I=amp·env, Q=0) */
  memset(silence, 0, sizeof silence);
  int prev_keyed = 0, prev_want = 0, keyed = 0, keyed_mox = 0, keyed_cw = 0;
  int prev_intent = 0;             /* keying-intent fingerprint (edge-triggered gate) */
  gint64 last_gate = 0, next_feed = 0, cw_key_on = 0;

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
      if (cw_content) {   /* deadline write under the lock (tx_run_cw_progress reads it) */
        s_cw_hang_deadline = now + (gint64)g_atomic_int_get(&s_cw_hang_ms) * 1000;
      }
      g_mutex_unlock(&s_cw_lock);
    }
    int want_cw_now = cw_mode && (now < s_cw_hang_deadline) ? 1 : 0;
    if (lat_on()) {
      static int lat_prev_want_cw;
      if (want_cw_now != lat_prev_want_cw) { LAT("want_cw %d", want_cw_now); }
      lat_prev_want_cw = want_cw_now;
    }
    g_atomic_int_set(&s_want_cw, want_cw_now);

    /* Edge-triggered gate (#5): any change in keying intent evaluates the gate
     * NOW instead of waiting out the 50 ms slot (measured 0-41 ms of both the
     * key-on and release budgets). Regular slots keep the meter/SWR cadence;
     * edge runs pass fresh_meter=0 so the coupler is never read off-schedule. */
    int intent = g_atomic_int_get(&s_want_mox)
               | (g_atomic_int_get(&s_want_tune) << 1)
               | (g_atomic_int_get(&s_want_tt)   << 2)
               | (want_cw_now                    << 3)
               | (p2_ptt_get()                   << 4);
    int fresh = now - last_gate >= GATE_US;
    if (fresh || intent != prev_intent) {
      if (fresh) { last_gate = now; }
      prev_intent = intent;
      keyed = gate_slot(&prev_keyed, &prev_want, silence, &keyed_mox, &keyed_cw, fresh);
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
        if (s_cw && !rf_hold) { cw_gen_pull(s_cw, cwenv, s_iq_block); }
        else                  { memset(cwenv, 0, sizeof cwenv); }
        g_mutex_unlock(&s_cw_lock);
        if (lat_on()) {                  /* first/last RF envelope of the over */
          static int lat_rf_live;
          int nz = 0;
          for (int i = 0; i < s_iq_block; i++) { if (cwenv[i] != 0.0f) { nz = 1; break; } }
          if (nz && !lat_rf_live)  { LAT("rf_first"); lat_rf_live = 1; }
          if (!nz && lat_rf_live)  { LAT("rf_last");  lat_rf_live = 0; }
        }
        for (int i = 0; i < s_iq_block; i++) {
          cwiq[2 * i]     = CW_IQ_AMP * (double)cwenv[i];
          cwiq[2 * i + 1] = 0.0;
        }
        s_mon_skip = 1;              /* CW: the sidetone below monitors, not the IQ tap */
        on_tx_iq(cwiq, s_iq_block, NULL);
        s_mon_skip = 0;
        if (g_atomic_int_get(&s_monitor)) {
          /* Sidetone: the SAME envelope that keys the RF, on a local tone. Its
           * trim is the FINAL level — demod_monitor_absolute(1) bypasses the
           * voice-calibrated monitor gain, so the slider has real authority
           * regardless of the monitor/volume settings (piHPSDR parity). */
          static double ph;
          float st[TX_IQ_BLOCK];
          double amp  = pow(10.0, (double)g_atomic_int_get(&s_st_cdb) / 2000.0);
          double step = 2.0 * G_PI * (double)g_atomic_int_get(&s_st_hz) / (double)s_iq_rate;
          for (int i = 0; i < s_iq_block; i++) {
            st[i] = (float)(amp * (double)cwenv[i] * sin(ph));
            ph += step;
            if (ph > 2.0 * G_PI) { ph -= 2.0 * G_PI; }
          }
          demod_monitor_absolute(1);
          demod_monitor_push(st, s_iq_block, s_iq_rate);
        }
      } else if (keyed_mox) {
        /* MOX: pull live mic — or the external TCI ring (digi TX audio) — and
         * pad any underrun with silence so the exciter never stalls (PACE_US
         * real-time cadence must be met every block). */
        int got;
        if (g_atomic_int_get(&s_ext_src)) {
          unsigned t = atomic_load_explicit(&ext_tail, memory_order_relaxed);
          unsigned h = atomic_load_explicit(&ext_head, memory_order_acquire);
          unsigned avail = h - t;
          got = avail < FEED_BLOCK ? (int)avail : FEED_BLOCK;
          for (int i = 0; i < got; i++) { mic[i] = ext_ring[(t + (unsigned)i) & EXT_MASK]; }
          atomic_store_explicit(&ext_tail, t + (unsigned)got, memory_order_release);
          void (*ecb)(int) = atomic_load_explicit(&s_ext_cb, memory_order_acquire);
          if (ecb) { ecb(FEED_BLOCK); }   /* chrono clock: request the next block */
        } else {
          got = mic_pull(mic, FEED_BLOCK);
        }
        for (int i = got; i < FEED_BLOCK; i++) { mic[i] = 0.0f; }
        if (!s_dump_init) { dump_ensure(); }
        if (s_dump_mic) { fwrite(mic, sizeof(float), FEED_BLOCK, s_dump_mic); }
        tx_dsp_feed_mic(mic, FEED_BLOCK);       /* → IQ → framer → port 1029 */
        if (g_atomic_int_get(&s_monitor) && g_atomic_int_get(&s_mon_raw)) {
          /* Raw-mic monitor (opt-in): the untouched mic block, like piHPSDR's
           * audiomonitor (transmitter.c:1953) — zero latency, but NOT what
           * goes on air. Default is the processed tap in on_tx_iq. */
          demod_monitor_absolute(0);
          demod_monitor_push(mic, FEED_BLOCK, tx_dsp_in_rate());
        }
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

int tx_run_start(long long tx_freq_hz, int pan_pixels, int fps, int p1) {
  if (s_thread) { return 0; }   /* already up */

  /* Protocol wire path: P1 = 48 k TX IQ into the EP2 payload ring, 512-pair
   * blocks (same 10.67 ms block duration as P2's 2048 @ 192 k → PACE_US and
   * the CW timing hold unchanged). */
  s_p1       = p1;
  s_iq_rate  = p1 ? 48000 : TX_IQ_RATE;
  s_iq_block = p1 ? 512   : TX_IQ_BLOCK;

  /* SAFE defaults before the thread can look at the config. */
  g_mutex_lock(&s_cfg_lock);
  memset(&s_cfg, 0, sizeof s_cfg);
  s_cfg.pa_enabled     = 0;      /* RF impossible until the operator enables the PA */
  s_cfg.pa_calibration = 53.0;   /* piHPSDR default (G2E-validated); per-band pushed by GUI */
  s_cfg.swr_protect    = 1;
  s_cfg.swr_alarm      = 3.0;
  s_cfg.pa_watts       = 100.0;  /* PA rating; per-device value pushed by the GUI */
  s_cfg.mode           = TX_MODE_DFLT;
  s_cfg.tx_flo         = TX_FLO;
  s_cfg.tx_fhi         = TX_FHI;
  s_cfg.country_key[0] = '\0';
  for (int i = 0; i < 11; i++) { s_cfg.pa_trim[i] = i * 10.0; }  /* G2E identity curve (PA_100W: piHPSDR radio.c:1330) */
  g_mutex_unlock(&s_cfg_lock);

  g_mutex_lock(&s_freq_lock); s_freq = tx_freq_hz; g_mutex_unlock(&s_freq_lock);

  g_atomic_int_set(&s_want_mox, 0);
  g_atomic_int_set(&s_want_tune, 0);
  g_atomic_int_set(&s_want_cw, 0);
  g_atomic_int_set(&s_mode, TX_MODE_DFLT);

  /* CW envelope generator @192k (the TX IQ rate) — safe defaults; GUI pushes the
   * operator's WPM/weight/ramp. It only makes an envelope; it never keys. */
  g_mutex_lock(&s_cw_lock);
  if (!s_cw) { s_cw = cw_gen_new(s_iq_rate, 20, 50.0, 9.0); }
  else       { cw_gen_flush(s_cw); }
  s_cw_hang_deadline = 0;   /* no hang held over from a previous runtime */
  g_mutex_unlock(&s_cw_lock);

  tx_run_status st; memset(&st, 0, sizeof st); st.running = 1; st.allowed = 1;
  publish(&st);

  /* WDSP TX channel (created stopped) + TX panadapter analyzer + IQ framer to the
   * port-1029 emitter. Both WDSP OpenChannel/XCreateAnalyzer happen here, before
   * RX starts flowing, so they don't race the live RX channel/analyzer. */
  p2_tx_iq_framer_init(&s_framer, p2_tx_iq_socket_emit, NULL);
  double flo, fhi;
  tx_passband(TX_MODE_DFLT, TX_FLO, TX_FHI, &flo, &fhi);
  if (tx_dsp_create(TX_MODE_DFLT, flo, fhi, s_p1, on_tx_iq, NULL) != 0) { return -1; }
  tx_analyzer_create(pan_pixels, s_iq_rate, s_iq_block, fps);
  tx_meter_reset();
  tx_gate_reset();
  ps_start();               /* PureSignal runtime (stays OFF until the GUI arms it) */

  g_atomic_int_set(&s_running, 1);
  s_thread = g_thread_new("sdrfl-tx", tx_thread, NULL);
  return 0;
}

void tx_run_stop(void) {
  if (!s_thread) { return; }
  g_atomic_int_set(&s_want_mox, 0);
  g_atomic_int_set(&s_want_tune, 0);
  g_atomic_int_set(&s_want_tt, 0);
  g_atomic_int_set(&s_want_cw, 0);
  g_atomic_int_set(&s_running, 0);
  g_thread_join(s_thread);
  s_thread = NULL;
  g_atomic_int_set(&s_keyed_pub, 0);
  ps_stop();               /* unhook the feedback callback before the channel dies */
  engine_set_tx_state(NULL);   /* belt-and-braces: ensure the wire state is RX-only */
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
  s_cfg.pa_watts       = cfg->pa_watts;
  s_cfg.allow_oob      = cfg->allow_oob;
  s_cfg.region         = cfg->region;
  s_cfg.mode           = cfg->mode;
  s_cfg.ptt_enabled    = cfg->ptt_enabled;
  s_cfg.tx_flo         = cfg->tx_flo;
  s_cfg.tx_fhi         = cfg->tx_fhi;
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

void tx_run_cw_progress(tx_cw_view *out) {
  if (!out) { return; }
  memset(out, 0, sizeof *out);
  gint64 now = g_get_monotonic_time();
  g_mutex_lock(&s_cw_lock);
  if (s_cw) {
    cw_gen_progress(s_cw, out->text, sizeof out->text, &out->cur);
    out->active = !cw_gen_idle(s_cw);
    if (!out->active) {
      int    hang = g_atomic_int_get(&s_cw_hang_ms);
      gint64 rem  = s_cw_hang_deadline - now;
      if (hang > 0 && rem > 0) {
        out->hang_frac = (double)rem / ((double)hang * 1000.0);
        if (out->hang_frac > 1.0) { out->hang_frac = 1.0; }
      }
    }
  }
  g_mutex_unlock(&s_cw_lock);
}

void tx_run_set_cw(int wpm, double weight, double ramp_ms, int hang_ms) {
  g_mutex_lock(&s_cw_lock);
  if (s_cw) { cw_gen_set_speed(s_cw, wpm, weight); cw_gen_set_ramp(s_cw, ramp_ms); }
  g_mutex_unlock(&s_cw_lock);
  g_atomic_int_set(&s_cw_hang_ms, hang_ms > 0 ? hang_ms : 0);
}

void tx_run_set_sidetone(int pitch_hz, double level_db) {
  g_atomic_int_set(&s_st_hz,  CLAMP(pitch_hz, 100, 2000));
  g_atomic_int_set(&s_st_cdb, (int)lrint(CLAMP(level_db, -40.0, 0.0) * 100.0));
}

void tx_run_set_ext_source(int on) {
  g_atomic_int_set(&s_ext_src, on ? 1 : 0);
  if (!on) {                                   /* drop leftover external audio */
    atomic_store_explicit(&ext_tail,
        atomic_load_explicit(&ext_head, memory_order_acquire),
        memory_order_release);
  }
}

void tx_run_ext_push(const float *mono48k, int n) {
  if (!g_atomic_int_get(&s_ext_src)) { return; }
  /* Digi TX-audio meter: peak + clip over the incoming block (before any ring
   * overflow drop — the meter shows what the client SENDS). Single producer,
   * so a plain compare-and-store max is enough; gate_slot exchanges to zero. */
  float pk = 0.0f; int nclip = 0;
  for (int i = 0; i < n; i++) {
    float a = fabsf(mono48k[i]);
    if (a > pk) { pk = a; }
    if (a >= 0.999f) { nclip++; }
  }
  unsigned v = (unsigned)(pk * 1000000.0f);
  if (v > atomic_load_explicit(&s_ext_pk_u, memory_order_relaxed)) {
    atomic_store_explicit(&s_ext_pk_u, v, memory_order_relaxed);
  }
  if (nclip) { atomic_fetch_add_explicit(&s_ext_clip_n, nclip, memory_order_relaxed); }
  unsigned h = atomic_load_explicit(&ext_head, memory_order_relaxed);
  unsigned t = atomic_load_explicit(&ext_tail, memory_order_acquire);
  for (int i = 0; i < n; i++) {
    if (h - t >= EXT_FRAMES) { break; }        /* full → drop */
    ext_ring[h & EXT_MASK] = mono48k[i];
    h++;
  }
  atomic_store_explicit(&ext_head, h, memory_order_release);
}

void tx_run_set_ext_notify(void (*cb)(int nsamples)) {
  atomic_store_explicit(&s_ext_cb, cb, memory_order_release);
}

void tx_run_set_freq(long long tx_freq_hz) {
  g_mutex_lock(&s_freq_lock); s_freq = tx_freq_hz; g_mutex_unlock(&s_freq_lock);
}

void tx_run_set_mic_gain(double db) { tx_dsp_set_mic_gain(db); }   /* tx_dsp locks internally */

void tx_run_set_comp(int on, double gain_db) { tx_dsp_set_compressor(on, gain_db); }

void tx_run_set_gate(int on, double thresh_db) { tx_dsp_set_gate(on, thresh_db); }

void tx_run_set_monitor(int on) { g_atomic_int_set(&s_monitor, on ? 1 : 0); }
void tx_run_set_monitor_raw(int on) { g_atomic_int_set(&s_mon_raw, on ? 1 : 0); }

void tx_run_set_span(double span_hz) { tx_analyzer_set_span(span_hz); }   /* TX zoom (analyzer locks) */

/* Two-tone test (⛔ delta #2, PS-SCOPE §6): a keying INTENT like MOX — the
 * safety gate still decides. Runs at the current drive; not persisted (a test
 * must never re-arm itself at startup). */
void tx_run_set_twotone(int on) {
  g_atomic_int_set(&s_want_tt, on ? 1 : 0);
}

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
