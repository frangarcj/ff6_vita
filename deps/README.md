# FF6 graphics dependency

The port requires `vglInPlaceTexSubImage`; an arbitrary vitaGL SDK install is
not enough. This directory keeps the exact additional source used for the
candidate so a build does not depend on an unpublished local branch.

- Upstream: https://github.com/Rinnegatamante/vitaGL
- Base: `c707ff00d9a372f0ba7056b4a4b723f5a00ba880`.
- Apply `vitagl-ff6.patch` from this directory.
- Machine-readable provenance: [vitagl.lock.json](vitagl.lock.json).

The patch contains the in-place texture update switch, native fixed-point
color support and the diagnostic counters used by `FF6_DEBUG`. It also fixes
the diagnostic printf format after widening the shader mask to 64 bits.
The default loader still uses its own guarded color conversion
(`fixedcolor=0`). Keep `TEXTURES_SPEEDHACK` disabled: it changes texture
destruction as well as upload behavior.

## Build without replacing the shared SDK library

Run from the FF6 project root with VitaSDK already on PATH:

```sh
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"
FF6_ROOT="$PWD"
git clone https://github.com/Rinnegatamante/vitaGL build_release/deps/vitaGL
git -C build_release/deps/vitaGL checkout --detach c707ff00d9a372f0ba7056b4a4b723f5a00ba880
git -C build_release/deps/vitaGL apply --check "$FF6_ROOT/deps/vitagl-ff6.patch"
git -C build_release/deps/vitaGL apply "$FF6_ROOT/deps/vitagl-ff6.patch"
make -C build_release/deps/vitaGL NO_DEBUG=1 NO_SPLASHSCREEN=1 SHARED_RENDERTARGETS=1 -j4
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release -DFF6_DEBUG=OFF \
  -DVITAGL_INCLUDE_DIR="$FF6_ROOT/build_release/deps/vitaGL/source" \
  -DVITAGL_LIBRARY="$FF6_ROOT/build_release/deps/vitaGL/libvitaGL.a"
cmake --build build_release --parallel 4
python3 tools/check_release.py build_release --output release
```

The clone command assumes the destination does not already exist. For an
existing dependency checkout, inspect its revision and diff before reusing it;
do not reset a checkout containing unrelated work. The recipe was built
locally, and the pinned base commit was also fetched successfully from the
advertised public upstream during candidate preparation. The feature-origin
commit IDs in the lock file are provenance, not required remote fetch targets;
their changes are supplied by the bundled patch.

`NO_DEBUG=1` turns off vitaGL error checks, `NO_SPLASHSCREEN=1` avoids an
extra splash/context initialization, and `SHARED_RENDERTARGETS=1` enables
the render-target mode used by this port. Keep these settings with the
candidate. A separate instrumented graphics build can use `LOG_ERRORS=1`
without `NO_DEBUG=1` when diagnosing a rendering issue.

The SDK must also supply the other libraries listed in the main README.
`tools/check_release.py` records the selected compiler, vitaGL header and
archive hashes. The recipe does not promise identical output bytes across
different SDK builds or absolute source paths.
