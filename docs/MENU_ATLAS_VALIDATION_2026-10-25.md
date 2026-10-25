# Menu glyph-cell follow-up -- 2026-10-25

This is a bounded follow-up to the RC1 validation, not a stable-release
signoff. The original staged RC1 has not been replaced. The source baseline
was `4f08d34`; this report describes the additional working-tree patch.

The build described here is preserved in `build_release/menu-atlas-candidate`.
It predates the subsequent sharp-bilinear compositor and UI-clipping work;
statements about the unimplemented sharp filter below describe this preserved
candidate, not the later working-tree implementation.

## Reproduced defect and correction

The glyph uploader previously replaced only the new bitmap's width and
height. Reusing the same atlas cell for a narrower or shorter glyph left
pixels from the previous occupant outside that rectangle. The registered
font atlases use linear filtering, so samples near the new bitmap's edge
can include those old pixels.

`tests/test_glyph_cell_reuse.c` reproduces the stale contents without a GPU:
a 32x32 opaque glyph is replaced by a 6x18 glyph in the same cell. The new
source bitmap deliberately has dirty padding. Before the fix the test
returned exit code 1 with:

```text
reused glyph cell retains wrong pixels at (38,32)
```

The corrected uploader distinguishes the upload cell dimensions from the
valid bitmap dimensions. It initializes the complete cell to transparent
pixels and copies only the valid bitmap rectangle, retaining the 1024-pixel
source stride. A zero-width glyph clears its cell instead of skipping the
upload. Adjacent cells, slot addressing, row wrapping, font metrics and
filter selection are unchanged.

The reusable staging buffer is bounded to one 1024x1024 RGBA cell (4 MiB in
the maximum admitted case). Source reads remain bounded to the 1024x640
text image. Smaller cells reuse the allocation. Uploading a whole cell can
transfer more bytes than uploading only the new glyph rectangle; no Vita
performance measurement is claimed for that tradeoff.

The new regression passes after the correction. It checks the entire
simulated atlas: the new bitmap is exact, all unused pixels in the reused
cell are transparent, a zero-width replacement erases it, and neighboring
cells retain their sentinel values. The expanded existing glyph-upload
suite covers padded stride, clipping, capacity-boundary wrapping, buffer
reuse, the maximum admitted cell and invalid arguments.

This establishes removal of stale cell contents. It does not prove that
every reported missing-letter symptom is resolved or that linear sampling
can never blend neighboring live cells at their shared boundary.

## Build and host verification

All five host suites passed with strict warnings, AddressSanitizer and
UndefinedBehaviorSanitizer. The real-font suite passed for the bundled JP,
SC and KR fonts. Commands, run from the project root:

```sh
set -eu
mkdir -p build_host_tests
for name in text_encoding file_compat glyph_upload glyph_cell_reuse; do
  /usr/bin/clang -std=c11 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    "tests/test_${name}.c" -o "build_host_tests/test_${name}"
  "./build_host_tests/test_${name}"
done
/usr/bin/clang -std=c11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  tests/test_font_raster.c loader/stb_truetype.c -lm \
  -o build_host_tests/test_font_raster
./build_host_tests/test_font_raster
VITASDK=/usr/local/vitasdk cmake --build build_release -j4
VITASDK=/usr/local/vitasdk cmake --build build_debug -j4
python3 tools/check_release.py build_release
```

Both Release and Debug produced VPKs. Debug retains the pre-existing
unused-alternative-function warnings. The release checker accepted the
release profile and verified the packaged executable against the build
output. Optional LiveArea artwork remains absent. Compilation and package
checks are not gameplay or hardware acceptance.

## Isolated emulator observation

A fresh copy of the earlier smoke workspace was created at:

`build_release/menu-atlas-smoke-py0xhe4x/`

Its configuration points to its own filesystem. Save files were copied;
the linked commercial library and OBB were read-only inputs. The fresh
Release executable replaced only the private copy's application executable.
Autoplay triggers were removed only in the new copy. Tested options:

```ini
bilinear=0
fontscale=100
offscreen=0
inplacetex=1
fixedcolor=0
```

This build does **not** implement sharp-bilinear. Ordinary art uses nearest
sampling and registered font atlases use linear sampling. Vita3K's
`screen-filter: Bilinear` is its ordinary presentation filter, not a
sharp-bilinear implementation in the port.

Native CUA startup failed, so process-scoped AppleScript input and the
emulator's F12 screenshots were used. Accessibility navigation was
intermittent and did not establish a reliable complete menu traversal.
The following are the two new inspected captures, under
`portable/screenshots/Final Fantasy VI/` in the new workspace:

| Capture | Bounded observation |
| --- | --- |
| `Final Fantasy VI_2026-10-25-194331.png` | Narshe field scene is rendered with the party character visible. |
| `Final Fantasy VI_2026-10-25-194358.png` | Battle scene has visible enemy and party sprites; the action menu shows readable Magitek/Items labels and HP/MP values. The lower Defend row is only partly visible, so complete menu layout is not signed off. |

The user reported that the image looked good. This is feedback on the
shown run, not an A/B comparison proving that this particular atlas fix
changed those frames. Earlier screenshots copied into the same directory
are historical evidence and do not validate this build.

`receipt.json` records `stop: process_exit`, exit code 0 and matching hashes
for both source smoke-workspace saves before and after the run. The wrapper
had a 240-second limit, but its timeout-stop path was not used. No normal-save
or quicksave round trip was performed. The tested executable's SHA-256 is:

```text
e8d648f77529c63bfcb1f0f58ee37947fd4b6f6a5b7528c5b66b8e2dad022581
```

Supporting local evidence is `run.log`, `receipt.json`, the captures above,
`build_host_tests/glyph-cell-before.log` and the bounded launch script
`build_host_tests/menu_atlas_smoke.py`. These ignored development artifacts
are not game data to distribute with the source or VPK.

## Remaining work

Additional character, equipment, item, status and dialogue screens need
visual checks, along with repeated transitions, languages and real Vita
performance. The full-string `createTextTexture`/`drawText00` path still
follows the general filtering override rather than the atlas exception.

The optional offscreen compositor has separate state-restoration defects:
`present_scene_fbo` changes the texture binding, active texture unit,
vertex/UV pointers, current color, matrix mode and texture environment
without restoring all their prior values. This patch does not change that
experimental path and does not validate it. Correcting its state handling
and implementing sharp-bilinear scaling remain separate work.
