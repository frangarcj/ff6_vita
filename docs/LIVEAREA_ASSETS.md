# Standard-Republic LiveArea assets

Thank you to **Standard-Republic** for supplying the artwork and a3 layout.
The refresh replaces the geometric placeholders in the existing public
`v1.0.0-rc1` at the user's explicit request. Earlier validation reports describe
the first publication, before this presentation-only refresh.

## Source and adaptations

Source: user-supplied `ff6.zip`, SHA-256:

```text
8a42e98a1e0d6b3f1b46b362d2127e79a4a44bd7b0e1c2f83f41e1e7f1712b3d
```

| Packaged resource | Dimensions | Adaptation |
| --- | --- | --- |
| `sce_sys/icon0.png` | 128 x 128 | Original indexed PNG, unchanged bytes |
| `sce_sys/pic0.png` | 960 x 544 | Original indexed PNG, unchanged bytes; added to packaging |
| `sce_sys/livearea/contents/bg0.png` | 840 x 500 | Original indexed PNG, unchanged bytes |
| `sce_sys/livearea/contents/frame3.png` | 128 x 31 | Original Configuration button, unchanged bytes |
| `sce_sys/livearea/contents/frame4.png` | 280 x 158 | RGBA converted to indexed PNG8 with palette alpha; no resizing |
| `sce_sys/livearea/contents/template.xml` | a3 layout | `placeholder` replaced by `psla:-config`; content revision raised to 2 |

The supplied button placement (`x=-66`, `y=-90`, centered, origin `frame`) and
its 128 x 31 size are retained. The gate references `frame4.png`. Obsolete
`startup.png` and `config.png` placeholders are no longer in the bundle.
The loader already routes `psla:-config` to `companion.bin`; no loader change
is needed for this artwork update.

The startup conversion uses Pillow's RGBA FASTOCTREE quantizer with 256 colors
and no dithering, then saves an optimized indexed PNG. Palette transparency
retains fully transparent and fully opaque pixels. Color reduction is not
lossless; the converted logo was visually compared against the supplied image
and no rescaling or cropping was performed. The packaged result's SHA-256 is:

```text
8a7ea16242502a59c4d197893214a03f29edbf5fabc5fdfb6c59561c9ddca64c
```

The conversion is not a build dependency: all final resources are checked in.
`tools/generate_livearea.py` is only a historical geometric-art fallback and
now defaults to `build_livearea_fallback/`. It refuses output paths that could
replace packaged artwork.

## Validation and publication

`tools/check_release.py` checks the actual resource bytes inside the VPK, not
just filenames: PNG signatures, chunk CRCs, dimensions, indexed PNG8 format,
palette/transparency, bounded decompression and the a3 template's exact
Configurator target and image references. `tests/test_package_assets.py`
uses the same validation on source resources and rejects missing resources,
placeholder targets, incompatible layouts, incorrect button dimensions,
CRC corruption, a true-color startup PNG and lost startup transparency.

The previous GitHub release assets and tag metadata are preserved locally
before replacement. The same RC tag is deliberately updated to the new tested
source revision; the existing release is replaced only through the explicit
manual `replace_existing` workflow input. Automatic tag runs leave existing
releases alone. Replacement is restricted to an existing mutable prerelease,
checks its release ID and tag commit, updates all five assets and release
notes, and downloads the published bundle to compare it with the build.
The matching manifest and SHA256SUMS identify the refreshed package.

This resource check and visual inspection are not a real-Vita LiveArea
acceptance test. Hardware layout/caching and the general gameplay acceptance
gates remain separate. Reinstall the VPK without deleting game data or saves.

The attribution does not relicense the underlying Final Fantasy artwork,
logos or trademarks. Their ownership remains with their respective owners.
