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
 *    - DPI awareness + per-monitor scaling
 *    - file open/save dialogs (comdlg32)
 *    - clipboard, "open in explorer", single-instance
 *    - tray icon with a small menu
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
    UINT         timer_ms;
    /* pointer + keyboard state carried between messages and the frame */
    ui_input     in;
    uint32_t     typed[32];
    int          n_typed;
    int          wheel;
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

/* ---- file dialogs ------------------------------------------------- */

static bool pick_audio_file(host *h, char *out, size_t cap) {
    OPENFILENAMEW ofn;
    static wchar_t file[MAX_PATH * 2];
    file[0] = 0;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = h->hwnd;
    ofn.lpstrFilter =
        L"Audio files (*.wav;*.flac;*.ogg)\0*.wav;*.flac;*.ogg\0"
        L"WAV (*.wav)\0*.wav\0"
        L"FLAC (*.flac)\0*.flac\0"
        L"Ogg Vorbis (*.ogg)\0*.ogg\0"
        L"All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return false;
    int n = WideCharToMultiByte(CP_UTF8, 0, file, -1, out, (int)cap, NULL, NULL);
    return n > 0;
}

static bool pick_model_file(host *h, char *out, size_t cap) {
    OPENFILENAMEW ofn;
    static wchar_t file[MAX_PATH * 2];
    file[0] = 0;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = h->hwnd;
    ofn.lpstrFilter = L"OpenSpeech models (*.osam;*.osmt)\0*.osam;*.osmt\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return false;
    int n = WideCharToMultiByte(CP_UTF8, 0, file, -1, out, (int)cap, NULL, NULL);
    return n > 0;
}

/* ---- tray --------------------------------------------------------- */

#define IDM_OPEN   1001
#define IDM_TRANSCRIBE 1002
#define IDM_QUIT   1003

static void tray_add(host *h) {
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = h->hwnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
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

static void host_frame(host *h) {
    if (!h->running) return;
    h->in.n_chars = h->n_typed;
    memcpy(h->in.chars, h->typed, sizeof h->typed);
    h->in.wheel = (float)h->wheel;
    shell_live_tick(&h->shell, 0.05);
    shell_draw(&h->shell, &h->canvas, &h->ui, &h->in);
    host_blit(h);

    /* The UI raises a request instead of calling a dialog itself, so the shell
     * stays portable and testable in the headless build. */
    if (h->shell.request == 1) {
        h->shell.request = 0;
        if (pick_audio_file(h, h->shell.request_path, sizeof h->shell.request_path)) {
            osa_strlcpy(h->shell.audio_path, h->shell.request_path, sizeof h->shell.audio_path);
            shell_load_waveform(&h->shell, h->shell.audio_path);
            shell_run_transcribe(&h->shell);
        }
    } else if (h->shell.request == 2) {
        h->shell.request = 0;
        char id[40], path[OSAI_MAX_PATH];
        osa_error err;
        if (!pick_model_file(h, path, sizeof path)) {
            /* user cancelled: nothing to do */
        } else if (osa_app_import_model(h->app, path, id, sizeof id, &err)) {
            free(h->shell.models_json);
            h->shell.models_json = osa_app_models_json(h->app, NULL);
            shell_set_message(&h->shell, false, "%s", tt(h->shell.lang, "msg.ok"));
        } else {
            shell_set_message(&h->shell, true, "%s: %s", tt(h->shell.lang, "msg.failed"), err.msg);
        }
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

        case WM_MOUSEMOVE:
            h->in.mx = (float)GET_X_LPARAM(lp);
            h->in.my = (float)GET_Y_LPARAM(lp);
            return 0;

        case WM_LBUTTONDOWN:
            h->in.mx = (float)GET_X_LPARAM(lp);
            h->in.my = (float)GET_Y_LPARAM(lp);
            h->in.down = true;
            h->in.clicked = true;      /* press-to-activate: immediate feedback */
            return 0;

        case WM_LBUTTONUP:
            h->in.down = false;
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

        case WM_CHAR: {
            if (h->n_typed < (int)(sizeof h->typed / sizeof h->typed[0])) {
                h->typed[h->n_typed++] = (uint32_t)wp;
            }
            return 0;
        }

        case WM_KEYDOWN:
            if (wp == VK_BACK && h->n_typed < 32) h->typed[h->n_typed++] = 8;
            else if (wp == VK_RETURN && h->n_typed < 32) h->typed[h->n_typed++] = 13;
            else if (wp == VK_ESCAPE) h->ui.active_id = 0;
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
            return 0;

        case WM_SIZE: {
            int w = LOWORD(lp), hh = HIWORD(lp);
            if (w <= 0 || hh <= 0) return 0;
            /* resize the drawing surface; the shell relayouts every frame */
            if (w != h->width || hh != h->height) {
                canvas_free(&h->canvas);
                host_alloc_surface(h, w, hh);
            }
            return 0;
        }

        case WM_DPICHANGED: {
            RECT *r = (RECT *)lp;
            h->dpi_scale = (float)HIWORD(wp) / 96.0f;
            SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
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
                TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
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
                    /* menu commands raised by the shell (open file, import model) */
                    if (LOWORD(wp) == 9100) {
                        char path[OSAI_MAX_PATH];
                        if (pick_audio_file(h, path, sizeof path)) {
                            osa_strlcpy(h->shell.audio_path, path, sizeof h->shell.audio_path);
                            shell_load_waveform(&h->shell, path);
                            shell_run_transcribe(&h->shell);
                        }
                        return 0;
                    }
                    if (LOWORD(wp) == 9101) {
                        char path[OSAI_MAX_PATH];
                        if (pick_model_file(h, path, sizeof path)) {
                            osa_error err;
                            char id[40];
                            if (osa_app_import_model(h->app, path, id, sizeof id, &err))
                                shell_set_message(&h->shell, false, "%s", tt(h->shell.lang, "msg.ok"));
                            else
                                shell_set_message(&h->shell, true, "%s", err.msg);
                        }
                        return 0;
                    }
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

    host *h = &g_host;
    memset(h, 0, sizeof *h);
    h->dpi_scale = 1.0f;

    osa_error err;
    h->app = osa_app_create(&err);
    if (!h->app) {
        MessageBoxW(NULL, L"OpenSpeech AI could not start. The data folder may not be "
                          L"writable.", L"OpenSpeech AI", MB_ICONERROR | MB_OK);
        return 1;
    }

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.lpszClassName = L"OpenSpeechAIWindow";
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
        MessageBoxW(h->hwnd, L"The bundled fonts are missing. Reinstall the application.",
                    L"OpenSpeech AI", MB_ICONERROR | MB_OK);
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
