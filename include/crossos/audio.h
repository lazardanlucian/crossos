/**
 * crossos/audio.h  -  Minimal sound playback helpers.
 *
 * This first version is intentionally simple: play a single audio file and
 * optionally stop current playback.
 */

#ifndef CROSSOS_AUDIO_H
#define CROSSOS_AUDIO_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Play an audio file asynchronously.
 *
 * Platform notes:
 * - Windows: uses PlaySound (WAV files are recommended).
 * - Linux: invokes "aplay" or "paplay" if available.
 * - Android: plays a short notification tone via ToneGenerator.
 */
crossos_result_t crossos_sound_play_file(const char *path);

/** Stop currently playing sound if supported by the platform backend. */
void crossos_sound_stop(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_AUDIO_H */
