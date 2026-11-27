// The translator of Recomp-Framework win32 in C: the port of tools/recomp_w32.py and tools/pe32.py (see translate32.h).
//
// The port keeps the order of every decision of the Python tool, so that the output is the same byte for byte:
//  - the function discovery iterates the set of function starts in the order of a CPython 3.9 set of ints (PySet);
//  - the post-passes of a function's C (stack access, constant loads, register caching) are hand-written matchers
//    with the semantics of the Python regular expressions;
//  - an error that Python raises as Unsupported inside one instruction becomes bad_insn for that instruction; any
//    other error of a function (Python: any other exception) becomes the "translator error" stub of the function.
// The messages of errors that the Python tool reports from its own exceptions (KeyError and others) are the str()
// of those exceptions.
#include "translate32.h"
#include <capstone/capstone.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#define MAX_INSNS 400000
#define FUNCS_PER_FILE 400

// ================================================================ small tools ================================================================

typedef struct { char *p; size_t n, cap; } Buf;   // a growable byte string (malloc), NUL-terminated when n > 0

static int buf_reserve(Buf *b, size_t extra) {
    if (extra > SIZE_MAX - b->n - 1) return 0;
    size_t need = b->n + extra + 1;
    if (need <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < need) { if (cap > SIZE_MAX / 2) { cap = need; break; } cap *= 2; }
    char *p = realloc(b->p, cap);
    if (!p) return 0;
    b->p = p; b->cap = cap;
    return 1;
}
static int buf_add(Buf *b, const void *s, size_t n) {
    if (!buf_reserve(b, n)) return 0;
    memcpy(b->p + b->n, s, n);
    b->n += n; b->p[b->n] = 0;
    return 1;
}
static int buf_str(Buf *b, const char *s) { return buf_add(b, s, strlen(s)); }
static int buf_vfmt(Buf *b, const char *fmt, va_list ap) {
    va_list aq; va_copy(aq, ap);
    int k = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (k < 0 || !buf_reserve(b, (size_t)k)) return 0;
    vsnprintf(b->p + b->n, (size_t)k + 1, fmt, ap);
    b->n += (size_t)k;
    return 1;
}
static int buf_fmt(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int buf_fmt(Buf *b, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = buf_vfmt(b, fmt, ap); va_end(ap); return r; }
static void buf_free(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static int isword(int ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'; }
static int ishex(int ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }
// Python's \s on the ASCII characters (str.isspace)
static int isspc(int ch) { return ch == ' ' || (ch >= 9 && ch <= 13) || (ch >= 0x1c && ch <= 0x1f); }
static int starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
static int ends(const char *s, const char *p) { size_t a = strlen(s), b = strlen(p); return a >= b && strcmp(s + a - b, p) == 0; }
// s is one of the strings of the NULL-terminated list
static int oneof(const char *s, const char *const *list) { for (; *list; list++) if (strcmp(s, *list) == 0) return 1; return 0; }
#define IN(s, ...) oneof((s), (const char *const[]){__VA_ARGS__, NULL})

// Python's int(s, 16) for the strings of the input files: optional sign is not accepted, an optional 0x prefix is
static int parse_hex(const char *s, size_t n, uint64_t *out) {
    if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; n -= 2; }
    if (!n) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        int ch = s[i], d;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
        else return 0;
        if (v > (UINT64_MAX >> 4)) return 0;
        v = (v << 4) | (uint64_t)d;
    }
    *out = v;
    return 1;
}

// ================================================================ sets of addresses ================================================================

// an open-addressing set of uint32 (key + 1 stored, 0 = empty)
typedef struct { uint64_t *t; size_t mask, n; } USet;
static uint64_t mix(uint64_t k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; return k; }
static int uset_has(const USet *s, uint32_t k) {
    if (!s->t) return 0;
    for (size_t i = mix(k) & s->mask;; i = (i + 1) & s->mask) {
        if (!s->t[i]) return 0;
        if (s->t[i] == (uint64_t)k + 1) return 1;
    }
}
static int uset_grow(USet *s) {
    size_t cap = s->t ? (s->mask + 1) * 2 : 64;
    uint64_t *t = calloc(cap, sizeof *t);
    if (!t) return 0;
    for (size_t j = 0; s->t && j <= s->mask; j++)
        if (s->t[j]) { size_t i = mix(s->t[j] - 1) & (cap - 1); while (t[i]) i = (i + 1) & (cap - 1); t[i] = s->t[j]; }
    free(s->t); s->t = t; s->mask = cap - 1;
    return 1;
}
// 1: added, 2: already there, 0: out of memory
static int uset_add(USet *s, uint32_t k) {
    if (!s->t || (s->n + 1) * 2 > s->mask + 1) if (!uset_grow(s)) return 0;
    size_t i = mix(k) & s->mask;
    while (s->t[i]) { if (s->t[i] == (uint64_t)k + 1) return 2; i = (i + 1) & s->mask; }
    s->t[i] = (uint64_t)k + 1; s->n++;
    return 1;
}
static void uset_del(USet *s, uint32_t k) {   // Python set.discard
    if (!s->t) return;
    size_t i = mix(k) & s->mask;
    while (s->t[i] && s->t[i] != (uint64_t)k + 1) i = (i + 1) & s->mask;
    if (!s->t[i]) return;
    s->t[i] = 0; s->n--;
    for (size_t j = (i + 1) & s->mask; s->t[j]; j = (j + 1) & s->mask) {   // re-place the rest of the run
        uint64_t v = s->t[j]; s->t[j] = 0;
        size_t h = mix(v - 1) & s->mask;
        while (s->t[h]) h = (h + 1) & s->mask;
        s->t[h] = v;
    }
}
static void uset_clear(USet *s) { if (s->t) memset(s->t, 0, (s->mask + 1) * sizeof *s->t); s->n = 0; }
static void uset_free(USet *s) { free(s->t); s->t = NULL; s->mask = s->n = 0; }

// A set of ints with the table and the iteration order of CPython 3.9 (Objects/setobject.c: set_add_entry,
// set_table_resize, set_insert_clean; the hash of an int below 2**61 - 1 is its value). The translator only adds.
typedef struct { uint64_t *t; size_t mask, fill, used; } PySet;   // t[i] = key + 1, 0 = unused
#define PY_LINEAR_PROBES 9
#define PY_PERTURB_SHIFT 5
static int pyset_init(PySet *s) { s->t = calloc(8, sizeof *s->t); s->mask = 7; s->fill = s->used = 0; return s->t != NULL; }
static void pyset_insert_clean(uint64_t *t, size_t mask, uint64_t key) {
    size_t perturb = (size_t)key, i = (size_t)key & mask;
    for (;;) {
        if (!t[i]) { t[i] = key + 1; return; }
        if (i + PY_LINEAR_PROBES <= mask)
            for (size_t j = 1; j <= PY_LINEAR_PROBES; j++) if (!t[i + j]) { t[i + j] = key + 1; return; }
        perturb >>= PY_PERTURB_SHIFT;
        i = (i * 5 + 1 + perturb) & mask;
    }
}
static int pyset_resize(PySet *s, size_t minused) {
    size_t newsize = 8;
    while (newsize <= minused) newsize <<= 1;
    uint64_t *t = calloc(newsize, sizeof *t);
    if (!t) return 0;
    for (size_t j = 0; j <= s->mask; j++) if (s->t[j]) pyset_insert_clean(t, newsize - 1, s->t[j] - 1);
    free(s->t); s->t = t; s->mask = newsize - 1; s->fill = s->used;
    return 1;
}
// the probe sequence of set_add_entry; returns the slot of key, or of the free slot where it goes (-1 never)
static size_t pyset_probe(const PySet *s, uint64_t key, int *found) {
    size_t mask = s->mask, i = (size_t)key & mask, perturb = (size_t)key;
    *found = 0;
    if (!s->t[i]) return i;
    for (;;) {
        if (s->t[i] == key + 1) { *found = 1; return i; }
        if (i + PY_LINEAR_PROBES <= mask)
            for (size_t j = 1; j <= PY_LINEAR_PROBES; j++) {
                if (!s->t[i + j]) return i + j;
                if (s->t[i + j] == key + 1) { *found = 1; return i + j; }
            }
        perturb >>= PY_PERTURB_SHIFT;
        i = (i * 5 + 1 + perturb) & mask;
        if (!s->t[i]) return i;
    }
}
static int pyset_has(const PySet *s, uint64_t key) { int f; pyset_probe(s, key, &f); return f; }
static int pyset_add(PySet *s, uint64_t key) {   // 0: out of memory
    int f;
    size_t i = pyset_probe(s, key, &f);
    if (f) return 1;
    s->t[i] = key + 1; s->fill++; s->used++;
    if (s->fill * 5 < s->mask * 3) return 1;
    return pyset_resize(s, s->used > 50000 ? s->used * 2 : s->used * 4);
}

// ================================================================ PE32 (pe32.py) ================================================================

typedef struct { uint8_t name[8]; int namelen; uint32_t va, vs, fo, fs, ch; } Sec;
typedef struct { uint32_t slot; char *name; } Iat;   // name: "dll!name" or "dll!#ord" (bytes of the file, latin-1)

typedef struct {
    uint8_t *d; size_t n;
    Sec *s; int ns;
    int *code; int nc;   // the executable sections (not .bind)
    uint32_t base, size, entry, text_lo, text_hi;
    Iat *iat; size_t niat;   // sorted by slot
} PE;

static int rd16le(const PE *p, uint64_t o, uint32_t *v) { if (o + 2 > p->n) return 0; *v = p->d[o] | (uint32_t)p->d[o + 1] << 8; return 1; }
static int rd32le(const PE *p, uint64_t o, uint32_t *v) {
    if (o + 4 > p->n) return 0;
    *v = p->d[o] | (uint32_t)p->d[o + 1] << 8 | (uint32_t)p->d[o + 2] << 16 | (uint32_t)p->d[o + 3] << 24;
    return 1;
}
static int pe_sec_of(const PE *p, int64_t a) {   // the first section that holds a (va .. va + max(vs, fs)), or -1
    for (int k = 0; k < p->ns; k++) {
        const Sec *s = &p->s[k];
        uint64_t end = (uint64_t)s->va + (s->vs > s->fs ? s->vs : s->fs);
        if ((int64_t)s->va <= a && a < (int64_t)end) return k;
    }
    return -1;
}
// pe32.read: 1 on success; 0 where Python raises KeyError (an address outside the sections)
static int pe_read(const PE *p, int64_t a, size_t n, uint8_t *out) {
    while (n > 0) {
        int k = pe_sec_of(p, a);
        if (k < 0) return 0;
        const Sec *s = &p->s[k];
        uint64_t end = (uint64_t)s->va + (s->vs > s->fs ? s->vs : s->fs);
        size_t chunk = (uint64_t)(end - (uint64_t)a) < n ? (size_t)(end - (uint64_t)a) : n;
        uint64_t rel = (uint64_t)a - s->va;
        if (rel >= s->fs) memset(out, 0, chunk);
        else {
            uint64_t avail = s->fs - rel < chunk ? s->fs - rel : chunk;
            uint64_t fo = (uint64_t)s->fo + rel;
            uint64_t have = fo < p->n ? (p->n - fo < avail ? p->n - fo : avail) : 0;
            if (have) memcpy(out, p->d + fo, (size_t)have);
            memset(out + have, 0, chunk - (size_t)have);
        }
        a += (int64_t)chunk; out += chunk; n -= chunk;
    }
    return 1;
}
static int pe_u32(const PE *p, int64_t a, uint32_t *v) {
    uint8_t b[4];
    if (!pe_read(p, a, 4, b)) return 0;
    *v = b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 1;
}
static int pe_is_code(const PE *p, int64_t a) {
    for (int k = 0; k < p->nc; k++) { const Sec *s = &p->s[p->code[k]]; if ((int64_t)s->va <= a && a < (int64_t)s->va + s->vs) return 1; }
    return 0;
}
static int64_t pe_code_end(const PE *p, int64_t a) {
    for (int k = 0; k < p->nc; k++) { const Sec *s = &p->s[p->code[k]]; if ((int64_t)s->va <= a && a < (int64_t)s->va + s->vs) return (int64_t)s->va + s->vs; }
    return a;
}
// pe32.cstr: the NUL-terminated string at a (malloc); NULL on an error
static char *pe_cstr(const PE *p, int64_t a) {
    int k = pe_sec_of(p, a);
    if (k < 0) return NULL;
    const Sec *s = &p->s[k];
    uint64_t rel = (uint64_t)a - s->va, o = rel >= s->fs ? 0 : (uint64_t)s->fo + rel;   // Python: off() None -> index from 0
    if (o > p->n) return NULL;
    const uint8_t *z = memchr(p->d + o, 0, p->n - o);
    if (!z) return NULL;
    size_t len = (size_t)(z - (p->d + o));
    char *r = malloc(len + 1);
    if (!r) return NULL;
    memcpy(r, p->d + o, len); r[len] = 0;
    return r;
}
static int iat_cmp(const void *a, const void *b) {
    uint32_t x = ((const Iat *)a)->slot, y = ((const Iat *)b)->slot;
    return x < y ? -1 : x > y;
}
static const char *pe_iat(const PE *p, uint32_t slot) {
    size_t lo = 0, hi = p->niat;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (p->iat[m].slot < slot) lo = m + 1; else hi = m; }
    return lo < p->niat && p->iat[lo].slot == slot ? p->iat[lo].name : NULL;
}
static void pe_free(PE *p) {
    free(p->d); free(p->s); free(p->code);
    for (size_t i = 0; i < p->niat; i++) free(p->iat[i].name);
    free(p->iat);
    memset(p, 0, sizeof *p);
}
static int pe_load(PE *p, const char *path, char *why, size_t wn) {
    memset(p, 0, sizeof *p);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(why, wn, "%s: %s", path, strerror(errno)); return 0; }
    if (fseeko(f, 0, SEEK_END) != 0) { snprintf(why, wn, "%s: %s", path, strerror(errno)); fclose(f); return 0; }
    off_t len = ftello(f);
    if (len < 0x40 || len > (off_t)1 << 30) { snprintf(why, wn, "%s: not a PE file", path); fclose(f); return 0; }
    rewind(f);
    p->d = malloc((size_t)len); p->n = (size_t)len;
    if (!p->d) { snprintf(why, wn, "out of memory"); fclose(f); return 0; }
    if (fread(p->d, 1, p->n, f) != p->n) { snprintf(why, wn, "%s: %s", path, ferror(f) ? strerror(errno) : "short read"); fclose(f); pe_free(p); return 0; }
    fclose(f);
    uint32_t e, nsec, optsz, magic, ndir;
#define BAD(...) do { snprintf(why, wn, __VA_ARGS__); pe_free(p); return 0; } while (0)
    if (!rd32le(p, 0x3C, &e) || (uint64_t)e + 4 > p->n || memcmp(p->d + e, "PE\0\0", 4) != 0) BAD("%s: not a PE file", path);
    if (!rd16le(p, (uint64_t)e + 6, &nsec) || !rd16le(p, (uint64_t)e + 20, &optsz)) BAD("%s: not a PE file", path);
    uint64_t o = (uint64_t)e + 24;
    if (!rd16le(p, o, &magic) || magic != 0x10B) BAD("%s: not a PE32 (i386) image", path);
    uint32_t entry_rva;
    if (!rd32le(p, o + 16, &entry_rva) || !rd32le(p, o + 28, &p->base) || !rd32le(p, o + 56, &p->size) || !rd32le(p, o + 92, &ndir))
        BAD("%s: not a PE file", path);
    p->entry = p->base + entry_rva;
    uint32_t imp_rva = 0;
    if (ndir < 2 || !rd32le(p, o + 96 + 8, &imp_rva)) BAD("%s: the image has no import directory", path);
    p->s = calloc(nsec ? nsec : 1, sizeof *p->s); p->code = calloc(nsec ? nsec : 1, sizeof *p->code);
    if (!p->s || !p->code) BAD("out of memory");
    uint64_t t = o + optsz;
    for (uint32_t k = 0; k < nsec; k++) {
        uint64_t s = t + 40ull * k;
        Sec *x = &p->s[k];
        if (s + 40 > p->n) BAD("%s: the section table is outside the file", path);
        memcpy(x->name, p->d + s, 8);
        x->namelen = 8;
        while (x->namelen > 0 && x->name[x->namelen - 1] == 0) x->namelen--;
        uint32_t va;
        rd32le(p, s + 8, &x->vs); rd32le(p, s + 12, &va); rd32le(p, s + 16, &x->fs); rd32le(p, s + 20, &x->fo); rd32le(p, s + 36, &x->ch);
        x->va = p->base + va;
    }
    p->ns = (int)nsec;
    for (int k = 0; k < p->ns; k++)
        if ((p->s[k].ch & 0x20000000) && !(p->s[k].namelen == 5 && memcmp(p->s[k].name, ".bind", 5) == 0)) p->code[p->nc++] = k;
    if (!p->nc) BAD("%s: the image has no code section", path);
    p->text_lo = UINT32_MAX; p->text_hi = 0;
    for (int k = 0; k < p->nc; k++) {
        const Sec *s = &p->s[p->code[k]];
        if (s->va < p->text_lo) p->text_lo = s->va;
        if (s->va + s->vs > p->text_hi) p->text_hi = s->va + s->vs;
    }
    // imports (read_imports)
    if (imp_rva) {
        size_t cap = 0;
        for (int64_t a = (int64_t)p->base + imp_rva;; a += 20) {
            uint8_t b[20];
            if (!pe_read(p, a, 20, b)) BAD("%s: the import directory is outside the image", path);
            uint32_t w[5];
            for (int k = 0; k < 5; k++) w[k] = b[4 * k] | (uint32_t)b[4 * k + 1] << 8 | (uint32_t)b[4 * k + 2] << 16 | (uint32_t)b[4 * k + 3] << 24;
            uint32_t ilt = w[0], name = w[3], iat = w[4];
            if (!name) break;
            char *dll = pe_cstr(p, (int64_t)p->base + name);
            if (!dll) BAD("%s: an import DLL name cannot be read", path);
            uint32_t src = ilt ? ilt : iat;
            for (int64_t k = 0;; k++) {
                uint32_t v;
                if (!pe_u32(p, (int64_t)p->base + src + 4 * k, &v)) { free(dll); BAD("%s: an import table is outside the image", path); }
                if (!v) break;
                uint32_t slot = (uint32_t)((int64_t)p->base + iat + 4 * k);
                char ord[16], *fn = NULL;
                if (v & 0x80000000u) snprintf(ord, sizeof ord, "#%u", v & 0xFFFFu);
                else if (!(fn = pe_cstr(p, (int64_t)p->base + v + 2))) { free(dll); BAD("%s: an import name cannot be read", path); }
                size_t len = strlen(dll) + 1 + strlen(fn ? fn : ord) + 1;
                char *full = malloc(len);
                if (!full) { free(fn); free(dll); BAD("out of memory"); }
                snprintf(full, len, "%s!%s", dll, fn ? fn : ord);
                free(fn);
                if (p->niat == cap) {
                    cap = cap ? cap * 2 : 256;
                    Iat *ni = realloc(p->iat, cap * sizeof *ni);
                    if (!ni) { free(full); free(dll); BAD("out of memory"); }
                    p->iat = ni;
                }
                p->iat[p->niat].slot = slot; p->iat[p->niat].name = full; p->niat++;
            }
            free(dll);
        }
        // a dict: a slot seen again keeps the last name
        for (size_t i = 0; i < p->niat; i++) p->iat[i].slot = p->iat[i].slot;   // (stable order for the sort below)
        // stable de-duplication: mark earlier duplicates
        for (size_t i = 0; i < p->niat; i++)
            for (size_t j = i + 1; j < p->niat; j++)
                if (p->iat[j].slot == p->iat[i].slot && p->iat[i].name) { free(p->iat[i].name); p->iat[i].name = NULL; break; }
        size_t w = 0;
        for (size_t i = 0; i < p->niat; i++) if (p->iat[i].name) p->iat[w++] = p->iat[i];
        p->niat = w;
        qsort(p->iat, p->niat, sizeof *p->iat, iat_cmp);
    }
#undef BAD
    return 1;
}

// dll_key and imp_sym of the Python tool: 'KERNEL32.dll!CreateFileA' -> 'kernel32_CreateFileA', 'X.dll!#3' -> 'x_ord3'
static void imp_sym(const char *full, char *out, size_t n) {
    const char *bang = strchr(full, '!');
    size_t dl = bang ? (size_t)(bang - full) : strlen(full);
    char d[512];
    if (dl >= sizeof d) dl = sizeof d - 1;
    for (size_t i = 0; i < dl; i++) d[i] = (char)((full[i] >= 'A' && full[i] <= 'Z') ? full[i] + 32 : full[i]);
    d[dl] = 0;
    static const char *const crt[] = {"api-ms-win-crt-", "ucrtbase", "vcruntime", "msvcp", "msvcr"};
    int is_crt = 0;
    for (size_t k = 0; k < sizeof crt / sizeof crt[0]; k++) if (starts(d, crt[k])) is_crt = 1;
    if (is_crt) strcpy(d, "crt");
    else if (ends(d, ".dll")) d[strlen(d) - 4] = 0;
    const char *nm = bang ? bang + 1 : "";
    char tmp[1024];
    if (nm[0] == '#') snprintf(tmp, sizeof tmp, "%s_ord%s", d, nm + 1);
    else snprintf(tmp, sizeof tmp, "%s_%s", d, nm);
    size_t i = 0;
    for (; tmp[i] && i + 1 < n; i++) out[i] = isword((unsigned char)tmp[i]) ? tmp[i] : '_';
    out[i] = 0;
}
// a string of the image (latin-1) as UTF-8, as Python writes it
static int buf_latin1(Buf *b, const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 0x80) { if (!buf_add(b, s + i, 1)) return 0; }
        else { char u[2] = {(char)(0xC0 | (s[i] >> 6)), (char)(0x80 | (s[i] & 0x3F))}; if (!buf_add(b, u, 2)) return 0; }
    }
    return 1;
}
static int buf_latin1s(Buf *b, const char *s) { return buf_latin1(b, (const uint8_t *)s, strlen(s)); }

// ================================================================ the translator state ================================================================

typedef struct { uint32_t addr; char *code; } Patch;
typedef struct { uint32_t addr; char *code; } Probe;   // several for one address, in file order
typedef struct { uint32_t addr; char *name; } Native;

typedef struct {
    PE pe;
    uint32_t *funcs; size_t nfuncs;   // sorted
    USet funcset, strong;
    Patch *patches; size_t npatches;  // sorted by address (a later line for one address replaces an earlier one)
    Probe *probes; size_t nprobes;    // sorted by address, file order within an address
    Native *natives; size_t nnatives; // sorted
    char **provided; size_t nprovided;   // sorted
    int abi_trim;
} Tr;

static const char *find_patch(const Tr *t, uint32_t a) {
    size_t lo = 0, hi = t->npatches;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (t->patches[m].addr < a) lo = m + 1; else hi = m; }
    return lo < t->npatches && t->patches[lo].addr == a ? t->patches[lo].code : NULL;
}
static const char *find_native(const Tr *t, uint32_t a) {
    size_t lo = 0, hi = t->nnatives;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (t->natives[m].addr < a) lo = m + 1; else hi = m; }
    return lo < t->nnatives && t->natives[lo].addr == a ? t->natives[lo].name : NULL;
}
static int is_provided(const Tr *t, const char *s) {
    size_t lo = 0, hi = t->nprovided;
    while (lo < hi) { size_t m = (lo + hi) / 2; int c = strcmp(t->provided[m], s); if (c < 0) lo = m + 1; else hi = m; }
    return lo < t->nprovided && strcmp(t->provided[lo], s) == 0;
}

// ---------------- the context of one worker thread ----------------

typedef struct Chunk { struct Chunk *next; size_t cap, used; } Chunk;   // arena chunk; data follows

typedef struct {
    uint32_t address; uint16_t size; uint8_t nops, jump;
    char mn[CS_MNEMONIC_SIZE]; char op[160];
    cs_x86_op ops[8];
} Ins;

typedef struct { uint32_t addr; int idx; } SeenEnt;   // idx into Fn.iv, -1: the decode failed (Python None)
typedef struct { uint32_t addr; uint32_t *t; size_t n; } Table;

typedef struct Ctx {
    const Tr *tr;
    csh cs;
    cs_insn *ci;
    Chunk *arena;
    jmp_buf fn_jb, ins_jb;
    int in_insn;           // an Unsupported error goes to ins_jb
    int oom;               // the error is out of memory: the translation stops
    char err[256];         // the str() of the error
    // the function
    uint32_t addr;
    Ins *iv; size_t niv, civ;          // decoded instructions, in the order they were decoded
    SeenEnt *so; size_t nso, cso;      // seen, in insertion order
    uint64_t *sh; size_t shmask;       // seen: address -> 1 + index into so (open addressing, key in high bits)
    Table *tables; size_t ntables, ctables;
    int *sorted; size_t nsorted;       // ins: indices into iv sorted by address
    USet labels;
    char **out; size_t nout, cout;     // the lines of the function
    const Ins *cur;
    // unsupported counts of this worker (for the summary)
    struct { char name[48]; long n; } *unsup; size_t nunsup, cunsup;
} Ctx;

static void fail_oom(Ctx *c) { c->oom = 1; snprintf(c->err, sizeof c->err, "out of memory"); longjmp(c->fn_jb, 1); }
static void *ar_alloc(Ctx *c, size_t n) {
    if (n > SIZE_MAX - 64) fail_oom(c);
    n = (n + 15) & ~(size_t)15;
    if (!c->arena || c->arena->cap - c->arena->used < n) {
        size_t cap = n > (1u << 20) ? n : (1u << 20);
        Chunk *k = malloc(sizeof(Chunk) + 16 + cap);
        if (!k) fail_oom(c);
        k->next = c->arena; k->cap = cap; k->used = 0;
        c->arena = k;
    }
    char *base = (char *)(c->arena + 1) + 16 - ((uintptr_t)(c->arena + 1) & 15 ? (uintptr_t)(c->arena + 1) & 15 : 16);
    void *r = base + c->arena->used;
    c->arena->used += n;
    return r;
}
static void ar_reset(Ctx *c) {   // keeps the newest chunk
    if (!c->arena) return;
    Chunk *k = c->arena->next;
    while (k) { Chunk *nx = k->next; free(k); k = nx; }
    c->arena->next = NULL; c->arena->used = 0;
}
static char *sf(Ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static char *sf(Ctx *c, const char *fmt, ...) {
    va_list ap, aq;
    va_start(ap, fmt); va_copy(aq, ap);
    int k = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (k < 0) { va_end(ap); fail_oom(c); }
    char *p = ar_alloc(c, (size_t)k + 1);
    vsnprintf(p, (size_t)k + 1, fmt, ap);
    va_end(ap);
    return p;
}
static char *sdup(Ctx *c, const char *s) { size_t n = strlen(s); char *p = ar_alloc(c, n + 1); memcpy(p, s, n + 1); return p; }

// Python: raise Unsupported(msg)
static void unsup(Ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
static void unsup(Ctx *c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(c->err, sizeof c->err, fmt, ap); va_end(ap);
    longjmp(c->in_insn ? c->ins_jb : c->fn_jb, 1);
}
// any other exception of Python: the error of the whole function
static void pyerr(Ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
static void pyerr(Ctx *c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(c->err, sizeof c->err, fmt, ap); va_end(ap);
    longjmp(c->fn_jb, 1);
}
// "text " + name in Python, where name can be None (TypeError)
#define NONE_CONCAT "can only concatenate str (not \"NoneType\") to str"
static void unsup_name(Ctx *c, const char *what, const char *name) __attribute__((noreturn));
static void unsup_name(Ctx *c, const char *what, const char *name) {
    if (!name) pyerr(c, NONE_CONCAT);
    unsup(c, "%s%s", what, name);
}

static void count_unsup(Ctx *c, const char *mn) {
    for (size_t i = 0; i < c->nunsup; i++) if (strcmp(c->unsup[i].name, mn) == 0) { c->unsup[i].n++; return; }
    if (c->nunsup == c->cunsup) {
        size_t cap = c->cunsup ? c->cunsup * 2 : 16;
        void *p = realloc(c->unsup, cap * sizeof *c->unsup);
        if (!p) return;   // only the summary
        c->unsup = p; c->cunsup = cap;
    }
    snprintf(c->unsup[c->nunsup].name, sizeof c->unsup[c->nunsup].name, "%s", mn);
    c->unsup[c->nunsup++].n = 1;
}

// ---------------- tables of the generator ----------------

static const char *const GPRS[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
typedef struct { const char *name; int g, size, hi; } RegInfo;
static const RegInfo REGS[] = {
    {"eax", 0, 4, 0}, {"ecx", 1, 4, 0}, {"edx", 2, 4, 0}, {"ebx", 3, 4, 0}, {"esp", 4, 4, 0}, {"ebp", 5, 4, 0}, {"esi", 6, 4, 0}, {"edi", 7, 4, 0},
    {"ax", 0, 2, 0}, {"cx", 1, 2, 0}, {"dx", 2, 2, 0}, {"bx", 3, 2, 0}, {"sp", 4, 2, 0}, {"bp", 5, 2, 0}, {"si", 6, 2, 0}, {"di", 7, 2, 0},
    {"al", 0, 1, 0}, {"cl", 1, 1, 0}, {"dl", 2, 1, 0}, {"bl", 3, 1, 0},
    {"ah", 0, 1, 1}, {"ch", 1, 1, 1}, {"dh", 2, 1, 1}, {"bh", 3, 1, 1},
};
static const RegInfo *reg_info(const char *n) {
    if (!n) return NULL;
    for (size_t i = 0; i < sizeof REGS / sizeof REGS[0]; i++) if (strcmp(REGS[i].name, n) == 0) return &REGS[i];
    return NULL;
}
static const char *U(Ctx *c, int64_t n) {
    switch (n) { case 1: return "uint8_t"; case 2: return "uint16_t"; case 4: return "uint32_t"; case 8: return "uint64_t"; }
    pyerr(c, "%lld", (long long)n);
}
static const char *Sg(Ctx *c, int64_t n) {
    switch (n) { case 1: return "int8_t"; case 2: return "int16_t"; case 4: return "int32_t"; case 8: return "int64_t"; }
    pyerr(c, "%lld", (long long)n);
}
static const char *MASKS(Ctx *c, int64_t n) {
    switch (n) { case 1: return "0xFFu"; case 2: return "0xFFFFu"; case 4: return "0xFFFFFFFFu"; case 8: return "0xFFFFFFFFFFFFFFFFull"; }
    pyerr(c, "%lld", (long long)n);
}
static int BITS(Ctx *c, int64_t n) {
    switch (n) { case 1: return 8; case 2: return 16; case 4: return 32; case 8: return 64; }
    pyerr(c, "%lld", (long long)n);
}
static const char *RD(Ctx *c, int64_t n) {
    switch (n) { case 1: return "rd8"; case 2: return "rd16"; case 4: return "rd32"; case 8: return "rd64"; }
    pyerr(c, "%lld", (long long)n);
}
static const char *WR(Ctx *c, int64_t n) {
    switch (n) { case 1: return "wr8"; case 2: return "wr16"; case 4: return "wr32"; case 8: return "wr64"; }
    pyerr(c, "%lld", (long long)n);
}
static const char *CC(const char *k) {
    static const char *const t[][2] = {
        {"o", "of"}, {"no", "!of"}, {"b", "cf"}, {"c", "cf"}, {"nae", "cf"}, {"ae", "!cf"}, {"nb", "!cf"}, {"nc", "!cf"},
        {"e", "zf"}, {"z", "zf"}, {"ne", "!zf"}, {"nz", "!zf"}, {"be", "(cf||zf)"}, {"na", "(cf||zf)"},
        {"a", "(!cf&&!zf)"}, {"nbe", "(!cf&&!zf)"}, {"s", "sf"}, {"ns", "!sf"}, {"p", "parity8(pr)"}, {"pe", "parity8(pr)"},
        {"np", "!parity8(pr)"}, {"po", "!parity8(pr)"}, {"l", "(sf!=of)"}, {"nge", "(sf!=of)"}, {"ge", "(sf==of)"},
        {"nl", "(sf==of)"}, {"le", "(zf||sf!=of)"}, {"ng", "(zf||sf!=of)"}, {"g", "(!zf&&sf==of)"}, {"nle", "(!zf&&sf==of)"},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (strcmp(t[i][0], k) == 0) return t[i][1];
    return NULL;
}
static const char *const NORETURN_IMPORTS[] = {"ExitProcess", "TerminateProcess", "FatalExit", "_exit", "exit", "abort", "terminate",
    "_invoke_watson", "_invalid_parameter_noinfo_noreturn", "_CxxThrowException", "RaiseException__noreturn", NULL};
static int noreturn_import(const char *full) { const char *b = strrchr(full, '!'); return oneof(b ? b + 1 : full, NORETURN_IMPORTS); }

// ================================================================ decoding ================================================================

static int decode_raw(Ctx *c, int64_t a, Ins *out) {
    int64_t end = pe_code_end(&c->tr->pe, a);
    size_t n = end - a < 16 ? (size_t)(end - a) : 16;
    uint8_t b[16];
    if (n == 0 || !pe_read(&c->tr->pe, a, n, b)) return 0;
    const uint8_t *p = b; size_t sz = n; uint64_t addr = (uint64_t)a;
    if (!cs_disasm_iter(c->cs, &p, &sz, &addr, c->ci)) return 0;
    out->address = (uint32_t)c->ci->address; out->size = c->ci->size;
    snprintf(out->mn, sizeof out->mn, "%s", c->ci->mnemonic);
    snprintf(out->op, sizeof out->op, "%s", c->ci->op_str);
    const cs_x86 *x = &c->ci->detail->x86;
    out->nops = x->op_count > 8 ? 8 : x->op_count;
    memcpy(out->ops, x->operands, sizeof out->ops);
    out->jump = cs_insn_group(c->cs, c->ci, CS_GRP_JUMP) ? 1 : 0;
    return 1;
}
static const char *rn(Ctx *c, unsigned reg) { return cs_reg_name(c->cs, reg); }

// ================================================================ FuncGen: the instructions of a function ================================================================

static int in_region(Ctx *c, int64_t a) {
    const PE *p = &c->tr->pe;
    return (int64_t)p->text_lo <= a && a < (int64_t)p->text_hi && (a == c->addr || !uset_has(&c->tr->strong, (uint32_t)a)) && pe_is_code(p, a);
}

// seen: address -> position in so
static long seen_pos(const Ctx *c, int64_t a) {
    if (!c->sh || a < 0 || a > UINT32_MAX) return -1;
    for (size_t i = mix((uint64_t)a) & c->shmask;; i = (i + 1) & c->shmask) {
        uint64_t v = c->sh[i];
        if (!v) return -1;
        if ((uint32_t)(v >> 32) == (uint32_t)a && (v & 0xFFFFFFFFu)) return (long)(v & 0xFFFFFFFFu) - 1;
    }
}
static int seen_has(const Ctx *c, int64_t a) { return seen_pos(c, a) >= 0; }
static const Ins *seen_ins(const Ctx *c, int64_t a) {   // NULL also for a failed decode
    long p = seen_pos(c, a);
    return p >= 0 && c->so[p].idx >= 0 ? &c->iv[c->so[p].idx] : NULL;
}
static void seen_put(Ctx *c, uint32_t a, int idx) {
    if (c->nso == c->cso) {
        size_t cap = c->cso ? c->cso * 2 : 1024;
        SeenEnt *n = realloc(c->so, cap * sizeof *n);
        if (!n) fail_oom(c);
        c->so = n; c->cso = cap;
    }
    if (!c->sh || (c->nso + 1) * 2 > c->shmask + 1) {
        size_t cap = c->sh ? (c->shmask + 1) * 2 : 4096;
        uint64_t *t = calloc(cap, sizeof *t);
        if (!t) fail_oom(c);
        for (size_t j = 0; c->sh && j <= c->shmask; j++)
            if (c->sh[j]) { size_t i = mix(c->sh[j] >> 32) & (cap - 1); while (t[i]) i = (i + 1) & (cap - 1); t[i] = c->sh[j]; }
        free(c->sh); c->sh = t; c->shmask = cap - 1;
    }
    c->so[c->nso].addr = a; c->so[c->nso].idx = idx; c->nso++;
    size_t i = mix(a) & c->shmask;
    while (c->sh[i]) i = (i + 1) & c->shmask;
    c->sh[i] = (uint64_t)a << 32 | (uint64_t)c->nso;
}

typedef struct { int64_t *v; size_t n, cap; } I64s;
static void i64_push(Ctx *c, I64s *s, int64_t x) {
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 256;
        int64_t *p = realloc(s->v, cap * sizeof *p);
        if (!p) fail_oom(c);
        s->v = p; s->cap = cap;
    }
    s->v[s->n++] = x;
}

static int ends_block(Ctx *c, const Ins *i);

static uint32_t iat_slot(Ctx *c, const cs_x86_op *op, int *ok) {
    *ok = 0;
    if (op->type == X86_OP_MEM && !op->mem.base && !op->mem.index && (op->mem.segment == 0 || op->mem.segment == X86_REG_DS)) {
        uint32_t a = (uint32_t)(op->mem.disp & 0xFFFFFFFF);
        if (pe_iat(&c->tr->pe, a)) { *ok = 1; return a; }
    }
    return 0;
}
static int noreturn_call(Ctx *c, const Ins *i) {
    if (i->nops < 1) pyerr(c, "list index out of range");
    const cs_x86_op *op = &i->ops[0];
    int ok; uint32_t slot = iat_slot(c, op, &ok);
    if (ok) return noreturn_import(pe_iat(&c->tr->pe, slot));
    if (op->type == X86_OP_IMM) return 0;   // the Translator's noreturn set is empty
    return 0;
}
static int ends_block(Ctx *c, const Ins *i) {
    const char *m = i->mn;
    if (strcmp(m, "call") == 0) return noreturn_call(c, i);
    return IN(m, "ret", "retf", "jmp", "ud2", "hlt", "int3", "iretd", "ljmp") || starts(m, "ret") ||
           (strcmp(m, "int") == 0 && i->nops && i->ops[0].imm == 0x29);
}
static int same_gpr(Ctx *c, unsigned a, unsigned b) {
    const RegInfo *na = reg_info(rn(c, a)), *nb = reg_info(rn(c, b));
    return na && nb && na->g == nb->g;
}
static int is_loopish(const char *m) { return IN(m, "loop", "loope", "loopne", "jecxz", "jcxz"); }

// read_table: the entries (code addresses in the function) of a table; *n entries in the arena
static uint32_t *read_table(Ctx *c, int64_t ta, int64_t lim, int64_t bound, int64_t step, size_t *nt) {
    size_t cap = 64, k = 0;
    uint32_t *t = ar_alloc(c, cap * sizeof *t);
    int64_t n = 0;
    while (n < lim) {
        uint32_t v;
        if (!pe_u32(&c->tr->pe, ta + step * n, &v)) break;
        n++;
        if (!pe_is_code(&c->tr->pe, v)) { if (bound) continue; break; }
        if (!in_region(c, v)) { if (bound) continue; break; }
        if (k == cap) { uint32_t *nt2 = ar_alloc(c, cap * 2 * sizeof *nt2); memcpy(nt2, t, cap * sizeof *t); t = nt2; cap *= 2; }
        t[k++] = v;
    }
    *nt = k;
    return t;
}

// the max of the bytes at a..a+n, +1; 0 when a byte is outside the image (KeyError) or n is 0
static int64_t max_byte_plus1(Ctx *c, int64_t a, int64_t n) {
    int m = -1;
    uint8_t b[4096];
    while (n > 0) {
        size_t k = n > (int64_t)sizeof b ? sizeof b : (size_t)n;
        if (!pe_read(&c->tr->pe, a, k, b)) return 0;
        for (size_t i = 0; i < k; i++) if (b[i] > m) m = b[i];
        a += (int64_t)k; n -= (int64_t)k;
    }
    if (m < 0) pyerr(c, "max() arg is an empty sequence");
    return m + 1;
}

// table_limit: 0 where Python returns None or 0
static int64_t table_limit(Ctx *c, const Ins *const *prev, long nprev, long k, unsigned idx) {
    long stop = k - 12 > -1 ? k - 12 : -1;
    for (long j = k - 1; j > stop; j--) {
        const Ins *p = prev[j];
        const cs_x86_op *po = p->ops;
        int np = p->nops;
        if (IN(p->mn, "mov", "movzx", "movsx") && np == 2 && po[0].type == X86_OP_REG && po[1].type == X86_OP_REG && same_gpr(c, po[0].reg, idx)) {
            idx = po[1].reg; continue;
        }
        if (strcmp(p->mn, "cmp") == 0 && np == 2 && po[1].type == X86_OP_IMM && po[0].type == X86_OP_REG && same_gpr(c, po[0].reg, idx)) {
            const char *nxt = j + 1 < nprev ? prev[j + 1]->mn : "";
            return (int64_t)(po[1].imm & 0xFFFFFFFF) + (IN(nxt, "jb", "jae", "jnb", "jc", "jnc") ? 0 : 1);
        }
        if (strcmp(p->mn, "and") == 0 && np == 2 && po[0].type == X86_OP_REG && po[1].type == X86_OP_IMM && same_gpr(c, po[0].reg, idx)) {
            int64_t m = po[1].imm & 0xFFFFFFFF;
            return m < 32 && !(m & (m + 1)) ? m + 1 : 0;
        }
        if (IN(p->mn, "add", "sub", "dec", "inc") && np && po[0].type == X86_OP_REG && same_gpr(c, po[0].reg, idx)) continue;
        if (strcmp(p->mn, "lea") == 0 && np == 2 && po[0].type == X86_OP_REG && same_gpr(c, po[0].reg, idx) && po[1].mem.base && !po[1].mem.index) {
            idx = po[1].mem.base; continue;
        }
        if (strcmp(p->mn, "movzx") == 0) {
            if (np < 2) pyerr(c, "list index out of range");
            if (po[0].type == X86_OP_REG && same_gpr(c, po[0].reg, idx) && po[1].type == X86_OP_MEM) {
                const x86_op_mem *mm = &po[1].mem;
                unsigned ix = mm->index ? mm->index : mm->base;
                int64_t bl = ix ? table_limit(c, prev, nprev, j, ix) : 0;
                if (bl && (mm->disp & 0xFFFFFFFF)) return max_byte_plus1(c, mm->disp & 0xFFFFFFFF, bl);
                return 0;
            }
        }
        if (np && po[0].type == X86_OP_REG && same_gpr(c, po[0].reg, idx) && !IN(p->mn, "test", "cmp")) return 0;
    }
    return 0;
}

// borrow_index: N of 'sub idx, N ; jb L' ... 'L: jmp [idx*4 + table]', else 0
static int64_t borrow_index(Ctx *c, const Ins *i, unsigned idx) {
    int64_t n = 0;
    for (size_t q = 0; q < c->nso; q++) {
        if (c->so[q].idx < 0) continue;
        const Ins *x = &c->iv[c->so[q].idx];
        if ((int64_t)x->address + x->size == i->address && !IN(x->mn, "jmp", "ret")) return 0;
        const cs_x86_op *ops = x->ops;
        if (!(x->jump && x->nops && ops[0].type == X86_OP_IMM && (uint32_t)(ops[0].imm & 0xFFFFFFFF) == i->address)) continue;
        if (strcmp(x->mn, "jb") != 0) return 0;
        const Ins *s = NULL;
        for (size_t r = 0; r < c->nso; r++) {
            if (c->so[r].idx < 0) continue;
            const Ins *p = &c->iv[c->so[r].idx];
            if ((int64_t)p->address + p->size == x->address) { s = p; break; }
        }
        if (!s || strcmp(s->mn, "sub") != 0 || s->nops != 2) return 0;
        const cs_x86_op *so = s->ops;
        if (so[0].type != X86_OP_REG || !same_gpr(c, so[0].reg, idx) || so[1].type != X86_OP_IMM) return 0;
        int64_t k = so[1].imm & 0xFFFFFFFF;
        if (!(0 < k && k <= 16) || (n && n != k)) return 0;
        n = k;
    }
    return n;
}

static int ins_addr_cmp(const void *a, const void *b) {
    uint32_t x = (*(const Ins *const *)a)->address, y = (*(const Ins *const *)b)->address;
    return x < y ? -1 : x > y;
}

// jump_table: the targets (arena) or NULL (Python None or an empty list)
static uint32_t *jump_table(Ctx *c, const Ins *i, size_t *nt) {
    *nt = 0;
    if (!i->nops) pyerr(c, "list index out of range");
    const cs_x86_op *op = &i->ops[0];
    // prev: the decoded instructions in the 96 bytes before i, by address
    size_t np = 0;
    const Ins **prev = ar_alloc(c, (c->niv ? c->niv : 1) * sizeof *prev);
    for (size_t q = 0; q < c->nso; q++) {
        if (c->so[q].idx < 0) continue;
        const Ins *x = &c->iv[c->so[q].idx];
        if ((int64_t)i->address - 96 <= (int64_t)x->address && x->address < i->address) prev[np++] = x;
    }
    qsort(prev, np, sizeof *prev, ins_addr_cmp);
    uint32_t *t;
    if (op->type == X86_OP_MEM) {
        const x86_op_mem *mm = &op->mem;
        int ok; iat_slot(c, op, &ok);
        if (mm->base || !mm->index || mm->scale != 4 || ok) return NULL;
        int64_t ta = mm->disp & 0xFFFFFFFF;
        int64_t n = borrow_index(c, i, mm->index);
        if (n) { t = read_table(c, ta - 4 * n, n, n, 4, nt); return *nt ? t : NULL; }
        for (long k = (long)np - 1; k >= 0; k--) {   // MSVC memmove: neg ecx ; jmp [ecx*4 + table] reads backward
            const Ins *x = prev[k];
            if (x->nops && x->ops[0].type == X86_OP_REG && same_gpr(c, x->ops[0].reg, mm->index) && !IN(x->mn, "cmp", "test")) {
                if (strcmp(x->mn, "neg") == 0) {
                    int64_t bound = table_limit(c, prev, (long)np, k, mm->index);
                    if (!bound) return NULL;
                    t = read_table(c, ta, bound, bound, -4, nt);
                    return *nt ? t : NULL;
                }
                break;
            }
        }
        int64_t bound = table_limit(c, prev, (long)np, (long)np, mm->index);
        t = read_table(c, ta, bound ? bound : 4096, bound, 4, nt);
        return *nt ? t : NULL;
    }
    unsigned r = op->reg;
    for (long k = (long)np - 1; k >= 0; k--) {
        const Ins *x = prev[k];
        const cs_x86_op *ops = x->ops;
        if (strcmp(x->mn, "mov") == 0 && x->nops == 2 && ops[0].type == X86_OP_REG && same_gpr(c, ops[0].reg, r) && ops[1].type == X86_OP_MEM) {
            const x86_op_mem *mm = &ops[1].mem;
            if (mm->base || !mm->index || mm->scale != 4) return NULL;
            int64_t bound = table_limit(c, prev, (long)np, k, mm->index);
            t = read_table(c, mm->disp & 0xFFFFFFFF, bound ? bound : 4096, bound, 4, nt);
            return *nt ? t : NULL;
        }
        if (x->nops && ops[0].type == X86_OP_REG && same_gpr(c, ops[0].reg, r)) return NULL;
    }
    return NULL;
}

static Ins *new_ins(Ctx *c) {
    if (c->niv == c->civ) {
        size_t cap = c->civ ? c->civ * 2 : 1024;
        Ins *n = realloc(c->iv, cap * sizeof *n);
        if (!n) fail_oom(c);
        c->iv = n; c->civ = cap;
    }
    return &c->iv[c->niv];
}

static const Table *find_table(const Ctx *c, uint32_t a) {
    for (size_t k = 0; k < c->ntables; k++) if (c->tables[k].addr == a) return &c->tables[k];
    return NULL;
}

// descend: every instruction reachable from the start that is not another function's start
static void descend(Ctx *c) {
    I64s work = {0}, pending = {0};
    // the vectors are freed on every path: an error longjmps out, so they live in the arena-free ctx copies below
    i64_push(c, &work, c->addr);
    I64s *wp = &work, *pp = &pending;
    for (;;) {
        while (wp->n) {
            int64_t a = wp->v[--wp->n];
            while (!seen_has(c, a) && in_region(c, a)) {
                Ins *i = new_ins(c);
                if (!decode_raw(c, a, i)) { seen_put(c, (uint32_t)a, -1); break; }
                int idx = (int)c->niv++;
                seen_put(c, (uint32_t)a, idx);
                if (c->nso > MAX_INSNS) { free(work.v); free(pending.v); unsup(c, "function too large"); }
                i = &c->iv[idx];
                const char *m = i->mn;
                if ((i->jump || is_loopish(m)) && i->nops && i->ops[0].type == X86_OP_IMM) {
                    int64_t t = i->ops[0].imm & 0xFFFFFFFF;
                    if (in_region(c, t)) i64_push(c, wp, t);
                }
                if (strcmp(m, "jmp") == 0 && i->nops && (i->ops[0].type == X86_OP_REG || i->ops[0].type == X86_OP_MEM)) i64_push(c, pp, idx);
                if (ends_block(c, &c->iv[idx])) break;
                a = (int64_t)c->iv[idx].address + c->iv[idx].size;
            }
        }
        for (size_t q = 0; q < pp->n; q++) {
            const Ins *i = &c->iv[pp->v[q]];
            if (find_table(c, i->address)) continue;
            size_t nt;
            uint32_t *res = jump_table(c, i, &nt);
            if (res) {
                if (c->ntables == c->ctables) {
                    size_t cap = c->ctables ? c->ctables * 2 : 16;
                    Table *n = realloc(c->tables, cap * sizeof *n);
                    if (!n) { free(work.v); free(pending.v); fail_oom(c); }
                    c->tables = n; c->ctables = cap;
                }
                c->tables[c->ntables].addr = i->address; c->tables[c->ntables].t = res; c->tables[c->ntables].n = nt; c->ntables++;
                for (size_t k = 0; k < nt; k++) if (!seen_has(c, res[k])) i64_push(c, wp, res[k]);
            }
        }
        pp->n = 0;
        if (!wp->n) break;
    }
    free(work.v); free(pending.v);
    // ins: sorted by address
    c->sorted = ar_alloc(c, (c->niv ? c->niv : 1) * sizeof *c->sorted);
    c->nsorted = 0;
    const Ins **tmp = ar_alloc(c, (c->niv ? c->niv : 1) * sizeof *tmp);
    for (size_t q = 0; q < c->nso; q++) if (c->so[q].idx >= 0) tmp[c->nsorted++] = &c->iv[c->so[q].idx];
    qsort(tmp, c->nsorted, sizeof *tmp, ins_addr_cmp);
    for (size_t k = 0; k < c->nsorted; k++) c->sorted[k] = (int)(tmp[k] - c->iv);
}

// ---------------- operands ----------------

static char *ea(Ctx *c, const cs_x86_op *op) {
    const x86_op_mem *m = &op->mem;
    const char *parts[4]; int np = 0;
    if (m->base) {
        const char *n = rn(c, m->base);
        const RegInfo *b = reg_info(n);
        if (!b || b->size != 4) unsup_name(c, "base ", n);
        parts[np++] = sf(c, "C.%s", GPRS[b->g]);
    }
    if (m->index) {
        const char *n = rn(c, m->index);
        const RegInfo *ix = reg_info(n);
        if (!ix || ix->size != 4) unsup_name(c, "index ", n);
        parts[np++] = m->scale != 1 ? sf(c, "C.%s*%du", GPRS[ix->g], m->scale) : sf(c, "C.%s", GPRS[ix->g]);
    }
    if (m->disp || !np) parts[np++] = sf(c, "0x%llxu", (unsigned long long)(m->disp & 0xFFFFFFFF));
    if (m->segment == X86_REG_FS) { memmove(parts + 1, parts, np * sizeof *parts); parts[0] = "C.fsbase"; np++; }
    else if (m->segment == X86_REG_GS) unsup(c, "gs");
    if (np == 1 && starts(parts[0], "0x")) return (char *)parts[0];
    size_t len = 13;
    for (int k = 0; k < np; k++) len += strlen(parts[k]) + 3;
    char *r = ar_alloc(c, len);
    strcpy(r, "(uint32_t)(");
    for (int k = 0; k < np; k++) { if (k) strcat(r, " + "); strcat(r, parts[k]); }
    strcat(r, ")");
    return r;
}
static char *rd(Ctx *c, const cs_x86_op *op, int64_t size) {
    if (!size) size = op->size;
    if (op->type == X86_OP_REG) {
        const char *n = rn(c, op->reg);
        const RegInfo *r = reg_info(n);
        if (!r) unsup_name(c, "reg ", n);
        if (r->hi) return sf(c, "(uint8_t)(C.%s >> 8)", GPRS[r->g]);
        return r->size == 4 ? sf(c, "C.%s", GPRS[r->g]) : sf(c, "(%s)C.%s", U(c, r->size), GPRS[r->g]);
    }
    if (op->type == X86_OP_IMM) {
        if (size >= 8) return sf(c, "0x%llxull", (unsigned long long)op->imm);
        const char *u = U(c, size);
        uint64_t mask = size >= 8 ? UINT64_MAX : (size <= 0 ? 0 : ((uint64_t)1 << (8 * size)) - 1);
        return sf(c, "(%s)0x%llxu", u, (unsigned long long)((uint64_t)op->imm & mask));
    }
    const char *f = RD(c, size);
    return sf(c, "%s(%s)", f, ea(c, op));
}
static char *wr_reg(Ctx *c, const RegInfo *r, const char *v) {
    const char *g = GPRS[r->g];
    if (r->hi) return sf(c, "C.%s = (C.%s & ~0xFF00u) | ((uint32_t)(uint8_t)(%s) << 8);", g, g, v);
    if (r->size == 4) return sf(c, "C.%s = (uint32_t)(%s);", g, v);
    const char *mk = MASKS(c, r->size), *u = U(c, r->size);
    return sf(c, "C.%s = (C.%s & ~%s) | (%s)(%s);", g, g, mk, u, v);
}
static char *wr(Ctx *c, const cs_x86_op *op, const char *v, int64_t size) {
    if (!size) size = op->size;
    if (op->type == X86_OP_REG) {
        const char *n = rn(c, op->reg);
        const RegInfo *r = reg_info(n);
        if (!r) unsup_name(c, "wreg ", n);
        return wr_reg(c, r, v);
    }
    const char *w = WR(c, size);
    const char *a = ea(c, op);
    const char *u = U(c, size);
    return sf(c, "%s(%s, (%s)(%s));", w, a, u, v);
}
static char *wr_name(Ctx *c, const char *reg, const char *v) { return wr_reg(c, reg_info(reg), v); }
static char *xmm(Ctx *c, const cs_x86_op *op) {
    const char *n = rn(c, op->reg);
    if (!n) pyerr(c, "'NoneType' object has no attribute 'startswith'");
    if (!starts(n, "xmm")) unsup(c, "not xmm %s", n);
    return sf(c, "C.xmm[%d]", atoi(n + 3));
}
static int isx(Ctx *c, const cs_x86_op *op) { const char *n; return op->type == X86_OP_REG && (n = rn(c, op->reg)) && starts(n, "xmm"); }
static int ismm(Ctx *c, const cs_x86_op *op) {
    const char *n;
    return op->type == X86_OP_REG && (n = rn(c, op->reg)) && n[0] == 'm' && n[1] == 'm' && n[2] >= '0' && n[2] <= '7' && !n[3];
}

static void emit_line(Ctx *c, char *s) {
    if (c->nout == c->cout) {
        size_t cap = c->cout ? c->cout * 2 : 1024;
        char **n = realloc(c->out, cap * sizeof *n);
        if (!n) fail_oom(c);
        c->out = n; c->cout = cap;
    }
    c->out[c->nout++] = s;
}
static void e(Ctx *c, const char *s) { emit_line(c, sf(c, "    %s", s)); }
static void ef(Ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void ef(Ctx *c, const char *fmt, ...) {
    va_list ap, aq;
    va_start(ap, fmt); va_copy(aq, ap);
    int k = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (k < 0) { va_end(ap); fail_oom(c); }
    char *p = ar_alloc(c, (size_t)k + 5);
    memcpy(p, "    ", 4);
    vsnprintf(p + 4, (size_t)k + 1, fmt, ap);
    va_end(ap);
    emit_line(c, p);
}
static char *szf(Ctx *c, const char *r, int64_t size) {
    const char *mk = MASKS(c, size);
    int b = BITS(c, size);
    return sf(c, "zf = ((%s) & %s) == 0; sf = ((%s) >> %d) & 1; pr = (uint8_t)(%s);", r, mk, r, b - 1, r);
}

// ---------------- control transfer ----------------

static char *tail(Ctx *c, int64_t t, uint32_t frm) {
    if (t >= 0 && t <= UINT32_MAX && uset_has(&c->tr->funcset, (uint32_t)t)) return sf(c, "F_%llx(c); return;", (long long)t);
    return sf(c, "missing_func(c, 0x%llxu, 0x%xu); return;", (long long)t, frm);
}
static char *go_to(Ctx *c, int64_t t, uint32_t frm) {
    if (seen_ins(c, t)) return sf(c, "goto L_%llx;", (long long)t);
    return tail(c, t, frm);
}
static char *goto_block(Ctx *c, int64_t t, uint32_t frm) {
    char *g = go_to(c, t, frm);
    return starts(g, "goto") ? g : sf(c, "{ %s }", g);
}
static void call(Ctx *c, const Ins *i, const cs_x86_op *op, uint32_t nxt) {
    if (op->type == X86_OP_IMM) {
        uint32_t t = (uint32_t)(op->imm & 0xFFFFFFFF);
        if (t == nxt) { ef(c, "PUSH(c, 0x%xu);", nxt); return; }
        if (uset_has(&c->tr->funcset, t)) ef(c, "PUSH(c, 0x%xu); F_%x(c);", nxt, t);
        else ef(c, "PUSH(c, 0x%xu); missing_func(c, 0x%xu, 0x%xu);", nxt, t, i->address);
        return;
    }
    int ok; uint32_t slot = iat_slot(c, op, &ok);
    if (ok) {
        char s[512]; imp_sym(pe_iat(&c->tr->pe, slot), s, sizeof s);
        ef(c, "PUSH(c, 0x%xu); I_%s(c); C.esp += 4;", nxt, s);
        return;
    }
    const char *v = rd(c, op, 4);
    ef(c, "{ uint32_t t = %s; PUSH(c, 0x%xu); call_ind(c, t); }", v, nxt);
}

// ---------------- integer ALU ----------------

static int muldiv(Ctx *c, const char *m, const Ins *in);
static int alu(Ctx *c, const char *m, const Ins *in) {
    const cs_x86_op *ops = in->ops;
    int nops = in->nops;
    if (IN(m, "add", "sub", "cmp", "adc", "sbb") && nops == 2) {
        const cs_x86_op *d = &ops[0], *s = &ops[1];
        int64_t n = d->size;
        const char *a = rd(c, d, 0), *b = rd(c, s, n);
        const char *u = U(c, n);
        const char *car = strcmp(m, "adc") == 0 ? sf(c, " + (%s)cf", u) : strcmp(m, "sbb") == 0 ? sf(c, " - (%s)cf", u) : "";
        if (IN(m, "add", "adc")) {
            const char *cfe = strcmp(m, "adc") == 0 ? "r < a || (cf_in && r == a)" : "r < a";
            int bits = BITS(c, n);
            const char *z = szf(c, "r", n), *w = wr(c, d, "r", 0);
            ef(c, "{ %s a = %s, b = %s, r; uint32_t cf_in = cf; (void)cf_in; r = (%s)(a + b%s); cf = %s; of = ((~(a ^ b) & (a ^ r)) >> %d) & 1; %s %s }",
               u, a, b, u, car, cfe, bits - 1, z, w);
        } else {
            const char *cfe = strcmp(m, "sbb") == 0 ? "a < b || (cf_in && a == b)" : "a < b";
            const char *wb = strcmp(m, "cmp") == 0 ? "" : wr(c, d, "r", 0);
            int bits = BITS(c, n);
            const char *z = szf(c, "r", n);
            ef(c, "{ %s a = %s, b = %s, r; uint32_t cf_in = cf; (void)cf_in; r = (%s)(a - b%s); cf = %s; of = (((a ^ b) & (a ^ r)) >> %d) & 1; %s %s }",
               u, a, b, u, car, cfe, bits - 1, z, wb);
        }
        return 1;
    }
    if (IN(m, "and", "or", "xor", "test") && nops == 2) {
        const cs_x86_op *d = &ops[0], *s = &ops[1];
        int64_t n = d->size;
        if (strcmp(m, "xor") == 0 && s->type == X86_OP_REG && d->type == X86_OP_REG && s->reg == d->reg) {
            ef(c, "%s cf = 0; of = 0; zf = 1; sf = 0; pr = 0;", wr(c, d, "0", 0)); return 1;
        }
        const char *op = strcmp(m, "or") == 0 ? "|" : strcmp(m, "xor") == 0 ? "^" : "&";
        const char *wb = strcmp(m, "test") == 0 ? "" : wr(c, d, "r", 0);
        const char *u = U(c, n);
        const char *a = rd(c, d, 0), *b = rd(c, s, n), *z = szf(c, "r", n);
        ef(c, "{ %s r = (%s)(%s %s %s); cf = 0; of = 0; %s %s }", u, u, a, op, b, z, wb);
        return 1;
    }
    if (IN(m, "inc", "dec")) {
        if (nops < 1) pyerr(c, "list index out of range");
        const cs_x86_op *d = &ops[0]; int64_t n = d->size;
        const char *op = strcmp(m, "inc") == 0 ? "+" : "-";
        const char *u = U(c, n); int bits = BITS(c, n);
        const char *ofc = strcmp(m, "inc") == 0 ? sf(c, "r == (%s)((%s)1 << %d)", u, u, bits - 1) : sf(c, "a == (%s)((%s)1 << %d)", u, u, bits - 1);
        const char *a = rd(c, d, 0), *z = szf(c, "r", n), *w = wr(c, d, "r", 0);
        ef(c, "{ %s a = %s, r = (%s)(a %s 1); of = %s; %s %s }", u, a, u, op, ofc, z, w);
        return 1;
    }
    if (strcmp(m, "neg") == 0) {
        const cs_x86_op *d = &ops[0]; int64_t n = d->size;
        const char *u = U(c, n), *a = rd(c, d, 0);
        int bits = BITS(c, n);
        const char *z = szf(c, "r", n), *w = wr(c, d, "r", 0);
        ef(c, "{ %s a = %s, r = (%s)(0 - a); cf = a != 0; of = a == (%s)((%s)1 << %d); %s %s }", u, a, u, u, u, bits - 1, z, w);
        return 1;
    }
    if (strcmp(m, "not") == 0) { const cs_x86_op *d = &ops[0]; e(c, wr(c, d, sf(c, "~%s", rd(c, d, 0)), 0)); return 1; }
    if (IN(m, "shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr")) {
        const cs_x86_op *d = &ops[0]; int64_t n = d->size; int bits = BITS(c, n);
        const char *cnt = nops > 1 && ops[1].type == X86_OP_IMM ? sf(c, "%lld", (long long)(ops[1].imm & 31))
                        : nops > 1 ? sf(c, "(uint32_t)(%s & 31)", rd(c, &ops[1], 1)) : "1";
        const char *a = rd(c, d, 0);
        const char *W = "uint32_t", *body;
        const char *u = U(c, n);
        if (IN(m, "shl", "sal"))
            body = sf(c, "r = (%s)((%s)a << k); cf = k <= %d ? ((%s)a >> (%d - k)) & 1 : 0; of = ((r >> %d) & 1) ^ cf;", u, W, bits, W, bits, bits - 1);
        else if (strcmp(m, "shr") == 0)
            body = sf(c, "r = (%s)((%s)a >> k); cf = ((%s)a >> (k - 1)) & 1; of = (a >> %d) & 1;", u, W, W, bits - 1);
        else if (strcmp(m, "sar") == 0) {
            const char *s = Sg(c, n);
            body = sf(c, "r = (%s)((int32_t)(%s)a >> (k > %d ? %d : k)); cf = ((int32_t)(%s)a >> (k - 1 > %d ? %d : k - 1)) & 1; of = 0;", u, s, bits - 1, bits - 1, s, bits - 1, bits - 1);
        } else if (IN(m, "rol", "ror")) {
            if (strcmp(m, "rol") == 0)
                body = sf(c, "k %%= %d; r = k ? (%s)((a << k) | (a >> (%d - k))) : a; cf = r & 1; of = ((r >> %d) & 1) ^ cf;", bits, u, bits, bits - 1);
            else
                body = sf(c, "k %%= %d; r = k ? (%s)((a >> k) | (a << (%d - k))) : a; cf = (r >> %d) & 1; of = cf ^ ((r >> %d) & 1);", bits, u, bits, bits - 1, bits - 2);
            ef(c, "{ %s a = %s, r; uint32_t k = %s; if (k) { %s %s } }", u, a, cnt, body, wr(c, d, "r", 0)); return 1;
        } else {
            if (strcmp(m, "rcl") == 0)
                body = sf(c, "for (uint32_t j = 0; j < k; j++) { uint32_t nc = (r >> %d) & 1; r = (%s)((r << 1) | cf); cf = nc; } if (k == 1) of = ((r >> %d) & 1) ^ cf;", bits - 1, u, bits - 1);
            else
                body = sf(c, "if (k == 1) of = ((r >> %d) & 1) ^ cf; for (uint32_t j = 0; j < k; j++) { uint32_t nc = r & 1; r = (%s)((r >> 1) | ((%s)cf << %d)); cf = nc; }", bits - 1, u, W, bits - 1);
            ef(c, "{ %s r = %s; uint32_t k = %s; %s %s }", u, a, cnt, body, wr(c, d, "r", 0)); return 1;
        }
        const char *z = szf(c, "r", n), *w = wr(c, d, "r", 0);
        ef(c, "{ %s a = %s, r; uint32_t k = %s; if (k) { %s %s %s } }", u, a, cnt, body, z, w);
        return 1;
    }
    if (IN(m, "shld", "shrd")) {
        const cs_x86_op *d = &ops[0], *s = &ops[1]; int64_t n = d->size; BITS(c, n);
        if (n != 4) unsup(c, "%s16", m);
        const char *cnt = ops[2].type == X86_OP_IMM ? sf(c, "%lld", (long long)(ops[2].imm & 31)) : "(uint32_t)(C.ecx & 31)";
        const char *body = strcmp(m, "shld") == 0 ? "r = (a << k) | (b >> (32 - k)); cf = (a >> (32 - k)) & 1;"
                                                    : "r = (a >> k) | (b << (32 - k)); cf = (a >> (k - 1)) & 1;";
        const char *a = rd(c, d, 0), *b = rd(c, s, n), *z = szf(c, "r", n), *w = wr(c, d, "r", 0);
        ef(c, "{ uint32_t a = %s, b = %s, r; uint32_t k = %s; if (k) { %s %s %s } }", a, b, cnt, body, z, w);
        return 1;
    }
    if (IN(m, "bt", "bts", "btr", "btc")) {
        if (nops != 2) pyerr(c, "not enough values to unpack");
        const cs_x86_op *d = &ops[0], *s = &ops[1]; int64_t n = d->size;
        if (d->type == X86_OP_MEM && s->type == X86_OP_REG) {
            const char *ad = ea(c, d), *sg = Sg(c, n), *v = rd(c, s, 0);
            const char *tl = strcmp(m, "bt") == 0 ? "" : strcmp(m, "bts") == 0 ? "wr8(a, v | (1u << k));" : strcmp(m, "btr") == 0 ? "wr8(a, v & ~(1u << k));" : "wr8(a, v ^ (1u << k));";
            ef(c, "{ int32_t o = (%s)%s; uint32_t a = %s + (uint32_t)(o >> 3); uint8_t v = rd8(a), k = o & 7; cf = (v >> k) & 1; %s }", sg, v, ad, tl);
            return 1;
        }
        const char *sv = s->type != X86_OP_IMM ? rd(c, s, n) : sf(c, "%lld", (long long)s->imm);
        const char *k = sf(c, "(%s & %d)", sv, BITS(c, n) - 1);
        const char *op = strcmp(m, "bt") == 0 ? NULL : strcmp(m, "bts") == 0 ? "v | b" : strcmp(m, "btr") == 0 ? "v & ~b" : "v ^ b";
        const char *u = U(c, n), *dv = rd(c, d, 0);
        ef(c, "{ %s v = %s, b = (%s)(1u << %s); cf = (v & b) != 0; %s }", u, dv, u, k, op ? wr(c, d, op, 0) : "");
        return 1;
    }
    if (IN(m, "bsf", "bsr", "tzcnt", "lzcnt", "popcnt")) {
        const cs_x86_op *d = &ops[0], *s = &ops[1]; int64_t n = d->size;
        const char *v = sf(c, "(uint32_t)%s", rd(c, s, 0));
        if (strcmp(m, "bsf") == 0) ef(c, "{ uint32_t v = %s; zf = v == 0; if (v) %s }", v, wr(c, d, "__builtin_ctz(v)", 0));
        else if (strcmp(m, "bsr") == 0) ef(c, "{ uint32_t v = %s; zf = v == 0; if (v) %s }", v, wr(c, d, "31 - __builtin_clz(v)", 0));
        else if (strcmp(m, "popcnt") == 0) ef(c, "{ uint32_t v = %s; %s zf = v == 0; cf = 0; }", v, wr(c, d, "__builtin_popcount(v)", 0));
        else if (strcmp(m, "tzcnt") == 0) { int b = BITS(c, n); ef(c, "{ uint32_t v = %s; uint32_t r = v ? (uint32_t)__builtin_ctz(v) : %d; cf = v == 0; zf = r == 0; %s }", v, b, wr(c, d, "r", 0)); }
        else { int b = BITS(c, n); ef(c, "{ uint32_t v = %s; uint32_t r = v ? (uint32_t)__builtin_clz(v) - %d : %d; cf = v == 0; zf = r == 0; %s }", v, 32 - b, b, wr(c, d, "r", 0)); }
        return 1;
    }
    if (strcmp(m, "bswap") == 0) { const cs_x86_op *d = &ops[0]; e(c, wr(c, d, sf(c, "__builtin_bswap32(%s)", rd(c, d, 0)), 0)); return 1; }
    if (strcmp(m, "xchg") == 0) {
        const cs_x86_op *a = &ops[0], *b = &ops[1];
        if (a->type == X86_OP_MEM || b->type == X86_OP_MEM) {
            const cs_x86_op *mem = a->type == X86_OP_MEM ? a : b, *reg = a->type == X86_OP_MEM ? b : a;
            const char *u = U(c, mem->size), *ad = ea(c, mem), *rv = rd(c, reg, 0), *w = wr(c, reg, "v", 0);
            ef(c, "{ %s v = __atomic_exchange_n((%s *)G2H(%s), %s, __ATOMIC_SEQ_CST); %s }", u, u, ad, rv, w);
        } else {
            const char *u = U(c, a->size), *av = rd(c, a, 0), *w1 = wr(c, a, rd(c, b, 0), 0), *w2 = wr(c, b, "t", 0);
            ef(c, "{ %s t = %s; %s %s }", u, av, w1, w2);
        }
        return 1;
    }
    if (IN(m, "imul", "mul", "div", "idiv")) return muldiv(c, m, in);
    return 0;
}

static int muldiv(Ctx *c, const char *m, const Ins *in) {
    const cs_x86_op *ops = in->ops;
    if (strcmp(m, "imul") == 0 && in->nops >= 2) {
        const cs_x86_op *d = &ops[0]; int64_t n = d->size;
        const char *a = in->nops == 3 ? rd(c, &ops[1], n) : rd(c, d, 0);
        const char *b = in->nops == 3 ? rd(c, &ops[2], n) : rd(c, &ops[1], n);
        const char *s = Sg(c, n), *u = U(c, n), *z = szf(c, "r", n), *w = wr(c, d, "r", 0);
        ef(c, "{ int64_t p = (int64_t)(%s)%s * (int64_t)(%s)%s; %s r = (%s)p; cf = of = p != (int64_t)(%s)r; %s %s }", s, a, s, b, u, u, s, z, w);
        return 1;
    }
    const cs_x86_op *s = &ops[0]; int64_t n = s->size; const char *v = rd(c, s, 0);
    if (n == 4) {
        if (strcmp(m, "mul") == 0) ef(c, "{ uint64_t p = (uint64_t)C.eax * %s; C.eax = (uint32_t)p; C.edx = (uint32_t)(p >> 32); cf = of = C.edx != 0; }", v);
        else if (strcmp(m, "imul") == 0) ef(c, "{ int64_t p = (int64_t)(int32_t)C.eax * (int32_t)%s; C.eax = (uint32_t)p; C.edx = (uint32_t)((uint64_t)p >> 32); cf = of = p != (int32_t)p; }", v);
        else if (strcmp(m, "div") == 0) ef(c, "{ uint64_t nn = ((uint64_t)C.edx << 32) | C.eax; uint32_t dd = %s; C.eax = (uint32_t)(nn / dd); C.edx = (uint32_t)(nn %% dd); }", v);
        else ef(c, "{ int64_t nn = (int64_t)(((uint64_t)C.edx << 32) | C.eax); int32_t dd = (int32_t)%s; C.eax = (uint32_t)(int32_t)(nn / dd); C.edx = (uint32_t)(int32_t)(nn %% dd); }", v);
    } else if (n == 2) {
        if (IN(m, "mul", "imul")) {
            const char *t = strcmp(m, "mul") == 0 ? "uint32_t" : "int32_t", *c16 = strcmp(m, "mul") == 0 ? "uint16_t" : "int16_t";
            const char *ov = strcmp(m, "mul") == 0 ? "(C.edx & 0xFFFF) != 0" : "p != (int16_t)p";
            ef(c, "{ %s p = (%s)(%s)C.eax * (%s)%s; C.eax = (C.eax & ~0xFFFFu) | (uint16_t)p; C.edx = (C.edx & ~0xFFFFu) | (uint16_t)((uint32_t)p >> 16); cf = of = %s; }", t, t, c16, c16, v, ov);
        } else {
            const char *t = strcmp(m, "div") == 0 ? "uint32_t" : "int32_t", *c16 = strcmp(m, "div") == 0 ? "uint16_t" : "int16_t";
            ef(c, "{ %s nn = (%s)(((uint32_t)(uint16_t)C.edx << 16) | (uint16_t)C.eax); %s dd = (%s)%s; C.eax = (C.eax & ~0xFFFFu) | (uint16_t)(nn / dd); C.edx = (C.edx & ~0xFFFFu) | (uint16_t)(nn %% dd); }", t, t, c16, c16, v);
        }
    } else {
        if (IN(m, "mul", "imul")) {
            const char *t = strcmp(m, "mul") == 0 ? "uint32_t" : "int32_t", *c8 = strcmp(m, "mul") == 0 ? "uint8_t" : "int8_t";
            const char *ov = strcmp(m, "mul") == 0 ? "(p >> 8) != 0" : "p != (int8_t)p";
            ef(c, "{ %s p = (%s)(%s)C.eax * (%s)%s; C.eax = (C.eax & ~0xFFFFu) | (uint16_t)p; cf = of = %s; }", t, t, c8, c8, v, ov);
        } else {
            const char *t = strcmp(m, "div") == 0 ? "uint32_t" : "int32_t", *c8 = strcmp(m, "div") == 0 ? "uint8_t" : "int8_t";
            ef(c, "{ %s nn = (%s)(%s)C.eax; %s dd = (%s)%s; C.eax = (C.eax & ~0xFFFFu) | (uint8_t)(nn / dd) | ((uint16_t)(uint8_t)(nn %% dd) << 8); }", t, t, strcmp(m, "div") == 0 ? "uint16_t" : "int16_t", c8, c8, v);
        }
    }
    return 1;
}

// ---------------- string ops ----------------

static int strop(Ctx *c, const char *m, const Ins *in) {
    static const char *const reps[] = {"rep ", "repe ", "repne ", "repz ", "repnz "};
    static const char *const bases[] = {"movs", "stos", "lods", "cmps", "scas"};
    const char *rep = NULL, *op = NULL; char w = 0;
    for (int r = -1; r < 5 && !op; r++) {
        const char *s = m;
        if (r >= 0) { if (!starts(m, reps[r])) continue; s = m + strlen(reps[r]); }
        for (int b = 0; b < 5; b++)
            if (starts(s, bases[b]) && strchr("bwd", s[4]) && s[4] && !s[5]) { rep = r >= 0 ? reps[r] : NULL; op = bases[b]; w = s[4]; break; }
    }
    if (!op) return 0;
    for (int k = 0; k < in->nops; k++) if (isx(c, &in->ops[k])) return 0;
    int n = w == 'b' ? 1 : w == 'w' ? 2 : 4;
    const char *T = U(c, n);
    const char *st = sf(c, "(df ? -%d : %d)", n, n);
    const char *R = RD(c, n), *Wf = WR(c, n);
    if (strcmp(op, "movs") == 0 && rep) {
        ef(c, "if (!df) { uint32_t k = C.ecx, s_ = C.esi, d_ = C.edi; if (d_ > s_ && d_ < s_ + k * %d) { for (uint32_t j = 0; j < k; j++) %s(d_ + j * %d, %s(s_ + j * %d)); }"
              " else memmove(G2H(d_), G2H(s_), (size_t)k * %d); C.esi += k * %d; C.edi += k * %d; C.ecx = 0; }"
              " else { while (C.ecx) { %s(C.edi, %s(C.esi)); C.esi -= %d; C.edi -= %d; C.ecx--; } }", n, Wf, n, R, n, n, n, n, Wf, R, n, n);
        return 1;
    }
    if (strcmp(op, "stos") == 0 && rep) {
        if (n == 1) e(c, "if (!df) { uint32_t k = C.ecx; memset(G2H(C.edi), (uint8_t)C.eax, k); C.edi += k; C.ecx = 0; } else { while (C.ecx) { wr8(C.edi, (uint8_t)C.eax); C.edi--; C.ecx--; } }");
        else ef(c, "{ uint32_t k = C.ecx; %s v = (%s)C.eax; for (uint32_t j = 0; j < k; j++) { %s(C.edi, v); C.edi += %s; } C.ecx = 0; }", T, T, Wf, st);
        return 1;
    }
    if (strcmp(op, "movs") == 0) { ef(c, "%s(C.edi, %s(C.esi)); C.esi += %s; C.edi += %s;", Wf, R, st, st); return 1; }
    if (strcmp(op, "stos") == 0) { ef(c, "%s(C.edi, (%s)C.eax); C.edi += %s;", Wf, T, st); return 1; }
    if (strcmp(op, "lods") == 0) {
        const char *reg = n == 1 ? "al" : n == 2 ? "ax" : "eax";
        const char *one = sf(c, "{ %s v = %s(C.esi); C.esi += %s; %s }", T, R, st, wr_name(c, reg, "v"));
        if (rep) ef(c, "while (C.ecx) { %s C.ecx--; }", one);
        else e(c, one);
        return 1;
    }
    const char *lhs = strcmp(op, "cmps") == 0 ? sf(c, "%s(C.esi)", R) : sf(c, "(%s)C.eax", T);
    const char *adv = strcmp(op, "cmps") == 0 ? sf(c, "C.esi += %s; C.edi += %s;", st, st) : sf(c, "C.edi += %s;", st);
    const char *z = szf(c, "r", n);
    const char *one = sf(c, "{ %s a = %s, b = %s(C.edi), r = (%s)(a - b); cf = a < b; of = (((a ^ b) & (a ^ r)) >> %d) & 1; %s %s }", T, lhs, R, T, BITS(c, n) - 1, z, adv);
    if (!rep) { e(c, one); return 1; }
    char rs[8]; snprintf(rs, sizeof rs, "%s", rep); rs[strlen(rs) - 1] = 0;
    const char *cond = IN(rs, "repe", "repz", "rep") ? "zf" : "!zf";
    ef(c, "while (C.ecx) { %s C.ecx--; if (!(%s)) break; }", one, cond);
    return 1;
}

// ---------------- lock-prefixed atomics ----------------

static int atomic(Ctx *c, const char *m, const Ins *in) {
    if (!starts(m, "lock ") && !IN(m, "cmpxchg", "xadd", "cmpxchg8b")) return 0;
    const char *base = starts(m, "lock ") ? m + 5 : m;
    const cs_x86_op *ops = in->ops;
    const cs_x86_op *d = &ops[0]; int64_t n = d->size;
    const char *T = U(c, n);
    if (strcmp(base, "cmpxchg8b") == 0) {
        const char *p = sf(c, "(uint64_t *)G2H(%s)", ea(c, d));
        ef(c, "{ uint64_t exp = ((uint64_t)C.edx << 32) | C.eax, nv = ((uint64_t)C.ecx << 32) | C.ebx; zf = __atomic_compare_exchange_n(%s, &exp, nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); if (!zf) { C.eax = (uint32_t)exp; C.edx = (uint32_t)(exp >> 32); } }", p);
        return 1;
    }
    if (d->type != X86_OP_MEM) {
        if (strcmp(base, "xadd") == 0) {
            const char *a = rd(c, d, 0), *b = rd(c, &ops[1], 0), *w1 = wr(c, &ops[1], "a", 0), *w2 = wr(c, d, "r", 0), *z = szf(c, "r", n);
            ef(c, "{ %s a = %s, b = %s, r = (%s)(a + b); %s %s cf = r < a; %s }", T, a, b, T, w1, w2, z);
            return 1;
        }
        if (strcmp(base, "cmpxchg") == 0) {
            const char *acc = n == 1 ? "al" : n == 2 ? "ax" : n == 4 ? "eax" : NULL;
            if (!acc) pyerr(c, "%lld", (long long)n);
            const char *cur = rd(c, d, 0); int bits = BITS(c, n);
            const char *z = szf(c, "r", n), *w = wr(c, d, rd(c, &ops[1], 0), 0), *wn = wr_name(c, acc, "cur");
            ef(c, "{ %s cur = %s, exp = (%s)C.eax, r = (%s)(exp - cur); cf = exp < cur; of = (((exp ^ cur) & (exp ^ r)) >> %d) & 1; %s if (zf) { %s } else { %s } }",
               T, cur, T, T, bits - 1, z, w, wn);
            return 1;
        }
        unsup(c, "%s", m);
    }
    const char *p = sf(c, "(%s *)G2H(%s)", T, ea(c, d));
    if (strcmp(base, "xadd") == 0) {
        const char *b = rd(c, &ops[1], 0), *w = wr(c, &ops[1], "a", 0); int bits = BITS(c, n); const char *z = szf(c, "r", n);
        ef(c, "{ %s b = %s, a = __atomic_fetch_add(%s, b, __ATOMIC_SEQ_CST), r = (%s)(a + b); %s cf = r < a; of = ((~(a ^ b) & (a ^ r)) >> %d) & 1; %s }", T, b, p, T, w, bits - 1, z);
        return 1;
    }
    if (strcmp(base, "cmpxchg") == 0) {
        const char *acc = n == 1 ? "al" : n == 2 ? "ax" : n == 4 ? "eax" : NULL;
        if (!acc) pyerr(c, "%lld", (long long)n);
        const char *nv = rd(c, &ops[1], 0);
        const char *nok = sf(c, "if (!ok) { %s }", wr_name(c, acc, "exp"));
        int bits = BITS(c, n); const char *z = szf(c, "r", n);
        ef(c, "{ %s exp = (%s)C.eax, acc = exp, r; int ok = __atomic_compare_exchange_n(%s, &exp, %s, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); %s r = (%s)(acc - exp); cf = acc < exp; of = (((acc ^ exp) & (acc ^ r)) >> %d) & 1; %s zf = ok; }",
           T, T, p, nv, nok, T, bits - 1, z);
        return 1;
    }
    if (IN(base, "inc", "dec", "add", "sub", "or", "and", "xor")) {
        const char *v = IN(base, "inc", "dec") ? "1" : rd(c, &ops[1], n);
        const char *fn = strcmp(base, "inc") == 0 ? "add" : strcmp(base, "dec") == 0 ? "sub" : base;
        const char *op = strcmp(fn, "add") == 0 ? "+" : strcmp(fn, "sub") == 0 ? "-" : strcmp(fn, "or") == 0 ? "|" : strcmp(fn, "and") == 0 ? "&" : "^";
        const char *fl;
        if (strcmp(base, "add") == 0) fl = sf(c, "cf = r < a; of = ((~(a ^ b) & (a ^ r)) >> %d) & 1;", BITS(c, n) - 1);
        else if (strcmp(base, "sub") == 0) fl = sf(c, "cf = a < b; of = (((a ^ b) & (a ^ r)) >> %d) & 1;", BITS(c, n) - 1);
        else if (IN(fn, "or", "and", "xor")) fl = "of = 0; cf = 0;";
        else if (strcmp(base, "inc") == 0) fl = sf(c, "of = r == (%s)((%s)1 << %d);", T, T, BITS(c, n) - 1);
        else fl = sf(c, "of = a == (%s)((%s)1 << %d);", T, T, BITS(c, n) - 1);
        const char *z = szf(c, "r", n);
        ef(c, "{ %s b = (%s)%s, a = __atomic_fetch_%s(%s, b, __ATOMIC_SEQ_CST), r = (%s)(a %s b); %s %s }", T, T, v, fn, p, T, op, z, fl);
        return 1;
    }
    if (IN(base, "bts", "btr", "btc")) {
        const char *sv = ops[1].type != X86_OP_IMM ? rd(c, &ops[1], n) : sf(c, "%lld", (long long)ops[1].imm);
        const char *k = sf(c, "(%s & %d)", sv, BITS(c, n) - 1);
        const char *fn = strcmp(base, "bts") == 0 ? "or" : strcmp(base, "btr") == 0 ? "and" : "xor";
        const char *mk = strcmp(base, "btr") == 0 ? sf(c, "(%s)~(1u << %s)", T, k) : sf(c, "(%s)(1u << %s)", T, k);
        ef(c, "{ %s a = __atomic_fetch_%s(%s, %s, __ATOMIC_SEQ_CST); cf = (a >> %s) & 1; }", T, fn, p, mk, k);
        return 1;
    }
    if (strcmp(base, "xchg") == 0) return alu(c, "xchg", in);
    unsup(c, "%s", m);
}

// ---------------- scalar SSE / moves ----------------

static int vec(Ctx *c, const char *m, const Ins *in, int nops);
static const char *src32(Ctx *c, const cs_x86_op *op) { return isx(c, op) ? sf(c, "%s.f[0]", xmm(c, op)) : sf(c, "rdf(%s)", ea(c, op)); }
static const char *src64(Ctx *c, const cs_x86_op *op) { return isx(c, op) ? sf(c, "%s.d[0]", xmm(c, op)) : sf(c, "rdd(%s)", ea(c, op)); }
static const char *x128(Ctx *c, const cs_x86_op *op) { return isx(c, op) ? xmm(c, op) : sf(c, "rdx(%s)", ea(c, op)); }

// m == prefix + one of a + one of b (a and b NULL-terminated); the indices of the parts
static int match3(const char *m, const char *prefix, const char *const *a, const char *const *b, int *ia, int *ib) {
    if (!starts(m, prefix)) return 0;
    const char *s = m + strlen(prefix);
    for (int i = 0; a[i]; i++) {
        if (!starts(s, a[i])) continue;
        const char *r = s + strlen(a[i]);
        for (int j = 0; b[j]; j++) if (strcmp(r, b[j]) == 0) { *ia = i; *ib = j; return 1; }
    }
    return 0;
}
#define L(...) ((const char *const[]){__VA_ARGS__, NULL})

static int sse(Ctx *c, const char *m, const Ins *in, int nops) {
    const cs_x86_op *ops = in->ops;
    int ia, ib;
    if (IN(m, "movaps", "movups", "movdqa", "movdqu", "movapd", "movupd", "movntps", "movntdq", "movntpd", "lddqu", "movntdqa")) {
        const cs_x86_op *d = &ops[0], *s = &ops[1];
        if (isx(c, d)) { const char *x = xmm(c, d); ef(c, "%s = %s;", x, x128(c, s)); }
        else { const char *a = ea(c, d); ef(c, "wrx(%s, %s);", a, xmm(c, s)); }
        return 1;
    }
    if (IN(m, "movss", "movsd") && nops == 2 && (isx(c, &ops[0]) || isx(c, &ops[1]))) {
        const cs_x86_op *d = &ops[0], *s = &ops[1];
        const char *f = strcmp(m, "movss") == 0 ? "f" : "d";
        if (isx(c, d) && isx(c, s)) { const char *a = xmm(c, d), *b = xmm(c, s); ef(c, "%s.%s[0] = %s.%s[0];", a, f, b, f); }
        else if (isx(c, d)) {
            if (strcmp(m, "movss") == 0) { const char *a = ea(c, s); ef(c, "{ XMM x = {0}; x.f[0] = rdf(%s); %s = x; }", a, xmm(c, d)); }
            else { const char *a = ea(c, s); ef(c, "{ XMM x = {0}; x.d[0] = rdd(%s); %s = x; }", a, xmm(c, d)); }
        } else { const char *a = ea(c, d); ef(c, "%s(%s, %s.%s[0]);", strcmp(m, "movss") == 0 ? "wrf" : "wrd", a, xmm(c, s), f); }
        return 1;
    }
    if (IN(m, "movd", "movq") && (isx(c, &ops[0]) || isx(c, &ops[1]))) {
        const cs_x86_op *d = &ops[0], *s = &ops[1];
        if (isx(c, d) && isx(c, s)) { const char *b = xmm(c, s); ef(c, "{ XMM x = {0}; x.q[0] = %s.q[0]; %s = x; }", b, xmm(c, d)); return 1; }
        if (isx(c, d)) { const char *v = rd(c, s, 0); ef(c, "{ XMM x = {0}; x.q[0] = (uint64_t)%s; %s = x; }", v, xmm(c, d)); return 1; }
        e(c, wr(c, d, sf(c, "%s.%s[0]", xmm(c, s), d->size == 8 ? "q" : "u"), 0)); return 1;
    }
    if (IN(m, "movlps", "movlpd", "movhps", "movhpd")) {
        const cs_x86_op *d = &ops[0], *s = &ops[1]; int k = starts(m, "movl") ? 0 : 1;
        if (isx(c, d)) { const char *x = xmm(c, d); ef(c, "%s.q[%d] = rd64(%s);", x, k, ea(c, s)); }
        else { const char *a = ea(c, d); ef(c, "wr64(%s, %s.q[%d]);", a, xmm(c, s), k); }
        return 1;
    }
    if (IN(m, "movmskps", "movmskpd")) {
        const cs_x86_op *d = &ops[0], *s = &ops[1]; const char *X_ = xmm(c, s);
        if (strcmp(m, "movmskps") == 0) e(c, wr(c, d, sf(c, "(%s.u[0] >> 31) | ((%s.u[1] >> 31) << 1) | ((%s.u[2] >> 31) << 2) | ((%s.u[3] >> 31) << 3)", X_, X_, X_, X_), 4));
        else e(c, wr(c, d, sf(c, "(uint32_t)((%s.q[0] >> 63) | ((%s.q[1] >> 63) << 1))", X_, X_), 4));
        return 1;
    }
    if (strcmp(m, "pmovmskb") == 0 && isx(c, &ops[1])) {
        const cs_x86_op *d = &ops[0], *s = &ops[1]; const char *X_ = xmm(c, s);
        ef(c, "{ uint32_t r = 0; for (int k = 0; k < 16; k++) r |= (uint32_t)(%s.b[k] >> 7) << k; %s }", X_, wr(c, d, "r", 4)); return 1;
    }
    static const char *const ar_k[] = {"add", "sub", "mul", "div"}, *const ar_v[] = {"+", "-", "*", "/"};
    if (match3(m, "", L("add", "sub", "mul", "div", "min", "max", "sqrt"), L("ss", "sd"), &ia, &ib)) {
        const cs_x86_op *d = &ops[0], *s = &ops[1];
        const char *f = ib == 0 ? "f" : "d"; const char *v = ib == 0 ? src32(c, s) : src64(c, s);
        const char *D = sf(c, "%s.%s[0]", xmm(c, d), f);
        if (ia < 4) ef(c, "%s = %s %s %s;", D, D, ar_v[ia], v);
        else if (ia == 4) ef(c, "{ __typeof__(%s) b = %s; %s = %s < b ? %s : b; }", D, v, D, D, D);
        else if (ia == 5) ef(c, "{ __typeof__(%s) b = %s; %s = %s > b ? %s : b; }", D, v, D, D, D);
        else ef(c, "%s = %s(%s);", D, ib == 0 ? "sqrtf" : "sqrt", v);
        return 1;
    }
    (void)ar_k;
    if (match3(m, "", L("add", "sub", "mul", "div"), L("ps", "pd"), &ia, &ib)) {
        const cs_x86_op *d = &ops[0], *s = &ops[1];
        const char *v = ib == 0 ? "vf" : "vd";
        const char *a = x128(c, s), *x = xmm(c, d);
        ef(c, "{ XMM b = %s, a = %s; a.%s = a.%s %s b.%s; %s = a; }", a, x, v, v, ar_v[ia], v, x);
        return 1;
    }
    if (IN(m, "andps", "andpd", "pand", "orps", "orpd", "por", "xorps", "xorpd", "pxor", "andnps", "andnpd", "pandn")) {
        const cs_x86_op *d = &ops[0], *s = &ops[1];
        if (!isx(c, d)) return 0;
        if (IN(m, "xorps", "xorpd", "pxor") && isx(c, s) && s->reg == d->reg) { ef(c, "%s = (XMM){0};", xmm(c, d)); return 1; }
        const char *ex;
        if (strstr(m, "andn")) ex = "~a.vq & b.vq";
        else ex = sf(c, "a.vq %s b.vq", strstr(m, "xor") ? "^" : strstr(m, "and") ? "&" : "|");
        const char *a = x128(c, s), *x = xmm(c, d);
        ef(c, "{ XMM b = %s, a = %s; a.vq = %s; %s = a; }", a, x, ex, x);
        return 1;
    }
    if (IN(m, "comiss", "ucomiss", "comisd", "ucomisd")) {
        const cs_x86_op *a = &ops[0], *b = &ops[1];
        const char *va = sf(c, "%s.%s[0]", xmm(c, a), ends(m, "ss") ? "f" : "d");
        const char *vb = ends(m, "ss") ? src32(c, b) : src64(c, b);
        ef(c, "{ double a = %s, b = %s; if (a != a || b != b) { zf = cf = 1; pr = 0; } else { zf = a == b; cf = a < b; pr = 1; } of = 0; sf = 0; }", va, vb);
        return 1;
    }
    if (strcmp(m, "cvtss2sd") == 0) { const char *x = xmm(c, &ops[0]); ef(c, "%s.d[0] = (double)%s;", x, src32(c, &ops[1])); return 1; }
    if (strcmp(m, "cvtsd2ss") == 0) { const char *x = xmm(c, &ops[0]); ef(c, "%s.f[0] = (float)%s;", x, src64(c, &ops[1])); return 1; }
    if (IN(m, "cvtsi2ss", "cvtsi2sd")) {
        const cs_x86_op *d = &ops[0], *s = &ops[1];
        int f = strcmp(m, "cvtsi2ss") == 0;
        const char *x = xmm(c, d);
        ef(c, "%s.%s[0] = (%s)(int32_t)%s;", x, f ? "f" : "d", f ? "float" : "double", rd(c, s, 4));
        return 1;
    }
    if (match3(m, "cvt", L("t", ""), L("ss2si", "sd2si"), &ia, &ib)) {
        const cs_x86_op *d = &ops[0], *s = &ops[1];
        const char *v = ib == 0 ? src32(c, s) : src64(c, s);
        v = ia == 0 ? sf(c, "(double)(%s)", v) : sf(c, "sse_round(c, %s)", v);
        e(c, wr(c, d, sf(c, "(uint32_t)cvt_i32(%s)", v), 0));
        return 1;
    }
    static const char *const preds[] = {"eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord", NULL};
    if (match3(m, "cmp", preds, L("ss", "sd"), &ia, &ib)) {
        const cs_x86_op *d = &ops[0], *s = &ops[1];
        static const char *const C_[] = {"a == b", "a < b", "a <= b", "a != a || b != b", "!(a == b)", "!(a < b)", "!(a <= b)", "a == a && b == b"};
        if (ib == 0) { const char *x = xmm(c, d), *b = src32(c, s); ef(c, "{ float a = %s.f[0], b = %s; %s.u[0] = (%s) ? 0xFFFFFFFFu : 0; }", x, b, x, C_[ia]); }
        else { const char *x = xmm(c, d), *b = src64(c, s); ef(c, "{ double a = %s.d[0], b = %s; %s.q[0] = (%s) ? ~0ull : 0; }", x, b, x, C_[ia]); }
        return 1;
    }
    if (IN(m, "cmpss", "cmpsd", "cmpps", "cmppd") && nops == 3 && isx(c, &ops[0])) {
        const char *pred = preds[ops[2].imm & 7];
        const char *nm = sf(c, "cmp%s%s", pred, m + 3);
        return sse(c, nm, in, 2) || vec(c, nm, in, 2);
    }
    if (IN(m, "rsqrtss", "rcpss")) {
        const char *v = src32(c, &ops[1]);
        const char *x = xmm(c, &ops[0]);
        ef(c, "%s.f[0] = %s;", x, strcmp(m, "rsqrtss") == 0 ? sf(c, "1.0f / sqrtf(%s)", v) : sf(c, "1.0f / (%s)", v)); return 1;
    }
    if (IN(m, "roundss", "roundsd", "roundps", "roundpd")) {
        if (nops != 3) pyerr(c, "not enough values to unpack");
        const cs_x86_op *d = &ops[0], *s = &ops[1], *im = &ops[2];
        static const char *const rr[] = {"nearbyint", "floor", "ceil", "trunc"};
        const char *r = (im->imm & 4) ? "nearbyint" : rr[im->imm & 3];
        if (strcmp(m, "roundss") == 0) { const char *x = xmm(c, d); ef(c, "%s.f[0] = (float)%s(%s);", x, r, src32(c, s)); }
        else if (strcmp(m, "roundsd") == 0) { const char *x = xmm(c, d); ef(c, "%s.d[0] = %s(%s);", x, r, src64(c, s)); }
        else if (strcmp(m, "roundps") == 0) { const char *b = x128(c, s); ef(c, "{ XMM b = %s; for (int k = 0; k < 4; k++) b.f[k] = (float)%s(b.f[k]); %s = b; }", b, r, xmm(c, d)); }
        else { const char *b = x128(c, s); ef(c, "{ XMM b = %s; for (int k = 0; k < 2; k++) b.d[k] = %s(b.d[k]); %s = b; }", b, r, xmm(c, d)); }
        return 1;
    }
    if (strcmp(m, "stmxcsr") == 0) { e(c, wr(c, &ops[0], "C.mxcsr", 4)); return 1; }
    if (strcmp(m, "ldmxcsr") == 0) { ef(c, "C.mxcsr = %s;", rd(c, &ops[0], 4)); return 1; }
    return 0;
}

typedef struct { const char *m, *fl; int n; const char *ex; } Lane;
static const Lane LANES[] = {
    {"paddb", "b", 16, "a + b"}, {"paddw", "w", 8, "a + b"}, {"paddd", "u", 4, "a + b"}, {"paddq", "q", 2, "a + b"},
    {"psubb", "b", 16, "a - b"}, {"psubw", "w", 8, "a - b"}, {"psubd", "u", 4, "a - b"}, {"psubq", "q", 2, "a - b"},
    {"pmullw", "h", 8, "(int16_t)(a * b)"}, {"pmulhw", "h", 8, "(int16_t)(((int32_t)a * b) >> 16)"},
    {"pmulhuw", "w", 8, "(uint16_t)(((uint32_t)a * b) >> 16)"}, {"pmulld", "i", 4, "(int32_t)((int64_t)a * b)"},
    {"paddsw", "h", 8, "(int16_t)(a + b > 32767 ? 32767 : a + b < -32768 ? -32768 : a + b)"},
    {"psubsw", "h", 8, "(int16_t)(a - b > 32767 ? 32767 : a - b < -32768 ? -32768 : a - b)"},
    {"paddsb", "c", 16, "(int8_t)(a + b > 127 ? 127 : a + b < -128 ? -128 : a + b)"},
    {"psubsb", "c", 16, "(int8_t)(a - b > 127 ? 127 : a - b < -128 ? -128 : a - b)"},
    {"paddusb", "b", 16, "(uint8_t)(a + b > 255 ? 255 : a + b)"}, {"paddusw", "w", 8, "(uint16_t)(a + b > 65535 ? 65535 : a + b)"},
    {"psubusb", "b", 16, "(uint8_t)(a > b ? a - b : 0)"}, {"psubusw", "w", 8, "(uint16_t)(a > b ? a - b : 0)"},
    {"pcmpeqb", "b", 16, "a == b ? 0xFF : 0"}, {"pcmpeqw", "w", 8, "a == b ? 0xFFFF : 0"}, {"pcmpeqd", "u", 4, "a == b ? 0xFFFFFFFFu : 0"},
    {"pcmpgtb", "c", 16, "a > b ? -1 : 0"}, {"pcmpgtw", "h", 8, "a > b ? -1 : 0"}, {"pcmpgtd", "i", 4, "a > b ? -1 : 0"},
    {"pmaxsw", "h", 8, "a > b ? a : b"}, {"pminsw", "h", 8, "a < b ? a : b"}, {"pmaxub", "b", 16, "a > b ? a : b"},
    {"pminub", "b", 16, "a < b ? a : b"},
    {"pavgb", "b", 16, "(uint8_t)((a + b + 1) >> 1)"}, {"pavgw", "w", 8, "(uint16_t)((a + b + 1) >> 1)"},
    {"minps", "f", 4, "a < b ? a : b"}, {"maxps", "f", 4, "a > b ? a : b"}, {"minpd", "d", 2, "a < b ? a : b"}, {"maxpd", "d", 2, "a > b ? a : b"},
    {"sqrtps", "f", 4, "sqrtf(b)"}, {"sqrtpd", "d", 2, "sqrt(b)"}, {"rcpps", "f", 4, "1.0f / b"}, {"rsqrtps", "f", 4, "1.0f / sqrtf(b)"},
};
static const Lane VLANES[] = {
    {"paddb", "vb", 0, "a + b"}, {"paddw", "vw", 0, "a + b"}, {"paddd", "vu", 0, "a + b"}, {"paddq", "vq", 0, "a + b"},
    {"psubb", "vb", 0, "a - b"}, {"psubw", "vw", 0, "a - b"}, {"psubd", "vu", 0, "a - b"}, {"psubq", "vq", 0, "a - b"},
    {"pmullw", "vw", 0, "a * b"}, {"pmulld", "vu", 0, "a * b"},
    {"pcmpeqb", "vb", 0, "(v16b)(a == b)"}, {"pcmpeqw", "vw", 0, "(v8w)(a == b)"}, {"pcmpeqd", "vu", 0, "(v4u)(a == b)"},
    {"pcmpgtb", "vc", 0, "(v16c)(a > b)"}, {"pcmpgtw", "vh", 0, "(v8h)(a > b)"}, {"pcmpgtd", "vi", 0, "(v4i)(a > b)"},
    {"pmaxsw", "vh", 0, "VSEL(a > b, a, b)"}, {"pminsw", "vh", 0, "VSEL(a < b, a, b)"}, {"pmaxub", "vb", 0, "VSEL(a > b, a, b)"},
    {"pminub", "vb", 0, "VSEL(a < b, a, b)"},
    {"minps", "vf", 0, "VSEL(a < b, a, b)"}, {"maxps", "vf", 0, "VSEL(a > b, a, b)"},
    {"minpd", "vd", 0, "VSEL(a < b, a, b)"}, {"maxpd", "vd", 0, "VSEL(a > b, a, b)"},
};
static const Lane *lane(const Lane *t, size_t n, const char *m) { for (size_t i = 0; i < n; i++) if (strcmp(t[i].m, m) == 0) return &t[i]; return NULL; }

// ---------------- MMX ----------------

static const char *mmr(Ctx *c, const cs_x86_op *op) { return sf(c, "C.mm[%d]", rn(c, op->reg)[2] - '0'); }
static const char *mm_rd64(Ctx *c, const cs_x86_op *op) {
    if (ismm(c, op)) return mmr(c, op);
    if (op->type == X86_OP_IMM) return sf(c, "%lluull", (unsigned long long)op->imm);
    if (isx(c, op)) return sf(c, "%s.q[0]", xmm(c, op));
    return sf(c, "(uint64_t)%s", rd(c, op, 0));
}
static int mmx(Ctx *c, const char *m, const Ins *in) {
    if (strcmp(m, "emms") == 0) return 1;
    const cs_x86_op *ops = in->ops; int nops = in->nops;
    int any = 0;
    for (int k = 0; k < nops; k++) if (ismm(c, &ops[k])) any = 1;
    if (!any) return 0;
    const cs_x86_op *d = &ops[0];
    if (IN(m, "movd", "movq")) {
        if (ismm(c, d)) {
            const char *v = strcmp(m, "movq") == 0 || ismm(c, &ops[1]) ? mm_rd64(c, &ops[1]) : sf(c, "(uint64_t)(uint32_t)%s", rd(c, &ops[1], 4));
            ef(c, "%s = %s;", mmr(c, d), v);
        } else if (strcmp(m, "movd") == 0) e(c, wr(c, d, sf(c, "(uint32_t)%s", mmr(c, &ops[1])), 4));
        else e(c, wr(c, d, mmr(c, &ops[1]), 8));
        return 1;
    }
    if (strcmp(m, "movdq2q") == 0) { const char *a = mmr(c, d); ef(c, "%s = %s.q[0];", a, xmm(c, &ops[1])); return 1; }
    if (strcmp(m, "movq2dq") == 0) { const char *a = mmr(c, &ops[1]); ef(c, "{ XMM x = {0}; x.q[0] = %s; %s = x; }", a, xmm(c, d)); return 1; }
    if (!ismm(c, d)) unsup(c, "mmx dest %s", m);
    const char *D = mmr(c, d);
    const char *src = nops > 1 ? mm_rd64(c, &ops[1]) : NULL;
    int has_imm = nops && ops[nops - 1].type == X86_OP_IMM;
    int64_t imm = has_imm ? ops[nops - 1].imm : 0;
    if (IN(m, "pand", "por", "pxor", "pandn")) {
        const char *ex = strcmp(m, "pand") == 0 ? "a & b" : strcmp(m, "por") == 0 ? "a | b" : strcmp(m, "pxor") == 0 ? "a ^ b" : "~a & b";
        ef(c, "{ uint64_t a = %s, b = %s; %s = %s; }", D, src, D, ex); return 1;
    }
    const Lane *ln = lane(LANES, sizeof LANES / sizeof LANES[0], m);
    if (ln && m[0] == 'p' && src) {
        const char *fl = ln->fl;
        ef(c, "{ MM64 s_ = {.q = {%s}}, d_ = {.q = {%s}}, r_; for (int k = 0; k < %d; k++) { __typeof__(d_.%s[0]) a = d_.%s[k], b = s_.%s[k]; (void)a; r_.%s[k] = (%s); } %s = r_.q[0]; }",
           src, D, ln->n / 2, fl, fl, fl, fl, ln->ex, D); return 1;
    }
    int ia, ib;
    if (match3(m, "ps", L("ll", "rl", "ra"), L("w", "d", "q"), &ia, &ib)) {
        static const char *const fls[] = {"w", "u", "q"}, *const sfl[] = {"h", "i", "l"};
        static const int ns[] = {4, 2, 1}, bitss[] = {16, 32, 64};
        const char *fl = fls[ib]; int n = ns[ib], bits = bitss[ib];
        const char *cnt = has_imm ? sf(c, "%lldull", (long long)imm) : src;
        const char *ex;
        if (ia == 0) ex = sf(c, "c_ >= %d ? 0 : d_.%s[k] << c_", bits, fl);
        else if (ia == 1) ex = sf(c, "c_ >= %d ? 0 : d_.%s[k] >> c_", bits, fl);
        else ex = sf(c, "(d_.%s[k] >> (c_ >= %d ? %d : c_))", sfl[ib], bits, bits - 1);
        ef(c, "{ MM64 d_ = {.q = {%s}}; uint64_t c_ = %s; for (int k = 0; k < %d; k++) d_.%s[k] = %s; %s = d_.q[0]; }", D, cnt, n, fl, ex, D); return 1;
    }
    if (match3(m, "punpck", L("l", "h"), L("bw", "wd", "dq"), &ia, &ib) && src) {
        static const char *const fls[] = {"b", "w", "u"}; static const int ns[] = {8, 4, 2};
        const char *fl = fls[ib]; int n = ns[ib]; int base = ia == 0 ? 0 : n / 2;
        ef(c, "{ MM64 s_ = {.q = {%s}}, d_ = {.q = {%s}}, r_; for (int k = 0; k < %d; k++) { r_.%s[2*k] = d_.%s[%d+k]; r_.%s[2*k+1] = s_.%s[%d+k]; } %s = r_.q[0]; }",
           src, D, n / 2, fl, fl, base, fl, fl, base, D); return 1;
    }
    if (IN(m, "packuswb", "packsswb", "packssdw") && src) {
        const char *fi, *fo; int n, lo, hi;
        if (strcmp(m, "packuswb") == 0) { fi = "h"; fo = "b"; n = 4; lo = 0; hi = 255; }
        else if (strcmp(m, "packsswb") == 0) { fi = "h"; fo = "c"; n = 4; lo = -128; hi = 127; }
        else { fi = "i"; fo = "h"; n = 2; lo = -32768; hi = 32767; }
        const char *sat = sf(c, "(v < %d ? %d : v > %d ? %d : v)", lo, lo, hi, hi);
        ef(c, "{ MM64 s_ = {.q = {%s}}, d_ = {.q = {%s}}, r_; for (int k = 0; k < %d; k++) { int32_t v = d_.%s[k]; r_.%s[k] = %s; v = s_.%s[k]; r_.%s[%d+k] = %s; } %s = r_.q[0]; }",
           src, D, n, fi, fo, sat, fi, fo, n, sat, D); return 1;
    }
    if (strcmp(m, "pmaddwd") == 0 && src) {
        ef(c, "{ MM64 s_ = {.q = {%s}}, d_ = {.q = {%s}}, r_; for (int k = 0; k < 2; k++) r_.i[k] = (int32_t)((uint32_t)(d_.h[2*k] * s_.h[2*k]) + (uint32_t)(d_.h[2*k+1] * s_.h[2*k+1])); %s = r_.q[0]; }", src, D, D); return 1;
    }
    if (strcmp(m, "pshufw") == 0 && has_imm) {
        ef(c, "{ MM64 s_ = {.q = {%s}}, r_; for (int k = 0; k < 4; k++) r_.w[k] = s_.w[(%lld >> (2 * k)) & 3]; %s = r_.q[0]; }", src, (long long)imm, D); return 1;
    }
    unsup(c, "mmx %s", m);
}

// ---------------- packed SSE ----------------

static int64_t pymod(int64_t a, int64_t b) { int64_t r = a % b; return r && ((r < 0) != (b < 0)) ? r + b : r; }

static int vec(Ctx *c, const char *m, const Ins *in, int nops) {
    const cs_x86_op *ops = in->ops;
    if (!nops || !isx(c, &ops[0])) {
        if (strcmp(m, "pextrw") == 0 && nops == 3 && isx(c, &ops[1])) {
            e(c, wr(c, &ops[0], sf(c, "(uint32_t)%s.w[%lld]", xmm(c, &ops[1]), (long long)(ops[2].imm & 7)), 4)); return 1;
        }
        if (IN(m, "pextrd", "pextrb", "extractps") && nops == 3 && isx(c, &ops[1])) {
            const char *f = strcmp(m, "pextrb") == 0 ? "b" : "u"; int n = strcmp(m, "pextrb") == 0 ? 16 : 4;
            e(c, wr(c, &ops[0], sf(c, "%s.%s[%lld]", xmm(c, &ops[1]), f, (long long)pymod(ops[2].imm, n)), 0)); return 1;
        }
        return 0;
    }
    const char *D = xmm(c, &ops[0]);
    #define S_(op) ( isx(c, (op)) ? (const char *)xmm(c, (op)) : (op)->type == X86_OP_MEM ? ((op)->size == 16 ? sf(c, "rdx(%s)", ea(c, (op))) : sf(c, "({ XMM t_ = {0}; t_.q[0] = (uint64_t)%s; t_; })", rd(c, (op), 0))) : (unsup(c, "src %s", m), "") )
    int has_imm = ops[nops - 1].type == X86_OP_IMM;
    int64_t imm = has_imm ? ops[nops - 1].imm : 0;
    const char *src = nops > 1 && (isx(c, &ops[1]) || ops[1].type == X86_OP_MEM) ? S_(&ops[1]) : NULL;
    const Lane *vl = lane(VLANES, sizeof VLANES / sizeof VLANES[0], m);
    if (vl && src) {
        const char *fv = vl->fl;
        ef(c, "{ XMM s_ = %s, d_ = %s; __typeof__(d_.%s) a = d_.%s, b = s_.%s; d_.%s = %s; %s = d_; }", src, D, fv, fv, fv, fv, vl->ex, D); return 1;
    }
    const Lane *ln = lane(LANES, sizeof LANES / sizeof LANES[0], m);
    if (ln && src) {
        const char *fl = ln->fl;
        ef(c, "{ XMM s_ = %s, d_ = %s, r_; for (int k = 0; k < %d; k++) { __typeof__(d_.%s[0]) a = d_.%s[k], b = s_.%s[k]; (void)a; r_.%s[k] = (%s); } %s = r_; }",
           src, D, ln->n, fl, fl, fl, fl, ln->ex, D); return 1;
    }
    int ia, ib;
    static const char *const preds[] = {"eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord", NULL};
    if (match3(m, "cmp", preds, L("ps", "pd"), &ia, &ib) && src) {
        static const char *const vc[] = {"a == b", "a < b", "a <= b", "(a != a) | (b != b)", "~(a == b)", "~(a < b)", "~(a <= b)", "(a == a) & (b == b)"};
        if (ib == 0) ef(c, "{ XMM s_ = %s, d_ = %s; v4f a = d_.vf, b = s_.vf; d_.vi = (v4i)(%s); %s = d_; }", src, D, vc[ia], D);
        else ef(c, "{ XMM s_ = %s, d_ = %s; v2d a = d_.vd, b = s_.vd; d_.vl = (v2l)(%s); %s = d_; }", src, D, vc[ia], D);
        return 1;
    }
    if (match3(m, "ps", L("ll", "rl", "ra"), L("w", "d", "q"), &ia, &ib)) {
        static const char *const fls[] = {"w", "u", "q"}, *const vfs[] = {"vw", "vu", "vq"}, *const svs[] = {"vh", "vi", "vl"}, *const sfl[] = {"h", "i", "l"};
        static const int ns[] = {8, 4, 2}, bitss[] = {16, 32, 64};
        const char *fl = fls[ib]; int n = ns[ib], bits = bitss[ib];
        if (has_imm) {
            if (ia == 2) ef(c, "%s.%s = %s.%s >> %lld;", D, svs[ib], D, svs[ib], (long long)(imm < bits - 1 ? imm : bits - 1));
            else if (imm >= bits) ef(c, "%s = (XMM){0};", D);
            else ef(c, "%s.%s = %s.%s %s %lld;", D, vfs[ib], D, vfs[ib], ia == 0 ? "<<" : ">>", (long long)imm);
            return 1;
        }
        const char *cnt = sf(c, "(%s).q[0]", src);
        const char *ex;
        if (ia == 0) ex = sf(c, "c_ >= %d ? 0 : d_.%s[k] << c_", bits, fl);
        else if (ia == 1) ex = sf(c, "c_ >= %d ? 0 : d_.%s[k] >> c_", bits, fl);
        else ex = sf(c, "(d_.%s[k] >> (c_ >= %d ? %d : c_))", sfl[ib], bits, bits - 1);
        ef(c, "{ XMM d_ = %s; uint64_t c_ = %s; for (int k = 0; k < %d; k++) d_.%s[k] = %s; %s = d_; }", D, cnt, n, fl, ex, D); return 1;
    }
    if (IN(m, "pslldq", "psrldq") && has_imm) {
        int64_t sh = imm < 16 ? imm : 16;
        if (strcmp(m, "pslldq") == 0) ef(c, "{ XMM d_ = %s, r_ = {0}; for (int k = %lld; k < 16; k++) r_.b[k] = d_.b[k - %lld]; %s = r_; }", D, (long long)sh, (long long)sh, D);
        else ef(c, "{ XMM d_ = %s, r_ = {0}; for (int k = 0; k + %lld < 16; k++) r_.b[k] = d_.b[k + %lld]; %s = r_; }", D, (long long)sh, (long long)sh, D);
        return 1;
    }
    if ((match3(m, "punpck", L("l", "h"), L("bw", "wd", "dq", "qdq", "ps", "pd"), &ia, &ib) ||
         match3(m, "unpck", L("l", "h"), L("bw", "wd", "dq", "qdq", "ps", "pd"), &ia, &ib)) && src) {
        static const char *const fls[] = {"b", "w", "u", "q", "u", "q"}; static const int ns[] = {16, 8, 4, 2, 4, 2};
        const char *fl = fls[ib]; int n = ns[ib]; int base = ia == 0 ? 0 : n / 2;
        Buf idx = {0};
        for (int k = 0; k < n / 2; k++) if (!buf_fmt(&idx, "%s%d, %d", k ? ", " : "", base + k, n + base + k)) { buf_free(&idx); fail_oom(c); }
        const char *is = sdup(c, idx.p ? idx.p : ""); buf_free(&idx);
        ef(c, "{ XMM s_ = %s, d_ = %s; d_.v%s = __builtin_shufflevector(d_.v%s, s_.v%s, %s); %s = d_; }", src, D, fl, fl, fl, is, D); return 1;
    }
    if (IN(m, "packsswb", "packuswb", "packssdw") && src) {
        if (strcmp(m, "packssdw") == 0)
            ef(c, "{ XMM s_ = %s, d_ = %s, r_; for (int k = 0; k < 4; k++) { int32_t a = d_.i[k], b = s_.i[k]; r_.w[k] = (uint16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a); r_.w[k + 4] = (uint16_t)(b > 32767 ? 32767 : b < -32768 ? -32768 : b); } %s = r_; }", src, D, D);
        else {
            const char *lo = strcmp(m, "packsswb") == 0 ? "-128" : "0", *hi = strcmp(m, "packsswb") == 0 ? "127" : "255";
            ef(c, "{ XMM s_ = %s, d_ = %s, r_; for (int k = 0; k < 8; k++) { int a = d_.h[k], b = s_.h[k]; r_.b[k] = (uint8_t)(a > %s ? %s : a < %s ? %s : a); r_.b[k + 8] = (uint8_t)(b > %s ? %s : b < %s ? %s : b); } %s = r_; }",
               src, D, hi, hi, lo, lo, hi, hi, lo, lo, D);
        }
        return 1;
    }
    if (strcmp(m, "pmuludq") == 0 && src) { ef(c, "{ XMM s_ = %s, d_ = %s, r_; r_.q[0] = (uint64_t)d_.u[0] * s_.u[0]; r_.q[1] = (uint64_t)d_.u[2] * s_.u[2]; %s = r_; }", src, D, D); return 1; }
    if (strcmp(m, "pmaddwd") == 0 && src) { ef(c, "{ XMM s_ = %s, d_ = %s, r_; for (int k = 0; k < 4; k++) r_.i[k] = d_.h[2*k] * s_.h[2*k] + d_.h[2*k+1] * s_.h[2*k+1]; %s = r_; }", src, D, D); return 1; }
    if (strcmp(m, "psadbw") == 0 && src) { ef(c, "{ XMM s_ = %s, d_ = %s, r_ = {0}; for (int h = 0; h < 2; h++) { uint32_t t = 0; for (int k = 0; k < 8; k++) { int v = d_.b[h*8+k] - s_.b[h*8+k]; t += v < 0 ? -v : v; } r_.q[h] = t; } %s = r_; }", src, D, D); return 1; }
    if (strcmp(m, "pshufb") == 0 && src) { ef(c, "{ XMM s_ = %s, d_ = %s, r_; for (int k = 0; k < 16; k++) r_.b[k] = (s_.b[k] & 0x80) ? 0 : d_.b[s_.b[k] & 15]; %s = r_; }", src, D, D); return 1; }
    if (strcmp(m, "shufps") == 0 && has_imm) {
        const char *s = S_(&ops[1]);
        ef(c, "{ XMM s_ = %s, d_ = %s; d_.vu = __builtin_shufflevector(d_.vu, s_.vu, %lld, %lld, %lld, %lld); %s = d_; }",
           s, D, (long long)(imm & 3), (long long)((imm >> 2) & 3), (long long)(4 + ((imm >> 4) & 3)), (long long)(4 + ((imm >> 6) & 3)), D); return 1;
    }
    if (strcmp(m, "shufpd") == 0 && has_imm) {
        const char *s = S_(&ops[1]);
        ef(c, "{ XMM s_ = %s, d_ = %s; d_.vq = __builtin_shufflevector(d_.vq, s_.vq, %lld, %lld); %s = d_; }", s, D, (long long)(imm & 1), (long long)(2 + ((imm >> 1) & 1)), D); return 1;
    }
    if (strcmp(m, "pshufd") == 0 && has_imm) {
        const char *s = S_(&ops[1]);
        ef(c, "{ XMM s_ = %s; s_.vu = __builtin_shufflevector(s_.vu, s_.vu, %lld, %lld, %lld, %lld); %s = s_; }",
           s, (long long)(imm & 3), (long long)((imm >> 2) & 3), (long long)((imm >> 4) & 3), (long long)((imm >> 6) & 3), D); return 1;
    }
    if (IN(m, "pshuflw", "pshufhw") && has_imm) {
        int o = strcmp(m, "pshuflw") == 0 ? 0 : 4;
        long long idx[8];
        for (int k = 0; k < 8; k++) idx[k] = k;
        for (int k = 0; k < 4; k++) idx[o + k] = o + ((imm >> (2 * k)) & 3);
        const char *s = S_(&ops[1]);
        ef(c, "{ XMM s_ = %s; s_.vw = __builtin_shufflevector(s_.vw, s_.vw, %lld, %lld, %lld, %lld, %lld, %lld, %lld, %lld); %s = s_; }",
           s, idx[0], idx[1], idx[2], idx[3], idx[4], idx[5], idx[6], idx[7], D); return 1;
    }
    if (strcmp(m, "pinsrw") == 0 && has_imm) {
        const char *v = ops[1].type == X86_OP_MEM ? rd(c, &ops[1], 2) : sf(c, "(uint16_t)%s", rd(c, &ops[1], 0));
        ef(c, "%s.w[%lld] = %s;", D, (long long)(imm & 7), v); return 1;
    }
    if (strcmp(m, "movlhps") == 0 && src) { ef(c, "{ XMM s_ = %s, d_ = %s; d_.vq = __builtin_shufflevector(d_.vq, s_.vq, 0, 2); %s = d_; }", src, D, D); return 1; }
    if (strcmp(m, "movhlps") == 0 && src) { ef(c, "{ XMM s_ = %s, d_ = %s; d_.vq = __builtin_shufflevector(d_.vq, s_.vq, 3, 1); %s = d_; }", src, D, D); return 1; }
    if (strcmp(m, "movshdup") == 0 && src) { ef(c, "{ XMM s_ = %s; s_.vu = __builtin_shufflevector(s_.vu, s_.vu, 1, 1, 3, 3); %s = s_; }", src, D); return 1; }
    if (strcmp(m, "movsldup") == 0 && src) { ef(c, "{ XMM s_ = %s; s_.vu = __builtin_shufflevector(s_.vu, s_.vu, 0, 0, 2, 2); %s = s_; }", src, D); return 1; }
    if (strcmp(m, "movddup") == 0 && src) { ef(c, "{ XMM s_ = %s; s_.vq = __builtin_shufflevector(s_.vq, s_.vq, 0, 0); %s = s_; }", src, D); return 1; }
    if (IN(m, "haddps", "hsubps") && src) {
        ef(c, "{ XMM s_ = %s, d_ = %s; d_.vf = __builtin_shufflevector(d_.vf, s_.vf, 0, 2, 4, 6) %s __builtin_shufflevector(d_.vf, s_.vf, 1, 3, 5, 7); %s = d_; }",
           src, D, strcmp(m, "haddps") == 0 ? "+" : "-", D); return 1;
    }
    static const char *const conv[][2] = {
        {"cvtps2pd", "r_.d[0] = s_.f[0]; r_.d[1] = s_.f[1];"},
        {"cvtpd2ps", "r_.q[1] = 0; r_.f[0] = (float)s_.d[0]; r_.f[1] = (float)s_.d[1];"},
        {"cvtdq2ps", "r_.vf = __builtin_convertvector(s_.vi, v4f);"},
        {"cvtdq2pd", "r_.d[0] = s_.i[0]; r_.d[1] = s_.i[1];"},
        {"cvtps2dq", "for (int k = 0; k < 4; k++) r_.i[k] = cvt_i32(sse_round(c, s_.f[k]));"},
        {"cvttps2dq", "v4i bad_ = (s_.vf != s_.vf) | (s_.vf >= 2147483648.0f) | (s_.vf < -2147483648.0f); "
                      "r_.vi = VSEL(bad_, V4I_MIN, __builtin_convertvector(VSEL(bad_, (v4f){0}, s_.vf), v4i));"},
        {"cvtpd2dq", "r_.q[1] = 0; r_.i[0] = cvt_i32(sse_round(c, s_.d[0])); r_.i[1] = cvt_i32(sse_round(c, s_.d[1]));"},
        {"cvttpd2dq", "r_.q[1] = 0; r_.i[0] = cvt_i32(s_.d[0]); r_.i[1] = cvt_i32(s_.d[1]);"},
    };
    for (size_t k = 0; k < sizeof conv / sizeof conv[0]; k++)
        if (strcmp(m, conv[k][0]) == 0 && src) { ef(c, "{ XMM s_ = %s, r_; %s %s = r_; }", src, conv[k][1], D); return 1; }
    if (strcmp(m, "cvtpi2ps") == 0) {
        const char *s = ismm(c, &ops[1]) ? sf(c, "C.mm[%d]", rn(c, ops[1].reg)[2] - '0') : rd(c, &ops[1], 8);
        ef(c, "{ MM64 s_ = {.q = {%s}}; %s.f[0] = (float)s_.i[0]; %s.f[1] = (float)s_.i[1]; }", s, D, D); return 1;
    }
    #undef S_
    return 0;
}

// ---------------- x87 ----------------

static const char *st(Ctx *c, const cs_x86_op *op) {
    const char *n = rn(c, op->reg);
    if (!n) pyerr(c, "'NoneType' object is not subscriptable");
    const char *s = strlen(n) >= 2 ? n + 2 : "";
    char t[32]; snprintf(t, sizeof t, "%s", s);
    char *b = t; while (*b == '(' || *b == ')') b++;
    size_t l = strlen(b); while (l && (b[l - 1] == '(' || b[l - 1] == ')')) b[--l] = 0;
    if (!*b) return "ST(0)";
    for (char *q = b; *q; q++) if (*q < '0' || *q > '9') pyerr(c, "invalid literal for int() with base 10: '%s'", b);
    return sf(c, "ST(%d)", atoi(b));
}
static const char *fmem(Ctx *c, const cs_x86_op *op) {
    const char *f = op->size == 4 ? "(double)rdf(%s)" : op->size == 8 ? "rdd(%s)" : op->size == 10 ? "rd80(%s)" : NULL;
    if (!f) pyerr(c, "%d", op->size);
    const char *a = ea(c, op);
    return op->size == 4 ? sf(c, "(double)rdf(%s)", a) : op->size == 8 ? sf(c, "rdd(%s)", a) : sf(c, "rd80(%s)", a);
}
static int x87(Ctx *c, const char *m, const Ins *in) {
    if (m[0] != 'f') return 0;
    const cs_x86_op *ops = in->ops; int nops = in->nops;
    static const char cmpset[] = "C.fsw = (C.fsw & ~0x4500u) | ((a != a || b != b) ? 0x4500u : a < b ? 0x0100u : a == b ? 0x4000u : 0);";
    if (IN(m, "fld", "fild") && nops && ops[0].type == X86_OP_MEM) {
        const cs_x86_op *op = &ops[0];
        if (strcmp(m, "fild") == 0) { const char *s = Sg(c, op->size); ef(c, "FPUSH(c, (double)(%s)%s);", s, rd(c, op, 0)); }
        else ef(c, "FPUSH(c, %s);", fmem(c, op));
        return 1;
    }
    if (strcmp(m, "fld") == 0 && nops) { ef(c, "{ double v = %s; FPUSH(c, v); }", st(c, &ops[0])); return 1; }
    if (IN(m, "fst", "fstp") && nops) {
        const cs_x86_op *op = &ops[0];
        const char *pop = strcmp(m, "fstp") == 0 ? " FPOP(c);" : "";
        if (op->type == X86_OP_MEM) {
            if (op->size != 4 && op->size != 8 && op->size != 10) pyerr(c, "%d", op->size);
            const char *a = ea(c, op);
            const char *w = op->size == 4 ? sf(c, "wrf(%s, (float)ST(0));", a) : op->size == 8 ? sf(c, "wrd(%s, ST(0));", a) : sf(c, "wr80(%s, ST(0));", a);
            ef(c, "%s%s", w, pop);
        } else ef(c, "%s = ST(0);%s", st(c, op), pop);
        return 1;
    }
    if (IN(m, "fistp", "fist", "fisttp") && nops) {
        int n = ops[0].size;
        const char *v = strcmp(m, "fisttp") == 0 ? "trunc(ST(0))" : "fpu_round(c, ST(0))";
        const char *cv = n == 8 ? sf(c, "cvt_i64(%s)", v) : n == 4 ? sf(c, "cvt_i32(%s)", v) : sf(c, "(int16_t)cvt_i32(%s)", v);
        const char *u = U(c, n);
        ef(c, "%s%s", wr(c, &ops[0], sf(c, "(%s)%s", u, cv), 0), strcmp(m, "fist") == 0 ? "" : " FPOP(c);"); return 1;
    }
    if (IN(m, "fnstcw", "fstcw")) { e(c, wr(c, &ops[0], "C.fcw", 2)); return 1; }
    if (strcmp(m, "fldcw") == 0) { ef(c, "C.fcw = %s;", rd(c, &ops[0], 2)); return 1; }
    if (IN(m, "fnstsw", "fstsw")) {
        const char *v = "(uint16_t)((C.fsw & ~0x3800u) | ((c->top & 7) << 11))";
        if (nops && ops[0].type == X86_OP_REG) ef(c, "C.eax = (C.eax & ~0xFFFFu) | %s;", v);
        else e(c, wr(c, &ops[0], v, 2));
        return 1;
    }
    if (IN(m, "fnclex", "fclex")) { e(c, "C.fsw &= 0x7F00u;"); return 1; }
    if (IN(m, "fninit", "finit")) { e(c, "C.fcw = 0x37F; C.fsw = 0; c->top = 0;"); return 1; }
    static const char *const consts[][2] = {{"fld1", "1.0"}, {"fldz", "0.0"}, {"fldpi", "M_PI"}, {"fldl2e", "M_LOG2E"}, {"fldln2", "M_LN2"},
                                            {"fldlg2", "0.30102999566398119521"}, {"fldl2t", "3.32192809488736234787"}};
    for (size_t k = 0; k < sizeof consts / sizeof consts[0]; k++) if (strcmp(m, consts[k][0]) == 0) { ef(c, "FPUSH(c, %s);", consts[k][1]); return 1; }
    static const char *const arops[] = {"add", "subr", "sub", "mul", "divr", "div", NULL};
    static const char *const arex[] = {"a + b", "b - a", "a - b", "a * b", "b / a", "a / b"};
    for (int isint = 1; isint >= 0; isint--) {
        const char *pre = isint ? "fi" : "f";
        int ia, ib;
        if (!match3(m, pre, arops, L("", "p"), &ia, &ib)) continue;
        int pop = ib == 1;
        if (nops && ops[0].type == X86_OP_MEM) {
            const cs_x86_op *o = &ops[0];
            const char *b = isint ? sf(c, "(double)(%s)%s", Sg(c, o->size), rd(c, o, 0)) : fmem(c, o);
            ef(c, "{ double a = ST(0), b = %s; ST(0) = %s; }", b, arex[ia]); return 1;
        }
        if (nops == 2) {
            const char *d = st(c, &ops[0]), *s_ = st(c, &ops[1]);
            ef(c, "{ double a = %s, b = %s; %s = %s;%s }", d, s_, d, arex[ia], pop ? " FPOP(c);" : ""); return 1;
        }
        if (nops == 1) {
            const char *s_ = st(c, &ops[0]);
            if (pop) { ef(c, "{ double a = %s, b = ST(0); %s = %s; FPOP(c); }", s_, s_, arex[ia]); return 1; }
            ef(c, "{ double a = ST(0), b = %s; ST(0) = %s; }", s_, arex[ia]); return 1;
        }
        if (!nops && pop) { ef(c, "{ double a = ST(1), b = ST(0); ST(1) = %s; FPOP(c); }", arex[ia]); return 1; }
        break;
    }
    static const char *const un[][2] = {{"fchs", "-ST(0)"}, {"fabs", "fabs(ST(0))"}, {"fsqrt", "sqrt(ST(0))"}, {"fsin", "sin(ST(0))"}, {"fcos", "cos(ST(0))"},
                                        {"frndint", "fpu_round(c, ST(0))"}, {"f2xm1", "exp2(ST(0)) - 1.0"}};
    for (size_t k = 0; k < sizeof un / sizeof un[0]; k++) if (strcmp(m, un[k][0]) == 0) { ef(c, "ST(0) = %s;", un[k][1]); return 1; }
    if (strcmp(m, "fxch") == 0) {
        const char *o = nops ? st(c, &ops[nops - 1]) : "ST(1)";
        ef(c, "{ double t = ST(0); ST(0) = %s; %s = t; }", o, o); return 1;
    }
    if (strcmp(m, "fpatan") == 0) { e(c, "{ double v = atan2(ST(1), ST(0)); FPOP(c); ST(0) = v; }"); return 1; }
    if (strcmp(m, "fptan") == 0) { e(c, "ST(0) = tan(ST(0)); FPUSH(c, 1.0);"); return 1; }
    if (strcmp(m, "fsincos") == 0) { e(c, "{ double v = ST(0); ST(0) = sin(v); FPUSH(c, cos(v)); }"); return 1; }
    if (strcmp(m, "fyl2x") == 0) { e(c, "{ double v = ST(1) * log2(ST(0)); FPOP(c); ST(0) = v; }"); return 1; }
    if (strcmp(m, "fyl2xp1") == 0) { e(c, "{ double v = ST(1) * log2(ST(0) + 1.0); FPOP(c); ST(0) = v; }"); return 1; }
    if (strcmp(m, "fscale") == 0) { e(c, "ST(0) = ldexp(ST(0), (int)trunc(ST(1)));"); return 1; }
    if (IN(m, "fprem", "fprem1")) { ef(c, "{ double r = %s(ST(0), ST(1)); ST(0) = r; C.fsw &= ~0x0400u; }", strcmp(m, "fprem") == 0 ? "fmod" : "remainder"); return 1; }
    if (strcmp(m, "fxtract") == 0) { e(c, "{ int ex; double mnt = frexp(ST(0), &ex); ST(0) = (double)(ex - 1); FPUSH(c, mnt * 2.0); }"); return 1; }
    if (IN(m, "fucomip", "fcomip", "fucompi", "fcompi", "fucomi", "fcomi")) {
        const char *b = nops ? st(c, &ops[nops - 1]) : "ST(1)";
        ef(c, "{ double a = ST(0), b = %s; if (a != a || b != b) { zf = cf = 1; pr = 0; } else { zf = a == b; cf = a < b; pr = 1; } of = sf = 0;%s }",
           b, !IN(m, "fucomi", "fcomi") ? " FPOP(c);" : ""); return 1;
    }
    {
        int isint = -1, pops = 0;
        static const char *const cmpn[][3] = {{"fi", "u", ""}, {"fi", "", ""}, {"f", "u", ""}, {"f", "", ""}};
        for (int k = 0; k < 4 && isint < 0; k++)
            for (int p = 2; p >= 0; p--) {
                char t[32]; snprintf(t, sizeof t, "%s%scom%s", cmpn[k][0], cmpn[k][1], p == 2 ? "pp" : p == 1 ? "p" : "");
                if (strcmp(m, t) == 0) { isint = k < 2; pops = p; break; }
            }
        if (isint >= 0) {
            const char *b;
            if (nops && ops[0].type == X86_OP_MEM) b = isint ? sf(c, "(double)(%s)%s", Sg(c, ops[0].size), rd(c, &ops[0], 0)) : fmem(c, &ops[0]);
            else if (nops) b = st(c, &ops[nops - 1]);
            else b = "ST(1)";
            ef(c, "{ double a = ST(0), b = %s; %s%s }", b, cmpset, pops == 2 ? " FPOP(c); FPOP(c);" : pops == 1 ? " FPOP(c);" : "");
            return 1;
        }
    }
    if (strcmp(m, "ftst") == 0) { ef(c, "{ double a = ST(0), b = 0.0; %s }", cmpset); return 1; }
    if (strcmp(m, "fxam") == 0) {
        e(c, "{ double v = ST(0); uint32_t s_ = signbit(v) ? 0x200u : 0; C.fsw = (C.fsw & ~0x4700u) | s_ | (v != v ? 0x100u : isinf(v) ? 0x500u : v == 0 ? 0x4000u : 0x400u); }"); return 1;
    }
    {
        int ia, ib;
        if (match3(m, "fcmov", L("n", ""), L("b", "e", "be", "u"), &ia, &ib)) {
            static const char *const cond[] = {"cf", "zf", "(cf||zf)", "parity8(pr)"};
            const char *s = nops > 1 ? st(c, &ops[1]) : st(c, &ops[0]);
            ef(c, "if (%s(%s)) ST(0) = %s;", ia == 0 ? "!" : "", cond[ib], s); return 1;
        }
    }
    if (strcmp(m, "fstp") == 0 && !nops) { e(c, "FPOP(c);"); return 1; }
    if (strcmp(m, "fincstp") == 0) { e(c, "c->top = (c->top + 1) & 7;"); return 1; }
    if (strcmp(m, "fdecstp") == 0) { e(c, "c->top = (c->top - 1) & 7;"); return 1; }
    if (IN(m, "ffree", "ffreep", "fnop")) { if (strcmp(m, "ffreep") == 0) e(c, "FPOP(c);"); return 1; }
    unsup(c, "%s", m);
}

// ---------------- instructions ----------------

static void insn(Ctx *c, const Ins *i) {
    char mb[CS_MNEMONIC_SIZE];
    snprintf(mb, sizeof mb, "%s", i->mn);
    const char *m = mb;
    const cs_x86_op *ops = i->ops; int nops = i->nops;
    uint32_t nxt = i->address + i->size;
    if (IN(m, "repz ret", "rep ret", "bnd ret")) m = "ret";
    if (starts(m, "bnd ")) m += 4;
    if (strcmp(m, "movnti") == 0) m = "mov";
    if (mmx(c, m, i)) return;
    if (IN(m, "nop", "pause", "lfence", "sfence", "mfence", "wait", "fwait", "endbr32", "clflush") || starts(m, "prefetch") || starts(m, "nop")) return;
    if (strcmp(m, "cld") == 0) { e(c, "df = 0;"); return; }
    if (strcmp(m, "std") == 0) { e(c, "df = 1;"); return; }
    if (IN(m, "int3", "ud2", "hlt")) { ef(c, "bad_insn(c, 0x%xu, \"%s\"); return;", i->address, m); return; }
    if (strcmp(m, "int") == 0) { ef(c, "cpu_int(c, 0x%xu, %lld); return;", i->address, (long long)ops[0].imm); return; }
    if (strcmp(m, "mov") == 0) {
        if (nops != 2) pyerr(c, "not enough values to unpack");
        const cs_x86_op *d = &ops[0], *s = &ops[1];
        for (int k = 0; k < 2; k++) { const cs_x86_op *o = &ops[k]; if (o->type == X86_OP_REG && !reg_info(rn(c, o->reg))) unsup_name(c, "mov special ", rn(c, o->reg)); }
        e(c, wr(c, d, rd(c, s, d->size), 0)); return;
    }
    if (strcmp(m, "lea") == 0) {
        const cs_x86_op *d = &ops[0], *s = &ops[1];
        const char *a = ea(c, s);
        Buf b = {0};   // str.replace("C.fsbase + ", "")
        for (const char *p = a; *p;) {
            if (starts(p, "C.fsbase + ")) { p += 11; continue; }
            if (!buf_add(&b, p, 1)) { buf_free(&b); fail_oom(c); }
            p++;
        }
        const char *v = sdup(c, b.p ? b.p : ""); buf_free(&b);
        e(c, wr(c, d, v, 0)); return;
    }
    if (strcmp(m, "movzx") == 0) { const cs_x86_op *d = &ops[0], *s = &ops[1]; const char *u = U(c, d->size); e(c, wr(c, d, sf(c, "(%s)%s", u, rd(c, s, 0)), 0)); return; }
    if (strcmp(m, "movsx") == 0) {
        const cs_x86_op *d = &ops[0], *s = &ops[1];
        const char *u = U(c, d->size), *a = Sg(c, d->size), *b = Sg(c, s->size);
        e(c, wr(c, d, sf(c, "(%s)(%s)(%s)%s", u, a, b, rd(c, s, 0)), 0)); return;
    }
    if (strcmp(m, "cwde") == 0) { e(c, "C.eax = (uint32_t)(int32_t)(int16_t)C.eax;"); return; }
    if (strcmp(m, "cbw") == 0) { e(c, "C.eax = (C.eax & ~0xFFFFu) | (uint16_t)(int16_t)(int8_t)C.eax;"); return; }
    if (strcmp(m, "cdq") == 0) { e(c, "C.edx = (uint32_t)((int32_t)C.eax >> 31);"); return; }
    if (strcmp(m, "cwd") == 0) { e(c, "C.edx = (C.edx & ~0xFFFFu) | (uint16_t)((int16_t)C.eax >> 15);"); return; }
    if (strcmp(m, "push") == 0) {
        const cs_x86_op *o = &ops[0];
        if (o->type == X86_OP_REG && !reg_info(rn(c, o->reg))) unsup_name(c, "push ", rn(c, o->reg));
        if (o->type == X86_OP_IMM) ef(c, "PUSH(c, 0x%llxu);", (unsigned long long)(o->imm & 0xFFFFFFFF));
        else if (o->size == 2) ef(c, "C.esp -= 2; wrs16(C.esp, %s);", rd(c, o, 0));
        else ef(c, "PUSH(c, %s);", rd(c, o, 4));
        return;
    }
    if (strcmp(m, "pop") == 0) {
        const cs_x86_op *o = &ops[0];
        if (o->type == X86_OP_REG && !reg_info(rn(c, o->reg))) unsup_name(c, "pop ", rn(c, o->reg));
        if (o->size == 2) { ef(c, "{ uint16_t v = rds16(C.esp); C.esp += 2; %s }", wr(c, o, "v", 0)); return; }
        ef(c, "{ uint32_t v = POP(c); %s }", wr(c, o, "v", 0)); return;
    }
    if (strcmp(m, "pushal") == 0) { e(c, "{ uint32_t s_ = C.esp; PUSH(c, C.eax); PUSH(c, C.ecx); PUSH(c, C.edx); PUSH(c, C.ebx); PUSH(c, s_); PUSH(c, C.ebp); PUSH(c, C.esi); PUSH(c, C.edi); }"); return; }
    if (strcmp(m, "popal") == 0) { e(c, "{ C.edi = POP(c); C.esi = POP(c); C.ebp = POP(c); (void)POP(c); C.ebx = POP(c); C.edx = POP(c); C.ecx = POP(c); C.eax = POP(c); }"); return; }
    if (strcmp(m, "pushfd") == 0) { e(c, "PUSH(c, 0x202u | (cf ? 1 : 0) | (parity8(pr) ? 4 : 0) | (zf ? 0x40 : 0) | (sf ? 0x80 : 0) | (df ? 0x400 : 0) | (of ? 0x800 : 0));"); return; }
    if (strcmp(m, "popfd") == 0) { e(c, "{ uint32_t v = POP(c); cf = v & 1; zf = (v >> 6) & 1; sf = (v >> 7) & 1; df = (v >> 10) & 1; of = (v >> 11) & 1; pr = (v & 4) ? 0 : 1; }"); return; }
    if (strcmp(m, "lahf") == 0) { e(c, "C.eax = (C.eax & ~0xFF00u) | ((uint32_t)(2 | (cf ? 1 : 0) | (parity8(pr) ? 4 : 0) | (zf ? 0x40 : 0) | (sf ? 0x80 : 0)) << 8);"); return; }
    if (strcmp(m, "sahf") == 0) { e(c, "{ uint32_t v = C.eax >> 8; cf = v & 1; zf = (v >> 6) & 1; sf = (v >> 7) & 1; pr = (v & 4) ? 0 : 1; }"); return; }
    if (strcmp(m, "clc") == 0) { e(c, "cf = 0;"); return; }
    if (strcmp(m, "stc") == 0) { e(c, "cf = 1;"); return; }
    if (strcmp(m, "cmc") == 0) { e(c, "cf = !cf;"); return; }
    if (strcmp(m, "leave") == 0) { e(c, "C.esp = C.ebp; C.ebp = POP(c);"); return; }
    if (strcmp(m, "enter") == 0) {
        int64_t n = ops[0].imm, lvl = ops[1].imm;
        if (lvl) unsup(c, "enter level");
        ef(c, "PUSH(c, C.ebp); C.ebp = C.esp; C.esp -= %lldu;", (long long)n); return;
    }
    if (strcmp(m, "ret") == 0) { int64_t n = nops ? ops[0].imm : 0; ef(c, "C.esp += %lld; return;", (long long)(4 + n)); return; }
    if (strcmp(m, "call") == 0) { call(c, i, &ops[0], nxt); return; }
    if (strcmp(m, "jmp") == 0) {
        const cs_x86_op *op = &ops[0];
        if (op->type == X86_OP_IMM) { e(c, go_to(c, op->imm & 0xFFFFFFFF, i->address)); return; }
        const Table *tb = find_table(c, i->address);
        if (tb && tb->n) {
            // sorted(set(tbl))
            uint32_t *u = ar_alloc(c, tb->n * sizeof *u); size_t nu = 0;
            for (size_t k = 0; k < tb->n; k++) u[nu++] = tb->t[k];
            for (size_t a = 1; a < nu; a++) { uint32_t v = u[a]; size_t b = a; while (b && u[b - 1] > v) { u[b] = u[b - 1]; b--; } u[b] = v; }
            Buf cs = {0};
            for (size_t k = 0; k < nu; k++) {
                if (k && u[k] == u[k - 1]) continue;
                if (!buf_fmt(&cs, "%scase 0x%xu: goto L_%x;", cs.n ? " " : "", u[k], u[k])) { buf_free(&cs); fail_oom(c); }
            }
            const char *cases = sdup(c, cs.p ? cs.p : ""); buf_free(&cs);
            ef(c, "{ uint32_t t = %s; switch (t) { %s default: call_ind(c, t); return; } }", rd(c, op, 4), cases);
            return;
        }
        int ok; uint32_t slot = iat_slot(c, op, &ok);
        if (ok) { char s[512]; imp_sym(pe_iat(&c->tr->pe, slot), s, sizeof s); ef(c, "I_%s(c); C.esp += 4; return;", s); return; }
        ef(c, "{ uint32_t t = %s; call_ind(c, t); return; }", rd(c, op, 4)); return;
    }
    if (IN(m, "jecxz", "jcxz")) {
        const char *r = strcmp(m, "jecxz") == 0 ? "C.ecx" : "(uint16_t)C.ecx";
        ef(c, "if (%s == 0) %s", r, goto_block(c, ops[0].imm & 0xFFFFFFFF, i->address)); return;
    }
    if (IN(m, "loop", "loope", "loopne")) {
        const char *cond = strcmp(m, "loop") == 0 ? "C.ecx != 0" : strcmp(m, "loope") == 0 ? "C.ecx != 0 && zf" : "C.ecx != 0 && !zf";
        ef(c, "C.ecx--; if (%s) %s", cond, goto_block(c, ops[0].imm & 0xFFFFFFFF, i->address)); return;
    }
    if (m[0] == 'j') {
        const char *cc = CC(m + 1);
        if (!cc) unsup(c, "%s", m);
        ef(c, "if (%s) %s", cc, goto_block(c, ops[0].imm & 0xFFFFFFFF, i->address)); return;
    }
    if (starts(m, "set") && CC(m + 3)) { e(c, wr(c, &ops[0], sf(c, "(%s) ? 1 : 0", CC(m + 3)), 0)); return; }
    if (starts(m, "cmov") && CC(m + 4)) { const cs_x86_op *d = &ops[0]; ef(c, "if (%s) %s", CC(m + 4), wr(c, d, rd(c, &ops[1], d->size), 0)); return; }
    if (strcmp(m, "xlatb") == 0) { e(c, "C.eax = (C.eax & ~0xFFu) | rd8((uint32_t)(C.ebx + (uint8_t)C.eax));"); return; }
    if (alu(c, m, i)) return;
    if (strop(c, m, i)) return;
    if (atomic(c, m, i)) return;
    if (strcmp(m, "cpuid") == 0) { e(c, "cpu_cpuid(c);"); return; }
    if (strcmp(m, "xgetbv") == 0) { e(c, "C.eax = 7; C.edx = 0;"); return; }
    if (strcmp(m, "rdtsc") == 0) { e(c, "{ uint64_t t = __builtin_readcyclecounter(); C.eax = (uint32_t)t; C.edx = (uint32_t)(t >> 32); }"); return; }
    if (sse(c, m, i, nops)) return;
    if (vec(c, m, i, nops)) return;
    if (x87(c, m, i)) return;
    unsup(c, "%s", m);
}

// ================================================================ the post-passes of a function's C ================================================================

// STACK_ACCESS: \b(rd|wr)(8|16|32|64|f|d|x)\(\(uint32_t\)\(C\.esp\b -> \1s\2((uint32_t)(C.esp
static int pass_stack(const char *t, size_t n, Buf *o) {
    static const char *const sz[] = {"8", "16", "32", "64", "f", "d", "x"};
    static const char tail_[] = "((uint32_t)(C.esp";
    size_t i = 0;
    while (i < n) {
        if ((t[i] == 'r' && t[i + 1] == 'd') || (t[i] == 'w' && t[i + 1] == 'r')) {
            if (i == 0 || !isword((unsigned char)t[i - 1])) {
                for (int k = 0; k < 7; k++) {
                    size_t l = strlen(sz[k]);
                    if (strncmp(t + i + 2, sz[k], l) != 0) continue;
                    size_t p = i + 2 + l;
                    if (strncmp(t + p, tail_, sizeof tail_ - 1) != 0) break;
                    size_t q = p + sizeof tail_ - 1;
                    if (q < n && isword((unsigned char)t[q])) break;
                    if (!buf_add(o, t + i, 2) || !buf_str(o, "s") || !buf_add(o, sz[k], l) || !buf_str(o, tail_)) return 0;
                    i = q;
                    goto next;
                }
            }
        }
        if (!buf_add(o, t + i, 1)) return 0;
        i++;
    next:;
    }
    return 1;
}

// const_loads: \b(rd8|rd16|rd32|rd64|rdf|rdd|rdx)\(0x([0-9a-f]+)u\) of read-only data -> rdc*
static int pass_const(const PE *pe, const char *t, size_t n, Buf *o) {
    static const char *const nm[] = {"rd8", "rd16", "rd32", "rd64", "rdf", "rdd", "rdx"};
    static const char *const cn[] = {"rdc8", "rdc16", "rdc32", "rdc64", "rdcf", "rdcd", "rdcx"};
    static const int sz[] = {1, 2, 4, 8, 4, 8, 16};
    size_t i = 0;
    while (i < n) {
        if (t[i] == 'r' && t[i + 1] == 'd' && (i == 0 || !isword((unsigned char)t[i - 1]))) {
            for (int k = 0; k < 7; k++) {
                size_t l = strlen(nm[k]);
                if (strncmp(t + i, nm[k], l) != 0) continue;
                size_t p = i + l;
                if (strncmp(t + p, "(0x", 3) != 0) break;
                size_t h = p + 3, e2 = h;
                while (e2 < n && ishex((unsigned char)t[e2])) e2++;
                if (e2 == h || strncmp(t + e2, "u)", 2) != 0) break;
                size_t end = e2 + 2;
                uint64_t a = 0; int big = 0;
                for (size_t q = h; q < e2; q++) { if (a >> 60) big = 1; a = a << 4 | (uint64_t)(t[q] <= '9' ? t[q] - '0' : t[q] - 'a' + 10); }
                int con = 0;
                if (!big && !(a <= UINT32_MAX && pe_iat(pe, (uint32_t)a))) {
                    for (int s = 0; s < pe->ns; s++) {
                        const Sec *x = &pe->s[s];
                        if (x->ch & 0x80000000u) continue;
                        if ((uint64_t)x->va <= a && a + (uint64_t)sz[k] <= (uint64_t)x->va + x->vs) { con = 1; break; }
                    }
                }
                if (con) { if (!buf_str(o, cn[k]) || !buf_str(o, "(0x") || !buf_add(o, t + h, e2 - h) || !buf_str(o, "u)")) return 0; }
                else if (!buf_add(o, t + i, end - i)) return 0;
                i = end;
                goto next;
            }
        }
        if (!buf_add(o, t + i, 1)) return 0;
        i++;
    next:;
    }
    return 1;
}

// a literal replacement at word starts: \bFROM -> TO (FROM starts with a word character)
static int sub_word(const char *t, size_t n, const char *from, const char *to, Buf *o) {
    size_t fl = strlen(from), i = 0;
    while (i < n) {
        if (t[i] == from[0] && (i == 0 || !isword((unsigned char)t[i - 1])) && strncmp(t + i, from, fl) == 0) {
            if (!buf_str(o, to)) return 0;
            i += fl; continue;
        }
        if (!buf_add(o, t + i, 1)) return 0;
        i++;
    }
    return 1;
}
// \bC\.(gpr)\b -> R_\1 and \bC\.xmm\[(\d+)\] -> X_\1 (two passes of the Python tool; their matches cannot overlap)
static int sub_regs(const char *t, size_t n, Buf *o) {
    size_t i = 0;
    while (i < n) {
        if (t[i] == 'C' && t[i + 1] == '.' && (i == 0 || !isword((unsigned char)t[i - 1]))) {
            int done = 0;
            for (int g = 0; g < 8 && !done; g++)
                if (strncmp(t + i + 2, GPRS[g], 3) == 0 && (i + 5 >= n || !isword((unsigned char)t[i + 5]))) {
                    if (!buf_str(o, "R_") || !buf_str(o, GPRS[g])) return 0;
                    i += 5; done = 1;
                }
            if (done) continue;
            if (strncmp(t + i + 2, "xmm[", 4) == 0) {
                size_t d = i + 6, e2 = d;
                while (e2 < n && t[e2] >= '0' && t[e2] <= '9') e2++;
                if (e2 > d && t[e2] == ']') {
                    if (!buf_str(o, "X_") || !buf_add(o, t + d, e2 - d)) return 0;
                    i = e2 + 1; continue;
                }
            }
        }
        if (!buf_add(o, t + i, 1)) return 0;
        i++;
    }
    return 1;
}
// _ASSIGN at p: \s*(?:[-+*/&|^%]|<<|>>)?=(?!=) ; with incdec, also \s*(?:\+\+|--)
static int assign_at(const char *t, size_t n, size_t p, int incdec) {
    while (p < n && isspc((unsigned char)t[p])) p++;
    if (p >= n) return 0;
    char ch = t[p];
    if (strchr("-+*/&|^%", ch) && ch) { if (p + 1 < n && t[p + 1] == '=' && !(p + 2 < n && t[p + 2] == '=')) return 1; }
    else if ((ch == '<' || ch == '>') && p + 1 < n && t[p + 1] == ch) { if (p + 2 < n && t[p + 2] == '=' && !(p + 3 < n && t[p + 3] == '=')) return 1; }
    else if (ch == '=') { if (!(p + 1 < n && t[p + 1] == '=')) return 1; }
    if (incdec && p + 1 < n && ((t[p] == '+' && t[p + 1] == '+') || (t[p] == '-' && t[p + 1] == '-'))) return 1;
    return 0;
}

// _BARRIER at p: the length of the match, or 0
static size_t barrier_at(const char *t, size_t n, size_t p) {
    if (p > 0 && isword((unsigned char)t[p - 1])) return 0;
    size_t q;
    if (t[p] == 'F' && t[p + 1] == '_') {
        q = p + 2;
        while (q < n && ishex((unsigned char)t[q])) q++;
        if (q == p + 2) return 0;
    } else if (t[p] == 'I' && t[p + 1] == '_') {
        q = p + 2;
        while (q < n && isword((unsigned char)t[q])) q++;
        if (q == p + 2) return 0;
    } else {
        static const char *const names[] = {"call_ind", "missing_func", "bad_insn", "bad_jump", "cpu_cpuid", "unimpl_import", "cpu_int"};
        q = 0;
        for (int k = 0; k < 7; k++) { size_t l = strlen(names[k]); if (strncmp(t + p, names[k], l) == 0) { q = p + l; break; } }
        if (!q) return 0;
    }
    if (!(q + 1 < n && t[q] == '(' && t[q + 1] == 'c')) return 0;
    q += 2;
    if (q + 1 < n && t[q] == ')' && t[q + 1] == ';') return q + 2 - p;
    if (q >= n || t[q] != ',') return 0;
    size_t s = q + 1;
    for (;;) {
        if (s >= n) return 0;
        if (t[s] == ';') break;
        if (t[s] == '"') {
            size_t j = s + 1; int okk = 0;
            while (j < n) {
                if (t[j] == '"') { okk = 1; break; }
                if (t[j] == '\\') { if (j + 1 < n && t[j + 1] != '\n') { j += 2; continue; } break; }
                j++;
            }
            if (!okk) return 0;
            s = j + 1; continue;
        }
        s++;
    }
    if (s - 1 > q && t[s - 1] == ')') return s + 1 - p;
    return 0;
}

static int cache_registers(const Tr *tr, Buf *text, Buf *tmp) {
    // PUSH(c,  -> PUSHL(  ;  POP(c) -> POPL()  ;  C.gpr -> R_gpr ; C.xmm[n] -> X_n
    tmp->n = 0; if (!sub_word(text->p, text->n, "PUSH(c, ", "PUSHL(", tmp)) return 0;
    text->n = 0; if (!sub_word(tmp->p, tmp->n, "POP(c)", "POPL()", text)) return 0;
    tmp->n = 0; if (!sub_regs(text->p, text->n, tmp)) return 0;
    const char *t = tmp->p; size_t n = tmp->n;
    int used_g[8] = {0}, dirty_g[8] = {0};
    uint8_t used_x[64] = {0}, dirty_x[64] = {0}; int any_x = 0, any_g = 0, has_push = 0;
    for (size_t i = 0; i + 1 < n; i++) {
        if (i > 0 && isword((unsigned char)t[i - 1])) continue;
        if (t[i] == 'R' && t[i + 1] == '_') {
            size_t e2 = i + 2; while (e2 < n && isword((unsigned char)t[e2])) e2++;
            if (e2 == i + 2) continue;
            for (int g = 0; g < 8; g++) if (e2 - (i + 2) == 3 && strncmp(t + i + 2, GPRS[g], 3) == 0) {
                used_g[g] = 1; any_g = 1;
                if (assign_at(t, n, e2, 1)) dirty_g[g] = 1;
            }
        } else if (t[i] == 'X' && t[i + 1] == '_') {
            size_t d = i + 2, e2 = d; while (e2 < n && t[e2] >= '0' && t[e2] <= '9') e2++;
            if (e2 == d) continue;
            long v = 0; for (size_t q = d; q < e2 && v < 1000; q++) v = v * 10 + (t[q] - '0');
            if (v >= 64) continue;   // only xmm0-7 occur
            if (e2 >= n || !isword((unsigned char)t[e2])) { used_x[v] = 1; any_x = 1; }
            // dirty: X_n(\.\w+(\[[^\]]*\])?)?ASSIGN
            size_t p = e2; int dirty = 0;
            if (p < n && t[p] == '.' && p + 1 < n && isword((unsigned char)t[p + 1])) {
                size_t w = p + 1; while (w < n && isword((unsigned char)t[w])) w++;
                if (w < n && t[w] == '[') {
                    size_t b = w + 1; while (b < n && t[b] != ']') b++;
                    if (b < n && assign_at(t, n, b + 1, 0)) dirty = 1;
                }
                if (!dirty && assign_at(t, n, w, 0)) dirty = 1;
            }
            if (!dirty && assign_at(t, n, p, 0)) dirty = 1;
            if (dirty) dirty_x[v] = 1;
        } else if ((t[i] == 'P' && (strncmp(t + i, "PUSHL(", 6) == 0 || strncmp(t + i, "POPL()", 6) == 0))) has_push = 1;
    }
    if (has_push) { used_g[4] = dirty_g[4] = 1; any_g = 1; }
    if (!any_g && !any_x) { text->n = 0; return buf_add(text, t, n); }
    Buf flush = {0}, reload = {0}, reload_v = {0};
    int ok = 1;
    for (int g = 0; g < 8; g++) if (dirty_g[g]) ok &= buf_fmt(&flush, "C.%s = R_%s; ", GPRS[g], GPRS[g]);
    for (int x = 0; x < 64; x++) if (dirty_x[x]) ok &= buf_fmt(&flush, "C.xmm[%d] = X_%d; ", x, x);
    for (int g = 0; g < 8; g++) if (used_g[g]) ok &= buf_fmt(&reload, "R_%s = C.%s; ", GPRS[g], GPRS[g]);
    for (int x = 0; x < 64; x++) if (used_x[x]) ok &= buf_fmt(&reload, "X_%d = C.xmm[%d]; ", x, x);
    for (int g = 0; g < 8; g++) if (used_g[g] && (g == 0 || g == 1 || g == 2 || g == 4)) ok &= buf_fmt(&reload_v, "R_%s = C.%s; ", GPRS[g], GPRS[g]);
    for (int x = 0; x < 64; x++) if (used_x[x]) ok &= buf_fmt(&reload_v, "X_%d = C.xmm[%d]; ", x, x);
    const char *fl = flush.p ? flush.p : "", *rl = reload.p ? reload.p : "", *rv = reload_v.p ? reload_v.p : "";
    // _BARRIER
    text->n = 0;
    for (size_t i = 0; ok && i < n;) {
        size_t l = (t[i] == 'F' || t[i] == 'I' || t[i] == 'c' || t[i] == 'm' || t[i] == 'b' || t[i] == 'u') ? barrier_at(t, n, i) : 0;
        if (l) {
            int trim = tr->abi_trim && (t[i] == 'F' || t[i] == 'I' || strncmp(t + i, "call_ind", 8) == 0);
            ok &= buf_str(text, "{ ") && buf_str(text, fl) && buf_add(text, t + i, l) && buf_str(text, " ") && buf_str(text, trim ? rv : rl) && buf_str(text, "}");
            i += l; continue;
        }
        ok &= buf_add(text, t + i, 1); i++;
    }
    // _RETURN
    tmp->n = 0;
    for (size_t i = 0; ok && i < text->n;) {
        if (text->p[i] == 'r' && (i == 0 || !isword((unsigned char)text->p[i - 1])) && strncmp(text->p + i, "return;", 7) == 0) {
            ok &= buf_str(tmp, "{ ") && buf_str(tmp, fl) && buf_str(tmp, "return; }");
            i += 7; continue;
        }
        ok &= buf_add(tmp, text->p + i, 1); i++;
    }
    // the declarations after the line of "c->fn = "; the flush before the last }
    Buf decl = {0};
    int first = 1;
    for (int g = 0; g < 8; g++) if (used_g[g]) { ok &= buf_fmt(&decl, first ? "    uint32_t R_%s = C.%s" : ", R_%s = C.%s", GPRS[g], GPRS[g]); first = 0; }
    if (!first) ok &= buf_str(&decl, ";\n");
    first = 1;
    for (int x = 0; x < 64; x++) if (used_x[x]) { ok &= buf_fmt(&decl, first ? "    XMM X_%d = C.xmm[%d]" : ", X_%d = C.xmm[%d]", x, x); first = 0; }
    if (!first) ok &= buf_str(&decl, ";\n");
    text->n = 0;
    if (ok) {
        const char *fn = strstr(tmp->p, "c->fn = ");
        const char *nl = fn ? strchr(fn, '\n') : NULL;
        size_t j = nl ? (size_t)(nl - tmp->p) + 1 : tmp->n;
        size_t end = tmp->n;
        while (end > 0 && isspc((unsigned char)tmp->p[end - 1])) end--;
        size_t k = SIZE_MAX;
        if (end > 0 && tmp->p[end - 1] == '}') k = end - 1;
        if (k != SIZE_MAX && k < j) k = SIZE_MAX;   // (the function always has lines after its declarations)
        ok &= buf_add(text, tmp->p, j) && (decl.n ? buf_add(text, decl.p, decl.n) : 1);
        if (k == SIZE_MAX) ok &= buf_add(text, tmp->p + j, tmp->n - j);
        else ok &= buf_add(text, tmp->p + j, k - j) && buf_str(text, "    ") && buf_str(text, fl) && buf_str(text, "\n") && buf_add(text, tmp->p + k, tmp->n - k);
    }
    buf_free(&flush); buf_free(&reload); buf_free(&reload_v); buf_free(&decl);
    return ok;
}

// ================================================================ one function ================================================================

static void fn_reset(Ctx *c) {
    ar_reset(c);
    c->niv = c->nso = c->ntables = c->nout = c->nsorted = 0;
    if (c->sh) memset(c->sh, 0, (c->shmask + 1) * sizeof *c->sh);
    uset_clear(&c->labels);
}

// FuncGen(tr, addr).gen() into text (the post-passes included); 1 on success; 0 with c->err (Python: an exception)
static int gen(Ctx *c, uint32_t addr, Buf *text, Buf *tmp) {
    fn_reset(c);
    c->addr = addr;
    c->in_insn = 0;
    if (setjmp(c->fn_jb)) return 0;
    descend(c);
    // labels
    for (size_t k = 0; k < c->nsorted; k++) {
        const Ins *i = &c->iv[c->sorted[k]];
        if ((i->jump || is_loopish(i->mn)) && i->nops && i->ops[0].type == X86_OP_IMM) {
            uint32_t t = (uint32_t)(i->ops[0].imm & 0xFFFFFFFF);
            if (seen_ins(c, t) && !uset_add(&c->labels, t)) fail_oom(c);
        }
        const Table *tb = find_table(c, i->address);
        if (tb) for (size_t q = 0; q < tb->n; q++) if (!uset_add(&c->labels, tb->t[q])) fail_oom(c);
    }
    for (size_t k = 0; k < c->nsorted; k++) {
        const Ins *i = &c->iv[c->sorted[k]];
        int64_t nx = (int64_t)i->address + i->size;
        if (!ends_block(c, i) && (k + 1 >= c->nsorted || c->iv[c->sorted[k + 1]].address != nx) && seen_ins(c, nx))
            if (!uset_add(&c->labels, (uint32_t)nx)) fail_oom(c);
    }
    emit_line(c, sf(c, "void F_%x(CPU *c) {", addr));
    emit_line(c, sdup(c, "    uint32_t cf = 0, zf = 0, sf = 0, of = 0, pr = 0, df = 0; (void)cf; (void)zf; (void)sf; (void)of; (void)pr; (void)df;"));
    emit_line(c, sf(c, "    c->fn = 0x%xu;", addr));
    if (c->nsorted && c->iv[c->sorted[0]].address != addr) {
        if (!uset_add(&c->labels, addr)) fail_oom(c);
        emit_line(c, sf(c, "    goto L_%x;", addr));
    }
    const Ins *last = NULL;
    for (size_t k = 0; k < c->nsorted; k++) {
        const Ins *i = &c->iv[c->sorted[k]];
        if (last && !ends_block(c, last) && i->address != (int64_t)last->address + last->size)
            e(c, go_to(c, (int64_t)last->address + last->size, last->address));
        c->cur = i;
        if (uset_has(&c->labels, i->address)) emit_line(c, sf(c, "L_%x:;", i->address));
        // probes (Python: PROBES.get)
        for (size_t q = 0; q < c->tr->nprobes; q++)
            if (c->tr->probes[q].addr == i->address) ef(c, "{ void rt_log(const char *, ...); %s }   /* probe */", c->tr->probes[q].code);
        size_t n0 = c->nout;
        c->in_insn = 1;
        if (setjmp(c->ins_jb) == 0) {
            const char *pt = find_patch(c->tr, i->address);
            if (pt) ef(c, "%s   /* patched: %s %s */", pt, i->mn, i->op);
            else insn(c, i);
            c->in_insn = 0;
            for (size_t q = n0; q < c->nout; q++) c->out[q] = sf(c, "%s /*@%x*/", c->out[q], i->address);
        } else {
            c->in_insn = 0;
            ef(c, "bad_insn(c, 0x%xu, \"%s %s\"); return; /* %s */", i->address, i->mn, i->op, c->err);
            count_unsup(c, i->mn);
        }
        last = i;
    }
    if (last && !ends_block(c, last)) e(c, tail(c, (int64_t)last->address + last->size, last->address));
    emit_line(c, sdup(c, "}"));
    // "\n".join(f), then the passes
    Buf j = {0};
    int ok = 1;
    for (size_t q = 0; q < c->nout && ok; q++) ok = (q ? buf_str(&j, "\n") : 1) && buf_str(&j, c->out[q]);
    if (!ok) { buf_free(&j); fail_oom(c); }
    tmp->n = 0;
    ok = pass_stack(j.p, j.n, tmp);
    buf_free(&j);
    text->n = 0;
    ok = ok && pass_const(&c->tr->pe, tmp->p, tmp->n, text) && cache_registers(c->tr, text, tmp);
    if (!ok) fail_oom(c);
    return 1;
}

// the Python error text of the stub: str(ex).replace('"', "'").replace("\\", "/")[:80]
static void err_text(const char *e, char *out, size_t n) {
    size_t k = 0;
    for (; e[k] && k < 80 && k + 1 < n; k++) out[k] = e[k] == '"' ? '\'' : e[k] == '\\' ? '/' : e[k];
    out[k] = 0;
}

// _work for one function: appends its C to body
static int work_one(Ctx *c, uint32_t a, Buf *body, Buf *text, Buf *tmp) {
    const char *nat = find_native(c->tr, a);
    if (nat) {
        Buf orig = {0};
        if (gen(c, a, text, tmp)) {
            char hdr[64], rep[80];
            snprintf(hdr, sizeof hdr, "void F_%x(CPU *c) {", a);
            snprintf(rep, sizeof rep, "static void F_%x__orig(CPU *c) {", a);
            const char *p = strstr(text->p, hdr);
            int ok = 1;
            if (p) ok = buf_add(&orig, text->p, (size_t)(p - text->p)) && buf_str(&orig, rep) && buf_str(&orig, p + strlen(hdr));
            else ok = buf_add(&orig, text->p, text->n);
            if (!ok) { buf_free(&orig); return 0; }
        } else {
            if (c->oom) return 0;
            if (!buf_fmt(&orig, "static void F_%x__orig(CPU *c) { bad_insn(c, 0x%xu, \"no translation\"); }", a, a)) { buf_free(&orig); return 0; }
        }
        int ok = buf_fmt(body, "void %s(CPU *c);\nint rt_native_enabled(uint32_t a);\n%s\nvoid F_%x(CPU *c) { static int on = -1; if (on < 0) on = rt_native_enabled(0x%xu);"
                               " if (on) { %s(c); c->esp += 4; } else F_%x__orig(c); }", nat, orig.p ? orig.p : "", a, a, nat, a);
        buf_free(&orig);
        return ok;
    }
    if (gen(c, a, text, tmp)) return buf_add(body, text->p, text->n);
    if (c->oom) return 0;
    char et[96];
    err_text(c->err, et, sizeof et);
    char key[64]; snprintf(key, sizeof key, "<error: %.40s>", c->err);
    count_unsup(c, key);
    return buf_fmt(body, "void F_%x(CPU *c) { bad_insn(c, 0x%xu, \"translator error: %s\"); }", a, a, et);
}

// ================================================================ discovery (Translator.discover) ================================================================

typedef struct {
    const PE *pe;
    csh cs; cs_insn *ci;
    USet weak;
    const uint8_t **cbytes; // the bytes of each code section (read(va, vs))
} Disc;

static int byte_at(const Disc *d, int64_t a) {
    for (int k = 0; k < d->pe->nc; k++) {
        const Sec *s = &d->pe->s[d->pe->code[k]];
        if ((int64_t)s->va <= a && a < (int64_t)s->va + s->vs) return d->cbytes[k][a - s->va];
    }
    return -1;
}
// one instruction (address, size, mnemonic, op_str) of disasm_lite; 0 when it does not decode
static int lite1(Disc *d, int64_t a, size_t n, const uint8_t *b, uint16_t *size, const char **mn, const char **op) {
    const uint8_t *p = b; size_t sz = n; uint64_t addr = (uint64_t)a;
    if (!cs_disasm_iter(d->cs, &p, &sz, &addr, d->ci)) return 0;
    *size = d->ci->size; *mn = d->ci->mnemonic; *op = d->ci->op_str;
    return 1;
}
static int ends_flow(Disc *d, int64_t a) {
    for (int k = 2; k < 8; k++) {
        uint8_t b[8];
        if (!pe_read(d->pe, a - k, (size_t)k, b)) continue;
        uint16_t sz; const char *mn, *op;
        if (lite1(d, a - k, (size_t)k, b, &sz, &mn, &op) && sz == k && (starts(mn, "ret") || strcmp(mn, "jmp") == 0 || strcmp(mn, "int3") == 0)) return 1;
    }
    return 0;
}
static int is_code_start(const PE *pe, int64_t a) { for (int k = 0; k < pe->nc; k++) if (pe->s[pe->code[k]].va == a) return 1; return 0; }
// boundary(a) with weak_ok (the only form the Python tool calls); -1: out of memory
static int boundary(Disc *d, int64_t a) {
    const PE *pe = d->pe;
    if (!pe_is_code(pe, a)) return 0;
    if (is_code_start(pe, a)) return 1;
    int p = byte_at(d, a - 1);
    if ((a % 16 == 0 && (p == 0xCC || p == 0xC3 || p == 0x90)) || (p == 0xCC && byte_at(d, a - 2) == 0xCC)) return 1;
    if (a % 16 == 0 && byte_at(d, a - 3) == 0xC2) return 1;
    if (a % 16 == 0 && pe_is_code(pe, a - 4)) { uint32_t v; if (pe_u32(pe, a - 4, &v) && pe_is_code(pe, v)) return 1; }
    if (p == 0xC3 || byte_at(d, a - 3) == 0xC2 || byte_at(d, a - 5) == 0xE9 || ends_flow(d, a)) {
        if (!uset_add(&d->weak, (uint32_t)a)) return -1;
        return 1;
    }
    return 0;
}

// the first \*\s*4\s*\+\s*0x([0-9a-f]+)\] of s
static int table_operand(const char *s, uint64_t *ta) {
    for (const char *p = strchr(s, '*'); p; p = strchr(p + 1, '*')) {
        const char *q = p + 1;
        while (isspc((unsigned char)*q)) q++;
        if (*q != '4') continue;
        q++;
        while (isspc((unsigned char)*q)) q++;
        if (*q != '+') continue;
        q++;
        while (isspc((unsigned char)*q)) q++;
        if (q[0] != '0' || q[1] != 'x') continue;
        q += 2;
        const char *h = q;
        while (ishex((unsigned char)*q)) q++;
        if (q == h || *q != ']') continue;
        if (!parse_hex(h, (size_t)(q - h), ta)) continue;
        return 1;
    }
    return 0;
}

static int discover(Tr *tr, FILE *log, char *why, size_t wn) {
    const PE *pe = &tr->pe;
    Disc d = {0};
    d.pe = pe;
    int ret = 0;
    PySet starts_ = {0};
    USet scanned = {0}, seen = {0}, noret = {0};
    I64s work = {0}, stack = {0};
    uint8_t *blob = NULL;
    Ctx dummy;   // i64_push needs a Ctx only for its out-of-memory jump
    memset(&dummy, 0, sizeof dummy);
    if (setjmp(dummy.fn_jb)) { snprintf(why, wn, "out of memory"); goto out; }
    if (cs_open(CS_ARCH_X86, CS_MODE_32, &d.cs) != CS_ERR_OK) { snprintf(why, wn, "capstone: cs_open failed"); goto out; }
    d.ci = cs_malloc(d.cs);
    d.cbytes = calloc((size_t)pe->nc, sizeof *d.cbytes);
    if (!d.ci || !d.cbytes || !pyset_init(&starts_)) { snprintf(why, wn, "out of memory"); goto out; }
    for (int k = 0; k < pe->nc; k++) {
        const Sec *s = &pe->s[pe->code[k]];
        uint8_t *b = malloc(s->vs ? s->vs : 1);
        if (!b) { snprintf(why, wn, "out of memory"); goto out; }
        if (!pe_read(pe, s->va, s->vs, b)) { free(b); snprintf(why, wn, "a code section is outside the image"); goto out; }
        d.cbytes[k] = b;
    }
    if (!pyset_add(&starts_, pe->entry)) { snprintf(why, wn, "out of memory"); goto out; }
    // code pointers in data
    for (int k = 0; k < pe->ns; k++) {
        const Sec *s = &pe->s[k];
        if (s->ch & 0x20000000u) continue;
        if ((s->namelen == 5 && memcmp(s->name, ".rsrc", 5) == 0) || (s->namelen == 6 && memcmp(s->name, ".reloc", 6) == 0) ||
            (s->namelen == 5 && memcmp(s->name, ".bind", 5) == 0)) continue;
        size_t n = s->fs ? (s->vs < s->fs ? s->vs : s->fs) : 0;
        free(blob); blob = malloc(n ? n : 1);
        if (!blob) { snprintf(why, wn, "out of memory"); goto out; }
        if (n && !pe_read(pe, s->va, n, blob)) { snprintf(why, wn, "a section is outside the image"); goto out; }
        for (size_t o = 0; o + 3 < n; o += 4) {
            uint32_t v = blob[o] | (uint32_t)blob[o + 1] << 8 | (uint32_t)blob[o + 2] << 16 | (uint32_t)blob[o + 3] << 24;
            if (pe->text_lo <= v && v < pe->text_hi) {
                int b = boundary(&d, v);
                if (b < 0 || (b && !pyset_add(&starts_, v))) { snprintf(why, wn, "out of memory"); goto out; }
            }
        }
    }
    size_t n_data = starts_.used;
    for (size_t k = 0; k < pe->niat; k++) if (noreturn_import(pe->iat[k].name) && !uset_add(&noret, pe->iat[k].slot)) { snprintf(why, wn, "out of memory"); goto out; }
    // work = list(starts): the table order of the CPython set
    for (size_t j = 0; j <= starts_.mask; j++) if (starts_.t[j]) i64_push(&dummy, &work, (int64_t)(starts_.t[j] - 1));
    while (work.n) {
        int64_t s0 = work.v[--work.n];
        if ((s0 >= 0 && s0 <= UINT32_MAX && uset_has(&scanned, (uint32_t)s0)) || !pe_is_code(pe, s0)) continue;
        if (!uset_add(&scanned, (uint32_t)s0)) { snprintf(why, wn, "out of memory"); goto out; }
        uset_clear(&seen);
        stack.n = 0;
        i64_push(&dummy, &stack, s0);
        while (stack.n) {
            int64_t a = stack.v[--stack.n];
            while (!(a >= 0 && a <= UINT32_MAX && uset_has(&seen, (uint32_t)a)) && pe_is_code(pe, a) &&
                   (a == s0 || !pyset_has(&starts_, (uint64_t)a) || uset_has(&d.weak, (uint32_t)a))) {
                if (!uset_add(&seen, (uint32_t)a)) { snprintf(why, wn, "out of memory"); goto out; }
                int64_t end = pe_code_end(pe, a);
                size_t bn = end - a < 16 ? (size_t)(end - a) : 16;
                uint8_t b[16];
                uint16_t sz; const char *mn0, *op0;
                if (!pe_read(pe, a, bn, b) || !lite1(&d, a, bn, b, &sz, &mn0, &op0)) break;
                char mn[CS_MNEMONIC_SIZE], opst[160];
                snprintf(mn, sizeof mn, "%s", mn0); snprintf(opst, sizeof opst, "%s", op0);
                int64_t ia = a, nxt = ia + sz;
                int has_t = 0; uint64_t t = 0;
                if (starts(opst, "0x") && (strcmp(mn, "call") == 0 || mn[0] == 'j' || starts(mn, "loop"))) {
                    if (!parse_hex(opst, strlen(opst), &t)) { snprintf(why, wn, "the target of %s %s at 0x%llx is not a number", mn, opst, (long long)ia); goto out; }
                    has_t = 1;
                }
                if (strcmp(mn, "call") == 0) {
                    if (has_t && pe_is_code(pe, (int64_t)t) && (int64_t)t != nxt) {
                        uset_del(&d.weak, (uint32_t)t);
                        if (!pyset_has(&starts_, t)) { if (!pyset_add(&starts_, t)) { snprintf(why, wn, "out of memory"); goto out; } i64_push(&dummy, &work, (int64_t)t); }
                    }
                    const char *br = strstr(opst, "dword ptr [0x");
                    if (br) {
                        const char *s1 = strchr(opst, '[') + 1;
                        const char *s2 = strchr(s1, '[');
                        size_t l = s2 ? (size_t)(s2 - s1) : strlen(s1);
                        while (l && s1[l - 1] == ']') l--;
                        uint64_t slot;
                        if (!parse_hex(s1, l, &slot)) { snprintf(why, wn, "the operand of call %s at 0x%llx is not an address", opst, (long long)ia); goto out; }
                        if (slot <= UINT32_MAX && uset_has(&noret, (uint32_t)slot)) break;
                    }
                } else if (mn[0] == 'j' || starts(mn, "loop")) {
                    if (has_t && pe_is_code(pe, (int64_t)t) && ((int64_t)t == s0 || !pyset_has(&starts_, t) || uset_has(&d.weak, (uint32_t)t))) i64_push(&dummy, &stack, (int64_t)t);
                    uint64_t ta;
                    if (strcmp(mn, "jmp") == 0 && starts(opst, "dword ptr [") && strstr(opst, "*4") && table_operand(opst, &ta)) {
                        for (int64_t k = 0; k < 1024; k++) {
                            uint32_t v;
                            if (!pe_u32(pe, (int64_t)ta + 4 * k, &v)) break;
                            int64_t dv = (int64_t)v - ia;
                            if (!pe_is_code(pe, v) || (dv < 0 ? -dv : dv) > 0x40000) break;
                            i64_push(&dummy, &stack, v);
                        }
                    }
                } else {
                    // IMM: 0x([0-9a-f]{6,8})\b
                    for (size_t p = 0; opst[p];) {
                        if (opst[p] == '0' && opst[p + 1] == 'x') {
                            size_t h = p + 2, q = h;
                            while (ishex((unsigned char)opst[q])) q++;
                            size_t len = q - h;
                            if (len >= 6 && len <= 8 && !isword((unsigned char)opst[q])) {
                                uint64_t v; parse_hex(opst + h, len, &v);
                                if (IN(mn, "push", "mov", "lea") && pe->text_lo <= v && v < pe->text_hi) {
                                    int bd = boundary(&d, (int64_t)v);
                                    if (bd < 0) { snprintf(why, wn, "out of memory"); goto out; }
                                    if (bd && !pyset_has(&starts_, v)) {
                                        if (!pyset_add(&starts_, v)) { snprintf(why, wn, "out of memory"); goto out; }
                                        i64_push(&dummy, &work, (int64_t)v);
                                    }
                                }
                                p = q; continue;
                            }
                        }
                        p++;
                    }
                }
                if (IN(mn, "ret", "retf", "jmp", "int3", "ud2", "hlt") || (strcmp(mn, "int") == 0 && strcmp(opst, "0x29") == 0)) break;
                a = nxt;
            }
        }
    }
    // funcs = sorted(starts); strong = starts - weak
    tr->nfuncs = starts_.used;
    tr->funcs = malloc((tr->nfuncs ? tr->nfuncs : 1) * sizeof *tr->funcs);
    if (!tr->funcs) { snprintf(why, wn, "out of memory"); goto out; }
    size_t w = 0;
    for (size_t j = 0; j <= starts_.mask; j++) if (starts_.t[j]) tr->funcs[w++] = (uint32_t)(starts_.t[j] - 1);
    for (size_t a = 1; a < w; a++) { uint32_t v = tr->funcs[a]; size_t b = a; while (b && tr->funcs[b - 1] > v) { tr->funcs[b] = tr->funcs[b - 1]; b--; } tr->funcs[b] = v; }
    for (size_t k = 0; k < w; k++) {
        if (!uset_add(&tr->funcset, tr->funcs[k])) { snprintf(why, wn, "out of memory"); goto out; }
        if (!uset_has(&d.weak, tr->funcs[k]) && !uset_add(&tr->strong, tr->funcs[k])) { snprintf(why, wn, "out of memory"); goto out; }
    }
    if (log) {
        fprintf(log, "weak starts (boundary only by a preceding ret/jmp): %zu\n", d.weak.n);
        fprintf(log, "functions: %zu (from data pointers %zu, entry/calls/immediates %zu)\n", tr->nfuncs, n_data, tr->nfuncs - n_data);
    }
    if (getenv("W32_TRANSLATE_DUMP_ORDER")) {   // debugging: the start order of the closure (compare with the Python tool)
        FILE *f = fopen(getenv("W32_TRANSLATE_DUMP_ORDER"), "w");
        if (f) { for (size_t j = 0; j <= starts_.mask; j++) if (starts_.t[j]) fprintf(f, "%llx\n", (unsigned long long)(starts_.t[j] - 1)); fclose(f); }
    }
    ret = 1;
out:
    free(blob); free(work.v); free(stack.v);
    uset_free(&scanned); uset_free(&seen); uset_free(&noret); uset_free(&d.weak);
    free(starts_.t);
    if (d.cbytes) for (int k = 0; k < pe->nc; k++) free((void *)d.cbytes[k]);
    free(d.cbytes);
    if (d.ci) cs_free(d.ci, 1);
    if (d.cs) cs_close(&d.cs);
    return ret;
}

// ================================================================ input files ================================================================

static char *read_file(const char *path, size_t *n, char *why, size_t wn) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(why, wn, "%s: %s", path, strerror(errno)); return NULL; }
    Buf b = {0};
    char tmp[65536];
    size_t k;
    while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) if (!buf_add(&b, tmp, k)) { fclose(f); buf_free(&b); snprintf(why, wn, "out of memory"); return NULL; }
    int err = ferror(f);
    fclose(f);
    if (err) { buf_free(&b); snprintf(why, wn, "%s: read error", path); return NULL; }
    if (!b.p && !buf_add(&b, "", 0)) { snprintf(why, wn, "out of memory"); return NULL; }
    *n = b.n;
    return b.p;
}
// the lines of a file: calls fn for each line (without the newline)
typedef int (*LineFn)(void *ctx, char *line, char *why, size_t wn);
static int each_line(const char *path, LineFn fn, void *ctx, char *why, size_t wn) {
    size_t n;
    char *s = read_file(path, &n, why, wn);
    if (!s) return 0;
    int ok = 1;
    char *p = s, *end = s + n;
    while (ok && p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (nl) *nl = 0;
        ok = fn(ctx, p, why, wn);
        p = nl ? nl + 1 : end;
    }
    free(s);
    return ok;
}
static char *strip(char *s) {
    while (*s && isspc((unsigned char)*s)) s++;
    size_t l = strlen(s);
    while (l && isspc((unsigned char)s[l - 1])) s[--l] = 0;
    return s;
}
// line.split(None, 1): the first word and the rest (both stripped)
static int split1(char *s, char **a, char **rest) {
    s = strip(s);
    if (!*s) return 0;
    char *p = s;
    while (*p && !isspc((unsigned char)*p)) p++;
    *a = s;
    if (!*p) { *rest = NULL; return 1; }
    *p++ = 0;
    while (*p && isspc((unsigned char)*p)) p++;
    *rest = p;
    return 1;
}

typedef struct { Tr *tr; const char *path; size_t cap; } LineCtx;
static int line_patch(void *cx, char *line, char *why, size_t wn) {
    LineCtx *l = cx; Tr *tr = l->tr;
    char *h = strchr(line, '#'); if (h) *h = 0;
    char *a, *code;
    if (!split1(line, &a, &code)) return 1;
    uint64_t v;
    if (!code || !*code || !parse_hex(a, strlen(a), &v) || v > UINT32_MAX) { snprintf(why, wn, "%s: a line is not 'address code': %s", l->path, a); return 0; }
    for (size_t k = 0; k < tr->npatches; k++) if (tr->patches[k].addr == v) { char *d = strdup(code); if (!d) { snprintf(why, wn, "out of memory"); return 0; } free(tr->patches[k].code); tr->patches[k].code = d; return 1; }
    if (tr->npatches == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; Patch *p = realloc(tr->patches, l->cap * sizeof *p); if (!p) { snprintf(why, wn, "out of memory"); return 0; } tr->patches = p; }
    char *d = strdup(code); if (!d) { snprintf(why, wn, "out of memory"); return 0; }
    tr->patches[tr->npatches].addr = (uint32_t)v; tr->patches[tr->npatches].code = d; tr->npatches++;
    return 1;
}
static int line_probe(void *cx, char *line, char *why, size_t wn) {
    LineCtx *l = cx; Tr *tr = l->tr;
    char *s = strip(line);
    if (!*s || *s == '#') return 1;
    char *a, *code;
    uint64_t v;
    if (!split1(s, &a, &code) || !code || !parse_hex(a, strlen(a), &v) || v > UINT32_MAX) { snprintf(why, wn, "%s: a line is not 'address code'", l->path); return 0; }
    if (tr->nprobes == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; Probe *p = realloc(tr->probes, l->cap * sizeof *p); if (!p) { snprintf(why, wn, "out of memory"); return 0; } tr->probes = p; }
    char *d = strdup(code); if (!d) { snprintf(why, wn, "out of memory"); return 0; }
    tr->probes[tr->nprobes].addr = (uint32_t)v; tr->probes[tr->nprobes].code = d; tr->nprobes++;
    return 1;
}
static int line_native(void *cx, char *line, char *why, size_t wn) {
    LineCtx *l = cx; Tr *tr = l->tr;
    char *h = strchr(line, '#'); if (h) *h = 0;
    char *a, *rest;
    if (!split1(line, &a, &rest)) return 1;
    char *nm = rest; if (nm) { char *q = nm; while (*q && !isspc((unsigned char)*q)) q++; *q = 0; }
    uint64_t v;
    if (!nm || !*nm || !parse_hex(a, strlen(a), &v) || v > UINT32_MAX) { snprintf(why, wn, "%s: a line is not 'address name'", l->path); return 0; }
    for (size_t k = 0; k < tr->nnatives; k++) if (tr->natives[k].addr == v) { char *d = strdup(nm); if (!d) { snprintf(why, wn, "out of memory"); return 0; } free(tr->natives[k].name); tr->natives[k].name = d; return 1; }
    if (tr->nnatives == l->cap) { l->cap = l->cap ? l->cap * 2 : 16; Native *p = realloc(tr->natives, l->cap * sizeof *p); if (!p) { snprintf(why, wn, "out of memory"); return 0; } tr->natives = p; }
    char *d = strdup(nm); if (!d) { snprintf(why, wn, "out of memory"); return 0; }
    tr->natives[tr->nnatives].addr = (uint32_t)v; tr->natives[tr->nnatives].name = d; tr->nnatives++;
    return 1;
}
static int line_provided(void *cx, char *line, char *why, size_t wn) {
    LineCtx *l = cx; Tr *tr = l->tr;
    char *s = strip(line);
    if (!*s) return 1;
    if (tr->nprovided == l->cap) { l->cap = l->cap ? l->cap * 2 : 512; char **p = realloc(tr->provided, l->cap * sizeof *p); if (!p) { snprintf(why, wn, "out of memory"); return 0; } tr->provided = p; }
    if (!(tr->provided[tr->nprovided] = strdup(s))) { snprintf(why, wn, "out of memory"); return 0; }
    tr->nprovided++;
    return 1;
}
static int cmp_patch(const void *a, const void *b) { uint32_t x = ((const Patch *)a)->addr, y = ((const Patch *)b)->addr; return x < y ? -1 : x > y; }
static int cmp_native(const void *a, const void *b) { uint32_t x = ((const Native *)a)->addr, y = ((const Native *)b)->addr; return x < y ? -1 : x > y; }
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// ================================================================ output ================================================================

static int write_if_changed(const char *path, const char *s, size_t n, char *why, size_t wn) {
    FILE *f = fopen(path, "rb");
    if (f) {
        int same = 1; size_t off = 0; char tmp[65536]; size_t k;
        while (same && (k = fread(tmp, 1, sizeof tmp, f)) > 0) { if (off + k > n || memcmp(tmp, s + off, k) != 0) same = 0; off += k; }
        if (ferror(f)) same = 0;
        fclose(f);
        if (same && off == n) return 1;
    }
    char tmpp[4096];
    int r = snprintf(tmpp, sizeof tmpp, "%s.tmp", path);
    if (r <= 0 || (size_t)r >= sizeof tmpp) { snprintf(why, wn, "%s: the path is too long", path); return 0; }
    f = fopen(tmpp, "wb");
    if (!f) { snprintf(why, wn, "%s: %s", tmpp, strerror(errno)); return 0; }
    int ok = fwrite(s, 1, n, f) == n;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { snprintf(why, wn, "%s: %s", tmpp, strerror(errno)); unlink(tmpp); return 0; }
    if (rename(tmpp, path) != 0) { snprintf(why, wn, "%s: %s", path, strerror(errno)); unlink(tmpp); return 0; }
    return 1;
}

typedef struct {
    const Tr *tr;
    const W32Translate *opt;
    const uint32_t *order; size_t norder;
    int nchunks;
    atomic_int next, done, failed;
    pthread_mutex_t mu;
    char why[1024];
    // the merged unsupported counts
    struct { char name[48]; long n; } *unsup; size_t nunsup, cunsup;
} Job;

static void job_fail(Job *j, const char *msg) {
    pthread_mutex_lock(&j->mu);
    if (!atomic_load(&j->failed)) snprintf(j->why, sizeof j->why, "%s", msg);
    atomic_store(&j->failed, 1);
    pthread_mutex_unlock(&j->mu);
}

static void *worker(void *arg) {
    Job *j = arg;
    Ctx *c = calloc(1, sizeof *c);
    Buf body = {0}, text = {0}, tmp = {0};
    if (!c) { job_fail(j, "out of memory"); return NULL; }
    c->tr = j->tr;
    if (cs_open(CS_ARCH_X86, CS_MODE_32, &c->cs) != CS_ERR_OK) { job_fail(j, "capstone: cs_open failed"); free(c); return NULL; }
    cs_option(c->cs, CS_OPT_DETAIL, CS_OPT_ON);
    c->ci = cs_malloc(c->cs);
    if (!c->ci) { job_fail(j, "out of memory"); goto out; }
    static const char hdr[] = "#include \"cpu32.h\"\n#include \"funcs.h\"\n#pragma clang diagnostic ignored \"-Wunused-label\"\n";
    for (;;) {
        if (atomic_load(&j->failed)) break;
        int k = atomic_fetch_add(&j->next, 1);
        if (k >= j->nchunks) break;
        body.n = 0;
        if (!buf_str(&body, hdr)) { job_fail(j, "out of memory"); break; }
        size_t lo = (size_t)k * FUNCS_PER_FILE, hi = lo + FUNCS_PER_FILE < j->norder ? lo + FUNCS_PER_FILE : j->norder;
        int ok = 1;
        for (size_t q = lo; q < hi && ok; q++) {
            if (q > lo && !buf_str(&body, "\n\n")) { ok = 0; break; }
            ok = work_one(c, j->order[q], &body, &text, &tmp);
        }
        if (!ok || !buf_str(&body, "\n")) { job_fail(j, c->oom || !ok ? "out of memory" : "out of memory"); break; }
        char path[4096], why[1024];
        int r = snprintf(path, sizeof path, "%s/code_%04d.c", j->opt->outdir, k);
        if (r <= 0 || (size_t)r >= sizeof path) { job_fail(j, "the output folder path is too long"); break; }
        if (!write_if_changed(path, body.p, body.n, why, sizeof why)) { job_fail(j, why); break; }
        int d = atomic_fetch_add(&j->done, 1) + 1;
        if (j->opt->progress) j->opt->progress(j->opt->progress_ctx, d, j->nchunks);
    }
    // merge the unsupported counts
    pthread_mutex_lock(&j->mu);
    for (size_t q = 0; q < c->nunsup; q++) {
        size_t r = 0;
        for (; r < j->nunsup; r++) if (strcmp(j->unsup[r].name, c->unsup[q].name) == 0) break;
        if (r == j->nunsup) {
            if (j->nunsup == j->cunsup) {
                size_t cap = j->cunsup ? j->cunsup * 2 : 16;
                void *p = realloc(j->unsup, cap * sizeof *j->unsup);
                if (!p) continue;
                j->unsup = p; j->cunsup = cap;
            }
            memcpy(j->unsup[r].name, c->unsup[q].name, sizeof j->unsup[r].name);
            j->unsup[r].n = 0; j->nunsup++;
        }
        j->unsup[r].n += c->unsup[q].n;
    }
    pthread_mutex_unlock(&j->mu);
out:
    buf_free(&body); buf_free(&text); buf_free(&tmp);
    while (c->arena) { Chunk *nx = c->arena->next; free(c->arena); c->arena = nx; }
    free(c->iv); free(c->so); free(c->sh); free(c->tables); free(c->out); free(c->unsup);
    uset_free(&c->labels);
    if (c->ci) cs_free(c->ci, 1);
    cs_close(&c->cs);
    free(c);
    return NULL;
}

typedef struct { char sym[512]; const char *name; } ImpSorted;
static int cmp_imp(const void *a, const void *b) {
    const ImpSorted *x = a, *y = b;
    int r = strcmp(x->sym, y->sym);
    return r ? r : strcmp(x->name, y->name);
}

static void tr_free(Tr *tr) {
    pe_free(&tr->pe);
    free(tr->funcs);
    uset_free(&tr->funcset); uset_free(&tr->strong);
    for (size_t k = 0; k < tr->npatches; k++) free(tr->patches[k].code);
    for (size_t k = 0; k < tr->nprobes; k++) free(tr->probes[k].code);
    for (size_t k = 0; k < tr->nnatives; k++) free(tr->natives[k].name);
    for (size_t k = 0; k < tr->nprovided; k++) free(tr->provided[k]);
    free(tr->patches); free(tr->probes); free(tr->natives); free(tr->provided);
}

static int cmp_unsup(const void *a, const void *b) {
    const struct { char name[48]; long n; } *x = a, *y = b;
    return x->n != y->n ? (x->n < y->n ? 1 : -1) : strcmp(x->name, y->name);
}

int w32_translate(const W32Translate *opt, char *why, size_t wn) {
    Tr tr; memset(&tr, 0, sizeof tr);
    Job job; memset(&job, 0, sizeof job);
    Buf t = {0};
    ImpSorted *imps = NULL;
    pthread_t *th = NULL;
    int nth = 0, ret = 0;
    pthread_mutex_init(&job.mu, NULL);
    if (!opt->exe || !opt->outdir) { snprintf(why, wn, "no executable or output folder"); goto out; }
    if (!opt->provided) { snprintf(why, wn, "the list of the import shims of the runtime (provided) is required"); goto out; }
    tr.abi_trim = opt->abi_trim;
    LineCtx lc = {&tr, NULL, 0};
    if (opt->patches) { lc.path = opt->patches; lc.cap = 0; if (!each_line(opt->patches, line_patch, &lc, why, wn)) goto out; qsort(tr.patches, tr.npatches, sizeof *tr.patches, cmp_patch); }
    if (opt->probes) {
        lc.path = opt->probes; lc.cap = 0;
        if (!each_line(opt->probes, line_probe, &lc, why, wn)) goto out;
        // stable by address: insertion sort keeps the file order within an address
        for (size_t a = 1; a < tr.nprobes; a++) { Probe v = tr.probes[a]; size_t b = a; while (b && tr.probes[b - 1].addr > v.addr) { tr.probes[b] = tr.probes[b - 1]; b--; } tr.probes[b] = v; }
    }
    if (opt->native) { lc.path = opt->native; lc.cap = 0; if (!each_line(opt->native, line_native, &lc, why, wn)) goto out; qsort(tr.natives, tr.nnatives, sizeof *tr.natives, cmp_native); }
    lc.path = opt->provided; lc.cap = 0;
    if (!each_line(opt->provided, line_provided, &lc, why, wn)) goto out;
    qsort(tr.provided, tr.nprovided, sizeof *tr.provided, cmp_str);
    if (!pe_load(&tr.pe, opt->exe, why, wn)) goto out;
    if (!discover(&tr, opt->log, why, wn)) goto out;
    if (mkdir(opt->outdir, 0755) != 0 && errno != EEXIST) { snprintf(why, wn, "%s: %s", opt->outdir, strerror(errno)); goto out; }
    job.tr = &tr; job.opt = opt;
    job.order = opt->only ? opt->only : tr.funcs;
    job.norder = opt->only ? opt->nonly : tr.nfuncs;
    job.nchunks = (int)((job.norder + FUNCS_PER_FILE - 1) / FUNCS_PER_FILE);
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int jobs = opt->jobs > 0 ? opt->jobs : (int)(ncpu > 0 ? ncpu : 1);
    if (jobs > 64) jobs = 64;
    if (jobs > job.nchunks) jobs = job.nchunks > 0 ? job.nchunks : 1;
    th = calloc((size_t)jobs, sizeof *th);
    if (!th) { snprintf(why, wn, "out of memory"); goto out; }
    for (; nth < jobs; nth++)
        if (pthread_create(&th[nth], NULL, worker, &job) != 0) { job_fail(&job, "can't start a worker thread"); break; }
    for (int k = 0; k < nth; k++) pthread_join(th[k], NULL);
    if (atomic_load(&job.failed)) { snprintf(why, wn, "%s", job.why); goto out; }
    // the stale code files
    DIR *dir = opendir(opt->outdir);
    if (!dir) { snprintf(why, wn, "%s: %s", opt->outdir, strerror(errno)); goto out; }
    struct dirent *de;
    while ((de = readdir(dir))) {
        int k; char tail_[8];
        if (sscanf(de->d_name, "code_%d.%7s", &k, tail_) == 2 && strcmp(tail_, "c") == 0 && starts(de->d_name, "code_") && ends(de->d_name, ".c")) {
            char want[32]; snprintf(want, sizeof want, "code_%04d.c", k);
            if (strcmp(want, de->d_name) == 0 && k >= 0 && k < job.nchunks) continue;
            char p[4096]; snprintf(p, sizeof p, "%s/%s", opt->outdir, de->d_name);
            if (unlink(p) != 0) { snprintf(why, wn, "%s: %s", p, strerror(errno)); closedir(dir); goto out; }
        } else if (starts(de->d_name, "code_") && ends(de->d_name, ".c")) {
            char p[4096]; snprintf(p, sizeof p, "%s/%s", opt->outdir, de->d_name);
            if (unlink(p) != 0) { snprintf(why, wn, "%s: %s", p, strerror(errno)); closedir(dir); goto out; }
        }
    }
    closedir(dir);
    // the imports, sorted by their symbol (one entry for each distinct name)
    const PE *pe = &tr.pe;
    imps = calloc(pe->niat ? pe->niat : 1, sizeof *imps);
    if (!imps) { snprintf(why, wn, "out of memory"); goto out; }
    size_t nimp = 0;
    for (size_t k = 0; k < pe->niat; k++) {
        int dup = 0;
        for (size_t q = 0; q < nimp && !dup; q++) if (strcmp(imps[q].name, pe->iat[k].name) == 0) dup = 1;
        if (dup) continue;
        imps[nimp].name = pe->iat[k].name; imp_sym(pe->iat[k].name, imps[nimp].sym, sizeof imps[nimp].sym); nimp++;
    }
    qsort(imps, nimp, sizeof *imps, cmp_imp);
    char path[4096];
    int ok = buf_str(&t, "#pragma once\n");
    for (size_t k = 0; k < tr.nfuncs && ok; k++) ok = buf_fmt(&t, "void F_%x(CPU *c);\n", tr.funcs[k]);
    for (size_t k = 0; k < nimp && ok; k++) ok = buf_fmt(&t, "void I_%s(CPU *c);\n", imps[k].sym);
    if (!ok) { snprintf(why, wn, "out of memory"); goto out; }
    snprintf(path, sizeof path, "%s/funcs.h", opt->outdir);
    if (!write_if_changed(path, t.p, t.n, why, wn)) goto out;
    t.n = 0;
    ok = buf_str(&t, "#include \"cpu32.h\"\n#include \"tables32.h\"\n#include \"funcs.h\"\n") && buf_str(&t, "const GFunc gfuncs[] = {\n");
    for (size_t k = 0; k < tr.nfuncs && ok; k++) ok = buf_fmt(&t, "  {0x%xu, F_%x},\n", tr.funcs[k], tr.funcs[k]);
    ok = ok && buf_str(&t, "};\n") && buf_fmt(&t, "const int ngfuncs = %zu;\n", tr.nfuncs);
    for (size_t k = 0; k < nimp && ok; k++) {
        char pn[520]; snprintf(pn, sizeof pn, "I_%s", imps[k].sym);
        if (!is_provided(&tr, pn)) ok = buf_fmt(&t, "void I_%s(CPU *c) { unimpl_import(c, \"", imps[k].sym) && buf_latin1s(&t, imps[k].name) && buf_str(&t, "\"); }\n");
    }
    ok = ok && buf_str(&t, "const GImport gimports[] = {\n");
    for (size_t k = 0; k < pe->niat && ok; k++) {
        char s[512]; imp_sym(pe->iat[k].name, s, sizeof s);
        ok = buf_fmt(&t, "  {0x%xu, \"", pe->iat[k].slot) && buf_latin1s(&t, pe->iat[k].name) && buf_fmt(&t, "\", I_%s},\n", s);
    }
    ok = ok && buf_str(&t, "};\n") && buf_fmt(&t, "const int ngimports = %zu;\n", pe->niat) && buf_str(&t, "const GSection gsections[] = {\n");
    for (int k = 0; k < pe->ns && ok; k++) {
        const Sec *s = &pe->s[k];
        ok = buf_str(&t, "  {\"") && buf_latin1(&t, s->name, (size_t)s->namelen) && buf_fmt(&t, "\", 0x%xu, 0x%x, 0x%x, 0x%x, 0x%x},\n", s->va, s->vs, s->fo, s->fs, s->ch);
    }
    ok = ok && buf_str(&t, "};\n") && buf_fmt(&t, "const int ngsections = %d;\n", pe->ns) &&
         buf_fmt(&t, "const uint32_t gimage_base = 0x%xu, gimage_size = 0x%xu, gentry = 0x%xu;\n", pe->base, pe->size, pe->entry);
    if (!ok) { snprintf(why, wn, "out of memory"); goto out; }
    snprintf(path, sizeof path, "%s/tables.c", opt->outdir);
    if (!write_if_changed(path, t.p, t.n, why, wn)) goto out;
    if (opt->log) {
        fprintf(opt->log, "translated %zu functions (%zu native), %zu imports\n", job.norder, tr.nnatives, pe->niat);
        qsort(job.unsup, job.nunsup, sizeof *job.unsup, cmp_unsup);
        fprintf(opt->log, "UNSUPPORTED:");
        for (size_t k = 0; k < job.nunsup && k < 60; k++) fprintf(opt->log, " %s %ld%s", job.unsup[k].name, job.unsup[k].n, k + 1 < job.nunsup && k + 1 < 60 ? "," : "");
        fprintf(opt->log, "\n");
    }
    ret = 1;
out:
    free(th); free(imps); free(job.unsup); buf_free(&t);
    pthread_mutex_destroy(&job.mu);
    tr_free(&tr);
    return ret;
}
