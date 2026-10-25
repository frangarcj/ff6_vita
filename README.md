# Final Fantasy VI Vita

This is a wrapper/port of *Final Fantasy VI Android* for the *PS Vita*.

The port works by loading the official Android ARMv7 executable in memory,
resolving its imports with native functions and patching it in order to
properly run.

## Current status

The project is being prepared as **1.0.0-rc1**. Fixes for undersized text,
glyphs discarded at bitmap edges, glyph-atlas upload limits and atlas sampling
are now implemented and covered by host regressions. The updated release build's
resume-confirmation menu has been inspected in an isolated emulator run:
text was readable with no missing letters or overlap observed in that menu.
Broader dialogue, menu, battle and real-hardware checks remain pending; the
candidate is not finalized for release.

Historical July 2026 testing on real Vita hardware confirmed an improvement
to Narshe scrolling after enabling in-place texture updates. The subsequent
menu/battle graphics fix still needs a fresh hardware check. A successful
build alone does not establish that the candidate is ready for release.

Normal-save and quicksave round trips also need verification. The code
contains fixes for Android file-open flags, save paths and reading all save
slots; older investigation notes that call saving broken predate or conflict
with those changes. No completed save-persistence test is recorded yet.

See [the release checklist](docs/RELEASE_CHECKLIST.md) for the acceptance gates,
[the 2026-10-25 validation report](docs/VALIDATION_2026-10-25.md) for the bounded
build, host-test and emulator results, and [HUNT.md](HUNT.md) for the historical
investigation. Known limitations
include thin seams between tiles and unavailable Google Play services such as
cloud saves and achievements. Offscreen rendering and native fixed-point
colors are experimental options and are disabled by default.

## How it works

Unlike [ff3_vita](https://github.com/frangarcj/ff3_vita), whose `.so` exposes a
tiny custom `render()`/`touch()` API, `libFF6.so` is a stock Android
**NativeActivity** built on `android_native_app_glue`:

```
ANativeActivity_onCreate
  -> android_main            pumps an ALooper; no drawing happens here
       -> engine_handle_cmd  APP_CMD_INIT_WINDOW -> initGame()
            -> initGame      SetupObbFile, sound, save data, ... then thread_init()
                 -> thread_main   <- the real game thread
                      initDisplay()   EGL + GLES1 setup
                      AgbMain()       the GBA-style main loop
```

`libFF6.so` ships with its symbol table intact (~15,000 exported symbols), so
instead of emulating the looper, the input queue and the activity lifecycle,
the loader drives the engine directly:

```c
initMainFrame();
File_SetAAssetManager(&fake_activity);
g_app->app = &fake_android_app;
initGame();                 /* spawns thread_main on its own */
```

From there the game's own thread and AGB main loop run against the loader's
EGL, GL and OpenSL ES implementations. The Vita input thread has the same job
the glue's main thread had: feeding input. It does that by
synthesizing `AInputEvent` objects (the loader owns the `AInputEvent_*`
accessors, so the struct layout is its own) and handing them to the engine's
own `InputDriverAndroid_onInputEvent()` and `inputMotion()`. The original
engine retains its auto-repeat, virtual-pad and button-configuration logic.

### Notes about the game itself

FF6 Android is a port of **Final Fantasy VI Advance** (Game Boy Advance, 2006),
not of the SNES version. The original AGB code is still in there --  `AgbMain`,
`int_VBlankFunc`, the `m4a`/MusicPlayer2000 sound driver, software
reimplementations of the GBA BIOS calls (`CpuSet`, `LZ77UnCompVram`, ...) and
the PPU state kept in plain RAM (`AgbIo` is 0x400 bytes, `AgbExRam` is the GBA's
256 KB of EWRAM). What was replaced is the hardware layer: the PPU became an
OpenGL renderer and the cartridge became the `.obb`.

Two consequences matter for this port:

- The loop is a **60 Hz GBA loop** with no delta-time, so vsync has to be
  respected.
- Battles switch to a separate, **depth-buffered** renderer
  (`BattleHighReso_*`). It is the only path that uses the depth buffer, so
  vitaGL must be brought up with one -- otherwise the field looks fine and
  combats render wrong.

## Setup Instructions (For End Users)

Use a legally obtained copy of the original Android game containing
`lib/armeabi-v7a/libFF6.so` and its matching OBB data. The VPK supplies the
loader and fonts; it does not supply the commercial game executable or data.
The reference game is Android version **2.1.6** (`versionCode=216`), package
`com.square_enix.android_googleplay.FFVI`. Compatibility with other revisions
has not been established. The matching OBB is named
`main.21701.com.square_enix.android_googleplay.FFVI.obb` before renaming; its
number does not match the APK's versionCode, so verify the actual files.

| File | Size in bytes | SHA-256 |
| --- | ---: | --- |
| `libFF6.so` | 22,340,224 | `f612f711330238cf405a403fb084abd2b8d7c9e72f9867740aed2b834fc731e6` |
| `main.obb` | 654,927,616 | `4f443b8d0be0a7976f8dd540cccb24a643937afd6eec043e7bf7d13fe427369f` |

Check your extracted files on the build computer without modifying them:

```bash
python3 tools/verify_game_data.py --so "/path/to/libFF6.so" --obb "/path/to/main.obb"
```

The loader checks the supported executable's size and CRC32 before parsing
it. The helper above checks SHA-256 for both the executable and the OBB.

1. Install [kubridge](https://github.com/TheOfficialFloW/kubridge/releases/)
   and [FdFix](https://github.com/TheOfficialFloW/FdFix/releases/) in your
   active taiHEN plugin configuration. For example, if your plugins are in
   `ux0:tai`, add the following under `*KERNEL`:

   ```
   *KERNEL
   ux0:tai/kubridge.skprx
   ux0:tai/fd_fix.skprx
   ```

   Use the paths for your own active plugin directory. Do not add FdFix if
   you already use rePatch, and do not duplicate existing plugin entries.

2. Install `libshacccg.suprx` if needed; the loader checks
   `ur0:data/libshacccg.suprx` and `ur0:data/external/libshacccg.suprx`.
   The [shader compiler setup guide](https://samilops2.gitbook.io/vita-troubleshooting-guide/shader-compiler/extract-libshacccg.suprx)
   describes extraction.
3. Create `ux0:data/ff6`. Copy the matching Android OBB there as `main.obb`.
   Its Android source is normally under
   `/sdcard/Android/obb/com.square_enix.android_googleplay.FFVI/`.
4. Open your APK as an archive and copy `lib/armeabi-v7a/libFF6.so` to
   `ux0:data/ff6/libFF6.so`.
5. Install `FF6.vpk`. Keep a backup of any existing `RockDat*.sav` files before
   testing a candidate or replacing an earlier build.

The final layout should be:

```
ux0:data/ff6/libFF6.so
ux0:data/ff6/main.obb
ux0:data/ff6/options.cfg   (optional)
```

Save data is stored alongside these files as `RockDat*.sav`. Replacing the
VPK does not require replacing or deleting that directory. The loader
requests 444 MHz CPU, 222 MHz bus/GPU and 166 MHz GPU crossbar clocks at startup.

The D-pad and left stick provide directional input. Face buttons, shoulder
buttons, Start and Select are forwarded as Android gamepad buttons, and the
front touchscreen supplies touch input. Use the game's own button settings
for its action mapping.

### options.cfg

Create this optional file in `ux0:data/ff6`. Use one `key=value` entry per line
without spaces around `=` and restart the game after changing it. Omitting
the file uses these defaults:

| Key | Default | Accepted settings |
| --- | --- | --- |
| `bilinear` | `0` | `0` nearest for non-atlas textures, `1` linear; registered glyph atlases always use linear sampling |
| `integerscale` | `0` | `0` fill the normal 3:2 game area, `1` use a whole-number scale |
| `language` | `0` | `0` system language; explicit values below |
| `antialiasing` | `2` | `0` off, `1` MSAA 2x, `2` MSAA 4x |
| `sound` | `1` | `0` disable audio, `1` enable audio |
| `fontscale` | `100` | Positive percentage of the EM text size requested by the Android engine; semantics changed in this candidate |
| `inplacetex` | `1` | `1` update atlas textures in place, `0` use vitaGL's normal texture-update path |

Language overrides are `1` Japanese, `2` English, `4` German, `5` French,
`6` Italian, `7` Spanish, `8` Simplified Chinese, `9` Traditional Chinese,
`10` Korean, `11` Thai, `12` Russian and `13` Brazilian Portuguese. These are
the loader's mappings; text rendering in every language is not yet verified.
The bundled fonts do not cover the tested Thai characters, so `language=11`
does not provide complete Thai text rendering. Complex combining-text
fallback and shaping are not guaranteed.

For example, to keep the default rendering and select Spanish:

```ini
language=7
```

The engine uses its normal 3:2 game area: 816x544 within the Vita's 960x544
display. There is no supported `aspect` option. `bilinear=2` is also not a
supported filtering mode.

#### Filtering

The engine composes a 480x320 world and the port scales it to the 816x544 box,
a 1.7x factor. With `bilinear=0`, ordinary art textures use `GL_NEAREST`:
source pixels can cover one or two screen pixels depending on where they
land, which can shimmer as the camera moves. `bilinear=1` uses `GL_LINEAR`
and smooths those textures, including the pixel art.

The two registered glyph-atlas pages use `GL_LINEAR` in either mode so
rasterized font coverage is not forced through nearest sampling. The loader
tracks those atlas handles and forgets them on deletion or page replacement;
an unrelated texture reusing a deleted handle follows the normal setting.
This exception is specific to the glyph atlases. The separate full-string
`createTextTexture`/`drawText00` path is not classified as an atlas and still
follows the general `bilinear` override.

`integerscale=1` is the other way to get an even pixel grid: it shrinks the
game area to the largest whole multiple of 480x320 that fits. On a 544-tall
panel that is only 1x, giving a centered 480x320 game area. Thin tile seams
remain a known visual issue; a filtering option is not a verified fix for it.

Sharp-bilinear scaling is not implemented. The experimental `offscreen=1`
path uses ordinary `GL_LINEAR` on its final quad. A future sharp-bilinear
pass could be placed at the final 1.7x world upscale; it cannot reconstruct
correct texels if neighboring atlas cells have already bled into the
composited image. `offscreen` remains experimental and defaults to `0`.

#### Font size and existing configurations

`fontscale=100` now uses the EM size requested by the Android engine
(`stbtt_ScaleForMappingEmToPixels`). The previous implementation made the
font's entire ascent/descent box fit that size, which reduced the glyphs.
The current vertical box is the separately rounded scaled ascent plus the
magnitude of the descent of the bundled Noto font; it is not a claim to
reproduce Android's default font's exact top/bottom metrics or layout.

Existing `fontscale` overrides are not converted automatically. Start with
`100` and reassess a value chosen for the older build. For the bundled JP
font, ascent `1160`, descent `-288` and `1000` units per EM give an unclamped
scale ratio of `1.448` between the new and old meanings of `100`. A value of
about `69` now approximates that font's previous `100` scale; this is a
font-metric comparison, not a verified Android screenshot match.

#### Experimental options

| Key | Default | Purpose and limitation |
| --- | --- | --- |
| `offscreen` | `0` | `1` draws the world into an intermediate buffer. Hardware correctness and performance remain unverified. |
| `fixedcolor` | `0` | `1` uses native `GL_FIXED` colors in the patched vitaGL shader path. Requires that patch and hardware validation. |

Keep both at `0` for the baseline candidate. The default `fixedcolor=0`
converts fixed-point colors in the loader. `inplacetex=0` is useful for
diagnosing texture corruption, but can restore the severe Narshe slowdown.

## Build Instructions (For Developers)

Set `VITASDK` to your [VitaSDK](https://github.com/vitasdk) installation, then
install the libraries used by `CMakeLists.txt`, including:

- [libmathneon](https://github.com/Rinnegatamante/math-neon) -- `make install`
- [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK) -- `make install`
- [kubridge](https://github.com/TheOfficialFloW/kubridge) --
  `mkdir build && cd build && cmake .. && make install`
- A patched [vitaGL](https://github.com/Rinnegatamante/vitaGL) providing
  `vglInPlaceTexSubImage`. Follow [deps/README.md](deps/README.md) to build
  upstream commit `c707ff00d9a372f0ba7056b4a4b723f5a00ba880` with the bundled
  FF6 patch. Its required release flags are `NO_DEBUG=1 NO_SPLASHSCREEN=1
  SHARED_RENDERTARGETS=1`. The patch also provides native `GL_FIXED` color
  support for the optional `fixedcolor=1` path.
- [opensles](https://github.com/frangarcj/opensles) --
  `cd libopensles && make install`

After preparing that project-local dependency, build the candidate with
instrumentation disabled:

```bash
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release -DFF6_DEBUG=OFF \
  -DVITAGL_INCLUDE_DIR="$PWD/build_release/deps/vitaGL/source" \
  -DVITAGL_LIBRARY="$PWD/build_release/deps/vitaGL/libvitaGL.a"
cmake --build build_release --parallel 4
```

The two `VITAGL_*` paths select the project-local build without replacing the
SDK's shared library installation. CMake checks both the header declaration
and the archive symbol for `vglInPlaceTexSubImage` before building. The pinned
source/patch recipe has been built with VitaSDK GCC 15.2.0; it does not promise
identical output bytes across different SDK installations or source paths.

The outputs include `build_release/FF6.vpk` and the matching
`build_release/FF6.elf`. Retain the ELF to investigate crash dumps from that
exact build. Validate and stage the package with:

```bash
python3 tools/check_release.py build_release --output release
```

This checks the release build settings, VPK contents and metadata, and stages
`FF6.vpk`, `FF6.elf`, `manifest.json` and `SHA256SUMS` in `release/`. Package
validation does not mark the hardware tests complete. Record the tested
artifact identity in the release checklist.

Debug builds use a separate directory and `-DFF6_DEBUG=ON`; they enable
`boot.log`, GL tracing, trigger files and other investigation tools. They also
require an instrumented vitaGL archive exporting `vgl_debug_log_draws` and
`vgl_debug_scene_splits`; CMake checks for these when debug mode is selected.

LiveArea artwork is optional: drop `icon0.png`, `livearea/contents/bg0.png`,
`livearea/contents/startup.png` and `livearea/contents/template.xml` into
`sce_sys/` and they are picked up automatically by the VPK rule.
Their absence does not invalidate the VPK. Review the presentation separately
from the runtime acceptance tests.

## Credits

- TheFloW for the initial arm elf loader.
- Rinnegatamante for vitaGL and fixes.

See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for
the project license and bundled third-party notices, including the Noto fonts.
