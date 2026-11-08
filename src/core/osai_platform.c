/* Portable platform layer: filesystem, threads, clocks, metrics, process
 * control.
 *
 * Both implementations live in this file, chosen by `_WIN32`: Win32
 * (CreateFile/GetProcessMemoryInfo) on Windows, POSIX (stat/opendir/getrusage)
 * elsewhere. src/platform/win32/ holds only the Windows entry point and
 * resources (`main_win.c`, `app.rc`, manifest, icon), not the platform layer. */
#include "osai_platform.h"

#ifdef _WIN32
#include <windows.h>
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 1
#endif
#include <psapi.h>
#include <direct.h>
#include <winhttp.h>
#include <mmsystem.h>
#else
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#endif

#include <sys/types.h>

/* ============================== paths ============================== */
static char g_paths[OSAI_DIR_COUNT][OSAI_MAX_PATH];
static char g_exe_dir[OSAI_MAX_PATH];
static bool g_paths_ready = false;

static const char *dir_names[OSAI_DIR_COUNT] = {
    "", "Models", "Datasets", "Downloads", "Database", "Logs", "Licenses",
    "Backups", "Audio", "Exports", "Cache", "Checkpoints", "LanguagePacks"
};

static bool mkdirs_impl(const char *path) {
#ifdef _WIN32
    char tmp[OSAI_MAX_PATH];
    osa_strlcpy(tmp, path, sizeof tmp);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char c = *p; *p = 0;
            _mkdir(tmp);
            *p = c;
        }
    }
    _mkdir(tmp);
    return true;
#else
    char tmp[OSAI_MAX_PATH];
    osa_strlcpy(tmp, path, sizeof tmp);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { char c = *p; *p = 0; if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { *p = c; return false; } *p = c; }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
    return true;
#endif
}

bool osa_mkdirs(const char *path) { return mkdirs_impl(path); }

bool osa_paths_init(void) {
    if (g_paths_ready) return true;
    char base[OSAI_MAX_PATH] = {0};
#ifdef _WIN32
    const char *lad = getenv("LOCALAPPDATA");
    if (lad && *lad) snprintf(base, sizeof base, "%s\\%s", lad, OSAI_APP_ID);
    else snprintf(base, sizeof base, "C:\\%s", OSAI_APP_ID);
    /* exe dir */
    GetModuleFileNameA(NULL, g_exe_dir, sizeof g_exe_dir);
    char *bs = strrchr(g_exe_dir, '\\');
    if (bs) *bs = 0;
#else
    const char *ovr = getenv("OSAI_DATA_DIR");
    const char *home = getenv("HOME");
    if (ovr && *ovr) snprintf(base, sizeof base, "%s", ovr);
    else if (home && *home) snprintf(base, sizeof base, "%s/.local/share/%s", home, OSAI_APP_ID);
    else snprintf(base, sizeof base, "/tmp/%s", OSAI_APP_ID);
    if (!getcwd(g_exe_dir, sizeof g_exe_dir)) osa_strlcpy(g_exe_dir, ".", sizeof g_exe_dir);
#endif
    osa_strlcpy(g_paths[OSAI_DIR_ROOT], base, OSAI_MAX_PATH);
    mkdirs_impl(base);
    for (int i = 1; i < OSAI_DIR_COUNT; i++) {
#ifdef _WIN32
        snprintf(g_paths[i], OSAI_MAX_PATH, "%s\\%s\\", base, dir_names[i]);
#else
        snprintf(g_paths[i], OSAI_MAX_PATH, "%s/%s/", base, dir_names[i]);
#endif
        mkdirs_impl(g_paths[i]);
    }
#ifdef _WIN32
    snprintf(g_paths[OSAI_DIR_ROOT], OSAI_MAX_PATH, "%s\\", base);
#else
    snprintf(g_paths[OSAI_DIR_ROOT], OSAI_MAX_PATH, "%s/", base);
#endif
    g_paths_ready = true;
    return true;
}

const char *osa_path(osa_dir_kind k) {
    if (!g_paths_ready) osa_paths_init();
    if (k < 0 || k >= OSAI_DIR_COUNT) k = OSAI_DIR_ROOT;
    return g_paths[k];
}
const char *osa_exe_dir(void) { if (!g_paths_ready) osa_paths_init(); return g_exe_dir; }

void osa_path_join(char *out, size_t cap, const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    size_t la = strlen(a);
    bool sep = la && (a[la-1] == '/' || a[la-1] == '\\');
    snprintf(out, cap, "%s%s%s", a, sep ? "" : "/", b);
}

const char *osa_path_basename(const char *p) {
    if (!p) return "";
    const char *s = strrchr(p, '/');
    const char *w = strrchr(p, '\\');
    if (w > s) s = w;
    return s ? s + 1 : p;
}

const char *osa_path_ext(const char *p) {
    const char *b = osa_path_basename(p);
    const char *d = strrchr(b, '.');
    return d ? d : "";
}

bool osa_path_replace_ext(char *out, size_t cap, const char *path, const char *newext) {
    const char *e = osa_path_ext(path);
    size_t keep = e ? (size_t)(e - path) : strlen(path);
    if (keep + strlen(newext) + 1 > cap) return false;
    memcpy(out, path, keep);
    osa_strlcpy(out + keep, newext, cap - keep);
    return true;
}

int64_t osa_disk_free(const char *path) {
#ifdef _WIN32
    ULARGE_INTEGER freeb;
    if (GetDiskFreeSpaceExA(path, &freeb, NULL, NULL)) return (int64_t)freeb.QuadPart;
    return -1;
#else
    struct statvfs st;
    if (statvfs(path, &st) != 0) return -1;
    return (int64_t)st.f_bavail * (int64_t)st.f_frsize;
#endif
}

/* ============================== files ============================== */
bool osa_file_exists(const char *path) {
    if (!path || !*path) return false;
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool osa_dir_exists(const char *path) {
    if (!path || !*path) return false;
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

int64_t osa_file_size(const char *path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return -1;
    return ((int64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
#else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
#endif
}

int64_t osa_file_mtime(const char *path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return 0;
    return ((int64_t)d.ftLastWriteTime.dwHighDateTime << 32 | d.ftLastWriteTime.dwLowDateTime) / 10000 - 11644473600000ll;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (int64_t)st.st_mtime * 1000;
#endif
}

bool osa_file_read(const char *path, osa_buf *out) {
    osa_buf_reset(out);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) osa_buf_putn(out, tmp, n);
    fclose(f);
    return !out->oom;
}

bool osa_file_write(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = n == 0 || fwrite(data, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;
    return ok;
}

bool osa_file_write_atomic(const char *path, const void *data, size_t n) {
    char tmp[OSAI_MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!osa_file_write(tmp, data, n)) { osa_file_remove(tmp); return false; }
    if (!osa_file_rename(tmp, path)) { osa_file_remove(tmp); return false; }
    return true;
}

bool osa_file_append(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "ab");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
}

bool osa_file_remove(const char *path) {
#ifdef _WIN32
    return DeleteFileA(path) != 0;
#else
    return unlink(path) == 0;
#endif
}

bool osa_file_rename(const char *from, const char *to) {
#ifdef _WIN32
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING)) return true;
    return false;
#else
    return rename(from, to) == 0;
#endif
}

bool osa_file_copy(const char *from, const char *to) {
    FILE *fi = fopen(from, "rb");
    if (!fi) return false;
    FILE *fo = fopen(to, "wb");
    if (!fo) { fclose(fi); return false; }
    static char buf[1 << 16];
    size_t n; bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, fi)) > 0)
        if (fwrite(buf, 1, n, fo) != n) { ok = false; break; }
    fclose(fi);
    if (fclose(fo) != 0) ok = false;
    return ok;
}

int osa_dir_scan(const char *dir, const char *ext_filter, char ***names, int max_names) {
    *names = NULL;
    int count = 0, cap = 0;
    char **list = NULL;
#ifdef _WIN32
    char pattern[OSAI_MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (ext_filter && *ext_filter && !osa_ends(fd.cFileName, ext_filter)) continue;
        if (count == cap) { cap = cap ? cap * 2 : 32; list = (char **)realloc(list, (size_t)cap * sizeof(char *)); }
        list[count++] = osa_strdup(fd.cFileName);
        if (count >= max_names) break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[OSAI_MAX_PATH];
        osa_path_join(full, sizeof full, dir, e->d_name);
        if (osa_dir_exists(full)) continue;
        if (ext_filter && *ext_filter && !osa_ends(e->d_name, ext_filter)) continue;
        if (count == cap) { cap = cap ? cap * 2 : 32; list = (char **)realloc(list, (size_t)cap * sizeof(char *)); }
        list[count++] = osa_strdup(e->d_name);
        if (count >= max_names) break;
    }
    closedir(d);
#endif
    *names = list;
    return count;
}

void osa_dir_free_list(char **names, int n) {
    if (!names) return;
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

int64_t osa_dir_size(const char *dir) {
    char **names = NULL;
    int n = osa_dir_scan(dir, NULL, &names, 100000);
    int64_t total = 0;
    for (int i = 0; i < n; i++) {
        char full[OSAI_MAX_PATH];
        osa_path_join(full, sizeof full, dir, names[i]);
        if (osa_dir_exists(full)) total += osa_dir_size(full);
        else total += osa_file_size(full);
    }
    osa_dir_free_list(names, n);
    return total;
}

bool osa_dir_remove_recursive(const char *dir) {
    char **names = NULL;
    int n = osa_dir_scan(dir, NULL, &names, 100000);
    for (int i = 0; i < n; i++) {
        char full[OSAI_MAX_PATH];
        osa_path_join(full, sizeof full, dir, names[i]);
        if (osa_dir_exists(full)) osa_dir_remove_recursive(full);
        else osa_file_remove(full);
    }
    osa_dir_free_list(names, n);
#ifdef _WIN32
    return RemoveDirectoryA(dir) != 0;
#else
    return rmdir(dir) == 0;
#endif
}

/* ============================== threads ============================== */
#ifdef _WIN32
void osa_mutex_init(osa_mutex *m) {
    CRITICAL_SECTION *cs = (CRITICAL_SECTION *)malloc(sizeof(CRITICAL_SECTION));
    InitializeCriticalSection(cs);
    m->h = cs;
}
void osa_mutex_destroy(osa_mutex *m) {
    if (!m->h) return;
    DeleteCriticalSection((CRITICAL_SECTION *)m->h);
    free(m->h); m->h = NULL;
}
void osa_mutex_lock(osa_mutex *m) { EnterCriticalSection((CRITICAL_SECTION *)m->h); }
void osa_mutex_unlock(osa_mutex *m) { LeaveCriticalSection((CRITICAL_SECTION *)m->h); }

typedef struct { osa_thread_fn fn; void *ud; } win_thunk;
static DWORD WINAPI win_thread_main(LPVOID p) {
    win_thunk *t = (win_thunk *)p;
    t->fn(t->ud);
    free(t);
    return 0;
}
bool osa_thread_start(osa_thread *t, osa_thread_fn fn, void *ud) {
    win_thunk *th = (win_thunk *)malloc(sizeof *th);
    th->fn = fn; th->ud = ud;
    HANDLE h = CreateThread(NULL, 0, win_thread_main, th, 0, NULL);
    t->h = h;
    return h != NULL;
}
void osa_thread_join(osa_thread *t) {
    if (!t->h) return;
    WaitForSingleObject((HANDLE)t->h, INFINITE);
    CloseHandle((HANDLE)t->h);
    t->h = NULL;
}
void osa_thread_detach(osa_thread *t) { if (t->h) { CloseHandle((HANDLE)t->h); t->h = NULL; } }
void osa_thread_exit(void) { ExitThread(0); }
int osa_cpu_count(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
#else
void osa_mutex_init(osa_mutex *m) {
    pthread_mutex_t *pm = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(pm, NULL);
    m->h = pm;
}
void osa_mutex_destroy(osa_mutex *m) {
    if (!m->h) return;
    pthread_mutex_destroy((pthread_mutex_t *)m->h);
    free(m->h); m->h = NULL;
}
void osa_mutex_lock(osa_mutex *m) { pthread_mutex_lock((pthread_mutex_t *)m->h); }
void osa_mutex_unlock(osa_mutex *m) { pthread_mutex_unlock((pthread_mutex_t *)m->h); }

bool osa_thread_start(osa_thread *t, osa_thread_fn fn, void *ud) {
    pthread_t *pt = (pthread_t *)malloc(sizeof(pthread_t));
    if (pthread_create(pt, NULL, (void *(*)(void *))fn, ud) != 0) { free(pt); t->h = NULL; return false; }
    t->h = pt;
    return true;
}
void osa_thread_join(osa_thread *t) {
    if (!t->h) return;
    pthread_join(*(pthread_t *)t->h, NULL);
    free(t->h); t->h = NULL;
}
void osa_thread_detach(osa_thread *t) {
    if (!t->h) return;
    pthread_detach(*(pthread_t *)t->h);
    free(t->h); t->h = NULL;
}
void osa_thread_exit(void) { pthread_exit(NULL); }
int osa_cpu_count(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
#endif

int osa_atomic_inc(volatile int *p) {
#ifdef _WIN32
    return (int)InterlockedIncrement((volatile LONG *)p);
#else
    return __sync_add_and_fetch(p, 1);
#endif
}
int osa_atomic_load(const volatile int *p) {
#ifdef _WIN32
    return (int)InterlockedCompareExchange((volatile LONG *)p, 0, 0);
#else
    return __sync_add_and_fetch((volatile int *)p, 0);
#endif
}

/* ============================== system stats ============================== */
static struct {
    int64_t last_cpu_total, last_cpu_idle;
    int64_t last_proc_ticks, last_proc_ms;
    bool inited;
    char gpu_name[128];
    bool gpu_probed;
} g_stats;

void osa_sys_stats_init(void) {
    memset(&g_stats, 0, sizeof g_stats);
    g_stats.inited = true;
}

/* Set by the snapshot harness. System metrics are live by nature, so a still
 * rendered twice would differ in its RAM figure and the visual-regression job
 * would report a change that is not one. */
static bool g_stats_frozen = false;
void osa_sys_stats_freeze(bool frozen) { g_stats_frozen = frozen; }
bool osa_sys_stats_frozen(void) { return g_stats_frozen; }

float osa_process_ram_mb(void) {
    /* Deliberately parts of the same sample: RSS is the one reading that
     * describes this process rather than the machine it shares with everything
     * else the user is running, so it is what "memory in use" means on screen. */
    osa_sys_stats live;
    osa_sys_stats_sample(&live);
    return live.proc_ram_mb;
}

void osa_sys_stats_sample(osa_sys_stats *out) {
    memset(out, 0, sizeof *out);
    if (g_stats_frozen) {
        /* A plausible, fixed machine. The About screen draws these values
         * during a snapshot run, so they must not vary between two renders of
         * the same still; the live path reports the real ones. */
        out->cpu_percent = 12.0f;
        out->ram_used_mb = 512.0f;
        out->ram_total_mb = 8192.0f;
        out->ram_percent = 6.25f;
        out->proc_ram_mb = 196.0f;
        out->proc_cpu_percent = 0.4f;
        out->gpu_percent = -1.0f;
        out->vram_used_mb = -1.0f;
        osa_strlcpy(out->gpu_name, "none", sizeof out->gpu_name);
        return;
    }
    out->gpu_percent = -1;
    out->vram_used_mb = -1;
#ifndef _WIN32
    /* CPU from /proc/stat */
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
        char line[512];
        if (fgets(line, sizeof line, f)) {
            long long u=0,n=0,s=0,i=0,w=0,irq=0,sirq=0,st=0;
            if (sscanf(line, "cpu %lld %lld %lld %lld %lld %lld %lld %lld",
                       &u,&n,&s,&i,&w,&irq,&sirq,&st) >= 4) {
                int64_t total = u+n+s+i+w+irq+sirq+st, idle = i + w;
                if (g_stats.last_cpu_total) {
                    int64_t dt = total - g_stats.last_cpu_total, di = idle - g_stats.last_cpu_idle;
                    if (dt > 0) out->cpu_percent = 100.0f * (float)(dt - di) / (float)dt;
                }
                g_stats.last_cpu_total = total;
                g_stats.last_cpu_idle = idle;
            }
        }
        fclose(f);
    }
    /* RAM from /proc/meminfo */
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        double total = 0, avail = 0;
        while (fgets(line, sizeof line, f)) {
            unsigned long long kb = 0;
            if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) total = (double)kb / 1024.0;
            else if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) avail = (double)kb / 1024.0;
        }
        fclose(f);
        out->ram_total_mb = (float)total;
        out->ram_used_mb = (float)(total - avail);
        if (total > 0) out->ram_percent = (float)(100.0 * (total - avail) / total);
    }
    /* process usage */
    f = fopen("/proc/self/stat", "r");
    if (f) {
        char line[1024];
        if (fgets(line, sizeof line, f)) {
            char *rp = strrchr(line, ')');
            if (rp) {
                long long ut = 0, st2 = 0, rss = 0;
                int field = 3;
                char *tok = strtok(rp + 1, " ");
                while (tok) {
                    if (field == 14) ut = atoll(tok);
                    if (field == 15) st2 = atoll(tok);
                    if (field == 24) { rss = atoll(tok); break; }
                    field++;
                    tok = strtok(NULL, " ");
                }
                long ticks = sysconf(_SC_CLK_TCK);
                int64_t pt = ut + st2;
                int64_t now = osa_now_ms();
                if (g_stats.last_proc_ticks && now > g_stats.last_proc_ms) {
                    double dt = (double)(now - g_stats.last_proc_ms) / 1000.0;
                    double used = (double)(pt - g_stats.last_proc_ticks) / (double)ticks;
                    out->proc_cpu_percent = (float)(100.0 * used / dt);
                }
                g_stats.last_proc_ticks = pt;
                g_stats.last_proc_ms = now;
                out->proc_ram_mb = (float)((double)rss * 4096.0 / (1024.0 * 1024.0));
            }
        }
        fclose(f);
    }
#else
    static ULONGLONG last_idle = 0, last_kernel = 0, last_user = 0;
    ULONGLONG idle, kernel, user;
    if (GetSystemTimes((FILETIME *)&idle, (FILETIME *)&kernel, (FILETIME *)&user)) {
        ULONGLONG dt_total = (kernel - last_kernel) + (user - last_user);
        ULONGLONG dt_idle = idle - last_idle;
        if (last_kernel && dt_total) out->cpu_percent = 100.0f * (1.0f - (float)dt_idle / (float)dt_total);
        last_idle = idle; last_kernel = kernel; last_user = user;
    }
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) {
        out->ram_total_mb = (float)(ms.ullTotalPhys / (1024.0 * 1024.0));
        out->ram_used_mb = (float)((ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0));
        out->ram_percent = (float)ms.dwMemoryLoad;
    }
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        out->proc_ram_mb = (float)(pmc.WorkingSetSize / (1024.0 * 1024.0));
#endif
    if (!g_stats.gpu_probed) {
        g_stats.gpu_probed = true;
        osa_strlcpy(g_stats.gpu_name, "", sizeof g_stats.gpu_name);
        FILE *p = popen("nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null", "r");
        if (p) {
            char line[128];
            if (fgets(line, sizeof line, p)) osa_strlcpy(g_stats.gpu_name, osa_trim_inplace(line), sizeof g_stats.gpu_name);
            pclose(p);
        }
    }
    osa_strlcpy(out->gpu_name, g_stats.gpu_name, sizeof out->gpu_name);
    out->gpu_percent = g_stats.gpu_name[0] ? 0.0f : -1.0f;
}

/* ============================== network ==============================
 * A download manager, and nothing else. The engines never open a socket; the
 * only callers are the model/language-pack downloads the user explicitly asks
 * for, and every download is verified against the SHA-256 in the catalogue
 * before it is installed.
 *
 * Windows uses WinHTTP (no libcurl, no OpenSSL: certificates come from the OS
 * store). Other targets return "not available" so the portable build keeps
 * compiling and the UI can say so honestly instead of hanging.
 * ==================================================================== */
int osa_http_download(const char *url, const char *dest_path, bool resume,
                      osa_download_progress cb, void *ud, volatile int *cancel,
                      osa_error *err) {
    if (!url || !dest_path) { osa_err_set(err, "bad arguments"); return -1; }
    if (!osa_http_has_tls() && osa_starts(url, "https")) {
        osa_err_set(err, "TLS is not available in this build");
        return -1;
    }
#ifdef _WIN32
    /* ---- WinHTTP ------------------------------------------------- */
    wchar_t wurl[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048)) {
        osa_err_set(err, "cannot convert the URL");
        return -1;
    }
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        osa_err_set(err, "malformed URL");
        return -1;
    }
    wchar_t host[512];
    size_t hl = uc.dwHostNameLength < 511 ? uc.dwHostNameLength : 511;
    memcpy(host, uc.lpszHostName, hl * sizeof(wchar_t));
    host[hl] = 0;
    wchar_t path[2048];
    size_t pl = uc.dwUrlPathLength;
    if (pl >= 2047) pl = 2047;
    memcpy(path, uc.lpszUrlPath, pl * sizeof(wchar_t));
    path[pl] = 0;

    HINTERNET ses = WinHttpOpen(L"OpenSpeechAI/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { osa_err_set(err, "cannot start WinHTTP (%lu)", GetLastError()); return -1; }
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con) {
        WinHttpCloseHandle(ses);
        osa_err_set(err, "cannot connect (%lu)", GetLastError());
        return -1;
    }
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
        osa_err_set(err, "cannot create the request (%lu)", GetLastError());
        return -1;
    }
    /* resume at the end of a partial file when asked to */
    int64_t have = 0;
    char part[OSAI_MAX_PATH];
    snprintf(part, sizeof part, "%s.part", dest_path);
    if (resume && osa_file_exists(part)) have = osa_file_size(part);
    if (have > 0) {
        wchar_t range[64];
        swprintf(range, 64, L"bytes=%lld-", (long long)have);
        WinHttpAddRequestHeaders(req, range, (DWORD)-1L,
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    BOOL sent = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!sent || !WinHttpReceiveResponse(req, NULL)) {
        osa_err_set(err, "request failed (%lu)", GetLastError());
        WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
        return -1;
    }
    DWORD status = 0, len = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status == 416) {                     /* already complete */
        osa_file_rename(part, dest_path);
        WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
        osa_err_ok(err);
        return 0;
    }
    if (status != 200 && status != 206) {
        osa_err_set(err, "server returned HTTP %lu", (unsigned long)status);
        WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
        return -1;
    }
    /* total size for the progress bar */
    int64_t total = 0;
    {
        wchar_t clen[64];
        DWORD cl = sizeof clen;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                clen, &cl, WINHTTP_NO_HEADER_INDEX) && cl)
            total = _wtoi64(clen) + have;
    }
    FILE *f = fopen(part, have > 0 ? "ab" : "wb");
    if (!f) {
        osa_err_set(err, "cannot write %s", part);
        WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
        return -1;
    }
    int64_t got = have;
    int rc = 0;
    for (;;) {
        if (cancel && *cancel) { rc = -2; break; }
        DWORD avail = 0, read = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) { rc = -1; break; }
        if (avail == 0) break;
        char buf[65536];
        DWORD chunk = avail < sizeof buf ? avail : (DWORD)sizeof buf;
        if (!WinHttpReadData(req, buf, chunk, &read)) { rc = -1; break; }
        if (read == 0) break;
        if (fwrite(buf, 1, read, f) != read) {
            osa_err_set(err, "disk full while writing %s", part);
            rc = -1;
            break;
        }
        got += read;
        if (cb) cb(got, total, ud);
        osa_sleep_ms(0);
    }
    fflush(f);
    fclose(f);
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    if (rc == 0) {
        if (!osa_file_rename(part, dest_path)) {
            osa_err_set(err, "cannot finalize %s", dest_path);
            return -1;
        }
        osa_err_ok(err);
        return 0;
    }
    if (rc == -2) { if (err) osa_err_set(err, "cancelled"); return -2; }
    if (err && !err->msg[0]) osa_err_set(err, "network error (%lu)", GetLastError());
    return -1;
#else
    (void)resume; (void)cb; (void)ud; (void)cancel;
    osa_err_set(err, "downloading is not available in this build");
    return -1;
#endif
}

int osa_http_get_text(const char *url, osa_buf *out, int timeout_ms, osa_error *err) {
    if (!url || !out) { osa_err_set(err, "bad arguments"); return -1; }
    (void)timeout_ms;
#ifdef _WIN32
    /* Reuse the downloader into a temporary file, then read it back: catalog
     * files are small (tens of KB) and this avoids a second WinHTTP path. */
    char tmp[OSAI_MAX_PATH];
    snprintf(tmp, sizeof tmp, "%sosa-catalog-%ld.json", osa_path(OSAI_DIR_DOWNLOADS),
             (long)osa_now_ms());
    int rc = osa_http_download(url, tmp, false, NULL, NULL, NULL, err);
    if (rc != 0) return rc;
    if (!osa_file_read(tmp, out)) {
        osa_file_remove(tmp);
        osa_err_set(err, "cannot read the downloaded catalogue");
        return -1;
    }
    osa_file_remove(tmp);
    if (out->len > 200000) {
        osa_err_set(err, "catalogue too large");
        return -1;
    }
    osa_err_ok(err);
    return 0;
#else
    osa_err_set(err, "downloading is not available in this build");
    return -1;
#endif
}

bool osa_http_has_tls(void) {
#ifdef _WIN32
    return true;      /* WinHTTP validates against the Windows certificate store */
#else
    return false;
#endif
}

/* ============================== desktop helpers ============================== */
bool osa_open_in_explorer(const char *path) {
#ifdef _WIN32
    HINSTANCE h = ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
    return (INT_PTR)h > 32;
#else
    (void)path;
    return false;
#endif
}

/* A single asynchronous playback slot. The shell may start a second clip while
 * the first is still running (pressing "listen" twice), and PlaySound with
 * SND_ASYNC stops the previous sound for us. */
static bool g_playing;

bool osa_audio_play_file(const char *wav_path) {
#ifdef _WIN32
    if (!wav_path || !*wav_path) return false;
    /* PlaySound needs a system code page path, so convert from UTF-8. */
    int wl = MultiByteToWideChar(CP_UTF8, 0, wav_path, -1, NULL, 0);
    if (wl <= 0) return false;
    wchar_t *w = (wchar_t *)malloc((size_t)wl * sizeof(wchar_t));
    if (!w) return false;
    MultiByteToWideChar(CP_UTF8, 0, wav_path, -1, w, wl);
    BOOL ok = PlaySoundW(w, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    free(w);
    g_playing = ok ? true : false;
    return ok ? true : false;
#else
    /* The headless build has no output device; the UI says so rather than
     * pretending a clip was played. */
    (void)wav_path;
    return false;
#endif
}

bool osa_audio_is_playing(void) { return g_playing; }
void osa_audio_stop(void) {
#ifdef _WIN32
    PlaySoundW(NULL, NULL, 0);
#endif
    g_playing = false;
}

bool osa_clipboard_set(const char *utf8) {
#ifdef _WIN32
    if (!OpenClipboard(NULL)) return false;
    EmptyClipboard();
    size_t n = strlen(utf8);
    /* convert to UTF-16 */
    int wl = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)n, NULL, 0);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(wl + 1) * sizeof(wchar_t));
    if (!h) { CloseClipboard(); return false; }
    wchar_t *w = (wchar_t *)GlobalLock(h);
    MultiByteToWideChar(CP_UTF8, 0, utf8, (int)n, w, wl);
    w[wl] = 0;
    GlobalUnlock(h);
    bool ok = SetClipboardData(CF_UNICODETEXT, h) != NULL;
    CloseClipboard();
    return ok;
#else
    (void)utf8;
    return false;
#endif
}

char *osa_clipboard_get(osa_arena *a) {
#ifdef _WIN32
    if (!OpenClipboard(NULL)) return NULL;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    char *out = NULL;
    if (h) {
        wchar_t *w = (wchar_t *)GlobalLock(h);
        if (w) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
            char *tmp = (char *)malloc((size_t)n + 1);
            if (tmp) {
                WideCharToMultiByte(CP_UTF8, 0, w, -1, tmp, n, NULL, NULL);
                out = osa_arena_strdup(a, tmp);
                free(tmp);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
#else
    (void)a;
    return NULL;
#endif
}
