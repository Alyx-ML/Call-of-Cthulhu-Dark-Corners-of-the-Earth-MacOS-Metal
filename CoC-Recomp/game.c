// Call of Cthulhu: Dark Corners of the Earth (GOG release, Engine/CoCMainWin32.exe) on Recomp-Framework win32.
//
// The game runs from its Engine folder (GOG's play task: workingDir "Engine") and opens its data relative to it
// (..\Development, ..\Pc Sound ...). Saves go to Documents\Bethesda\Call of Cthulhu, which maps to the data folder.
//
// The game folder (the one that contains Engine/CoCMainWin32.exe) comes from, in order: the second argument
// (`app <exe> <game folder>`), COCDCOTE_GAME_DIR, or the folder the project was built from (COC_BUILD_GAME_DIR).
#include "rt32.h"
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <libgen.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXE_SHA256 "f562e71e050565775f3bcf0ddbcec07fdac0deda4491ef98c956437ab61b13fe"   // GOG 1189711155

extern char rt_cmdline[1024];
void rt_init(const char *image_path);
void rt_run(void);
void rt_registry_default(const char *key, const char *name, uint32_t type, const void *data, uint32_t size);
void rt_cocoa_screen_size(int *w, int *h);   // cocoa32.m: the main screen in points
void coc_enh_init(void);                     // enhance.c: the enhancement settings (rt_enh.h)

static int is_game_dir(const char *dir) {
    char p[2048];
    snprintf(p, sizeof p, "%s/Engine/CoCMainWin32.exe", dir);
    return access(p, R_OK) == 0;
}

// SHA-256 of a file as lowercase hex; 0 on a read error
static int sha256_file(const char *path, char hex[65]) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    uint8_t buf[1 << 16];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; close(fd); return 0; }
        if (n == 0) break;
        CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
    }
    close(fd);
    uint8_t md[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(md, &ctx);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(hex + 2 * i, 3, "%02x", md[i]);
    return 1;
}

// the executable image the runtime loads (rt_game.image): the app's Resources, next to the binary, one folder up
static int find_image(char *out, size_t n) {
    char exe[1024]; uint32_t len = sizeof exe;
    if (_NSGetExecutablePath(exe, &len) != 0) return 0;
    char dir[1024]; snprintf(dir, sizeof dir, "%s", dirname(exe));
    const char *rel[] = {"/../Resources/", "/", "/../"};
    for (int i = 0; i < 3; i++) {
        snprintf(out, n, "%s%s%s", dir, rel[i], rt_game.image);
        if (access(out, R_OK) == 0) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    rt_verbose = getenv("RT_VERBOSE") != NULL;
    rt_headless = getenv("RT_HEADLESS") != NULL;
    rt_mute = getenv("RT_MUTE") != NULL;
    rt_game.name = "Call of Cthulhu: Dark Corners of the Earth";
    rt_game.exe = "CoCMainWin32.exe";
    rt_game.win_dir = "C:\\Games\\CoCDCoTE\\Engine";
    rt_game.docs_dir = "C:\\Users\\Player\\Documents";
    rt_game.image = "orig.exe";
    rt_game.steam_app_id = 0;

    // the game folder
    const char *game = NULL;
    if (argc >= 3 && is_game_dir(argv[2])) game = argv[2];
    else if (argc >= 3) { fprintf(stderr, "%s is not the game folder: it has no Engine/CoCMainWin32.exe\n", argv[2]); return 1; }
    else if (getenv("COCDCOTE_GAME_DIR")) {
        game = getenv("COCDCOTE_GAME_DIR");
        if (!is_game_dir(game)) { fprintf(stderr, "COCDCOTE_GAME_DIR=%s has no Engine/CoCMainWin32.exe\n", game); return 1; }
    }
#ifdef COC_BUILD_GAME_DIR
    else if (is_game_dir(COC_BUILD_GAME_DIR)) game = COC_BUILD_GAME_DIR;
#endif
    if (!game) {
        fprintf(stderr, "The game folder was not found. Start the app with the folder that contains Engine/CoCMainWin32.exe:\n"
                        "  \"Call of Cthulhu DCoTE.app/Contents/MacOS/CoCDCoTE\" CoCMainWin32.exe <game folder>\n"
                        "or set COCDCOTE_GAME_DIR to that folder.\n");
        return 1;
    }
    char root[1024];
    if (!realpath(game, root)) { fprintf(stderr, "%s: %s\n", game, strerror(errno)); return 1; }
    snprintf(rt_game_dir, sizeof rt_game_dir, "%s/Engine", root);
    if (chdir(rt_game_dir) != 0) { fprintf(stderr, "can't enter %s: %s\n", rt_game_dir, strerror(errno)); return 1; }

    // the supported executable only
    char image[2048], hex[65];
    if (!find_image(image, sizeof image)) { fprintf(stderr, "the game image %s is missing from the app (rebuild it with make app)\n", rt_game.image); return 1; }
    if (!sha256_file(image, hex)) { fprintf(stderr, "can't read %s: %s\n", image, strerror(errno)); return 1; }
    if (strcmp(hex, EXE_SHA256) != 0) {
        fprintf(stderr, "Unsupported CoCMainWin32.exe (SHA-256 %s). This build supports only the GOG release "
                        "(SHA-256 " EXE_SHA256 ").\n", hex);
        return 1;
    }

    // saves, settings and the registry: ~/Library/Application Support/CoCDCoTE-recomp
    const char *home = getenv("HOME");
    if (!home || !*home) { fprintf(stderr, "HOME is not set\n"); return 1; }
    snprintf(rt_data_dir, sizeof rt_data_dir, "%s/Library/Application Support/CoCDCoTE-recomp", home);
    if (mkdir(rt_data_dir, 0755) != 0 && errno != EEXIST) { fprintf(stderr, "can't create %s: %s\n", rt_data_dir, strerror(errno)); return 1; }

    snprintf(rt_cmdline, sizeof rt_cmdline, "\"%s\\%s\"", rt_game.win_dir, rt_game.exe);

    rt_init(image);
    coc_enh_init();
    static const char key[] = "HKEY_CURRENT_USER\\Software\\Bethesda Softworks\\Call Of Cthulhu DCoTE\\Settings";
    // the value GOG's installer writes (goggame-1189711155.script, registryKey2)
    static const char exe_name[] = "CoCMainWin32.exe";
    rt_registry_default(key, "Executable", 1 /* REG_SZ */, exe_name, sizeof exe_name);
    // The video settings CoCDCoTELauncher.exe writes. The game exits ("Please start the launcher app manually")
    // until VideoModeSet is nonzero, and creates its device from ScreenWidth/ScreenHeight/RefreshRate. The mode is
    // the main display's: the Metal Direct3D 9 adapter lists that size (in points, from the same function) at 60 Hz.
    // Values already in the registry are kept. AdapterName and Language stay unset: the game reads "" and "English".
    int sw = 0, sh = 0;
    rt_cocoa_screen_size(&sw, &sh);
    if (sw <= 0 || sh <= 0) { fprintf(stderr, "can't read the main display's size\n"); return 1; }
    const struct { const char *name; uint32_t v; } dw[] = {
        {"VideoModeSet", 1}, {"ScreenWidth", (uint32_t)sw}, {"ScreenHeight", (uint32_t)sh}, {"RefreshRate", 60},
        {"Adapter", 0}, {"MultisampleMode", 0 /* D3DMULTISAMPLE_NONE */}, {"ActivateSubtitles", 1},
    };
    for (size_t i = 0; i < sizeof dw / sizeof dw[0]; i++)
        rt_registry_default(key, dw[i].name, 4 /* REG_DWORD */, &dw[i].v, 4);
    rt_run();
    return 0;
}
