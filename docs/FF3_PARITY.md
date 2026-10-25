# FF3 feature/workflow parity for the first public FF6 RC

Reference: `frangarcj/ff3_vita` commit
`f08d74fb29b149134465f212257766ce86de6878` (v1.2.5-era checkout).
This is a compatibility matrix, not a claim that two different engines render
or behave identically. The first public FF6 tag is **v1.0.0-rc1**; local
RC1/RC2 validation reports predate that public version numbering.

| Reference feature | FF6 implementation | Important difference / validation boundary |
| --- | --- | --- |
| LiveArea configuration companion | Packaged `companion.bin`, Configure target and original LiveArea assets | Launches independently of game data; shares the parser/defaults with the loader |
| Save/launch/discard controls | Configurator buttons and START for save-and-launch | Temporary-file save preserves existing config on failure and retains unknown keys/comments |
| Resolution selection | 960x544, 1280x725, 1920x1088, with VitaGL native fallback | Higher modes are experimental and require suitable output support; 480x320 world composition is unchanged |
| Texture filtering / MSAA | Existing nearest/bilinear and 0/2x/4x MSAA options exposed in UI | Glyph atlases retain their separate linear-sampling exception |
| Language selection | Explicit FF6 language values exposed, including system default | FF6 has different numeric IDs and additional languages; Thai font coverage remains incomplete |
| Cross/Circle swap | Both press and release events swap confirm/cancel | Does not remap the Configurator's own navigation or logical debug autopilot events |
| Aspect ratios | Native 3:2 plus 4:3, 16:9, 2:1 and 21:9 presentation choices | FF6 is a fixed 2D composition, not FF3's adjustable 3D camera. The view stretches within the original game region and retains side panels. No extra scene content is revealed |
| PostFX | Negative, FXAA, Sepia, Greyscale, CRT, CRT-Bleeding, CRT-Bleeding-Flat | Separate full-screen pass after world/UI composition, before system dialogs; FXAA uses the selected output size rather than fixed 960x544 |
| Automatic builds | Push/PR/manual/dispatch CI with verified VPK and symbols | Adds host sanitizers, package tests and pinned source dependencies; no Android data is needed |
| Tag releases | Version-tag workflow publishes via `gh` | RC tags are prereleases; existing releases are never overwritten; checksums and both matching ELFs are included |
| Screenshots and application presentation | Original generated indexed PNGs and documented setup | No extracted commercial data is packaged |

## Features retained specifically for FF6

Sharp-bilinear world upscaling, integer scaling, in-place atlas updates,
fixed-point colour conversion, Android ABI/file compatibility, glyph-cell
clearing, complete edge clipping, independent font baseline alignment and
screen-to-FBO scissor mapping remain in place. Default rendering is not
silently replaced by PostFX or a new aspect ratio. The Configurator also exposes
FF6's sound switch and advanced options.

Native touch coordinates are inverse-mapped when presentation geometry changes.
The published layout is coherent across the render and input threads; touch
points in the newly blank part of the old game region are ignored rather than
activating invisible menu controls. Integer scaling takes precedence over
aspect selection. It does not shrink the screen-space clock/battery panels.

## Deliberately not copied blindly

FF3's optional extracted `opening.mp4` playback is a game-specific asset/flow;
no FF3 movie, filenames or playback hooks are added to FF6. FF3's ICU resource
bundle and Spanish/Korean character-name patches address that executable's
encoding and script conventions; FF6 retains its tested encoding/font helpers
rather than installing those binary hooks at unrelated addresses.

The Configurator needs vitaGL's **ENABLE_LEGACY_PIPELINE=1**, because the ImGui
backend calls `vglDrawObjects`. A linkable symbol alone is insufficient: without
that flag the draw entrypoint is a no-op. The flag is included in the pinned
build recipe for both local and automated builds.

## Verification and remaining acceptance

`tools/run_host_tests.py` exercises nine sanitizer-enabled C suites, the real
frame-presentation entrypoint in both build profiles, and source bundle assets.
`tools/check_release.py` checks the Release profile, matching game and companion
executables, every shader pair, resources, licenses and provenance. Neither
script substitutes for actual game/hardware testing.

The release remains a **prerelease**. A full-language/layout sweep, higher
resolution performance, PostFX performance on Vita/PSTV, first/repeated battle
effects, suspend/resume, sustained play, and normal/quicksave round trips after
a full restart remain explicit acceptance tasks. Prior emulator observations
must not be described as hardware validation. The existing full-string text
path still follows the general texture-filter setting.
