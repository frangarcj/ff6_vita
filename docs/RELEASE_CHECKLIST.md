# Release candidate checklist

Public candidate: **v1.0.0-rc1**. Earlier local RC1/RC2 snapshots and their
evidence are preserved; those names were not public GitHub release numbers.

The public candidate adds the [FF3 parity features](FF3_PARITY.md), Configurator,
LiveArea, PostFX and automated build/release workflows. Its authoritative
revision and checksums belong to the bundle attached to the GitHub tag, not to
the older local snapshots listed below. These additions do not close hardware
or full-game acceptance gates.

Status: **vertical text alignment and sharp-bilinear battle-menu clipping
corrected; Release diagnostics cleaned; broader acceptance pending**.
The [RC2 report](RC2_VALIDATION_2026-10-25.md) records the follow-up host tests,
menu captures, diagnostic cleanup and candidate identity. The preparation
checks below also retain historical RC1 evidence; those observations are not
automatically evidence for every later build.
This document records the release gates. A build, an emulator screenshot or a
historical observation does not mark a hardware gate complete.

See [the RC1 validation report](VALIDATION_2026-10-25.md) for the earlier
checks and their limits. The current candidate is not a stable-release signoff.

## Recovered baseline

The resumed source baseline is commit `88b3bb6` (2026-07-29). It contains the
loader bootstrap, Android compatibility shims, save-path/open-flag fixes,
text and palette fixes, in-place texture updates, and the latest correction
for reusing a stale `GL_FIXED` color pointer. The historical investigation is
preserved in [HUNT.md](../HUNT.md).

| Area | Evidence available at resumption | Candidate acceptance |
| --- | --- | --- |
| Build | Baseline release build completed with `FF6_DEBUG=OFF`, VitaSDK at `/usr/local/vitasdk`, GCC 15.2.0 | Rebuild and inspect the final changed candidate |
| Narshe scrolling | July hardware session recorded that `inplacetex=1` made it smooth | Recheck default candidate and inspect scrolling edges |
| Menus and battles | Stale color-pointer fix committed after a hardware regression | Verify text, sprites, effects and repeated transitions on hardware |
| Save files | Bionic flag/path translation (`034c0c4`) and slot-read fix (`d603fcd`) are implemented | Normal-save and quicksave persistence after a full restart |
| Text metrics | Resumed code used pixel-height sizing; the October fix changes `fontscale=100` to the engine's requested EM size | Check updated menus/dialogue, complete glyphs and the languages claimed by the release |
| Optional rendering | `offscreen=0`, `fixedcolor=0` by default; alternative paths have limited historical emulator evidence | Keep experimental status unless separately tested on hardware |
| Tile seams | Thin seams were reported in the historical hardware investigation | Record remaining visual limitations accurately |

The initial successful build is evidence about the resumed baseline. It does
not validate later source edits or mark the final package as tested.

## Preparation checks completed

The following development checks have passed during candidate preparation:

- The [pinned vitaGL recipe](../deps/README.md) builds from upstream commit
  `c707ff00d9a372f0ba7056b4a4b723f5a00ba880` plus the bundled FF6 patch with
  `NO_DEBUG=1 NO_SPLASHSCREEN=1 SHARED_RENDERTARGETS=1`.
- Both release and debug loader builds succeed using that project-local
  archive. The release package passes `tools/check_release.py`.
- `tools/verify_game_data.py` verifies the reference executable and OBB against
  the sizes and SHA-256 values below.
- Host tests in `tests/test_text_encoding.c` and `tests/test_file_compat.c`
  pass with AddressSanitizer and UndefinedBehaviorSanitizer.
- New `tests/test_font_raster.c` and `tests/test_glyph_upload.c` regressions
  pass strict warnings plus AddressSanitizer and UndefinedBehaviorSanitizer.
  They exercise the actual bundled fonts, bitmap-edge clipping, atlas row
  wrapping and glyph uploads larger than 64 pixels. Commands and scope are in
  the dated validation report.
- The subsequent glyph-atlas filtering change has passed host registry
  lifecycle tests, including deletion/handle reuse and page replacement.
  Both profiles rebuilt and the package check passed. A debug runtime trace
  with default `bilinear=0` confirms MIN/MAG `LINEAR` on both atlas pages.
  A subsequent release run exceeded 60 seconds without `boot.log` and showed
  the same resume-confirmation menu with readable, smoothed text and no
  missing letters or overlap observed there. The action was not accepted;
  broader scene and device checks remain open.
- After integrating the font changes, release and debug builds completed and
  the release package checker passed again. A post-fix release emulator run
  lasted more than 90 seconds without creating `boot.log`; its
  resume-confirmation menu showed readable text without observed missing
  letters or overlap. The resume action was not accepted, so this is not a
  completed save/load round trip.
- Negative fixtures are rejected for a debug-profile ELF despite an OFF
  cache, an injected `libFF6.so`, a mismatched packaged `eboot.bin`, and missing
  vitaGL header declarations/archive symbols.
- An isolated release first boot ran for more than 60 seconds and produced
  title/logo/copyright screenshots without creating `boot.log`. Initial
  `RockDat0.sav`/`RockDat1.sav` creation was observed; this is not a save/load
  round trip.
- Before the new font fixes, debug autoplay loaded a copied save into Narshe
  (`map=39`) and reached frame 9,960. The original save-file hashes remained
  unchanged. The native exception messages were traced to handled Vita3K
  Dynarmic fall-through faults, with no fatal trap observed. This does not
  establish a complete gameplay save/load round trip.

These checks cover dependency preparation, packaging, asset identification
and extracted compatibility helpers, plus bounded emulator observations.
They do not establish complete save persistence, correct rendering throughout
the game or device behavior. Repeat affected checks after further source
changes and inspect the final staged artifacts.

## Candidate identity and provenance

The authoritative revision, dirty state, build inputs and checksums for RC2
are recorded in `release/2026-10-25-rc2/manifest.json` and `SHA256SUMS` when
staged. This local candidate may have uncommitted source changes: review the
manifest rather than treating the baseline revision alone as its identity.
No publication or source commit is implied. Do not infer Android versions
from APK filenames or use a different build's ELF to analyze a crash.

| Field | Value |
| --- | --- |
| Source revision and local modifications | Authoritative values in the final staged `manifest.json` (`source_revision` / `source_dirty`) |
| Candidate label | `1.0.0-rc2`, dated 2026-10-25; local candidate, not published or signed off as stable |
| SFO title ID | `FF6000000` |
| SFO application version | `01.00`, accepted by the candidate package checks; distinct from the RC label |
| Release VPK path and SHA-256 | `release/2026-10-25-rc1/FF6.vpk`; checksum in the final staged `SHA256SUMS` |
| Matching ELF path and SHA-256 | `release/2026-10-25-rc1/FF6.elf`; checksum in the final staged `SHA256SUMS` |
| Build command and compiler versions | Pinned dependency recipe with Release / `FF6_DEBUG=OFF`, VitaSDK GCC 15.2.0; final compiler versions and executable hashes in the manifest |
| vitaGL source revision, patches and flags | Upstream `c707ff00d9a372f0ba7056b4a4b723f5a00ba880` plus `deps/vitagl-ff6.patch`; see `deps/vitagl.lock.json` and the preparation recipe |
| Other linked dependency versions/provenance | Manifest hashes the CMake link command and 38 explicit static archives in the tested build; implicit compiler runtimes and upstream source revisions are not inferred |
| Android package/versionCode/versionName | `com.square_enix.android_googleplay.FFVI` / `216` / `2.1.6`, verified from the decoded Android manifest |
| Supported `libFF6.so` | 22,340,224 bytes; SHA-256 `f612f711330238cf405a403fb084abd2b8d7c9e72f9867740aed2b834fc731e6` |
| Matching `main.obb` | 654,927,616 bytes; SHA-256 `4f443b8d0be0a7976f8dd540cccb24a643937afd6eec043e7bf7d13fe427369f` |
| Tester, date and hardware | Pending device validation |
| Firmware and plugin versions | Pending device validation |
| Configuration and clock overrides | Baseline below; record any deviations |

Use the bundled [dependency recipe](../deps/README.md) and
[lock file](../deps/vitagl.lock.json). The local vitaGL checkout inspected at
resumption was `cc69f12f90cb695688558d4af25f19b93d42899e`; that historical local
revision is not the candidate's fetch target. The recipe supplies the FF6
changes as a patch against the upstream base above, so it does not require
the unpublished feature-origin commits to be available remotely.

The original OBB filename is
`main.21701.com.square_enix.android_googleplay.FFVI.obb`; this numeric prefix
differs from the APK's verified `versionCode=216`. Use the file hashes above
to identify the fixture pair. The Android executable's CRC32 is `1441da16`.
Verify the pair without changing files using:

```bash
python3 tools/verify_game_data.py --so "/path/to/libFF6.so" --obb "/path/to/main.obb"
```

## Baseline test configuration

Use a release build with `FF6_DEBUG=OFF`. Start with no `options.cfg`, or use
the following explicit defaults. Restart the application after changes.

```ini
bilinear=0
integerscale=0
language=0
antialiasing=2
sound=1
fontscale=100
inplacetex=1
offscreen=0
fixedcolor=0
```

The loader requests 444 MHz CPU, 222 MHz bus/GPU and 166 MHz GPU crossbar
clocks. Record any plugin overrides because they affect performance results.
Keep existing saves backed up and use separate test slots for save checks.
`fontscale=100` now means the requested EM size. Existing overrides retain
their numeric values and need reassessment; see the README's migration note.

## Build and package gate

After visual validation, final fixes and a clean source commit, rebuild
and stage the exact candidate:

```bash
python3 tools/check_release.py build_release --output release/2026-10-25-rc1
```

The staged `manifest.json` records package/build evidence, and `SHA256SUMS`
identifies the staged VPK, ELF and manifest. Keep these together. A passing
checker result covers its package checks; runtime and device checks below
still require separate evidence. Compiler executable hashes, the selected
vitaGL header/archive hashes and explicit linked-archive hashes identify
those inputs; they do not identify an entire SDK source revision or promise a
byte-for-byte reproducible build.

- [x] Build the candidate source using the documented SDK and scoped vitaGL
      dependency, with `FF6_DEBUG=OFF`; also build the debug profile separately.
- [x] Inspect the candidate VPK: readable archive, `eboot.bin`, correct `param.sfo`, all
      three Noto fonts and the intended license/notice files.
- [x] Confirm the candidate VPK excludes APKs, OBBs, `libFF6.so`, save files, crash dumps,
      local logs and other test fixtures.
- [ ] Rebuild after remaining source fixes, rerun affected checks, and record
      final checksums for the VPK and matching ELF, the source revision/local
      modifications, compiler details, and required dependency patches.
- [x] Observe a clean release emulator first boot without creation of `boot.log`.
- [ ] Confirm the final release ignores debug trigger files and retains the
      expected instrumentation-free behavior on the target device.
- [ ] Review optional icon/LiveArea artwork. Its absence is a presentation
      decision, not a malformed-VPK condition.

## Font fix acceptance

- [x] Restore requested EM-size scaling with a separate, conservative
      `ceil(ascent * scale) + ceil(-descent * scale)` box for the bundled Noto
      font.
- [x] Replace whole-glyph rejection at bitmap edges with cropping; regression
      evidence includes the negative-left-bearing `j` that was discarded.
- [x] Restore atlas row wrapping at capacity and remove the artificial
      64-pixel glyph upload ceiling while retaining cell and buffer bounds.
- [x] Run real-font raster and atlas-upload host regressions with strict
      warnings, AddressSanitizer and UndefinedBehaviorSanitizer.
- [x] Rebuild both profiles, recheck the release package and inspect one
      post-fix resume-confirmation menu in the emulator without observed
      missing letters or overlap.
- [x] Implement the glyph-atlas-only linear-filter exception and pass host
      registry tests for page replacement, deletion and handle reuse.
- [x] Rebuild and use a debug runtime trace to confirm both registered atlas
      pages receive MIN/MAG linear sampling with the default `bilinear=0`.
- [x] Inspect the latest release filtering change in the resume-confirmation
      menu with default options, without observed missing letters or overlap.
- [ ] Check additional art/text scenes. Ordinary art and the unclassified
      full-string text path continue to follow the general override.
- [ ] Verify size, complete text and layout in updated dialogue and menus on
      the game build; host raster tests do not prove the full rendering path.
- [ ] Rerun affected build, package and emulator checks before freezing the
      candidate. Record which languages and scenes were actually checked.

The separate glyph-coverage check found none of 87 tested Thai characters in
any of the three bundled fonts. The tested precomposed Spanish and Russian
sets were present; this is glyph coverage, not proof of every language or
complex combining-text layout. No complete Thai fallback is provided.

## Hardware acceptance

For every result, record the candidate checksum, device, configuration,
procedure and outcome. A failure stays open until retested on the corrected
candidate.

### Installation and first run

- [ ] Install the candidate with the matching supported game files and required
      plugins. Reach the title screen and start a new game.
- [ ] Confirm actionable errors for missing game files or missing required
      components using a disposable test setup.
- [ ] Confirm expected behavior for unsupported game binaries if the candidate
      enforces a supported-binary fingerprint.
- [ ] Upgrade from the previous installed build while preserving the backed-up
      save directory, then confirm existing saves are still recognized.

### Save persistence -- required before release

- [ ] Save normally in a new slot, exit the application fully, relaunch and
      load it. Verify location, party and progress against the pre-save state.
- [ ] Repeat with a slot beyond the first two to cover the slot-detection fix.
- [ ] Overwrite a test slot, relaunch and confirm the newer state is loaded.
- [ ] Create a quicksave through the game menu, exit fully and resume it through
      the game's intended flow. Verify progress and expected quicksave
      lifecycle behavior.
- [ ] Confirm a pre-existing save is loaded without being mistaken for empty
      or damaged data. Test error handling only with disposable copies.

An on-screen success message or creation of a file is insufficient: the
restart-and-load result is the acceptance evidence.

### Graphics and performance -- required before release

- [ ] Walk through Narshe and scroll in multiple directions. Record smoothness
      and inspect newly revealed tiles for stale or mixed texture content.
- [ ] Open and close character, equipment, item and status menus repeatedly.
      Check text color, spacing, textures and highlights.
- [ ] Enter a battle and inspect party/enemy sprites, attacks, effects, menu
      text and depth ordering. Return to the field and repeat transitions.
- [ ] Check dialogue and title/menu text with `fontscale=100`; verify every
      language explicitly advertised as tested.
- [ ] Check scene fades, map changes and return from menus for visual
      corruption. Record any remaining tile seams as a known limitation.
- [ ] If corruption occurs, compare the same scene with `inplacetex=0` after
      restarting, and record both correctness and performance. Do not use the
      slower comparison configuration to claim default-mode acceptance.

### Input, audio and lifecycle

- [ ] Verify the D-pad, left stick, face/shoulder buttons, Start/Select and
      front touch in gameplay and menus, including release/repeat behavior.
- [ ] Verify music, sound effects and volume behavior across field/battle/menu
      transitions.
- [ ] Suspend and resume the Vita, then confirm input, rendering, audio and
      saving continue to work.
- [ ] Complete a sustained play session of at least 30 minutes with several
      area, menu and battle transitions; record crashes or progressive
      corruption. This is a smoke test, not full-game completion coverage.

### Optional configurations

`offscreen=1` and `fixedcolor=1` remain experimental until separately tested.
Testing them does not replace the default-mode gates. Document alternate
filtering, integer scaling, MSAA and language combinations actually exercised;
do not imply that every combination passed.

## Release decision

- [ ] Complete visual acceptance of the font fixes and the outstanding
      gameplay restart/save checks in the validation report.
- [ ] All required gates above passed on the same final candidate, or a failure
      was fixed and the affected gate plus relevant regressions were retested.
- [ ] README setup, options, supported Android data, build instructions and
      known limitations match the final package and observed results.
- [ ] Record the tested gameplay scope and remaining limitations. Google Play
      cloud saves, achievements and campaigns are unavailable.
- [ ] Choose the publication destination and final release label, then publish
      only the validated loader distribution and its supporting notices.

Until visual font acceptance and the required hardware gates are complete,
describe the output as a candidate under validation. Do not mark this
checklist complete because the build, package inspection or a bounded
emulator run passed.
