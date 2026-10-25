#!/usr/bin/env python3
"""Run portable regressions without VitaSDK or any commercial game data."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT/'build_host_tests')
    parser.add_argument('--no-sanitizers', action='store_true')
    args = parser.parse_args()
    output = args.build_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which(os.environ.get('HOST_CC', 'clang'))
    if not compiler:
        raise RuntimeError('Install clang or set HOST_CC to a host C compiler')
    flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-g']
    if not args.no_sanitizers:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    suites = ['text_encoding', 'file_compat', 'glyph_upload', 'glyph_cell_reuse',
              'sharp_bilinear', 'scene_clip', 'options', 'font_raster', 'font_alignment']
    for name in suites:
        sources = [str(ROOT/f'tests/test_{name}.c')]
        if name in ('font_raster', 'font_alignment'):
            sources.append(str(ROOT/'loader/stb_truetype.c'))
        binary = output/f'test_{name}'
        subprocess.run([compiler, *flags, *sources, '-lm', '-o', str(binary)],
                       cwd=ROOT, check=True, timeout=90)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=90)
    for script in ('test_frame_present.py', 'test_package_assets.py'):
        subprocess.run([sys.executable, str(ROOT/'tests'/script)],
                       cwd=ROOT, check=True, timeout=120)
    print(f'PASS: {len(suites)} C suites and presentation/resource regressions')

if __name__ == '__main__':
    main()
