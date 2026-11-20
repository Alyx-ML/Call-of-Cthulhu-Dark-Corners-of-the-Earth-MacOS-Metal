#!/bin/bash
# Builds "Call of Cthulhu DCoTE.app" from your own copy of the game (GOG release) and installs it.
#
#   bash install.sh                         asks for the game folder
#   bash install.sh "/path/to/game folder"  uses that folder
#   bash install.sh --dest DIR ...          installs the app in DIR (default: /Applications)
#
# The game folder is the folder that contains Engine/CoCMainWin32.exe. The app reads the game data from this
# folder. Do not move or delete the folder after the install. If you move it, run this script again.
set -euo pipefail

EXE_SHA256="f562e71e050565775f3bcf0ddbcec07fdac0deda4491ef98c956437ab61b13fe"   # GOG release, game id 1189711155
FFMPEG_URL="https://github.com/FFmpeg/FFmpeg.git"
FFMPEG_COMMIT="3a0867c2bfda4a4d4309ca1a8cbdc6175e67f587"                          # FFmpeg 7.1.5
PY_PACKAGES=(capstone==5.0.7 lief==0.16.6)
APP_FILE="Call of Cthulhu DCoTE.app"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$ROOT/CoC-Recomp"
FW="$ROOT/Recomp-Framework"

fail() { printf '\nError: %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }

DEST="/Applications"
GAME=""
while [ $# -gt 0 ]; do
    case "$1" in
        --dest) [ $# -ge 2 ] || fail "--dest needs a folder"; DEST="$2"; shift 2 ;;
        -h|--help) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) fail "unknown option: $1" ;;
        *) [ -z "$GAME" ] || fail "give one game folder only"; GAME="$1"; shift ;;
    esac
done

step "Checking this Mac"
[ "$(uname -m)" = "arm64" ] || fail "this port needs a Mac with Apple silicon (arm64). This Mac is $(uname -m)."
MACOS="$(sw_vers -productVersion)"
[ "${MACOS%%.*}" -ge 14 ] || fail "this port needs macOS 14 or later. This Mac has macOS $MACOS."
if ! xcode-select -p >/dev/null 2>&1 || ! xcrun --find clang >/dev/null 2>&1; then
    xcode-select --install >/dev/null 2>&1 || true
    fail "the Xcode Command Line Tools are not installed. Complete the installer window that opened, then run this script again."
fi
for tool in clang make git python3 shasum; do
    command -v "$tool" >/dev/null 2>&1 || fail "the command '$tool' was not found. Install the Xcode Command Line Tools (xcode-select --install)."
done
echo "macOS $MACOS on Apple silicon; Command Line Tools at $(xcode-select -p)"

step "Finding the game"
if [ -z "$GAME" ]; then
    [ -t 0 ] || fail "give the game folder as an argument: bash install.sh \"/path/to/game folder\""
    echo "Drag the game folder (the folder that contains the Engine folder) into this window, then press Return:"
    IFS= read -r GAME
    # Terminal writes a dragged path with a backslash before each space and special character
    GAME="$(printf '%s' "$GAME" | sed -e 's/[[:space:]]*$//' -e 's/^[[:space:]]*//' -e "s/^'\(.*\)'\$/\1/" -e 's/^"\(.*\)"$/\1/' -e 's/\\\(.\)/\1/g')"
fi
[ -d "$GAME" ] || fail "the folder '$GAME' does not exist."
GAME="$(cd "$GAME" && pwd)"
case "$GAME" in
    *[\"\'\\\$\`]*) fail "the game folder path has a quote, backslash, dollar or backquote character. Rename the folder or move it to a path without these characters." ;;
esac
EXE="$GAME/Engine/CoCMainWin32.exe"
[ -f "$EXE" ] || fail "'$GAME' is not the game folder: it has no Engine/CoCMainWin32.exe."
SUM="$(shasum -a 256 "$EXE" | cut -d' ' -f1)"
[ "$SUM" = "$EXE_SHA256" ] || fail "Engine/CoCMainWin32.exe is not the supported version.
  Found SHA-256:    $SUM
  Expected SHA-256: $EXE_SHA256 (GOG release)
  Only the GOG release is supported."
echo "Game folder: $GAME"
echo "CoCMainWin32.exe is the supported GOG version."

step "Installing SDL3 (game controller support) with Homebrew"
command -v brew >/dev/null 2>&1 || fail "Homebrew was not found. Install it from https://brew.sh, then run this script again."
brew list sdl3 >/dev/null 2>&1 || brew install sdl3
SDL3_PREFIX="$(brew --prefix)"
[ -f "$SDL3_PREFIX/lib/libSDL3.0.dylib" ] || fail "SDL3 is installed, but $SDL3_PREFIX/lib/libSDL3.0.dylib was not found."
echo "SDL3: $SDL3_PREFIX"

step "Preparing the Python tools (capstone, lief)"
VENV="$PROJECT/build/venv"
if [ ! -x "$VENV/bin/python" ]; then
    mkdir -p "$PROJECT/build"
    python3 -m venv "$VENV" || fail "python3 could not make a virtual environment in $VENV."
fi
"$VENV/bin/python" -m pip install --quiet --disable-pip-version-check "${PY_PACKAGES[@]}" \
    || fail "pip could not install ${PY_PACKAGES[*]}."
"$VENV/bin/python" -c "import capstone, lief" || fail "the Python tools do not load in $VENV."

step "Getting the FFmpeg source (game videos)"
FFMPEG_SRC="$FW/third_party/ffmpeg"
if [ ! -f "$FFMPEG_SRC/configure" ]; then
    rm -rf "$FFMPEG_SRC"
    mkdir -p "$FFMPEG_SRC"
    git -C "$FFMPEG_SRC" init --quiet
    git -C "$FFMPEG_SRC" fetch --quiet --depth 1 "$FFMPEG_URL" "$FFMPEG_COMMIT" || fail "git could not download FFmpeg from $FFMPEG_URL."
    git -C "$FFMPEG_SRC" -c advice.detachedHead=false checkout --quiet FETCH_HEAD
fi
[ "$(git -C "$FFMPEG_SRC" rev-parse HEAD)" = "$FFMPEG_COMMIT" ] || fail "$FFMPEG_SRC is not FFmpeg commit $FFMPEG_COMMIT."

step "Translating the game code (this takes some minutes)"
cd "$PROJECT"
ln -sfn "$EXE" orig.exe
JOBS="$(sysctl -n hw.ncpu)"
MAKEVARS=(PYTHON="$VENV/bin/python" SDL3_PREFIX="$SDL3_PREFIX" GAME_DIR="$GAME")
make "${MAKEVARS[@]}" gen

step "Building the app (this takes some minutes)"
rm -f build/game/game.o   # it holds the game folder
make -j"$JOBS" "${MAKEVARS[@]}" app
[ -d "build/$APP_FILE" ] || fail "the build did not make build/$APP_FILE."

step "Installing the app in $DEST"
[ -d "$DEST" ] || fail "the folder '$DEST' does not exist."
[ -w "$DEST" ] || fail "you cannot write to '$DEST'. Use --dest with a different folder, for example --dest ~/Applications."
rm -rf "${DEST:?}/$APP_FILE"
ditto "build/$APP_FILE" "$DEST/$APP_FILE"

printf '\nDone. Open "%s" in %s to play.\n' "${APP_FILE%.app}" "$DEST"
echo "Saved games and settings are in ~/Library/Application Support/CoCDCoTE-recomp."
echo "The app reads the game data from: $GAME"
