// Gamepad input through SDL3 (Options > Enhancements > Gamepad).
//
// The PC version of the game reads only the keyboard and the mouse (DirectInput). This file reads the first
// connected SDL3 gamepad and gives the game the keys and mouse buttons that the player's own key configuration
// (Options/Configuration.Data, written by the game's Key Configuration menu) binds to each action. The right stick
// moves the mouse (looking, and the cursor in menus). Menus also get the arrow keys, Return and Escape.
//
//   Left stick      Forwards, Backwards, Strafe Left, Strafe Right (and the arrow keys)
//   Right stick     mouse movement
//   A (south)       Jump (and Return)          B (east)        Crouch
//   X (west)        Action                     Y (north)       Reload
//   Left shoulder   Lean Left                  Right shoulder  Lean Right
//   Left trigger    Aim                        Right trigger   Fire
//   Left stick      Sneak                      Right stick     Look Strafe
//   D-pad up        Quick Heal (and Up)        D-pad down      Hide/Show Weapon (and Down)
//   D-pad left      previous weapon slot (and Left)            D-pad right     next weapon slot (and Right)
//   Back            Inventory                  Start           Escape (pause menu, back in menus)
//   Misc (share)    Skip Scene
//
// Configuration.Data: a 0x16a-byte header (its first 32-bit value is the file size), then 17-byte records
// {u32 action, u8 keyboard, u32 code, u32 flags, u32 name}: code is a DirectInput key (DIK) for keyboard records,
// and 6, 7, 8 for the left, right and middle mouse buttons otherwise. The action numbers and their names come from
// the Key Configuration menu's table in CoCMainWin32.exe (0x62ce48).
#include "rt32.h"
#include "rt_enh.h"
#include <SDL3/SDL.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

void rt_dinput_pad_key(uint8_t dik, int down);              // dinput8.c
void rt_dinput_pad_mouse(int dx, int dy, int buttons);

enum { A_FORWARD = 0, A_BACK = 1, A_LEFT = 2, A_RIGHT = 3, A_ACTION = 4, A_HIDEWEAPON = 7, A_CROUCH = 9, A_JUMP = 10,
       A_AIM = 12, A_FIRE = 14, A_RELOAD = 16, A_INVENTORY = 17, A_HEAL = 18, A_SKIP = 19, A_SNEAK = 20,
       A_LEANLEFT = 21, A_LEANRIGHT = 22, A_LOOKSTRAFE = 23, A_SLOT1 = 25, NSLOTS = 8, NACTIONS = 64 };
#define DIK_ESCAPE 0x01
#define DIK_RETURN 0x1C
#define DIK_UP     0xC8
#define DIK_LEFT   0xCB
#define DIK_RIGHT  0xCD
#define DIK_DOWN   0xD0

#define STICK_DEAD   0.20f    // radial dead zone of the right stick (fraction of the full range)
#define MOVE_ON      0.40f    // left stick: a direction key is held beyond this deflection
#define TRIGGER_ON   0.30f
#define LOOK_SPEED   1400.0f  // mouse counts per second at full deflection (the response is quadratic)
#define POLL_NS      4000000  // at most one gamepad update per 4 ms

static struct { int bound; int keyboard; uint32_t code; } bind[NACTIONS];
static int binds_loaded;
static struct timespec binds_mtime;
static int sdl_state;            // 0 not started, 1 running, -1 SDL_Init failed
static SDL_Gamepad *pad;
static uint8_t held[256];        // keys this file holds down
static int held_buttons;
static int slot;                 // the weapon slot of the D-pad (0 .. NSLOTS - 1)
static int dpad_prev[2];
static float frac_x, frac_y;
static uint64_t last_ns, last_detect_ns, last_binds_ns;

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }

static void binds_path(char *p, size_t n) { snprintf(p, n, "%s/Bethesda/Call of Cthulhu/Options/Configuration.Data", rt_data_dir); }

// the key bindings of the game's Configuration.Data (again when the file changed)
static void binds_load(void) {
    char p[1200];
    binds_path(p, sizeof p);
    struct stat st;
    if (stat(p, &st) != 0) {
        if (binds_loaded != -1) rt_log("[pad] %s: %s; the gamepad gives no game actions until the game saves its key configuration", p, strerror(errno));
        binds_loaded = -1;
        return;
    }
    if (binds_loaded == 1 && st.st_mtimespec.tv_sec == binds_mtime.tv_sec && st.st_mtimespec.tv_nsec == binds_mtime.tv_nsec) return;
    uint8_t b[4096];
    FILE *f = fopen(p, "rb");
    size_t n = f ? fread(b, 1, sizeof b, f) : 0;
    if (f) fclose(f);
    uint32_t size = 0;
    if (n >= 4) memcpy(&size, b, 4);
    if (!f || n < 0x16a || n == sizeof b || size != n || (n - 0x16a) % 17) {
        rt_log("[pad] %s is not a key configuration of this game (%zu bytes); the gamepad gives no game actions", p, n);
        memset(bind, 0, sizeof bind);
        binds_loaded = -1;
        return;
    }
    memset(bind, 0, sizeof bind);
    for (size_t o = 0x16a; o + 17 <= n; o += 17) {
        uint32_t a, code; memcpy(&a, b + o, 4); memcpy(&code, b + o + 5, 4);
        int kb = b[o + 4] != 0;
        if (a >= NACTIONS || bind[a].bound) continue;   // the first record of an action is its primary binding
        if ((kb && (code == 0 || code > 0xFF)) || (!kb && (code < 6 || code > 8))) {
            rt_log("[pad] action %u: binding (keyboard %d, code 0x%x) is not a key or mouse button the gamepad can give", a, kb, code);
            continue;
        }
        bind[a].bound = 1; bind[a].keyboard = kb; bind[a].code = code;
    }
    binds_mtime = st.st_mtimespec;
    binds_loaded = 1;
    if (rt_verbose) {
        static const struct { int a; const char *name; } names[] = {
            {A_FORWARD, "Forwards"}, {A_BACK, "Backwards"}, {A_LEFT, "Strafe Left"}, {A_RIGHT, "Strafe Right"},
            {A_CROUCH, "Crouch"}, {A_JUMP, "Jump"}, {A_SNEAK, "Sneak"}, {A_LEANLEFT, "Lean Left"}, {A_LEANRIGHT, "Lean Right"},
            {A_LOOKSTRAFE, "Look Strafe"}, {A_ACTION, "Action"}, {A_INVENTORY, "Inventory"}, {A_HEAL, "Quick Heal"},
            {A_SKIP, "Skip Scene"}, {A_AIM, "Aim"}, {A_FIRE, "Fire"}, {A_RELOAD, "Reload"}, {A_HIDEWEAPON, "Hide/Show Weapon"}};
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
            int a = names[i].a;
            if (bind[a].bound) rt_log("[pad] %s: %s 0x%x", names[i].name, bind[a].keyboard ? "key" : "mouse", bind[a].code);
            else rt_log("[pad] %s: not bound", names[i].name);
        }
    }
}

static void want_action(uint8_t *keys, int *buttons, int a) {
    if (a < 0 || a >= NACTIONS || !bind[a].bound) return;
    if (bind[a].keyboard) keys[bind[a].code] = 1;
    else *buttons |= 1 << (bind[a].code - 6);
}
// makes the held keys and buttons those of `keys` / `buttons`, and adds mouse movement
static void apply(const uint8_t *keys, int buttons, int dx, int dy) {
    for (int k = 1; k < 256; k++)
        if (keys[k] != held[k]) { held[k] = keys[k]; rt_dinput_pad_key((uint8_t)k, keys[k]); }
    if (dx || dy || buttons != held_buttons) rt_dinput_pad_mouse(dx, dy, buttons);
    held_buttons = buttons;
}
static void release_all(void) {
    static const uint8_t none[256];
    apply(none, 0, 0, 0);
    frac_x = frac_y = 0;
}

static void close_pad(void) {
    if (pad) { SDL_CloseGamepad(pad); pad = NULL; }
    release_all();
}
static void open_first_pad(void) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (!ids) { rt_log("[pad] SDL_GetGamepads failed: %s", SDL_GetError()); return; }
    for (int i = 0; i < n && !pad; i++) {
        pad = SDL_OpenGamepad(ids[i]);
        if (!pad) rt_log("[pad] SDL_OpenGamepad failed: %s", SDL_GetError());
        else rt_log("[pad] gamepad connected: %s", SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "(no name)");
    }
    SDL_free(ids);
}

static float axis(SDL_GamepadAxis a) { return (float)SDL_GetGamepadAxis(pad, a) / (float)SDL_JOYSTICK_AXIS_MAX; }

// enhance.c's input hook: once per keyboard read of the game
void coc_pad_poll(void) {
    if (!rt_enh || rt_enh->pad != 1) {
        if (pad || held_buttons) close_pad();
        else { for (int k = 1; k < 256; k++) if (held[k]) { release_all(); break; } }
        return;
    }
    uint64_t t = now_ns();
    if (t - last_ns < POLL_NS) return;
    float dt = last_ns ? (float)(t - last_ns) * 1e-9f : 0.0f;
    if (dt > 0.05f) dt = 0.05f;
    last_ns = t;
    if (sdl_state == 0) {
        if (!SDL_Init(SDL_INIT_GAMEPAD)) {
            rt_log("[pad] SDL_Init(SDL_INIT_GAMEPAD) failed: %s; the gamepad is not available", SDL_GetError());
            sdl_state = -1;
            return;
        }
        SDL_SetGamepadEventsEnabled(false);   // the state is read directly (SDL_UpdateGamepads); no event queue
        int v = SDL_GetVersion();
        rt_log("[pad] SDL %d.%d.%d gamepad support started", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
        sdl_state = 1;
    }
    if (sdl_state != 1) return;
    SDL_UpdateGamepads();
    if (pad && !SDL_GamepadConnected(pad)) { rt_log("[pad] gamepad disconnected"); close_pad(); }
    if (!pad && t - last_detect_ns >= 1000000000u) { last_detect_ns = t; open_first_pad(); }
    if (!pad) return;
    if (t - last_binds_ns >= 2000000000u || !binds_loaded) { last_binds_ns = t; binds_load(); }

    uint8_t keys[256]; memset(keys, 0, sizeof keys);
    int buttons = 0;
    // movement
    float lx = axis(SDL_GAMEPAD_AXIS_LEFTX), ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
    if (ly < -MOVE_ON) { want_action(keys, &buttons, A_FORWARD); keys[DIK_UP] = 1; }
    if (ly > MOVE_ON) { want_action(keys, &buttons, A_BACK); keys[DIK_DOWN] = 1; }
    if (lx < -MOVE_ON) { want_action(keys, &buttons, A_LEFT); keys[DIK_LEFT] = 1; }
    if (lx > MOVE_ON) { want_action(keys, &buttons, A_RIGHT); keys[DIK_RIGHT] = 1; }
    // buttons
    static const struct { SDL_GamepadButton b; int a; uint8_t extra; } map[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, A_JUMP, DIK_RETURN}, {SDL_GAMEPAD_BUTTON_EAST, A_CROUCH, 0},
        {SDL_GAMEPAD_BUTTON_WEST, A_ACTION, 0}, {SDL_GAMEPAD_BUTTON_NORTH, A_RELOAD, 0},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, A_LEANLEFT, 0}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, A_LEANRIGHT, 0},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, A_SNEAK, 0}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, A_LOOKSTRAFE, 0},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, A_HEAL, DIK_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, A_HIDEWEAPON, DIK_DOWN},
        {SDL_GAMEPAD_BUTTON_BACK, A_INVENTORY, 0}, {SDL_GAMEPAD_BUTTON_START, -1, DIK_ESCAPE},
        {SDL_GAMEPAD_BUTTON_MISC1, A_SKIP, 0},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (SDL_GetGamepadButton(pad, map[i].b)) { want_action(keys, &buttons, map[i].a); if (map[i].extra) keys[map[i].extra] = 1; }
    if (axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER_ON) want_action(keys, &buttons, A_AIM);
    if (axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_ON) want_action(keys, &buttons, A_FIRE);
    // weapon slots: a D-pad press selects the previous or next slot and holds that slot's key while it is held
    int dl = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT), dr = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    if (dl && !dpad_prev[0]) slot = (slot + NSLOTS - 1) % NSLOTS;
    if (dr && !dpad_prev[1]) slot = (slot + 1) % NSLOTS;
    dpad_prev[0] = dl; dpad_prev[1] = dr;
    if (dl) keys[DIK_LEFT] = 1;
    if (dr) keys[DIK_RIGHT] = 1;
    if (dl || dr) want_action(keys, &buttons, A_SLOT1 + slot);
    // looking: the right stick moves the mouse
    float rx = axis(SDL_GAMEPAD_AXIS_RIGHTX), ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
    float m = sqrtf(rx * rx + ry * ry);
    int dx = 0, dy = 0;
    if (m > STICK_DEAD) {
        float s = ((m > 1.0f ? 1.0f : m) - STICK_DEAD) / (1.0f - STICK_DEAD);
        float k = LOOK_SPEED * s * s * dt / m;
        frac_x += rx * k; frac_y += ry * k;
        dx = (int)frac_x; dy = (int)frac_y;
        frac_x -= (float)dx; frac_y -= (float)dy;
    } else frac_x = frac_y = 0;
    apply(keys, buttons, dx, dy);
}
