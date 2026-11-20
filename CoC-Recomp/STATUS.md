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
