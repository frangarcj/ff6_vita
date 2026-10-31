#!/usr/bin/env python3
"""Check user-supplied FF6 Android 2.1.6 / 2.1.7 data without modifying any files."""
import argparse
import hashlib
from pathlib import Path
import sys

# Both Android releases use the same OBB; only the ARMv7 library differs.
SUPPORTED = {
    "so": {
        "2.1.6": (22340224, "f612f711330238cf405a403fb084abd2b8d7c9e72f9867740aed2b834fc731e6"),
        "2.1.7": (22468868, "7cd94bbba3f13987ba1e3f2a3c57db6a1abf360f0ff31a76bac945601a70bcd3"),
    },
    "obb": {
        "any": (654927616, "4f443b8d0be0a7976f8dd540cccb24a643937afd6eec043e7bf7d13fe427369f"),
    },
}


def verify(path, kind):
    try:
        actual_size = path.stat().st_size
        builds = {v: e for v, e in SUPPORTED[kind].items() if e[0] == actual_size}
        if not path.is_file() or not builds:
            sizes = ", ".join(str(e[0]) for e in SUPPORTED[kind].values())
            print(f"FAIL {path}: expected {sizes} bytes, found {actual_size}", file=sys.stderr)
            return False
        version, (size, expected) = next(iter(builds.items()))
        digest = hashlib.sha256()
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
        actual = digest.hexdigest()
        if actual != expected:
            print(f"FAIL {path}: unsupported SHA-256 {actual}", file=sys.stderr)
            return False
        label = f" (Android {version})" if kind == "so" else ""
        print(f"OK {path.name}{label}: {size} bytes, SHA-256 {actual}")
        return True
    except OSError as error:
        print(f"FAIL {path}: {error}", file=sys.stderr)
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--so", type=Path, required=True, help="ARMv7 libFF6.so extracted from the 2.1.6 or 2.1.7 APK")
    parser.add_argument("--obb", type=Path, help="main.21701...obb, or the same file renamed main.obb")
    args = parser.parse_args()
    valid = verify(args.so, "so")
    if args.obb is not None:
        valid = verify(args.obb, "obb") and valid
    return 0 if valid else 1


if __name__ == "__main__":
    sys.exit(main())
