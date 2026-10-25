# RC2 follow-up: text alignment, upscale and diagnostic cleanup

Working label: **1.0.0-rc2**, 2026-10-25. This is a local candidate, not a
stable-release or publication signoff. The staged RC1 remains in
`release/2026-10-25-rc1/`. No commit or push is implied by candidate staging.
The starting source revision was `4f08d34`; the additional working-tree
changes, not that revision alone, are required to reproduce this build.

## Changes carried into this candidate

- Reused glyph-atlas cells are replaced completely with transparent padding,
  preventing the previous occupant's pixels from surviving around a smaller
  glyph. See [the atlas follow-up](MENU_ATLAS_VALIDATION_2026-10-25.md).
- `sharpbilinear=1` selects a real final-pass Cg shader. For the observed
  480x320 source and 816x544 destination its remapping is equivalent to 2x
  nearest prescaling followed by bilinear resizing. The compositor uses a
  separate programmable draw and restores the state it changes.
- Battle-menu scissor rectangles are converted from display coordinates to
  the offscreen buffer and restored for display overlays. This fixes the
  missing/cut-off battle options seen with the earlier offscreen variant.
- The font's visual baseline is separated from its conservative atlas/layout
  height, correcting labels that sat below their accompanying icons.
- The diagnostic 15-second freeze, obsolete overlay/replay code and automatic
  pixel-readback probes are removed. Release excludes diagnostic files and
  test triggers; the optional Debug build remains available for investigation.

## Vertical alignment: contract and evidence

The locally inspected Android `FF6.java` uses `abs(fontmet.ascent)` as the draw
baseline, but reports a height based on `abs(top) + abs(bottom)`. These are
separate concepts. This does not establish that the bundled Noto font shares
Android's default font metrics.

The Vita implementation previously used its conservatively rounded Noto
ascent both for allocation/layout and for the raster baseline. The adjustment
keeps the same scale, advances and reported ascent/descent box. It centers a
fixed capital `H` reference inside that box and uses the resulting baseline
for all text at that font/size. It does not recenter each word or strip accents
and descenders. This is a visual alignment policy for the substituted font,
not an exact reproduction of Android's system font.

For each of the bundled JP, SC and KR fonts, the measured values are:

| Requested size | Reported bitmap height | Previous baseline | New baseline |
| --- | --- | --- | --- |
| 19 | 29 | 23 | 21 |
| 28 | 42 | 33 | 31 |

`tests/test_font_alignment.c` checks sizes 12, 16, 19, 24, 28, 32 and 70;
complete ASCII ink; sampled Spanish accents, Cyrillic and CJK characters;
fontscale values 69, 100, 125 and 200; maximum-size clamping; and invalid
height. Complete raster bytes are compared with independently generated stb
bitmaps, including padded rows and allocation guards. These are sampled
coverage tests, not acceptance of every supported language or combining form.

The corrected Debug build ran in
`build_release/sharp-bilinear-smoke-pq6fikee/` with:

```ini
bilinear=0
sharpbilinear=1
offscreen=1
fontscale=100
inplacetex=1
fixedcolor=0
```

Its `upscale-status.txt` confirmed a linked sharp-bilinear shader, 480x320 to
816x544, prescale 2x2. New capture `Final Fantasy VI_2026-10-25-210052.png`
shows Fire Beam, Thunder Beam, Ice Beam, Healing Force, and the battle action
panels. It was compared with pre-alignment capture
`Final Fantasy VI_2026-10-25-204135.png` in
`build_release/sharp-bilinear-smoke-vjnc7xs1/`.

The comparison uses the same right-hand action panel, not the animated battle
background. A neutral-bright-pixel mask (all RGB channels at least 180 and
channel spread at most 45; no OCR) gives:

| Region | Before Y bounds, exclusive bottom | After Y bounds | Observation |
| --- | --- | --- | --- |
| Magitek text | 440..455 | 439..454 | Visible text shifts up 1 display pixel |
| Items text | 495..506 | 493..505 | Visible edges shift up 1-2 display pixels |
| Magitek icon | 434..455 | 434..455 | Unchanged bounds and mask pixel count |
| Items icon | 486..510 | 486..510 | Unchanged bounds and mask pixel count |

The fractional game scale/filter explains why a two-pixel bitmap adjustment
need not produce two whole display pixels at every visible edge. These are
threshold measurements of these captures, not a universal pixel displacement.
The comparison image and numbers are `alignment-before-after.png` and `.json`
in the new run directory. The bounded comparison script is
`build_host_tests/compare_alignment.py` and uses only the Python standard
library. The user accepted moving on to diagnostic cleanup after this check.

## Diagnostic cleanup and Fire Beam

The prior Debug run's log at
`build_release/sharp-bilinear-smoke-x03pzlkw/portable/fs/ux0/data/ff6/boot.log`
contains a deliberate `holding 15s` freeze at frame 2068 during battle,
followed by the watchdog and resumed frames. This is a confirmed artificial
stall; it is not evidence that every first-use effect/loading stall has been
eliminated.

The freeze and its bright-frame detector were removed. The presentation
cleanup also removes the unused overlay/replay experiment, its snapshots of
client arrays, and automatic `glReadPixels` probes at frames 100 and 600.
`fake_eglSwapBuffers` now performs optional Debug reporting followed by scene
presentation, swap and capture rearming, without diagnostic draws or waits.

`tests/test_frame_present.py` compiles the production swap body with host
stubs. It exercises 2,100 frames in both profiles, the former bright-frame
condition and the input-dialog flag. The expanded probe regression failed
before cleanup with **12 diagnostic probes**, and passes afterward with no
waits, probes or injected draws. This is a control-flow test, not a GPU timing
measurement.

`upscale-status.txt` is now Debug-only. `tools/check_release.py` rejects the
Debug build marker and eight known diagnostic paths/messages even when the
build cache says Release. The pre-cleanup Release was correctly rejected for
its status-file path. After a clean rebuild the checker passes, and
`tests/test_release_profile.py` rejects all nine injected diagnostic fixtures.
The standalone ELF deliberately retains symbols for crash analysis; symbols
are not runtime Debug instrumentation.

Optional Debug-only timing, GL logs, watchdog and explicit trigger-based test
controls remain. They are not included in the Release executable. Debug still
has the pre-existing unused-alternative-function compiler warnings; the clean
Release compilation completed without those warnings.

## Checks and repetition

Eight C suites passed with strict warnings, AddressSanitizer and
UndefinedBehaviorSanitizer. Both Python regressions, Release and Debug builds,
and the Release package/provenance checker also passed.

```sh
set -eu
mkdir -p build_host_tests
for name in text_encoding file_compat glyph_upload glyph_cell_reuse sharp_bilinear scene_clip; do
  /usr/bin/clang -std=c11 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    "tests/test_${name}.c" -lm -o "build_host_tests/test_${name}"
  "./build_host_tests/test_${name}"
done
for name in font_raster font_alignment; do
  /usr/bin/clang -std=c11 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    "tests/test_${name}.c" loader/stb_truetype.c -lm \
    -o "build_host_tests/test_${name}"
  "./build_host_tests/test_${name}"
done
python3 tests/test_frame_present.py
VITASDK=/usr/local/vitasdk cmake --build build_release --clean-first -j4
VITASDK=/usr/local/vitasdk cmake --build build_debug -j4
python3 tools/check_release.py build_release
python3 tests/test_release_profile.py build_release/FF6.elf
```

## Clean Release smoke and artifact identity

The cleaned Release ran in another private workspace:
`build_release/sharp-bilinear-smoke-_4wqkdki/`. Its configuration points only
to its own filesystem. Test saves are copies, while the commercial library
and OBB are read-only inputs. Inherited diagnostic files and triggers were
removed from the private copy before launch, not from the source workspace.
The options above were retained, including sharp-bilinear and fontscale 100.

New captures `Final Fantasy VI_2026-10-25-211025.png` and
`Final Fantasy VI_2026-10-25-211112.png` show the logo and the title menu.
At the 2-minute-25-second process check, no `boot.log`, `upscale-status.txt`
or trigger files were present, and both source test-save hashes were
unchanged. Earlier copied screenshots do not validate this Release.
This startup/menu observation is separate from the corrected Debug battle
comparison; it does not by itself prove a new Release Fire Beam round trip.
The tested Release `eboot.bin` SHA-256 is:

```text
2889c15510a9368f6f098fe60af740ea95b4dee46625cb54621ff2d72251c31d
```

Stage the exact candidate without overwriting RC1:

```sh
python3 tools/check_release.py build_release --output release/2026-10-25-rc2
```

`manifest.json` and `SHA256SUMS` in that directory identify the VPK, matching
unstripped ELF, inputs and source dirty state. The app SFO version remains
`01.00`; the candidate label is separate. Release does not enable sharp by
default: `sharpbilinear=0` and `offscreen=0` remain the conservative defaults.
Use the explicit test configuration above when reproducing the accepted
sharp-bilinear appearance. Do not replace an existing user's options or saves
automatically.

## Remaining release gates

The text-position issue is closed for the inspected menu labels and covered
host cases. Stable-release acceptance still requires physical Vita testing
of sharp/offscreen rendering, performance and repeated menu/battle transitions;
normal-save and quicksave exit/restart round trips; sustained play and
suspend/resume; and the languages actually claimed by the release. Additional
character/equipment/status/dialogue layouts remain outside this visual check.

First and repeated Fire Beam use should still be checked on the final Release
and hardware: the artificial pause is removed, but shader/resource first-use
cost has not been comprehensively measured. The full-string text-texture path
still follows the general texture filtering option rather than the registered
atlas exception. Optional LiveArea artwork is absent and remains a separate
presentation decision. None of these gates is marked complete by packaging.
