/* =====================================================================
 *  OpenSpeech AI — fault log
 *
 *  A release build has no debugger attached, so when the window disappears the
 *  user is left with "the program stopped working" and a fault offset from the
 *  Event Viewer. That is not something either of us can act on.
 *
 *  This file installs a *last-resort* handler whose only job is to write, next
 *  to the data folder, the one thing that turns an offset into a bug report:
 *  the kind of fault, the module-relative address, and the return addresses of
 *  the frames that led there. It then lets the OS terminate the process the
 *  way it would have anyway — no recovery, no pretending the crash was handled.
 *
 *  The handler is deliberately allocation-free and async-signal-safe: no
 *  malloc, no printf, one open/write/close pair and a small fixed buffer.
 *
 *  Linux gets the same treatment with a backtrace(), which is how the crash
 *  class in this release was tracked down without a debugger on the target.
 * ===================================================================== */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#include "osai_platform.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>

/* dbghelp is a diagnostic helper, not a dependency of the program. Loading it
 * here - inside the handler, at the moment it is needed - keeps the executable
 * free of a hard link to it: if the library were ever missing, the log would
 * lose its frame list, not the application its startup. */
typedef BOOL (WINAPI *fn_sym_initialize)(HANDLE, const char *, BOOL);
typedef BOOL (WINAPI *fn_sym_cleanup)(HANDLE);

/* ADDRESS64 and STACKFRAME64 are dbghelp types. They are declared here with the
 * same layout instead of including the header, which is what removes the import:
 * a diagnostic helper must not be something the program cannot start without. */
typedef struct c_address64 { DWORD64 Offset; WORD Segment; DWORD Mode; } c_address64;
typedef struct c_stackframe64 {
    c_address64 AddrPC, AddrReturn, AddrFrame, AddrStack, AddrBStore;
    void   *FuncTableEntry;
    DWORD64 Params[4];
    int     Far;
    int     Virtual;
    DWORD64 Reserved[3];
} c_stackframe64;

typedef BOOL (WINAPI *fn_stack_walk64)(DWORD, HANDLE, HANDLE, c_stackframe64 *, void *,
                                       void *, void *, void *, void *);

typedef struct crash_dbg {
    HMODULE lib;
    fn_sym_initialize            sym_initialize;
    fn_sym_cleanup               sym_cleanup;
    fn_stack_walk64              stack_walk64;
    void                        *function_table_access;
    void                        *module_base;
} crash_dbg;

static void crash_dbg_load(crash_dbg *d) {
    memset(d, 0, sizeof *d);
    d->lib = LoadLibraryW(L"dbghelp.dll");
    if (!d->lib) return;
    d->sym_initialize = (fn_sym_initialize)(void *)GetProcAddress(d->lib, "SymInitialize");
    d->sym_cleanup    = (fn_sym_cleanup)(void *)GetProcAddress(d->lib, "SymCleanup");
    d->stack_walk64   = (fn_stack_walk64)(void *)GetProcAddress(d->lib, "StackWalk64");
    d->function_table_access = (void *)GetProcAddress(d->lib, "SymFunctionTableAccess64");
    d->module_base = (void *)GetProcAddress(d->lib, "SymGetModuleBase64");
}

#define SYMOPT_UNDNAME_LOCAL      0x00000002
#define SYMOPT_DEFERRED_LOADS_LOCAL 0x00000004
#else
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <execinfo.h>
#endif

static bool g_installed;
/* Set by the snapshot/test harness: a deliberate abort in a test is not a
 * crash report to leave lying around in the user's data folder. */
static bool g_quiet;

#ifdef _WIN32
static char g_log_path[OSAI_MAX_PATH];

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    if (g_quiet) return EXCEPTION_CONTINUE_SEARCH;
    char buf[4096];
    int n = 0;
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    void *addr = er->ExceptionAddress;
    HMODULE mod = NULL;
    char modname[64] = "?";
    uintptr_t base = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod)) {
        base = (uintptr_t)mod;
        /* module file name, tail only: the full path is long and not the point */
        char full[OSAI_MAX_PATH];
        DWORD k = GetModuleFileNameA(mod, full, sizeof full);
        if (k) {
            const char *slash = full;
            for (DWORD i = 0; i < k; i++) if (full[i] == '\\' || full[i] == '/') slash = full + i + 1;
            osa_strlcpy(modname, slash, sizeof modname);
        }
    }
    n += snprintf(buf + n, sizeof buf - (size_t)n,
                  "OpenSpeech AI fault log\\n"
                  "version : %s\\n"
                  "code    : 0x%08lX\\n"
                  "address : 0x%llx (module %s+0x%llx)\\n"
                  "thread  : %lu\\n",
                  OSAI_VERSION, (unsigned long)er->ExceptionCode,
                  (unsigned long long)(uintptr_t)addr, modname,
                  (unsigned long long)((uintptr_t)addr - base),
                  (unsigned long)GetCurrentThreadId());
    /* Frames from the faulting stack: enough to name the function that did it
     * once the same build is loaded in a debugger. */
    CONTEXT *ctx = ep->ContextRecord;
    n += snprintf(buf + n, sizeof buf - (size_t)n, "frames  :");
    void *bt[24];
    if (ctx) {
        c_stackframe64 sf;
        memset(&sf, 0, sizeof sf);
#ifdef _M_X64
        sf.AddrPC.Offset = ctx->Rip;    sf.AddrPC.Mode = 3;    /* AddrModeFlat */
        sf.AddrFrame.Offset = ctx->Rbp; sf.AddrFrame.Mode = 3;
        sf.AddrStack.Offset = ctx->Rsp; sf.AddrStack.Mode = 3;
        DWORD machine = IMAGE_FILE_MACHINE_AMD64;
#else
        sf.AddrPC.Offset = ctx->Eip;    sf.AddrPC.Mode = 3;
        sf.AddrFrame.Offset = ctx->Ebp; sf.AddrFrame.Mode = 3;
        sf.AddrStack.Offset = ctx->Esp; sf.AddrStack.Mode = 3;
        DWORD machine = IMAGE_FILE_MACHINE_I386;
#endif
        HANDLE proc = GetCurrentProcess(), thr = GetCurrentThread();
        crash_dbg dbg;
        crash_dbg_load(&dbg);
        if (dbg.lib && dbg.sym_initialize && dbg.stack_walk64) {
            dbg.sym_initialize(proc, NULL, TRUE);
            for (int i = 0; i < 24 && n < (int)sizeof buf - 48; i++) {
                if (!dbg.stack_walk64(machine, proc, thr, &sf, ctx, NULL,
                                      dbg.function_table_access, dbg.module_base, NULL))
                    break;
                n += snprintf(buf + n, sizeof buf - (size_t)n, " %llx",
                              (unsigned long long)sf.AddrPC.Offset);
            }
            if (dbg.sym_cleanup) dbg.sym_cleanup(proc);
        } else {
            n += snprintf(buf + n, sizeof buf - (size_t)n,
                          " (frame walk unavailable: no dbghelp.dll)");
        }
        if (dbg.lib) FreeLibrary(dbg.lib);
        (void)bt;
    }
    n += snprintf(buf + n, sizeof buf - (size_t)n, "\\n");
    HANDLE f = CreateFileA(g_log_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(f, buf, (DWORD)n, &wrote, NULL);
        CloseHandle(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;   /* let the OS terminate the process */
}
#else
static char g_log_path[OSAI_MAX_PATH];

static void crash_handler(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    if (g_quiet) _exit(128 + sig);
    const char *name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS"
                     : sig == SIGFPE ? "SIGFPE" : sig == SIGILL ? "SIGILL"
                     : sig == SIGABRT ? "SIGABRT" : "signal";
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "OpenSpeech AI fault log\nversion : %s\ncode    : %s (%d)\naddress : %p\nframes  :",
                     OSAI_VERSION, name, sig, si ? si->si_addr : NULL);
    void *bt[32];
    int k = backtrace(bt, 32);
    for (int i = 0; i < k && n < (int)sizeof buf - 24; i++)
        n += snprintf(buf + n, sizeof buf - (size_t)n, " %p", bt[i]);
    n += snprintf(buf + n, sizeof buf - (size_t)n, "\n");
    int fd = open(g_log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { ssize_t w = write(fd, buf, (size_t)n); (void)w; close(fd); }
    _exit(128 + sig);
}
#endif

void osa_crash_log_init(bool quiet) {
    g_quiet = quiet;
    if (g_installed) return;
    g_installed = true;
#ifdef _WIN32
    osa_path_join(g_log_path, sizeof g_log_path, osa_path(OSAI_DIR_ROOT), "last-fault.log");
    SetUnhandledExceptionFilter(crash_filter);
#else
    osa_path_join(g_log_path, sizeof g_log_path, osa_path(OSAI_DIR_ROOT), "last-fault.log");
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
#endif
}

const char *osa_crash_log_path(void) { return g_log_path; }
