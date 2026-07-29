# Final Fantasy VI Vita

This is a wrapper/port of *Final Fantasy VI Android* for the *PS Vita*.

The port works by loading the official Android ARMv7 executable in memory,
resolving its imports with native functions and patching it in order to
properly run.

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

`libFF6.so` ships with its symbol table intact (~15.000 exported symbols), so
instead of emulating the looper, the input queue and the activity lifecycle,
the loader drives the engine directly:

```c
initMainFrame();
File_SetAAssetManager(&fake_activity);
g_app->app = &fake_android_app;
initGame();                 /* spawns thread_main on its own */
```

From there everything below `initGame()` -- EGL, GL, OpenSL ES, the AGB main
loop -- runs exactly as it does on Android. The Vita thread is left with the
same job the glue's main thread had: feeding input. It does that by
synthesizing `AInputEvent` objects (the loader owns the `AInputEvent_*`
accessors, so the struct layout is its own) and handing them to the engine's
own `InputDriverAndroid_onInputEvent()` and `inputMotion()`. Auto-repeat, the
virtual pad and the in-game button config therefore keep working untouched.

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

- Install [kubridge](https://github.com/TheOfficialFloW/kubridge/releases/) and
  [FdFix](https://github.com/TheOfficialFloW/FdFix/releases/) by copying
  `kubridge.skprx` and `fd_fix.skprx` to your taiHEN plugins folder (usually
  `ux0:tai`) and adding two entries to your `config.txt` under `*KERNEL`:

```
  *KERNEL
  ux0:tai/kubridge.skprx
  ux0:tai/fd_fix.skprx
```

**Note:** Don't install `fd_fix.skprx` if you're using the repatch plugin.

- **Optional**: Install [PSVshell](https://github.com/Electry/PSVshell/releases)
  to overclock your device.
- Install `libshacccg.suprx`, if you don't have it already, by following
  [this guide](https://samilops2.gitbook.io/vita-troubleshooting-guide/shader-compiler/extract-libshacccg.suprx).
- Obtain your copy of *Final Fantasy VI* legally from the Google Play store in
  the form of an `.apk` file and one or more `.obb` files (usually located
  inside `/sdcard/Android/obb/com.square_enix.android_googleplay.FFVI/`).
- Copy the `.obb` file to `ux0:data/ff6` and rename it to `main.obb`.
- Open the apk and extract `libFF6.so` from the `lib/armeabi-v7a` folder to
  `ux0:data/ff6`.
- Install `FF6.vpk` on your *PS Vita*.

The final layout should be:

```
ux0:data/ff6/libFF6.so
ux0:data/ff6/main.obb
ux0:data/ff6/options.cfg   (optional)
```

### options.cfg

An optional `key=value` file in `ux0:data/ff6`:

| key            | values                                                       |
| -------------- | ------------------------------------------------------------ |
| `bilinear`     | `0` nearest (default), `1` linear, `2` crisp + filtered upscale |
| `integerscale` | `0` fill the 3:2 box (default), `1` snap to a whole multiple  |
| `language`     | `0` follow the system, otherwise `getSystemLanguage()` + 1    |
| `antialiasing` | `0` off, `1` MSAA 2x, `2` MSAA 4x (default)                   |
| `aspect`       | `0` pillarboxed 3:2 (default), `1` stretch to fullscreen      |

`aspect=0` reproduces the engine's own non-tablet path, which picks the largest
3:2 box that fits the screen -- 816x544 on the Vita, matching the GBA's 240x160
aspect. `aspect=1` patches `DeviceIsGen5`/`SysConfigIsWide` so the engine
renders at the full 960x544 instead.

#### Filtering

The engine composes a 480x320 world and the port scales it to the 816x544 box,
a 1.7x factor. The Android build asks for `GL_LINEAR` on every texture, which
blurs the art before it is ever scaled; sampling everything `GL_NEAREST`
instead keeps it sharp but makes a source pixel cover one or two screen pixels
depending on where it lands, which shimmers as the camera moves.

`bilinear=2` avoids both: the world is drawn into an offscreen buffer at twice
its own size with `GL_NEAREST`, then scaled to the box once with `GL_LINEAR`.
Sampling stays exact and only the final step is filtered. The side panels are
drawn separately in screen space, so they stay pixel-exact either way.

`integerscale=1` is the other way to get an even pixel grid: it shrinks the
game area to the largest whole multiple of 480x320 that fits. On a 544-tall
panel that is only 1x, i.e. a 480x320 image using half the screen, so
`bilinear=2` is usually the better trade.

## Build Instructions (For Developers)

You'll need a [vitasdk](https://github.com/vitasdk) build, plus:

- [libmathneon](https://github.com/Rinnegatamante/math-neon) -- `make install`
- [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK) -- `make install`
- [kubridge](https://github.com/TheOfficialFloW/kubridge) --
  `mkdir build && cd build && cmake .. && make install`
- [vitaGL](https://github.com/Rinnegatamante/vitaGL) --
  `make NO_DEBUG=1 install`
- [opensles](https://github.com/frangarcj/opensles) --
  `cd libopensles && make install`

Then:

```bash
mkdir build && cd build
cmake .. && make
```

LiveArea artwork is optional: drop `icon0.png`, `livearea/contents/bg0.png`,
`livearea/contents/startup.png` and `livearea/contents/template.xml` into
`sce_sys/` and they are picked up automatically by the VPK rule.

## Status

The loader is complete and builds, but **it has not been verified on hardware
yet**. All 219 imports of `libFF6.so` are resolved, the bootstrap path and the
JNI contract were derived from the disassembly and from the decompiled
`FF6.java`, but nothing here has been run on a real Vita.

Things most likely to need attention first:

- `glCompressedTexImage2D` is reached from `ResumeTexture` with a format that
  comes from the data; if it turns out to be ETC1, vitaGL needs to handle it or
  the textures have to be converted.
- vitaGL is initialised on the loader thread while the game does its GL from
  `thread_main`. That mirrors Android's threading, but it is the first thing to
  suspect if rendering misbehaves.
- Google Play services (achievements, cloud saves, campaigns) are stubbed to
  fail rather than removed.

## Credits

- TheFloW for the initial arm elf loader.
- Rinnegatamante for vitaGL and fixes.
