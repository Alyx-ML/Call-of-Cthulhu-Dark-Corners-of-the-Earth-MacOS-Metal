// The registry for 32-bit guests (advapi32 Reg*, shlwapi SH*Value): keys and values live in memory and are saved to
// <data folder>/registry.txt after every change, so settings a game writes persist between launches.
//
// File format, one value per line: <key path> TAB <value name> TAB <type> TAB <hex bytes>. Key paths start with the
// root name (HKEY_CURRENT_USER\Software\...); a line with an empty value name and type 0 records a key without
// values. Names compare case-insensitively, as on Windows.
#include "rt32.h"
#include <ctype.h>
#include <errno.h>
#include <strings.h>
#include <sys/stat.h>

#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_INVALID_HANDLE 6
#define ERROR_OUTOFMEMORY 14
#define ERROR_INVALID_PARAMETER 87
#define ERROR_MORE_DATA 234
#define MAXKEY 512
#define MAXDATA 4096

typedef struct { char key[MAXKEY]; char name[256]; uint32_t type, size; uint8_t *data; } RegVal;
static RegVal *vals; static int nvals, capvals;
static pthread_mutex_t reg_mu = PTHREAD_MUTEX_INITIALIZER;
static int loaded;

static const char *root_name(uint32_t h) {
    switch (h) {
    case 0x80000000u: return "HKEY_CLASSES_ROOT";
    case 0x80000001u: return "HKEY_CURRENT_USER";
    case 0x80000002u: return "HKEY_LOCAL_MACHINE";
    case 0x80000003u: return "HKEY_USERS";
    case 0x80000005u: return "HKEY_CURRENT_CONFIG";
    default: return NULL;
    }
}

// opened keys: handle -> full path
#define NKEYS 256
#define KEY_BASE 0x00700000u
static char *open_keys[NKEYS];
static uint32_t key_new(const char *path) {
    for (int i = 0; i < NKEYS; i++) if (!open_keys[i]) { open_keys[i] = strdup(path); return KEY_BASE + 4u * (uint32_t)i; }
    return 0;
}
static const char *key_path(uint32_t h) {
    const char *r = root_name(h);
    if (r) return r;
    uint32_t i = (h - KEY_BASE) / 4;
    return (h >= KEY_BASE && !(h & 3) && i < NKEYS) ? open_keys[i] : NULL;
}

static void reg_file(char *out, size_t n) { snprintf(out, n, "%s/registry.txt", rt_data_dir); }

static RegVal *add_val(void) {
    if (nvals == capvals) {
        int nc = capvals ? capvals * 2 : 64;
        RegVal *nv = realloc(vals, (size_t)nc * sizeof *nv);
        if (!nv) return NULL;
        vals = nv; capvals = nc;
    }
    RegVal *v = &vals[nvals++];
    memset(v, 0, sizeof *v);
    return v;
}

static void load(void) {
    if (loaded) return;
    loaded = 1;
    char p[1100]; reg_file(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[MAXKEY + 256 + 2 * MAXDATA + 32];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *k = line, *n = strchr(k, '\t'); if (!n) continue; *n++ = 0;
        char *t = strchr(n, '\t'); if (!t) continue; *t++ = 0;
        char *d = strchr(t, '\t'); if (!d) continue; *d++ = 0;
        size_t hl = strlen(d);
        if (hl % 2 || hl / 2 > MAXDATA) { rt_log("[reg] %s: skipping a malformed line for %s\\%s", p, k, n); continue; }
        RegVal *v = add_val(); if (!v) break;
        snprintf(v->key, sizeof v->key, "%s", k); snprintf(v->name, sizeof v->name, "%s", n);
        v->type = (uint32_t)strtoul(t, NULL, 10); v->size = (uint32_t)(hl / 2);
        v->data = malloc(v->size + 1);
        for (uint32_t i = 0; i < v->size; i++) { unsigned b; sscanf(d + 2 * i, "%2x", &b); v->data[i] = (uint8_t)b; }
    }
    fclose(f);
}
static void save(void) {
    char p[1100], tmp[1110]; reg_file(p, sizeof p); snprintf(tmp, sizeof tmp, "%s.new", p);
    mkdir(rt_data_dir, 0755);
    FILE *f = fopen(tmp, "w");
    if (!f) { rt_log("[reg] can't write %s: %s", tmp, strerror(errno)); return; }
    for (int i = 0; i < nvals; i++) {
        fprintf(f, "%s\t%s\t%u\t", vals[i].key, vals[i].name, vals[i].type);
        for (uint32_t k = 0; k < vals[i].size; k++) fprintf(f, "%02x", vals[i].data[k]);
        fputc('\n', f);
    }
    if (fclose(f) || rename(tmp, p)) rt_log("[reg] can't save %s: %s", p, strerror(errno));
}

// parent + "\" + sub, without doubled or trailing separators
static int join(const char *parent, const char *sub, char *out, size_t n) {
    char s[MAXKEY]; snprintf(s, sizeof s, "%s", sub ? sub : "");
    size_t l = strlen(s);
    while (l && s[l - 1] == '\\') s[--l] = 0;
    const char *q = s; while (*q == '\\') q++;
    int w = *q ? snprintf(out, n, "%s\\%s", parent, q) : snprintf(out, n, "%s", parent);
    return w > 0 && (size_t)w < n;
}
static int key_exists(const char *path) {
    size_t n = strlen(path);
    if (!strchr(path, '\\')) return 1;   // a root
    for (int i = 0; i < nvals; i++)
        if (!strncasecmp(vals[i].key, path, n) && (vals[i].key[n] == 0 || vals[i].key[n] == '\\')) return 1;
    return 0;
}
static RegVal *find(const char *key, const char *name, uint32_t type_min) {
    for (int i = 0; i < nvals; i++)
        if (!strcasecmp(vals[i].key, key) && !strcasecmp(vals[i].name, name) && vals[i].type >= type_min) return &vals[i];
    return NULL;
}
static void ensure_key(const char *path) {
    if (key_exists(path)) return;
    RegVal *v = add_val(); if (!v) return;
    snprintf(v->key, sizeof v->key, "%s", path);   // marker: empty name, type 0
}
static uint32_t set_value(const char *key, const char *name, uint32_t type, const void *data, uint32_t size) {
    if (size > MAXDATA) return ERROR_INVALID_PARAMETER;
    RegVal *v = find(key, name, 1);
    if (!v) { v = add_val(); if (!v) return ERROR_OUTOFMEMORY; snprintf(v->key, sizeof v->key, "%s", key); snprintf(v->name, sizeof v->name, "%s", name); }
    uint8_t *d = malloc(size + 1); if (!d) return ERROR_OUTOFMEMORY;
    if (size) memcpy(d, data, size);
    free(v->data); v->data = d; v->size = size; v->type = type;
    save();
    return ERROR_SUCCESS;
}
// copy a value out the way RegQueryValueEx does (type*, data, size* in/out; ERROR_MORE_DATA when it doesn't fit)
static uint32_t query(const RegVal *v, uint32_t typep, uint32_t datap, uint32_t sizep) {
    if (typep) wr32(typep, v->type);
    uint32_t cap = sizep ? rd32(sizep) : 0;
    if (sizep) wr32(sizep, v->size);
    if (!datap) return ERROR_SUCCESS;
    if (!sizep) return ERROR_INVALID_PARAMETER;
    if (cap < v->size) return ERROR_MORE_DATA;
    memcpy(G2H(datap), v->data, v->size);
    return ERROR_SUCCESS;
}

// game projects: a value the installer would have written (only when it isn't there yet)
void rt_registry_default(const char *key, const char *name, uint32_t type, const void *data, uint32_t size) {
    pthread_mutex_lock(&reg_mu);
    load();
    if (!find(key, name, 1)) set_value(key, name, type, data, size);
    pthread_mutex_unlock(&reg_mu);
}

// ---- advapi32 ----
SHIM(advapi32_RegOpenKeyExA, 5) {
    pthread_mutex_lock(&reg_mu); load();
    const char *base = key_path(ARG(0)); char p[MAXKEY];
    uint32_t r = ERROR_INVALID_HANDLE;
    if (base) {
        if (!join(base, STR(1), p, sizeof p)) r = ERROR_INVALID_PARAMETER;
        else if (!key_exists(p)) r = ERROR_FILE_NOT_FOUND;
        else { uint32_t h = key_new(p); r = h ? ERROR_SUCCESS : ERROR_OUTOFMEMORY; if (h) wr32(ARG(4), h); }
    }
    pthread_mutex_unlock(&reg_mu);
    return r;
}
SHIM(advapi32_RegCreateKeyExA, 9) {
    pthread_mutex_lock(&reg_mu); load();
    const char *base = key_path(ARG(0)); char p[MAXKEY];
    uint32_t r = ERROR_INVALID_HANDLE;
    if (base) {
        if (!join(base, STR(1), p, sizeof p)) r = ERROR_INVALID_PARAMETER;
        else {
            int existed = key_exists(p);
            if (!existed) { ensure_key(p); save(); }
            uint32_t h = key_new(p);
            r = h ? ERROR_SUCCESS : ERROR_OUTOFMEMORY;
            if (h) wr32(ARG(7), h);
            if (h && ARG(8)) wr32(ARG(8), existed ? 2 : 1);   // REG_OPENED_EXISTING_KEY / REG_CREATED_NEW_KEY
        }
    }
    pthread_mutex_unlock(&reg_mu);
    return r;
}
SHIM(advapi32_RegQueryValueExA, 6) {
    pthread_mutex_lock(&reg_mu); load();
    const char *k = key_path(ARG(0));
    uint32_t r = ERROR_INVALID_HANDLE;
    if (k) { RegVal *v = find(k, ARG(1) ? STR(1) : "", 1); r = v ? query(v, ARG(3), ARG(4), ARG(5)) : ERROR_FILE_NOT_FOUND; }
    pthread_mutex_unlock(&reg_mu);
    return r;
}
SHIM(advapi32_RegSetValueExA, 6) {
    pthread_mutex_lock(&reg_mu); load();
    const char *k = key_path(ARG(0));
    uint32_t r = k ? set_value(k, ARG(1) ? STR(1) : "", ARG(3), ARG(4) ? G2H(ARG(4)) : NULL, ARG(4) ? ARG(5) : 0) : ERROR_INVALID_HANDLE;
    pthread_mutex_unlock(&reg_mu);
    return r;
}
SHIM(advapi32_RegCloseKey, 1) {
    uint32_t h = ARG(0);
    if (root_name(h)) return ERROR_SUCCESS;
    uint32_t i = (h - KEY_BASE) / 4;
    pthread_mutex_lock(&reg_mu);
    uint32_t r = ERROR_INVALID_HANDLE;
    if (h >= KEY_BASE && !(h & 3) && i < NKEYS && open_keys[i]) { free(open_keys[i]); open_keys[i] = NULL; r = ERROR_SUCCESS; }
    pthread_mutex_unlock(&reg_mu);
    return r;
}

// ---- shlwapi: one-call forms ----
SHIM(shlwapi_SHGetValueA, 6) {
    pthread_mutex_lock(&reg_mu); load();
    const char *base = key_path(ARG(0)); char p[MAXKEY];
    uint32_t r = ERROR_INVALID_HANDLE;
    if (base) {
        if (!join(base, STR(1), p, sizeof p)) r = ERROR_INVALID_PARAMETER;
        else { RegVal *v = find(p, ARG(2) ? STR(2) : "", 1); r = v ? query(v, ARG(3), ARG(4), ARG(5)) : ERROR_FILE_NOT_FOUND; }
    }
    pthread_mutex_unlock(&reg_mu);
    return r;
}
SHIM(shlwapi_SHSetValueA, 6) {
    pthread_mutex_lock(&reg_mu); load();
    const char *base = key_path(ARG(0)); char p[MAXKEY];
    uint32_t r = ERROR_INVALID_HANDLE;
    if (base) {
        if (!join(base, STR(1), p, sizeof p)) r = ERROR_INVALID_PARAMETER;
        else r = set_value(p, ARG(2) ? STR(2) : "", ARG(3), ARG(4) ? G2H(ARG(4)) : NULL, ARG(4) ? ARG(5) : 0);
    }
    pthread_mutex_unlock(&reg_mu);
    return r;
}
