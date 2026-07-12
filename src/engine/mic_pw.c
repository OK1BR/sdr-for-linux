/*
 * sdr-for-linux — native PipeWire microphone capture (F6c). See mic_pw.h.
 *
 * The capture twin of audio_pw.c: a pw_thread_loop drives a capture pw_stream on
 * the default source; its RT `on_process` reads the captured F32 mono block and
 * pushes it into a lock-free SPSC ring. The TX feed thread drains it via
 * mic_pull(). No blocking in the RT callback, no lock on the puller. Mono only —
 * the exciter takes one mic channel.
 */
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "mic_pw.h"

/* Mono ring, power-of-two frames. 16384 @ 48k = 341 ms; the puller keeps it far
 * emptier while keyed, and mic_flush() clears the idle backlog at key-down. */
#define RING_FRAMES 16384u
#define RING_MASK   (RING_FRAMES - 1u)

static float          m_ring[RING_FRAMES];
static atomic_uint    m_head;   /* producer (PW capture thread) frame index */
static atomic_uint    m_tail;   /* consumer (TX feed thread) frame index    */
static volatile float m_peak;   /* best-effort level meter (benign races)   */
static atomic_uint    m_drops;  /* frames dropped on a full ring (drift /
                                   stalled-consumer diagnostics)            */
static atomic_uint    m_short;  /* frames the puller wanted but the ring
                                   didn't have (starvation → WDSP gets a
                                   short block; periodic = audible buzz)    */

static struct pw_thread_loop *m_loop;
static struct pw_stream       *m_stream;

/* --- RT callback: drain one captured PW buffer into the ring -------------- */
static void on_process(void *userdata) {
  (void)userdata;
  struct pw_buffer *b = pw_stream_dequeue_buffer(m_stream);
  if (!b) { return; }
  struct spa_buffer *buf = b->buffer;
  if (!buf->datas[0].data) { pw_stream_queue_buffer(m_stream, b); return; }

  const uint8_t *base = buf->datas[0].data;
  uint32_t off  = buf->datas[0].chunk->offset;
  uint32_t size = buf->datas[0].chunk->size;
  const float *src = (const float *)(base + off);
  uint32_t nframes = size / sizeof(float);            /* mono F32 */

  unsigned h = atomic_load_explicit(&m_head, memory_order_relaxed);
  unsigned t = atomic_load_explicit(&m_tail, memory_order_acquire);
  unsigned space = RING_FRAMES - (h - t);
  if (nframes > space) {                              /* full → drop the newest */
    atomic_fetch_add_explicit(&m_drops, nframes - space, memory_order_relaxed);
    nframes = space;
  }

  float peak = m_peak;
  for (uint32_t i = 0; i < nframes; i++) {
    float s = src[i];
    m_ring[(h + i) & RING_MASK] = s;
    float a = s < 0.0f ? -s : s;
    if (a > peak) { peak = a; }
  }
  m_peak = peak;
  atomic_store_explicit(&m_head, h + nframes, memory_order_release);

  pw_stream_queue_buffer(m_stream, b);
}

static const struct pw_stream_events stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .process = on_process,
};

/* --- consumer: pull mono frames (TX feed thread) -------------------------- */
int mic_pull(float *out, int frames) {
  if (!m_stream || frames <= 0) { return 0; }
  unsigned h = atomic_load_explicit(&m_head, memory_order_acquire);
  unsigned t = atomic_load_explicit(&m_tail, memory_order_relaxed);
  unsigned avail = h - t;
  unsigned n = (unsigned)frames;
  if (n > avail) {
    atomic_fetch_add_explicit(&m_short, n - avail, memory_order_relaxed);
    n = avail;
  }
  for (unsigned i = 0; i < n; i++) { out[i] = m_ring[(t + i) & RING_MASK]; }
  atomic_store_explicit(&m_tail, t + n, memory_order_release);
  return (int)n;
}

/* Read-and-clear the ring health counters (single consumer: the unkey stats
 * line in tx_run). Nonzero drops/shorts during an over = the buzz/click
 * suspects: periodic shorts sound like mains hum. */
void mic_stats_take(int *drops, int *shorts) {
  if (drops)  { *drops  = (int)atomic_exchange_explicit(&m_drops, 0, memory_order_relaxed); }

  if (shorts) { *shorts = (int)atomic_exchange_explicit(&m_short, 0, memory_order_relaxed); }
}

float mic_peak(void) {
  float p = m_peak;
  m_peak = 0.0f;
  return p;
}

int mic_queued(void) {
  unsigned h = atomic_load_explicit(&m_head, memory_order_relaxed);
  unsigned t = atomic_load_explicit(&m_tail, memory_order_relaxed);
  return (int)(h - t);
}

void mic_flush(void) {
  /* Consumer owns the tail. Discard the idle backlog but KEEP the freshest
   * ~16 ms: an empty ring at key-on shorts every pull until the next PW
   * capture buffer lands (the "shorts=512" one-quantum hole in the over
   * stats, = a 10 ms silence chop at the start of every voice over). The
   * kept tail doubles as a head start that catches the first word's attack. */
  unsigned keep = 2048;  /* frames @48k = 43 ms ≈ 3 capture quanta (the stream
                            asks for the audio-latency setting, ~15 ms): jitter
                            headroom so mid-over pulls never race a late PW
                            buffer into silence-padding (= crackle on air) */
  unsigned h = atomic_load_explicit(&m_head, memory_order_acquire);
  unsigned t = atomic_load_explicit(&m_tail, memory_order_relaxed);
  if (h - t > keep) { atomic_store_explicit(&m_tail, h - keep, memory_order_release); }
}

/* --- device enumeration (one-shot registry roundtrip) --------------------- */
struct enum_state {
  mic_source          *out;
  int                  max, count;
  struct pw_main_loop *loop;
};

static void reg_global(void *data, uint32_t id, uint32_t perm, const char *type,
                       uint32_t ver, const struct spa_dict *props) {
  (void)id; (void)perm; (void)ver;
  struct enum_state *st = data;
  if (!props || !type || strcmp(type, PW_TYPE_INTERFACE_Node) != 0) { return; }
  const char *cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  if (!cls || strcmp(cls, "Audio/Source") != 0) { return; }     /* real capture only */
  const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  if (!name || st->count >= st->max) { return; }
  const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
  snprintf(st->out[st->count].name, sizeof st->out[0].name, "%s", name);
  snprintf(st->out[st->count].desc, sizeof st->out[0].desc, "%s", desc ? desc : name);
  st->count++;
}
static const struct pw_registry_events registry_events = {
  PW_VERSION_REGISTRY_EVENTS, .global = reg_global,
};

static void core_done(void *data, uint32_t id, int seq) {
  (void)seq;
  struct enum_state *st = data;
  if (id == PW_ID_CORE) { pw_main_loop_quit(st->loop); }   /* initial dump complete */
}
static const struct pw_core_events core_events = {
  PW_VERSION_CORE_EVENTS, .done = core_done,
};

int mic_list_sources(mic_source *out, int max) {
  if (!out || max <= 0) { return 0; }
  pw_init(NULL, NULL);
  struct pw_main_loop *loop = pw_main_loop_new(NULL);
  if (!loop) { return 0; }
  struct pw_context *ctx = pw_context_new(pw_main_loop_get_loop(loop), NULL, 0);
  struct pw_core *core = ctx ? pw_context_connect(ctx, NULL, 0) : NULL;
  if (!core) {
    if (ctx) { pw_context_destroy(ctx); }
    pw_main_loop_destroy(loop);
    return 0;
  }
  struct enum_state st = { .out = out, .max = max, .count = 0, .loop = loop };
  struct pw_registry *reg = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
  struct spa_hook reg_hook, core_hook;
  spa_zero(reg_hook); spa_zero(core_hook);
  pw_registry_add_listener(reg, &reg_hook, &registry_events, &st);
  pw_core_add_listener(core, &core_hook, &core_events, &st);
  pw_core_sync(core, PW_ID_CORE, 0);        /* fires core_done after the initial globals */
  pw_main_loop_run(loop);

  spa_hook_remove(&reg_hook);
  spa_hook_remove(&core_hook);
  pw_proxy_destroy((struct pw_proxy *)reg);
  pw_core_disconnect(core);
  pw_context_destroy(ctx);
  pw_main_loop_destroy(loop);
  return st.count;
}

/* --- lifecycle ------------------------------------------------------------ */
int mic_start(int rate, int latency_ms, const char *target) {
  atomic_store(&m_head, 0);
  atomic_store(&m_tail, 0);
  m_peak = 0.0f;

  pw_init(NULL, NULL);
  m_loop = pw_thread_loop_new("sdrfl-mic", NULL);
  if (!m_loop) { return -1; }

  int q = (latency_ms * rate) / 1000;
  if (q < 32) { q = 32; }
  char latprop[32];
  snprintf(latprop, sizeof(latprop), "%d/%d", q, rate);

  struct pw_properties *props = pw_properties_new(
      PW_KEY_MEDIA_TYPE,     "Audio",
      PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE,     "Communication",
      PW_KEY_NODE_LATENCY,   latprop,
      PW_KEY_NODE_NAME,      "sdr-for-linux mic",
      NULL);
  if (target && *target) {                 /* pin a specific source node */
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, target);
  }
  m_stream = pw_stream_new_simple(
      pw_thread_loop_get_loop(m_loop), "sdr-for-linux mic", props,
      &stream_events, NULL);
  if (!m_stream) { pw_thread_loop_destroy(m_loop); m_loop = NULL; return -2; }

  uint8_t bpod[1024];
  struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(bpod, sizeof(bpod));
  const struct spa_pod *params[1];
  params[0] = spa_format_audio_raw_build(&pb, SPA_PARAM_EnumFormat,
      &SPA_AUDIO_INFO_RAW_INIT(
          .format   = SPA_AUDIO_FORMAT_F32,
          .channels = 1,
          .rate     = rate));

  if (pw_stream_connect(m_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
          params, 1) < 0) {
    pw_stream_destroy(m_stream);
    pw_thread_loop_destroy(m_loop);
    m_stream = NULL;
    m_loop = NULL;
    return -3;
  }

  pw_thread_loop_start(m_loop);
  fprintf(stderr, "mic: PipeWire capture up (%d Hz, 1 ch, quantum %s)\n", rate, latprop);
  return 0;
}

void mic_stop(void) {
  if (m_loop) { pw_thread_loop_stop(m_loop); }
  if (m_stream) { pw_stream_destroy(m_stream); m_stream = NULL; }
  if (m_loop) { pw_thread_loop_destroy(m_loop); m_loop = NULL; }
  pw_deinit();
}
