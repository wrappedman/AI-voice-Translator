/* tts_sapi_stub.c — the "no platform voices here" answer, for every target that
 * is not Windows. Compiled only when OSAI_WIN_SAPI is not defined; the Windows
 * build gets the real implementation from tts_sapi.c, exactly like audio_m4a. */
#include "tts_sapi.h"

#if !defined(OSAI_WIN_SAPI)

bool osa_tts_platform_available(void) { return false; }
const char *osa_tts_platform_name(void) { return ""; }
bool osa_tts_platform_speak(const char *t, const char *l) { (void)t; (void)l; return false; }
bool osa_tts_platform_save(const char *t, const char *l, const char *p) {
    (void)t; (void)l; (void)p; return false;
}
void osa_tts_platform_stop(void) {}

#endif
