# Call of Cthulhu: Dark Corners of the Earth for macOS (Apple silicon, Metal)

This project makes a native macOS app of *Call of Cthulhu: Dark Corners of the Earth* (2005) from your own copy of the game.

- The app runs natively on Apple silicon. It does not use Rosetta, Wine or a Vulkan translation layer.
- Direct3D 9 goes directly to Metal.
- The installer translates the game code of your copy on your Mac (static recompilation).

This repository contains no game files and no game code. You must own the game. The installer supports only the GOG release.

## Requirements

- A Mac with Apple silicon (M1 or later).
- macOS 14 or later.
- The Xcode Command Line Tools. The installer opens the install window if they are not installed.
- Homebrew (https://brew.sh). The installer uses Homebrew to install SDL3 for game controllers.
- An internet connection during the install. The installer downloads the FFmpeg source and two Python packages (`capstone`, `lief`).
- Approximately 1 GB of free disk space for the build.
- The game folder of the GOG release, with these contents:
  - The `Engine` folder with `CoCMainWin32.exe`.
  - The other game folders from the GOG install (for example `Scripts`, `Resources`, `MainMenu`).

The installer checks the SHA-256 of `Engine/CoCMainWin32.exe`. The supported value is:

```
f562e71e050565775f3bcf0ddbcec07fdac0deda4491ef98c956437ab61b13fe
```

The installer stops if the file has a different value.

### How to get the game folder on a Mac

- Copy the game folder from a Windows computer with the GOG release installed.
- Or extract the GOG offline installer (`setup_call_of_cthulhu_*.exe`) with `innoextract` (`brew install innoextract`).

## Install

1. Download this repository. Use **Code > Download ZIP** or the source archive of the latest release, or use `git clone`.
2. Open **Terminal**.
3. Type `cd` and a space. Drag the repository folder into the Terminal window. Push Return.
4. Start the installer:

   ```bash
   bash install.sh
   ```

5. When the installer asks for the game folder, drag the game folder into the Terminal window. Then push Return.
6. Wait until the installer shows `Done`. On the test Mac, the build took less than 1 minute. A slower Mac takes more time.
7. Open **Call of Cthulhu DCoTE** in the Applications folder.

You can also give the game folder directly:

```bash
bash install.sh "/path/to/Call of Cthulhu - DCotE"
```

To install the app in a different folder, use `--dest`:

```bash
bash install.sh --dest ~/Applications "/path/to/Call of Cthulhu - DCotE"
```

The app reads the game data from the game folder. Do not move or delete the game folder after the install. If you move the game folder, run the installer again.

## Saved games and settings

The app keeps saved games, game options and the enhancement settings in this folder:

```
~/Library/Application Support/CoCDCoTE-recomp
```

The installer does not change this folder.

## Enhancements

The game has a menu **Options > Enhancements**. The menu also shows in the pause menu.

| Setting | Values |
|---|---|
| Ambient Occlusion | Off, Low, Medium, High |
| GT7 Tone Mapping | Off, On |
| HDR Output | Off, On |
| Contact Shadows | Off, On |
| Bloom | Off, Low, Medium, High |
| Sharp Character Shadows (restart) | Off, On |
| Skip Intro Videos | Off, On |

**Options > Video** has the setting **Subtitle Size** (Game, 1.25x, 1.5x, 1.75x, 2x).

These items are always on: 16x anisotropic texture filtering, reduced color banding, 10-bit color output and game controller support (SDL3).

HDR Output needs a display with HDR (EDR) support. HDR Output and GT7 Tone Mapping change the 3D scene. The UI, the text and the menus stay at paper white.

- **Highlights:** the game clamps its final image to white. The app keeps the colors of the scene that are brighter than white (lamps, fire, bloom) from before the game's post-processing. With HDR Output, these highlights go above the white of the game, up to the peak. With GT7 Tone Mapping in SDR, they keep their detail.
- **Contact shadows** use the lights of the game: the light of the character nearest to the camera. A surface that the light does not reach gets no contact shadow.

### HDR Calibration

The game has a menu **Options > HDR Calibration**. The menu also shows in the pause menu. Set **HDR Output** to On before you use it. The test image shows only with HDR Output on.

| Setting | Values | Default |
|---|---|---|
| Peak (symbol barely visible) | 400 to 4000 nits | 1000 nits |
| Paper White | 50% to 200% of the white of macOS | 100% |

1. Find the square on the left of the test image. It has a ring and a dot in it.
2. Increase **Peak** until the symbol is almost not visible. Then go back one value, so that the symbol is barely visible.
3. Look at the bar and the gray steps on the right. Set **Paper White** to the brightness that you prefer for the white of the game and the UI. At 100%, the white of the game is equal to the white of other apps.

macOS does not give the brightness of the screen in nits. The app takes the peak of the screen as 100 nits × the potential EDR headroom of the screen (1600 nits on a Liquid Retina XDR display).

## Mac features

- **Full screen:** use **View > Enter Full Screen** (Control-Command-F) or the green window button. The image keeps its aspect ratio.
- **Pause in the background:** when you switch to a different app, the game stops and the sound stops. The game continues when you go back.
- **Game controllers:** controllers that macOS and SDL3 support work.

## Troubleshooting

- **"is not the supported version":** the `CoCMainWin32.exe` in your game folder is not the GOG release. Only the GOG release is supported.
- **"Homebrew was not found":** install Homebrew from https://brew.sh. Then start the installer again.
- **The look direction is inverted:** the game has its own setting. Go to **Options > Controls > Advanced** and change the invert setting.
- **The app does not find the game:** the game folder moved. Start the installer again with the new folder.

## Repository contents

| Folder or file | Contents |
|---|---|
| `install.sh` | The installer. It checks the game, installs the build tools, builds the app and installs it. |
| `CoC-Recomp/` | The game project: start code, the Enhancements menu, the game controller input and the instruction patches. `STATUS.md` records the work and the verification. |
| `Recomp-Framework/` | The recompiler and the Windows runtime for macOS (Win32, Direct3D 9 to Metal, audio, video). The license is MIT (`Recomp-Framework/LICENSE`). |

The build output (`CoC-Recomp/gen`, `CoC-Recomp/build`) contains the translated game code. Do not share it or the built app.

## Credits

- Recomp-Framework: NightSightProductions (MIT license).
- FFmpeg (LGPL): https://ffmpeg.org. The installer downloads and builds it on your Mac.
- SDL3 (zlib license): https://libsdl.org.
- *Call of Cthulhu: Dark Corners of the Earth* is the property of its rights holders. This project is not affiliated with them.
