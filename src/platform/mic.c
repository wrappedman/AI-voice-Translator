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

#define MIC_BUFS 8
#define MIC_FRAMES 1600            /* 100 ms at 16 kHz */

typedef struct mic_buf {
    WAVEHDR hdr;
    int16_t data[MIC_FRAMES];
} mic_buf;

struct osa_mic {
    HWAVEIN         h;
    int             sr;
    mic_buf         bufs[MIC_BUFS];
    int             n_bufs;
    CRITICAL_SECTION lock;
    /* one decoded 16-bit frame ready for the reader */
    float           pending[MIC_FRAMES * 2];
    int             pending_n;
    int             pending_at;
    int64_t         total;
    float           level;
    bool            open;
};

static void CALLBACK mic_cb(HWAVEIN hwi, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2) {
    (void)hwi; (void)p2;
    if (msg != WIM_DATA) return;
    osa_mic *m = (osa_mic *)inst;
    if (!m) return;
    WAVEHDR *hdr = (WAVEHDR *)p1;
    EnterCriticalSection(&m->lock);
    int n = (int)(hdr->dwBytesRecorded / sizeof(int16_t));
    if (n > 0) {
        /* decode into the pending buffer; if the reader fell behind, drop the
         * oldest audio rather than growing without bound (a live transcription
         * that lags is worse than one that skips) */
        if (m->pending_at + n > (int)(sizeof m->pending / sizeof m->pending[0])) {
            memmove(m->pending, m->pending + m->pending_at,
                    (size_t)(m->pending_n - m->pending_at) * sizeof(float));
            m->pending_n -= m->pending_at;
            m->pending_at = 0;
        }
        if (m->pending_at + n > (int)(sizeof m->pending / sizeof m->pending[0]))
            n = (int)(sizeof m->pending / sizeof m->pending[0]) - m->pending_at;
        const int16_t *src = (const int16_t *)hdr->lpData;
        float peak = m->level;
        for (int i = 0; i < n; i++) {
            float v = (float)src[i] * (1.0f / 32768.0f);
            m->pending[m->pending_at + i] = v;
            float a = fabsf(v);
            if (a > peak) peak = a;
        }
        m->pending_at += n;
        if (m->pending_at > m->pending_n) m->pending_n = m->pending_at;
        m->level = peak * 0.85f + m->level * 0.15f;
        m->total += n;
    }
    LeaveCriticalSection(&m->lock);
    waveInAddBuffer(hwi, hdr, sizeof(WAVEHDR));
}

osa_mic *osa_mic_open(int sample_rate, osa_error *err) {
    if (sample_rate <= 0) sample_rate = 16000;
    osa_mic *m = (osa_mic *)calloc(1, sizeof *m);
    if (!m) { osa_err_set(err, "out of memory"); return NULL; }
    m->sr = sample_rate;
    InitializeCriticalSection(&m->lock);
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = (DWORD)sample_rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    MMRESULT r = waveInOpen(&m->h, WAVE_MAPPER, &wf, (DWORD_PTR)mic_cb, (DWORD_PTR)m,
                            CALLBACK_FUNCTION);
    if (r != MMSYSERR_NOERROR) {
        DeleteCriticalSection(&m->lock);
        free(m);
        osa_err_set(err, "no microphone available (waveIn error %u)", (unsigned)r);
        return NULL;
    }
    for (int i = 0; i < MIC_BUFS; i++) {
        memset(&m->bufs[i], 0, sizeof(WAVEHDR));
        m->bufs[i].hdr.lpData = (LPSTR)m->bufs[i].data;
        m->bufs[i].hdr.dwBufferLength = (DWORD)sizeof m->bufs[i].data;
        if (waveInPrepareHeader(m->h, &m->bufs[i].hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) break;
        if (waveInAddBuffer(m->h, &m->bufs[i].hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) break;
        m->n_bufs++;
    }
    if (m->n_bufs == 0 || waveInStart(m->h) != MMSYSERR_NOERROR) {
        if (m->h) waveInClose(m->h);
        DeleteCriticalSection(&m->lock);
        free(m);
        osa_err_set(err, "microphone could not start");
        return NULL;
    }
    m->open = true;
    osa_err_ok(err);
    return m;
}

int osa_mic_read(osa_mic *m, float *out, int max) {
    if (!m || !out || max <= 0 || !m->open) return 0;
    EnterCriticalSection(&m->lock);
    int avail = m->pending_n - m->pending_at;
    int n = avail < max ? avail : max;
    if (n > 0) {
        memcpy(out, m->pending + m->pending_at, (size_t)n * sizeof(float));
        m->pending_at += n;
        if (m->pending_at >= m->pending_n) { m->pending_at = 0; m->pending_n = 0; }
    }
    float lv = m->level;
    LeaveCriticalSection(&m->lock);
    m->level = lv * 0.999f;
    return n;
}

float osa_mic_level(osa_mic *m) { return m ? m->level : 0.0f; }

int64_t osa_mic_total(const osa_mic *m) { return m ? m->total : 0; }

void osa_mic_close(osa_mic *m) {
    if (!m) return;
    if (m->open && m->h) {
        waveInStop(m->h);
        waveInReset(m->h);
        for (int i = 0; i < m->n_bufs; i++) {
            waveInUnprepareHeader(m->h, &m->bufs[i].hdr, sizeof(WAVEHDR));
        }
        waveInClose(m->h);
    }
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
