/*
 * sdr-for-linux — audio sink seam (headless).
 *
 * A thin, backend-agnostic interface between the demodulator and the sound
 * system. The current implementation is native PipeWire (audio_pw.c) tuned for
 * low latency; the seam lets us swap backends without touching demod code.
 *
 * The DSP thread calls audio_push() (non-blocking); a backend thread drains to
 * the device. Interleaved float @ the given rate (1 or 2 channels — stereo
 * carries the WDSP binaural output).
 */
#ifndef SDRFL_ENGINE_AUDIO_H
#define SDRFL_ENGINE_AUDIO_H

/* One playback sink for the RX output-device picker (node.name + friendly label). */
typedef struct {
  char name[128];   /* PipeWire node.name — pass to audio_start() as `target` */
  char desc[160];   /* node.description — human label for the combo           */
} audio_sink;

/*
 * Enumerate Audio/Sink playback devices into out[0..max-1]; returns the count
 * written. One synchronous PipeWire registry roundtrip — call from the GUI thread.
 */
int audio_list_sinks(audio_sink *out, int max);

/*
 * Start the sink: `rate` Hz, `channels` (1 = mono), targeting ~`latency_ms` of
 * output latency. `target` pins the output device by PipeWire node.name; NULL/""
 * = system default. Returns 0 on success.
 */
int audio_start(int rate, int channels, int latency_ms, const char *target);

/*
 * Push `frames` interleaved float frames to the sink (channels-per-frame as
 * given to audio_start; e.g. stereo = 2*frames floats, L/R interleaved).
 * Non-blocking (lock-free ring); drops the excess on overflow. Call from the
 * DSP/feed thread — never blocks it.
 */
void audio_push(const float *samples, int frames);

/* Frames currently queued in the ring (latency diagnostics). */
int audio_queued(void);

/* Stop and tear down the sink. */
void audio_stop(void);

#endif /* SDRFL_ENGINE_AUDIO_H */
