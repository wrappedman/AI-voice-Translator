/* tts_sapi.c — speak through the voices Windows already has.
 *
 * The builtin formant synthesiser (tts.c) can pronounce the small builtin
 * lexicons, but anything else is spelled out letter by letter and sounds like a
 * growl. Windows ships real speech voices, so the "listen" button should use
 * them: SAPI5 is part of the OS, offline, and needs no download and no extra
 * DLL in the package.
 *
 * The voice is driven through *automation* (IDispatch), not through the native
 * ISpVoice vtable. That is a deliberate choice for safety: there are no SAPI
 * headers in this build, so a hand-declared native vtable could put a call in
 * the wrong slot and crash on someone else's machine, while IDispatch is a
 * documented seven-slot layout that any COM object implements the same way.
 * Speak() is reached by name, with the same flags the native API uses.
 *
 * Everything is balanced: every successful CoInitialize/CoCreateInstance is
 * undone when the process shuts the voice down, and any failure simply returns
 * false so the caller falls back to the builtin synthesiser. */
#include "tts_sapi.h"

#if defined(OSAI_WIN_SAPI)

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <stdlib.h>
#include <string.h>

/* SpeechVoiceSpeakFlags (sapi.h): async so the UI never waits, purge so a second
 * press replaces the sentence in progress, XML so we control escaping. */
#define SVSF_ASYNC             1
#define SVSF_PURGE_BEFORE      2
#define SVSF_IS_XML            8

static const GUID kClsidSpVoice =
    {0x96749377, 0x3391, 0x11d2, {0x9e, 0xe3, 0x00, 0xc0, 0x4f, 0x79, 0x73, 0x96}};

static IDispatch *g_voice;
static bool g_tried;
static bool g_ok;
static bool g_com_ready;
static char g_name[32] = "Windows SAPI";

/* The voice speaks asynchronously and SAPI delivers its notifications through a
 * window message. A message-only window plus a pump on its own thread keeps the
 * playback flowing without ever blocking the UI thread. */
static HWND g_hwnd;
static HANDLE g_pump;
static volatile LONG g_pump_stop;

static DWORD WINAPI pump_thread(LPVOID ud) {
    (void)ud;
    MSG msg;
    while (InterlockedCompareExchange(&g_pump_stop, 0, 0) == 0) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
    return 0;
}

/* Calls a method on the voice by name, with one argument. Late binding: the
 * object's type library is not needed and, more importantly, no vtable offset
 * is assumed. */
static bool sapi_call(const wchar_t *method, VARIANT *arg, int flags) {
    if (!g_voice) return false;
    LPOLESTR names[1] = {(LPOLESTR)method};
    DISPID id = 0;
    HRESULT hr = g_voice->lpVtbl->GetIDsOfNames(g_voice, &IID_NULL, names, 1,
                                                LOCALE_USER_DEFAULT, &id);
    if (FAILED(hr)) return false;
    DISPPARAMS dp;
    memset(&dp, 0, sizeof dp);
    if (arg) { dp.rgvarg = arg; dp.cArgs = 1; }
    if (flags >= 0 && arg) {
        /* The method's own flags argument ("Speak" takes a SpeechVoiceSpeakFlags
         * value) is a VARIANT of type VT_I4, passed as the *last* declared
         * parameter, which in IDispatch order comes first in rgvarg. */
        VARIANT args[2];
        memset(args, 0, sizeof args);
        args[0].vt = VT_I4;
        args[0].lVal = flags;
        args[1] = *arg;
        dp.rgvarg = args;
        dp.cArgs = 2;
        hr = g_voice->lpVtbl->Invoke(g_voice, id, &IID_NULL, LOCALE_USER_DEFAULT,
                                     DISPATCH_METHOD, &dp, NULL, NULL, NULL);
    } else {
        hr = g_voice->lpVtbl->Invoke(g_voice, id, &IID_NULL, LOCALE_USER_DEFAULT,
                                     DISPATCH_METHOD, &dp, NULL, NULL, NULL);
    }
    return SUCCEEDED(hr);
}

static bool sapi_init(void) {
    if (g_tried) return g_ok;
    g_tried = true;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (hr == RPC_E_CHANGED_MODE) hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    g_com_ready = SUCCEEDED(hr) || hr == S_FALSE;
    if (!g_com_ready) return false;
    hr = CoCreateInstance(&kClsidSpVoice, NULL, CLSCTX_INPROC_SERVER, &IID_IDispatch,
                          (void **)&g_voice);
    if (FAILED(hr) || !g_voice) { g_voice = NULL; return false; }

    /* Volume 100 and a natural rate. Both are optional: a voice that does not
     * expose them still speaks, so their failure is not fatal. */
    VARIANT v;
    memset(&v, 0, sizeof v);
    v.vt = VT_I4; v.lVal = 100;
    sapi_call(L"SetVolume", &v, -1);
    v.vt = VT_I4; v.lVal = 0;
    sapi_call(L"SetRate", &v, -1);

    g_hwnd = CreateWindowExW(0, L"STATIC", L"osai-tts", 0,
                             0, 0, 0, 0,
                             HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    if (g_hwnd) {
        memset(&v, 0, sizeof v);
        v.vt = VT_I4; v.lVal = WM_USER + 1;
        sapi_call(L"SetNotifyWindowMessage", &v, -1);
    }
    InterlockedExchange(&g_pump_stop, 0);
    g_pump = CreateThread(NULL, 0, pump_thread, NULL, 0, NULL);
    g_ok = true;
    return true;
}

bool osa_tts_platform_available(void) { return sapi_init(); }

const char *osa_tts_platform_name(void) { return osa_tts_platform_available() ? g_name : ""; }

/* XML escaping: with SVSF_IS_XML set, an "&" or "<" in the sentence would be
 * parsed as markup and the rest of the sentence would silently vanish. */
static wchar_t *xml_escape(const wchar_t *in, size_t cap) {
    wchar_t *out = (wchar_t *)malloc(cap * sizeof(wchar_t));
    if (!out) return NULL;
    size_t o = 0;
    for (const wchar_t *p = in; *p && o + 8 < cap; p++) {
        const wchar_t *rep = NULL;
        switch (*p) {
            case L'&': rep = L"&amp;"; break;
            case L'<': rep = L"&lt;"; break;
            case L'>': rep = L"&gt;"; break;
            case L'"': rep = L"&quot;"; break;
            default: out[o++] = *p; continue;
        }
        size_t n = wcslen(rep);
        memcpy(out + o, rep, n * sizeof(wchar_t));
        o += n;
    }
    out[o] = 0;
    return out;
}

bool osa_tts_platform_speak(const char *utf8_text, const char *lang) {
    (void)lang;   /* the installed voices decide what they can say; the sentence
                   * is always passed in its own script, never transliterated */
    if (!utf8_text || !*utf8_text) return false;
    if (!sapi_init()) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8_text, -1, NULL, 0);
    if (n <= 0) return false;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return false;
    MultiByteToWideChar(CP_UTF8, 0, utf8_text, -1, w, n);
    wchar_t *esc = xml_escape(w, (size_t)n * 6 + 64);
    free(w);
    if (!esc) return false;

    VARIANT text;
    memset(&text, 0, sizeof text);
    text.vt = VT_BSTR;
    text.bstrVal = SysAllocString(esc);
    free(esc);
    if (!text.bstrVal) return false;
    bool ok = sapi_call(L"Speak", &text, SVSF_ASYNC | SVSF_PURGE_BEFORE | SVSF_IS_XML);
    SysFreeString(text.bstrVal);
    return ok;
}

bool osa_tts_platform_save(const char *utf8_text, const char *lang, const char *wav_path) {
    (void)utf8_text; (void)lang; (void)wav_path;
    return false;   /* the button offers playback; rendering is not needed */
}

void osa_tts_platform_stop(void) {
    if (!g_voice) return;
    VARIANT none;
    memset(&none, 0, sizeof none);
    /* Speak(NULL) with purge: the documented way to stop mid-sentence. */
    none.vt = VT_BSTR;
    none.bstrVal = NULL;
    sapi_call(L"Speak", &none, SVSF_ASYNC | SVSF_PURGE_BEFORE);
}

#endif /* OSAI_WIN_SAPI */
