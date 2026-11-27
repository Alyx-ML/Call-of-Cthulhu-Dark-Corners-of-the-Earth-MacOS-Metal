// The conversion of the game on the player's Mac. See convert32.h.
#include "convert32.h"
#include "../convert/translate32.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <libgen.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define PATH_LEN 2048
// A compile of one file takes a few seconds; a process that runs this long has stopped working.
#define PROCESS_TIMEOUT_S 600
// The memory of one compile process, for the number of processes. The largest file of Call of Cthulhu (3.3 MB of C)
// needs 355 MB (maximum resident set size).
#define COMPILE_MEMORY (512ull << 20)
#define MAX_PROCESSES 32

// the parts of the work (the progress callback): translation, compilation, link
#define P_TRANSLATE 0.08
#define P_COMPILE 0.97

typedef struct { char **v; size_t n, cap; } Args;

static int args_add(Args *a, const char *s) {
    if (a->n + 1 >= a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 64;
        char **v = realloc(a->v, cap * sizeof *v);
        if (!v) return 0;
        a->v = v; a->cap = cap;
    }
    char *d = strdup(s);
    if (!d) return 0;
    a->v[a->n++] = d;
    a->v[a->n] = NULL;
    return 1;
}
static void args_free(Args *a) {
    for (size_t i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    memset(a, 0, sizeof *a);
}
// the lines of a file (one argument on each line; empty lines are skipped)
static int args_file(Args *a, const char *path, char *why, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(why, n, "%s: %s", path, strerror(errno)); return 0; }
    char line[1024];
    int ok = 1;
    while (ok && fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        if (l && line[l - 1] != '\n' && !feof(f)) { snprintf(why, n, "%s: a line is too long", path); ok = 0; break; }
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] && !args_add(a, line)) { snprintf(why, n, "out of memory"); ok = 0; }
    }
    if (ok && ferror(f)) { snprintf(why, n, "%s: read error", path); ok = 0; }
    fclose(f);
    return ok;
}

static int path_join(char *out, size_t n, const char *a, const char *b) {
    int r = snprintf(out, n, "%s/%s", a, b);
    return r > 0 && (size_t)r < n;
}

static int rm_entry(const char *p, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw;
    return remove(p) != 0 && errno != ENOENT ? -1 : 0;
}
// removes a folder and its files (symbolic links are removed, not followed)
static int rm_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT;
    return nftw(path, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0;
}

// the last part of a log file, for an error message (one line, at most n - 1 characters)
static void log_tail(const char *path, char *out, size_t n) {
    out[0] = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    char buf[4096];
    off_t end = lseek(fd, 0, SEEK_END);
    off_t from = end > (off_t)sizeof buf - 1 ? end - (off_t)sizeof buf + 1 : 0;
    ssize_t k = end >= 0 ? pread(fd, buf, sizeof buf - 1, from) : -1;
    close(fd);
    if (k <= 0) return;
    buf[k] = 0;
    // the first line with "error", else the last line
    char *e = strstr(buf, "error:");
    char *s = e;
    if (s) { while (s > buf && s[-1] != '\n') s--; }
    else {
        size_t l = strlen(buf);
        while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = 0;
        s = strrchr(buf, '\n');
        s = s ? s + 1 : buf;
    }
    size_t l = strcspn(s, "\n");
    if (l >= n) l = n - 1;
    memcpy(out, s, l);
    out[l] = 0;
}

typedef struct {
    Args args;            // the command (args.v[0]: the program)
    char log[PATH_LEN];   // stdout and stderr of the process
    const char *name;     // for the error message
    pid_t pid;
    time_t start;
} Job;

typedef struct {
    const W32Convert *c;
    double lo, hi;   // the progress of the jobs, from lo to hi
} Range;

static void progress(const W32Convert *c, double v) { if (c->progress) c->progress(c->progress_ctx, v); }

static void stop_all(Job *jobs, size_t njobs) {
    for (size_t i = 0; i < njobs; i++)
        if (jobs[i].pid > 0) kill(jobs[i].pid, SIGKILL);
    for (size_t i = 0; i < njobs; i++)
        if (jobs[i].pid > 0) {
            int st;
            while (waitpid(jobs[i].pid, &st, 0) < 0 && errno == EINTR) {}
            jobs[i].pid = 0;
        }
}

static int spawn_job(Job *j, char *why, size_t n) {
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) { snprintf(why, n, "out of memory"); return 0; }
    int ok = posix_spawn_file_actions_addopen(&fa, 1, j->log, O_WRONLY | O_CREAT | O_TRUNC, 0644) == 0 &&
             posix_spawn_file_actions_adddup2(&fa, 1, 2) == 0 &&
             posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0) == 0;
    if (!ok) { posix_spawn_file_actions_destroy(&fa); snprintf(why, n, "out of memory"); return 0; }
    int r = posix_spawn(&j->pid, j->args.v[0], &fa, NULL, j->args.v, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (r != 0) { j->pid = 0; snprintf(why, n, "%s cannot start: %s", j->args.v[0], strerror(r)); return 0; }
    j->start = time(NULL);
    return 1;
}

// the end of the process of a job: 1 when it exited with status 0; else 0 and why
static int job_result(Job *j, int st, char *why, size_t n) {
    j->pid = 0;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return 1;
    char tail[400];
    log_tail(j->log, tail, sizeof tail);
    if (WIFSIGNALED(st)) snprintf(why, n, "%s: %s stopped (signal %d). %s", j->name, j->args.v[1], WTERMSIG(st), tail);
    else snprintf(why, n, "%s: %s failed (status %d). %s", j->name, j->args.v[1], WIFEXITED(st) ? WEXITSTATUS(st) : -1, tail);
    return 0;
}

// Runs the jobs, at most maxpar at the same time. 1 when every job succeeds; else the other processes are stopped.
static int run_jobs(Job *jobs, size_t njobs, size_t maxpar, Range rg, char *why, size_t n) {
    const W32Convert *c = rg.c;
    int kq = kqueue();
    if (kq < 0) { snprintf(why, n, "kqueue: %s", strerror(errno)); return 0; }
    size_t next = 0, running = 0, done = 0;
    int ok = 1;
    while (ok && (next < njobs || running)) {
        while (ok && running < maxpar && next < njobs) {
            Job *j = &jobs[next++];
            if (!spawn_job(j, why, n)) { ok = 0; break; }
            running++;
            struct kevent kev;
            EV_SET(&kev, (uintptr_t)j->pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, j);
            if (kevent(kq, &kev, 1, NULL, 0, NULL) != 0) {
                if (errno != ESRCH) { snprintf(why, n, "kevent: %s", strerror(errno)); ok = 0; break; }
                // the process has already exited
                int st;
                pid_t p;
                while ((p = waitpid(j->pid, &st, 0)) < 0 && errno == EINTR) {}
                if (p < 0) { snprintf(why, n, "waitpid: %s", strerror(errno)); j->pid = 0; ok = 0; break; }
                running--;
                if (!job_result(j, st, why, n)) { ok = 0; break; }
                done++;
                progress(c, rg.lo + (rg.hi - rg.lo) * (double)done / (double)njobs);
            }
        }
        if (!ok || !running) break;
        struct kevent ev;
        struct timespec ts = {0, 250 * 1000 * 1000};   // the wait for the next exit; then the checks below
        int k = kevent(kq, NULL, 0, &ev, 1, &ts);
        if (k < 0 && errno != EINTR) { snprintf(why, n, "kevent: %s", strerror(errno)); ok = 0; break; }
        if (k == 1) {
            Job *j = ev.udata;
            int st;
            pid_t p;
            while ((p = waitpid(j->pid, &st, 0)) < 0 && errno == EINTR) {}
            if (p < 0) { snprintf(why, n, "waitpid: %s", strerror(errno)); j->pid = 0; ok = 0; break; }
            running--;
            if (!job_result(j, st, why, n)) { ok = 0; break; }
            done++;
            progress(c, rg.lo + (rg.hi - rg.lo) * (double)done / (double)njobs);
        }
        if (c->cancel && atomic_load(c->cancel)) { snprintf(why, n, "cancelled"); ok = 0; break; }
        time_t now = time(NULL);
        for (size_t i = 0; i < next && ok; i++)
            if (jobs[i].pid > 0 && now - jobs[i].start > PROCESS_TIMEOUT_S) {
                snprintf(why, n, "%s: %s did not finish in %d minutes", jobs[i].name, jobs[i].args.v[1], PROCESS_TIMEOUT_S / 60);
                ok = 0;
            }
    }
    if (!ok) stop_all(jobs, njobs);
    close(kq);
    return ok;
}

static size_t process_count(void) {
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    uint64_t mem = 0;
    size_t len = sizeof mem;
    size_t n = ncpu > 0 ? (size_t)ncpu : 1;
    if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) == 0 && mem > 0) {
        uint64_t by_mem = mem / 2 / COMPILE_MEMORY;   // half of the memory for the compile processes
        if (by_mem < 1) by_mem = 1;
        if (by_mem < n) n = (size_t)by_mem;
    }
    return n > MAX_PROCESSES ? MAX_PROCESSES : n;
}

static void translate_progress(void *ctx, int done, int total) {
    const W32Convert *c = ctx;
    if (total > 0) progress(c, P_TRANSLATE * (double)done / (double)total);
}

static int is_hex16(const char *s) { return strlen(s) == 16 && strspn(s, "0123456789abcdef") == 16; }

// the folders of other conversions in the Converted folder: a folder of another conversion ID, or a work folder
static void remove_others(const W32Convert *c, const char *converted, const char *id) {
    DIR *d = opendir(converted);
    if (!d) { if (c->log) fprintf(c->log, "convert: %s: %s\n", converted, strerror(errno)); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        char head[17];
        snprintf(head, sizeof head, "%.16s", e->d_name);
        int other = is_hex16(e->d_name) && strcmp(e->d_name, id) != 0;
        int work = is_hex16(head) && strncmp(e->d_name + 16, ".work.", 6) == 0;
        if (!other && !work) continue;
        char p[PATH_LEN];
        if (!path_join(p, sizeof p, converted, e->d_name)) continue;
        if (!rm_tree(p) && c->log) fprintf(c->log, "convert: can't remove %s: %s\n", p, strerror(errno));
    }
    closedir(d);
}

// Copies the compiler of the app to dst (the work folder) with read and write: the copy has the bytes and the
// signature of the app's binary but no extended attributes, so it has no quarantine attribute of a download. The
// player approved the app at its first start; the conversion runs this copy.
static int copy_tool(const char *src, const char *dst, char *why, size_t n) {
    int in = open(src, O_RDONLY);
    if (in < 0) { snprintf(why, n, "%s: %s", src, strerror(errno)); return 0; }
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0755);
    if (out < 0) { snprintf(why, n, "%s: %s", dst, strerror(errno)); close(in); return 0; }
    static char buf[1 << 20];   // one conversion at a time (the launcher and the headless start)
    int ok = 1;
    for (;;) {
        ssize_t k = read(in, buf, sizeof buf);
        if (k < 0) { if (errno == EINTR) continue; snprintf(why, n, "%s: %s", src, strerror(errno)); ok = 0; break; }
        if (k == 0) break;
        for (ssize_t w = 0; w < k;) {
            ssize_t m = write(out, buf + w, (size_t)(k - w));
            if (m < 0) { if (errno == EINTR) continue; snprintf(why, n, "%s: %s", dst, strerror(errno)); ok = 0; break; }
            w += m;
        }
        if (!ok) break;
    }
    close(in);
    if (close(out) != 0 && ok) { snprintf(why, n, "%s: %s", dst, strerror(errno)); ok = 0; }
    return ok;
}

static int code_name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int w32_convert(const W32Convert *c, char *why, size_t n) {
    char outdir[PATH_LEN], converted[PATH_LEN], id[PATH_LEN], work[PATH_LEN], gen[PATH_LEN], obj[PATH_LEN], p[PATH_LEN];
    Args cflags = {0}, ldflags = {0};
    Job *jobs = NULL;
    size_t njobs = 0;
    char **names = NULL;
    size_t nnames = 0;
    int ret = 0;
    work[0] = 0;
    // the folders: <converted>/<id>/game.bundle, the work folder <converted>/<id>.work.XXXXXX
    if (strlen(c->out) >= sizeof outdir) { snprintf(why, n, "the data folder path is too long"); return 0; }
    if (!dirname_r(c->out, outdir) || !dirname_r(outdir, converted) || !basename_r(outdir, id)) {
        snprintf(why, n, "%s: %s", c->out, strerror(errno));
        return 0;
    }
    if (!is_hex16(id)) { snprintf(why, n, "%s is not a conversion folder", outdir); return 0; }
    if (mkdir(converted, 0755) != 0 && errno != EEXIST) { snprintf(why, n, "%s: %s", converted, strerror(errno)); return 0; }
    int r = snprintf(work, sizeof work, "%s/%s.work.XXXXXX", converted, id);
    if (r <= 0 || (size_t)r >= sizeof work) { snprintf(why, n, "the data folder path is too long"); return 0; }
    if (!mkdtemp(work)) { snprintf(why, n, "%s: %s", converted, strerror(errno)); work[0] = 0; return 0; }
    if (!path_join(gen, sizeof gen, work, "gen") || !path_join(obj, sizeof obj, work, "obj")) { snprintf(why, n, "the data folder path is too long"); goto out; }
    if (mkdir(obj, 0755) != 0) { snprintf(why, n, "%s: %s", obj, strerror(errno)); goto out; }

    // 1. translate
    char tool[PATH_LEN];
    if (!path_join(tool, sizeof tool, work, "llvm")) { snprintf(why, n, "the data folder path is too long"); goto out; }
    if (!copy_tool(c->llvm, tool, why, n)) goto out;
    char provided[PATH_LEN], patches[PATH_LEN], native[PATH_LEN];
    if (!path_join(provided, sizeof provided, c->conv_dir, "provided.txt") || !path_join(patches, sizeof patches, c->conv_dir, "patches.txt") ||
        !path_join(native, sizeof native, c->conv_dir, "native.txt")) { snprintf(why, n, "the app path is too long"); goto out; }
    W32Translate t = {0};
    t.exe = c->exe;
    t.outdir = gen;
    t.provided = provided;
    t.patches = access(patches, F_OK) == 0 ? patches : NULL;
    t.native = access(native, F_OK) == 0 ? native : NULL;
    t.log = c->log;
    t.progress = translate_progress;
    t.progress_ctx = (void *)c;
    if (!w32_translate(&t, why, n)) goto out;
    if (c->cancel && atomic_load(c->cancel)) { snprintf(why, n, "cancelled"); goto out; }
    progress(c, P_TRANSLATE);

    // 2. compile: code_*.c (in name order) and tables.c
    if (!path_join(p, sizeof p, c->conv_dir, "cflags") || !args_file(&cflags, p, why, n)) goto out;
    if (!path_join(p, sizeof p, c->conv_dir, "ldflags") || !args_file(&ldflags, p, why, n)) goto out;
    DIR *d = opendir(gen);
    if (!d) { snprintf(why, n, "%s: %s", gen, strerror(errno)); goto out; }
    struct dirent *e;
    size_t cap = 0;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (!(strncmp(e->d_name, "code_", 5) == 0 && l > 7 && strcmp(e->d_name + l - 2, ".c") == 0)) continue;
        if (nnames == cap) {
            cap = cap ? cap * 2 : 64;
            char **nn = realloc(names, cap * sizeof *nn);
            if (!nn) { closedir(d); snprintf(why, n, "out of memory"); goto out; }
            names = nn;
        }
        if (!(names[nnames] = strdup(e->d_name))) { closedir(d); snprintf(why, n, "out of memory"); goto out; }
        nnames++;
    }
    closedir(d);
    if (!nnames) { snprintf(why, n, "the translator wrote no code files"); goto out; }
    qsort(names, nnames, sizeof *names, code_name_cmp);
    {
        char **nn = realloc(names, (nnames + 1) * sizeof *nn);
        if (!nn) { snprintf(why, n, "out of memory"); goto out; }
        names = nn;
        if (!(names[nnames] = strdup("tables.c"))) { snprintf(why, n, "out of memory"); goto out; }
        nnames++;
    }
    char resdir[PATH_LEN], geninc[PATH_LEN], rtinc[PATH_LEN];
    if (!path_join(resdir, sizeof resdir, c->conv_dir, "clang") || !path_join(geninc, sizeof geninc, c->conv_dir, "gen_include") ||
        !path_join(rtinc, sizeof rtinc, c->conv_dir, "runtime")) { snprintf(why, n, "the app path is too long"); goto out; }
    jobs = calloc(nnames + 1, sizeof *jobs);
    if (!jobs) { snprintf(why, n, "out of memory"); goto out; }
    for (size_t i = 0; i < nnames; i++) {
        Job *j = &jobs[njobs++];
        char src[PATH_LEN], o[PATH_LEN], base[256];
        snprintf(base, sizeof base, "%.*s", (int)(strlen(names[i]) - 2), names[i]);
        r = snprintf(o, sizeof o, "%s/%s.o", obj, base);
        int r2 = snprintf(j->log, sizeof j->log, "%s/%s.log", obj, base);
        if (!path_join(src, sizeof src, gen, names[i]) || r <= 0 || (size_t)r >= sizeof o || r2 <= 0 || (size_t)r2 >= sizeof j->log) {
            snprintf(why, n, "the data folder path is too long"); goto out;
        }
        j->name = names[i];
        int ok = args_add(&j->args, tool) && args_add(&j->args, "clang") && args_add(&j->args, "-resource-dir") && args_add(&j->args, resdir);
        for (size_t k = 0; k < cflags.n && ok; k++) ok = args_add(&j->args, cflags.v[k]);
        ok = ok && args_add(&j->args, "-isystem") && args_add(&j->args, geninc) && args_add(&j->args, "-I") && args_add(&j->args, rtinc) &&
             args_add(&j->args, "-c") && args_add(&j->args, src) && args_add(&j->args, "-o") && args_add(&j->args, o);
        if (!ok) { snprintf(why, n, "out of memory"); goto out; }
    }
    Range rg = {c, P_TRANSLATE, P_COMPILE};
    if (!run_jobs(jobs, njobs, process_count(), rg, why, n)) goto out;

    // 3. link
    {
        Job *j = &jobs[njobs++];
        char tbd[PATH_LEN], bundle[PATH_LEN];
        j->name = "the game bundle";
        if (!path_join(tbd, sizeof tbd, c->conv_dir, "libSystem.tbd") || !path_join(bundle, sizeof bundle, work, "game.bundle") ||
            !path_join(j->log, sizeof j->log, work, "link.log")) { snprintf(why, n, "the data folder path is too long"); goto out; }
        int ok = args_add(&j->args, tool) && args_add(&j->args, "ld64.lld");
        for (size_t k = 0; k < ldflags.n && ok; k++) ok = args_add(&j->args, ldflags.v[k]);
        ok = ok && args_add(&j->args, "-bundle_loader") && args_add(&j->args, c->loader);
        for (size_t i = 0; i < njobs - 1 && ok; i++) ok = args_add(&j->args, jobs[i].args.v[jobs[i].args.n - 1]);   // the objects
        ok = ok && args_add(&j->args, tbd) && args_add(&j->args, "-o") && args_add(&j->args, bundle);
        if (!ok) { snprintf(why, n, "out of memory"); goto out; }
        Range lr = {c, P_COMPILE, 1.0};
        if (!run_jobs(j, 1, 1, lr, why, n)) goto out;

        // 4. move the bundle to its place
        if (mkdir(outdir, 0755) != 0 && errno != EEXIST) { snprintf(why, n, "%s: %s", outdir, strerror(errno)); goto out; }
        if (rename(bundle, c->out) != 0) { snprintf(why, n, "%s: %s", c->out, strerror(errno)); goto out; }
    }
    if (c->log) fprintf(c->log, "convert: made %s\n", c->out);
    ret = 1;
out:
    if (work[0] && !rm_tree(work) && c->log) fprintf(c->log, "convert: can't remove %s: %s\n", work, strerror(errno));
    if (ret) remove_others(c, converted, id);
    for (size_t i = 0; i < njobs; i++) args_free(&jobs[i].args);
    free(jobs);
    for (size_t i = 0; i < nnames; i++) free(names[i]);
    free(names);
    args_free(&cflags);
    args_free(&ldflags);
    return ret;
}
