// The enhancement settings of rt_enh.h in the game's own options menus.
//
// The settings live in guest memory (RtEnh), so that controls of the game's menu system write them directly. The
// game's menu builder (CoCMenuBuilder.cpp) gets a menu "ENHANCEDOPTIONS", made with the same calls as its
// "SOUNDOPTIONS" menu, and the menus "OPTIONS" (main menu) and "INGAMEOPTIONS" (pause menu) get a link to it after
// their Sound link, and a link to the menu "HDRCALIBRATION" (the peak brightness and paper white of HDR output, as
// the two settings of the Capcom RE Engine, with the test image of the runtime) after it. (A link in a menu of the SOUNDOPTIONS kind does not open its menu: Return
// there goes back to OPTIONS.) The game's "VIDEOOPTIONS" menu gets the Subtitle Size setting after its own setting.
// The text
// of these controls has string IDs from TXT_BASE; the game's string lookup (0x4b3db0, CoCStringManager) gives this
// file's text for them. patches.txt has the hook points: the five menu hooks, the intro videos (0x4b235c) and the
// subtitle size (0x4c43fa, 0x4c44d4), the field of view of the player camera (0x4352c0) and the aspect ratio of the
// camera projections (0x4347e9), and the intensity of the visual sanity effects (0x55d88e, 0x55e01d, 0x55f08d, 0x55f35d,
// 0x55f58f), and the resolution of the game's settings (0x40b533).
//
// The settings are saved to Enhancements.cfg in the data folder (key=value lines) when a value changes.
// Anisotropic filtering is always 16x and is not in the menu (the file can set another value).
// Reduced banding and the gamepad are always on and are not in the menu or the file.
// Defaults: Retina resolution, headphone audio, anti-aliasing, ambient occlusion, tone mapping, HDR, contact shadows, bloom, sharp character shadows and skip intro
// videos off; subtitles at the game's size; the game's field of view and sanity effects; Max Brightness (HDR) 0 (1000
// nits) and Brightness (HDR) 0 (300 nits).
#include "rt32.h"
#include "rt_enh.h"
#include <errno.h>
#include <stddef.h>
#include <string.h>

void coc_pad_poll(void);                 // pad.c
extern void (*rt_dinput_poll_hook)(void);   // dinput8.c

// ---- guest functions of the menu system (CoCMainWin32.exe) ----
#define G_NEW            0x40c690u   // cdecl (size, block type, file, line, flags): debug operator new
#define G_MENU_CTOR      0x4177c0u   // thiscall CoCMenu::CoCMenu()
#define G_MENU_TITLE     0x416080u   // thiscall (string id)
#define G_MENU_HELP      0x4161c0u   // thiscall (string id, string id, -1): the button help line
#define G_MENU_BACK      0x4160f0u   // thiscall (button, 1, menu name): the menu a button goes back to
#define G_MENU_416600    0x416600u
#define G_MENU_COLOR     0x416650u   // thiscall (which, color)
#define G_MENU_416270    0x416270u
#define G_MENU_SOUND     0x416190u   // thiscall (which, sound id, "")
#define G_MENU_4163F0    0x4163f0u
#define G_MENU_RECT      0x416400u   // thiscall (float[4] *): the rectangle of the rows (copied)
#define G_MENU_TITLE_RECT 0x416440u  // thiscall (float[4] *): the rectangle of the title (copied)
#define G_MENU_416470    0x416470u
#define G_MENU_416220    0x416220u
#define G_MENU_ADD       0x417910u   // thiscall (control type name) -> control
#define G_MENU_FINISH    0x417a70u   // thiscall ()
#define G_COLOR          0x43c030u   // thiscall (r, g, b, a) on a 4-byte color -> its address
#define G_CTL_LABEL      0x419a30u   // thiscall (string id)
#define G_CTL_LINK       0x4196a0u   // thiscall (1, menu name, 0)
#define G_CTL_LAYOUT     0x4197f0u   // thiscall (-1, -2, -2)
#define G_ENUM_VALUE     0x41c9d0u   // thiscall (string id): adds a value
#define G_ENUM_BIND      0x41c360u   // thiscall (int *): the value index lives there
#define G_MENU_MANAGER   0x40fed0u   // cdecl () -> the menu manager
#define G_MENU_REGISTER  0x40fd70u   // thiscall (name, menu)
#define S_FILE           0x62c014u   // ".\CoCMenuBuilder.cpp"
#define S_OPTIONS        0x62beecu   // "OPTIONS"
#define S_EMPTY          0x62bf2bu   // ""
#define S_TEXTOPTION     0x62bf20u   // "TEXTOPTION"
#define S_TEXTOPTIONENUM 0x62c0b8u   // "TEXTOPTIONENUM"
#define G_SUB_LAYOUT     0x4c4590u   // thiscall () on CoCSubtitles: the layout of the subtitle panel and its text
#define G_MENU_SHOWN     0x6c8590u   // the menu manager (0x6c8560, CoCStateMenuManager) +0x30: the menu on the screen
                                     // (its update and draw calls go to this menu; 0 without a menu)

// ---- text ----
#define TXT_BASE 0xE000u
enum { T_TITLE, T_GAME, T_2X, T_AO, T_OFF, T_LOW, T_MEDIUM, T_HIGH, T_ON,
       T_TONEMAP, T_HDR, T_CONTACT, T_SKIP, T_BLOOM, T_SHADOWS, T_SUBS, T_125, T_150, T_175,
       T_CALIB, T_PEAK, T_PAPER, T_FOV, T_110, T_120, T_130, T_140, T_SANITY, T_S75, T_S50, T_S25, T_AA, T_RETINA, T_HRTF,
       T_NITS0, T_NITS_END = T_NITS0 + RT_ENH_HDR_PEAKS - 1,     // the values of RT_ENH_HDR_PEAK_NITS
       T_BRI0, T_BRI_END = T_BRI0 + RT_ENH_HDR_PAPERS - 1,       // the values of RT_ENH_HDR_PAPER_NITS
       T_COUNT };
static const char *const txt[T_COUNT] = {
    [T_TITLE] = "Enhancements", [T_GAME] = "Game", [T_2X] = "2x", [T_AO] = "Ambient Occlusion", [T_OFF] = "Off", [T_LOW] = "Low",
    [T_MEDIUM] = "Medium", [T_HIGH] = "High", [T_ON] = "On", [T_TONEMAP] = "GT7 Tone Mapping", [T_HDR] = "HDR Output",
    [T_CONTACT] = "Contact Shadows", [T_SKIP] = "Skip Intro Videos", [T_BLOOM] = "Bloom",
    [T_SHADOWS] = "Sharp Character Shadows (restart)",
    [T_SUBS] = "Subtitle Size", [T_125] = "1.25x", [T_150] = "1.5x", [T_175] = "1.75x",
    [T_CALIB] = "HDR Calibration", [T_PEAK] = "Max Brightness (HDR)", [T_PAPER] = "Brightness (HDR)",
    [T_FOV] = "Field of View", [T_110] = "110%", [T_120] = "120%", [T_130] = "130%", [T_140] = "140%",
    [T_SANITY] = "Sanity Effects", [T_S75] = "75%", [T_S50] = "50%", [T_S25] = "25%",
    [T_AA] = "Anti-Aliasing (MetalFX)", [T_RETINA] = "Retina Resolution (restart)",
    [T_HRTF] = "Headphone Audio (HRTF)",
    // the steps -10 .. +10 of the two settings of the RE Engine, with their values in nits (rt_enh.h)
    [T_NITS0] =
    "-10 (300 nits)", "-9 (400 nits)", "-8 (500 nits)", "-7 (600 nits)", "-6 (700 nits)", "-5 (750 nits)",
    "-4 (800 nits)", "-3 (850 nits)", "-2 (900 nits)", "-1 (950 nits)", "0 (1000 nits)", "+1 (1100 nits)",
    "+2 (1200 nits)", "+3 (1400 nits)", "+4 (1600 nits)", "+5 (1800 nits)", "+6 (2000 nits)", "+7 (2500 nits)",
    "+8 (3000 nits)", "+9 (3500 nits)", "+10 (4000 nits)",
    [T_BRI0] =
    "-10 (50 nits)", "-9 (80 nits)", "-8 (120 nits)", "-7 (160 nits)", "-6 (180 nits)", "-5 (200 nits)",
    "-4 (220 nits)", "-3 (240 nits)", "-2 (260 nits)", "-1 (280 nits)", "0 (300 nits)", "+1 (320 nits)",
    "+2 (340 nits)", "+3 (380 nits)", "+4 (420 nits)", "+5 (460 nits)", "+6 (500 nits)", "+7 (540 nits)",
    "+8 (580 nits)", "+9 (610 nits)", "+10 (640 nits)",
};
_Static_assert(T_NITS_END == T_NITS0 + 20 && T_BRI_END == T_BRI0 + 20, "the value texts follow rt_enh.h");
static uint32_t txt_guest[T_COUNT];

// patch at 0x4b3db0: the text of string id `id` (TXT_BASE ..), as the game's lookup returns it (a guest char *)
uint32_t coc_enh_text(uint32_t id) {
    uint32_t i = id - TXT_BASE;
    if (i >= T_COUNT) rt_fatal("[enh] string id 0x%x is not an enhancement text", id);
    if (!txt_guest[i]) {
        if (!(txt_guest[i] = gstrdup(txt[i]))) rt_fatal("[enh] out of guest memory for menu text");
        if (rt_verbose) rt_log("[enh] menu text 0x%x: %s", id, txt[i]);
    }
    return txt_guest[i];
}

// ---- the settings ----
static uint32_t enh_guest;               // guest address of the RtEnh
static RtEnh saved;                      // the values in Enhancements.cfg
static const struct { const char *key; size_t off; int32_t max; int32_t def; } keys[] = {
    {"anisotropic_filtering", offsetof(RtEnh, aniso), 4, 4},   // not in the menu: 16x unless the file sets another value
    {"ambient_occlusion", offsetof(RtEnh, ao), 3, 0},
    {"tone_mapping", offsetof(RtEnh, tonemap), 1, 0},
    {"hdr", offsetof(RtEnh, hdr), 1, 0},
    {"contact_shadows", offsetof(RtEnh, contact), 1, 0},
    {"skip_intro", offsetof(RtEnh, skip_intro), 1, 0},
    {"bloom", offsetof(RtEnh, bloom), 3, 0},
    {"sharp_character_shadows", offsetof(RtEnh, shadows), 1, 0},
    {"subtitle_size", offsetof(RtEnh, sub_scale), 4, 0},
    {"hdr_max_brightness", offsetof(RtEnh, hdr_peak), RT_ENH_HDR_PEAKS - 1, RT_ENH_HDR_DEFAULT},   // 0: 1000 nits
    {"hdr_brightness", offsetof(RtEnh, hdr_paper), RT_ENH_HDR_PAPERS - 1, RT_ENH_HDR_DEFAULT},     // 0: 300 nits
    {"field_of_view", offsetof(RtEnh, fov), 4, 0},
    {"sanity_effects", offsetof(RtEnh, sanity), 4, 0},
    {"anti_aliasing", offsetof(RtEnh, aa), 1, 0},
    {"retina_resolution", offsetof(RtEnh, retina), 1, 0},
    {"headphone_audio", offsetof(RtEnh, hrtf), 1, 0},
};
enum { K_ANISO, K_AO, K_TONEMAP, K_HDR, K_CONTACT, K_SKIP, K_BLOOM, K_SHADOWS, K_SUBS, K_PEAK, K_PAPER, K_FOV, K_SANITY, K_AA, K_RETINA, K_HRTF };
// keys of earlier versions: settings that are now always on (gamepad, reduced_banding) and the HDR calibration of
// other steps (hdr_peak: 400 .. 4000 nits; hdr_paper_white: a percentage of the SDR white of macOS). The loader skips
// them, and the next save leaves them out.
static const char *const retired_keys[] = {"gamepad", "reduced_banding", "hdr_peak", "hdr_paper_white"};
#define NKEYS (sizeof keys / sizeof keys[0])
static int32_t *field(RtEnh *e, size_t i) { return (int32_t *)((char *)e + keys[i].off); }
static void cfg_path(char *p, size_t n) { snprintf(p, n, "%s/Enhancements.cfg", rt_data_dir); }

static void cfg_save(void) {
    char p[1200], tmp[1210];
    cfg_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) { rt_log("[enh] can't write %s: %s; the settings are not saved", tmp, strerror(errno)); return; }
    int bad = fprintf(f, "# Call of Cthulhu DCoTE recompilation: enhancement settings (Options > Enhancements)\n") < 0;
    for (size_t i = 0; i < NKEYS; i++) bad |= fprintf(f, "%s=%d\n", keys[i].key, *field(rt_enh, i)) < 0;
    bad |= fclose(f) != 0;
    if (bad || rename(tmp, p) != 0) { rt_log("[enh] can't write %s: %s; the settings are not saved", p, strerror(errno)); remove(tmp); return; }
    saved = *rt_enh;
    if (rt_verbose) rt_log("[enh] settings saved: aniso %d, ao %d, tonemap %d, hdr %d, contact %d, skip_intro %d, "
                           "bloom %d, shadows %d, subtitle size %d, hdr max brightness %d, hdr brightness %d, field of view %d, sanity effects %d, anti-aliasing %d, retina %d, headphone audio %d",
                           rt_enh->aniso,
                           rt_enh->ao, rt_enh->tonemap, rt_enh->hdr, rt_enh->contact, rt_enh->skip_intro, rt_enh->bloom,
                           rt_enh->shadows, rt_enh->sub_scale, rt_enh->hdr_peak, rt_enh->hdr_paper, rt_enh->fov, rt_enh->sanity, rt_enh->aa, rt_enh->retina, rt_enh->hrtf);
}
static void cfg_load(void) {
    for (size_t i = 0; i < NKEYS; i++) *field(rt_enh, i) = keys[i].def;
    rt_enh->banding = 1;                 // reduced banding: always on
    rt_enh->pad = 1;                     // gamepad input (SDL3): always on
    char p[1200];
    cfg_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) {
        if (errno != ENOENT) rt_log("[enh] can't read %s: %s; the settings are the defaults", p, strerror(errno));
        saved = *rt_enh;
        return;
    }
    char line[256]; int ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\r' || !*s) continue;
        char *eq = strchr(s, '=');
        size_t i = NKEYS, r;
        if (eq) for (i = 0; i < NKEYS; i++) if ((size_t)(eq - s) == strlen(keys[i].key) && !strncmp(s, keys[i].key, (size_t)(eq - s))) break;
        for (r = 0; eq && i == NKEYS && r < sizeof retired_keys / sizeof retired_keys[0]; r++)
            if ((size_t)(eq - s) == strlen(retired_keys[r]) && !strncmp(s, retired_keys[r], (size_t)(eq - s))) break;
        if (eq && i == NKEYS && r < sizeof retired_keys / sizeof retired_keys[0]) continue;
        char *end = NULL;
        long v = eq ? strtol(eq + 1, &end, 10) : -1;
        if (i == NKEYS || !end || end == eq + 1 || (*end && *end != '\n' && *end != '\r') || v < 0 || v > keys[i].max) {
            rt_log("[enh] %s line %d is not a valid setting; the line is ignored", p, ln);
            continue;
        }
        *field(rt_enh, i) = (int32_t)v;
    }
    fclose(f);
    saved = *rt_enh;
}

static uint32_t calib_menu;              // the HDRCALIBRATION menu (guest address)

// the input hook (dinput8.c, once per keyboard read): gamepad input, the settings file when a menu changed them,
// and the test image of HDR calibration while its menu is on the screen
static void input_frame(void) {
    int changed = 0;
    for (size_t i = 0; i < NKEYS; i++) changed |= *field(rt_enh, i) != *field(&saved, i);
    int test = calib_menu && rd32(G_MENU_SHOWN) == calib_menu;
    if (rt_verbose && test != rt_enh->hdr_test) rt_log("[enh] the HDR Calibration menu is %s the screen", test ? "on" : "off");
    rt_enh->hdr_test = test;
    if (changed) {
        for (size_t i = 0; i < NKEYS; i++)
            if (*field(rt_enh, i) < 0 || *field(rt_enh, i) > keys[i].max)
                rt_fatal("[enh] the menu wrote %d to setting %s (range 0..%d)", *field(rt_enh, i), keys[i].key, keys[i].max);
        cfg_save();
    }
    coc_pad_poll();
}

// patch at 0x4b235c (the start of the game, after the settings): 1 when the intro videos are skipped. The game then
// takes its own SkipVideos path (CoCSettingsData +0x671) and opens the main menu directly.
int coc_enh_skip_intro(void) { return rt_enh && rt_enh->skip_intro; }

void coc_enh_init(void) {
    enh_guest = gcalloc(1, sizeof(RtEnh));
    if (!enh_guest) rt_fatal("[enh] out of guest memory for the enhancement settings");
    rt_enh = (RtEnh *)G2H(enh_guest);
    cfg_load();
    // the character shadow projector (512x512 R5G6B5 target, no depth buffer, the light's view-projection matrix in
    // c0..c3): the game draws into these targets during the scene
    d3d9_enh_light_pass(512, 512, 23 /* D3DFMT_R5G6B5 */, 0);
    // the light of the contact shadows is the game's own light of its characters: the character shaders
    // (Skinning_*bone*.vso) light the normal maps with the vector toward the light skinLightDir (c87) at the
    // character's center skinCenterPos (c86), plus skinLightGradients (c89, c91, c93: its derivatives along world
    // x, y and z; c90, c92, c94 are the derivatives of the light color) times the offset from the center. In a level
    // the derivatives were those of a point light approximately 100 units from the center (-(I - L L^T) / r).
    d3d9_enh_light_vs("skinLightDir", "skinCenterPos", "skinLightGradients", 2);
    // the camera: the game draws the world with vertex shaders and does not set the fixed-function transforms; its
    // world shaders (Static_Lightmapped.vso and others) have the world-view-projection matrix matFinal in c0..c3
    d3d9_enh_camera_vs(0);
    // the same six 512x512 R5G6B5 targets hold the character shadows: with Sharp Character Shadows they are stored at
    // 2048x2048 with 8 bits per channel
    d3d9_enh_shadow_target(512, 512, 23 /* D3DFMT_R5G6B5 */, 4);
    rt_dinput_poll_hook = input_frame;
    static const char *const lvl[] = {"off", "low", "medium", "high"}, *const sub[] = {"game", "1.25x", "1.5x", "1.75x", "2x"};
    rt_log("[enh] settings: anisotropic filtering %s, ambient occlusion %s, GT7 tone mapping %s, HDR %s (max brightness %s, "
           "brightness %s), contact shadows %s, bloom %s, sharp character shadows %s, subtitle size %s, skip intro videos %s, field of view %s, sanity effects %s, anti-aliasing %s, Retina resolution %s, headphone audio %s",
           (const char *[]){"game", "2x", "4x", "8x", "16x"}[rt_enh->aniso], lvl[rt_enh->ao], rt_enh->tonemap ? "on" : "off",
           rt_enh->hdr ? "on" : "off", txt[T_NITS0 + rt_enh->hdr_peak], txt[T_BRI0 + rt_enh->hdr_paper],
           rt_enh->contact ? "on" : "off", lvl[rt_enh->bloom], rt_enh->shadows ? "on" : "off", sub[rt_enh->sub_scale],
           rt_enh->skip_intro ? "on" : "off", (const char *[]){"game", "110%", "120%", "130%", "140%"}[rt_enh->fov],
           (const char *[]){"game", "75%", "50%", "25%", "off"}[rt_enh->sanity], rt_enh->aa ? "MetalFX" : "off", rt_enh->retina ? "on" : "off",
           rt_enh->hrtf ? "HRTF" : "off");
}

// ---- the resolution ----
// The game's settings (0x40b440 reads them from the registry into a settings object) have the resolution of its
// full-screen display mode at +8 (ScreenWidth) and +0xc (ScreenHeight). The game writes the settings object back to
// the registry (0x40b6d3), so the registry can keep the resolution of an earlier run.
void rt_cocoa_screen_size(int *w, int *h);     // cocoa32.m: the screen in points
void rt_cocoa_screen_pixels(int *w, int *h);   // cocoa32.m: the screen in pixels
// patch at 0x40b533, after the settings object (guest address s) has its resolution: with Retina Resolution, the
// resolution is the screen in pixels (a display mode of the runtime with this setting, rt_enh.h). Without it, a
// resolution larger than the screen in points (from a run with Retina Resolution) becomes the screen in points, the
// largest display mode without this setting.
void coc_enh_resolution(uint32_t s) {
    uint32_t w = rd32(s + 8u), h = rd32(s + 0xcu), nw = w, nh = h;
    int sw = 0, sh = 0;
    if (rt_enh->retina) rt_cocoa_screen_pixels(&sw, &sh);
    else rt_cocoa_screen_size(&sw, &sh);
    if (sw <= 0 || sh <= 0) rt_fatal("[enh] the screen size is %d x %d", sw, sh);
    if (rt_enh->retina || w > (uint32_t)sw || h > (uint32_t)sh) { nw = (uint32_t)sw; nh = (uint32_t)sh; }
    if (nw != w || nh != h) {
        wr32(s + 8u, nw); wr32(s + 0xcu, nh);
        static uint32_t logged_w, logged_h;
        if (nw != logged_w || nh != logged_h) {
            rt_log("[enh] resolution %ux%u (the game's settings: %ux%u; Retina Resolution %s)", nw, nh, w, h,
                   rt_enh->retina ? "on" : "off");
            logged_w = nw; logged_h = nh;
        }
    }
}

// ---- the field of view and the aspect ratio ----
// The game's camera (0x4347c0: field of view in degrees, aspect ratio) makes its projection with
// D3DXMatrixPerspectiveFovRH. All the game's cameras give it the aspect ratio 1.3256 (approximately 4:3), whatever the
// back buffer is: on a 16:10 back buffer (1800 x 1125) the 3D image was 21% wider than it should be (measured: field
// of view 55 degrees vertical, aspect 1.3256). The aspect ratio is now that of the back buffer. The vertical field of
// view does not change, so a wider screen shows more at the sides.
// The player camera (CoCPlayerFovManager, 0x549209 -> 0x4352c0) gets its field of view (the game: 55 degrees, 1 to
// 120) times the Field of View setting. Cut scenes and the game's other cameras keep their field of view.
static const float fov_scale[] = {1.0f, 1.1f, 1.2f, 1.3f, 1.4f};

// patch at 0x4352c0: the field of view (degrees, as float bits) that the player camera sets
uint32_t coc_enh_player_fov(uint32_t bits) {
    if (!rt_enh || rt_enh->fov == 0) return bits;
    float f;
    memcpy(&f, &bits, sizeof f);
    if (!(f > 0.0f && f <= 120.0f)) rt_fatal("[enh] the player camera set the field of view %g (the game: 1 to 120)", f);
    f *= fov_scale[rt_enh->fov];
    memcpy(&bits, &f, sizeof bits);
    return bits;
}

// patch at 0x4347e9: the aspect ratio (float bits) of a camera projection
uint32_t coc_enh_aspect(uint32_t bits) {
    uint32_t w, h;
    d3d9_enh_back_buffer(&w, &h);
    if (!w || !h) rt_fatal("[enh] a camera projection was made before the device had a back buffer");
    float a = (float)w / (float)h, g;
    memcpy(&g, &bits, sizeof g);
    static float logged;
    if (a != logged) { rt_log("[enh] camera aspect ratio %.4f (the back buffer %ux%u; the game gave %.4f)", a, w, h, g); logged = a; }
    memcpy(&bits, &a, sizeof bits);
    return bits;
}

// ---- the visual sanity effects ----
// The insanity effects of the game (CoCInsanityEffect classes) with a picture: Black And White (0x55d850), Blurred
// Vision (0x55e000), Vertigo Blur (0x55f070), Screen Distort (0x55f340) and Camera Motion Blur (0x55f4c0). Each Start
// (vtable slot 9) gets an intensity from 0 to 1 and gives it to the camera effect of its id (1, 9, 0xe, 0xd, 0xc) with
// that effect's set-intensity function (vtable +0x10, 0x4649c0). Stop gives 0. The patches multiply the intensity just
// before it goes to the camera effect: Sanity Effects 75%, 50% and 25% make the effects weaker, Off gives 0 (as Stop).
// The game's sounds, sanity value and other insanity effects do not change.
static const float sanity_scale[] = {1.0f, 0.75f, 0.5f, 0.25f, 0.0f};

// patches at 0x55d88e, 0x55e01d, 0x55f08d, 0x55f35d (the intensity argument of Start) and 0x55f58f (the intensity that
// Camera Motion Blur gives): the intensity (float bits) that the camera effect gets
uint32_t coc_enh_sanity(uint32_t bits) {
    if (!rt_enh || rt_enh->sanity == 0) return bits;
    float f;
    memcpy(&f, &bits, sizeof f);
    float r = f * sanity_scale[rt_enh->sanity];
    static int logged = -1;
    if (f > 0.0f && logged != rt_enh->sanity) {
        rt_log("[enh] sanity effect intensity %.3f -> %.3f (Sanity Effects %s)", f, r, txt[(int[]){T_GAME, T_S75, T_S50, T_S25, T_OFF}[rt_enh->sanity]]);
        logged = rt_enh->sanity;
    }
    memcpy(&bits, &r, sizeof bits);
    return bits;
}

// ---- the menus ----
// a guest call that leaves the caller's registers as they were (the hooks run inside translated functions)
static uint32_t gcall(CPU *c, uint32_t fn, uint32_t self, int n, const uint32_t *a) {
    CPU keep = *c;
    c->ecx = self;
    uint32_t r = guest_call(c, fn, n, a);
    *c = keep;
    return r;
}
#define CALL(fn, self, ...) gcall(c, fn, self, (int)(sizeof (uint32_t[]){__VA_ARGS__} / 4), (uint32_t[]){__VA_ARGS__})
#define CALL0(fn, self) gcall(c, fn, self, 0, NULL)

// ---- the subtitle size ----
// The game's subtitle layout (0x4c4590) copies CharacterHeight (CoCSettingsData +0x618, the text height as a part of
// the screen height) into the subtitle object; the panel position, the speaker name and the text follow from it.
static const float sub_size[] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
static int32_t sub_laid_out;             // the subtitle_size of the last layout

// patch at 0x4c43fa (mov edx, [eax+0x618]): the CharacterHeight bits that the layout uses
uint32_t coc_enh_sub_height(uint32_t bits) {
    sub_laid_out = rt_enh ? rt_enh->sub_scale : 0;
    float h;
    memcpy(&h, &bits, sizeof h);
    float g = h;
    h *= sub_size[sub_laid_out];
    if (rt_verbose) rt_log("[enh] subtitle layout: character height %.4f -> %.4f", g, h);
    memcpy(&bits, &h, sizeof bits);
    return bits;
}

// patch at 0x4c44d4 (CoCSubtitles show, after its priority test): when the size changed after the last layout, the
// game's layout runs again before the new subtitle shows. The layout resets the panel and the current priority,
// which the show then sets for the new subtitle.
void coc_enh_sub_show(CPU *c, uint32_t subtitles) {
    if (!rt_enh || rt_enh->sub_scale == sub_laid_out) return;
    CALL0(G_SUB_LAYOUT, subtitles);
    if (rt_verbose) rt_log("[enh] subtitle layout again: size %s", (const char *[]){"game", "1.25x", "1.5x", "1.75x", "2x"}[sub_laid_out]);
}

static uint32_t menu_name, calib_name;   // "ENHANCEDOPTIONS" and "HDRCALIBRATION" in guest memory
static uint32_t name_guest(void) {
    if (!menu_name && !(menu_name = gstrdup("ENHANCEDOPTIONS"))) rt_fatal("[enh] out of guest memory for the menu name");
    return menu_name;
}
static uint32_t calib_name_guest(void) {
    if (!calib_name && !(calib_name = gstrdup("HDRCALIBRATION"))) rt_fatal("[enh] out of guest memory for the menu name");
    return calib_name;
}

// patches at 0x411a1f (OPTIONS) and 0x410be6 (INGAMEOPTIONS): links to the Enhancements menu and to the HDR
// Calibration menu after the Sound link
void coc_enh_add_link(CPU *c, uint32_t menu) {
    uint32_t ctl = CALL(G_MENU_ADD, menu, S_TEXTOPTION);
    if (!ctl) rt_fatal("[enh] the menu system did not add the Enhancements link");
    CALL(G_CTL_LABEL, ctl, TXT_BASE + T_TITLE);
    CALL(G_CTL_LINK, ctl, 1, name_guest(), 0);
    ctl = CALL(G_MENU_ADD, menu, S_TEXTOPTION);
    if (!ctl) rt_fatal("[enh] the menu system did not add the HDR Calibration link");
    CALL(G_CTL_LABEL, ctl, TXT_BASE + T_CALIB);
    CALL(G_CTL_LINK, ctl, 1, calib_name_guest(), 0);
}

#define ENH_SETTINGS 12                  // the settings in the Enhancements menu (coc_enh_build_menu)
static void add_enum(CPU *c, uint32_t menu, int label, const int *values, int nvalues, size_t key) {
    uint32_t ctl = CALL(G_MENU_ADD, menu, S_TEXTOPTIONENUM);
    if (!ctl) rt_fatal("[enh] the menu system did not add an Enhancements control");
    CALL(G_CTL_LABEL, ctl, TXT_BASE + (uint32_t)label);
    for (int i = 0; i < nvalues; i++) CALL(G_ENUM_VALUE, ctl, TXT_BASE + (uint32_t)values[i]);
    if (nvalues != keys[key].max + 1) rt_fatal("[enh] control %s has %d values for the range 0..%d", keys[key].key, nvalues, keys[key].max);
    CALL(G_ENUM_BIND, ctl, enh_guest + (uint32_t)keys[key].off);
    CALL(G_CTL_LAYOUT, ctl, 0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFFFFFEu);
}


// The menu rectangles, as parts of the screen (left, top, right, bottom). The game's menus of this kind use
// (0.28, 0.33, 0.72, 0.73) for the rows and (0.28, 0.25, 0.72, 0.32) for the title; the rows have a fixed spacing
// and go below the rectangle when there are more than 7. The rectangles here are sized for the rows and centered
// on the screen. The row spacing (0.0545) and the space above the first row and below the last row (0.035) are
// measured on the game's menus; the title height (0.07) and the gap below it (0.01) are those of the game.
#define ROW_SPACING 0.0545f
#define ROW_PAD     0.035f
#define TITLE_H     0.07f
#define TITLE_GAP   0.01f
// the rectangles of `rows` rows (the last one the Back row) and of the title above them, from `top`
static void menu_rects_at(CPU *c, uint32_t menu, int rows, float top) {
    float rh = 2.0f * ROW_PAD + ROW_SPACING * (float)rows;
    const float r[8] = {0.15f, top + TITLE_H + TITLE_GAP, 0.85f, top + TITLE_H + TITLE_GAP + rh,   // the rows
                        0.15f, top, 0.85f, top + TITLE_H};                                        // the title
    uint32_t g = gcalloc(1, sizeof r);
    if (!g) rt_fatal("[enh] out of guest memory for the menu rectangles");
    memcpy(G2H(g), r, sizeof r);
    CALL(G_MENU_RECT, menu, g);
    CALL(G_MENU_TITLE_RECT, menu, g + 16);
    gfree(g);
}
// centered on the screen
static void menu_rects(CPU *c, uint32_t menu, int rows) {
    menu_rects_at(c, menu, rows, 0.5f - (TITLE_H + TITLE_GAP + 2.0f * ROW_PAD + ROW_SPACING * (float)rows) / 2.0f);
}

// patch at 0x412101 (the VIDEOOPTIONS builder, after its one setting): the Subtitle Size setting. The game draws the
// brightness test image of this menu at a fixed place, below the Brightness row and the Back row; with the new row
// the Back row was on the image. The rectangles of the menu (the game's (0.28, 0.33, 0.72, 0.73) and
// (0.28, 0.25, 0.72, 0.32)) move up by one row, so that the Back row is above the image again.
void coc_enh_add_video(CPU *c, uint32_t menu) {
    add_enum(c, menu, T_SUBS, (const int[]){T_GAME, T_125, T_150, T_175, T_2X}, 5, K_SUBS);
    const float r[8] = {0.28f, 0.33f - ROW_SPACING, 0.72f, 0.73f - ROW_SPACING,    // the rows
                        0.28f, 0.25f - ROW_SPACING, 0.72f, 0.32f - ROW_SPACING};   // the title
    uint32_t g = gcalloc(1, sizeof r);
    if (!g) rt_fatal("[enh] out of guest memory for the menu rectangles");
    memcpy(G2H(g), r, sizeof r);
    CALL(G_MENU_RECT, menu, g);
    CALL(G_MENU_TITLE_RECT, menu, g + 16);
    gfree(g);
    rt_log("[enh] Subtitle Size added to the menu VIDEOOPTIONS");
}

// a menu built like SOUNDOPTIONS (the calls of the game's builder at 0x4123f1): the title `title`, Back goes to the
// menu named `back` (a guest string), `rows` rows (the last one the Back row) from `top` (< 0: centered)
static uint32_t menu_new(CPU *c, int title, uint32_t back, int rows, float top) {
    uint32_t mem = CALL(G_NEW, 0, 0x8c0, 7, S_FILE, 0x4c, 1);
    if (!mem) rt_fatal("[enh] out of guest memory for a menu");
    uint32_t menu = CALL0(G_MENU_CTOR, mem);
    if (!menu) rt_fatal("[enh] the menu system did not create a menu");
    uint32_t col = gcalloc(1, 16);
    if (!col) rt_fatal("[enh] out of guest memory for a menu color");
    CALL(G_MENU_TITLE, menu, TXT_BASE + (uint32_t)title);
    CALL(G_MENU_HELP, menu, 0xf57, 0xf58, 0xFFFFFFFFu);
    CALL(G_MENU_BACK, menu, 0, 1, back);
    CALL(G_MENU_BACK, menu, 1, 1, back);
    CALL(G_MENU_416600, menu, 1, 0xFFFFFFFFu);
    CALL(G_MENU_COLOR, menu, 1, rd32(CALL(G_COLOR, col, 0, 0, 0, 0x60)));
    CALL(G_MENU_416270, menu, 1, 1);
    CALL(G_MENU_COLOR, menu, 0, rd32(CALL(G_COLOR, col, 0xff, 0xff, 0xff, 0)));
    CALL(G_MENU_416600, menu, 0, 0xFFFFFFFFu);
    CALL(G_MENU_SOUND, menu, 0, 0x16, S_EMPTY);
    CALL(G_MENU_SOUND, menu, 1, 0x17, S_EMPTY);
    CALL(G_MENU_4163F0, menu, 9);
    if (top < 0.0f) menu_rects(c, menu, rows); else menu_rects_at(c, menu, rows, top);
    CALL(G_MENU_416470, menu, 0);
    CALL(G_MENU_416220, menu, 0, 1);
    gfree(col);
    return menu;
}
static void menu_register(CPU *c, uint32_t name, uint32_t menu) {
    CALL0(G_MENU_FINISH, menu);
    uint32_t mgr = CALL0(G_MENU_MANAGER, 0);
    if (!mgr) rt_fatal("[enh] the menu manager is missing");
    CALL(G_MENU_REGISTER, mgr, name, menu);
}

// patch at 0x4123f1 (the end of the SOUNDOPTIONS builder): the Enhancements menu and the HDR Calibration menu
void coc_enh_build_menu(CPU *c) {
    uint32_t menu = menu_new(c, T_TITLE, S_OPTIONS, ENH_SETTINGS + 1, -1.0f);   // the settings and the Back row
    add_enum(c, menu, T_FOV, (const int[]){T_GAME, T_110, T_120, T_130, T_140}, 5, K_FOV);
    add_enum(c, menu, T_SANITY, (const int[]){T_GAME, T_S75, T_S50, T_S25, T_OFF}, 5, K_SANITY);
    add_enum(c, menu, T_AA, (const int[]){T_OFF, T_ON}, 2, K_AA);
    add_enum(c, menu, T_RETINA, (const int[]){T_OFF, T_ON}, 2, K_RETINA);
    add_enum(c, menu, T_HRTF, (const int[]){T_OFF, T_ON}, 2, K_HRTF);
    add_enum(c, menu, T_AO, (const int[]){T_OFF, T_LOW, T_MEDIUM, T_HIGH}, 4, K_AO);
    add_enum(c, menu, T_TONEMAP, (const int[]){T_OFF, T_ON}, 2, K_TONEMAP);
    add_enum(c, menu, T_HDR, (const int[]){T_OFF, T_ON}, 2, K_HDR);
    add_enum(c, menu, T_CONTACT, (const int[]){T_OFF, T_ON}, 2, K_CONTACT);
    add_enum(c, menu, T_BLOOM, (const int[]){T_OFF, T_LOW, T_MEDIUM, T_HIGH}, 4, K_BLOOM);
    add_enum(c, menu, T_SHADOWS, (const int[]){T_OFF, T_ON}, 2, K_SHADOWS);
    add_enum(c, menu, T_SKIP, (const int[]){T_OFF, T_ON}, 2, K_SKIP);
    menu_register(c, name_guest(), menu);
    rt_log("[enh] menu ENHANCEDOPTIONS registered (%d settings)", ENH_SETTINGS);

    // the HDR calibration: two settings and the Back row near the top of the screen; the runtime draws the test
    // image below them (from 0.6 to 0.84 of the screen height) while the menu is on the screen
    menu = menu_new(c, T_CALIB, S_OPTIONS, 3, 0.2f);
    int peaks[RT_ENH_HDR_PEAKS], papers[RT_ENH_HDR_PAPERS];
    for (int i = 0; i < RT_ENH_HDR_PEAKS; i++) peaks[i] = T_NITS0 + i;
    for (int i = 0; i < RT_ENH_HDR_PAPERS; i++) papers[i] = T_BRI0 + i;
    add_enum(c, menu, T_PEAK, peaks, RT_ENH_HDR_PEAKS, K_PEAK);
    add_enum(c, menu, T_PAPER, papers, RT_ENH_HDR_PAPERS, K_PAPER);
    menu_register(c, calib_name_guest(), menu);
    calib_menu = menu;
    rt_log("[enh] menu HDRCALIBRATION registered (2 settings)");
}
