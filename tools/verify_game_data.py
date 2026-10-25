#!/usr/bin/env python3
"""Check user-supplied FF6 Android 2.1.6 data without modifying any files."""
import argparse
import hashlib
from pathlib import Path
import sys

SUPPORTED = {
    "so": (22340224, "f612f711330238cf405a403fb084abd2b8d7c9e72f9867740aed2b834fc731e6"),
    "obb": (654927616, "4f443b8d0be0a7976f8dd540cccb24a643937afd6eec043e7bf7d13fe427369f"),
}


def verify(path, kind):
    size, expected = SUPPORTED[kind]
    try:
        actual_size = path.stat().st_size
        if not path.is_file() or actual_size != size:
            print(f"FAIL {path}: expected {size} bytes, found {actual_size}", file=sys.stderr)
            return False
        digest = hashlib.sha256()
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
        actual = digest.hexdigest()
        if actual != expected:
            print(f"FAIL {path}: unsupported SHA-256 {actual}", file=sys.stderr)
            return False
        print(f"OK {path.name}: {size} bytes, SHA-256 {actual}")
        return True
    except OSError as error:
        print(f"FAIL {path}: {error}", file=sys.stderr)
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--so", type=Path, required=True, help="ARMv7 libFF6.so extracted from the APK")
    parser.add_argument("--obb", type=Path, help="main.21701...obb, or the same file renamed main.obb")
    args = parser.parse_args()
    valid = verify(args.so, "so")
    if args.obb is not None:
        valid = verify(args.obb, "obb") and valid
    return 0 if valid else 1


if __name__ == "__main__":
    sys.exit(main())
