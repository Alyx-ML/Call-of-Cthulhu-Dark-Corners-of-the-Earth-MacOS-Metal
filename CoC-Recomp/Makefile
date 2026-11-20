# Call of Cthulhu: Dark Corners of the Earth on Recomp-Framework (win32 target: PE32 i386 -> C -> arm64).
#   ln -s "../Engine/CoCMainWin32.exe" orig.exe
#   make gen && make app
FW        = ../Recomp-Framework
GAME_EXE  = orig.exe
APP_NAME  = CoCDCoTE
APP_FILE  = Call of Cthulhu DCoTE
APP_DISPLAY = Call of Cthulhu: Dark Corners of the Earth
BUNDLE_ID = com.nightsight.cocdcote-recomp
GAME_SRC  = game.c enhance.c pad.c
PATCHES   = patches.txt
# SDL3 (gamepad input, pad.c): linked from SDL3_PREFIX and bundled in Contents/Frameworks; the binary loads the
# bundled copy (@rpath = @executable_path/../Frameworks)
SDL3_PREFIX ?= /opt/homebrew
SDL3_LIB  = $(SDL3_PREFIX)/lib/libSDL3.0.dylib
LDLIBS    = -L$(SDL3_PREFIX)/lib -lSDL3 -Wl,-rpath,@executable_path/../Frameworks
POSTLINK  = install_name_tool -change "$$(otool -D $(SDL3_LIB) | tail -1)" @rpath/libSDL3.0.dylib $@ && codesign --force --sign - $@
APP_FLAGS = --lib $(SDL3_LIB)
include $(FW)/win32/win32.mk

# the game folder (it contains Engine/CoCMainWin32.exe): game.c finds the game there when no folder is passed.
# The default is the folder this project is in. install.sh sets GAME_DIR to the player's game folder.
GAME_DIR ?= $(CURDIR)/..
$(BUILD)/game/game.o: CFLAGS += -DCOC_BUILD_GAME_DIR='"$(GAME_DIR)"'
$(BUILD)/game/pad.o: CFLAGS += -I$(SDL3_PREFIX)/include
