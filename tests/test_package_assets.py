#!/usr/bin/env python3
"""Check the complete source-side application bundle before cross-compiling."""
import importlib.util
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]


def main():
    spec = importlib.util.spec_from_file_location('release_check', ROOT/'tools/check_release.py')
    checker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checker)
    for name in (*checker.FONTS, *checker.NOTICES, *checker.ARTWORK, *checker.SHADERS):
        path = ROOT/name
        assert path.is_file() and path.stat().st_size > 0, name
    resources = {name: (ROOT/name).read_bytes() for name in checker.ARTWORK}
    checker.validate_livearea(resources)
    failures_checked = 0

    def rejected(changed):
        nonlocal failures_checked
        try:
            checker.validate_livearea(changed)
        except ValueError:
            failures_checked += 1
        else:
            raise AssertionError('Invalid LiveArea bundle accepted')

    # These checks use the same validator on source files and inside the VPK.
    for missing in checker.ARTWORK:
        changed = resources.copy()
        changed.pop(missing)
        rejected(changed)
    xml = checker.LIVEAREA_TEMPLATE
    for old, new in ((b'psla:-config', b'placeholder'),
                     (b'frame4.png', b'startup.png'),
                     (b'style="a3"', b'style="a5"'),
                     (b'width="128"', b'width="256"')):
        changed = resources.copy()
        assert old in changed[xml]
        changed[xml] = changed[xml].replace(old, new)
        rejected(changed)
    gate = 'sce_sys/livearea/contents/frame4.png'
    changed = resources.copy()
    corrupted = bytearray(changed[gate])
    corrupted[-1] ^= 1
    changed[gate] = bytes(corrupted)
    rejected(changed)
    changed = resources.copy()
    rgba = bytearray(changed[gate])
    rgba[25] = 6
    struct.pack_into('>I', rgba, 29, zlib.crc32(rgba[12:29]) & 0xffffffff)
    changed[gate] = bytes(rgba)
    rejected(changed)
    changed = resources.copy()
    data, offset, opaque = changed[gate], 8, changed[gate][:8]
    while offset < len(data):
        size = struct.unpack_from('>I', data, offset)[0]
        end = offset + size + 12
        if data[offset+4:offset+8] != b'tRNS':
            opaque += data[offset:end]
        offset = end
    changed[gate] = opaque
    rejected(changed)
    for name in checker.SHADERS:
        source = (ROOT/name).read_text()
        assert 'main(' in source and 'TEXCOORD' in source
    assert 'uInputSize' in (ROOT/'shaders/2_FXAA_v.cg').read_text()
    assert 'uOutputSize' in (ROOT/'shaders/2_FXAA_f.cg').read_text()
    assert 'companion.bin' in checker.REQUIRED
    print(f'bundle resources: Standard-Republic a3 layout, PNG8/alpha, Configurator, '
          f'shaders and notices pass; {failures_checked} invalid LiveArea bundles rejected')

if __name__ == '__main__':
    main()
