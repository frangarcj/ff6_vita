# Third-party notices

## Bundled fonts

`NotoSansJP-Regular.ttf`, `NotoSansKR-Regular.ttf`, and
`NotoSansSC-Regular.ttf` are Noto Sans fonts, version 2.002. Their embedded
copyright notice identifies Adobe, 2014-2020. They are distributed under the
SIL Open Font License, version 1.1.
The complete notice and license accompany the fonts in
[`licenses/OFL-Noto.txt`](licenses/OFL-Noto.txt), including in the VPK.

Upstream font project: <https://github.com/notofonts/noto-cjk>.
The license text comes from
<https://github.com/notofonts/noto-cjk/blob/main/Sans/LICENSE>.
The copyright years above describe the bundled files, not a newer font release.

## Loader sources and linked libraries

The loader's existing license is in [`LICENSE`](LICENSE), included in the VPK.
The bundled `stb_image` and `stb_truetype` source files retain their authors'
license notices in their source headers. Dependencies retain their own licenses;
the loader's MIT license does not replace them.

Build dependency provenance and any patches used for a release must be retained
with its source release. In particular, vitaGL is licensed under LGPL-3.0;
its corresponding library source and applicable license notices are available
from <https://github.com/Rinnegatamante/vitaGL> and must match the build being
distributed. This notice is not a substitute for the dependency sources.

## Game data

The VPK does not include the commercial Android executable, APK, OBB, or saved
games. The user supplies the compatible game data from their own copy. Game
titles and trademarks belong to their respective owners.
