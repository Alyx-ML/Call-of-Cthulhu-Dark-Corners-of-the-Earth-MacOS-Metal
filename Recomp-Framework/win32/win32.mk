# Recomp-Framework win32: build rules for 32-bit Windows games (PE32 i386 -> C -> arm64).
# A game project's Makefile sets these and includes this file:
#
#   FW        = path/to/Recomp-Framework
#   GAME_EXE  = orig.exe                  # the game's executable, DRM removed (32-bit PE)
#   APP_NAME  = MyGame
#   GAME_SRC  = src/game.c                # main(): settings, rt_init, rt_run; plus game-specific natives
#   NATIVE    = native.txt                # optional: guest functions replaced by native ones (addr name)
#   PATCHES   = patches.txt               # optional: guest instructions replaced by C (addr code)
#   include $(FW)/win32/win32.mk
#
# Targets: gen (translate), boot (link), runtime (compile check without the game), clean.

CC        ?= clang
PYTHON    ?= python3
ARCH      ?= -arch arm64
MACOS_MIN ?= 14.0
GEN_DIR   ?= gen
BUILD     ?= build
APP_NAME  ?= Game
GAME_SRC  ?= game.c
NATIVE    ?=
PATCHES   ?=
W32       = $(FW)/win32

WARN       = -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-label -Wno-parentheses-equality
W32_INC    = -I$(W32)/runtime
CFLAGS    ?= -O1 -g $(ARCH) -mmacosx-version-min=$(MACOS_MIN) $(WARN) $(W32_INC)
GEN_CFLAGS ?= -O2 -g $(ARCH) -mmacosx-version-min=$(MACOS_MIN) $(WARN) -Wno-tautological-constant-out-of-range-compare $(W32_INC)

TRANSLATE = $(PYTHON) $(W32)/tools/recomp_w32.py
TRANSLATE_OPTS = $(if $(NATIVE),--native $(NATIVE)) $(if $(PATCHES),--patches $(PATCHES))

gen:
	$(TRANSLATE) $(GAME_EXE) $(GEN_DIR) $(TRANSLATE_OPTS)

GEN_SRC = $(sort $(wildcard $(GEN_DIR)/code_*.c)) $(GEN_DIR)/tables.c
GEN_OBJ = $(patsubst %.c,$(BUILD)/%.o,$(GEN_SRC))
W32_HDR = $(wildcard $(W32)/runtime/*.h)
$(BUILD)/$(GEN_DIR)/%.o: $(GEN_DIR)/%.c $(W32)/runtime/cpu32.h $(W32)/runtime/tables32.h
	@mkdir -p $(dir $@)
	$(CC) $(GEN_CFLAGS) -c $< -o $@
$(BUILD)/libgame.a: $(GEN_OBJ)
	rm -f $@; ar rcs $@ $^

# Direct3D 9 runs on Metal: d3d9_metal.m (device, resources, presentation), d3d9_shader.c (shader bytecode -> MSL),
# d3d9_ffp.c (fixed-function pipeline -> MSL), d3d9_enhance.m (ambient occlusion and contact shadows, rt_enh.h)
W32_RT_C ?= rt32.c imports.c kernel32.c crt.c user32.c misc.c registry.c com32.c dinput8.c steam32.c wmvcore.c xaudio2_9.c fmodex.c dshow.c d3dx9.c \
            d3d9_shader.c d3d9_ffp.c
W32_RT_M ?= cocoa32.m d3d9_metal.m d3d9_enhance.m
include $(W32)/ffmpeg.mk
RT_OBJ   = $(patsubst %.c,$(BUILD)/w32/%.o,$(W32_RT_C)) $(patsubst %.m,$(BUILD)/w32/%.o,$(W32_RT_M))
GAME_OBJ = $(patsubst %.c,$(BUILD)/game/%.o,$(GAME_SRC))
$(BUILD)/w32/%.o: $(W32)/runtime/%.c $(W32_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
# the WMV reader and DirectShow compile against the FFmpeg they link (ffmpeg.mk)
$(BUILD)/w32/wmvcore.o $(BUILD)/w32/dshow.o: $(BUILD)/w32/%.o: $(W32)/runtime/%.c $(W32_HDR) $(FFMPEG_OUT)/lib/libavcodec.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(FFMPEG_INC) -c $< -o $@
$(BUILD)/w32/%.o: $(W32)/runtime/%.m $(W32_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fobjc-arc -c $< -o $@
$(BUILD)/game/%.o: %.c $(W32_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

FRAMEWORKS ?= -framework CoreFoundation -framework AppKit -framework QuartzCore -framework Metal -framework MetalFX -framework MetalKit \
              -framework AudioToolbox -framework CoreAudio -framework GameController
# LDLIBS: a game project's extra libraries; POSTLINK: a command run on the linked binary ($@), e.g. install_name_tool
LDLIBS   ?=
POSTLINK ?=
$(BUILD)/$(APP_NAME): $(BUILD)/libgame.a $(RT_OBJ) $(GAME_OBJ)
	$(CC) $(ARCH) $(RT_OBJ) $(GAME_OBJ) $(BUILD)/libgame.a $(FFMPEG_LIBS) $(FRAMEWORKS) $(LDLIBS) -lc++ -o $@
	$(POSTLINK)
boot: $(BUILD)/$(APP_NAME)
runtime: $(RT_OBJ) $(GAME_OBJ)

# ---- macOS app (framework/tools/make_app.py): binary, the game image, icon ----
#   APP_FILE / APP_DISPLAY   the .app's file name / the name in the menu bar and Dock (default APP_NAME)
#   APP_IMAGE                the executable image to bundle as Resources/$(APP_IMAGE_NAME) (rt_game.image)
#   APP_ICON                 an image for the icon when the exe's own is too small (optional)
APP_FILE       ?= $(APP_NAME)
APP_DISPLAY    ?= $(APP_FILE)
BUNDLE_ID      ?= com.example.game-recomp
APP_CATEGORY   ?= public.app-category.games
APP_IMAGE      ?= $(GAME_EXE)
APP_IMAGE_NAME ?= orig.exe
APP_ICON       ?=
APP_FLAGS      ?=
app: $(BUILD)/$(APP_NAME)
	$(PYTHON) $(FW)/tools/make_app.py $(BUILD)/$(APP_NAME) $(GAME_EXE) "$(BUILD)/$(APP_FILE).app" --name "$(APP_NAME)" \
	    --display-name "$(APP_DISPLAY)" --bundle-id $(BUNDLE_ID) --category $(APP_CATEGORY) \
	    --resource "$(APP_IMAGE)=$(APP_IMAGE_NAME)" --no-vulkan $(if $(APP_ICON),--icon "$(APP_ICON)") $(APP_FLAGS)

clean:
	rm -rf $(BUILD)

.PHONY: gen boot runtime app clean
