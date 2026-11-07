/* portable platform layer: filesystem, threads, metrics (POSIX implementation;
 * the Windows build uses platform/win32/{win_platform.c,win_net.c}). */
#include "osai_platform.h"

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
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

void osa_sys_stats_sample(osa_sys_stats *out) {
    memset(out, 0, sizeof *out);
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
