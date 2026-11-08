/* =====================================================================
 *  OpenSpeech AI — microphone capture (winmm / headless stub)
 * ===================================================================== */
#include "mic.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(_WIN32)

#include <windows.h>
#include <mmsystem.h>

/* Two buffers of 500 ms. Small buffers keep the latency down; large ones keep
 * the number of callbacks (and therefore lock acquisitions) down. 500 ms is
 * where a live transcript still feels live without the callback waking the
 * process twenty times a second. */
#define MIC_BUFS 2
#define MIC_FRAMES 8000            /* 500 ms at 16 kHz */

typedef struct mic_buf {
    WAVEHDR hdr;
    int16_t data[MIC_FRAMES];
} mic_buf;

/* The event-driven form of waveIn, deliberately.
 *
 * The callback form (CALLBACK_FUNCTION) cannot re-queue its buffer from inside
 * the callback — the winmm documentation says so, and doing it anyway is what
 * made stopping the microphone freeze the application: waveInReset() waits for
 * the driver's callbacks to finish, while our callback waits inside
 * waveInAddBuffer() for the device lock waveInReset() is holding. A deadlock
 * that only shows up on the stop press, i.e. exactly what the report described.
 *
 * With CALLBACK_EVENT the audio thread never runs our code: the reader polls
 * the event (osa_mic_read is already non-blocking for the UI) and re-queues
 * finished buffers from the caller's thread, where waveIn calls are legal.
 * Closing is then just stop, unprepare, close — no callback can be in flight. */
struct osa_mic {
    HWAVEIN         h;
    HANDLE          evt;
    int             sr;
    mic_buf         bufs[MIC_BUFS];
    int             n_bufs;
    CRITICAL_SECTION lock;
    /* decoded 16-bit frames ready for the reader; a ring so a slow frame does
     * not lose audio that already arrived */
    float           pending[MIC_FRAMES * 4];
    int             pend_start, pend_len;
    int64_t         total;
    float           level;
    volatile LONG   open;
};

/* Pulls every finished buffer into the ring and hands it straight back to the
 * driver. Called from the reader's thread only, so no locking is required
 * around the waveIn calls themselves; the ring is guarded because the level
 * meter and the reader may be the same thread anyway — cheap and harmless. */
static void mic_service(osa_mic *m) {
    for (int i = 0; i < m->n_bufs; i++) {
        WAVEHDR *hdr = &m->bufs[i].hdr;
        if (!(hdr->dwFlags & WHDR_DONE)) continue;
        int n = (int)(hdr->dwBytesRecorded / sizeof(int16_t));
        if (n > 0) {
            EnterCriticalSection(&m->lock);
            int room = (int)(sizeof m->pending / sizeof m->pending[0]) - m->pend_len;
            if (n > room) {
                /* the reader fell behind by more than two seconds: drop the
                 * oldest audio rather than growing without bound (a live
                 * transcript that lags is worse than one that skips) */
                int drop = n - room;
                if (drop >= m->pend_len) { m->pend_start = 0; m->pend_len = 0; }
                else {
                    m->pend_start = (m->pend_start + drop) %
                                    (int)(sizeof m->pending / sizeof m->pending[0]);
                    m->pend_len -= drop;
                }
                room = (int)(sizeof m->pending / sizeof m->pending[0]) - m->pend_len;
            }
            const int16_t *src = (const int16_t *)hdr->lpData;
            int cap = (int)(sizeof m->pending / sizeof m->pending[0]);
            float peak = m->level;
            for (int k = 0; k < n && k < room; k++) {
                float v = (float)src[k] * (1.0f / 32768.0f);
                m->pending[(m->pend_start + m->pend_len + k) % cap] = v;
                float a = fabsf(v);
                if (a > peak) peak = a;
            }
            int take = n < room ? n : room;
            m->pend_len += take;
            m->total += take;
            m->level = peak * 0.85f + m->level * 0.15f;
            LeaveCriticalSection(&m->lock);
        }
        hdr->dwFlags &= ~WHDR_DONE;
        hdr->dwBytesRecorded = 0;
        if (InterlockedCompareExchange(&m->open, 1, 1))
            waveInAddBuffer(m->h, hdr, sizeof(WAVEHDR));
    }
    ResetEvent(m->evt);
}

osa_mic *osa_mic_open(int sample_rate, osa_error *err) {
    if (sample_rate <= 0) sample_rate = 16000;
    osa_mic *m = (osa_mic *)calloc(1, sizeof *m);
    if (!m) { osa_err_set(err, "out of memory"); return NULL; }
    m->sr = sample_rate;
    InitializeCriticalSection(&m->lock);
    m->evt = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!m->evt) {
        DeleteCriticalSection(&m->lock);
        free(m);
        osa_err_set(err, "no microphone available (no event)");
        return NULL;
    }
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = (DWORD)sample_rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    MMRESULT r = waveInOpen(&m->h, WAVE_MAPPER, &wf, (DWORD_PTR)m->evt, 0,
                            CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR) {
        CloseHandle(m->evt);
        DeleteCriticalSection(&m->lock);
        free(m);
        osa_err_set(err, "no microphone available (waveIn error %u)", (unsigned)r);
        return NULL;
    }
    m->n_bufs = 0;
    for (int i = 0; i < MIC_BUFS; i++) {
        memset(&m->bufs[i], 0, sizeof(WAVEHDR));
        m->bufs[i].hdr.lpData = (LPSTR)m->bufs[i].data;
        m->bufs[i].hdr.dwBufferLength = (DWORD)sizeof m->bufs[i].data;
        if (waveInPrepareHeader(m->h, &m->bufs[i].hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) break;
        if (waveInAddBuffer(m->h, &m->bufs[i].hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) break;
        m->n_bufs++;
    }
    InterlockedExchange(&m->open, 1);
    if (m->n_bufs == 0 || waveInStart(m->h) != MMSYSERR_NOERROR) {
        InterlockedExchange(&m->open, 0);
        if (m->h) { waveInReset(m->h); waveInClose(m->h); }
        CloseHandle(m->evt);
        DeleteCriticalSection(&m->lock);
        free(m);
        osa_err_set(err, "microphone could not start");
        return NULL;
    }
    osa_err_ok(err);
    return m;
}

int osa_mic_read(osa_mic *m, float *out, int max) {
    if (!m || !out || max <= 0 || !InterlockedCompareExchange(&m->open, 1, 1)) return 0;
    mic_service(m);
    EnterCriticalSection(&m->lock);
    int cap = (int)(sizeof m->pending / sizeof m->pending[0]);
    int n = m->pend_len < max ? m->pend_len : max;
    for (int i = 0; i < n; i++)
        out[i] = m->pending[(m->pend_start + i) % cap];
    m->pend_start = (m->pend_start + n) % cap;
    m->pend_len -= n;
    float lv = m->level;
    LeaveCriticalSection(&m->lock);
    m->level = lv * 0.999f;
    return n;
}

float osa_mic_level(osa_mic *m) { return m ? m->level : 0.0f; }

int64_t osa_mic_total(const osa_mic *m) { return m ? m->total : 0; }

void osa_mic_close(osa_mic *m) {
    if (!m) return;
    InterlockedExchange(&m->open, 0);          /* the service loop stops re-queuing */
    if (m->h) {
        waveInStop(m->h);
        waveInReset(m->h);                     /* returns its buffers to us       */
        for (int i = 0; i < m->n_bufs; i++)
            waveInUnprepareHeader(m->h, &m->bufs[i].hdr, sizeof(WAVEHDR));
        waveInClose(m->h);
    }
    if (m->evt) CloseHandle(m->evt);
    DeleteCriticalSection(&m->lock);
    free(m);
}

bool osa_mic_available(void) { return true; }

#else /* ------------------------- headless stub ---------------------- */

struct osa_mic { int sr; int64_t total; float level; };

osa_mic *osa_mic_open(int sample_rate, osa_error *err) {
    (void)sample_rate;
    osa_err_set(err, "microphone capture is not available on this platform");
    return NULL;
}
int   osa_mic_read(osa_mic *m, float *out, int max) { (void)m; (void)out; (void)max; return 0; }
float osa_mic_level(osa_mic *m) { return m ? m->level : 0.0f; }
int64_t osa_mic_total(const osa_mic *m) { return m ? m->total : 0; }
void  osa_mic_close(osa_mic *m) { free(m); }
bool  osa_mic_available(void) { return false; }

#endif
