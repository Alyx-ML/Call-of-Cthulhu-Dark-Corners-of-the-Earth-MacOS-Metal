// DirectInput 8 (ANSI interfaces) for 32-bit guests: the system keyboard and mouse, fed by the Cocoa host
// (cocoa32.m: DIK scan codes, mouse deltas and buttons). Joysticks are not enumerated here: games read
// controllers through XInput, or a game project maps a controller to keys and mouse input (rt_dinput_pad_key,
// rt_dinput_pad_mouse, rt_dinput_poll_hook).
#include "com32.h"
#include <strings.h>

extern uint8_t rt_keys[256];
extern int rt_mouse_dx, rt_mouse_dy, rt_mouse_dz;

#define DI_OK 0
#define DI_NOEFFECT 1
#define DIERR_UNSUPPORTED 0x80004001u
#define DIERR_DEVICENOTREG 0x80040154u
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_NOTACQUIRED 0x8007000Cu
#define DIERR_INPUTLOST 0x8007001Eu
#define DIERR_OTHERAPPHASPRIO 0x80070005u
#define DI_BUFFEROVERFLOW 1
#define DISCL_EXCLUSIVE 1u
#define DISCL_FOREGROUND 4u

int rt_cocoa_foreground(void);         // cocoa32.m: the guest's windows have the foreground
void rt_cocoa_mouse_capture(int on);   // cocoa32.m: hide the pointer and keep it still (exclusive mouse)

typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } Guid;
static const Guid GUID_SysMouse    = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const Guid GUID_SysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

// ---- keyboard state and buffered events ----
// A key is down while the keyboard or a game project's input source (rt_dinput_pad_key, e.g. a gamepad) holds it.
static uint8_t dik_state[256], kb_down[256], pad_down[256];
typedef struct { uint8_t dik, down; uint32_t time, seq; } KeyEv;
static KeyEv kq[256]; static int kq_head, kq_tail; static uint32_t kseq;
static pthread_mutex_t kmu = PTHREAD_MUTEX_INITIALIZER;
static void key_queue(uint8_t dik) {   // kmu held
    int nt = (kq_tail + 1) & 255;
    if (nt != kq_head) { kq[kq_tail] = (KeyEv){dik, dik_state[dik], 0, kseq++}; kq_tail = nt; }
}
void rt_dinput_key(uint8_t dik, int down) {
    pthread_mutex_lock(&kmu);
    kb_down[dik] = down != 0;
    dik_state[dik] = (kb_down[dik] || pad_down[dik]) ? 0x80 : 0;
    key_queue(dik);
    pthread_mutex_unlock(&kmu);
}
// A game project's input source: key `dik` held (down) or released. A buffered event is queued when the combined
// state of the key changes.
void rt_dinput_pad_key(uint8_t dik, int down) {
    pthread_mutex_lock(&kmu);
    pad_down[dik] = down != 0;
    uint8_t v = (kb_down[dik] || pad_down[dik]) ? 0x80 : 0;
    if (v != dik_state[dik]) { dik_state[dik] = v; key_queue(dik); }
    pthread_mutex_unlock(&kmu);
}
// A game project's input source for the mouse: relative movement (added to the mouse's own) and held buttons
// (bit 0 left, bit 1 right, bit 2 middle), combined with the mouse buttons in the reads.
static int pad_mouse_buttons;
void rt_dinput_pad_mouse(int dx, int dy, int buttons) {
    if (dx) __atomic_fetch_add(&rt_mouse_dx, dx, __ATOMIC_RELAXED);
    if (dy) __atomic_fetch_add(&rt_mouse_dy, dy, __ATOMIC_RELAXED);
    __atomic_store_n(&pad_mouse_buttons, buttons, __ATOMIC_RELAXED);
}
// Called on the game thread before each keyboard read (GetDeviceState, GetDeviceData), so that a game project's
// input source can update its state for the read. NULL: no call.
void (*rt_dinput_poll_hook)(void);

typedef struct {
    ComObj base;
    int kind;            // 1 keyboard, 2 mouse
    int acquired;
    uint32_t data_size;  // from SetDataFormat
    uint32_t buffer;     // DIPROP_BUFFERSIZE
    uint32_t coop;       // SetCooperativeLevel flags (DISCL_*)
    int lost;            // unacquired by a loss of the foreground: the next read reports DIERR_INPUTLOST
} DiDev;

// Acquisition, as on Windows: reads of an unacquired device fail; a DISCL_FOREGROUND device can be acquired only while
// the game has the foreground and is unacquired when it loses it; while an exclusive mouse is acquired, the pointer is
// hidden and kept still. The game thread (Acquire, Unacquire, reads) and the main thread (foreground loss) change it.
static pthread_mutex_t amu = PTHREAD_MUTEX_INITIALIZER;
#define MAX_DEVS 16
static DiDev *devs[MAX_DEVS];
static int pointer_captured;
static void capture_update(void) {   // amu held
    int want = 0;
    for (int i = 0; i < MAX_DEVS; i++)
        if (devs[i] && devs[i]->kind == 2 && devs[i]->acquired && (devs[i]->coop & DISCL_EXCLUSIVE)) want = 1;
    if (want != pointer_captured) { pointer_captured = want; rt_cocoa_mouse_capture(want); }
}
// cocoa32.m: the foreground changed
void rt_dinput_foreground(int on) {
    if (on) return;   // the game acquires its devices again when a read fails
    pthread_mutex_lock(&amu);
    for (int i = 0; i < MAX_DEVS; i++)
        if (devs[i] && devs[i]->acquired && (devs[i]->coop & DISCL_FOREGROUND)) { devs[i]->acquired = 0; devs[i]->lost = 1; }
    capture_update();
    pthread_mutex_unlock(&amu);
}
// GetDeviceState / GetDeviceData: DI_OK when the device is acquired, else the error of the read
static uint32_t read_status(DiDev *d) {
    pthread_mutex_lock(&amu);
    uint32_t r = d->acquired ? DI_OK : d->lost ? DIERR_INPUTLOST : DIERR_NOTACQUIRED;
    d->lost = 0;
    pthread_mutex_unlock(&amu);
    return r;
}

// ---- IDirectInputDevice8A ----
static uint32_t dev_GetCapabilities(CPU *c, ComObj *s, const ComMethod *m) {
    DiDev *d = (DiDev *)s; uint32_t p = M(1);
    if (!p) return DIERR_INVALIDPARAM;
    uint32_t size = rd32(p);
    memset(G2H(p + 4), 0, size > 4 ? size - 4 : 0);
    wr32(p + 4, 1 | 4);   // DIDC_ATTACHED | DIDC_EMULATED
    wr32(p + 8, d->kind == 1 ? 0x13 | (1u << 8) : 0x12 | (2u << 8));   // DI8DEVTYPE_KEYBOARD/PC101, DI8DEVTYPE_MOUSE/TRADITIONAL
    wr32(p + 12, d->kind == 2 ? 3 : 0);    // axes
    wr32(p + 16, d->kind == 2 ? 5 : 128);  // buttons / keys
    return DI_OK;
}
static uint32_t dev_EnumObjects(CPU *c, ComObj *s, const ComMethod *m) { return DI_OK; }
static uint32_t dev_GetProperty(CPU *c, ComObj *s, const ComMethod *m) {
    DiDev *d = (DiDev *)s;
    if (M(1) == 1 && M(2)) { wr32(M(2) + 16, d->buffer); return DI_OK; }   // DIPROP_BUFFERSIZE: DIPROPDWORD.dwData
    return DIERR_UNSUPPORTED;
}
static uint32_t dev_SetProperty(CPU *c, ComObj *s, const ComMethod *m) {
    DiDev *d = (DiDev *)s;
    if (M(1) == 1 && M(2)) d->buffer = rd32(M(2) + 16);
    return DI_OK;
}
static uint32_t dev_Acquire(CPU *c, ComObj *s, const ComMethod *m) {
    DiDev *d = (DiDev *)s; uint32_t r;
    pthread_mutex_lock(&amu);
    if (d->acquired) r = DI_NOEFFECT;
    else if ((d->coop & DISCL_FOREGROUND) && !rt_cocoa_foreground()) r = DIERR_OTHERAPPHASPRIO;
    else {
        d->acquired = 1; d->lost = 0; r = DI_OK;
        if (d->kind == 2) {   // the mouse reports the movement after the acquisition
            __atomic_store_n(&rt_mouse_dx, 0, __ATOMIC_RELAXED); __atomic_store_n(&rt_mouse_dy, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&rt_mouse_dz, 0, __ATOMIC_RELAXED);
        }
        capture_update();
    }
    pthread_mutex_unlock(&amu);
    return r;
}
static uint32_t dev_Unacquire(CPU *c, ComObj *s, const ComMethod *m) {
    DiDev *d = (DiDev *)s;
    pthread_mutex_lock(&amu);
    uint32_t r = d->acquired ? DI_OK : DI_NOEFFECT;
    d->acquired = 0; d->lost = 0;
    capture_update();
    pthread_mutex_unlock(&amu);
    return r;
}
static uint32_t dev_SetCooperativeLevel(CPU *c, ComObj *s, const ComMethod *m) {
    DiDev *d = (DiDev *)s; uint32_t f = M(2);
    // exactly one of EXCLUSIVE / NONEXCLUSIVE (2) and one of FOREGROUND / BACKGROUND (8)
    if (!(f & DISCL_EXCLUSIVE) == !(f & 2u) || !(f & DISCL_FOREGROUND) == !(f & 8u)) return DIERR_INVALIDPARAM;
    pthread_mutex_lock(&amu);
    d->coop = f;
    capture_update();
    pthread_mutex_unlock(&amu);
    if (rt_verbose) rt_log("[dinput8] %s: SetCooperativeLevel(0x%x)", d->kind == 1 ? "keyboard" : "mouse", f);
    return DI_OK;
}
static uint32_t dev_GetDeviceState(CPU *c, ComObj *s, const ComMethod *m) {
    DiDev *d = (DiDev *)s; uint32_t n = M(1), p = M(2);
    if (!p) return DIERR_INVALIDPARAM;
    uint32_t st = read_status(d);
    if (st != DI_OK) return st;
    memset(G2H(p), 0, n);
    if (d->kind == 1) {
        if (rt_dinput_poll_hook) rt_dinput_poll_hook();
        pthread_mutex_lock(&kmu);
        memcpy(G2H(p), dik_state, n < 256 ? n : 256);
        // RT_VERBOSE: the first read of each key press
        static uint32_t traced_seq = UINT32_MAX;
        if (rt_verbose && kseq != traced_seq)
            for (int k = 0; k < 256; k++) if (dik_state[k]) { traced_seq = kseq; rt_log("[dinput8] keyboard read: DIK 0x%02x down", k); break; }
        pthread_mutex_unlock(&kmu);
    } else {
        // DIMOUSESTATE / DIMOUSESTATE2: relative axes since the last read, then buttons
        int dx = __atomic_exchange_n(&rt_mouse_dx, 0, __ATOMIC_RELAXED);
        int dy = __atomic_exchange_n(&rt_mouse_dy, 0, __ATOMIC_RELAXED);
        int dz = __atomic_exchange_n(&rt_mouse_dz, 0, __ATOMIC_RELAXED);
        if (n >= 12) { wr32(p, (uint32_t)dx); wr32(p + 4, (uint32_t)dy); wr32(p + 8, (uint32_t)dz); }
        int pb = __atomic_load_n(&pad_mouse_buttons, __ATOMIC_RELAXED);
        if (n >= 13) wr8(p + 12, rt_keys[1] || (pb & 1) ? 0x80 : 0);
        if (n >= 14) wr8(p + 13, rt_keys[2] || (pb & 2) ? 0x80 : 0);
        if (n >= 15) wr8(p + 14, rt_keys[4] || (pb & 4) ? 0x80 : 0);
    }
    return DI_OK;
}
// GetDeviceData(cbObjectData, DIDEVICEOBJECTDATA *rgdod, DWORD *pdwInOut, flags): buffered key events
static uint32_t dev_GetDeviceData(CPU *c, ComObj *s, const ComMethod *m) {
    DiDev *d = (DiDev *)s;
    uint32_t sz = M(1), out = M(2), io = M(3), flags = M(4);
    if (!io) return DIERR_INVALIDPARAM;
    uint32_t st = read_status(d);
    if (st != DI_OK) return st;
    uint32_t want = rd32(io), got = 0;
    if (d->kind != 1) { wr32(io, 0); return DI_OK; }
    if (rt_dinput_poll_hook) rt_dinput_poll_hook();
    pthread_mutex_lock(&kmu);
    int h = kq_head;
    while (h != kq_tail && got < want) {
        if (out) {
            uint32_t e = out + got * sz;
            wr32(e, kq[h].dik); wr32(e + 4, kq[h].down); wr32(e + 8, kq[h].time); wr32(e + 12, kq[h].seq);
            if (sz >= 20) wr32(e + 16, 0);
        }
        got++; h = (h + 1) & 255;
    }
    if (!(flags & 1)) kq_head = h;   // DIGDD_PEEK leaves them queued
    pthread_mutex_unlock(&kmu);
    wr32(io, got);
    return DI_OK;
}
static uint32_t dev_SetDataFormat(CPU *c, ComObj *s, const ComMethod *m) {
    if (M(1)) ((DiDev *)s)->data_size = rd32(M(1) + 12);   // DIDATAFORMAT.dwDataSize
    return DI_OK;
}
static uint32_t dev_ok(CPU *c, ComObj *s, const ComMethod *m) { return DI_OK; }
static uint32_t dev_GetDeviceInfo(CPU *c, ComObj *s, const ComMethod *m) {
    DiDev *d = (DiDev *)s; uint32_t p = M(1);
    if (!p) return DIERR_INVALIDPARAM;
    uint32_t size = rd32(p);
    memset(G2H(p + 4), 0, size > 4 ? size - 4 : 0);
    const Guid *g = d->kind == 1 ? &GUID_SysKeyboard : &GUID_SysMouse;
    memcpy(G2H(p + 4), g, 16); memcpy(G2H(p + 20), g, 16);
    wr32(p + 36, d->kind == 1 ? 0x13 | (1u << 8) : 0x12 | (2u << 8));
    snprintf(G2H(p + 40), 260, "%s", d->kind == 1 ? "Keyboard" : "Mouse");
    snprintf(G2H(p + 300), 260, "%s", d->kind == 1 ? "Keyboard" : "Mouse");
    return DI_OK;
}

#define DEV(name, n, fn) {name, n, fn, 0, 0, 0}
#define DEVX(name, n, r) {name, n, NULL, r, 0, 0}
static const ComMethod didev_m[] = {
    {"QueryInterface", 3, com_QueryInterface, 0, 0, 0}, {"AddRef", 1, com_AddRef, 0, 0, 0}, {"Release", 1, com_Release, 0, 0, 0},
    DEV("GetCapabilities", 2, dev_GetCapabilities), DEV("EnumObjects", 4, dev_EnumObjects), DEV("GetProperty", 3, dev_GetProperty),
    DEV("SetProperty", 3, dev_SetProperty), DEV("Acquire", 1, dev_Acquire), DEV("Unacquire", 1, dev_Unacquire),
    DEV("GetDeviceState", 3, dev_GetDeviceState), DEV("GetDeviceData", 5, dev_GetDeviceData), DEV("SetDataFormat", 2, dev_SetDataFormat),
    DEV("SetEventNotification", 2, dev_ok), DEV("SetCooperativeLevel", 3, dev_SetCooperativeLevel), DEVX("GetObjectInfo", 4, DIERR_UNSUPPORTED),
    DEV("GetDeviceInfo", 2, dev_GetDeviceInfo), DEV("RunControlPanel", 3, dev_ok), DEV("Initialize", 4, dev_ok),
    DEVX("CreateEffect", 5, DIERR_UNSUPPORTED), DEV("EnumEffects", 4, dev_ok), DEVX("GetEffectInfo", 3, DIERR_UNSUPPORTED),
    DEVX("GetForceFeedbackState", 2, DIERR_UNSUPPORTED), DEVX("SendForceFeedbackCommand", 2, DIERR_UNSUPPORTED),
    DEV("EnumCreatedEffectObjects", 4, dev_ok), DEVX("Escape", 2, DIERR_UNSUPPORTED), DEV("Poll", 1, dev_ok),
    DEVX("SendDeviceData", 5, DIERR_UNSUPPORTED), DEVX("EnumEffectsInFile", 5, DIERR_UNSUPPORTED),
    DEVX("WriteEffectToFile", 5, DIERR_UNSUPPORTED), DEVX("BuildActionMap", 4, DIERR_UNSUPPORTED),
    DEVX("SetActionMap", 4, DIERR_UNSUPPORTED), DEVX("GetImageInfo", 2, DIERR_UNSUPPORTED),
};
// last Release: the device leaves the acquisition list (and gives the pointer back if it held it)
static void didev_destroy(ComObj *o) {
    DiDev *d = (DiDev *)o;
    pthread_mutex_lock(&amu);
    for (int i = 0; i < MAX_DEVS; i++) if (devs[i] == d) devs[i] = NULL;
    capture_update();
    pthread_mutex_unlock(&amu);
    free(d);
}
static ComClass didev_cls = {"IDirectInputDevice8A", sizeof didev_m / sizeof didev_m[0], didev_m, 0, didev_destroy};

// ---- IDirectInput8A ----
static uint32_t di_CreateDevice(CPU *c, ComObj *s, const ComMethod *m) {
    Guid g; memcpy(&g, G2H(M(1)), 16);
    int kind = !memcmp(&g, &GUID_SysKeyboard, 16) ? 1 : !memcmp(&g, &GUID_SysMouse, 16) ? 2 : 0;
    if (!kind || !M(2)) { if (M(2)) wr32(M(2), 0); return DIERR_DEVICENOTREG; }
    DiDev *d = calloc(1, sizeof *d);
    if (!d) { wr32(M(2), 0); return 0x8007000Eu; }   // E_OUTOFMEMORY
    d->kind = kind;
    pthread_mutex_lock(&amu);
    int slot = -1;
    for (int i = 0; i < MAX_DEVS && slot < 0; i++) if (!devs[i]) slot = i;
    if (slot >= 0) devs[slot] = d;
    pthread_mutex_unlock(&amu);
    if (slot < 0) { free(d); wr32(M(2), 0); rt_log("[dinput8] CreateDevice: more than %d devices", MAX_DEVS); return 0x8007000Eu; }
    wr32(M(2), com_wrap(&d->base, &didev_cls));
    return DI_OK;
}
// DIDEVICEINSTANCEA (580 bytes): dwSize, guidInstance, guidProduct, dwDevType, tszInstanceName[260], tszProductName[260], ...
static uint32_t di_EnumDevices(CPU *c, ComObj *s, const ComMethod *m) {
    uint32_t type = M(1), cb = M(2), ref = M(3);
    for (int kind = 1; kind <= 2; kind++) {
        // DI8DEVCLASS_ALL 0, DEVICE 1, POINTER 2, KEYBOARD 3, GAMECTRL 4; or a DI8DEVTYPE_*
        int match = type == 0 || (kind == 1 && (type == 3 || (type & 0xFF) == 0x13)) || (kind == 2 && (type == 2 || (type & 0xFF) == 0x12));
        if (!match || !cb) continue;
        uint32_t inst = gcalloc(1, 580);
        wr32(inst, 580);
        const Guid *g = kind == 1 ? &GUID_SysKeyboard : &GUID_SysMouse;
        memcpy(G2H(inst + 4), g, 16); memcpy(G2H(inst + 20), g, 16);
        wr32(inst + 36, kind == 1 ? 0x13 | (1u << 8) : 0x12 | (2u << 8));
        snprintf(G2H(inst + 40), 260, "%s", kind == 1 ? "Keyboard" : "Mouse");
        snprintf(G2H(inst + 300), 260, "%s", kind == 1 ? "Keyboard" : "Mouse");
        uint32_t args[2] = {inst, ref};
        uint32_t r = guest_call(c, cb, 2, args);
        gfree(inst);
        if (r == 0) break;   // DIENUM_STOP
    }
    return DI_OK;
}
static uint32_t di_GetDeviceStatus(CPU *c, ComObj *s, const ComMethod *m) {
    Guid g; memcpy(&g, G2H(M(1)), 16);
    return (!memcmp(&g, &GUID_SysKeyboard, 16) || !memcmp(&g, &GUID_SysMouse, 16)) ? DI_OK : 0x8007048Fu;   // DIERR_NOTATTACHED
}
static const ComMethod di_m[] = {
    {"QueryInterface", 3, com_QueryInterface, 0, 0, 0}, {"AddRef", 1, com_AddRef, 0, 0, 0}, {"Release", 1, com_Release, 0, 0, 0},
    DEV("CreateDevice", 4, di_CreateDevice), DEV("EnumDevices", 5, di_EnumDevices), DEV("GetDeviceStatus", 2, di_GetDeviceStatus),
    DEV("RunControlPanel", 3, dev_ok), DEV("Initialize", 3, dev_ok), DEVX("FindDevice", 4, DIERR_DEVICENOTREG),
    DEV("EnumDevicesBySemantics", 6, dev_ok), DEVX("ConfigureDevices", 5, DIERR_UNSUPPORTED),
};
static ComClass di_cls = {"IDirectInput8A", sizeof di_m / sizeof di_m[0], di_m, 0, NULL};

// DirectInput8Create(HINSTANCE, DWORD version, REFIID, LPVOID *out, LPUNKNOWN outer)
SHIM(dinput8_DirectInput8Create, 5) {
    if (!ARG(3)) return DIERR_INVALIDPARAM;
    ComObj *o = calloc(1, sizeof *o);
    wr32(ARG(3), com_wrap(o, &di_cls));
    if (rt_verbose) rt_log("[dinput8] DirectInput8Create(version 0x%x)", ARG(1));
    return DI_OK;
}
