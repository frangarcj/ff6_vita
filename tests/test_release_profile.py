#!/usr/bin/env python3
"""Check diagnostic rejection using copies of a real, unstripped Release ELF.

Run after building: python3 tests/test_release_profile.py build_release/FF6.elf
The ELF and all real build artifacts remain unchanged.
"""
import argparse
import importlib.util
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("check_release", ROOT / "tools/check_release.py")
    if spec is None or spec.loader is None:
        raise RuntimeError("Unable to load the release checker")
    checker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checker)
    info = checker.inspect_elf(args.elf)
    assert info["debug_symbols"], "Crash-report symbols must remain in the standalone ELF"
    original = args.elf.read_bytes()
    probes = [(checker.DEBUG_MARKER, "instrumented")]
    probes.extend((marker, "development diagnostic") for marker in checker.DIAGNOSTIC_MARKERS)
    with tempfile.TemporaryDirectory(prefix="ff6-release-profile-") as directory:
        fixture = Path(directory) / "fixture.elf"
        for marker, expected in probes:
            fixture.write_bytes(original + b"\0" + marker + b"\0")
            try:
                checker.inspect_elf(fixture)
            except ValueError as error:
                if expected not in str(error):
                    raise AssertionError(f"Wrong rejection for {marker!r}: {error}") from error
            else:
                raise AssertionError(f"Release checker accepted {marker!r}")
    assert args.elf.read_bytes() == original, "Real build ELF changed during the test"
    print(f"Release accepted with crash symbols; {len(probes)} diagnostic fixtures rejected")


if __name__ == "__main__":
    main()
