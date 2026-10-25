# FF6 Vita candidate validation -- 2026-10-25

Working candidate: **1.0.0-rc1**. This report records bounded development and
emulator checks performed during preparation. It is **not a stable-release
signoff**, and the candidate is not frozen for publication.

The user reported that fonts look too small and that letters sometimes do
not appear. Reproduced defects in size metrics, bitmap-edge handling and
atlas uploads have now been corrected and covered by host regressions.
One post-fix resume-confirmation menu has now been inspected in the emulator
with readable text and no missing letters or overlap observed there. Broader
visual acceptance is still pending; this does not establish that every
reported text symptom is resolved. The physical-device gates in
[RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) also remain open.

## Artifact identity

The label `1.0.0-rc1` is distinct from the Vita SFO application version
`01.00`; the title ID is `FF6000000`. Once final source changes are committed
and the candidate is staged, the authoritative artifact records are:

- `release/2026-10-25-rc1/manifest.json`: exact source revision/dirty state,
  build settings, compiler identification, selected dependency hashes and
  package contents.
- `release/2026-10-25-rc1/SHA256SUMS`: the staged VPK, matching ELF and manifest
  checksums.

Those generated records, rather than a commit identifier embedded in this
tracked report, identify the exact staged build. Preparation checks below do
not certify binaries rebuilt after further fixes; rerun affected checks and
regenerate the artifact records after font integration and visual validation.

The manifest inventories **38 explicit static-archive inputs** from the
CMake link command in the tested build and hashes that command. It also
records compiler version strings and executable hashes, the selected vitaGL
header/archive, and the dependency patch/lock files. Compiler-added implicit
runtime libraries are outside that explicit-archive inventory. Archive hashes
identify binary inputs, not their upstream source revisions, and this is not
a claim of byte-for-byte reproducibility across different SDK installations.

## Build, host tests and package checks

| Check | Observed result | Scope and limit |
| --- | --- | --- |
| Pinned graphics dependency | Build succeeded from upstream vitaGL `c707ff00d9a372f0ba7056b4a4b723f5a00ba880` plus the bundled FF6 patch | Uses `NO_DEBUG=1 NO_SPLASHSCREEN=1 SHARED_RENDERTARGETS=1`; see [dependency recipe](../deps/README.md) |
| Release loader | Built with `CMAKE_BUILD_TYPE=Release`, `FF6_DEBUG=OFF`, and the project-local vitaGL header/archive; rebuilt successfully after the font changes | Compilation/linking and the post-fix package check passed; the bounded post-fix menu observation is recorded below |
| Debug loader | Built with `FF6_DEBUG=ON` and the scoped graphics dependency; rebuilt successfully after the font changes | Existing unused-alternative-function warnings remained in the debug build. The earlier debug build supplied the autoplay evidence below; that evidence does not become a post-fix test. |
| Text-encoding host tests | Passed with AddressSanitizer and UndefinedBehaviorSanitizer | Covers bounded UTF-8/UTF-16 conversion and IME buffer helpers, not the Vita keyboard UI or font rendering |
| File-compatibility host tests | Passed with AddressSanitizer and UndefinedBehaviorSanitizer | Covers Android open-flag translation and bounded path translation using temporary host files, not the game's complete save format or save UI |
| Real-font raster host tests | Passed with strict warnings, AddressSanitizer and UndefinedBehaviorSanitizer against bundled JP, SC and KR fonts | Checks the new EM sizing and raster helper against independently cropped complete stb bitmaps; not a visual game or Android-reference comparison |
| Glyph-upload host tests | Passed with strict warnings, AddressSanitizer and UndefinedBehaviorSanitizer | Checks atlas coordinates, row wrapping, source stride and bounded staging; does not exercise Vita GPU rendering |
| Reference game-data verifier | Accepted the known ARMv7 library and matching OBB | Exact sizes/SHA-256 match the reference pair documented in the README |
| Package checker, valid candidate | Accepted the release package and staged verified VPK/ELF/manifest/checksums during the checker test | Final staged artifact identity must come from the final candidate directory above |
| Debug-profile rejection | Rejected a fixture whose ELF contains the debug profile even with `FF6_DEBUG=OFF` in its cache | Demonstrates that the cache alone cannot label a debug ELF as release |
| Commercial-data rejection | Rejected a VPK fixture with `libFF6.so` injected | Verifies the package-content restriction |
| Executable mismatch rejection | Rejected a VPK fixture whose `eboot.bin` differs from the build output | Does not independently prove ELF-to-SELF equivalence beyond the validated build procedure |
| Dependency preflight rejection | Configuration rejected fixtures missing the required header declaration or archive symbol | Avoids building against an incompatible vitaGL dependency |

The host test sources are
[`tests/test_text_encoding.c`](../tests/test_text_encoding.c) and
[`tests/test_file_compat.c`](../tests/test_file_compat.c). Encoding coverage
includes accented/Japanese text, supplementary characters, malformed input,
small destination buffers, null/empty inputs and the maximum IME capacity.
File coverage includes create/read/append/truncate/exclusive-open behavior,
slots 0 and 3, translated `/sdcard/` paths, oversized paths and invalid input.

### Repeating the host regressions

Run from the repository root so the real-font tests can open the bundled font
files. The validation host uses macOS `/usr/bin/clang`; these are host tests,
not Vita executables.

```sh
set -eu
mkdir -p build_host_tests
for test_name in text_encoding file_compat glyph_upload; do
  /usr/bin/clang -std=c11 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    "tests/test_${test_name}.c" -o "build_host_tests/test_${test_name}"
  "./build_host_tests/test_${test_name}"
done
/usr/bin/clang -std=c11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  tests/test_font_raster.c loader/stb_truetype.c -lm \
  -o build_host_tests/test_font_raster
./build_host_tests/test_font_raster
```

The production data-verification helper is
[`tools/verify_game_data.py`](../tools/verify_game_data.py). The package checker
is [`tools/check_release.py`](../tools/check_release.py). The known Android
pair is version 2.1.6 (`versionCode=216`), with a 22,340,224-byte `libFF6.so`
and a 654,927,616-byte OBB; complete SHA-256 values are in the README and
release checklist.

## Font corrections and measured scope

The follow-up review reproduced concrete defects; the implemented changes
are independently covered in
[`tests/test_font_raster.c`](../tests/test_font_raster.c) and
[`tests/test_glyph_upload.c`](../tests/test_glyph_upload.c).

| Finding | Implemented change | Evidence and limit |
| --- | --- | --- |
| Requested text size was interpreted as the complete ascent/descent height | Use `stbtt_ScaleForMappingEmToPixels` for `fontscale=100`, matching requested EM-size semantics | Host tests compare the scale against stb's EM function. This does not establish identical Android-default-font layout. |
| A glyph with a negative left bearing, such as `j`, could be discarded completely by an all-or-nothing bitmap-bounds check | Rasterize the complete glyph and crop its intersection with the destination | Real-font tests compare exact pixel coverage against a separately cropped bitmap at all four edges, including partially and fully out-of-bounds cases. |
| Glyph uploads larger than 64 pixels were silently excluded | Use reusable staging bounded by the 1024x640 text source while preserving atlas-cell clipping | A 65x80 regression, 1024-pixel source stride, staging reuse and maximum-size cases pass. No GPU rendering is exercised by this host test. |
| The atlas row did not wrap at an admitted capacity boundary | Restore row modulo behavior | Capacity-boundary fixtures with cell sizes 27 and 32 produce the expected wrapped coordinates. |

The measured bundled JP font has `hhea` ascent `1160`, descent `-288`, and
`1000` units per EM. At normal, unclamped sizes, changing the scale definition
from pixel height to EM size gives `(1160 - (-288)) / 1000 = 1.448`. Thus the
new `fontscale=100` uses 1.448 times the old scale for that font, and a value
near `69` approximates the old `100`. This is a font-metric calculation, not a
claimed match against an Android screenshot or a universal percentage for
other fonts. Existing numeric overrides are not migrated automatically.

The vertical box is `ceil(ascent * scale) + ceil(-descent * scale)`, bounded
to the text buffer. This is a conservative box for the substituted Noto font;
Android's default-font top/bottom metrics are not available and are not
reproduced by that formula.

The raster tests run against all three bundled JP, SC and KR fonts. They
cover every ASCII codepoint from 33 through 126, accented Spanish examples,
selected Cyrillic/CJK glyphs, the zero-ink space, negative bearings, four-edge
clipping, overlapping glyph coverage, padded strides, buffer guards and
oversized scale inputs. Glyph-upload tests also reject invalid dimensions
and retain the atlas cell's clipping limits.

A separate glyph-index coverage inspection found **0 of 87 tested Thai
characters** in each bundled font. The tested precomposed Spanish set
(16/16) and Russian set (66/66) were present. Glyph presence is not a complete
language-rendering test: no complete Thai fallback or general complex
combining-text/shaping support is established. The font fixes address the
reproduced code defects; visual checks are still needed to determine whether
all of the user's reported symptoms are resolved.

## FF3 comparison and subsequent glyph-atlas filtering change

A read-only comparison against the user's local FF3 Vita checkout found that
the three bundled Noto font files and the stb_truetype sources matched by
hash. The texture-filter policy differed:

| Setting or path | Inspected FF3 behavior | FF6 behavior |
| --- | --- | --- |
| `bilinear=0` | Preserves the engine's requested texture parameters | Previously forced nearest sampling on glyph atlases as well as art; the new change exempts registered glyph atlases |
| `bilinear=1` | Forces linear sampling | Forces linear sampling |
| Sharp-bilinear | No such shader identified in the inspected checkout | Not implemented; the existing experimental offscreen final quad uses ordinary `GL_LINEAR` |

The new FF6 registry records the reference engine's two glyph-atlas page
handles. The texture-parameter override queries the actually bound texture
and keeps `GL_LINEAR` for those registered pages, even when `bilinear=0`.
Ordinary art textures retain their previous nearest/linear selection. Entries
are removed on texture deletion or replaced when a page receives a new
handle, so reusing a deleted atlas name for art does not retain font filtering.

The expanded `tests/test_glyph_upload.c` fixture passed with strict warnings,
AddressSanitizer and UndefinedBehaviorSanitizer. Its registry cases cover
both pages, unrelated handles, deletion/reuse, replacement and invalid
registrations. The latest release and debug builds also completed, and the
release package checker passed again.

The isolated debug run confirms the runtime selection: the archived
`font-linear-debug-boot.log` records MIN and MAG `LINEAR` for both texture
handles 6 and 7 and reaches frame 600 on `map=23`. It also records
`getTextFontSize(19) -> 29`, which is the computed Noto text box, not a measured
Android reference height. The private `options.cfg` was absent, so this used
the defaults `bilinear=0`, `fontscale=100`, `offscreen=0`; the trace exercises
the atlas exception rather than the all-linear option.

A subsequent release run supplied a limited visual check of this filtering
change in one resume-confirmation menu, recorded below. The earlier menu
screenshots are retained as evidence of the preceding build.

The exception does **not** identify every text/UI texture. The full-string
`createTextTexture`/`drawText00` path still follows the general filtering
override. No all-text filtering or FF3-identical visual result is claimed.

For future world scaling work, the final 480x320-to-816x544 upscale is the
appropriate place to consider a sharp-bilinear pass. That is a design option,
not an implemented feature or a release fix. It cannot recover correct atlas
sampling once neighboring-cell pixels are already mixed into the composed
image. The existing `offscreen` path remains experimental with default `0`.

## Isolated emulator observations before the new font fixes

The emulator was run from a copied Vita3K application with its own portable
filesystem, configuration, cache and logs under `build_release/smoke`.
Gameplay checks used copies of existing saves. No physical Vita was operated
for these checks, and emulator performance does not establish device
performance.

| Scenario | Observation | Acceptance status |
| --- | --- | --- |
| Release, clean first boot | Ran for more than 60 seconds; screenshots show the title/logo and copyright. Created `RockDat0.sav` and `RockDat1.sav`, each 147,280 bytes. No `boot.log` was created in the clean test directory. | Startup observation only. Initial save-file creation does not prove saving through the game menu or persistence after restart. |
| Unsupported executable, unchanged size | A deliberately altered copy of the library was rejected before engine startup. The log records `sceMsgDialogInit` with the unsupported/damaged-library error. | Fingerprint rejection observed in the log. The dialog was not interactively dismissed or visually accepted as a device UI test. |
| Debug build, copied saves, autoplay | Loaded the Narshe scene reported as `map=39`; a screenshot contains rendered gameplay. The debug trace reached frame 9,960 during roughly three minutes before the controlled stop. | Limited scene-load/rendering evidence. Menus, battles, user-controlled input and a gameplay save round trip were not exercised. |
| Release restart with copied saves | The restart log was saved and screenshot `Final Fantasy VI_2026-10-25-183033.png` shows the logo. The private copy of `RockDat1.sav` changed; `RockDat0.sav` did not change during that restart. | Unresolved. A changed file and a logo screenshot are insufficient to establish that the correct gameplay state was restored. |

The native `EXC_BAD_ACCESS` log messages were traced to Vita3K's handled
Dynarmic fall-through fault path. The emulator recovered and continued frame
processing; no fatal trap was observed in the inspected runs. Those messages
are therefore not, by themselves, evidence of a fatal game crash. This
attribution does not establish that the release restart restored the expected
gameplay state or that a full save/load round trip succeeded.

The observations above precede the new EM-size, clipping and atlas-upload
changes. They must not be treated as post-fix gameplay evidence.

Interactive native control could not be initialized on two attempts because
of native-pipe connection failures. That prevented driving the required menu,
battle and save interactions through the available control interface. The
autoplay observation above does not cover those missing interactions.

## Post-fix release menu observation

After the strict sanitized font regression passed, both loader profiles
rebuilt successfully and the release package checker passed again. The
updated release build then ran for more than 90 seconds in the isolated
portable emulator without creating `boot.log`.

Screenshots `Final Fantasy VI_2026-10-25-184217.png` and
`Final Fantasy VI_2026-10-25-184247.png` show the Continue/quicksave
resume-confirmation menu. It displays party metadata for Terra, Wedge and
Biggs, HP/MP values, Narshe, time `00:05`, steps `49`, gil `3378`, and the
confirmation prompt. Text was readable, with no missing letters or overlap
observed in that menu. The party-name `??????` sequence is a game-provided
story placeholder.

This checks **one menu only**. The resume action was not accepted, so the
observation does not prove restoration of gameplay state, completion of a
quicksave round trip, battle rendering, other dialogue/menu layouts or
physical-device behavior. It supplies limited visual evidence for the
integrated font fixes, beyond the host-only raster tests.

This run predates the subsequent glyph-atlas filtering change described
above. Reuse of these screenshots does not validate that newer change.

## Latest atlas-filter release menu observation

The release build containing the atlas-filter exception ran for more than
60 seconds and produced screenshot
`Final Fantasy VI_2026-10-25-185139.png`. It shows the same Continue/quicksave
resume-confirmation fixture as the earlier `184217` capture. The font coverage
appears smoothed and readable, with no missing letters or overlap observed in
that screen. No other visual differences were observed in that screen.

The private `options.cfg` was absent and no release `boot.log` was created.
The defaults were therefore `bilinear=0`, `fontscale=100`, `offscreen=0`,
consistent with the debug trace proving the atlas exception. The run was
stopped deliberately. The resume action was not accepted, and this remains
one-menu evidence rather than a gameplay/save round trip or physical-device
test.

The receipt `build_release/smoke/font-filter-validation.json` records the
configuration, evidence filenames and the artifacts used for this test:

| Tested artifact | SHA-256 |
| --- | --- |
| `FF6.vpk` | `9e1d5ada678786d772af60db8a97a8c9cd26ca40d26daf458d7fc4eaf98e549d` |
| `FF6.elf` | `ecc9d4519283e3db4f229c09dda03c9a859b21f7de8b9d937d6b766017e18053` |

These identify the smoke-test artifacts. The final staged candidate's
authoritative revision and checksums remain in its generated manifest and
`SHA256SUMS`; they are not inferred from this report or its eventual commit.

## Save preservation

The final comparison recorded in
`build_release/smoke/final-save-preservation.json` reports
`original_saves_unchanged: true`. SHA-256 values for both original
`RockDat0.sav` and `RockDat1.sav` matched their pre-test values. Only copies in
the isolated test filesystem were used by the emulator.

The same record distinguishes private-copy hashes before and after the
release restart: `RockDat1.sav` changed. No explanation of that change is
accepted yet, and it is not counted as a successful gameplay save/load test.

## Local evidence locations

These are local test-workspace paths. They are not commercial data to bundle
in the VPK, and the ignored emulator workspace is not part of the source
release.

| Evidence | Project-relative location |
| --- | --- |
| Clean release startup | `build_release/smoke/run-clean.log` |
| Unsupported-library rejection | `build_release/smoke/run-invalid-library.log` |
| Instrumented scene/autoplay trace | `build_release/smoke/run-autoplay.log` |
| Release restart | `build_release/smoke/run-restart-release.log` |
| Post-fix release menu run | `build_release/smoke/run-font-release.log` |
| Latest atlas-filter debug run | `build_release/smoke/run-font-linear-debug.log` and archived `build_release/smoke/font-linear-debug-boot.log` |
| Latest atlas-filter release run | `build_release/smoke/run-font-linear-release.log` |
| Latest atlas-filter receipt | `build_release/smoke/font-filter-validation.json` |
| Original and private save-hash comparison | `build_release/smoke/final-save-preservation.json` |
| Captured emulator frames | `build_release/smoke/portable/screenshots/Final Fantasy VI/` |
| Restart logo screenshot | The screenshot directory above, file `Final Fantasy VI_2026-10-25-183033.png` |
| Post-fix menu screenshots | The screenshot directory above, files `Final Fantasy VI_2026-10-25-184217.png` and `Final Fantasy VI_2026-10-25-184247.png` |
| Latest atlas-filter menu screenshot | The screenshot directory above, file `Final Fantasy VI_2026-10-25-185139.png` |

## Open work before acceptance

1. Extend visual acceptance of the integrated font fixes beyond the one
   resume-confirmation menu already checked. Verify text size, complete glyphs
   and layout in additional dialogue, menus and battles on actual game frames.
   Include the latest glyph-atlas filtering change: its debug sampling trace
   and one release menu have been checked, but other text/art paths have not.
2. Determine whether the copied-save release restart reaches the expected
   gameplay state. The recovered native emulator faults have been attributed,
   but that is independent of save-state correctness.
3. Verify a normal save through the game UI, a full exit/restart/load, another
   slot beyond the first two, overwrite behavior, and quicksave/resume.
4. Verify menus, dialogue, battle sprites/effects, Narshe scrolling edges,
   input, audible audio and suspend/resume on real hardware. Historical July
   results do not validate the current candidate automatically.
5. Complete the sustained-play and relevant language/configuration checks in
   the release checklist. Neither a 60-second first boot nor a three-minute
   debug scene run represents full-game or long-session coverage.
6. Rebuild and stage the final candidate after fixes, preserve its matching
   ELF and generated provenance records, then decide optional LiveArea
   presentation and the publication destination.

Optional LiveArea assets were absent and accepted by the package checks.
Their final presentation remains a release decision; the absence of those
assets is not a malformed-VPK result. Broader visual acceptance of the font
fixes and the hardware gates remain the substantive blockers to stable-release
signoff.
