# Call of Cthulhu: Dark Corners of the Earth on Recomp-Framework (win32 target: PE32 i386 -> C -> arm64).
#   ln -s "../Engine/CoCMainWin32.exe" orig.exe
#   make gen && make app
FW        = ../Recomp-Framework
GAME_EXE  = orig.exe
APP_NAME  = CoCDCoTE
APP_FILE  = Call of Cthulhu DCoTE
APP_DISPLAY = Call of Cthulhu: Dark Corners of the Earth
APP_SHORT = Call of Cthulhu
BUNDLE_ID = com.nightsight.cocdcote-recomp
GAME_SRC  = game.c enhance.c pad.c launcher.m
PATCHES   = patches.txt
# the oldest macOS of the app (LSMinimumSystemVersion, the compile target of the runtime and of the game code)
MACOS_MIN = 26.0
# SDL3 (gamepad input, pad.c): linked from SDL3_PREFIX and bundled in Contents/Frameworks; the binary loads the
# bundled copy (@rpath = @executable_path/../Frameworks, win32.mk)
SDL3_PREFIX ?= /opt/homebrew
SDL3_LIB  = $(SDL3_PREFIX)/lib/libSDL3.0.dylib
LDLIBS    = -L$(SDL3_PREFIX)/lib -lSDL3
POSTLINK  = install_name_tool -change "$$(otool -D $(SDL3_LIB) | tail -1)" @rpath/libSDL3.0.dylib $@ && codesign --force --sign - $@
APP_FLAGS = --lib $(SDL3_LIB) --resource "$(SDL3_PREFIX)/opt/sdl3/LICENSE.txt=licenses/SDL3-zlib.txt"
# the app icon: the 256 x 256 icon of the GOG release in the game folder (the icon of the exe has only 48 x 48)
APP_ICON = $(GAME_DIR)/goggame-1189711155.ico
include $(FW)/win32/win32.mk

# the game folder of the build (it contains Engine/CoCMainWin32.exe and the GOG icon, APP_ICON). The default is the
# folder this project is in. The app does not keep this path: the player chooses the game folder in the launcher.
GAME_DIR ?= $(CURDIR)/..
$(BUILD)/game/pad.o: CFLAGS += -I$(SDL3_PREFIX)/include

# Development: `make app` also puts the game bundle of this build where the app keeps the result of its first-start
# setup (Converted/<conversion ID>/game.bundle in the data folder of game.c). The app on the development Mac then
# starts the game with no setup. The release zip has no game code; a player's app makes the bundle on the first Play.
DEV_DATA = $(HOME)/Library/Application Support/CoCDCoTE-recomp
app: dev-bundle
dev-bundle: $(BUILD)/game.bundle $(BUILD)/conv_id
	d="$(DEV_DATA)/Converted/$$(cat $(BUILD)/conv_id)" && mkdir -p "$$d" && \
	    cp $(BUILD)/game.bundle "$$d/game.bundle.tmp" && mv "$$d/game.bundle.tmp" "$$d/game.bundle"
.PHONY: dev-bundle
