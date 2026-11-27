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
# the compiler of the app (clang and lld) and the translator in C: their variables are used in the rules below
include $(W32)/llvm.mk
include $(W32)/capstone.mk

WARN       = -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-label -Wno-parentheses-equality
W32_INC    = -I$(W32)/runtime
CFLAGS    ?= -O1 -g $(ARCH) -mmacosx-version-min=$(MACOS_MIN) $(WARN) $(W32_INC)
# the runtime and the game sources read the tables of the game bundle (tables32.h, rt_load_game)
CFLAGS    += -DRT_GAME_LIBRARY
# the translated game code compiles without the macOS SDK (gen_include: the C library functions that it uses), as
# the app compiles it on the player's Mac. GEN_FLAGS are the flags without the include folders.
GEN_FLAGS  ?= -O2 -target arm64-apple-macos$(MACOS_MIN) -nostdlibinc $(WARN) -Wno-tautological-constant-out-of-range-compare
GEN_CFLAGS ?= $(GEN_FLAGS) -isystem $(W32)/gen_include $(W32_INC)
# the link of the game bundle (with -bundle_loader, the objects and gen_lib/libSystem.tbd)
GEN_LDFLAGS ?= -arch arm64 -platform_version macos $(MACOS_MIN) $(MACOS_MIN) -bundle

# provided.txt: the import shims (I_ functions) of the app binary. The game bundle links against them; the
# translator writes a stub only for the other imports.
$(BUILD)/provided.txt: $(BUILD)/$(APP_NAME)
	nm -gU $< | awk '$$3 ~ /^_I_/ { print substr($$3, 2) }' | sort > $@.tmp && test -s $@.tmp && mv $@.tmp $@

# the translator in C (convert/translate32.c, capstone.mk), the one the app runs. tools/recomp_w32.py is the same
# translator in Python: the two write the same files, byte for byte.
TRANSLATE = $(BUILD)/translate32
TRANSLATE_OPTS = --provided $(BUILD)/provided.txt $(if $(NATIVE),--native $(NATIVE)) $(if $(PATCHES),--patches $(PATCHES))

gen: $(BUILD)/provided.txt $(TRANSLATE)
	$(TRANSLATE) $(GAME_EXE) $(GEN_DIR) $(TRANSLATE_OPTS)

# conv_id: the conversion ID of the app, a hash of the inputs of the conversion (the translator, the headers of the
# translated code, the patches, the import shims of the runtime, the link library, the compile flags and the
# compiler). The app keeps the converted game in a folder with this name, so a new ID makes the app convert the game
# again.
CONV_INPUTS = $(W32)/convert/translate32.c $(W32)/convert/translate32.h $(W32)/runtime/cpu32.h $(W32)/runtime/tables32.h \
              $(wildcard $(W32)/gen_include/*.h) $(W32)/gen_lib/libSystem.tbd $(PATCHES) $(NATIVE) $(BUILD)/provided.txt
$(BUILD)/conv_id: $(CONV_INPUTS) $(W32)/win32.mk $(LLVM_BIN)
	{ cat $(CONV_INPUTS) && printf '%s\n' '$(GEN_FLAGS)' '$(GEN_LDFLAGS)' && $(LLVM_CLANG) --version | head -1; } | shasum -a 256 | cut -c1-16 > $@.tmp \
	    && mv $@.tmp $@

GEN_SRC = $(sort $(wildcard $(GEN_DIR)/code_*.c)) $(GEN_DIR)/tables.c
GEN_OBJ = $(patsubst %.c,$(BUILD)/%.o,$(GEN_SRC))
W32_HDR = $(wildcard $(W32)/runtime/*.h)
# the game code compiles and links with the compiler of the app (llvm.mk), as on the player's Mac
$(BUILD)/$(GEN_DIR)/%.o: $(GEN_DIR)/%.c $(W32)/runtime/cpu32.h $(W32)/runtime/tables32.h $(LLVM_BIN)
	@mkdir -p $(dir $@)
	$(LLVM_CLANG) $(GEN_CFLAGS) -c $< -o $@

# Direct3D 9 runs on Metal: d3d9_metal.m (device, resources, presentation), d3d9_shader.c (shader bytecode -> MSL),
# d3d9_ffp.c (fixed-function pipeline -> MSL), d3d9_enhance.m (ambient occlusion and contact shadows, rt_enh.h),
# d3d9_nrd.mm (the NVIDIA NRD denoiser of the global illumination, nrd.mk)
W32_RT_C ?= rt32.c convert32.c imports.c kernel32.c crt.c user32.c misc.c registry.c com32.c dinput8.c steam32.c wmvcore.c xaudio2_9.c fmodex.c dshow.c d3dx9.c \
            d3d9_shader.c d3d9_ffp.c
W32_RT_M ?= cocoa32.m d3d9_metal.m d3d9_enhance.m
W32_RT_MM ?= d3d9_nrd.mm
include $(W32)/ffmpeg.mk
include $(W32)/nrd.mk
RT_OBJ   = $(BUILD)/w32/translate32.o $(patsubst %.c,$(BUILD)/w32/%.o,$(W32_RT_C)) $(patsubst %.m,$(BUILD)/w32/%.o,$(W32_RT_M)) \
           $(patsubst %.mm,$(BUILD)/w32/%.o,$(W32_RT_MM))
# GAME_SRC may have Objective-C files (.m): they are compiled with ARC, as the runtime's
GAME_OBJ = $(patsubst %.c,$(BUILD)/game/%.o,$(filter %.c,$(GAME_SRC))) $(patsubst %.m,$(BUILD)/game/%.o,$(filter %.m,$(GAME_SRC)))
$(BUILD)/w32/%.o: $(W32)/runtime/%.c $(W32_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
# the translator in C (capstone.mk): the app converts the game with it (convert32.c)
$(BUILD)/w32/translate32.o: $(W32)/convert/translate32.c $(W32)/convert/translate32.h $(CAPSTONE_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CAPSTONE_INC) -c $< -o $@
# the WMV reader and DirectShow compile against the FFmpeg they link (ffmpeg.mk)
$(BUILD)/w32/wmvcore.o $(BUILD)/w32/dshow.o: $(BUILD)/w32/%.o: $(W32)/runtime/%.c $(W32_HDR) $(FFMPEG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(FFMPEG_INC) -c $< -o $@
$(BUILD)/w32/%.o: $(W32)/runtime/%.m $(W32_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fobjc-arc -c $< -o $@
# the NRD denoiser (Objective-C++) compiles against the NRD headers and the shaders of nrd.mk
$(BUILD)/w32/%.o: $(W32)/runtime/%.mm $(W32_HDR) $(NRD_MSL) $(NRD_LIBS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=c++17 -fobjc-arc $(NRD_INC) -c $< -o $@
$(BUILD)/game/%.o: %.c $(W32_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/game/%.o: %.m $(W32_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fobjc-arc -c $< -o $@

FRAMEWORKS ?= -framework CoreFoundation -framework AppKit -framework QuartzCore -framework Metal -framework MetalFX -framework MetalKit \
              -framework AudioToolbox -framework CoreAudio -framework GameController
# LDLIBS: a game project's extra libraries; POSTLINK: a command run on the linked binary ($@), e.g. install_name_tool
LDLIBS   ?=
POSTLINK ?=
# The app binary has the runtime and no game code. The game code is a bundle that links against the app binary
# (-bundle_loader) and that rt_load_game opens.
$(BUILD)/$(APP_NAME): $(RT_OBJ) $(GAME_OBJ) $(FFMPEG_STAMP)
	$(CC) $(ARCH) -mmacosx-version-min=$(MACOS_MIN) -Wl,-rpath,@executable_path/../Frameworks $(RT_OBJ) $(GAME_OBJ) $(FFMPEG_LIBS) $(NRD_LIBS) $(CAPSTONE_LIB) $(FRAMEWORKS) $(LDLIBS) -lc++ -o $@
	$(POSTLINK)
$(BUILD)/game.bundle: $(GEN_OBJ) $(BUILD)/$(APP_NAME) $(W32)/gen_lib/libSystem.tbd
	$(LLVM_LLD) $(GEN_LDFLAGS) -bundle_loader $(BUILD)/$(APP_NAME) $(GEN_OBJ) $(W32)/gen_lib/libSystem.tbd -o $@
bundle: $(BUILD)/game.bundle
boot: $(BUILD)/$(APP_NAME)
runtime: $(RT_OBJ) $(GAME_OBJ)

# ---- macOS app (framework/tools/make_app.py): the runtime, the conversion of the game, icon ----
# The app has no game code: it converts the player's executable on the first start (runtime/convert32.h) with the
# files of Contents/Resources/conversion and the compiler in Contents/Helpers.
#   APP_FILE / APP_DISPLAY   the .app's file name / the name in the Dock and the app switcher (default APP_NAME)
#   APP_SHORT                the name of the app menu in the menu bar (default APP_DISPLAY)
#   APP_ICON                 an image for the icon when the exe's own is too small (optional)
APP_FILE       ?= $(APP_NAME)
APP_DISPLAY    ?= $(APP_FILE)
APP_SHORT      ?= $(APP_DISPLAY)
BUNDLE_ID      ?= com.example.game-recomp
APP_CATEGORY   ?= public.app-category.games
APP_ICON       ?=
APP_FLAGS      ?=
# the arguments of the compile and of the link, one on each line (convert32.c reads them)
$(BUILD)/conv_cflags: $(W32)/win32.mk
	printf '%s\n' $(GEN_FLAGS) > $@
$(BUILD)/conv_ldflags: $(W32)/win32.mk
	printf '%s\n' $(GEN_LDFLAGS) > $@
# the clang headers of the translated code (stddef.h and stdint.h: cpu32.h, gen_include/string.h)
CONV_CLANG_HDR = $(addprefix $(LLVM_RESOURCE)/include/,stddef.h stdint.h) $(wildcard $(LLVM_RESOURCE)/include/__stddef_*.h)
CONV_RES = --resource "$(BUILD)/conv_id=conversion/id" --resource "$(BUILD)/provided.txt=conversion/provided.txt" \
           $(if $(PATCHES),--resource "$(PATCHES)=conversion/patches.txt") $(if $(NATIVE),--resource "$(NATIVE)=conversion/native.txt") \
           --resource "$(BUILD)/conv_cflags=conversion/cflags" --resource "$(BUILD)/conv_ldflags=conversion/ldflags" \
           --resource "$(W32)/gen_lib/libSystem.tbd=conversion/libSystem.tbd" \
           $(foreach h,$(wildcard $(W32)/gen_include/*.h),--resource "$(h)=conversion/gen_include/$(notdir $(h))") \
           $(foreach h,cpu32.h tables32.h,--resource "$(W32)/runtime/$(h)=conversion/runtime/$(h)") \
           $(foreach h,$(CONV_CLANG_HDR),--resource "$(h)=conversion/clang/include/$(notdir $(h))") \
           --helper "$(LLVM_BIN)"
# the licenses of the code in the app (Contents/Resources/licenses); a game project adds its own with APP_FLAGS
APP_LICENSES = --resource "$(W32)/../LICENSE=licenses/Recomp-Framework-MIT.txt" \
               --resource "$(FFMPEG_SRC)/COPYING.LGPLv2.1=licenses/FFmpeg-LGPL-2.1.txt" \
               --resource "$(LLVM_SRC)/llvm/LICENSE.TXT=licenses/LLVM-Apache-2.0-with-LLVM-exception.txt" \
               --resource "$(CAPSTONE_SRC)/LICENSE.TXT=licenses/Capstone-BSD.txt" \
               --resource "$(CAPSTONE_SRC)/LICENSE_LLVM.TXT=licenses/Capstone-LLVM-parts.txt" \
               --resource "$(NRD_SRC)/LICENSE.txt=licenses/NVIDIA-NRD-RTX-SDKs-License.txt" \
               --resource "$(NRD_BUILD)/_deps/shadermake-src/LICENSE.txt=licenses/NVIDIA-ShaderMake-MIT.txt" \
               --resource "$(NRD_BUILD)/_deps/mathlib-src/LICENSE.txt=licenses/NVIDIA-MathLib-MIT.txt" \
               --resource "$(NRD_BUILD)/_deps/sse2neon-src/LICENSE=licenses/sse2neon-MIT.txt"
app: $(BUILD)/$(APP_NAME) $(FFMPEG_STAMP) $(BUILD)/conv_id $(BUILD)/provided.txt $(BUILD)/conv_cflags $(BUILD)/conv_ldflags $(LLVM_BIN)
	$(PYTHON) $(FW)/tools/make_app.py $(BUILD)/$(APP_NAME) $(GAME_EXE) "$(BUILD)/$(APP_FILE).app" --name "$(APP_NAME)" \
	    --display-name "$(APP_DISPLAY)" --short-name "$(APP_SHORT)" --bundle-id $(BUNDLE_ID) --category $(APP_CATEGORY) \
	    --min-macos $(MACOS_MIN) $(CONV_RES) $(APP_LICENSES) $$(for f in $(FFMPEG_OUT)/bundle/*.dylib; do printf -- '--lib %s ' "$$f"; done) --no-vulkan $(if $(APP_ICON),--icon "$(APP_ICON)") $(APP_FLAGS)

clean:
	rm -rf $(BUILD)

.PHONY: gen boot bundle runtime app clean
