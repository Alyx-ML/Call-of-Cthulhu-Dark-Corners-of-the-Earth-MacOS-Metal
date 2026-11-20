// kernel32 for 32-bit guests: handles, synchronization, threads, files (with overlapped and ReadFileEx/APC I/O),
// memory, time, INI files.
#include "rt32.h"
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <fnmatch.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <mach/mach_time.h>

#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NO_MORE_FILES 18
#define ERROR_HANDLE_EOF 38
#define ERROR_ALREADY_EXISTS 183
#define ERROR_IO_PENDING 997
#define WAIT_TIMEOUT 0x102
#define WAIT_FAILED 0xFFFFFFFFu
#define INVALID_HANDLE 0xFFFFFFFFu

// ---------------- handles ----------------
enum { H_FREE, H_FILE, H_EVENT, H_SEM, H_MUTEX, H_THREAD, H_FIND };
typedef struct {
    int type;
    int fd;
    int manual, signaled;              // event
    int count, max;                    // semaphore
    uint32_t owner, recursion;         // mutex (owner tid)
    RtThread *thread; int done; uint32_t exit_code;
    DIR *dir; char pattern[256], dirpath[1024];
    int overlapped;
    char name[128];
} Handle;
#define NHANDLES 8192
static Handle handles[NHANDLES];
static pthread_mutex_t h_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t h_cv = PTHREAD_COND_INITIALIZER;   // broadcast whenever any waitable object changes
#define HVAL(i) (0x100u + 4u * (uint32_t)(i))
static Handle *hget(uint32_t h) {
    if (h < 0x100 || (h & 3)) return NULL;
    uint32_t i = (h - 0x100) / 4;
    return i < NHANDLES && handles[i].type != H_FREE ? &handles[i] : NULL;
}
static uint32_t hnew(int type) {
    pthread_mutex_lock(&h_mu);
    for (int i = 1; i < NHANDLES; i++) if (handles[i].type == H_FREE) {
        memset(&handles[i], 0, sizeof handles[i]);
        handles[i].type = type; handles[i].fd = -1;
        pthread_mutex_unlock(&h_mu);
        return HVAL(i);
    }
    pthread_mutex_unlock(&h_mu);
    rt_fatal("out of handles");
}
static Handle *hnamed(int type, const char *name) {
    if (!name || !*name) return NULL;
    for (int i = 1; i < NHANDLES; i++) if (handles[i].type == type && !strcmp(handles[i].name, name)) return &handles[i];
    return NULL;
}

// ---------------- waiting ----------------
static int signaled(Handle *o, uint32_t tid) {
    switch (o->type) {
    case H_EVENT: return o->signaled;
    case H_SEM: return o->count > 0;
    case H_MUTEX: return !o->owner || o->owner == tid;
    case H_THREAD: return o->done;
    default: return 1;
    }
}
static void acquire(Handle *o, uint32_t tid) {
    if (o->type == H_EVENT && !o->manual) o->signaled = 0;
    else if (o->type == H_SEM) o->count--;
    else if (o->type == H_MUTEX) { o->owner = tid; o->recursion++; }
}
static void deadline(struct timespec *ts, uint32_t ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000; ts->tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) { ts->tv_sec++; ts->tv_nsec -= 1000000000; }
}
void rt_run_apcs(void);
static int apc_pending(void);
// `input` (MsgWaitForMultipleObjects): also return WAIT_OBJECT_0 + n when it reports queued messages; the message
// queue wakes waiters through rt_wait_kick
static uint32_t wait_many_input(int n, const uint32_t *hs, int all, uint32_t ms, int alertable, int (*input)(void)) {
    uint32_t tid = rt_thread() ? rt_thread()->tid : 0;
    struct timespec ts; if (ms != 0xFFFFFFFFu) deadline(&ts, ms);
    pthread_mutex_lock(&h_mu);
    for (;;) {
        Handle *o[64];
        for (int i = 0; i < n; i++) { o[i] = hget(hs[i]); if (!o[i]) { pthread_mutex_unlock(&h_mu); LAST_ERROR = ERROR_INVALID_HANDLE; return WAIT_FAILED; } }
        if (all) {
            int ok = 1; for (int i = 0; i < n; i++) ok &= signaled(o[i], tid);
            if (ok) { for (int i = 0; i < n; i++) acquire(o[i], tid); pthread_mutex_unlock(&h_mu); return 0; }
        } else {
            for (int i = 0; i < n; i++) if (signaled(o[i], tid)) { acquire(o[i], tid); pthread_mutex_unlock(&h_mu); return (uint32_t)i; }
        }
        if (input && input()) { pthread_mutex_unlock(&h_mu); return (uint32_t)n; }
        if (alertable && apc_pending()) { pthread_mutex_unlock(&h_mu); rt_run_apcs(); return 0xC0; }   // WAIT_IO_COMPLETION
        if (ms == 0) { pthread_mutex_unlock(&h_mu); return WAIT_TIMEOUT; }
        if (ms == 0xFFFFFFFFu) pthread_cond_wait(&h_cv, &h_mu);
        else if (pthread_cond_timedwait(&h_cv, &h_mu, &ts) == ETIMEDOUT) { pthread_mutex_unlock(&h_mu); return WAIT_TIMEOUT; }
    }
}
static uint32_t wait_many(int n, const uint32_t *hs, int all, uint32_t ms, int alertable) {
    return wait_many_input(n, hs, all, ms, alertable, NULL);
}
static void kick(void) { pthread_cond_broadcast(&h_cv); }
uint32_t rt_wait_handles_or_input(int n, const uint32_t *hs, int all, uint32_t ms, int (*input)(void)) {
    if (n > 64) { LAST_ERROR = 87; return WAIT_FAILED; }
    return wait_many_input(n, hs, all, ms, 0, input);
}
void rt_wait_kick(void) { pthread_mutex_lock(&h_mu); kick(); pthread_mutex_unlock(&h_mu); }

SHIM(kernel32_WaitForSingleObject, 2) { uint32_t h = ARG(0); return wait_many(1, &h, 0, ARG(1), 0); }
SHIM(kernel32_WaitForMultipleObjects, 4) {
    uint32_t n = ARG(0), hs[64];
    if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++) hs[i] = rd32(ARG(1) + 4 * i);
    return wait_many((int)n, hs, ARG(2) != 0, ARG(3), 0);
}
SHIM(kernel32_CreateEventA, 4) {
    const char *name = STR(3);
    pthread_mutex_lock(&h_mu); Handle *ex = hnamed(H_EVENT, name); pthread_mutex_unlock(&h_mu);
    if (ex) { LAST_ERROR = ERROR_ALREADY_EXISTS; return HVAL(ex - handles); }
    uint32_t h = hnew(H_EVENT); Handle *o = hget(h);
    o->manual = ARG(1) != 0; o->signaled = ARG(2) != 0;
    if (name) snprintf(o->name, sizeof o->name, "%s", name);
    LAST_ERROR = 0;
    return h;
}
SHIM(kernel32_SetEvent, 1) { pthread_mutex_lock(&h_mu); Handle *o = hget(ARG(0)); if (o) o->signaled = 1; kick(); pthread_mutex_unlock(&h_mu); return o != NULL; }
SHIM(kernel32_ResetEvent, 1) { pthread_mutex_lock(&h_mu); Handle *o = hget(ARG(0)); if (o) o->signaled = 0; pthread_mutex_unlock(&h_mu); return o != NULL; }
SHIM(kernel32_CreateSemaphoreA, 4) {
    uint32_t h = hnew(H_SEM); Handle *o = hget(h);
    o->count = (int)ARG(1); o->max = (int)ARG(2);
    return h;
}
SHIM(kernel32_ReleaseSemaphore, 3) {
    pthread_mutex_lock(&h_mu);
    Handle *o = hget(ARG(0)); int ok = 0;
    if (o && o->type == H_SEM) {
        if (ARG(2)) wr32(ARG(2), (uint32_t)o->count);
        if (o->count + (int)ARG(1) <= o->max) { o->count += (int)ARG(1); ok = 1; kick(); }
    }
    pthread_mutex_unlock(&h_mu);
    return ok;
}
SHIM(kernel32_CreateMutexA, 3) {
    const char *name = STR(2);
    pthread_mutex_lock(&h_mu); Handle *ex = hnamed(H_MUTEX, name); pthread_mutex_unlock(&h_mu);
    if (ex) { LAST_ERROR = ERROR_ALREADY_EXISTS; return HVAL(ex - handles); }
    uint32_t h = hnew(H_MUTEX); Handle *o = hget(h);
    if (ARG(1)) { o->owner = rt_thread()->tid; o->recursion = 1; }
    if (name) snprintf(o->name, sizeof o->name, "%s", name);
    LAST_ERROR = 0;
    return h;
}
SHIM(kernel32_ReleaseMutex, 1) {
    pthread_mutex_lock(&h_mu);
    Handle *o = hget(ARG(0)); int ok = 0;
    if (o && o->type == H_MUTEX && o->owner == rt_thread()->tid) { if (!--o->recursion) o->owner = 0; ok = 1; kick(); }
    pthread_mutex_unlock(&h_mu);
    return ok;
}
SHIM(kernel32_CloseHandle, 1) {
    pthread_mutex_lock(&h_mu);
    Handle *o = hget(ARG(0));
    if (o) {
        if (o->fd >= 0) close(o->fd);
        if (o->dir) closedir(o->dir);
        if (o->type != H_THREAD || o->done) o->type = H_FREE;   // a running thread keeps its slot until it exits
    }
    pthread_mutex_unlock(&h_mu);
    return o != NULL;
}

// ---------------- critical sections ----------------
// The guest CRITICAL_SECTION (24 bytes) holds only what Windows keeps there -- DebugInfo, LockCount, RecursionCount,
// OwningThread, LockSemaphore (0), SpinCount -- because games read or overlap those fields (RE HD's objects embed one
// whose LockSemaphore overlaps a list link that must stay 0). The host mutex is found in a table keyed by the
// guest address: lock-free lookups, inserts under a lock, entries never removed (a re-initialized section at the
// same address reuses its mutex).
#define CSN (1u << 17)
static struct { uint32_t key; pthread_mutex_t *m; } cs_tab[CSN];
static pthread_mutex_t cs_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *cs_get(uint32_t cs) {
    uint32_t i = ((cs >> 2) * 2654435761u) & (CSN - 1);
    for (;;) {
        uint32_t k = __atomic_load_n(&cs_tab[i].key, __ATOMIC_ACQUIRE);
        if (k == cs) return cs_tab[i].m;
        if (k == 0) {
            pthread_mutex_lock(&cs_mu);
            if (__atomic_load_n(&cs_tab[i].key, __ATOMIC_ACQUIRE) == 0) {
                pthread_mutex_t *m = malloc(sizeof *m);
                pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
                pthread_mutex_init(m, &a);
                cs_tab[i].m = m;
                __atomic_store_n(&cs_tab[i].key, cs, __ATOMIC_RELEASE);
                pthread_mutex_unlock(&cs_mu);
                return m;
            }
            pthread_mutex_unlock(&cs_mu);
            continue;   // someone filled this slot: look at it again
        }
        i = (i + 1) & (CSN - 1);
    }
}
static void cs_init_fields(uint32_t cs) {
    wr32(cs + 0x00, 0xFFFFFFFFu);   // DebugInfo: -1, "no debug info" (Windows 8+)
    wr32(cs + 0x04, 0xFFFFFFFFu);   // LockCount
    wr32(cs + 0x08, 0); wr32(cs + 0x0C, 0); wr32(cs + 0x10, 0); wr32(cs + 0x14, 0);
}
SHIM(kernel32_InitializeCriticalSection, 1) { cs_init_fields(ARG(0)); cs_get(ARG(0)); return 0; }
SHIM(kernel32_DeleteCriticalSection, 1) { return 0; }
SHIM(kernel32_EnterCriticalSection, 1) {
    uint32_t cs = ARG(0); pthread_mutex_lock(cs_get(cs));
    wr32(cs + 0x0C, rt_thread()->tid); wr32(cs + 0x08, rd32(cs + 0x08) + 1);
    return 0;
}
SHIM(kernel32_TryEnterCriticalSection, 1) {
    uint32_t cs = ARG(0);
    if (pthread_mutex_trylock(cs_get(cs))) return 0;
    wr32(cs + 0x0C, rt_thread()->tid); wr32(cs + 0x08, rd32(cs + 0x08) + 1);
    return 1;
}
SHIM(kernel32_LeaveCriticalSection, 1) {
    uint32_t cs = ARG(0);
    uint32_t r = rd32(cs + 0x08) - 1; wr32(cs + 0x08, r);
    if (!r) wr32(cs + 0x0C, 0);
    pthread_mutex_unlock(cs_get(cs));
    return 0;
}
SHIM(kernel32_InitializeSListHead, 1) { wr64(ARG(0), 0); return 0; }

// ---------------- threads ----------------
RtThread *rt_thread_new(uint32_t stack_size);
void rt_thread_attach(RtThread *t);
typedef struct { RtThread *t; uint32_t start, param, h; int suspended; pthread_mutex_t mu; pthread_cond_t cv; } ThreadStart;
static void *thread_main(void *p) {
    ThreadStart *s = p;
    rt_thread_attach(s->t);
    pthread_mutex_lock(&s->mu);
    while (s->suspended) pthread_cond_wait(&s->cv, &s->mu);
    pthread_mutex_unlock(&s->mu);
    uint32_t r = guest_call(s->t->cpu, s->start, 1, &s->param);
    pthread_mutex_lock(&h_mu);
    Handle *o = hget(s->h);
    if (o) { o->done = 1; o->exit_code = r; }
    kick();
    pthread_mutex_unlock(&h_mu);
    return NULL;
}
static ThreadStart *thread_starts[NHANDLES];
SHIM(kernel32_CreateThread, 6) {
    uint32_t stack = ARG(1), start = ARG(2), param = ARG(3), flags = ARG(4), tidp = ARG(5);
    RtThread *t = rt_thread_new(stack ? stack : 0x100000);
    uint32_t h = hnew(H_THREAD);
    Handle *o = hget(h); o->thread = t; t->handle = h;
    ThreadStart *s = calloc(1, sizeof *s);
    s->t = t; s->start = start; s->param = param; s->h = h; s->suspended = (flags & 4) != 0;
    pthread_mutex_init(&s->mu, NULL); pthread_cond_init(&s->cv, NULL);
    thread_starts[(h - 0x100) / 4] = s;
    if (tidp) wr32(tidp, t->tid);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);   // host stack: translated frames are big
    pthread_create(&t->pt, &a, thread_main, s);
    if (rt_verbose) rt_log("[k32] CreateThread start=0x%x param=0x%x tid=%u%s", start, param, t->tid, s->suspended ? " (suspended)" : "");
    return h;
}
SHIM(kernel32_ResumeThread, 1) {
    uint32_t h = ARG(0);
    Handle *o = hget(h);
    if (!o || o->type != H_THREAD) return 0xFFFFFFFFu;
    ThreadStart *s = thread_starts[(h - 0x100) / 4];
    uint32_t prev = 0;
    if (s) { pthread_mutex_lock(&s->mu); prev = s->suspended; s->suspended = 0; pthread_cond_signal(&s->cv); pthread_mutex_unlock(&s->mu); }
    return prev;
}
SHIM(kernel32_GetExitCodeThread, 2) {
    Handle *o = hget(ARG(0));
    if (!o || o->type != H_THREAD) return 0;
    wr32(ARG(1), o->done ? o->exit_code : 259);   // STILL_ACTIVE
    return 1;
}
SHIM(kernel32_SetThreadPriority, 2) { return 1; }
SHIM(kernel32_SetThreadIdealProcessor, 2) { return 0; }
SHIM(kernel32_GetCurrentThreadId, 0) { return rt_thread()->tid; }
SHIM(kernel32_GetCurrentProcessId, 0) { return 0x10; }
SHIM(kernel32_GetCurrentProcess, 0) { return 0xFFFFFFFFu; }
SHIM(kernel32_Sleep, 1) {
    uint32_t ms = ARG(0);
    if (!ms) sched_yield(); else usleep(ms * 1000u);
    return 0;
}

// APCs (ReadFileEx completion routines): run on the issuing thread when it waits alertably
typedef struct Apc { struct Apc *next; uint32_t fn, a0, a1, a2; } Apc;
static _Thread_local Apc *apc_head, *apc_tail;
static int apc_pending(void) { return apc_head != NULL; }
static void apc_queue(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    Apc *p = malloc(sizeof *p); *p = (Apc){NULL, fn, a0, a1, a2};
    if (apc_tail) apc_tail->next = p; else apc_head = p;
    apc_tail = p;
}
void rt_run_apcs(void) {
    while (apc_head) {
        Apc *p = apc_head; apc_head = p->next; if (!apc_head) apc_tail = NULL;
        uint32_t args[3] = {p->a0, p->a1, p->a2};
        guest_call(rt_cpu(), p->fn, 3, args);
        free(p);
    }
}
SHIM(kernel32_SleepEx, 2) {
    uint32_t ms = ARG(0);
    if (ARG(1) && apc_pending()) { rt_run_apcs(); return 0xC0; }
    if (!ms) sched_yield(); else usleep(ms * 1000u);
    if (ARG(1) && apc_pending()) { rt_run_apcs(); return 0xC0; }
    return 0;
}

// ---------------- files ----------------
static uint32_t file_error(void) {
    switch (errno) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: return ERROR_ACCESS_DENIED;
    case EEXIST: return 80;   // ERROR_FILE_EXISTS
    default: return 31;       // ERROR_GEN_FAILURE
    }
}
static int trace_files = -1;
static uint32_t create_file(const char *win, uint32_t access, uint32_t disp, uint32_t flags) {
    if (trace_files < 0) trace_files = getenv("RT_TRACE_FILES") != NULL;
    if (!win) { LAST_ERROR = ERROR_PATH_NOT_FOUND; return INVALID_HANDLE; }
    char p[1024]; rt_host_path(win, p, sizeof p);
    int o = (access & 0x40000000u) ? ((access & 0x80000000u) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
    case 1: o |= O_CREAT | O_EXCL; break;    // CREATE_NEW
    case 2: o |= O_CREAT | O_TRUNC; break;   // CREATE_ALWAYS
    case 4: o |= O_CREAT; break;             // OPEN_ALWAYS
    case 5: o |= O_TRUNC; break;             // TRUNCATE_EXISTING
    default: break;                          // OPEN_EXISTING
    }
    int fd = open(p, o, 0644);
    if (fd < 0) { if (trace_files) rt_log("[file] CreateFileA %s -> %s", p, strerror(errno)); LAST_ERROR = file_error(); return INVALID_HANDLE; }
    struct stat st;
    if (fstat(fd, &st) != 0) { LAST_ERROR = file_error(); close(fd); return INVALID_HANDLE; }
    if (S_ISDIR(st.st_mode) && !(flags & 0x02000000u)) {   // a folder opens only with FILE_FLAG_BACKUP_SEMANTICS
        close(fd);
        if (trace_files) rt_log("[file] CreateFileA %s -> a folder (access denied)", p);
        LAST_ERROR = ERROR_ACCESS_DENIED; return INVALID_HANDLE;   // as Windows answers
    }
    uint32_t h = hnew(H_FILE); Handle *hd = hget(h);
    hd->fd = fd; hd->overlapped = (flags & 0x40000000u) != 0;
    if (trace_files) rt_log("[file] CreateFileA %s -> 0x%x%s", p, h, hd->overlapped ? " (overlapped)" : "");
    LAST_ERROR = (disp == 2 || disp == 4) ? 0 : 0;
    return h;
}
SHIM(kernel32_CreateFileA, 7) { return create_file(STR(0), ARG(1), ARG(4), ARG(5)); }
SHIM(kernel32_CreateFileW, 7) {
    if (!ARG(0)) return create_file(NULL, ARG(1), ARG(4), ARG(5));
    char n[1024]; w_to_utf8(WSTR(0), n, sizeof n);
    return create_file(n, ARG(1), ARG(4), ARG(5));
}
// OVERLAPPED (20 bytes): Internal(status), InternalHigh(bytes), Offset, OffsetHigh, hEvent
static int64_t ov_offset(uint32_t ov) { return (int64_t)(((uint64_t)rd32(ov + 12) << 32) | rd32(ov + 8)); }
static void ov_done(uint32_t ov, uint32_t status, uint32_t bytes) {
    wr32(ov + 0, status); wr32(ov + 4, bytes);
    uint32_t ev = rd32(ov + 16);
    if (ev) { pthread_mutex_lock(&h_mu); Handle *e = hget(ev); if (e) e->signaled = 1; kick(); pthread_mutex_unlock(&h_mu); }
}
SHIM(kernel32_ReadFile, 5) {
    Handle *f = hget(ARG(0));
    uint32_t buf = ARG(1), n = ARG(2), outp = ARG(3), ov = ARG(4);
    if (!f || f->type != H_FILE) { LAST_ERROR = ERROR_INVALID_HANDLE; return 0; }
    WATCH_BULK(buf, n, "ReadFile");
    if (trace_files > 0) rt_log("[file] ReadFile h=0x%x n=%u ov=%s off=%lld event=0x%x (from 0x%x)", ARG(0), n, ov ? "yes" : "no",
                                ov ? (long long)ov_offset(ov) : -1ll, ov ? rd32(ov + 16) : 0, rd32(c->esp));
    ssize_t r = ov ? pread(f->fd, G2H(buf), n, ov_offset(ov)) : read(f->fd, G2H(buf), n);
    if (r < 0) { LAST_ERROR = file_error(); return 0; }
    if (outp) wr32(outp, (uint32_t)r);
    if (ov) {
        ov_done(ov, r == 0 && n ? 0xC0000011u : 0, (uint32_t)r);   // STATUS_END_OF_FILE
        if (r == 0 && n) { LAST_ERROR = ERROR_HANDLE_EOF; return 0; }
    }
    return 1;
}
SHIM(kernel32_ReadFileEx, 5) {
    Handle *f = hget(ARG(0));
    uint32_t buf = ARG(1), n = ARG(2), ov = ARG(3), done = ARG(4);
    if (!f || f->type != H_FILE) { LAST_ERROR = ERROR_INVALID_HANDLE; return 0; }
    WATCH_BULK(buf, n, "ReadFileEx");
    if (trace_files > 0) rt_log("[file] ReadFileEx h=0x%x n=%u off=%lld (from 0x%x)", ARG(0), n, (long long)ov_offset(ov), rd32(c->esp));
    ssize_t r = pread(f->fd, G2H(buf), n, ov_offset(ov));
    uint32_t err = r < 0 ? file_error() : (r == 0 && n ? ERROR_HANDLE_EOF : 0);
    wr32(ov + 0, err ? 0xC0000011u : 0); wr32(ov + 4, r > 0 ? (uint32_t)r : 0);
    apc_queue(done, err, r > 0 ? (uint32_t)r : 0, ov);
    LAST_ERROR = 0;
    return 1;
}
SHIM(kernel32_GetOverlappedResult, 4) {
    if (trace_files > 0) rt_log("[file] GetOverlappedResult h=0x%x wait=%u -> %u bytes", ARG(0), ARG(3), rd32(ARG(1) + 4));
    uint32_t ov = ARG(1);
    if (ARG(2)) wr32(ARG(2), rd32(ov + 4));
    if (rd32(ov) == 0xC0000011u) { LAST_ERROR = ERROR_HANDLE_EOF; return 0; }
    return 1;
}
SHIM(kernel32_WriteFile, 5) {
    Handle *f = hget(ARG(0));
    uint32_t buf = ARG(1), n = ARG(2), outp = ARG(3), ov = ARG(4);
    int fd = f && f->type == H_FILE ? f->fd : (ARG(0) == 0xFFFFFFF5u || ARG(0) == 0xFFFFFFF4u ? 2 : -1);
    if (fd < 0) { LAST_ERROR = ERROR_INVALID_HANDLE; return 0; }
    ssize_t r = ov ? pwrite(fd, G2H(buf), n, ov_offset(ov)) : write(fd, G2H(buf), n);
    if (r < 0) { LAST_ERROR = file_error(); return 0; }
    if (outp) wr32(outp, (uint32_t)r);
    if (ov) ov_done(ov, 0, (uint32_t)r);
    return 1;
}
SHIM(kernel32_SetFilePointer, 4) {
    Handle *f = hget(ARG(0));
    if (!f || f->type != H_FILE) { LAST_ERROR = ERROR_INVALID_HANDLE; return 0xFFFFFFFFu; }
    int64_t off = (int32_t)ARG(1);
    if (ARG(2)) off = (int64_t)(((uint64_t)rd32(ARG(2)) << 32) | ARG(1));
    off_t r = lseek(f->fd, off, ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) { LAST_ERROR = 131; return 0xFFFFFFFFu; }
    if (ARG(2)) wr32(ARG(2), (uint32_t)((uint64_t)r >> 32));
    LAST_ERROR = 0;
    return (uint32_t)r;
}
SHIM(kernel32_SetEndOfFile, 1) {
    Handle *f = hget(ARG(0));
    if (!f || f->type != H_FILE) return 0;
    return ftruncate(f->fd, lseek(f->fd, 0, SEEK_CUR)) == 0;
}
SHIM(kernel32_GetFileSize, 2) {
    Handle *f = hget(ARG(0)); struct stat st;
    if (!f || f->type != H_FILE || fstat(f->fd, &st)) { LAST_ERROR = ERROR_INVALID_HANDLE; return 0xFFFFFFFFu; }
    if (ARG(1)) wr32(ARG(1), (uint32_t)((uint64_t)st.st_size >> 32));
    return (uint32_t)st.st_size;
}
static uint32_t attrs_of(struct stat *st) { return S_ISDIR(st->st_mode) ? 0x10 : 0x80; }   // DIRECTORY / NORMAL
SHIM(kernel32_GetFileAttributesA, 1) {
    char p[1024]; struct stat st;
    if (stat(rt_host_path(STR(0), p, sizeof p), &st)) { LAST_ERROR = file_error(); return 0xFFFFFFFFu; }
    return attrs_of(&st);
}
SHIM(kernel32_DeleteFileA, 1) { char p[1024]; if (unlink(rt_host_path(STR(0), p, sizeof p))) { LAST_ERROR = file_error(); return 0; } return 1; }
SHIM(kernel32_CreateDirectoryA, 2) {
    char p[1024];
    if (mkdir(rt_host_path(STR(0), p, sizeof p), 0755)) { LAST_ERROR = errno == EEXIST ? ERROR_ALREADY_EXISTS : file_error(); return 0; }
    return 1;
}
SHIM(kernel32_RemoveDirectoryA, 1) { char p[1024]; if (rmdir(rt_host_path(STR(0), p, sizeof p))) { LAST_ERROR = file_error(); return 0; } return 1; }
SHIM(kernel32_MoveFileA, 2) {
    char a[1024], b[1024];
    if (rename(rt_host_path(STR(0), a, sizeof a), rt_host_path(STR(1), b, sizeof b))) { LAST_ERROR = file_error(); return 0; }
    return 1;
}
SHIM(kernel32_CopyFileA, 3) {
    char a[1024], b[1024];
    rt_host_path(STR(0), a, sizeof a); rt_host_path(STR(1), b, sizeof b);
    if (ARG(2) && access(b, F_OK) == 0) { LAST_ERROR = 80; return 0; }
    int in = open(a, O_RDONLY); if (in < 0) { LAST_ERROR = file_error(); return 0; }
    int out = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (out < 0) { close(in); LAST_ERROR = file_error(); return 0; }
    char buf[65536]; ssize_t r;
    while ((r = read(in, buf, sizeof buf)) > 0) write(out, buf, (size_t)r);
    close(in); close(out);
    return 1;
}
SHIM(kernel32_GetDiskFreeSpaceA, 5) {
    struct statfs s; if (statfs(rt_data_dir[0] ? rt_data_dir : ".", &s)) return 0;
    uint64_t freeb = (uint64_t)s.f_bavail * s.f_bsize, tot = (uint64_t)s.f_blocks * s.f_bsize;
    uint32_t spc = 8, bps = 512, cl = spc * bps;
    if (ARG(1)) wr32(ARG(1), spc);
    if (ARG(2)) wr32(ARG(2), bps);
    if (ARG(3)) wr32(ARG(3), (uint32_t)(freeb / cl > 0xFFFFFFFFull ? 0xFFFFFFFFu : freeb / cl));
    if (ARG(4)) wr32(ARG(4), (uint32_t)(tot / cl > 0xFFFFFFFFull ? 0xFFFFFFFFu : tot / cl));
    return 1;
}
// WIN32_FIND_DATAA: attrs, 3 FILETIMEs, sizeHigh, sizeLow, 2 reserved, cFileName[260], cAlternateFileName[14]
static void unix_to_filetime(time_t t, uint32_t ft) { uint64_t v = ((uint64_t)t + 11644473600ull) * 10000000ull; wr32(ft, (uint32_t)v); wr32(ft + 4, (uint32_t)(v >> 32)); }
static int find_next(Handle *f, uint32_t fd) {
    struct dirent *e;
    while ((e = readdir(f->dir))) {
        if (fnmatch(f->pattern, e->d_name, FNM_CASEFOLD)) continue;
        char full[1400]; struct stat st;
        snprintf(full, sizeof full, "%s/%s", f->dirpath, e->d_name);
        if (stat(full, &st)) continue;
        memset(G2H(fd), 0, 320);
        wr32(fd, attrs_of(&st));
        unix_to_filetime(st.st_birthtime, fd + 4); unix_to_filetime(st.st_atime, fd + 12); unix_to_filetime(st.st_mtime, fd + 20);
        wr32(fd + 28, (uint32_t)((uint64_t)st.st_size >> 32)); wr32(fd + 32, (uint32_t)st.st_size);
        snprintf((char *)G2H(fd + 44), 260, "%s", e->d_name);
        return 1;
    }
    return 0;
}
SHIM(kernel32_FindFirstFileA, 2) {
    char p[1024]; rt_host_path(STR(0), p, sizeof p);
    char *slash = strrchr(p, '/');
    uint32_t h = hnew(H_FIND); Handle *f = hget(h);
    if (slash) { *slash = 0; snprintf(f->dirpath, sizeof f->dirpath, "%s", *p ? p : "/"); snprintf(f->pattern, sizeof f->pattern, "%s", slash + 1); }
    else { snprintf(f->dirpath, sizeof f->dirpath, "."); snprintf(f->pattern, sizeof f->pattern, "%s", p); }
    if (!strcmp(f->pattern, "*.*")) strcpy(f->pattern, "*");
    f->dir = opendir(f->dirpath);
    if (!f->dir || !find_next(f, ARG(1))) { f->type = H_FREE; if (f->dir) closedir(f->dir); f->dir = NULL; LAST_ERROR = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE; }
    return h;
}
SHIM(kernel32_FindNextFileA, 2) {
    Handle *f = hget(ARG(0));
    if (!f || f->type != H_FIND) return 0;
    if (!find_next(f, ARG(1))) { LAST_ERROR = ERROR_NO_MORE_FILES; return 0; }
    return 1;
}
SHIM(kernel32_FindClose, 1) {
    Handle *f = hget(ARG(0));
    if (!f || f->type != H_FIND) return 0;
    if (f->dir) closedir(f->dir);
    f->dir = NULL; f->type = H_FREE;
    return 1;
}
static char cwd_win[260];
SHIM(kernel32_GetCurrentDirectoryA, 2) {
    if (!cwd_win[0]) snprintf(cwd_win, sizeof cwd_win, "%s", rt_game.win_dir);
    uint32_t n = (uint32_t)strlen(cwd_win);
    if (ARG(0) <= n) return n + 1;
    memcpy(G2H(ARG(1)), cwd_win, n + 1);
    return n;
}
SHIM(kernel32_SetCurrentDirectoryA, 1) { snprintf(cwd_win, sizeof cwd_win, "%s", STR(0)); return 1; }

// ---------------- INI files ----------------
static int ini_find(FILE *f, const char *sec, const char *key, char *out, size_t n) {
    char line[1024]; int in = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        s[strcspn(s, "\r\n")] = 0;
        if (*s == '[') { char *e = strchr(s, ']'); if (e) *e = 0; in = !strcasecmp(s + 1, sec); continue; }
        if (!in) continue;
        char *eq = strchr(s, '='); if (!eq) continue;
        *eq = 0; char *k = s; char *ke = eq; while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        if (!strcasecmp(k, key)) { char *v = eq + 1; while (*v == ' ') v++; snprintf(out, n, "%s", v); return 1; }
    }
    return 0;
}
SHIM(kernel32_GetPrivateProfileStringA, 6) {
    const char *sec = STR(0), *key = STR(1), *def = STR(2);
    uint32_t out = ARG(3), n = ARG(4);
    char p[1024], v[1024] = "";
    rt_host_path(STR(5), p, sizeof p);
    FILE *f = fopen(p, "r");
    int found = f && sec && key && ini_find(f, sec, key, v, sizeof v);
    if (f) fclose(f);
    if (!found) snprintf(v, sizeof v, "%s", def ? def : "");
    if (!n) return 0;
    size_t len = strlen(v); if (len > n - 1) len = n - 1;
    memcpy(G2H(out), v, len); wr8(out + (uint32_t)len, 0);
    return (uint32_t)len;
}
SHIM(kernel32_WritePrivateProfileStringA, 4) {
    const char *sec = STR(0), *key = STR(1), *val = STR(2);
    char p[1024]; rt_host_path(STR(3), p, sizeof p);
    FILE *f = fopen(p, "r");
    char *buf = NULL; size_t len = 0;
    if (f) { fseek(f, 0, SEEK_END); len = (size_t)ftell(f); fseek(f, 0, SEEK_SET); buf = malloc(len + 1); len = fread(buf, 1, len, f); buf[len] = 0; fclose(f); }
    FILE *o = fopen(p, "w"); if (!o) { free(buf); return 0; }
    int in = 0, done = 0, seen_sec = 0;
    char *save = NULL;
    for (char *line = buf ? strtok_r(buf, "\n", &save) : NULL; line; line = strtok_r(NULL, "\n", &save)) {
        char *s = line; s[strcspn(s, "\r")] = 0;
        if (*s == '[') {
            if (in && !done && key && val) { fprintf(o, "%s=%s\r\n", key, val); done = 1; }
            char name[256]; snprintf(name, sizeof name, "%s", s + 1); char *e = strchr(name, ']'); if (e) *e = 0;
            in = !strcasecmp(name, sec); if (in) seen_sec = 1;
            if (in && !key) continue;
        } else if (in && key) {
            char *eq = strchr(s, '=');
            if (eq) { char k[256]; snprintf(k, sizeof k, "%.*s", (int)(eq - s), s); if (!strcasecmp(k, key)) { if (val && !done) fprintf(o, "%s=%s\r\n", key, val); done = 1; continue; } }
        } else if (in && !key) continue;
        fprintf(o, "%s\r\n", s);
    }
    if (!done && key && val) { if (!seen_sec || !in) fprintf(o, "[%s]\r\n", sec); fprintf(o, "%s=%s\r\n", key, val); }
    fclose(o); free(buf);
    return 1;
}

// ---------------- memory ----------------
SHIM(kernel32_VirtualAlloc, 4) {
    uint32_t a = gvm_alloc(ARG(0), ARG(1), (ARG(2) & 0x1000) != 0, ARG(3));
    if (!a) LAST_ERROR = 8;   // ERROR_NOT_ENOUGH_MEMORY
    return a;
}
SHIM(kernel32_VirtualFree, 3) {
    if (ARG(2) & 0x8000) return gvm_free(ARG(0));   // MEM_RELEASE
    return 1;                                      // MEM_DECOMMIT: keep the pages
}
SHIM(kernel32_HeapAlloc, 3) {
    uint32_t p = (ARG(1) & 8) ? gcalloc(1, ARG(2)) : gmalloc(ARG(2));   // HEAP_ZERO_MEMORY
    return p;
}
SHIM(kernel32_HeapFree, 3) { gfree(ARG(2)); return 1; }
SHIM(kernel32_GetProcessHeap, 0) { return rt_process_heap; }
SHIM(kernel32_GlobalAlloc, 2) { return (ARG(0) & 0x40) ? gcalloc(1, ARG(1)) : gmalloc(ARG(1)); }
SHIM(kernel32_GlobalFree, 1) { gfree(ARG(0)); return 0; }
SHIM(kernel32_GlobalLock, 1) { return ARG(0); }
SHIM(kernel32_GlobalUnlock, 1) { return 1; }

// ---------------- time ----------------
SHIM(kernel32_QueryPerformanceFrequency, 1) { wr64(ARG(0), 10000000ull); return 1; }
SHIM(kernel32_QueryPerformanceCounter, 1) {
    wr64(ARG(0), rt_clock_ns() / 100);
    return 1;
}
SHIM(kernel32_GetSystemTimeAsFileTime, 1) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t v = ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100;
    wr64(ARG(0), v);
    return 0;
}
SHIM(kernel32_FileTimeToLocalFileTime, 2) {
    uint64_t v = rd64(ARG(0)); time_t t = (time_t)(v / 10000000ull - 11644473600ull);
    struct tm lt; localtime_r(&t, &lt);
    wr64(ARG(1), v + (uint64_t)((int64_t)lt.tm_gmtoff * 10000000ll));
    return 1;
}
SHIM(kernel32_FileTimeToSystemTime, 2) {
    uint64_t v = rd64(ARG(0)); time_t t = (time_t)(v / 10000000ull - 11644473600ull);
    struct tm g; gmtime_r(&t, &g);
    uint32_t st = ARG(1);
    wr16(st, (uint16_t)(g.tm_year + 1900)); wr16(st + 2, (uint16_t)(g.tm_mon + 1)); wr16(st + 4, (uint16_t)g.tm_wday);
    wr16(st + 6, (uint16_t)g.tm_mday); wr16(st + 8, (uint16_t)g.tm_hour); wr16(st + 10, (uint16_t)g.tm_min);
    wr16(st + 12, (uint16_t)g.tm_sec); wr16(st + 14, (uint16_t)((v / 10000) % 1000));
    return 1;
}

// ---------------- process / system ----------------
static uint32_t cmdline_a;
extern char rt_cmdline[1024];
SHIM(kernel32_GetCommandLineA, 0) { if (!cmdline_a) cmdline_a = gstrdup(rt_cmdline); return cmdline_a; }
static uint32_t known_module(const char *name);   // a DLL the runtime provides: its module handle, or 0
SHIM(kernel32_GetModuleHandleA, 1) {
    const char *n = STR(0);
    if (!n || !strcasecmp(n, rt_game.exe)) return gimage_base;
    uint32_t h = known_module(n);   // the runtime's DLLs are always loaded, as a process's imports are on Windows
    if (h) return h;
    LAST_ERROR = 126; return 0;
}
SHIM(kernel32_GetModuleHandleW, 1) {
    char n[260]; if (!ARG(0)) return gimage_base;
    w_to_utf8(WSTR(0), n, sizeof n);
    if (!strcasecmp(n, rt_game.exe)) return gimage_base;
    uint32_t h = known_module(n);
    if (h) return h;
    LAST_ERROR = 126; return 0;
}
SHIM(kernel32_K32GetModuleFileNameExA, 4) {
    char p[512]; snprintf(p, sizeof p, "%s\\%s", rt_game.win_dir, rt_game.exe);
    uint32_t n = ARG(3); size_t len = strlen(p); if (len >= n) len = n - 1;
    memcpy(G2H(ARG(2)), p, len); wr8(ARG(2) + (uint32_t)len, 0);
    return (uint32_t)len;
}
SHIM(kernel32_GetStartupInfoW, 1) { memset(G2H(ARG(0)), 0, 68); wr32(ARG(0), 68); return 0; }
SHIM(kernel32_GetSystemInfo, 1) {
    uint32_t s = ARG(0); memset(G2H(s), 0, 36);
    wr16(s, 0);                // PROCESSOR_ARCHITECTURE_INTEL
    wr32(s + 4, 0x1000);       // page size
    wr32(s + 8, 0x10000); wr32(s + 12, 0x7FFEFFFF);
    wr32(s + 16, 0xFF);        // active processor mask
    wr32(s + 20, 8);           // processors
    wr32(s + 24, 586);
    wr32(s + 28, 0x10000);     // allocation granularity
    wr16(s + 32, 6); wr16(s + 34, 0x2A07);
    return 0;
}
SHIM(kernel32_IsProcessorFeaturePresent, 1) {
    switch (ARG(0)) {
    case 1: case 2: case 3: case 6: case 7: case 8: case 10: case 13: case 23: return 1;   // FP, MMX, XMMI, XMMI64, SSE3, fastfail...
    default: return 0;
    }
}
SHIM(kernel32_SetUnhandledExceptionFilter, 1) { return 0; }
SHIM(kernel32_DebugBreak, 0) { rt_log("[k32] DebugBreak from guest"); return 0; }
SHIM(kernel32_GetLastError, 0) { return LAST_ERROR; }
SHIM(kernel32_GetUserDefaultLangID, 0) { return 0x0409; }
// (OSVERSIONINFOEXA *, DWORD typeMask, DWORDLONG conditionMask): the 64-bit mask takes two stack slots
SHIM(kernel32_VerifyVersionInfoA, 4) { return 1; }   // "at least" checks: we are a modern Windows
RAWSHIM(kernel32_VerSetConditionMask) {   // (ULONGLONG mask, DWORD type, BYTE cond) -> ULONGLONG, stdcall 16 bytes
    uint64_t m = ARG64(0); uint32_t type = ARG(2); uint8_t cond = (uint8_t)ARG(3);
    for (int k = 0; k < 8; k++) if (type & (1u << k)) m |= (uint64_t)(cond & 7) << (3 * k);
    c->eax = (uint32_t)m; c->edx = (uint32_t)(m >> 32); c->esp += 16;
}
void rt_cocoa_release_pointer(void);   // cocoa32.m: _exit skips the atexit handler that gives the pointer back
SHIM(kernel32_ExitProcess, 1) { rt_log("[k32] ExitProcess(%u)", ARG(0)); fflush(NULL); rt_cocoa_release_pointer(); _exit((int)ARG(0)); }
SHIM(kernel32_CreateProcessA, 10) { LAST_ERROR = 2; return 0; }
SHIM(kernel32_MultiByteToWideChar, 6) {
    uint32_t src = ARG(2), dst = ARG(4); int32_t sl = (int32_t)ARG(3), dl = (int32_t)ARG(5);
    const char *s = gp(src); if (!s) return 0;
    size_t n = sl < 0 ? strlen(s) + 1 : (size_t)sl;
    uint16_t *tmp = malloc((n + 1) * 2 + 8);
    // CP_ACP/1252 and UTF-8: decode UTF-8 when it's valid, else Latin-1
    char *z = malloc(n + 1); memcpy(z, s, n); z[n] = 0;
    size_t k = 0;
    if (ARG(0) == 65001) { utf8_to_w(z, tmp, n + 2); k = w_len(tmp); if (sl < 0 || (n && !s[n - 1])) k++; }
    else { for (size_t i = 0; i < n; i++) tmp[k++] = (uint8_t)s[i]; }
    free(z);
    if (!dl) { free(tmp); return (uint32_t)k; }
    if ((size_t)dl < k) { free(tmp); LAST_ERROR = 122; return 0; }
    memcpy(G2H(dst), tmp, k * 2);
    free(tmp);
    return (uint32_t)k;
}

// ---------------- more kernel32 (32-bit MSVC 7.1 static-CRT games: Call of Cthulhu DCoTE) ----------------
SHIM(kernel32_WaitForSingleObjectEx, 3) { uint32_t h = ARG(0); return wait_many(1, &h, 0, ARG(1), ARG(2) != 0); }
SHIM(kernel32_GetThreadPriority, 1) { return 0; }   // THREAD_PRIORITY_NORMAL: SetThreadPriority is not applied
SHIM(kernel32_GetCurrentThread, 0) { return 0xFFFFFFFEu; }   // pseudo-handle
SHIM(kernel32_SetLastError, 1) { LAST_ERROR = ARG(0); return 0; }
SHIM(kernel32_GetTickCount, 0) {
    return (uint32_t)(rt_clock_ns() / 1000000ull);
}
SHIM(kernel32_MulDiv, 3) {
    int64_t a = (int32_t)ARG(0), b = (int32_t)ARG(1), d = (int32_t)ARG(2);
    if (!d) return 0xFFFFFFFFu;
    int64_t p = a * b;
    // round half away from zero, as Windows does
    int64_t q = ((p < 0) != (d < 0)) ? (p - d / 2) / d : (p + d / 2) / d;
    if (q > INT32_MAX || q < INT32_MIN) return 0xFFFFFFFFu;
    return (uint32_t)(int32_t)q;
}
SHIM(kernel32_InterlockedIncrement, 1) { return __atomic_add_fetch((uint32_t *)G2H(ARG(0)), 1u, __ATOMIC_SEQ_CST); }
SHIM(kernel32_InterlockedDecrement, 1) { return __atomic_sub_fetch((uint32_t *)G2H(ARG(0)), 1u, __ATOMIC_SEQ_CST); }
SHIM(kernel32_InterlockedExchange, 2) { return __atomic_exchange_n((uint32_t *)G2H(ARG(0)), ARG(1), __ATOMIC_SEQ_CST); }

SHIM(kernel32_LocalAlloc, 2) { return (ARG(0) & 0x40) ? gcalloc(1, ARG(1)) : gmalloc(ARG(1)); }   // LMEM_ZEROINIT
SHIM(kernel32_LocalFree, 1) { gfree(ARG(0)); return 0; }
SHIM(kernel32_HeapCreate, 3) { return rt_process_heap; }   // one guest heap serves every heap handle
SHIM(kernel32_HeapDestroy, 1) { return 1; }
SHIM(kernel32_HeapReAlloc, 4) {
    uint32_t fl = ARG(1), p = ARG(2), n = ARG(3);
    if (fl & 0x10) {   // HEAP_REALLOC_IN_PLACE_ONLY: succeed only when the block is already big enough
        return gmsize(p) >= n ? p : 0;
    }
    uint32_t old = gmsize(p), r = grealloc(p, n);
    if (r && (fl & 8) && n > old) memset(G2H(r + old), 0, n - old);   // HEAP_ZERO_MEMORY
    return r;
}
SHIM(kernel32_HeapSize, 3) { return ARG(2) ? gmsize(ARG(2)) : 0xFFFFFFFFu; }
SHIM(kernel32_VirtualProtect, 4) {
    uint32_t base, size, state, prot = 4;
    gvm_query(ARG(0), &base, &size, &state, &prot);
    if (ARG(3)) wr32(ARG(3), prot);
    return (uint32_t)gvm_protect(ARG(0), ARG(1), ARG(2));
}
SHIM(kernel32_VirtualQuery, 3) {
    uint32_t a = ARG(0), mbi = ARG(1), base, size, state, prot;
    if (ARG(2) < 28) { LAST_ERROR = 24; return 0; }   // ERROR_BAD_LENGTH
    if (a >= gimage_base && a < gimage_base + gimage_size) {
        base = gimage_base; size = gimage_size; state = 0x1000; prot = 0x40;
        wr32(mbi + 24, 0x1000000);   // MEM_IMAGE
    } else {
        gvm_query(a, &base, &size, &state, &prot);
        wr32(mbi + 24, state == 0x1000 ? 0x20000 : 0);   // MEM_PRIVATE
    }
    wr32(mbi + 0, a & ~0xFFFu); wr32(mbi + 4, state == 0x10000 ? 0 : base); wr32(mbi + 8, state == 0x10000 ? 0 : prot);
    wr32(mbi + 12, base + size - (a & ~0xFFFu)); wr32(mbi + 16, state); wr32(mbi + 20, state == 0x10000 ? 1 : prot);
    return 28;
}
// IsBad*Ptr: whether [p, p+n) is committed guest memory (the image, or a committed VirtualAlloc region)
static int range_ok(uint32_t p, uint32_t n) {
    if (!p) return 0;
    if (!n) return 1;
    uint64_t end = (uint64_t)p + n;
    if (end > 0x100000000ull) return 0;
    if (p >= gimage_base && end <= (uint64_t)gimage_base + gimage_size) return 1;
    for (uint64_t a = p; a < end;) {
        uint32_t base, size, state, prot;
        gvm_query((uint32_t)a, &base, &size, &state, &prot);
        if (state != 0x1000 || prot == 1) return 0;
        a = (uint64_t)base + size;
    }
    return 1;
}
SHIM(kernel32_IsBadReadPtr, 2) { return !range_ok(ARG(0), ARG(1)); }
SHIM(kernel32_IsBadWritePtr, 2) { return !range_ok(ARG(0), ARG(1)); }
SHIM(kernel32_IsBadCodePtr, 1) {
    uint32_t a = ARG(0);
    if (a >= 0xFF000000u) return 0;   // host trampolines (rt32.c HOSTFN_BASE) are callable
    for (int i = 0; i < ngsections; i++)
        if ((gsections[i].flags & 0x20000000) && a >= gsections[i].va && a < gsections[i].va + gsections[i].vsize) return 0;
    return 1;
}
SHIM(kernel32_GlobalMemoryStatus, 1) {
    // MEMORYSTATUS as a 32-bit process on a 2 GB Windows sees it (the values clamp to the 2 GB user space)
    uint32_t m = ARG(0);
    wr32(m + 0, 32); wr32(m + 4, 25);
    wr32(m + 8, 0x7FFF0000u); wr32(m + 12, 0x60000000u);    // physical: total, available
    wr32(m + 16, 0x7FFF0000u); wr32(m + 20, 0x60000000u);   // page file
    wr32(m + 24, 0x7FFE0000u); wr32(m + 28, 0x70000000u);   // virtual
    return 0;
}

// ---- strings ----
SHIM(kernel32_lstrlenA, 1) { return ARG(0) ? (uint32_t)strlen(STR(0)) : 0; }
SHIM(kernel32_lstrcpyA, 2) { if (!ARG(0) || !ARG(1)) return 0; memmove(G2H(ARG(0)), STR(1), strlen(STR(1)) + 1); return ARG(0); }
SHIM(kernel32_lstrcatA, 2) {
    if (!ARG(0) || !ARG(1)) return 0;
    char *d = G2H(ARG(0)); size_t n = strlen(d);
    memmove(d + n, STR(1), strlen(STR(1)) + 1);
    return ARG(0);
}
// code pages: CP_ACP is Windows-1252 (Latin-1 for the printable range), 65001 UTF-8
SHIM(kernel32_GetACP, 0) { return 1252; }
SHIM(kernel32_GetOEMCP, 0) { return 437; }
SHIM(kernel32_GetCPInfo, 2) {
    uint32_t cp = ARG(0), o = ARG(1);
    if (cp != 0 && cp != 1 && cp != 1252 && cp != 437 && cp != 3 && cp != 65001) { LAST_ERROR = 87; return 0; }
    memset(G2H(o), 0, 20);
    wr32(o, cp == 65001 ? 4 : 1);   // MaxCharSize
    wr8(o + 4, '?');                // DefaultChar
    return 1;
}
SHIM(kernel32_WideCharToMultiByte, 8) {
    uint32_t cp = ARG(0), dst = ARG(4), useddef = ARG(7);
    int32_t sl = (int32_t)ARG(3), dl = (int32_t)ARG(5);
    const uint16_t *s = gp(ARG(2)); if (!s) { LAST_ERROR = 87; return 0; }
    size_t n = sl < 0 ? w_len(s) + 1 : (size_t)sl;
    char defc = ARG(6) ? *(char *)G2H(ARG(6)) : '?';
    char *tmp = malloc(n * 4 + 4); size_t k = 0; int used = 0;
    if (cp == 65001) {
        uint16_t *z = malloc((n + 1) * 2); memcpy(z, s, n * 2); z[n] = 0;
        // w_to_utf8 stops at a NUL: convert segment by segment so embedded NULs survive
        size_t i = 0;
        while (i < n) {
            if (!z[i]) { tmp[k++] = 0; i++; continue; }
            size_t j = i; while (j < n && z[j]) j++;
            uint16_t save = z[j < n ? j : n]; z[j < n ? j : n] = 0;
            w_to_utf8(z + i, tmp + k, (j - i) * 4 + 4); k += strlen(tmp + k);
            z[j < n ? j : n] = save; i = j;
        }
        free(z);
    } else {
        for (size_t i = 0; i < n; i++) { uint16_t ch = s[i]; if (ch < 256) tmp[k++] = (char)ch; else { tmp[k++] = defc; used = 1; } }
    }
    if (useddef) wr32(useddef, (uint32_t)used);
    if (!dl) { free(tmp); return (uint32_t)k; }
    if ((size_t)dl < k) { memcpy(G2H(dst), tmp, (size_t)dl); free(tmp); LAST_ERROR = 122; return 0; }   // ERROR_INSUFFICIENT_BUFFER
    memcpy(G2H(dst), tmp, k);
    free(tmp);
    return (uint32_t)k;
}
static int ch_lower(int ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7) ? ch + 32 : ch; }
static int ch_upper(int ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 0xE0 && ch <= 0xFE && ch != 0xF7) ? ch - 32 : ch; }
// LCMapString: LCMAP_LOWERCASE 0x100, LCMAP_UPPERCASE 0x200, LCMAP_SORTKEY 0x400 (a byte-wise key: the CRT's
// strcoll/strxfrm compare these with memcmp)
static uint32_t lcmap(uint32_t fl, const uint32_t *src, size_t n, uint32_t *out) {
    size_t k = 0;
    if (fl & 0x400) {
        for (size_t i = 0; i < n; i++) out[k++] = (uint32_t)ch_upper((int)src[i]) & 0xFF;
        out[k++] = 0;
        return (uint32_t)k;
    }
    for (size_t i = 0; i < n; i++) out[k++] = (fl & 0x100) ? (uint32_t)ch_lower((int)src[i]) : (fl & 0x200) ? (uint32_t)ch_upper((int)src[i]) : src[i];
    return (uint32_t)k;
}
SHIM(kernel32_LCMapStringA, 6) {
    uint32_t fl = ARG(1); int32_t sl = (int32_t)ARG(3), dl = (int32_t)ARG(5);
    const uint8_t *s = gp(ARG(2)); if (!s) { LAST_ERROR = 87; return 0; }
    size_t n = sl < 0 ? strlen((const char *)s) + 1 : (size_t)sl;
    uint32_t *a = malloc((n + 1) * 4), *b = malloc((n + 2) * 4);
    for (size_t i = 0; i < n; i++) a[i] = s[i];
    uint32_t k = lcmap(fl, a, n, b);
    uint32_t r = k;
    if (dl) {
        if ((uint32_t)dl < k) { LAST_ERROR = 122; r = 0; }
        else for (uint32_t i = 0; i < k; i++) wr8(ARG(4) + i, (uint8_t)b[i]);
    }
    free(a); free(b);
    return r;
}
SHIM(kernel32_LCMapStringW, 6) {
    uint32_t fl = ARG(1); int32_t sl = (int32_t)ARG(3), dl = (int32_t)ARG(5);
    const uint16_t *s = gp(ARG(2)); if (!s) { LAST_ERROR = 87; return 0; }
    size_t n = sl < 0 ? w_len(s) + 1 : (size_t)sl;
    uint32_t *a = malloc((n + 1) * 4), *b = malloc((n + 2) * 4);
    for (size_t i = 0; i < n; i++) a[i] = s[i];
    uint32_t k = lcmap(fl, a, n, b);
    uint32_t r = k;
    if (dl) {
        // a sort key is a byte string even from the W function
        if (fl & 0x400) { if ((uint32_t)dl < k) { LAST_ERROR = 122; r = 0; } else for (uint32_t i = 0; i < k; i++) wr8(ARG(4) + i, (uint8_t)b[i]); }
        else if ((uint32_t)dl < k) { LAST_ERROR = 122; r = 0; }
        else for (uint32_t i = 0; i < k; i++) wr16(ARG(4) + 2 * i, (uint16_t)b[i]);
    }
    free(a); free(b);
    return r;
}
// GetStringType CT_CTYPE1 flags for Windows-1252
static uint16_t ctype1(uint32_t ch) {
    uint16_t f = 0;
    if (ch < 0x20 || ch == 0x7F) f |= 0x20;                                          // C1_CNTRL
    if (ch == ' ' || ch == '\t' || ch == 0xA0) f |= 0x40;                            // C1_BLANK
    if (ch == ' ' || (ch >= 9 && ch <= 13) || ch == 0xA0) f |= 0x08;                 // C1_SPACE
    if (ch >= '0' && ch <= '9') f |= 0x04 | 0x80;                                    // C1_DIGIT | C1_XDIGIT
    if ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) f |= 0x80;
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7)) f |= 0x01 | 0x100;   // C1_UPPER | C1_ALPHA
    if ((ch >= 'a' && ch <= 'z') || (ch >= 0xDF && ch <= 0xFF && ch != 0xF7)) f |= 0x02 | 0x100;   // C1_LOWER | C1_ALPHA
    if ((ch > 0x20 && ch < 0x7F && !(f & 0x104)) || (ch >= 0xA1 && ch <= 0xBF) || ch == 0xD7 || ch == 0xF7) f |= 0x10;   // C1_PUNCT
    return f;
}
SHIM(kernel32_GetStringTypeA, 5) {
    if (ARG(1) != 1) { LAST_ERROR = 1004; return 0; }   // only CT_CTYPE1 (ERROR_INVALID_FLAGS)
    const uint8_t *s = gp(ARG(2)); int32_t n = (int32_t)ARG(3);
    if (!s) { LAST_ERROR = 87; return 0; }
    if (n < 0) n = (int32_t)strlen((const char *)s) + 1;
    for (int32_t i = 0; i < n; i++) wr16(ARG(4) + 2u * (uint32_t)i, ctype1(s[i]));
    return 1;
}
SHIM(kernel32_GetStringTypeW, 4) {
    if (ARG(0) != 1) { LAST_ERROR = 1004; return 0; }
    const uint16_t *s = gp(ARG(1)); int32_t n = (int32_t)ARG(2);
    if (!s) { LAST_ERROR = 87; return 0; }
    if (n < 0) n = (int32_t)w_len(s) + 1;
    for (int32_t i = 0; i < n; i++) wr16(ARG(3) + 2u * (uint32_t)i, s[i] < 256 ? ctype1(s[i]) : 0x100);
    return 1;
}

// ---- locale (en-US, the GOG English release) ----
static const char *locale_value(uint32_t type, char *tmp) {
    switch (type & 0x0FFFFFFF) {   // strip LOCALE_NOUSEROVERRIDE / LOCALE_RETURN_NUMBER
    case 0x01: return "0409";             // LOCALE_ILANGUAGE
    case 0x02: return "English (United States)";
    case 0x03: return "ENU";              // LOCALE_SABBREVLANGNAME
    case 0x04: return "English";          // LOCALE_SNATIVELANGNAME
    case 0x05: return "1";                // LOCALE_ICOUNTRY
    case 0x06: return "United States";
    case 0x07: return "USA";              // LOCALE_SABBREVCTRYNAME
    case 0x0B: return "437";              // LOCALE_IDEFAULTCODEPAGE
    case 0x0C: return ",";                // LOCALE_SLIST
    case 0x0E: return ".";                // LOCALE_SDECIMAL
    case 0x0F: return ",";                // LOCALE_STHOUSAND
    case 0x10: return "3;0";              // LOCALE_SGROUPING
    case 0x11: return "2";                // LOCALE_IDIGITS
    case 0x12: return "1";                // LOCALE_ILZERO
    case 0x14: return "$";                // LOCALE_SCURRENCY
    case 0x15: return "$";
    case 0x16: return ".";
    case 0x17: return ",";
    case 0x18: return "3;0";
    case 0x19: return "2";
    case 0x1B: return "0";                // LOCALE_ICURRENCY
    case 0x1C: return "0";                // LOCALE_INEGCURR
    case 0x1D: return "/";                // LOCALE_SDATE
    case 0x1E: return ":";                // LOCALE_STIME
    case 0x1F: return "M/d/yyyy";         // LOCALE_SSHORTDATE
    case 0x20: return "dddd, MMMM dd, yyyy";   // LOCALE_SLONGDATE
    case 0x21: return "0";                // LOCALE_IDATE (MDY)
    case 0x22: return "0";
    case 0x23: return "0";                // LOCALE_ITIME (12 h)
    case 0x25: return "0";                // LOCALE_ITLZERO
    case 0x28: return "AM";               // LOCALE_S1159
    case 0x29: return "PM";               // LOCALE_S2359
    case 0x1001: return "0409";           // LOCALE_IDEFAULTLANGUAGE ... (0x1001..) handled below
    case 0x1002: return "1";              // LOCALE_IDEFAULTCOUNTRY? (LOCALE_ITIMEMARKPOSN family)
    case 0x1003: return "h:mm:ss tt";     // LOCALE_STIMEFORMAT
    case 0x1004: return "1252";           // LOCALE_IDEFAULTANSICODEPAGE
    case 0x1009: return "0";              // LOCALE_IFIRSTDAYOFWEEK (Monday=0... en-US: 6 = Sunday)
    case 0x100C: return "0";              // LOCALE_IFIRSTWEEKOFYEAR
    case 0x1014: return "1";              // LOCALE_IPOSSEPBYSPACE..
    case 0x0059: return "en";             // LOCALE_SISO639LANGNAME
    case 0x005A: return "US";             // LOCALE_SISO3166CTRYNAME
    default: break;
    }
    uint32_t t = type & 0x0FFFFFFF;
    static const char *days[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    static const char *sdays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    static const char *months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    static const char *smonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (t >= 0x2A && t <= 0x30) return days[t - 0x2A];       // LOCALE_SDAYNAME1..7
    if (t >= 0x31 && t <= 0x37) return sdays[t - 0x31];      // LOCALE_SABBREVDAYNAME1..7
    if (t >= 0x38 && t <= 0x43) return months[t - 0x38];     // LOCALE_SMONTHNAME1..12
    if (t >= 0x44 && t <= 0x4F) return smonths[t - 0x44];    // LOCALE_SABBREVMONTHNAME1..12
    (void)tmp;
    return NULL;
}
SHIM(kernel32_GetLocaleInfoA, 4) {
    char tmp[64]; const char *v = locale_value(ARG(1), tmp);
    if (!v) { LAST_ERROR = 1004; rt_log("[k32] GetLocaleInfoA: no value for LCTYPE 0x%x", ARG(1)); return 0; }
    uint32_t n = (uint32_t)strlen(v) + 1;
    if (!ARG(3)) return n;
    if (ARG(3) < n) { LAST_ERROR = 122; return 0; }
    memcpy(G2H(ARG(2)), v, n);
    return n;
}
// SYSTEMTIME: wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds
static void systime_now(uint32_t st, int local) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    time_t t = ts.tv_sec; struct tm g;
    if (local) localtime_r(&t, &g); else gmtime_r(&t, &g);
    wr16(st, (uint16_t)(g.tm_year + 1900)); wr16(st + 2, (uint16_t)(g.tm_mon + 1)); wr16(st + 4, (uint16_t)g.tm_wday);
    wr16(st + 6, (uint16_t)g.tm_mday); wr16(st + 8, (uint16_t)g.tm_hour); wr16(st + 10, (uint16_t)g.tm_min);
    wr16(st + 12, (uint16_t)g.tm_sec); wr16(st + 14, (uint16_t)(ts.tv_nsec / 1000000));
}
SHIM(kernel32_GetLocalTime, 1) { systime_now(ARG(0), 1); return 0; }
// date/time pictures (d dd ddd dddd M MM MMM MMMM y yy yyyy gg; h hh H HH m mm s ss t tt; 'quoted')
static size_t fmt_picture(const char *f, const uint16_t *st, char *out, size_t cap) {
    static const char *days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    size_t k = 0;
#define PUT(...) do { int w_ = snprintf(out + k, k < cap ? cap - k : 0, __VA_ARGS__); if (w_ > 0) k += (size_t)w_; } while (0)
    while (*f) {
        char ch = *f; int run = 1;
        if (ch == '\'') { f++; while (*f && *f != '\'') { PUT("%c", *f); f++; } if (*f) f++; continue; }
        while (f[run] == ch) run++;
        int mon = st[1] >= 1 && st[1] <= 12 ? st[1] - 1 : 0, dow = st[2] < 7 ? st[2] : 0;
        switch (ch) {
        case 'd': if (run == 1) PUT("%u", st[3]); else if (run == 2) PUT("%02u", st[3]); else if (run == 3) PUT("%.3s", days[dow]); else PUT("%s", days[dow]); break;
        case 'M': if (run == 1) PUT("%u", st[1]); else if (run == 2) PUT("%02u", st[1]); else if (run == 3) PUT("%.3s", months[mon]); else PUT("%s", months[mon]); break;
        case 'y': if (run <= 2) PUT(run == 1 ? "%u" : "%02u", st[0] % 100); else PUT("%u", st[0]); break;
        case 'g': PUT("A.D."); break;
        case 'h': { unsigned h = st[4] % 12; if (!h) h = 12; PUT(run == 1 ? "%u" : "%02u", h); break; }
        case 'H': PUT(run == 1 ? "%u" : "%02u", st[4]); break;
        case 'm': PUT(run == 1 ? "%u" : "%02u", st[5]); break;
        case 's': PUT(run == 1 ? "%u" : "%02u", st[6]); break;
        case 't': if (run == 1) PUT("%c", st[4] < 12 ? 'A' : 'P'); else PUT("%s", st[4] < 12 ? "AM" : "PM"); break;
        default: for (int i = 0; i < run; i++) PUT("%c", ch); break;
        }
        f += run;
    }
#undef PUT
    return k;
}
static uint32_t date_time_format(CPU *c, int time) {
    uint32_t fl = ARG(1), stp = ARG(2), fmtp = ARG(3), out = ARG(4); int32_t n = (int32_t)ARG(5);
    uint16_t st[8];
    if (stp) for (int i = 0; i < 8; i++) st[i] = rd16(stp + 2u * (uint32_t)i);
    else { uint32_t tmp = gmalloc(16); systime_now(tmp, 1); for (int i = 0; i < 8; i++) st[i] = rd16(tmp + 2u * (uint32_t)i); gfree(tmp); }
    const char *f = fmtp ? (const char *)G2H(fmtp) : NULL;
    if (!f) {
        if (time) f = (fl & 2) ? "h:mm tt" : "h:mm:ss tt";   // TIME_NOSECONDS
        else f = (fl & 2) ? "dddd, MMMM dd, yyyy" : "M/d/yyyy";   // DATE_LONGDATE
    }
    if (time && (fl & 8)) f = "H:mm:ss";   // TIME_FORCE24HOURFORMAT
    char buf[512];
    size_t k = fmt_picture(f, st, buf, sizeof buf);
    if (k >= sizeof buf) k = sizeof buf - 1;
    buf[k] = 0;
    if (!n) return (uint32_t)k + 1;
    if ((size_t)n < k + 1) { LAST_ERROR = 122; return 0; }
    memcpy(G2H(out), buf, k + 1);
    return (uint32_t)k + 1;
}
SHIM(kernel32_GetDateFormatA, 6) { return date_time_format(c, 0); }
SHIM(kernel32_GetTimeFormatA, 6) { return date_time_format(c, 1); }

// ---- version: Windows XP SP2, the platform this 2005 game shipped for ----
static void version_info(uint32_t v, int wide) {
    uint32_t sz = rd32(v);
    wr32(v + 4, 5); wr32(v + 8, 1); wr32(v + 12, 2600); wr32(v + 16, 2);   // 5.1.2600, VER_PLATFORM_WIN32_NT
    const char *sp = "Service Pack 2";
    if (wide) { uint16_t w[128]; utf8_to_w(sp, w, 128); memset(G2H(v + 20), 0, 256); memcpy(G2H(v + 20), w, (w_len(w) + 1) * 2); }
    else { memset(G2H(v + 20), 0, 128); memcpy(G2H(v + 20), sp, strlen(sp) + 1); }
    uint32_t ex = wide ? 276 : 148;   // OSVERSIONINFOEX tail: wServicePackMajor/Minor, wSuiteMask, wProductType
    if (sz >= ex + 8) { wr16(v + ex, 2); wr16(v + ex + 2, 0); wr16(v + ex + 4, 0x100); wr8(v + ex + 6, 1); wr8(v + ex + 7, 0); }
}
SHIM(kernel32_GetVersionExA, 1) { uint32_t s = rd32(ARG(0)); if (s != 148 && s != 156) { LAST_ERROR = 122; return 0; } version_info(ARG(0), 0); return 1; }
SHIM(kernel32_GetVersionExW, 1) { uint32_t s = rd32(ARG(0)); if (s != 276 && s != 284) { LAST_ERROR = 122; return 0; } version_info(ARG(0), 1); return 1; }
SHIM(kernel32_GetComputerNameA, 2) {
    const char *n = "MAC";
    uint32_t cap = rd32(ARG(1));
    if (cap < strlen(n) + 1) { wr32(ARG(1), (uint32_t)strlen(n) + 1); LAST_ERROR = 111; return 0; }   // ERROR_BUFFER_OVERFLOW
    memcpy(G2H(ARG(0)), n, strlen(n) + 1); wr32(ARG(1), (uint32_t)strlen(n));
    return 1;
}
SHIM(kernel32_DeviceIoControl, 8) { LAST_ERROR = 50; return 0; }   // ERROR_NOT_SUPPORTED: no devices
SHIM(kernel32_OutputDebugStringA, 1) { if (rt_verbose && ARG(0)) rt_log("[debug] %s", STR(0)); return 0; }

// ---- process start-up (MSVC 7.1 CRT) ----
SHIM(kernel32_GetStartupInfoA, 1) { memset(G2H(ARG(0)), 0, 68); wr32(ARG(0), 68); return 0; }
SHIM(kernel32_GetStdHandle, 1) {
    uint32_t n = ARG(0);
    if (n == 0xFFFFFFF6u || n == 0xFFFFFFF5u || n == 0xFFFFFFF4u) return n;   // STD_INPUT/OUTPUT/ERROR_HANDLE (pseudo values)
    LAST_ERROR = ERROR_INVALID_HANDLE; return INVALID_HANDLE;
}
SHIM(kernel32_SetStdHandle, 2) { return 1; }
SHIM(kernel32_SetHandleCount, 1) { return ARG(0); }
SHIM(kernel32_GetFileType, 1) {
    uint32_t h = ARG(0);
    if (h == 0xFFFFFFF6u || h == 0xFFFFFFF5u || h == 0xFFFFFFF4u) return 2;   // FILE_TYPE_CHAR
    Handle *o = hget(h);
    if (o && o->type == H_FILE) return 1;   // FILE_TYPE_DISK
    LAST_ERROR = ERROR_INVALID_HANDLE;
    return 0;                                // FILE_TYPE_UNKNOWN
}
SHIM(kernel32_FlushFileBuffers, 1) {
    Handle *f = hget(ARG(0));
    if (!f || f->type != H_FILE) return ARG(0) == 0xFFFFFFF5u || ARG(0) == 0xFFFFFFF4u;
    return fsync(f->fd) == 0;
}
SHIM(kernel32_GetModuleFileNameA, 3) {
    uint32_t mod = ARG(0), out = ARG(1), n = ARG(2);
    if (mod && mod != gimage_base) { LAST_ERROR = 126; return 0; }
    char p[512]; snprintf(p, sizeof p, "%s\\%s", rt_game.win_dir, rt_game.exe);
    uint32_t len = (uint32_t)strlen(p);
    if (!n) return 0;
    if (len >= n) { memcpy(G2H(out), p, n - 1); wr8(out + n - 1, 0); LAST_ERROR = 122; return n; }
    memcpy(G2H(out), p, len + 1);
    return len;
}
SHIM(kernel32_GetCurrentDirectoryW, 2) {
    if (!cwd_win[0]) snprintf(cwd_win, sizeof cwd_win, "%s", rt_game.win_dir);
    uint16_t w[260]; utf8_to_w(cwd_win, w, 260);
    uint32_t n = (uint32_t)w_len(w);
    if (ARG(0) <= n) return n + 1;
    memcpy(G2H(ARG(1)), w, (n + 1) * 2);
    return n;
}
// environment block: what a Windows process sees, minus anything that names the host
static const char env_block[] = "ALLUSERSPROFILE=C:\\ProgramData\0COMPUTERNAME=MAC\0ComSpec=C:\\Windows\\system32\\cmd.exe\0"
    "NUMBER_OF_PROCESSORS=8\0OS=Windows_NT\0PATH=C:\\Windows\\system32;C:\\Windows\0PROCESSOR_ARCHITECTURE=x86\0"
    "SystemRoot=C:\\Windows\0TEMP=C:\\Windows\\Temp\0TMP=C:\\Windows\\Temp\0USERNAME=Player\0windir=C:\\Windows\0\0";
SHIM(kernel32_GetEnvironmentStrings, 0) {
    uint32_t p = gmalloc(sizeof env_block);
    memcpy(G2H(p), env_block, sizeof env_block);
    return p;
}
SHIM(kernel32_GetEnvironmentStringsW, 0) {
    uint32_t p = gmalloc(sizeof env_block * 2);
    for (size_t i = 0; i < sizeof env_block; i++) wr16(p + 2u * (uint32_t)i, (uint8_t)env_block[i]);
    return p;
}
SHIM(kernel32_FreeEnvironmentStringsA, 1) { gfree(ARG(0)); return 1; }
SHIM(kernel32_FreeEnvironmentStringsW, 1) { gfree(ARG(0)); return 1; }
SHIM(kernel32_TerminateProcess, 2) {
    if (ARG(0) != 0xFFFFFFFFu) { LAST_ERROR = ERROR_ACCESS_DENIED; return 0; }   // only ourselves
    rt_log("[k32] TerminateProcess(%u)", ARG(1)); fflush(NULL); _exit((int)ARG(1));
}

// ---- TLS: TEB.TlsSlots at +0xE10 (64 slots), as Windows lays them out ----
static uint64_t tls_used;
static pthread_mutex_t tls_mu = PTHREAD_MUTEX_INITIALIZER;
SHIM(kernel32_TlsAlloc, 0) {
    pthread_mutex_lock(&tls_mu);
    for (uint32_t i = 0; i < 64; i++) if (!(tls_used & (1ull << i))) {
        tls_used |= 1ull << i;
        pthread_mutex_unlock(&tls_mu);
        // a fresh index reads as NULL on every thread
        wr32(c->fsbase + 0xE10 + 4 * i, 0);
        return i;
    }
    pthread_mutex_unlock(&tls_mu);
    LAST_ERROR = 8;
    return 0xFFFFFFFFu;   // TLS_OUT_OF_INDEXES
}
SHIM(kernel32_TlsFree, 1) {
    uint32_t i = ARG(0);
    if (i >= 64) { LAST_ERROR = 87; return 0; }
    pthread_mutex_lock(&tls_mu); tls_used &= ~(1ull << i); pthread_mutex_unlock(&tls_mu);
    return 1;
}
SHIM(kernel32_TlsSetValue, 2) { if (ARG(0) >= 64) { LAST_ERROR = 87; return 0; } wr32(c->fsbase + 0xE10 + 4 * ARG(0), ARG(1)); return 1; }
SHIM(kernel32_TlsGetValue, 1) { if (ARG(0) >= 64) { LAST_ERROR = 87; return 0; } LAST_ERROR = 0; return rd32(c->fsbase + 0xE10 + 4 * ARG(0)); }

// ---- modules: the DLLs whose exports the runtime implements ----
static const char *const known_dlls[] = {"kernel32", "user32", "gdi32", "advapi32", "shell32", "shlwapi", "ole32", "oleaut32",
    "winmm", "version", "d3d9", "d3dx9_28", "dinput8", "fmodex", "ntdll", NULL};
#define MODULE_BASE 0xFE000000u   // module handles: opaque values above the guest heap, below the host trampolines
static int dll_index(const char *name) {
    const char *b = strrchr(name, '\\'); b = b ? b + 1 : name;
    const char *s = strrchr(b, '/'); b = s ? s + 1 : b;
    size_t n = strlen(b);
    if (n > 4 && !strcasecmp(b + n - 4, ".dll")) n -= 4;
    for (int i = 0; known_dlls[i]; i++) if (strlen(known_dlls[i]) == n && !strncasecmp(known_dlls[i], b, n)) return i;
    return -1;
}
static uint32_t known_module(const char *name) { int i = dll_index(name); return i < 0 ? 0 : MODULE_BASE + 0x10000u * (uint32_t)i; }
SHIM(kernel32_LoadLibraryA, 1) {
    if (!ARG(0)) { LAST_ERROR = 87; return 0; }
    int i = dll_index(STR(0));
    if (i < 0) { rt_log("[k32] LoadLibraryA(\"%s\"): not available", STR(0)); LAST_ERROR = 126; return 0; }   // ERROR_MOD_NOT_FOUND
    return MODULE_BASE + 0x10000u * (uint32_t)i;
}
SHIM(kernel32_FreeLibrary, 1) { return 1; }
SHIM(kernel32_GetProcAddress, 2) {
    uint32_t h = ARG(0), name = ARG(1);
    const char *dll = NULL;
    if (h >= MODULE_BASE && h < MODULE_BASE + 0x10000u * 15 && !(h & 0xFFFF)) dll = known_dlls[(h - MODULE_BASE) >> 16];
    else if (h != gimage_base) { LAST_ERROR = ERROR_INVALID_HANDLE; return 0; }
    if (name < 0x10000) { rt_log("[k32] GetProcAddress(%s, #%u): ordinals are not exported", dll ? dll : rt_game.exe, name); LAST_ERROR = 127; return 0; }
    gfn f = dll ? rt_import_by_name(dll, STR(1)) : NULL;
    if (!f) { rt_log("[k32] GetProcAddress(%s, %s): not implemented", dll ? dll : rt_game.exe, STR(1)); LAST_ERROR = 127; return 0; }   // ERROR_PROC_NOT_FOUND
    return rt_host_fn(f, STR(1));
}

// ---- structured exceptions ----
// The SEH chain (fs:[0]) is walked the way ntdll does: handlers run as guest code, and RtlUnwind calls them with
// the unwinding flags before it resets the chain. Resuming at a continuation address inside a function (a
// __try/__except body or a C++ catch continuation) needs the translator's cooperation; until then such a
// handoff stops the game with the exception's details (rt32.c, bad_jump / call_ind_slow).
#define EXC_UNWINDING 0x2
#define EXC_EXIT_UNWIND 0x4
#define EXC_TARGET_UNWIND 0x20
static void exc_name(uint32_t rec, char *out, size_t n) {
    uint32_t code = rd32(rec);
    if (code == 0xE06D7363u && rd32(rec + 16) >= 3) {   // MSVC C++ exception: ThrowInfo -> CatchableTypeArray -> TypeDescriptor name
        uint32_t ti = rd32(rec + 20 + 8);
        uint32_t cta = ti ? rd32(ti + 12) : 0;
        uint32_t ct = cta && rd32(cta) ? rd32(cta + 4) : 0;
        uint32_t td = ct ? rd32(ct + 4) : 0;
        snprintf(out, n, "C++ exception %s", td ? (const char *)G2H(td + 8) : "(unknown type)");
    } else snprintf(out, n, "exception 0x%08x", code);
}
SHIM(kernel32_RaiseException, 4) {
    uint32_t code = ARG(0), flags = ARG(1), nargs = ARG(2), args = ARG(3);
    if (nargs > 15) nargs = 15;
    uint32_t rec = gcalloc(1, 80), ctx = gcalloc(1, 0x2CC), disp = gcalloc(1, 16);
    wr32(rec + 0, code); wr32(rec + 4, flags & 1); wr32(rec + 8, 0); wr32(rec + 12, rd32(c->esp)); wr32(rec + 16, nargs);
    for (uint32_t i = 0; i < nargs; i++) wr32(rec + 20 + 4 * i, rd32(args + 4 * i));
    // CONTEXT_FULL with the caller's registers
    wr32(ctx + 0, 0x10007);
    wr32(ctx + 0x9C, c->edi); wr32(ctx + 0xA0, c->esi); wr32(ctx + 0xA4, c->ebx); wr32(ctx + 0xA8, c->edx);
    wr32(ctx + 0xAC, c->ecx); wr32(ctx + 0xB0, c->eax); wr32(ctx + 0xB4, c->ebp); wr32(ctx + 0xB8, rd32(c->esp));
    wr32(ctx + 0xC4, c->esp + 4 + 16);
    char what[160]; exc_name(rec, what, sizeof what);
    rt_log("[k32] RaiseException: %s from 0x%x", what, rd32(c->esp));
    for (uint32_t fr = rd32(c->fsbase); fr && fr != 0xFFFFFFFFu; fr = rd32(fr)) {
        uint32_t handler = rd32(fr + 4);
        uint32_t a[4] = {rec, fr, ctx, disp};
        uint32_t r = guest_call(c, handler, 4, a);
        if (r == 0) {   // ExceptionContinueExecution
            if (flags & 1) rt_fatal("%s: a handler continued a non-continuable exception", what);
            gfree(rec); gfree(ctx); gfree(disp);
            return 0;
        }
        if (r != 1) rt_fatal("%s: handler 0x%x returned disposition %u (nested exceptions are not supported)", what, handler, r);
    }
    rt_fatal("unhandled %s (raised from 0x%x)", what, rd32(c->esp));
}
SHIM(kernel32_RtlUnwind, 4) {
    uint32_t target = ARG(0), rec = ARG(2), ret = ARG(3);
    uint32_t own = 0;
    if (!rec) { own = rec = gcalloc(1, 80); wr32(rec, 0xC0000027u); }   // STATUS_UNWIND
    wr32(rec + 4, rd32(rec + 4) | (target ? EXC_UNWINDING : EXC_UNWINDING | EXC_EXIT_UNWIND));
    uint32_t ctx = gcalloc(1, 0x2CC), disp = gcalloc(1, 16);
    uint32_t fr = rd32(c->fsbase);
    while (fr && fr != 0xFFFFFFFFu && fr != target) {
        uint32_t a[4] = {rec, fr, ctx, disp};
        guest_call(c, rd32(fr + 4), 4, a);
        fr = rd32(fr);
        wr32(c->fsbase, fr);   // unlink the frame that was unwound
    }
    gfree(ctx); gfree(disp); if (own) gfree(own);
    c->eax = ret;
    return ret;
}
SHIM(kernel32_UnhandledExceptionFilter, 1) {
    uint32_t ep = ARG(0), rec = ep ? rd32(ep) : 0;
    char what[160] = "exception";
    if (rec) exc_name(rec, what, sizeof what);
    rt_log("[k32] UnhandledExceptionFilter: %s", what);
    return 1;   // EXCEPTION_EXECUTE_HANDLER: the CRT then ends the process
}
// events for other runtime modules (winmm timers): set, or pulse (release current waiters, leave reset)
int rt_event_set(uint32_t h, int pulse) {
    pthread_mutex_lock(&h_mu);
    Handle *o = hget(h);
    int ok = o && o->type == H_EVENT;
    if (ok) { o->signaled = 1; kick(); }
    pthread_mutex_unlock(&h_mu);
    if (ok && pulse) { sched_yield(); pthread_mutex_lock(&h_mu); o = hget(h); if (o && o->type == H_EVENT) o->signaled = 0; pthread_mutex_unlock(&h_mu); }
    return ok;
}
// event handles that runtime modules create for the guest (DirectShow's IMediaEvent::GetEventHandle); 0 when the
// handle table is full
uint32_t rt_event_new(int manual, int initial) {
    uint32_t h = hnew(H_EVENT);
    pthread_mutex_lock(&h_mu);
    Handle *o = hget(h);
    if (o) { o->manual = manual != 0; o->signaled = initial != 0; }
    pthread_mutex_unlock(&h_mu);
    return o ? h : 0;
}
int rt_event_reset(uint32_t h) {
    pthread_mutex_lock(&h_mu);
    Handle *o = hget(h);
    int ok = o && o->type == H_EVENT;
    if (ok) o->signaled = 0;
    pthread_mutex_unlock(&h_mu);
    return ok;
}
void rt_event_close(uint32_t h) {
    pthread_mutex_lock(&h_mu);
    Handle *o = hget(h);
    if (o && o->type == H_EVENT) o->type = H_FREE;
    pthread_mutex_unlock(&h_mu);
}
