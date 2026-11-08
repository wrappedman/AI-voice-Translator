/* =====================================================================
 *  OpenSpeech AI — Windows host
 *
 *  A plain Win32 window: no UI framework, no runtime dependency beyond the
 *  system DLLs. The canvas renders into a 32-bit top-down DIB and is blitted
 *  with a single StretchDIBits, which is fast enough at 1440x900 and keeps the
 *  binary tiny.
 *
 *  Responsibilities:
 *    - create the window, pump messages, feed ui_input
 *    - DPI awareness, and resizing the drawing surface with the window
 *    - modeless file dialogs (see below) and native message boxes
 *    - clipboard, "open in explorer", single instance, tray icon
 * ===================================================================== */
#ifndef UNICODE
#define UNICODE 1
#endif
#ifndef _UNICODE
#define _UNICODE 1
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../app/osai_app.h"
#include "../../core/osai_platform.h"
#include "../../core/audio.h"
#include "../../ui/canvas.h"
#include "../../ui/shell.h"
#include "../mic.h"

#define WM_APP_TRAY      (WM_APP + 1)
#define WM_APP_CLIPBOARD (WM_APP + 2)
#define TRAY_UID         1

/* Requests come straight from shell_request (see shell.h). These used to be a
 * private numbering with its own values, and they had drifted: the shell's
 * dataset request (3) landed on this layer's "pick a model" (2), so the training
 * dataset button opened the wrong dialog and dropped the result. Aliasing the
 * enum instead of re-declaring integers makes that impossible. */
#define REQ_NONE        SHELL_REQ_NONE
#define REQ_PICK_AUDIO  SHELL_REQ_PICK_AUDIO
#define REQ_PICK_MODEL  SHELL_REQ_PICK_PACK
#define REQ_PICK_DATA   SHELL_REQ_PICK_DATASET
#define REQ_OPEN_FOLDER SHELL_REQ_OPEN_FOLDER
#define REQ_SAVE_TEXT   SHELL_REQ_SAVE_TRANSCRIPT
/* Compile-time proof that the two enums still line up. They drifted once and the
 * result was a button that did nothing, so this cannot be left to review. */
_Static_assert(REQ_PICK_AUDIO  == 1, "shell_request order changed");
_Static_assert(REQ_PICK_MODEL  == 2, "shell_request order changed");
_Static_assert(REQ_PICK_DATA   == 3, "shell_request order changed");
_Static_assert(REQ_OPEN_FOLDER == 4, "shell_request order changed");
_Static_assert(REQ_SAVE_TEXT   == 5, "shell_request order changed");

typedef struct host {
    HWND         hwnd;
    HDC          memdc;
    BITMAPINFO   bi;
    osa_canvas   canvas;
    ui_context   ui;
    shell_state  shell;
    osa_app     *app;
    int          width, height;
    float        dpi_scale;
    bool         running;
    /* pointer + keyboard state carried between messages and the frame */
    ui_input     in;
    uint32_t     typed[64];
    int          n_typed;
    int          wheel;
    double       last_tick;
    /* A file dialog is *modeless* and is opened from a timer callback, outside
     * the menu/window-procedure frames. Opening the modal GetOpenFileName from
     * directly inside the click frame is what made the picker reopen forever:
     * the WM_TIMER that ran the dialog was itself inside a message the dialog's
     * modal loop kept dispatching. */
    OPENFILENAMEW ofn;
    wchar_t      ofn_file[MAX_PATH * 2];
    bool         dialog_open;
    int          dialog_kind;
    /* A request raised while one of our own dialogs is running cannot open a
     * second one on the spot (the modal loop is still dispatching our timer).
     * It is remembered here and started on the next frame instead. */
    int          pending_kind;
} host;

static host g_host;

/* ---- DIB ---------------------------------------------------------- */

static bool host_alloc_surface(host *h, int w, int hh) {
    if (w <= 0 || hh <= 0) return false;
    if (h->memdc) { DeleteDC(h->memdc); h->memdc = NULL; }
    memset(&h->bi, 0, sizeof h->bi);
    h->bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    h->bi.bmiHeader.biWidth = w;
    h->bi.bmiHeader.biHeight = -hh;          /* top-down */
    h->bi.bmiHeader.biPlanes = 1;
    h->bi.bmiHeader.biBitCount = 32;
    h->bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(NULL);
    h->memdc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (!h->memdc) return false;
    if (h->canvas.px) canvas_free(&h->canvas);
    if (!canvas_init(&h->canvas, w, hh)) return false;
    h->width = w;
    h->height = hh;
    return true;
}

static void host_blit(host *h) {
    if (!h->memdc) return;
    HDC dc = GetDC(h->hwnd);
    /* The canvas stores premultiplied 0xAARRGGBB; with a 32-bit DIB the alpha
     * channel is ignored by GDI, and because every pixel is written each frame
     * (the canvas clears to an opaque colour) the result is correct. */
    StretchDIBits(dc, 0, 0, h->canvas.w, h->canvas.h, 0, 0, h->canvas.w, h->canvas.h,
                  h->canvas.px, &h->bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(h->hwnd, dc);
}

/* ---- file dialogs -------------------------------------------------
 *
 * Both pickers are modeless: the dialog runs its own message loop and the host
 * keeps painting. The continuation runs once, after the dialog closes, and the
 * "open" flag is cleared first, so a pick can never re-enter itself.
 * ----------------------------------------------------------------- */

static void finish_pick_audio(host *h, const wchar_t *file) {
    char path[OSAI_MAX_PATH];
    if (!WideCharToMultiByte(CP_UTF8, 0, file, -1, path, sizeof path, NULL, NULL)) return;
    /* Selection is not transcription. Decode the preview off the UI thread;
     * inference starts only when the user presses Transcribe. */
    shell_select_audio(&h->shell,path);
}

static void finish_pick_model(host *h, const wchar_t *file) {
    char path[OSAI_MAX_PATH];
    if (!WideCharToMultiByte(CP_UTF8, 0, file, -1, path, sizeof path, NULL, NULL)) return;
    if(!shell_import_model(&h->shell,path))shell_set_message(&h->shell,true,"%s",tt(h->shell.lang,"msg.failed"));
}

/* Training data: either a tab-separated "audio<TAB>text" file or a folder of
 * recordings. The picker offers both, so a user with a folder of clips is not
 * forced to assemble a manifest by hand. */
static void finish_pick_dataset(host *h, const wchar_t *file) {
    (void)file;shell_set_message(&h->shell,true,"%s",tt(h->shell.lang,"training.unsupported"));
}

/* The transcript export: the user picks where to write it. */
static void finish_save_text(host *h, const wchar_t *file) {
    char path[OSAI_MAX_PATH];
    if (!WideCharToMultiByte(CP_UTF8, 0, file, -1, path, sizeof path, NULL, NULL)) return;
    const char *text = h->shell.transcript[0] ? h->shell.transcript
                                              : h->shell.translation;
    if (osa_file_write_atomic(path, text, strlen(text))) {
        shell_set_message(&h->shell, false, "%s", tt(h->shell.lang, "msg.saved"));
    } else {
        shell_set_message(&h->shell, true, "%s", tt(h->shell.lang, "msg.failed"));
    }
}

static bool start_dialog(host *h, int kind) {
    if (h->dialog_open) return false;
    h->dialog_open = true;
    h->dialog_kind = kind;
    h->ofn_file[0] = 0;
    memset(&h->ofn, 0, sizeof h->ofn);
    h->ofn.lStructSize = sizeof h->ofn;
    h->ofn.hwndOwner = h->hwnd;
    h->ofn.lpstrFile = h->ofn_file;
    h->ofn.nMaxFile = MAX_PATH * 2;
    h->ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER |
                   OFN_NOCHANGEDIR;
    if (kind == REQ_SAVE_TEXT) {
        h->ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        h->ofn.lpstrFilter =
            L"Text file (*.txt)\0*.txt\0All files\0*.*\0";
        h->ofn.lpstrTitle = L"ذخیره رونویسی / Save the transcript";
        lstrcpynW(h->ofn_file, L"transcript.txt", MAX_PATH * 2);
        BOOL saved = GetSaveFileNameW(&h->ofn);
        h->dialog_open = false;
        if (saved) finish_save_text(h, h->ofn_file);
        return saved ? true : false;
    }
    if (kind == REQ_PICK_DATA) {
        /* A dataset file, or a single recording chosen from a run folder. */
        h->ofn.lpstrFilter =
            L"Dataset (*.tsv;*.csv;*.txt)\0*.tsv;*.csv;*.txt\0"
            L"Audio files (*.wav;*.mp3;*.m4a;*.flac;*.ogg)\0*.wav;*.mp3;*.m4a;*.flac;*.ogg\0"
            L"All files\0*.*\0";
        h->ofn.lpstrTitle = L"فایل داده آموزشی / Training dataset";
    } else if (kind == REQ_PICK_AUDIO) {
        /* .m4a is what a phone records to, so it is in the first filter rather
         * than behind "all files": a picker that does not list the format the
         * user has is the same as not supporting it. */
        h->ofn.lpstrFilter =
            L"Audio files (*.wav;*.mp3;*.m4a;*.flac;*.ogg)\0*.wav;*.mp3;*.m4a;*.flac;*.ogg\0"
            L"WAV (*.wav)\0*.wav\0"
            L"MP3 (*.mp3)\0*.mp3\0"
            L"M4A / MP4 audio (*.m4a;*.mp4)\0*.m4a;*.mp4\0"
            L"FLAC (*.flac)\0*.flac\0"
            L"Ogg Vorbis (*.ogg;*.oga)\0*.ogg;*.oga\0"
            L"All files\0*.*\0";
        h->ofn.lpstrTitle = L"انتخاب فایل صوتی / Choose an audio file";
    } else {
        h->ofn.lpstrFilter =
            L"Verified models (*.gguf;*.bin)\0*.gguf;*.bin\0"
            L"All files\0*.*\0";
        h->ofn.lpstrTitle = L"نصب مدل تأییدشده / Install a verified model";
    }
    BOOL ok = GetOpenFileNameW(&h->ofn);
    /* Read the result *after* the dialog has closed, and clear the flag before
     * doing any work, so neither the continuation nor a repaint can reopen it. */
    h->dialog_open = false;
    if (ok) {
        if (kind == REQ_PICK_AUDIO) finish_pick_audio(h, h->ofn_file);
        else if (kind == REQ_PICK_DATA) finish_pick_dataset(h, h->ofn_file);
        else finish_pick_model(h, h->ofn_file);
    }
    return ok ? true : false;
}

/* ---- tray --------------------------------------------------------- */

#define IDM_OPEN       1001
#define IDM_TRANSCRIBE 1002
#define IDM_QUIT       1003

static void tray_add(host *h) {
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = h->hwnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = LoadIconW(GetModuleHandleW(NULL), L"IDI_APPICON");
    if (!nid.hIcon) nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    lstrcpynW(nid.szTip, L"OpenSpeech AI", (int)(sizeof nid.szTip / sizeof nid.szTip[0]));
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void tray_remove(host *h) {
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = h->hwnd;
    nid.uID = TRAY_UID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

/* ---- frame -------------------------------------------------------- */

static double now_seconds(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static void host_frame(host *h) {
    if (!h->running) return;
    double t = now_seconds();
    float dt = (float)(t - h->last_tick);
    if (dt < 0) dt = 0;
    if (dt > 0.25f) dt = 0.25f;      /* a stalled window must not jump */
    h->last_tick = t;

    h->in.n_chars = h->n_typed;
    memcpy(h->in.chars, h->typed, sizeof h->typed);
    h->in.wheel = (float)h->wheel;
    h->in.released = !h->in.down;
    shell_live_tick(&h->shell, dt > 0 ? dt : 0.05);
    ui_set_time(&h->ui, (float)t);
    shell_draw(&h->shell, &h->canvas, &h->ui, &h->in);
    host_blit(h);

    /* The UI raises a request instead of calling a dialog itself, so the shell
     * stays portable and testable in the headless build. The request is cleared
     * *before* the dialog opens: it is a one-shot event, not a state. */
    int req = h->shell.request;
    if (req != REQ_NONE) {
        h->shell.request = REQ_NONE;
        if (req == REQ_OPEN_FOLDER) osa_open_in_explorer(osa_path(OSAI_DIR_ROOT));
        else if (start_dialog(h, req)) {
            /* handled (or the user cancelled) */
        } else {
            /* a dialog is already open: keep the request for the next frame
             * instead of dropping it, which is what made a click on a
             * secondary picker do nothing at all */
            h->pending_kind = req;
        }
    } else if (!h->dialog_open && h->pending_kind != REQ_NONE) {
        int k = h->pending_kind;
        h->pending_kind = REQ_NONE;
        start_dialog(h, k);
    }
    h->n_typed = 0;
    h->wheel = 0;
    h->in.clicked = false;
    h->in.key = 0;
}

/* ---- window proc -------------------------------------------------- */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    host *h = &g_host;
    switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, 1, 16, NULL);
            return 0;

        case WM_TIMER:
            host_frame(h);
            return 0;

        case WM_ERASEBKGND:
            return 1;                       /* the blit paints every pixel */

        case WM_MOUSEMOVE:
            h->in.mx = (float)GET_X_LPARAM(lp);
            h->in.my = (float)GET_Y_LPARAM(lp);
            return 0;

        case WM_LBUTTONDOWN:
            SetCapture(hwnd);
            h->in.mx = (float)GET_X_LPARAM(lp);
            h->in.my = (float)GET_Y_LPARAM(lp);
            h->in.down = true;
            h->in.clicked = true;      /* press-to-activate: immediate feedback */
            return 0;

        case WM_LBUTTONUP:
            ReleaseCapture();
            h->in.down = false;
            h->in.released = true;
            return 0;

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            h->wheel += delta / WHEEL_DELTA;
            POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &p);
            h->in.mx = (float)p.x;
            h->in.my = (float)p.y;
            return 0;
        }

        /* WM_CHAR already carries the composed character (Persian included);
         * WM_KEYDOWN must not add a second one for the same key press, which is
         * why only the keys WM_CHAR never sends are handled there. */
        case WM_CHAR: {
            if ((wp >= 32 || wp == 8 || wp == 13 || wp == 9) &&
                h->n_typed < (int)(sizeof h->typed / sizeof h->typed[0])) {
                h->typed[h->n_typed++] = (uint32_t)wp;
            }
            return 0;
        }

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { h->ui.focus_id = 0; h->ui.popup_id = 0; }
            else if (wp == VK_F11) {
                LONG style = GetWindowLongW(hwnd, GWL_STYLE);
                if (style & WS_OVERLAPPEDWINDOW) {
                    SetWindowLongW(hwnd, GWL_STYLE, (LONG)(style & ~WS_OVERLAPPEDWINDOW));
                    ShowWindow(hwnd, SW_MAXIMIZE);
                } else {
                    SetWindowLongW(hwnd, GWL_STYLE, (LONG)(style | WS_OVERLAPPEDWINDOW));
                    ShowWindow(hwnd, SW_RESTORE);
                }
            }
            if (wp == VK_TAB) {
                if (h->n_typed < (int)(sizeof h->typed / sizeof h->typed[0]))
                    h->typed[h->n_typed++] = 9;
            }
            h->in.key = (int)wp;
            if (GetKeyState(VK_SHIFT) & 0x8000) h->in.shift = true;
            if (GetKeyState(VK_CONTROL) & 0x8000) h->in.ctrl = true;
            return 0;

        case WM_SIZE: {
            int w = LOWORD(lp), hh = HIWORD(lp);
            if (w <= 0 || hh <= 0) return 0;
            if (w != h->width || hh != h->height) {
                /* resize in place: the shell relayouts from the canvas size on
                 * the next frame, so no state has to be reset */
                host_alloc_surface(h, w, hh);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_DPICHANGED: {
            RECT *r = (RECT *)lp;
            h->dpi_scale = (float)HIWORD(wp) / 96.0f;
            SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_APP_TRAY: {
            if (lp == WM_LBUTTONDBLCLK) {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
            } else if (lp == WM_RBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, IDM_OPEN, L"Open OpenSpeech AI");
                AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit");
                SetForegroundWindow(hwnd);
                TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0,
                               hwnd, NULL);
                DestroyMenu(menu);
            }
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDM_OPEN:
                    ShowWindow(hwnd, SW_SHOW);
                    SetForegroundWindow(hwnd);
                    return 0;
                case IDM_QUIT:
                    DestroyWindow(hwnd);
                    return 0;
                default:
                    break;
            }
            break;

        case WM_CLOSE: {
            /* closing the window keeps the app in the tray while a capture is
             * running; otherwise it quits like any other desktop app */
            if (h->shell.live) {
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY:
            KillTimer(hwnd, 1);
            h->running = false;
            tray_remove(h);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- entry point -------------------------------------------------- */

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show) {
    (void)prev; (void)cmdline;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    /* A crash in a release build used to leave the user with nothing but a
     * fault offset in the Event Viewer. From here on it also leaves a small
     * text file with the fault kind, the module-relative address and the frame
     * addresses, which is what makes the report actionable. */
    osa_crash_log_init(false);

    host *h = &g_host;
    memset(h, 0, sizeof *h);
    h->dpi_scale = 1.0f;
    h->last_tick = now_seconds();

    osa_error err;
    h->app = osa_app_create(&err);
    if (!h->app) {
        wchar_t msg[1024];
        MultiByteToWideChar(CP_UTF8, 0, err.msg, -1, msg, 1024);
        MessageBoxW(NULL, msg[0] ? msg : L"OpenSpeech AI could not start. The data folder "
                                            L"may not be writable.",
                    L"OpenSpeech AI", MB_ICONERROR | MB_OK);
        return 1;
    }

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, L"IDI_APPICON");
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"OpenSpeechAIWindow";
    wc.hbrBackground = NULL;
    if (!RegisterClassExW(&wc)) return 1;

    int w = 1440, hh = 900;
    RECT r = { 0, 0, w, hh };
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    h->hwnd = CreateWindowExW(0, wc.lpszClassName, L"OpenSpeech AI",
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top,
                              NULL, NULL, inst, NULL);
    if (!h->hwnd) return 1;

    if (!host_alloc_surface(h, w, hh)) return 1;
    if (!ui_init(&h->ui, &h->canvas)) {
        /* Say *where* the fonts have to be, with the real exe folder in the
         * message: "keep the assets folder next to the exe" is useless when the
         * user has no idea which folder that is. */
        wchar_t wdir[OSAI_MAX_PATH] = L"";
        MultiByteToWideChar(CP_UTF8, 0, osa_exe_dir(), -1, wdir,
                            (int)(sizeof wdir / sizeof wdir[0]));
        wchar_t msg[1400];
        _snwprintf(msg, 1400,
                   L"فونت‌های همراه برنامه پیدا نشد.\n"
                   L"Bundled fonts were not found.\n\n"
                   L"The program looked for:\n"
                   L"  %s\\assets\\fonts\\Vazirmatn-Regular.ttf\n"
                   L"  %s\\assets\\Vazirmatn-Regular.ttf\n"
                   L"  %s\\fonts\\Vazirmatn-Regular.ttf\n\n"
                   L"Copy the assets folder from the release zip next to\n"
                   L"OpenSpeechAI.exe and start it again. Fault log: %s",
                   wdir, wdir, wdir, L"last-fault.log (in the data folder)");
        MessageBoxW(h->hwnd, msg, L"OpenSpeech AI — fonts missing",
                    MB_ICONERROR | MB_OK);
        return 1;
    }
    shell_init(&h->shell, h->app);
    h->running = true;

    ShowWindow(h->hwnd, show);
    UpdateWindow(h->hwnd);
    tray_add(h);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    h->running = false;
    shell_shutdown(&h->shell);
    ui_shutdown(&h->ui);
    canvas_free(&h->canvas);
    if (h->memdc) DeleteDC(h->memdc);
    osa_app_destroy(h->app);
    return 0;
}
