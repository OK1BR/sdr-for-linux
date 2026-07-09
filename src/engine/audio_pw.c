/*
 * sdr-for-linux — native PipeWire audio sink (low latency). See audio.h.
 *
 * A pw_thread_loop drives a playback pw_stream; its RT `on_process` callback
 * pulls interleaved float frames from a lock-free SPSC ring that the DSP thread
 * fills via audio_push(). No blocking on the DSP thread, no lock in the RT
 * callback. PW_KEY_NODE_LATENCY requests a small quantum for minimum latency.
 * The ring is interleaved by channel (a_channels, 1 or 2) so the demodulator can
 * feed genuine stereo (binaural) without a second buffer.
 */
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"

/* Interleaved ring, power-of-two frames. 16384 @ 48k = 341 ms of headroom; we
 * keep it far emptier — it only absorbs scheduling jitter. Slots are MAX_CH
 * wide; a_channels (1 or 2) tells how many are live. */
#define RING_FRAMES 16384u
#define RING_MASK   (RING_FRAMES - 1u)
#define MAX_CH      2

static float          a_ring[RING_FRAMES * MAX_CH];   /* interleaved by channel */
static atomic_uint    a_head;   /* producer frame index (free-running)  */
static atomic_uint    a_tail;   /* consumer frame index (free-running)  */

static struct pw_thread_loop *a_loop;
static struct pw_stream       *a_stream;
static int                     a_channels = 1;

/* --- RT callback: fill one PW buffer from the ring ------------------------ */
static void on_process(void *userdata) {
  (void)userdata;
  struct pw_buffer *b = pw_stream_dequeue_buffer(a_stream);
  if (!b) { return; }
  struct spa_buffer *buf = b->buffer;
  float *dst = buf->datas[0].data;
  if (!dst) { pw_stream_queue_buffer(a_stream, b); return; }

  int stride = sizeof(float) * a_channels;
  uint32_t maxframes = buf->datas[0].maxsize / stride;
  uint32_t nframes = maxframes;
  if (b->requested) { nframes = SPA_MIN(b->requested, (uint64_t)maxframes); }

  unsigned h = atomic_load_explicit(&a_head, memory_order_acquire);
  unsigned t = atomic_load_explicit(&a_tail, memory_order_relaxed);
  unsigned avail = h - t;

  for (uint32_t i = 0; i < nframes; i++) {
    const float *src = (i < avail) ? &a_ring[((t + i) & RING_MASK) * MAX_CH] : NULL;
    for (int c = 0; c < a_channels; c++) { *dst++ = src ? src[c] : 0.0f; }  /* underrun → silence */
  }
  unsigned consumed = (avail < nframes) ? avail : nframes;
  atomic_store_explicit(&a_tail, t + consumed, memory_order_release);

  buf->datas[0].chunk->offset = 0;
  buf->datas[0].chunk->stride = stride;
  buf->datas[0].chunk->size   = nframes * stride;
  pw_stream_queue_buffer(a_stream, b);
}

static const struct pw_stream_events stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .process = on_process,
};

/* --- producer: push interleaved frames (DSP thread) ----------------------- */
void audio_push(const float *samples, int frames) {
  if (!a_stream || frames <= 0) { return; }
  unsigned h = atomic_load_explicit(&a_head, memory_order_relaxed);
  unsigned t = atomic_load_explicit(&a_tail, memory_order_acquire);
  unsigned space = RING_FRAMES - (h - t);
  unsigned n = (unsigned)frames;
  if (n > space) { n = space; }          /* full → drop the excess */
  for (unsigned i = 0; i < n; i++) {
    float *slot = &a_ring[((h + i) & RING_MASK) * MAX_CH];
    for (int c = 0; c < a_channels; c++) { slot[c] = samples[(size_t)i * a_channels + c]; }
  }
  atomic_store_explicit(&a_head, h + n, memory_order_release);
}

int audio_queued(void) {
  unsigned h = atomic_load_explicit(&a_head, memory_order_relaxed);
  unsigned t = atomic_load_explicit(&a_tail, memory_order_relaxed);
  return (int)(h - t);
}

/* --- device enumeration (one-shot registry roundtrip) --------------------- */
struct sink_enum {
  audio_sink          *out;
  int                  max, count;
  struct pw_main_loop *loop;
};
static void sink_global(void *data, uint32_t id, uint32_t perm, const char *type,
                        uint32_t ver, const struct spa_dict *props) {
  (void)id; (void)perm; (void)ver;
  struct sink_enum *st = data;
  if (!props || !type || strcmp(type, PW_TYPE_INTERFACE_Node) != 0) { return; }
  const char *cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  if (!cls || strcmp(cls, "Audio/Sink") != 0) { return; }        /* playback devices */
  const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  if (!name || st->count >= st->max) { return; }
  const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
  snprintf(st->out[st->count].name, sizeof st->out[0].name, "%s", name);
  snprintf(st->out[st->count].desc, sizeof st->out[0].desc, "%s", desc ? desc : name);
  st->count++;
}
static const struct pw_registry_events sink_registry_events = {
  PW_VERSION_REGISTRY_EVENTS, .global = sink_global,
};
static void sink_core_done(void *data, uint32_t id, int seq) {
  (void)seq;
  struct sink_enum *st = data;
  if (id == PW_ID_CORE) { pw_main_loop_quit(st->loop); }
}
static const struct pw_core_events sink_core_events = {
  PW_VERSION_CORE_EVENTS, .done = sink_core_done,
};
int audio_list_sinks(audio_sink *out, int max) {
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
  struct sink_enum st = { .out = out, .max = max, .count = 0, .loop = loop };
  struct pw_registry *reg = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
  struct spa_hook reg_hook, core_hook;
  spa_zero(reg_hook); spa_zero(core_hook);
  pw_registry_add_listener(reg, &reg_hook, &sink_registry_events, &st);
  pw_core_add_listener(core, &core_hook, &sink_core_events, &st);
  pw_core_sync(core, PW_ID_CORE, 0);
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
int audio_start(int rate, int channels, int latency_ms, const char *target) {
  a_channels = (channels < 1) ? 1 : (channels > MAX_CH ? MAX_CH : channels);
  channels = a_channels;
  atomic_store(&a_head, 0);
  atomic_store(&a_tail, 0);

  pw_init(NULL, NULL);
  a_loop = pw_thread_loop_new("sdrfl-audio", NULL);
  if (!a_loop) { return -1; }

  int q = (latency_ms * rate) / 1000;    /* quantum frames for the target */
  if (q < 32) { q = 32; }
  char latprop[32];
  snprintf(latprop, sizeof(latprop), "%d/%d", q, rate);

  struct pw_properties *props = pw_properties_new(
          PW_KEY_MEDIA_TYPE,     "Audio",
          PW_KEY_MEDIA_CATEGORY, "Playback",
          PW_KEY_MEDIA_ROLE,     "Communication",
          PW_KEY_NODE_LATENCY,   latprop,
          PW_KEY_NODE_NAME,      "sdr-for-linux",
          NULL);
  if (target && *target) { pw_properties_set(props, PW_KEY_TARGET_OBJECT, target); }
  a_stream = pw_stream_new_simple(
      pw_thread_loop_get_loop(a_loop), "sdr-for-linux", props,
      &stream_events, NULL);
  if (!a_stream) { pw_thread_loop_destroy(a_loop); a_loop = NULL; return -2; }

  uint8_t bpod[1024];
  struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(bpod, sizeof(bpod));
  const struct spa_pod *params[1];
  params[0] = spa_format_audio_raw_build(&pb, SPA_PARAM_EnumFormat,
      &SPA_AUDIO_INFO_RAW_INIT(
          .format   = SPA_AUDIO_FORMAT_F32,
          .channels = channels,
          .rate     = rate));

  if (pw_stream_connect(a_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
          params, 1) < 0) {
    pw_stream_destroy(a_stream);
    pw_thread_loop_destroy(a_loop);
    a_stream = NULL;
    a_loop = NULL;
    return -3;
  }

  pw_thread_loop_start(a_loop);
  fprintf(stderr, "audio: PipeWire sink up (%d Hz, %d ch, quantum %s)\n", rate, channels, latprop);
  return 0;
}

void audio_stop(void) {
  if (a_loop) { pw_thread_loop_stop(a_loop); }
  if (a_stream) { pw_stream_destroy(a_stream); a_stream = NULL; }
  if (a_loop) { pw_thread_loop_destroy(a_loop); a_loop = NULL; }
  pw_deinit();
}
