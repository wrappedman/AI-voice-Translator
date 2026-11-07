/* =====================================================================
 *  OpenSpeech AI — microphone capture
 *
 *  A tiny, dependency-free capture API: open, poll, close. The Windows build
 *  uses waveIn (winmm), which needs no extra runtime; other targets get a
 *  stub so the headless build and the unit tests still link.
 *
 *  Capture is *always* local. Samples live in one ring buffer in the process
 *  and are never written to disk unless the user enables "keep a copy of the
 *  audio" in the settings.
 * ===================================================================== */
#ifndef OSAI_MIC_H
#define OSAI_MIC_H

#include <stdbool.h>
#include <stdint.h>
#include "../core/osai_common.h"

typedef struct osa_mic osa_mic;

/* Opens the default input device at 16 kHz mono. Returns NULL when capture is
 * unavailable (no device, unsupported platform, no permission). */
osa_mic *osa_mic_open(int sample_rate, osa_error *err);

/* Reads up to `max` samples without blocking. Returns the number written (0 is
 * normal: it just means not enough audio has arrived yet). */
int osa_mic_read(osa_mic *m, float *out, int max);

/* Current input level in 0..1, for the live level meter. */
float osa_mic_level(osa_mic *m);

/* Total samples captured since open. */
int64_t osa_mic_total(const osa_mic *m);

void osa_mic_close(osa_mic *m);

/* True when this build can actually capture audio. The headless build returns
 * false so the UI can show "microphone unavailable" instead of a dead button. */
bool osa_mic_available(void);

#endif /* OSAI_MIC_H */
