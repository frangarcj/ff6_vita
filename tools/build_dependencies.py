#!/usr/bin/env python3
"""Build pinned source dependencies in a fresh directory.

Run in an isolated VitaSDK (CI uses the official SDK container): OpenSL ES and
ImGui install into that SDK. The patched vitaGL stays project-local.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def run(*args, timeout=900):
    subprocess.run(args, cwd=ROOT, check=True, timeout=timeout)


def checkout(destination, repository, revision):
    if not re.fullmatch(r'https://github.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('Unexpected dependency repository')
    if not re.fullmatch(r'[0-9a-f]{40}', revision):
        raise ValueError('Dependency revision must be a complete commit ID')
    if destination.exists():
        raise RuntimeError(f'Refusing to replace existing dependency checkout: {destination}')
    destination.mkdir(parents=True)
    run('git', 'init', '--quiet', str(destination))
    run('git', '-C', str(destination), 'remote', 'add', 'origin', repository)
    run('git', '-C', str(destination), 'fetch', '--depth=1', 'origin', revision)
    run('git', '-C', str(destination), 'checkout', '--detach', 'FETCH_HEAD')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=ROOT/'build_ci/deps')
    parser.add_argument('--jobs', type=int, default=min(4, os.cpu_count() or 2))
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32:
        raise ValueError('jobs must be between 1 and 32')
    sdk = Path(os.environ['VITASDK']).resolve()
    if not (sdk/'bin/arm-vita-eabi-gcc').is_file():
        raise RuntimeError('VITASDK must point to a complete native-host SDK')
    os.environ['PATH'] = str(sdk/'bin')+os.pathsep+os.environ['PATH']
    destination = args.directory.resolve()
    lock = json.loads((ROOT/'deps/build.lock.json').read_text())
    graphics = json.loads((ROOT/'deps/vitagl.lock.json').read_text())
    patch = ROOT/graphics['patch']
    if hashlib.sha256(patch.read_bytes()).hexdigest() != graphics['patch_sha256']:
        raise RuntimeError('FF6 graphics patch checksum does not match its lock')
    for name in ('opensles', 'imgui'):
        entry = lock[name]
        checkout(destination/name, entry['repository'], entry['commit'])
    checkout(destination/'vitaGL', graphics['repository'], graphics['base_commit'])
    run('git', '-C', str(destination/'vitaGL'), 'apply', '--check', str(patch))
    run('git', '-C', str(destination/'vitaGL'), 'apply', str(patch))
    jobs = f'-j{args.jobs}'
    run('make', '-C', str(destination/'opensles/libopensles'), jobs, 'install')
    run('make', '-C', str(destination/'imgui'), jobs, 'install')
    flags = [f'{key}={value}' for key, value in graphics['make_options'].items()]
    run('make', '-C', str(destination/'vitaGL'), jobs, *flags)
    print(f'VITAGL_INCLUDE_DIR={destination}/vitaGL/source')
    print(f'VITAGL_LIBRARY={destination}/vitaGL/libvitaGL.a')

if __name__ == '__main__':
    main()
