#!/usr/bin/env python3
"""Check the complete source-side application bundle before cross-compiling."""
import importlib.util
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def main():
    spec = importlib.util.spec_from_file_location('release_check', ROOT/'tools/check_release.py')
    checker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checker)
    for name in (*checker.FONTS, *checker.NOTICES, *checker.ARTWORK, *checker.SHADERS):
        path = ROOT/name
        assert path.is_file() and path.stat().st_size > 0, name
    for name, dimensions in (
        ('sce_sys/icon0.png', (128,128)),
        ('sce_sys/livearea/contents/bg0.png', (840,500)),
        ('sce_sys/livearea/contents/startup.png', (280,158)),
        ('sce_sys/livearea/contents/config.png', (256,64)),
    ):
        data = (ROOT/name).read_bytes()
        assert data[:8] == b'\x89PNG\r\n\x1a\n'
        assert struct.unpack('>II', data[16:24]) == dimensions
        assert data[24:26] == bytes([8,3]), 'LiveArea assets must be indexed PNG8'
    root = ET.parse(ROOT/'sce_sys/livearea/contents/template.xml').getroot()
    assert root.find('.//target').text == 'psla:-config'
    for node in root.iter():
        if node.tag in ('image','startup-image','background'):
            assert (ROOT/'sce_sys/livearea/contents'/node.text).is_file()
    for name in checker.SHADERS:
        source = (ROOT/name).read_text()
        assert 'main(' in source and 'TEXCOORD' in source
    assert 'uInputSize' in (ROOT/'shaders/2_FXAA_v.cg').read_text()
    assert 'uOutputSize' in (ROOT/'shaders/2_FXAA_f.cg').read_text()
    assert 'companion.bin' in checker.REQUIRED
    print('bundle resources: LiveArea/config target, all shader pairs and notices present')

if __name__ == '__main__':
    main()
