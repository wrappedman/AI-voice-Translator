/* m4a_mf.c — AAC (.m4a/.mp4/.aac) decoding through Media Foundation.
 *
 * Vendoring an AAC decoder for one container is several hundred kilobytes of
 * source and a patent question; Windows has had one built in since Vista. This
 * file uses the Source Reader, which is the small part of Media Foundation:
 * open the file, set the audio stream to 16 kHz mono PCM, read frames in a loop.
 * MFCreateSourceReaderFromURL pulls in the whole source-resolver stack, so the
 * reader is created from a plain IStream over the file instead, and the two
 * DLLs are loaded by name. That keeps the exe's import table free of Media
 * Foundation: on a stripped-down system the app still starts, and only .m4a
 * files are refused, with a message that says so.
 *
 * Only `MFStartup` and the float->PCM handoff need MF; everything else is plain
 * COM, which is already linked for the shell bits. */
#if defined(_WIN32)

#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include "audio_m4a.h"

/* Media Foundation's GUIDs are *not* in the import libraries zig ships beyond
 * the ones we link, and the two reader attributes we set are plain integers.
 *
 * MF_SOURCE_READER_ENABLE_AUDIO_PROCESSING pulls the decoded stream into the
 * format we asked for instead of handing back the file's native profile, which
 * is what makes a 44.1 kHz stereo AAC file arrive as 16 kHz mono float. */
#ifndef MF_SOURCE_READER_ENABLE_AUDIO_PROCESSING
static const GUID OSAI_MF_SR_AUDIO_PROCESSING =
    { 0x97fba2fa, 0x89c3, 0x4d1f, { 0x9b, 0x19, 0x77, 0x54, 0x34, 0xaf, 0xa5, 0x0a } };
#else
#define OSAI_MF_SR_AUDIO_PROCESSING MF_SOURCE_READER_ENABLE_AUDIO_PROCESSING
#endif

#define OSAI_MF_TARGET_RATE 16000

typedef struct mf_api {
    HMODULE plat, rw;
    HRESULT (STDAPICALLTYPE *startup)(ULONG, DWORD);
    HRESULT (STDAPICALLTYPE *shutdown)(void);
    HRESULT (STDAPICALLTYPE *create_reader_from_stream)(IUnknown *, IMFAttributes *,
                                                        REFIID, void **);
    HRESULT (STDAPICALLTYPE *create_attributes)(IMFAttributes **, UINT32);
    HRESULT (STDAPICALLTYPE *create_media_type)(IMFMediaType **);
} mf_api;

static mf_api g_mf;
static bool   g_mf_loaded;

/* One attempt per process: loading two DLLs on every file pick is pointless,
 * and a machine without Media Foundation should not pay for it twice. */
static mf_api *mf_load(void) {
    if (g_mf_loaded) return g_mf.plat ? &g_mf : NULL;
    g_mf_loaded = true;
    g_mf.plat = LoadLibraryW(L"mfplat.dll");
    g_mf.rw = LoadLibraryW(L"mfreadwrite.dll");
    if (!g_mf.plat || !g_mf.rw) return NULL;
    g_mf.startup = (HRESULT (STDAPICALLTYPE *)(ULONG, DWORD))
        GetProcAddress(g_mf.plat, "MFStartup");
    g_mf.shutdown = (HRESULT (STDAPICALLTYPE *)(void))
        GetProcAddress(g_mf.plat, "MFShutdown");
    g_mf.create_reader_from_stream =
        (HRESULT (STDAPICALLTYPE *)(IUnknown *, IMFAttributes *, REFIID, void **))
        GetProcAddress(g_mf.rw, "MFCreateSourceReaderFromByteStream");
    g_mf.create_attributes = (HRESULT (STDAPICALLTYPE *)(IMFAttributes **, UINT32))
        GetProcAddress(g_mf.plat, "MFCreateAttributes");
    g_mf.create_media_type = (HRESULT (STDAPICALLTYPE *)(IMFMediaType **))
        GetProcAddress(g_mf.plat, "MFCreateMediaType");
    if (!g_mf.startup || !g_mf.create_reader_from_stream || !g_mf.create_attributes ||
        !g_mf.create_media_type)
        return NULL;
    return &g_mf;
}

/* An IStream that reads a file through the C runtime, so no shell32 shim or
 * COM initialisation of a particular apartment is required. Only the methods
 * the source reader actually calls are implemented; the rest report
 * E_NOTIMPL, which is a legal (and common) thing for a read-only stream. */
typedef struct file_stream {
    IStream vtbl;
    HANDLE  file;
    int64_t size;
    LONG    refs;      /* real COM lifetime: the reader takes its own reference */
} file_stream;

static HRESULT STDMETHODCALLTYPE fs_QueryInterface(IStream *iface, REFIID riid, void **out) {
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IStream) ||
        IsEqualIID(riid, &IID_ISequentialStream)) {
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE fs_AddRef(IStream *iface) {
    file_stream *f = (file_stream *)iface;
    return (ULONG)InterlockedIncrement(&f->refs);
}
static ULONG STDMETHODCALLTYPE fs_Release(IStream *iface) {
    file_stream *f = (file_stream *)iface;
    LONG left = InterlockedDecrement(&f->refs);
    if (left > 0) return (ULONG)left;
    if (f->file && f->file != INVALID_HANDLE_VALUE) CloseHandle(f->file);
    f->file = NULL;
    free(f);
    return 0;
}
static HRESULT STDMETHODCALLTYPE fs_Read(IStream *iface, void *buf, ULONG n, ULONG *got) {
    file_stream *f = (file_stream *)iface;
    if (got) *got = 0;
    if (f->file == INVALID_HANDLE_VALUE || !f->file) return E_FAIL;
    DWORD want = n, read = 0;
    if (!ReadFile(f->file, buf, want, &read, NULL)) return E_FAIL;
    if (got) *got = read;
    return read == want ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE fs_Seek(IStream *iface, LARGE_INTEGER move, DWORD origin,
                                         ULARGE_INTEGER *pos) {
    file_stream *f = (file_stream *)iface;
    if (f->file == INVALID_HANDLE_VALUE || !f->file) return E_FAIL;
    LARGE_INTEGER np;
    np.QuadPart = move.QuadPart;
    if (!SetFilePointerEx(f->file, np, &np, origin)) return E_FAIL;
    if (pos) pos->QuadPart = (ULONGLONG)np.QuadPart;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fs_Stat(IStream *iface, STATSTG *st, DWORD flags) {
    (void)flags;
    file_stream *f = (file_stream *)iface;
    if (!st) return E_POINTER;
    memset(st, 0, sizeof *st);
    st->type = STGTY_STREAM;
    st->cbSize.QuadPart = (ULONGLONG)f->size;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fs_SetSize(IStream *i, ULARGE_INTEGER n) { (void)i; (void)n; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE fs_CopyTo(IStream *i, IStream *o, ULARGE_INTEGER n,
                                           ULARGE_INTEGER *r, ULARGE_INTEGER *w) {
    (void)i; (void)o; (void)n; (void)r; (void)w; return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE fs_Commit(IStream *i, DWORD f) { (void)i; (void)f; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE fs_Revert(IStream *i) { (void)i; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE fs_LockRegion(IStream *i, ULARGE_INTEGER a, ULARGE_INTEGER b, DWORD l) {
    (void)i; (void)a; (void)b; (void)l; return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE fs_UnlockRegion(IStream *i, ULARGE_INTEGER a, ULARGE_INTEGER b, DWORD l) {
    (void)i; (void)a; (void)b; (void)l; return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE fs_Clone(IStream *i, IStream **out) { (void)i; if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE fs_Write(IStream *i, const void *b, ULONG n, ULONG *w) {
    (void)i; (void)b; (void)n; if (w) *w = 0; return E_NOTIMPL;
}

static IStreamVtbl FILE_STREAM_VTBL = {
    fs_QueryInterface, fs_AddRef, fs_Release,
    fs_Read, fs_Write, fs_Seek, fs_SetSize, fs_CopyTo, fs_Commit, fs_Revert,
    fs_LockRegion, fs_UnlockRegion, fs_Stat, fs_Clone
};

static file_stream *stream_open(const char *path) {
    wchar_t wpath[OSAI_MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, OSAI_MAX_PATH)) return NULL;
    HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return NULL; }
    file_stream *f = (file_stream *)calloc(1, sizeof *f);
    if (!f) { CloseHandle(h); return NULL; }
    f->vtbl.lpVtbl = &FILE_STREAM_VTBL;
    f->refs = 1;
    f->file = h;
    f->size = sz.QuadPart;
    return f;
}

/* Read the whole decoded stream as mono float at OSAI_MF_TARGET_RATE. */
bool osa_m4a_decode(const char *path, float **frames, int64_t *n, int *rate,
                    osa_error *err) {
    if (frames) *frames = NULL;
    if (n) *n = 0;
    if (rate) *rate = 0;
    if (!path || !*path) { osa_err_set(err, "no file"); return false; }

    mf_api *mf = mf_load();
    if (!mf) {
        osa_err_set(err, "Media Foundation is not available on this system, so .m4a "
                         "cannot be decoded here; convert the file to WAV or MP3");
        return false;
    }
    /* The window thread owns the apartment; a failed COM init means somebody
     * else already set it up, which is fine for a stream. */
    bool com = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));
    bool mf_ok = SUCCEEDED(mf->startup(MF_VERSION, MFSTARTUP_LITE));
    bool ok = false;
    IMFSourceReader *reader = NULL;
    file_stream *stream = stream_open(path);
    IMFMediaType *want = NULL;
    float  *buf = NULL;
    size_t  cap = 0, used = 0;

    if (!stream) { osa_err_set(err, "cannot open the file"); goto done; }
    if (!mf_ok) { osa_err_set(err, "Media Foundation could not start"); goto done; }

    /* MF_SOURCE_READER_ENABLE_AUDIO_PROCESSING at creation time: without it the
     * reader hands back the file's native profile and refuses the 16 kHz mono
     * type we ask for below. AAC is the usual case; a container holding plain
     * PCM converts just as well, so no format list is needed. */
    {
        IMFAttributes *attrs = NULL;
        if (SUCCEEDED(mf->create_attributes(&attrs, 1)) && attrs) {
            attrs->lpVtbl->SetUINT32(attrs, &OSAI_MF_SR_AUDIO_PROCESSING, TRUE);
        }
        IUnknown *unk = (IUnknown *)&stream->vtbl;
        HRESULT hr = mf->create_reader_from_stream(unk, attrs, &IID_IMFSourceReader,
                                                   (void **)&reader);
        if (attrs) attrs->lpVtbl->Release(attrs);
        if (FAILED(hr) || !reader) {
            osa_err_set(err, "the file is not an audio container Media Foundation can open");
            goto done;
        }
    }
    {
        HRESULT hr = mf->create_media_type(&want);
        if (FAILED(hr) || !want) { osa_err_set(err, "out of memory"); goto done; }
        want->lpVtbl->SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
        want->lpVtbl->SetGUID(want, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
        want->lpVtbl->SetUINT32(want, &MF_MT_AUDIO_NUM_CHANNELS, 1);
        want->lpVtbl->SetUINT32(want, &MF_MT_AUDIO_SAMPLES_PER_SECOND, OSAI_MF_TARGET_RATE);
        want->lpVtbl->SetUINT32(want, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        hr = reader->lpVtbl->SetCurrentMediaType(reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                                 NULL, want);
        if (FAILED(hr)) {
            osa_err_set(err, "the file has no audio track that can be decoded");
            goto done;
        }
    }

    for (;;) {
        DWORD flags = 0;
        IMFSample *sample = NULL;
        HRESULT hr = reader->lpVtbl->ReadSample(reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                                0, NULL, &flags, NULL, &sample);
        if (FAILED(hr)) { if (sample) sample->lpVtbl->Release(sample); break; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { if (sample) sample->lpVtbl->Release(sample); break; }
        if (!sample) continue;
        IMFMediaBuffer *mb = NULL;
        if (SUCCEEDED(sample->lpVtbl->ConvertToContiguousBuffer(sample, &mb)) && mb) {
            BYTE *data = NULL;
            DWORD len = 0;
            if (SUCCEEDED(mb->lpVtbl->Lock(mb, &data, NULL, &len)) && data && len) {
                size_t add = (size_t)len / sizeof(int16_t);
                if (used + add > cap) {
                    size_t ncap = cap ? cap * 2 : 65536;
                    while (ncap < used + add) ncap *= 2;
                    float *nb = (float *)realloc(buf, ncap * sizeof(float));
                    if (!nb) { mb->lpVtbl->Unlock(mb); mb->lpVtbl->Release(mb);
                               sample->lpVtbl->Release(sample); goto done; }
                    buf = nb;
                    cap = ncap;
                }
                const int16_t *src = (const int16_t *)data;
                for (size_t i = 0; i < add; i++) buf[used + i] = (float)src[i] / 32768.0f;
                used += add;
                mb->lpVtbl->Unlock(mb);
            }
            mb->lpVtbl->Release(mb);
        }
        sample->lpVtbl->Release(sample);
    }

    if (!used) {
        osa_err_set(err, "the AAC track is empty");
        goto done;
    }
    if (frames) *frames = buf;
    if (n) *n = (int64_t)used;
    if (rate) *rate = OSAI_MF_TARGET_RATE;
    buf = NULL;
    ok = true;

done:
    free(buf);
    if (want) want->lpVtbl->Release(want);
    if (reader) reader->lpVtbl->Release(reader);
    if (stream) fs_Release((IStream *)&stream->vtbl);
    if (mf_ok && mf->shutdown) mf->shutdown();
    if (com) CoUninitialize();
    return ok;
}

#endif /* _WIN32 */

/* The definition above is the only one in the Windows build: src/core/audio_m4a.c
 * is compiled out by OSAI_WIN_MEDIA_FOUNDATION, which the Makefile passes to
 * every Windows object. */
