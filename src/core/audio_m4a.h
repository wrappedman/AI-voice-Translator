/* audio_m4a.h — an optional hook for a decoder the core cannot carry.
 *
 * The in-tree decoders (dr_wav / dr_mp3 / dr_flac / stb_vorbis) are plain C
 * files that build everywhere. AAC is in none of them, and an AAC encoder is
 * far too much source to vendor for one container, so the platform layer gets
 * the job: Windows asks Media Foundation, which every supported Windows version
 * already has. The core keeps a weak, empty default so the Linux/headless build
 * needs no special case — it simply reports that it cannot decode.
 *
 * Contract: on success `*frames` is mono float audio at `*rate`, owned by the
 * caller and released with free(); the platform must not resample. */
#ifndef OSAI_AUDIO_M4A_H
#define OSAI_AUDIO_M4A_H

#include "osai_common.h"

bool osa_m4a_decode(const char *path, float **frames, int64_t *n, int *rate,
                    osa_error *err);

#endif
