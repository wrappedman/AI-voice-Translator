/* audio_m4a.c — the AAC hook, with no decoder of its own.
 *
 * Compiled into the portable core on every target. The Windows build defines
 * OSAI_WIN_MEDIA_FOUNDATION for all of its objects and links the real
 * implementation from src/platform/win32/m4a_mf.c, so this file contributes
 * nothing there — deliberately, because a second (weak) definition next to the
 * real one is exactly the kind of thing a linker resolves by accident. On every
 * other target the answer is "not supported", and osa_audio_load() turns that
 * into a message which names the workaround instead of pretending the format
 * works. */
#include "audio_m4a.h"

#if !defined(OSAI_WIN_MEDIA_FOUNDATION)

bool osa_m4a_decode(const char *path, float **frames, int64_t *n, int *rate,
                    osa_error *err) {
    (void)path; (void)rate;
    if (frames) *frames = NULL;
    if (n) *n = 0;
    if (err) osa_err_set(err, "no AAC decoder on this platform");
    return false;
}

#endif /* !OSAI_WIN_MEDIA_FOUNDATION */
