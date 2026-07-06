/*
 * sdr-for-linux — audio sink seam (headless).
 *
 * A thin, backend-agnostic interface between the demodulator and the sound
 * system. The current implementation is native PipeWire (audio_pw.c) tuned for
 * low latency; the seam lets us swap backends without touching demod code.
 *
 * The DSP thread calls audio_push() (non-blocking); a backend thread drains to
 * the device. Mono float @ the given rate.
 */
#ifndef SDRFL_ENGINE_AUDIO_H
#define SDRFL_ENGINE_AUDIO_H

/*
 * Start the sink: `rate` Hz, `channels` (1 = mono), targeting ~`latency_ms` of
 * output latency (smaller = lower latency, more CPU). Returns 0 on success.
 */
int audio_start(int rate, int channels, int latency_ms);

/*
 * Push `frames` mono float samples to the sink. Non-blocking (lock-free ring);
 * drops the excess on overflow. Call from the DSP/feed thread — never blocks it.
 */
void audio_push(const float *samples, int frames);

/* Frames currently queued in the ring (latency diagnostics). */
int audio_queued(void);

/* Stop and tear down the sink. */
void audio_stop(void);

#endif /* SDRFL_ENGINE_AUDIO_H */
