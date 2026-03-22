/**
 * crossos/audio.h  -  Sound playback and PCM streaming helpers.
 *
 * Two APIs are provided:
 *
 *  1. Simple file playback – play an audio file asynchronously.
 *     Supported formats depend on the platform backend (WAV always works).
 *
 *  2. PCM streaming context – low-latency callback-based audio output.
 *     The callback runs on a high-priority audio thread; keep it brief and
 *     avoid blocking operations or memory allocation inside it.
 */

#ifndef CROSSOS_AUDIO_H
#define CROSSOS_AUDIO_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Simple file playback ─────────────────────────────────────────────── */

/**
 * Play an audio file asynchronously.
 *
 * Platform notes:
 * - Windows:  uses PlaySound (WAV) or MCI (other formats).
 * - Linux:    forks aplay / paplay if available.
 * - Android:  plays a notification tone via ToneGenerator.
 */
crossos_result_t crossos_sound_play_file(const char *path);

/** Stop currently playing sound (best-effort). */
void crossos_sound_stop(void);

/* ── PCM streaming ────────────────────────────────────────────────────── */

/**
 * Opaque PCM audio output context.
 * Internally manages a background thread and platform audio API.
 */
typedef struct crossos_audio_context crossos_audio_context_t;

/**
 * Called by the audio subsystem whenever it needs more samples.
 *
 * @param stereo     Interleaved float samples [L0,R0, L1,R1, ...],
 *                   pre-zeroed before the call.  Write normalised values
 *                   in the range [-1.0, 1.0].
 * @param frame_count Number of stereo frames to fill.
 * @param user_data   Opaque pointer supplied to crossos_audio_open().
 */
typedef void (*crossos_audio_callback_t)(float       *stereo,
                                         int          frame_count,
                                         void        *user_data);

/**
 * Open a PCM audio output stream.
 *
 * @param sample_rate   Desired sample rate in Hz (e.g. 44100 or 48000).
 * @param buffer_frames Period size in frames; smaller = lower latency.
 *                      Use 512 or 1024 as a safe default.
 * @param callback      Function called to fill each output buffer.
 * @param user_data     Passed verbatim to every callback invocation.
 * @param out_ctx       Receives the newly created context on success.
 *
 * @return CROSSOS_OK on success; CROSSOS_ERR_AUDIO on failure.
 *         CROSSOS_ERR_UNSUPPORT if the platform has no implementation yet.
 */
crossos_result_t crossos_audio_open(int                      sample_rate,
                                    int                      buffer_frames,
                                    crossos_audio_callback_t callback,
                                    void                    *user_data,
                                    crossos_audio_context_t **out_ctx);

/** Pause or resume the audio stream (0 = resume, 1 = pause). */
void crossos_audio_set_paused(crossos_audio_context_t *ctx, int paused);

/** Close the audio stream and release all resources. */
void crossos_audio_close(crossos_audio_context_t *ctx);

/**
 * Generate a simple sine-wave beep on the default output device.
 * Non-blocking; returns once the tone has been queued.
 *
 * @param freq_hz     Frequency in hertz (e.g. 440).
 * @param duration_ms Duration in milliseconds.
 */
void crossos_audio_beep(int freq_hz, int duration_ms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_AUDIO_H */
