/*
 * sdr-for-linux — native PipeWire microphone capture seam (F6c, docs/TX-DESIGN.md).
 *
 * The RX-side twin of audio.h: instead of a playback sink it opens a capture
 * pw_stream on the system's default audio source and drains it into a lock-free
 * SPSC ring. The TX feed thread (tx_run) pulls mono float @ 48 kHz from that ring
 * and hands it to tx_dsp_feed_mic() while MOX is keyed. Host-soundcard mic, the
 * same path as RX audio — Richard's F6c choice (not the radio's mic jack).
 *
 * ⛔ Capture ONLY produces samples; it never keys or sends anything. Nothing here
 * touches the radio. Whether those samples ever reach the exciter is decided by
 * tx_gate/tx_run (F6c-2/3): until MOX is wired + enabled, this is dormant.
 */
#ifndef SDRFL_ENGINE_MIC_PW_H
#define SDRFL_ENGINE_MIC_PW_H

/* One capture source for the device picker: stable node name + friendly label. */
typedef struct {
  char name[128];   /* PipeWire node.name — pass to mic_start() as `target` */
  char desc[160];   /* node.description — human label for the combo         */
} mic_source;

/*
 * Enumerate Audio/Source capture devices into out[0..max-1]; returns the count
 * written (<= max). One synchronous PipeWire registry roundtrip — call from the
 * GUI thread when building the settings picker, NOT from the RT/feed path.
 */
int  mic_list_sources(mic_source *out, int max);

/*
 * Open a capture source at `rate` Hz mono, targeting ~`latency_ms` of input
 * latency (smaller = lower latency, more CPU). `target` picks the source by its
 * PipeWire node name/serial (e.g. "alsa_input.pci-…analog-stereo"); NULL or ""
 * = the system default source. PipeWire resamples the device to `rate` for us.
 * Returns 0 on success, <0 on error. Safe when no mic exists — yields silence.
 */
int  mic_start(int rate, int latency_ms, const char *target);

/*
 * Pull up to `frames` mono float samples into out[]; returns the number actually
 * available (0..frames — the caller pads the remainder with silence). Non-blocking
 * (lock-free ring). Call from the TX feed thread. A no-op returning 0 if not started.
 */
int  mic_pull(float *out, int frames);

/* Peak |sample| seen since the last call, in [0,1] (level meter). Resets on read. */
float mic_peak(void);

/* Frames currently queued in the capture ring (latency diagnostics). */
int  mic_queued(void);

/*
 * Drop everything currently queued (consumer-side; call from the same thread as
 * mic_pull). Use at MOX key-down so the exciter starts on fresh mic audio rather
 * than the stale block that accumulated in the ring while idle.
 */
void mic_flush(void);

/* Stop and tear down the capture stream. Safe if not started. */
void mic_stop(void);

#endif /* SDRFL_ENGINE_MIC_PW_H */
