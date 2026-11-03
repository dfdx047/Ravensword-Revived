# Changelog

## 1.0.0 — 2026-11-03

First public release. A patcher for your own copy of Ravensword: Shadowlands 1.26 (SHA-256
`bbdbab91…af71a9`). Tested only on the AYN Odin 3, Android 15, arm64-v8a.

### Compatibility
- Runs the 32-bit game on 64-bit-only Android 15: a new arm64 `libunity.so` loads the game's original
  ARM32 code and runs it through the dynarmic JIT, with libc, pthread, OpenGL ES and JNI as thunks.
- `targetSdkVersion` 28 and `minSdkVersion` 24, so Android 15 installs it without the old-app warning.
- Google Play billing (IAB v3) and the MOGA controller library are turned off; both crashed on
  current Android.
- The patcher checks the APK's SHA-256, never downloads anything from the game, and signs the result
  with a key created on the user's computer.

### Graphics
- All Unity quality levels set to their maximum; 32-bit color surface and 24-bit depth instead of a
  16-bit surface that showed banding.
- HDR scene with MSAA (up to 8x), bloom, ambient occlusion, sun rays, sharpening and vignette. The color
  grade keeps mid-tones where the artists left them and only compresses what goes past white.
- Projected shadows from 256 px up to 4096 px, casters refreshed 4 times per second, sun tilted to at
  most 55° so shadows no longer hide under the characters; rocks and buildings now receive them.
- Sun shadows turned off in scenes with baked lighting, where they drew dark stripes on walls.
- Resolution from 360p to 2160p, with menus always at native resolution.

### Performance
- Steady 60 FPS: frames are paced with `eglPresentationTimeANDROID` and the frame budget is reported
  through `APerformanceHint`, so the CPU no longer clocks down while the game waits between frames.
- Measured on the Odin 3 in South Aven on Ultra: 60.0 FPS standing and 59.7 FPS walking, main core at
  4.32 GHz, 76–99 °C over 4 minutes with no throttling. Before, Ultra ran at 56–58 FPS.

### Interface
- "Graphics" and "Controls & Language" pages inside the game's own Options menu; changes apply live.
- Five quality presets (Low, Medium, High, Very High, Ultra), an FPS limit, resolution and an Advanced
  page. Changing anything in Advanced switches the preset to Custom.
- Effects in 11 steps (off to 100%): bloom, ambient occlusion, sun shadows, sun rays, film color,
  contrast, saturation, sharpening, vignette. Exposure from 50% to 150%.
- Also adjustable: shadow range and size, textures, draw distance, fog, vegetation, field of view,
  fullscreen, dead zone, touch buttons and language.

### Controls
- Touch and gamepad work at the same time; the last one used decides whether touch buttons show.
- 157 gamepad bindings taken from the game's controller scene: sticks, RT attack, LT block, RB magic,
  LB compass, A use, Y jump, B inventory, L3 stealth, R3 camera, Start menu.
- Y now jumps and A now uses doors, chests, items and conversations; both only reacted to touch before.
- A gold frame moves between everything on screen with the D-pad or stick, in menus, dialogs and
  character creation. A confirms and left/right adjust sliders. LB/RB switch pages, B goes back.

### Translation
- Brazilian Portuguese for 1,797 lines: dialogue, quests, items and menus.
- The game's font had no accented letters; the patcher builds them from the font's own pieces.
- The repository stores the translation keyed by a hash of each English line. The patcher rebuilds
  the full dictionary from the user's own APK, so none of the game's text is distributed.
- Lines still missing a translation are written to the log with the `[ravenlang]` prefix.

### Known issues
- **Rare hang at start with `Inflate Error`.** It happened in about 1 of 4 launches while the mod called
  JNI in a loop (every 0.3 s). The loop was removed: 31 launches and about 15 minutes of play without
  a failure since. The root cause was not found; the real zlib error now goes to the log
  (`RavenBridge` tag).
- **The in-game shop for paid items is unavailable**, because Google Play billing is turned off.
- **Areas of the full game stay locked**: the full version was sold through the OUYA store, which no
  longer exists.
- **Only the AYN Odin 3 has been tested.** Other devices may fail to install or start.
- Changing the shadow type (None, Simple, Projected) applies on entering another area, and changing the
  language needs a restart to reach every text.
