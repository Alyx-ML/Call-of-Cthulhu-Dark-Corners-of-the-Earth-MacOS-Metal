# Call of Cthulhu: Dark Corners of the Earth: macOS recompilation status

This file records each step of the port. Each section states what was verified and what was not verified.

## 2026-11-19: Build and packaging

Supported executable: `Engine/CoCMainWin32.exe` from the GOG release (product 1189711155).
SHA-256: `f562e71e050565775f3bcf0ddbcec07fdac0deda4491ef98c956437ab61b13fe`.

### Pipeline

1. `recomp_w32.py` translates `orig.exe` (a link to `../Engine/CoCMainWin32.exe`) to C in `gen/`.
2. Clang compiles the generated C and the win32 runtime for arm64.
3. The Direct3D 9 runtime (`d3d9_metal.m`) sends Direct3D 9 calls directly to Metal. The section "2026-11-19: Native Metal renderer" gives the data.
4. `make app` makes `build/Call of Cthulhu DCoTE.app`.

### Runtime parts added for this game

| Part | File | Function |
|---|---|---|
| FMOD Ex 4.01 | `Recomp-Framework/win32/runtime/fmodex.c` | Sound on CoreAudio. Ogg Vorbis and WAVE PCM. 3D sound, lowpass DSP. |
| DirectShow | `Recomp-Framework/win32/runtime/dshow.c` | Filter graph, ASF (WMV) reader on FFmpeg, audio renderer on CoreAudio. Plays the videos in `Development/pcvideo`. |
| D3DX 9 (d3dx9_28) | `Recomp-Framework/win32/runtime/d3dx9.c` | Matrix and vector functions. Textures from DDS, TGA and BMP files. |
| Game settings | `game.c` | Game folder, SHA-256 check, data folder, command line, registry value. |

### Verified

- `make boot` links `build/CoCDCoTE`. The runtime files of this port compile with no warnings.
- `make app` makes the app. The app is arm64 only and has an ad-hoc signature.
- A host test of `d3dx9.c` loads all 138 DDS, TGA and BMP files of the game with no error.
- The same test checks the matrix inverse (maximum error 2.4e-7), a singular matrix (NULL result), the yaw direction and the FVF vertex size.
- The app refuses a folder without `Engine/CoCMainWin32.exe`. The app refuses an executable with a different SHA-256.

### Not verified

- The game was not started. The runtime has no headless mode, so a start opens a window and plays audio.
- FMOD, DirectShow and D3DX texture creation were not run against the game.
- FMOD error codes other than 0x21 (`FMOD_ERR_INVALID_HANDLE`) are not confirmed against `fmodex.dll`.

### Known limits

- DirectShow returns `E_NOTIMPL` for: `IGraphBuilder` RenderFile, AddSourceFilter, Connect, Reconnect, SetLogFile, SetDefaultSyncSource, Abort and ShouldOperationContinue; `IMediaControl` RenderFile, AddSourceFilter and the collection methods; all `IDispatch` methods; playback rates other than 1.0; a connection of the audio pin to a filter of the game.
- D3DX does not read JPG, PNG, HDR, PPM or DIB files. D3DX does not make P8 textures. D3DX ignores `D3DX_FILTER_DITHER`.

### Start the app

Prerequisite: the game folder contains `Engine/CoCMainWin32.exe`.

1. Open `build/Call of Cthulhu DCoTE.app`.

The app finds the game folder in this order:

1. The second argument: `CoCDCoTE CoCMainWin32.exe <game folder>`.
2. The environment variable `COCDCOTE_GAME_DIR`.
3. The folder that contains this project (set at build time).

Saves, settings and `registry.txt` are in `~/Library/Application Support/CoCDCoTE-recomp`.

## 2026-11-19: Native Metal renderer

The port does not use DXVK, Vulkan or KosmicKrisp. The Direct3D 9 runtime uses Metal directly.

### Parts

| Part | File | Function |
|---|---|---|
| Device, resources, presentation | `Recomp-Framework/win32/runtime/d3d9_metal.m` | `IDirect3D9`, `IDirect3DDevice9` (119 methods), textures, surfaces, volumes, vertex and index buffers, declarations, shaders, queries, state blocks and swap chains on Metal. |
| Shaders | `Recomp-Framework/win32/runtime/d3d9_shader.c` | Shader model 1.1 to 3.0 bytecode to Metal Shading Language (MSL). |
| Fixed-function pipeline | `Recomp-Framework/win32/runtime/d3d9_ffp.c` | Fixed-function vertex and pixel stages to MSL. |
| Shared MSL | `Recomp-Framework/win32/runtime/d3d9_prelude.h` | Register structs, constant buffers, D3D9 arithmetic, fog and alpha test. |

`Recomp-Framework/win32/win32.mk` builds these files. The build does not use `dxvk.mk`, `kosmickrisp.mk` or `d3d9_bridge.cpp`. These files are not deleted.

### Diagnostics

- Set `RT_VERBOSE=1` to write a line every 600 presented frames. The line gives the back-buffer size, the drawable size, the number of draws and the number of draws that were not drawn.
- Set `D3D9_MSL_DIR` to a folder to write each generated MSL source to that folder.
- Set `MTL_DEBUG_LAYER=1` to enable the Metal API validation layer.

### Verified

- `make boot` and `make app` link the game with the Metal runtime. The binary has no DXVK or Vulkan symbol of the D3D9 path.
- `d3d9_metal.m` compiles with `-Wall -Wextra` and no warnings.
- The shader test (`Recomp-Framework/win32/tests/d3d9_shader_test.m`) finds 119 shaders in the game data. The test translates 119 shaders and compiles all 112 unique shaders with Metal.
- The game starts on the Metal runtime. The log shows `device on Apple M4 Pro: 1800x1125 format 21, full screen, 0 samples, depth format 75`.
- The game plays `legal.wmv`, `beth_logo.wmv` and `hf_logo.wmv` through Direct3D 9 on Metal. In one run, the game presented 600 frames with 3133 draws. No draw was dropped.
- With `MTL_DEBUG_LAYER=1`, the Metal validation layer reported no error.
- The generated fixed-function and shader MSL compiled with no error during the runs.

### Not verified

- Gamma ramp, the D3D9 cursor, occlusion queries, multisampling and `StretchRect` were not run by the game in the test runs.

## 2026-11-20: Headless runs, x87 corrections, first level

### Run options

| Variable | Function |
|---|---|
| `RT_HEADLESS=1` | The app does not show a window, does not become the active application and does not take the keyboard focus. The game receives `WM_ACTIVATEAPP` and `WM_ACTIVATE`, so it does not pause. App Nap is disabled. |
| `RT_MUTE=1` | The audio output volume is 0. FMOD Ex and DirectShow audio continue to play at volume 0. |
| `RT_INPUT=SECONDS:KEY,...` | The app presses each key at the given time and releases it 100 ms later. Keys: `RETURN`, `ESCAPE`, `SPACE`, `TAB`, `UP`, `DOWN`, `LEFT`, `RIGHT`, `F1` to `F4`, one letter or one digit. The limit is 64 entries and 3600 s. A bad entry stops the app with an error. |

`RT_INPUT=135:RETURN,145:RETURN` selects New Game in the main menu and then the first difficulty. The intro videos take approximately 125 s.

### Translator corrections (`Recomp-Framework/win32/tools/recomp_w32.py`)

1. `fadd`, `fsub`, `fsubr`, `fmul`, `fdiv` and `fdivr` with one register operand (opcode D8) now calculate ST(0) = ST(0) op ST(i). Before, they wrote the result to ST(i). This defect changed approximately 1,070 instructions. It caused a heap corruption and a crash in the AutoSave write after New Game.
2. `fxch st(i)` now exchanges ST(0) and ST(i). Capstone gives the operands (st(0), st(i)), and the translator used the first operand. Before, all 218 `fxch` instructions in the game did nothing. This defect caused an invalid character reference in the script function `VM_GetPositionOfCharacterHead` and a crash at the end of the asylum cutscene. It also made the load of `01_HOUSE` very slow.
3. `fcomi`, `fucomi`, `fcompi` and `fucompi` now compare ST(0) with the operand that Capstone gives. Before, they always compared with ST(1). The game uses these instructions only with ST(1), so the generated code of the game did not change.

### Verified

- In headless runs, no window came to the front and no sound was audible.
- The game played the intro videos, showed the main menu, started a new game and played the asylum cutscene `MISC03_ASYLUM_CUTSCENE`.
- After correction 2, the game loaded `01_HOUSE`. The file trace showed `Backgrounds/01_HOUSE/*`, `Resources/Xbox/01_HOUSE.xbd`, `01_HOUSE.pc.xbd`, `Scripts/01_HOUSE.bat`, `Development/01_HOUSE.ai1`, `01_HOUSE.ai2`, `Pc Sound/xact/01_HOUSE/01_HOUSE.xap` and the IK animations. The game data has no `.ai3` file, so the failed open of `01_HOUSE.ai3` is correct.
- In one run of 560 s, the game presented approximately 62,000 frames (approximately 110 frames per second, not synchronized to the display). In the level, each frame had approximately 280 draws. No draw was dropped. The game did not stop or crash.
- With an lldb breakpoint on the "Character ID was invalid" check in `CoCCharacterReference::GetCharacter`, the check did not fail after correction 2. Before correction 2, it failed in three runs: one run crashed and two runs stopped at the breakpoint.

### Mouse in menus

The game moves its menu cursor with the relative mouse movement from DirectInput. It does not read the pointer position. The game sets the mouse to `DISCL_EXCLUSIVE | DISCL_FOREGROUND` (0x5) and the keyboard to `DISCL_NONEXCLUSIVE | DISCL_FOREGROUND` (0x6). Before, the runtime ignored these flags, and the macOS pointer stayed visible and moved apart from the game cursor.

The runtime now does these steps, as Windows does:

1. A read of a device that is not acquired fails with `DIERR_NOTACQUIRED`. After a loss of the foreground, the first read fails with `DIERR_INPUTLOST`. The game then calls `Acquire`.
2. A `DISCL_FOREGROUND` device can be acquired only while the game has the foreground. The runtime unacquires the device when the game loses the foreground.
3. While the exclusive mouse is acquired, the runtime moves the macOS pointer to the center of the window, hides it and detaches it from the mouse. The game cursor is then the only cursor. The runtime gives the pointer back when the mouse is unacquired, when the game loses the foreground and when the process stops.
4. The runtime keeps the fractional part of trackpad movement. Before, slow movement was lost.

`RT_HEADLESS=1` runs never hide or move the macOS pointer.

Verified: in a headless run, the log showed `SetCooperativeLevel(0x6)` for the keyboard and `SetCooperativeLevel(0x5)` for the mouse. The game acquired the keyboard after the first failed read, read both `RETURN` presses and started a new game.

Not verified: hiding and detaching the pointer in a window on screen, because the test runs were headless.

### Not verified

- Game play in `01_HOUSE` with player input. The test runs gave no input after the difficulty selection.
- The levels after `01_HOUSE`.
- The image on screen. The test runs did not capture or examine images.

## 2026-11-20: Enhancements menu

The game has a new menu, Options > Enhancements. The menu is also in the pause menu (Options). The menu has these settings:

| Setting | Values | Default | Part |
|---|---|---|---|
| Anisotropic Filtering | Game, 2x, 4x, 8x, 16x | 16x | `d3d9_metal.m` (`samp_get`) |
| Ambient Occlusion | Off, Low, Medium, High | Off | `d3d9_enhance.m` (XeGTAO, MIT License) |
| GT7 Tone Mapping | Off, On | Off | `d3d9_metal.m` (`present_enh_fs`, MIT License) |
| HDR Output | Off, On | Off | `d3d9_metal.m`, `cocoa32.m` (EDR layer) |
| Contact Shadows | Off, On | Off | `d3d9_enhance.m` (Bend Studio screen-space shadows, Apache License 2.0) |
| Gamepad (SDL3) | Off, On | On | `pad.c`, `dinput8.c` |

A change in the menu has an effect immediately. The game saves the settings in `Enhancements.cfg` in the data folder.

### Parts

| Part | File | Function |
|---|---|---|
| Settings and menu | `enhance.c`, `patches.txt` | The settings are in guest memory. The menu is made with the calls of the game's Sound menu. Four patched instructions add the menu, the two links and the menu text. |
| Settings contract | `Recomp-Framework/win32/runtime/rt_enh.h` | The settings struct and the functions between the game project and the renderer. |
| Scene passes | `Recomp-Framework/win32/runtime/d3d9_enhance.m` | Ambient occlusion and contact shadows. Compute passes read the depth buffer. A render pass multiplies the back buffer. |
| Gamepad | `pad.c` | Reads the first SDL3 gamepad. Gives the keys and mouse buttons of the game's own key configuration (`Options/Configuration.Data`). The right stick moves the mouse. |
| Build | `Makefile`, `Recomp-Framework/win32/win32.mk` | Links SDL3 and puts `libSDL3.0.dylib` in `Contents/Frameworks`. The binary loads that copy. |

### Gamepad controls

| Control | Action |
|---|---|
| Left stick | Forwards, Backwards, Strafe Left, Strafe Right. Arrow keys in menus. |
| Right stick | Mouse movement |
| A, B, X, Y | Jump (and Return), Crouch, Action, Reload |
| Left shoulder, right shoulder | Lean Left, Lean Right |
| Left trigger, right trigger | Aim, Fire |
| Left stick button, right stick button | Sneak, Look Strafe |
| D-pad up, D-pad down | Quick Heal, Hide/Show Weapon. Up and Down in menus. |
| D-pad left, D-pad right | Previous and next weapon slot (keys of slots 1 to 8). Left and Right in menus. |
| Back, Start, Misc | Inventory, Escape, Skip Scene |

### How the passes find their data

- Camera: the view and projection transforms of the last opaque draw into the back buffer with depth writes. A frame with a projection that is not a centered perspective projection gets no ambient occlusion and no contact shadows.
- Light of the contact shadows: the first draw of the frame into a 512 x 512 R5G6B5 target with no depth buffer (the character shadow projector). Vertex shader constants c0 to c3 hold the matrix of the light.
- The passes run once in each frame, at the first of these events after the scene: a change of render target, a copy or depth clear of the back buffer, or Present.
- HDR output uses an extended dynamic range (EDR) layer: RGBA16Float, extended linear sRGB. The SDR white of the game is 250 nits. The peak is 250 nits multiplied by the EDR headroom of the screen.

### Verified

- `make gen` applied the four patches. `make app` built the app with no new warnings. The app signature is valid. The binary loads SDL3 from `Contents/Frameworks` (dyld log).
- The Metal shader compiler compiled the shader source of `d3d9_enhance.m` and of `d3d9_metal.m` with no error.
- In a headless run, keys from `RT_INPUT` opened Options. The menu showed the Enhancements link. The Enhancements menu showed the six settings with the saved values. LEFT changed Anisotropic Filtering to 8x and the game saved the file. RIGHT changed it back to 16x and the game saved the file.
- In a headless run with all settings on, the level `01_HOUSE` ran approximately 13,000 frames with XeGTAO (3 slices, 3 steps, 1800 x 1125), Bend contact shadows (light at -53.2, -3633.7, -1028.4) and HDR output with GT7 tone mapping. With `MTL_DEBUG_LAYER=1`, the Metal validation layer reported no error. No draw was dropped.
- SDL 3.4.20 started in the game.

### Not verified

- The image on screen: the anisotropic filtering, the ambient occlusion, the contact shadows, the tone mapping and the HDR output. The test runs did not capture or examine images.
- A gamepad. No gamepad was connected during the test runs.
- The Enhancements link in the pause menu.
- One world unit is taken as 1 cm (UNCONFIRMED). The ambient occlusion radius (50 units) and the contact shadow fade distance (4000 to 8000 units) use this value.
- The Bend shader was not compared with the original Bend Studio source. The port in `Graphic Enhancements/contact-shadows-bend` was the source.

### Known limits

- The tone mapping and the HDR output also apply to the interface and the subtitles.
- Ambient occlusion and contact shadows are not available with multisampling. The log shows the cause.
- The contact shadows remove 60% of the color. The game has no separate light term.

### Rules

Do not publish, upload or commit the app, `gen/`, `build/` or the game files. The app contains the code of the game.

## 2026-11-20: Bloom, reduced banding, sharp character shadows, skip intro videos, subtitle size

The Enhancements menu has five new settings. The menu now has 11 settings.

| Setting | Values | Default | Applied by |
|---|---|---|---|
| Bloom | Off, Low, Medium, High | Off | `d3d9_enhance.m` (`d3d9_enh_bloom`) |
| Reduced Banding (restart) | Off, On | Off | `d3d9_metal.m` (render target storage, present) |
| Sharp Character Shadows (restart) | Off, On | Off | `d3d9_metal.m` (shadow target storage) |
| Subtitle Size | Game, 1.25x, 1.5x, 1.75x, 2x | Game | `enhance.c`, `patches.txt` |
| Skip Intro Videos | Off, On | Off | `enhance.c`, `patches.txt` |

The keys in `Enhancements.cfg` are `bloom`, `reduced_banding`, `sharp_character_shadows`, `subtitle_size` and `skip_intro`.

### Bloom

- The game's own glow pass gives no light. Its bright pass calculates `a * rgb - 1`. With 8-bit color this value is never more than 0.
- The bloom is a dual Kawase filter (Bjørge). The first downsample is the 13-tap filter of Jimenez with the Karis average. It has a soft threshold of 0.75 with a knee of 0.35.
- The filter uses 6 levels, from 1/2 to 1/64 of the back buffer size, in RGBA16Float textures.
- The intensity is 0.35 (Low), 0.7 (Medium) or 1.2 (High). These values are not tuned against images (UNCONFIRMED).
- The bloom is added to the back buffer in linear light at the end of the scene, before the interface.
- Frames with multisampling, or a back buffer smaller than 128 pixels, get no bloom. The log shows the cause one time.

### Reduced banding

- Render targets of format 21 (A8R8G8B8) and 22 (X8R8G8B8) in the default pool that the CPU cannot lock are stored as RGBA16Unorm. A unorm format keeps the range [0, 1] and the clamping of the 8-bit format.
- The present pass adds triangular noise of plus or minus 1/255 to the image when it converts 16-bit color to an 8-bit drawable. HDR output does not use the 8-bit path and gets no noise.
- Copies between the CPU and such a texture go through a temporary texture in the Direct3D format.
- The setting applies to render targets that the game creates after the change. The game creates one target when it starts and the other targets when a level loads. Restart the game after a change.

### Sharp character shadows

- The game draws the character shadows into six 512 x 512 R5G6B5 render targets. With this setting, these targets are stored at 2048 x 2048 with 8 bits per channel. Direct3D still sees 512 x 512.
- Viewports, scissor rectangles, clears and copies of such a target are scaled by 4.
- A draw into such a target with a depth buffer stops the program with an error. The game draws these targets with no depth buffer.
- The game creates the targets when a level loads. The new value applies at the next level load.

### Skip intro videos

- The patch at 0x4b235c reads the game's SkipVideos flag (CoCSettingsData +0x671). With the setting on, the flag reads as set. The game then opens the main menu directly.

### Subtitle size

- The game's subtitle layout (0x4c4590) copies CharacterHeight (CoCSettingsData +0x618, the text height as a part of the screen height, 0.040) into the subtitle object. The patch at 0x4c43fa multiplies this value by the subtitle size. The panel position and the speaker name follow from this value.
- The game runs the layout when it starts and when a game starts.
- The patch at 0x4c44d4 runs when a new subtitle shows. When the size changed after the last layout, the layout runs again before the subtitle shows. A subtitle that is on the screen during the change keeps the old size.

### HUD scale: not done

The game has no in-game HUD. In a level, the game draws only the scene and, during dialogue, the subtitles. The button display (`CoCButtonDisplay`) is the "Select" and "Back" strip of the menus. The interface items (`HfGuiItem`) use positions as a part of the screen. Text height is also a part of the screen height. Thus no HUD scale setting was added.

### Verified

- `make gen` applied the patches. `make app` built the app. The only warnings are the two `key_frame` deprecation warnings in `wmvcore.c`, which were there before.
- The Metal shader compiler compiled the shader source of `d3d9_enhance.m` and of `d3d9_metal.m` with no error.
- In a headless run with all settings on and `MTL_DEBUG_LAYER=1`, the level ran approximately 7,000 frames. The log shows the 16-bit render targets, the six shadow targets at 2048 x 2048, the bloom (6 levels from 900 x 562, intensity 0.70) and XeGTAO. The Metal validation layer reported no error. No draw was dropped.
- With Skip Intro Videos on, the game opened no intro video and showed the main menu.
- In a headless run, keys from `RT_INPUT` opened Options > Enhancements. The menu showed the 11 settings with the saved values. DOWN moved to Subtitle Size. RIGHT changed it from 1.5x to 1.75x. The game saved the file.
- The subtitle layout used a character height of 0.0600 at 1.5x and 0.0700 at 1.75x (log).

### Not verified

- The image on screen: the bloom, the banding, the shadows and the subtitles. The test runs did not capture or examine images.
- The second layout at 0x4c44d4 after a change in the pause menu. Keys from `RT_INPUT` did not open the pause menu in the level, and no subtitle was logged. The code was examined: the show function sets the text, the opacity, the priority and the visibility after the layout.
- Subtitles of 2 lines or more at 2x. The panel width (0.75 of the screen) does not change, so the text uses more lines.

## 2026-11-20: Look direction, anisotropic filtering, scene camera, HDR, full screen, pause, player install

### Inverted look

- The game has its own Invert Look option (Controls > Advanced, text 0xf91). The value is at +0x1c of the game options object (0x6e5078). The function at 0x54c140 reads it and swaps the up and down look input. This applies to the mouse and to the controller.
- The game saves this value in `Options/Configuration.Data` at byte 0x35. In the saved options of this installation, the value was 1. The runtime does not change the look direction.
- Byte 0x35 was changed from 1 to 0. The old file is `Configuration.Data.before-invert-fix` in the same folder. The file has no checksum.
- Marker values in a copy of the file identified byte 0x35. Byte 0x45 is the subtitle flag (+0x2c).

### Anisotropic filtering

- The Enhancements menu does not show anisotropic filtering. The value is 16x. The key `anisotropic_filtering` in `Enhancements.cfg` can set a different value. The menu now has 10 settings.

### Scene camera for ambient occlusion and contact shadows

- Cause: the effects got the camera from the fixed-function transforms (`SetTransform`). The game draws the world with vertex shaders. Thus the effects had no valid camera in a level and did not run.
- The world vertex shaders get `matFinal` (World x View x Projection) in the constants c0 to c3. `enhance.c` calls `d3d9_enh_camera_vs(0)`.
- `d3d9_metal.m` splits each matrix into a view part and a projection part. The split is valid only for a perspective projection with no skew: columns 0, 1 and 3 orthogonal, column 3 of length 1, column 2 equal to a x column 3, a > 0, b < 0. The matrix of the most draws in a frame is the scene camera. A frame with no valid matrix gets no ambient occlusion and no contact shadows.

### HDR

- Cause: the HDR expansion was applied to each color channel. This decreased the saturation of bright colors.
- The expansion is now applied to the luminance (Rec. 709 weights) and the color is scaled by the same ratio. Values to 0.6 do not change. Above 0.6 the curve rises to 8.2 x SDR white.

### Full screen

- The window can go to full screen with View > Enter Full Screen (Control-Command-F) or the green window button.
- When the game asks for full screen (Direct3D `Windowed = FALSE`), the window goes to full screen. The image keeps its aspect ratio, with black bars. Mouse positions are calculated for the image area.
- Headless runs do not change the window.

### Pause in the background

- When the app is not active, the game thread waits at `Present`. The audio output gives silence.
- `QueryPerformanceCounter`, `GetTickCount` and `timeGetTime` do not count the paused time. The game thus continues at the same game time after the pause.
- Headless runs do not pause.

### Player install

- `install.sh` (repository root) builds the app from the player's own game folder. It checks the SHA-256 of `Engine/CoCMainWin32.exe` and stops for a different file.
- The `Makefile` has the variable `GAME_DIR`. The default is the folder above this project, as before.

### Verified

- `make gen` and `make app` built the app. The only warnings are the two `key_frame` deprecation warnings in `wmvcore.c`.
- In a headless run with `MTL_DEBUG_LAYER=1`, the level ran approximately 7,800 frames. The log shows the scene camera from c0 to c3, XeGTAO, the contact shadows and the bloom. The Metal validation layer reported no error.
- The menu registered 10 settings.
- `install.sh` ran in a new clone of the repository with the game folder of this installation. It installed the Python tools, downloaded FFmpeg, translated the game code and built the app in 46 seconds on the test Mac. The installed app ran 60 seconds headless and reached the level with the scene camera, XeGTAO and the bloom. The log has no error.
- `install.sh` stopped with an error for a `CoCMainWin32.exe` with a different SHA-256, for a folder that does not exist, and for no folder with no terminal input.

### Not verified

- The image on screen: the strength of ambient occlusion and contact shadows, and the HDR colors. The test runs did not capture or examine images.
- Full screen and the pause. Headless runs do not change the window and do not pause. The code was examined.
- The look direction in a level after the change of byte 0x35. The game reads the value at start; no input test in the level examined the look direction.

## 2026-11-20: Menu layout, Subtitle Size in Video, ambient occlusion strength, HDR colors

### Menus

- The Enhancements menu has 7 settings: Ambient Occlusion, GT7 Tone Mapping, HDR Output, Contact Shadows, Bloom, Sharp Character Shadows (restart) and Skip Intro Videos.
- Cause of the bad position: the game gives each menu a rectangle (parts of the screen). The Enhancements menu used the rectangle of the game's short menus, (0.28, 0.33, 0.72, 0.73). The rows have a fixed spacing, so the 11 rows went below the rectangle and the long labels went into the value column.
- `enhance.c` (`menu_rects`) now gives the menu a rectangle from 0.15 to 0.85 of the screen width. The height is calculated for the rows and the Back row, and the title and the rows are centered on the screen. The row spacing (0.0545) and the space above and below the rows (0.035) are measured on the game's menus.
- Reduced banding and the gamepad are always on. They are not in the menu or in `Enhancements.cfg`. The loader skips the old keys `reduced_banding` and `gamepad`, and the next save removes them.
- Subtitle Size is in Options > Video (`VIDEOOPTIONS`), after the game's own setting. The patch at 0x412101 adds it.

### Ambient occlusion

- Cause: the passes ran too early in some scenes. The game removes its scene depth buffer and binds it again during the scene. A change of the depth buffer ended the scene, so the passes ran after 1 of approximately 700 scene draws and the later draws covered the result.
- Now only the removal or the replacement of the depth buffer that the scene uses ends the scene. Binding a depth buffer where there was none does not end it. After the change, the passes ran after all scene draws (for example 500 before, 4 after).
- The XeGTAO default strength was too weak for this game: the mean visibility of a level frame on High was 0.94 to 0.985. The levels now change the strength: effect radius 0.8, 1.2 and 1.6 m, final value power 2.4, 3.0 and 3.6. On High the mean visibility is now 0.86 to 0.89 in most level frames, and 15 to 20% of the pixels are darker by 30% or more (measured on the AO texture).

### HDR

- Cause of the low saturation: the SDR layer has no color space, so macOS shows the game's colors with the primaries of the screen (Display P3 on the test Mac). The EDR layer used extended linear sRGB, so macOS changed the colors to the smaller sRGB gamut.
- The EDR layer now uses the extended linear form of the screen's color space. The log of the test run shows `kCGColorSpaceDisplayP3`.
- The expansion knee for HDR output is now 0.25 of SDR white (sRGB 0.54). With the knee at 0.6, only values above sRGB 0.85 changed. Calculated for a gray with GT7 tone mapping and an EDR headroom of 2.67: sRGB 0.75 gives 0.68 x SDR white (0.52 before), sRGB 0.85 gives 1.32 x (0.72 before), sRGB 0.95 gives the peak 2.67 x (1.60 before). SDR output with tone mapping keeps the knee at 0.6.

### Verified

- `make gen` and `make app` built the app. The only warnings are the two `key_frame` deprecation warnings in `wmvcore.c`.
- Headless runs with keys from `RT_INPUT`: Options > Video > Subtitle Size changed from 1.75x to 2x and the game saved it. Options > Enhancements showed the new menu; Ambient Occlusion and Contact Shadows changed and the game saved them.
- The menu log shows 7 settings in ENHANCEDOPTIONS and Subtitle Size in VIDEOOPTIONS. An old settings file with `gamepad` and `reduced_banding` loaded with no message.
- In a level run with `MTL_DEBUG_LAYER=1`, the Metal validation layer reported no error. The log shows XeGTAO, the contact shadows, the bloom, the 16-bit render targets and HDR output with the Display P3 color space.

### Not verified

- The image on screen: the menu position, the strength of the ambient occlusion and the HDR colors. The test runs did not capture or examine images.

## 2026-11-20: Contact shadow light, HDR in EDR units, GT7 toe, Video menu position

### Contact shadows

- Cause: the light came from the projector of the character shadows. In a level this light is a lamp approximately 1.7 m above the ground, and it changes between frames (screen positions (1351, 296), (286, 525) and (1637, -32) in three measured frames). With this light, 25 to 65% of the frame was in shadow (mean of the shadow texture 0.25 to 0.65), and single columns differed from their neighbors by up to 0.2 (the vertical lines on the screen).
- The new function `d3d9_enh_light_dir` gives a fixed light. `enhance.c` sets a light straight above (+Z, the up axis of the world: the camera's up vector had a Z part of 0.92 to 1.0).
- Result (measured on the shadow texture in a level): the mean is 0.88 to 0.94, and the largest difference of a column from its neighbors is 0.05.
- With a fill value in the shadow texture, 34 to 385 of 2,025,000 pixels kept the fill value after the dispatches. Thus the dispatches write almost all pixels; the lines did not come from pixels that were not written.

### HDR

- The units are now EDR units: 1.0 = SDR white, which follows the display brightness. The value 2.5 (250 nits) of the GT7 reference is not used. The peak of HDR output is the EDR headroom of the screen (`maximumExtendedDynamicRangeColorComponentValue`), read again every 60 frames.
- The log shows the headroom now and the largest headroom of the screen (`maximumPotentialExtendedDynamicRangeColorComponentValue`). On the test Mac: 2.67 now, 16.0 possible. The headroom becomes larger when the display brightness is lower.
- HDR output without tone mapping has a new curve: above the knee (0.25) it has slope 1 at the knee and gives SDR white the peak. A color above the peak is scaled down as a whole, so its hue stays.

| sRGB value | SDR, GT7 before | SDR, GT7 now | HDR, no tone mapping | HDR, GT7 |
|---|---|---|---|---|
| 0.05 | 0.001 | 0.004 | 0.004 | 0.004 |
| 0.20 | 0.020 | 0.033 | 0.033 | 0.033 |
| 0.40 | 0.127 | 0.133 | 0.133 | 0.133 |
| 0.75 | 0.519 | 0.519 | 0.603 | 0.678 |
| 0.85 | 0.674 | 0.674 | 1.034 | 1.322 |
| 0.95 | 1.000 | 1.000 | 1.928 | 2.670 |
| 1.00 | 1.000 | 1.000 | 2.670 | 2.670 |

Values in units of SDR white, calculated for a gray with an EDR headroom of 2.67.

### GT7 tone mapping

- Cause of the dark image: the toe of the GT7 curve (strength 1.280). The game's image is already display-referred SDR, and the toe made the shadows darker: sRGB 0.20 by 40%, sRGB 0.05 by 75%. The toe strength is now 1.0, so values below the linear section do not change.

### Video menu

- The game draws the brightness test image of Options > Video at a fixed place. With the Subtitle Size row, the Back row was on the image. The rectangles of the Video menu now move up by one row (0.0545 of the screen height).

### Verified

- `make app` built the app. The only warnings are the two `key_frame` deprecation warnings in `wmvcore.c`.
- Options > Video > Subtitle Size changed from 2x to 1.75x in a headless run, and the game saved it.
- In a level run with `MTL_DEBUG_LAYER=1`, the Metal validation layer reported no error. The log shows the fixed light of the contact shadows, XeGTAO, the bloom and HDR output with the Display P3 color space.

### Not verified

- The image on screen: the Video menu position, the contact shadows, the HDR image and the GT7 image. The test runs did not capture or examine images.

## 2026-11-20: GT7 tone mapping curve

- Cause: with the toe strength 1.0 and the expansion knee at 0.6, the GT7 curve did not change sRGB values from 0.05 to 0.70 in SDR output. Thus the setting had no visible effect.
- The toe strength is now 1.06 and the expansion knee is 0.25 for SDR output and HDR output.

| sRGB value | 0.05 | 0.20 | 0.40 | 0.60 | 0.70 | 0.80 | 0.90 |
|---|---|---|---|---|---|---|---|
| Game | 0.05 | 0.20 | 0.40 | 0.60 | 0.70 | 0.80 | 0.90 |
| GT7, toe 1.280 (too dark) | 0.01 | 0.13 | 0.34 | 0.59 | 0.70 | 0.79 | 0.91 |
| GT7, toe 1.0 (no effect) | 0.05 | 0.20 | 0.40 | 0.60 | 0.70 | 0.79 | 0.91 |
| GT7 now | 0.04 | 0.18 | 0.39 | 0.60 | 0.75 | 0.90 | 1.00 |

SDR output, calculated for a gray.

- Verified: `make app` built the app with only the two `key_frame` warnings. A level run with `MTL_DEBUG_LAYER=1` had no Metal validation error. The log of the player's run before this change had no error.
- Not verified: the image on screen.

## 2026-11-20: HDR output of the scene only, and the HDR Calibration menu

### Cause of the problem

- HDR output expanded the full presented image: the scene and the UI.
- The expansion changed only values above the knee (linear 0.25, sRGB 0.54). Most pixels of a level are darker than the knee. Thus the scene changed little.
- The UI text and the menus are near SDR white. The expansion moved SDR white to the peak of the screen. Thus the UI became 2.67 times brighter.

### Scene and UI

- Measured in a level: after the 3D scene, the game makes its own post-processing in other render targets. Then it copies the result to the back buffer with one full-screen draw (no depth test, no blending). After that, the game draws the HUD, the text and the menus with alpha blending and with no depth test.
- The runtime (`d3d9_metal.m`, `tm_note`) now records each draw into the back buffer:
  - A draw with no depth test and with alpha blending is a UI draw.
  - Each other draw is a scene draw.
- At each change from scene draws to UI draws, the runtime copies the back buffer (`tm_ref`). A frame has a maximum of 4 copies.
  - Measured: 1 copy for each frame in a level. 2 copies for each frame during approximately 11 s near the start of the level. Menu frames have no scene draws and no copy.
- The presentation shader (`present_enh_fs`) gets the final image and the scene copy.
  - A pixel that the UI did not change gets the scene curve.
  - A pixel that the UI changed: final × paper white + (scene curve − scene × paper white) × min(Y final / Y scene, 1). Thus a fade keeps the curve in proportion, and the light of the UI stays at paper white.
  - A frame with no scene draw (a menu) stays at paper white. It gets no expansion and no GT7 tone mapping.
- GT7 tone mapping in SDR output uses the same scene copy. Thus GT7 no longer changes the UI.

### Units of HDR output

- macOS gives the brightness of the screen only as EDR headroom. EDR headroom is a factor of the current SDR white. macOS does not give a value in nits.
- The runtime takes the peak of the screen as 100 nits × the potential EDR headroom. This is an estimate. It agrees with a Liquid Retina XDR display: potential headroom 16, HDR peak 1600 nits.
- The test Mac: EDR headroom 2.67, potential headroom 16. Thus SDR white is approximately 600 nits, and the peak is approximately 1600 nits.
- The peak in EDR units = peak in nits × current headroom / (100 × potential headroom). macOS clips values above the current headroom.

### HDR Calibration menu

- **Options > HDR Calibration** (main menu and pause menu), after Enhancements.
- A link in the Enhancements menu did not open its menu: in a menu of the SOUNDOPTIONS kind, Return goes back to OPTIONS. This was also true for a link to SOUNDOPTIONS. Thus the link is in OPTIONS and INGAMEOPTIONS.
- Settings (Enhancements.cfg keys `hdr_peak` and `hdr_paper_white`):

| Setting | Values | Default |
|---|---|---|
| Peak (symbol barely visible) | 400, 600, 800, 1000, 1200, 1400, 1600, 2000, 3000, 4000 nits | 1000 nits |
| Paper White | 50%, 75%, 100%, 125%, 150%, 200% of SDR white | 100% |

- While the menu is on the screen (the menu manager at 0x6c8560, field +0x30), the runtime draws a test image from 0.6 to 0.84 of the screen height:
  - Left: a square at 64 × SDR white, which macOS clips to the peak of the screen. A ring and a dot in the square are at the selected peak. The symbol disappears when the selected peak is equal to or more than the peak of the screen.
  - Right: a bar at paper white (the UI), and below it ten steps of the game's gray scale (sRGB 0.1 to 1.0) through the scene curve.
- The test image shows only with HDR output on.

### Verification

- `make app` built the app with only the two `key_frame` warnings.
- Headless runs with `MTL_DEBUG_LAYER=1` had no Metal validation error: the main menu, the HDR Calibration menu, and a level with ambient occlusion, contact shadows, bloom, GT7 tone mapping and HDR output.
- In the HDR Calibration menu, Right and Left changed the peak (1000, 1200, 1400 nits) and paper white (100%, 125%). Each change was saved to Enhancements.cfg, and the runtime used the new values in the next frame (log: "HDR output: peak 1200 nits (2.00 x SDR white)").
- The log showed "the HDR Calibration menu is on the screen" when the menu opened, and "off the screen" after Escape.
- Not verified: the image on the screen (screenshots are not permitted). The layout of the OPTIONS menu with one more row and the position of the test image are not verified on the screen.

## 2026-11-20: Highlights above white, HDR bloom, 10-bit output, contact shadows from the game's lights

### Highlights above white

- Measured with float render targets: the game's post-processing ends with fixed-function texture stages. These stages clamp each result to 1 (Direct3D 9 behavior). Thus the final image of the game is never above white.
- Measured before the post-processing (at the first copy of the back buffer): the scene is above white in up to 0.37% of the pixels, with a maximum of 1.84 (additive blending).
- The runtime keeps these values:
  - The back buffer of the swap chain is RGBA16Float (half float). The other 32-bit color render targets stay RGBA16Unorm. Thus the game's own post-processing does not change.
  - At the end of the opaque scene, after ambient occlusion, contact shadows and bloom, the runtime copies the back buffer (`tm_pre`). This copy occurs only with HDR output or GT7 tone mapping on.
  - The presentation shader multiplies the scene by luminance(pre) / luminance(pre clamped to white), in linear light. The factor is 1 where the copy is not above white. The factor keeps the hue of the post-processed image (for example black and white, or sepia).
- The scene curve now accepts values above white:
  - HDR output without GT7: white goes to 1 + 0.75 × (peak / paper white − 1). Above white, an exponential curve continues with the same slope and approaches the peak.
  - GT7: a value above white gets 14.5 × paper white × its luminance. Then the GT7 curve compresses it to the peak.
- Measured in a level, peak 1000 nits (1.67 × SDR white): with GT7, up to 0.17% of the pixels were above SDR white, with a maximum of 1.666 (the peak). Without GT7, 0.23% of the pixels were above 1.5 (the new white) and reached the peak. No pixel was negative or NaN.

### Bloom in HDR

- The bloom passes no longer clamp to white. With the half-float back buffer, the bloom gets the scene values above white, and the composite stays above white. Thus `tm_pre` contains the bloom above white.

### 10-bit output

- The SDR layer is now BGR10A2Unorm (10 bits per channel). It was BGRA8Unorm. The HDR layer stays RGBA16Float.
- The dither of the presentation is now one step of the output: 1/1023 for a 10-bit drawable, 1/255 for an 8-bit drawable.
- `GetFrontBufferData` converts a half-float front buffer to 8 bits (clamped to 0..1).
- Measured: drawable format 94 (BGR10A2Unorm), back buffer format 115 (RGBA16Float), dither on, in SDR with GT7 on and off.

### Contact shadows from the game's lights

- The constant tables (CTAB) of the game's shaders give the names of the constants. The character shaders (`Skinning_*bone*.vso`) light the normal maps with:
  - `skinLightDir` (c87): a unit vector toward the light, in world space, at `skinCenterPos` (c86).
  - `skinLightGradients` (c89, c91, c93): the derivatives of that vector along world x, y and z. c90, c92 and c94 are the derivatives of the light color.
  - The pixel shader calculates saturate(N·L). Thus c87 points toward the light.
- Measured in a level: the derivatives agree with a point light: −(I − L Lᵀ) / r. Example: predicted (−0.0062, 0.0022, −0.0043), measured (−0.0068, 0.0020, −0.0040), r approximately 100. Thus r = −2 / trace, and the light is at center + r × L.
- New runtime function `d3d9_enh_light_vs` (`rt_enh.h`). New shader function `d3d9_shader_const_reg` (`d3d9_shader.c`) reads a constant table. The game project calls `d3d9_enh_light_vs("skinLightDir", "skinCenterPos", "skinLightGradients", 2)` in place of the fixed light above.
- The light of a frame is the light of the character draw nearest the camera. The light moves 15% of the way to a new light in each frame. A frame with no character draw keeps the last light. Until the first character draw, no frame has contact shadows.
- Measured: a directional light at the camera (a character draw at the camera position, with no derivatives) and point lights 155 to 570 units from the characters.

### Cause of the contact shadow artifacts

- Measured with the game's light, before this change: 21% to 99.9% of a level frame had more than 20% darkening. In the measured level, the light of the nearest character was 75 units below the center of the character.
- Measured with the old fixed light straight above: 1% to 26% of a level frame had more than 20% darkening.
- Cause: the shadow of one light darkened the full color. A surface that faces away from the light, or that the light only grazes, has no light from it. The screen-space ray of such a surface goes into the surface, and Bend shows it as shadow.
- Correction: the composite (`d3d9_enhance.m`, `enh_fs`) multiplies the strength by N·L. N is the normal from the depth buffer (on each axis, the neighbor with the smaller depth step). L is the direction to the light in view space.
- Measured after the correction: with the game's light, 0.02% to 2.4% of a level frame has more than 20% darkening. With the fixed light straight above, 0.01% to 1.8%. The shadows that stay are on surfaces that the light reaches.

### Verification

- `make app` built the app with no warnings.
- Headless runs with `MTL_DEBUG_LAYER=1` had no Metal validation error: a level with HDR output, GT7, ambient occlusion, contact shadows and bloom; a level in SDR with GT7 on and off; the HDR Calibration menu.
- `d3d9_shader_const_reg` gave the same registers as a separate Python reader for 7 constant names in all 119 shader files of the game. Corrupted copies of each shader (changed words and all truncations) had no error with AddressSanitizer and UndefinedBehaviorSanitizer.
- The measurement code was temporary. It is removed.
- Not verified: the image on the screen (screenshots are not permitted). `GetFrontBufferData` with a half-float front buffer did not run in the tests.

## 2026-11-20: GT7 tone mapping as in the reference, bloom without a threshold, softer N·L weight of contact shadows

### GT7 tone mapping

- Reference: `gt7_tone_mapping.cpp` v1.0 (Polyphony Digital Inc., MIT License), from the SIGGRAPH 2025 course "Physically Based Shading in Theory and Practice" (https://blog.selfshadow.com/publications/s2025-shading-course/pdi/supplemental/gt7_tone_mapping.cpp).
- Cause of the muted image: the earlier port did not use the units of the reference.
  - The reference uses frame-buffer units: 1.0 is 100 nits. The mid point 0.538 of the toe is in these units.
  - The earlier port used EDR units (1.0 is SDR white) with the target 1.0 for SDR output. Thus the toe acted on all values below 0.538 of white (sRGB 0.76), and most of the image was in the toe.
  - The earlier port also had a toe strength of 1.06 in place of 1.280, and an inverse Reinhard expansion before the curve.
- Correction (`d3d9_metal.m`, `scene_tm`, `present_tm_consts`):
  - SDR output (`initializeAsSDR`): the target is 2.5 (250 nits, the SDR paper white of the reference). The white of the game is at 2.5. The output is divided by 2.5.
  - HDR output (`initializeAsHDR`): the target is the calibrated peak in nits / 100. The white of the game is at the calibrated paper white in nits / 100. The output in nits is converted to EDR units.
  - The parameters are those of the reference: alpha 0.25, mid point 0.538, linear section 0.444, toe strength 1.280, blend ratio 0.6, chroma fade 0.98 to 1.16.
  - GT7 has no inverse Reinhard expansion now. A color above white comes from the highlights of `tm_pre`. HDR output without GT7 does not change.
- Calculated for a gray in SDR output: linear 0.215 to 0.444 (sRGB 0.50 to 0.70) does not change. The white of the game goes to linear 0.835 (sRGB 0.92). A highlight above white goes toward 1.0. Linear 0.05 (sRGB 0.25) goes to 0.036 (sRGB 0.21).
- Calculated for HDR output, peak 1000 nits, paper white 600 nits: the white of the game goes to 585 nits. A highlight of 4 × white goes to the peak.

### Bloom

- Measured on the back buffer before the bloom, in a level: the brightest pixel was 1.0 to 1.8 of linear white, and 0.000% to 0.007% of the pixels were above white. The mean of the earlier bright pass (soft threshold 0.75) was 0.00004 of white per pixel. Thus the bloom was almost not visible.
- Correction (`d3d9_enhance.m`): the bloom of Jorge Jimenez (SIGGRAPH 2014) has no threshold. The composite mixes the mean of the levels into the scene: scene × (1 − k) + bloom × k. k is 0.04 (low, the value of Jimenez), 0.08 (medium) and 0.15 (high). The bloom moves light from bright areas into their surroundings. It does not add light to the image.
- Measured after the correction with bloom high: the mean luminance of the frame did not change (0.0135 before and after). The mean change was 0.0002 to 0.0005 per pixel. The maximum change near lamps was 0.10 to 0.18 of linear white.

### Contact shadows

- The N·L weight of the contact shadow is now smoothstep(0, 0.25, N·L). The weight was N·L. A surface that the light reaches at an angle up to 76 degrees from its normal now gets the full contact shadow. A surface that faces away from the light still gets no contact shadow.
- The contact shadows are a port of the screen-space shadows of Bend Studio (`bend_sss_gpu.h`, `bend_sss_cpu.h`) with the default settings of Bend: 60 samples, 4 hard samples, 8 fade samples, surface thickness 0.005, bilinear threshold 0.02, contrast 4.

### Verification

- `make app` built the app with no warnings.
- Two headless level runs (45 and 55 seconds) with HDR output, GT7, ambient occlusion high, contact shadows and bloom high. The second run had `MTL_DEBUG_LAYER=1` and no Metal validation error.
- The measurement code was temporary. It is removed.
- Not verified: the image on the screen (screenshots are not permitted).

## 2026-11-20: Frame pacing at 60 Hz, field of view, sanity effect strength, MetalFX anti-aliasing

### Frame rate and frame pacing

- Measured before the change: on a ProMotion display, the runtime presented 120 frames per second. The game asks for a full-screen refresh rate of 60 Hz (`RefreshRate` 60, `D3DPRESENT_INTERVAL_ONE`).
- The frame rate stays at 60. The scripts of the game fail above 60 frames per second (crashes in the prologue basement and in the house library, from player reports). The GOG release limits the game to 60 frames per second for this reason (notes of the DCoTE Unofficial Patch v1.7, https://www.moddb.com/games/call-of-cthulhu-dark-corners-of-the-earth/downloads/dcote-unofficial-patch-v17; https://steamcommunity.com/app/22340/discussions/0/620695877170217098; https://steamcommunity.com/app/22340/discussions/0/600765934005656804).
- Change (`d3d9_metal.m`, `pace_setup`, `pace_drawable`, `pace_wait`):
  - When the game asks for a full-screen refresh rate and a presentation interval that is not `D3DPRESENT_INTERVAL_IMMEDIATE`, the runtime presents at that rate.
  - Each frame starts on a fixed time grid with a step of 1 / rate. When `nextDrawable` waits more than 0.5 ms, the grid moves later. The grid does not move earlier.
  - The drawable is presented with `presentDrawable:afterMinimumDuration:` (1 / rate).
- `CAMetalDisplayLink` was tested first. In a headless run it gave 3 updates and then no more updates, and the game stopped. It is not used.
- Change (`cocoa32.m`): the game thread has the QoS class `QOS_CLASS_USER_INTERACTIVE`. With the default QoS class, the timed waits of the pacing ended 1 to 3.6 ms late (measured).
- Measured after the change, in the menus and in a level: 60.0 frames per second; frame times 15.7 to 17.7 ms; 0 to 1 of 240 frames more than 1 ms from 16.7 ms.

### Field of view

- All cameras of the game give the aspect ratio 1.3256 to `D3DXMatrixPerspectiveFovRH` (0x4347c0). On the 1800 × 1125 back buffer (1.6), the 3D image was 21% too wide.
- Change (patch 0x4347e9, `coc_enh_aspect`): the aspect ratio is the aspect ratio of the back buffer. The vertical field of view does not change.
- Change (patch 0x4352c0, `coc_enh_player_fov`): the setting **Field of View** (Game, 110%, 120%, 130%, 140%) multiplies the field of view of the player camera (`CoCPlayerFovManager`; the game: 55 degrees vertical). Cut scenes and the other cameras keep their field of view.
- Measured: the log shows "camera aspect ratio 1.6000 (the back buffer 1800x1125; the game gave 1.3256)". With 120%, the player field of view changed from 55 to 66 degrees and from 39.5 to 47.4 degrees.

### Sanity effects

- The game has five visual sanity effects: Black And White (0x55d850), Blurred Vision (0x55e000), Vertigo Blur (0x55f070), Screen Distort (0x55f340) and Camera Motion Blur (0x55f4c0). Each Start function gives an intensity from 0 to 1 to the camera effect of its id (1, 9, 0xe, 0xd, 0xc) through the set-intensity function 0x4649c0. Each Stop function gives 0.
- Change (patches 0x55d88e, 0x55e01d, 0x55f08d, 0x55f35d, 0x55f58f; `coc_enh_sanity`): the setting **Sanity Effects** (Game, 75%, 50%, 25%, Off) multiplies this intensity. Off gives 0, as Stop does. The sounds, the sanity value and the other sanity effects do not change.
- Verified with temporary test code in a level: Start of Black And White with the intensity 1.0 gave the camera effect the intensity 0.5 at 50%, and 0 after Stop. At Off, Black And White and Camera Motion Blur got 0.
- Not verified: a sanity event in normal play. The reachable part of the first level had no sanity event in the test time.

### Anti-aliasing

- The setting **Anti-Aliasing (MetalFX)** (Off, On) is new. MetalFX is a part of macOS. SMAA T2x was not selected: it needs the SMAA reference source and its look-up tables, which must be downloaded.
- Change (`d3d9_metal.m`, `taa_offset`, `enh_apply`; `d3d9_enhance.m`, `d3d9_enh_taa`):
  - The MetalFX temporal scaler runs at the size of the back buffer (input size = output size).
  - The draws into the back buffer with a depth buffer, before the end of the opaque scene, move by a sub-pixel offset: the Halton (2, 3) sequence, 8 phases. The offset is added to the half-pixel offset of the vertex constants. The post-processing and the UI do not move.
  - The motion vectors come from the depth buffer and the camera of this frame and of the last frame. They are in pixels (y down), from the pixel to its position in the last frame.
  - The output of MetalFX replaces the back buffer before the bloom.
- Limit: the game's vertex shaders do not give the positions of the last frame. An object that moves by itself (a character, a door) gets the motion of the camera only. At such pixels MetalFX finds that the history does not agree with the frame and uses less history.
- Measured in a level with an almost static camera (a 256 × 256 area at the center, 15 frames): the mean change from frame to frame was 0.0048 at the input of MetalFX and 0.00034 at the output (14 times less). With the sign of the MetalFX jitter reversed for a test, the output change was 0.00058. Thus the sign of the documented convention is correct.
- Measured: 60.0 frames per second with anti-aliasing, ambient occlusion high, contact shadows, bloom high, HDR output and GT7.
- `win32.mk` links `MetalFX.framework`.

### Not done

- Screen-space reflections on water and wet surfaces:
  - The first level has no water draws. The water shaders (`WaterDefault`, `Fresnel`, `WaterFog`) are loaded, but no draw uses them (measured in a 60 second level run).
  - The game has no material or shader for wet surfaces. A wet surface uses the same shader as a dry surface.
  - The design needs the data of a water draw: its textures, its depth writes and its blend state. This needs a saved game at a place with water.
- Engine bug fixes from the community lists:
  - The 60 Hz limit is done.
  - The DCoTE Unofficial Patch v1.7 has fixes for the blue light shader of the reef and for the sky dome shader. To read these fixes, the patch must be downloaded.

### Verification

- `make gen` and `make app` built the app. The only warnings are the two known `key_frame` warnings of `wmvcore.c`.
- Headless level runs of 40 to 70 seconds with all settings on: no error, 60.0 frames per second, a physical footprint of 776 MB that did not increase.
- One headless run ended at approximately 59 seconds with `SIGKILL`. The log has no error, the system log has no kill record, and there is no crash report. The next run of the same build ran for 70 seconds. The cause is not known.
- The measurement code was temporary. It is removed.
- Not verified: the image on the screen (screenshots are not permitted).

## 2026-11-20: HDR calibration of the RE Engine

The HDR Calibration menu now has the two settings of the Capcom RE Engine (Resident Evil Village, Resident Evil 4), with the same steps and the same values in nits.

### Sources

- [KoKlusz HDR Gaming Database, Resident Evil Village (PC) HDR Analysis](https://github.com/KoKlusz/HDR-Gaming-Database/discussions/62) and [Resident Evil 4 (PC) HDR Analysis](https://github.com/KoKlusz/HDR-Gaming-Database/discussions/63), published 2025-10-07, read 2026-11-20: the tables of both settings.
- [hdrgamer, Resident Evil 4 HDR Settings](https://www.hdrgamer.com/2023/03/resident-evil-4-hdr-settings.html), published 2023-03-24, and [Resident Evil Village PS5 HDR](https://www.hdrgamer.com/2022/01/resident-evil-village-ps5-hdr-peak.html), published 2022-01-15, read 2026-11-20: the same Max Brightness table. The Village table gives the brightness of the test square as equal to the setting.
- [Steam, Resident Evil 4 discussion](https://steamcommunity.com/app/2050650/discussions/0/3827536762636739149): the first screen is set until the outline of the block is barely visible.
- These are secondary sources. Capcom does not publish the values.

### The settings

| Step | Max Brightness (HDR), nits | Brightness (HDR), nits |
|---|---|---|
| -10 | 300 | 50 |
| -9 | 400 | 80 |
| -8 | 500 | 120 |
| -7 | 600 | 160 |
| -6 | 700 | 180 |
| -5 | 750 | 200 |
| -4 | 800 | 220 |
| -3 | 850 | 240 |
| -2 | 900 | 260 |
| -1 | 950 | 280 |
| 0 (default) | 1000 | 300 |
| +1 | 1100 | 320 |
| +2 | 1200 | 340 |
| +3 | 1400 | 380 |
| +4 | 1600 | 420 |
| +5 | 1800 | 460 |
| +6 | 2000 | 500 |
| +7 | 2500 | 540 |
| +8 | 3000 | 580 |
| +9 | 3500 | 610 |
| +10 | 4000 | 640 |

- Change (`rt_enh.h`): `RT_ENH_HDR_PEAK_NITS` has the 21 Max Brightness values. `RT_ENH_HDR_PAPER_NITS` has the 21 Brightness values and replaces `RT_ENH_HDR_PAPER_PCT` (a percentage of the SDR white of macOS). `RT_ENH_HDR_DEFAULT` is the index of step 0.
- Change (`d3d9_metal.m`): paper white is now in nits. The runtime converts it to EDR units with the SDR white of the screen (100 nits × potential EDR headroom / current EDR headroom), as it does for the peak. The log gives both values in nits and in EDR units.
- Change (`enhance.c`): the menu rows are **Max Brightness (HDR)** and **Brightness (HDR)**. Each value shows the step and the nits, for example "0 (1000 nits)".
- Change (`enhance.c`): the Enhancements.cfg keys are now `hdr_max_brightness` and `hdr_brightness` (index 0 to 20, default 10). The old keys `hdr_peak` and `hdr_paper_white` had other steps. The loader skips them, and the next save removes them. Thus an old file gets the defaults for these two settings.
- Change (`d3d9_metal.m`, `calib`): the first test is now the test square of the RE Engine: a square at the selected peak in a field at the maximum of the screen. The outline of the square disappears when the selected peak is at the peak of the screen. The ring and the dot are removed.
- Not changed: the second test (the bar at paper white above the ten gray steps). The RE Engine shows red and blue areas on this screen. The sources do not describe how these areas change, so this test is not copied.
- Not changed: the RE Engine shows the two settings on two screens. This menu shows them on one screen, above the test image.

### Verification

- `make app` built the app. The only warnings are the two known `key_frame` warnings of `wmvcore.c`.
- Headless run (35 seconds) with an old Enhancements.cfg (`hdr_peak=3`, `hdr_paper_white=2`): no error and no "not a valid setting" message. The log shows "max brightness 0 (1000 nits), brightness 0 (300 nits)" and "peak 1000 nits (1.67 x SDR white), paper white 300 nits (0.50 x SDR white)" with an SDR white of approximately 600 nits. The HDRCALIBRATION menu was registered; the menu code stops with an error if a control does not have 21 values.
- Headless run (25 seconds) with `hdr_max_brightness=20` and `hdr_brightness=0`: the log shows "+10 (4000 nits)", "-10 (50 nits)", "peak 4000 nits (6.67 x SDR white)" and "paper white 50 nits (0.08 x SDR white)".
- The presentation shader with the new test square compiled (the runs presented frames).
- Not verified: the test image on the screen. The headless runs did not open the HDR Calibration menu, and screenshots are not permitted.

## 2026-11-20: Unofficial Patch 1.7, texture LOD bias, Retina resolution, Mac menus and icon, headphone audio

### DCoTE Unofficial Patch 1.7

- The user downloaded `DCoTEPatch1.7.zip` (MD5 d3a9971267c1c3b9efece732c3330528, equal to the published value). It contains one file, `DCoTEPatch.exe`, a .NET patcher. It was not run. Its IL was read with System.Reflection.Metadata (a dump tool in the scratch folder).
- The two shader fixes of the patch ("by Guzz") change bytes of two game files:
  - Blue light (`Resources/PC/SHADERS/psModulateColorByAlpha.pso`): bytes 432 to 443, the constant `def c0` of the pixel shader, change from -0.009 (`0xBC1374BC`) to -1.0 (`0xBF800000`) in x, y and z.
  - Sky dome (`Resources/PC/SHADERS/Skydome_NoColor.vso`): byte 1116, the source register of `mad r1.xyz, v?, c6.w, c6`, changes from `v0` to `v1`.
- Result: the files of the GOG release already have both fixes (-1.0 and `v1`). No change is necessary.
- The other items of the patch are options and cheats (for example film grain, the legal video, weapons, debug mode). They are not in this task.

### Texture LOD bias with anti-aliasing

- Change (`d3d9_metal.m`, `taa_draw_offset`, `ps_consts_header`): the scene draws with a sub-pixel offset of anti-aliasing add `TAA_LOD_BIAS` (-1.0) to `D3DSAMP_MIPMAPLODBIAS` of all their samplers. The value is the formula of the MetalFX integration guide of Apple, log2(input size / output size) - 1, with the input size equal to the output size. The UI and the post-processing do not change.
- `taa_offset` now returns 1 when the frame has offsets, so that the bias applies to the same draws as the offset.
- Verified: the log shows "anti-aliasing: frame 754, texture LOD bias -1.0 on the scene draws".

### Retina resolution

- Finding: the game used the size of the screen in points (1800 × 1125) as its resolution. On a Retina screen (backing scale factor 2), macOS scaled this image up by 2.
- The game reads its resolution from the registry (`ScreenWidth`, `ScreenHeight`) into a settings object (0x40b440, +8 and +0xc; the defaults are 800 × 600). It writes the object back to the registry (0x40b6d3).
- The assert text "Resolution has been set too high, the glass shatter routine has overwritten memory" (0x59ef09, 0x59f7ac, 0x59f7d2) is about the mesh of the glass shards: the loops make rings of 17 points around world positions. They do not use the screen resolution (inference from the code).
- Change (setting **Retina Resolution (restart)**, `retina_resolution`; `rt_enh.h` `retina`):
  - `enhance.c`, `coc_enh_resolution` (patch 0x40b533): with the setting, the settings object gets the size of the screen in pixels. Without it, a resolution larger than the screen in points becomes the screen in points.
  - `d3d9_metal.m`, `mode_list`: with the setting, the display modes include the screen in pixels.
  - `cocoa32.m`, `rt_cocoa_screen_pixels`, `content_points`: the size of the screen in pixels; a window for a client area larger than the screen in points gets its size in points (pixels / backing scale factor).
- Verified (headless run with the setting): "resolution 3600x2250 (the game's settings: 1800x1125; Retina Resolution on)"; the device, the back buffer and the drawable were 3600 × 2250.
- Verified (headless run without the setting and with 3600 × 2250 in the registry): "resolution 1800x1125 (the game's settings: 3600x2250; Retina Resolution off)".
- Measured: with all enhancements on (ambient occlusion high, contact shadows, bloom medium, HDR, GT7 tone mapping, anti-aliasing), 600 frames took 10.86 s and 11.16 s at 3600 × 2250 (55 and 54 frames per second), and 10.0 s at 1800 × 1125 (60 frames per second).

### Mac menus, window size, icon and name

- Change (`cocoa32.m`): the app menu has About, Hide (Command-H), Hide Others (Option-Command-H), Show All and Quit (Command-Q), with the name of the app (CFBundleName). The new Window menu has Minimize (Command-M), Zoom, Window Size 50%, 75% and 100% (of the visible area of the screen, with the aspect ratio of the image; not in full screen) and Bring All to Front.
- Change (`make_app.py`, `win32.mk`, `Makefile`): the option `--short-name` (APP_SHORT) sets CFBundleName, the name of the app menu. For this game it is "Call of Cthulhu" (15 characters, the limit of Apple). CFBundleDisplayName stays "Call of Cthulhu: Dark Corners of the Earth".
- Change (`Makefile`): the app icon comes from `goggame-1189711155.ico` of the GOG game folder (256 × 256). The icon of the exe has only 48 × 48. `make_app.py` stops with an error if the icon file does not exist.
- Verified: Info.plist has CFBundleName "Call of Cthulhu"; the icon has a 256 × 256 source image and all the sizes of an iconset.
- Not verified: the menus on the screen. The headless runs have no menu bar, and screenshots are not permitted.

### Headphone audio (HRTF)

- Change (`fmodex.c`; setting **Headphone Audio (HRTF)**, `headphone_audio`; `rt_enh.h` `hrtf`):
  - The output unit now pulls an AUSpatialMixer with 65 inputs. Input 0 is the stereo mix (as before, passed through). With the setting, each 3D channel i plays on its mono input 1 + i, as a point source of the HRTF for headphones (`kSpatializationAlgorithm_UseOutputType`, `kSpatialMixerOutputType_Headphones`, personalized HRTF in the Auto mode).
  - The azimuth and the elevation come from the direction of the source in the listener frame. The attenuation of the distance and the doppler stay those of `fmodex.c`. The mixer gets a fixed distance at its reference distance.
  - The HRTF inputs have a gain of +2.55 dB, so that a source in front keeps the loudness of the balance panning.
  - A setup error stops the audio output with a log message that names the step. A render error gives silence and a log message (System_Update).
- Measured with an offline test of the mixer (no audio device): azimuth +90 degrees is the right ear (left 0.035, right 1.311 × the input energy), -90 degrees is the left ear, a source behind is 0.33 ×, a source in front is 1.113 ×. The HRTF HQ algorithm and the headphone output type gave the same output.
- Verified (headless run, muted output, with temporary measurement code that is now removed): the log shows "Headphone Audio (HRTF) on"; the output had signal (RMS 0.06 to 0.26); up to one 3D channel at a time was on an HRTF input in the first level. Most sounds of that level are 2D.
- Not verified: the sound in headphones (the runs are muted).

### Verification

- `make gen` and `make app` built the app with no new warnings.
- The headless runs ended without errors. The registry and Enhancements.cfg of the test folder were restored after the runs.
- The runner with time stamps (a copy of the headless runner) did not stop the game process at the end of the first run. The process was stopped with `pkill`. The next runs stopped it the same way.

### Retina resolution removed

- The user decided that the game must not render at the Retina resolution. The setting **Retina Resolution (restart)** and its code are removed: `coc_enh_resolution` and the patch 0x40b533 (`enhance.c`, `patches.txt`), the field `retina` (`rt_enh.h`), the display mode of the screen in pixels (`d3d9_metal.m`, `mode_list`), and `rt_cocoa_screen_pixels` and `content_points` (`cocoa32.m`).
- The game again uses its own resolution (the screen in points, 1800 × 1125 on the test Mac). The Enhancements menu has 11 settings.
- Verified: `make gen` and `make app` built the app (only the two known `key_frame` warnings of `wmvcore.c`). A headless run of 30 seconds had no error: the device was 1800 × 1125, and the log showed "menu ENHANCEDOPTIONS registered (11 settings)".

## 2026-11-20: Interface in 4:3, raw mouse input, Game Mode

### Interface in 4:3

- Cause: the game places its interface as parts of the screen width and height. The game was made for 4:3 screens. On the 1800 × 1125 back buffer (16:10), the menus, the text and the pointer were 20% too wide.
- Measured (headless runs with temporary logging, now removed): the game draws the interface with the vertex shader `Font.vsh`. The screen position of a vertex is `v0.xy * postDest.xy + postDest.zw`, with `postDest` in c95 and a SHORT4 position. The pointer is a draw with pretransformed vertices (32 × 32 pixels). Other 2D draws (the copy of the post-processed scene with `ScreenQuadScaled.vsh`, two fixed-function full-screen quads in each frame) do not use these shaders.
- Measured interface draws: full-screen backgrounds of the Options menus (0 to 1), the menu panel (0.28 to 0.72), menu text, subtitle text (from 0.125), and the bands behind the subtitles (0 to 1).
- Change (`d3d9_metal.m`, `rt_enh.h`: `d3d9_enh_ui_vs`; `enhance.c`: `d3d9_enh_ui_vs("Font.vsh", 95)`):
  - The runtime identifies the shaders of `d3d9_enh_ui_vs` by the source file name in their debug information (DBUG).
  - For each draw with such a shader, or with pretransformed vertices, into a back buffer wider than 4:3, the runtime reads the screen x of the vertices. When the vertices do not cover the full width, the viewport and the scissor rectangle of the draw are scaled horizontally by 4:3 / the aspect ratio (0.8333 at 16:10), about the center. The half-pixel offset uses the scaled viewport.
  - A draw that covers the full width (from 0 to 1) keeps the full width: the menu backgrounds, the fades and the bands behind the subtitles.
- The game moves its own pointer with the DirectInput mouse movement (the sum at +0x240 and +0x244 of its mouse object, 0x40caf0). The game does not import `GetCursorPos`. The pointer is drawn in 4:3 with the menus, so the pointer and the menu items stay aligned.
- Verified (headless runs): the log shows "interface in 4:3: ... 0.8333 of the width of the 1800x1125 back buffer". In the Options menu, the two full-screen background draws kept the full width. The panel, the 10 text draws and the pointer were drawn in 4:3. In a level, the subtitle text was drawn in 4:3 and the bands behind it kept the full width. The pause menu was drawn in 4:3. With `MTL_DEBUG_LAYER=1`, the Metal validation layer reported no error in the Options menu.
- Not verified: the image on the screen (screenshots are not permitted). The inventory, the journal and the loading screens were not opened in the test runs. A screen of the game that puts text on a full-screen image keeps the image at the full width and draws the text in 4:3.

### Raw mouse input

- Investigation of the game: the mouse read (0x40caa1) gets the DirectInput movement. The input bindings (0x40d0f0) keep the last value and the current value. The value function (0x40cfe0) returns the current value. Its callers send the value as an action event (0x43a3f0). These steps do not average or filter the movement. The later steps to the camera were not traced. A headless test with an injected movement did not reach player control (the level started with a cut scene), so the response of the camera was not measured.
- Cause on macOS: the runtime gave the game `NSEvent.deltaX` and `deltaY`. These are the movements of the pointer, with the pointer acceleration of macOS.
- Change (`cocoa32.m`; setting **Raw Mouse Input**, `raw_mouse_input`; `rt_enh.h` `raw_mouse`): with the setting, the movement is `kCGEventUnacceleratedPointerMovementX` and `kCGEventUnacceleratedPointerMovementY` of the event (CoreGraphics, `CGEventTypes.h` of the macOS 27.3 SDK). The first movement is written to the log, with the accelerated values. The default is Off. The Enhancements menu has 12 settings.
- Verified: a headless run with `raw_mouse_input=1` showed "raw mouse input on" and "12 settings".
- Not verified: the movement of a real mouse or trackpad with the setting. The headless runs have no mouse events. The unaccelerated values have the units of the device, so the game's mouse sensitivity can need a different value.

### Game Mode

- Change (`make_app.py`): for the category `public.app-category.games`, Info.plist has `LSSupportsGameMode` = true. Apple ([WWDC25 session 209](https://developer.apple.com/videos/play/wwdc2025/209/)) gives this key to let macOS turn on Game Mode for the game. On a Mac, Game Mode is on only in full screen.
- Verified: `plutil -p` of the built Info.plist shows `LSSupportsGameMode => true`.
- Not verified: Game Mode on the screen. The headless runs are not in full screen. The [macOS Tahoe 26 release notes](https://developer.apple.com/go/?id=macos-26-rn) say that Game Mode does not turn on for a binary started directly from Terminal.

### Verification

- `make gen` and `make app` built the app. The only warnings are the two known `key_frame` warnings of `wmvcore.c`.
- The headless runs ended without errors. The Enhancements.cfg of the test folder was restored after the runs.

## 2026-11-20: Launcher window

The app opens a launcher window before the game starts. The window is adapted from the launcher window of the Prototype Mac port (`runtime/loader/rt_launcher.m` of that project). It has the same layout, controls and behavior, with the settings, the game folder check and the look of this game.

### Behavior

- The window shows the game folder and its status, 13 enhancement settings in three groups (Graphics, Sound and mouse, Game), Restore Defaults and Play.
- The HDR calibration (Max Brightness (HDR), Brightness (HDR)) and anisotropic filtering are not in the window. Play keeps their values in `Enhancements.cfg`.
- The folder check (`game_problem` in `game.c`): the folder must contain `Engine/CoCMainWin32.exe` with the SHA-256 of the GOG release. If the player selects the `Engine` folder, the window tells the player to select the folder that contains it.
- The window shows first the folder of the last Play (NSUserDefaults key `GameFolder`). If that folder cannot be used and the folder of the build (`COC_BUILD_GAME_DIR`) can be used, the window shows the folder of the build.
- When the folder can be used, the header shows the icon of the GOG release from that folder (`goggame-1189711155.ico`). The repository and the app contain no game art.
- Play writes `Enhancements.cfg`, remembers the folder and starts the game. A closed window or Quit ends the process with exit status 0.
- The launcher window does not open when the folder is given (the second argument or `COCDCOTE_GAME_DIR`) or with `RT_HEADLESS`.

### Change from the Prototype launcher

The Prototype launcher calls `finishLaunching` only when the game starts. Before that call, AppKit does not answer accessibility requests (`AXUIElementCopyAttributeValue` returned `kAXErrorCannotComplete`, -25204, for the window of this launcher). Thus VoiceOver could not read the window. This launcher calls `finishLaunching` before it shows the window. The quit Apple event (Quit in the Dock) then goes to the handler of AppKit, which ends the process.

### Changed files

- `launcher.m`, `launcher.h` (new): the launcher window.
- `enhance.h` (new), `enhance.c`: the settings table (`coc_enh_keys`) and the read and write functions of `Enhancements.cfg` (`coc_enh_read`, `coc_enh_write`) are shared with the launcher. The game reads and writes the file with the same functions.
- `game.c`: the data folder is set before the game folder; the launcher window gives the game folder.
- `Makefile`: `launcher.m` is in `GAME_SRC`.
- `Recomp-Framework/win32/win32.mk`: `GAME_SRC` can contain Objective-C files (`.m`). They are compiled with ARC.

### Verification

- `make app` built the app with no new warnings. `launcher.m`, `enhance.c` and `game.c` also compiled with `-Wall -Wextra` with no warnings.
- A headless run started the game as before (the settings, the Metal device, the interface in 4:3).
- The launcher window was tested through the accessibility tree (no screen capture), with a test home folder:
  - The window showed the values of the test `Enhancements.cfg`, the game folder with the Ready status, and the GOG icon.
  - Restore Defaults set all 13 menus to the default values.
  - Three menus were changed, then Play. `Enhancements.cfg` had the new values and kept the other values. The game log showed the new values. The Metal device started.
  - With the `Engine` folder as the remembered folder, the window showed the game folder.
- The test preferences and the test `Enhancements.cfg` were restored after the tests.

### Not verified

- The error messages of the folder check for a folder selected with Choose Folder… (the test stopped before this step).
- Quit in the Dock and Command-Q while the window is open.
- The look of the window on the screen (no screen capture).
- VoiceOver with the window.

## 2026-11-20: Crash in memcpy (small copy to an unaligned address)

Problem: the game stopped during play with `[rt] indirect call/jump to unknown address 0x47ce74` in `F_47cd10` (MSVC `memcpy`).

Cause: for a copy of 0 to 3 bytes to an unaligned destination, `memcpy` runs `sub ecx, 4 ; jb 0x47cd64`, and `0x47cd64: jmp dword ptr [ecx*4 + 0x47ce6c]`. Only the borrow reaches the jump, so the index is -4 to -1 and the 4 entries are before `0x47ce6c`. The recompiler read the table forward from `0x47ce6c` and found no entries, so the translated jump had no cases.

Correction: `Recomp-Framework/win32/tools/recomp_w32.py`, new `borrow_index()`. When every branch to the jump is `sub reg, N ; jb`, and no instruction falls through to the jump, the translator reads the N entries before the table address. The new code translated 2 jumps: `0x47cd64` (cases `0x47ce6c`, `0x47ce74`, `0x47ce80`, `0x47ce94`) and `0x47b2d4`, a second copy of the same routine (cases `0x47b3dc`, `0x47b3e4`, `0x47b3f0`, `0x47b404`). No other generated line changed.

Verification: `make gen` and `make app` completed with no warnings. A 45 s headless boot presented 2400 frames with no runtime error. Not verified: the small unaligned copy path itself at run time.

## 2026-11-20: HDR paper white at the SDR white of macOS

Problem (user report): HDR output was far too dark.

Cause: the runtime used the Brightness (HDR) values of the RE Engine as absolute nits. It converted them to EDR units with an estimate of the nits of SDR white (100 nits x potential EDR headroom / current EDR headroom). macOS always shows the desktop with EDR, at the SDR white that the user selects. Measured on the user's screen with the game open: potential EDR headroom 16.000, current EDR headroom 2.667, thus SDR white approximately 600 nits. Paper white was 300 / 600 = 0.50 x the white of the desktop at step 0, and 640 / 600 = 1.07 x at step +10.

Sources: [KoKlusz, Resident Evil 4 (PC) HDR Analysis](https://github.com/KoKlusz/HDR-Gaming-Database/discussions/63) gives Brightness (HDR) as "scene brightness" and compares the HDR default with the SDR image at 300 nits. [hdrgamer, Resident Evil 4 HDR Settings](https://www.hdrgamer.com/2023/03/resident-evil-4-hdr-settings.html) gives it as the midpoint setting. Both are secondary sources, read 2026-11-20.

Correction (`Recomp-Framework/win32/runtime/d3d9_metal.m`, `rt_enh.h`): step 0 of Brightness (HDR) is paper white at the SDR white of macOS (EDR 1.0). Each other step is that white x its nits / 300 (+10: 2.13, -10: 0.17). The UI stays at paper white. With GT7 tone mapping, expo is paper white x the nits of SDR white / 100, so the game's white is again at paper white. Max Brightness (HDR) does not change: the peak is in nits and is converted with the estimate of SDR white, and the test square shows the peak of the screen.

Verification: `make build/CoCDCoTE` compiled and linked (only the two known `key_frame` warnings of `wmvcore.c`). Not verified: a run with the new code. The user's game was open from the app bundle, so the bundle was not rebuilt and no second game was started.

Not changed: the calibration screens. No source found describes the image of the second screen (Brightness) of the RE Engine.

## 2026-11-20: GT7 tone mapping without the toe, bright area under characters

### GT7 tone mapping made the game too dark

Problem (user report): with GT7 tone mapping on, the game was very dark.

Cause: the GT7 curve has a toe (strength 1.280) below its mid point (0.538 frame-buffer units). The reference applies the curve to the linear light of a scene. This game gives an image that its own pipeline already tone mapped, with its own toe. The second toe darkened the shadows again. Calculated with a copy of the shader functions in Python (paper white at the SDR white of macOS, SDR white 600 nits, peak 1600 nits):

| Game gray (sRGB) | Output / input, HDR | Output / input, SDR |
|---|---|---|
| 0.05 | 0.42 | 0.33 |
| 0.10 | 0.56 | 0.43 |
| 0.20 | 0.83 | 0.62 |
| 0.30 | 1.00 | 0.81 |
| 0.50 | 1.00 | 1.00 |

Correction (`Recomp-Framework/win32/runtime/d3d9_metal.m`, `present_tm_consts`): the toe strength is 1.0. With this value the curve is linear below the linear section. Calculated: all the grays of the table give 1.00 in HDR and SDR. A dark color (sRGB 0.2, 0.1, 0.05) gives 1.00 in each channel (before: 0.77, 0.63, 0.55). The shoulder above the linear section does not change (SDR white: 0.835, as before).

### Bright area under characters

Problem (user report, with an image of the town): a bright, flat area with straight edges on the ground under a character.

Measured (headless run from the user's town save, temporary logging, now removed): the game projects the character shadows with the pixel shader `psProjector1_xDiff_Neg.pso` into the back buffer (RGBA16Float, 1800 × 1125), with alpha blending, `SRCBLEND` = ZERO and `DESTBLEND` = SRCCOLOR. Thus the ground is multiplied by the shader color. The shader (ps_1_1, disassembled) calculates `r0.rgb = (1 - shadow) * v0 + 0.5` (`def c0 = 0.5`).

Cause: where the projector has no shadow, the color is 0.5 + v0, which is above 1 when the vertex color v0 is above 0.5. The game's back buffer is 8-bit (X8R8G8B8 or A8R8G8B8). An 8-bit target clamps the shader color to [0, 1] before the blend, so the factor is 1 and the ground does not change. The runtime stores this back buffer as RGBA16Float (highlights above white), and a half-float target does not clamp. Thus the projector area was multiplied by up to 1.5.

Correction (`d3d9_metal.m`, `ps_consts_header` and the pixel shader entry point; `d3d9_internal.h`, comment of `alpha_ref`): for each color output into a target of `fmt_fp_argb` or `fmt_fp_xrgb` (an 8-bit back buffer stored as half float), `alpha_ref.w` has a bit, and the shader output is clamped to [0, 1]. The blend itself stays in half float, so additive blending can still go above white. The fixed-function path already clamps each texture stage. The other 32-bit render targets are RGBA16Unorm and clamp in the hardware.

### Verification

- `make app` built the app (only the two known `key_frame` warnings of `wmvcore.c`).
- Headless run of 90 s from a copy of the user's town save, with `MTL_DEBUG_LAYER=1`: no Metal validation error, no translation error, 4200 frames. The dumped Metal source of all 29 pixel shaders has the clamp.
- Not verified: the image on the screen. Screenshots are not permitted, and the value of v0 under the character was not measured.

## 2026-11-21: Global illumination (RTGI)

Request (user): add RTGI to the game as a setting in the menus. The user selected screen-space ray tracing (as the RTGI shader of ReShade), not hardware ray tracing.

Changes:

- `Recomp-Framework/win32/runtime/rt_enh.h`: the new field `gi` of `RtEnh` (0 off, 1 to 3 low, medium, high). `d3d9_enh_scene` has the new argument `gi`.
- `Recomp-Framework/win32/runtime/d3d9_enhance.m`: the new kernels `gi_main` and `gi_denoise`, and the global illumination in the composite `enh_fs`. The method is in the comment at the top of the file. `to_lin` and `to_srgb` moved above the composite, so that the composite can use them. The bloom uses them as before.
- `Recomp-Framework/win32/runtime/d3d9_metal.m`: the scene passes run when `gi` is on, and a frame with no scene camera gets no global illumination.
- `CoC-Recomp/enhance.c`: the setting **Global Illumination (RTGI)** (Off, Low, Medium, High) in Options > Enhancements, after Raw Mouse Input. The key `global_illumination` in `Enhancements.cfg` (default 0). The menu has 13 settings.
- `CoC-Recomp/launcher.m`: the same setting in the Graphics panel of the launcher window.

The pass needs only the projection of the scene camera, not the camera motion from frame to frame. Thus the errors of the camera motion that the anti-aliasing investigation found do not affect it.

Limits:

- The light comes only from surfaces on the screen. A surface behind the camera, off the screen or behind another surface gives no light.
- The game has no albedo buffer. The albedo comes from the chroma of the pixel color (UNCONFIRMED approximation).
- The ray length (3 m), the thickness (0.3 m) and the strengths (0.5, 1.0, 1.5) are UNCONFIRMED tuning.

Verification:

- `make app` built the app (only the two known `key_frame` warnings of `wmvcore.c`).
- Headless run from a copy of the user's town save, High, with all other enhancements on, `MTL_DEBUG_LAYER=1` and `MTL_SHADER_VALIDATION=1`: 1800 frames, no Metal validation error. The log shows "screen-space global illumination 1800x1125 (computed at 900x563): 4 rays x 20 steps, strength 1.50" and "menu ENHANCEDOPTIONS registered (13 settings)".
- Headless run with Low alone (ambient occlusion and contact shadows off), `MTL_DEBUG_LAYER=1`: 2400 frames, no Metal validation error.
- Frame time (time stamps of the log, 600 frames for each report, 1800 × 1125, all other enhancements on): High 10.3 to 10.8 s (56 to 58 frames per second); off 10.0 s (60 frames per second, the frame pacing limit).
- Not verified: the image on the screen (screenshots and pixel readback are not permitted for this task), the launcher window on the screen, and the menu on the screen.

## 2026-11-21: GT7 tone mapping with the reference toe, and Film Grain

Request (user): GT7 tone mapping did not seem to work, and the user wants proper GT7 tone mapping. Also, the user wants a way to remove the film grain of the game.

Cause of the GT7 problem: the curve had a toe strength of 1.0, not the 1.280 of the reference. With toe 1.0, the curve has no effect below the linear section (lin 0.444 x the peak). In the town at night, the maximum scene value before the game's post-processing was 0.271 (no pixel above 1.0). Thus the curve did not change the image.

Changes:

- `Recomp-Framework/win32/runtime/d3d9_metal.m`: the GT7 toe strength is 1.280, as in the reference. The comment gives the calculated effect: the gray sRGB 0.1 of the game keeps 43% of its light in SDR, and 72% in HDR with the settings of the user (paper white 2.13 x SDR white, SDR white 600 nits, peak 1600 nits).
- `Recomp-Framework/win32/runtime/d3d9_metal.m`: the new function `d3d9_enh_grain` sets the grain texture of the game (size, format, stage and color operation). When `grain` is 1, the fixed-function pixel key of a draw with that texture at that stage and that color operation stops at that stage. The draw then copies the image without the noise.
- `Recomp-Framework/win32/runtime/rt_enh.h`: the new field `grain` of `RtEnh` (0 the game's film grain, 1 off), and the declaration of `d3d9_enh_grain`.
- `CoC-Recomp/enhance.c`: `coc_enh_init` calls `d3d9_enh_grain(64, 64, D3DFMT_L8, 1, D3DTOP_BLENDFACTORALPHA)`. The setting **Film Grain** (Game, Off) in Options > Enhancements, after Sanity Effects. The key `film_grain` in `Enhancements.cfg` (default 0). The menu has 14 settings.
- `CoC-Recomp/launcher.m`: the same setting in the Game panel of the launcher window.

The grain pass (measured from the draws of the game): two full-screen draws into an 1800 x 1125 render target. Stage 0 is the image. Stage 1 is a 64 x 64 `D3DFMT_L8` noise texture with `D3DTOP_BLENDFACTORALPHA` (texture factor alpha 0x0a and 0x30).

Verification:

- `make app` built the app (only the two known `key_frame` warnings of `wmvcore.c`).
- Headless run of 60 s with `film_grain=1` and `MTL_DEBUG_LAYER=1`: 3000 frames, no Metal validation error. The log shows "film grain off: frame 932, a draw with the grain texture (64x64, format 50) at stage 1 has stages 0 to 0 only" and "menu ENHANCEDOPTIONS registered (14 settings)".
- Not verified: the image on the screen (screenshots are not permitted), and the GT7 curve on the screen. The effect of the toe is calculated, not measured.
