#!/usr/bin/env python3
"""Validate a local FF6 build and optionally stage its release artifacts.

Usage: python3 tools/check_release.py build_release --output release
This checks packaging and build provenance; it does not validate gameplay.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile


FONTS = (
    "NotoSansJP-Regular.ttf",
    "NotoSansKR-Regular.ttf",
    "NotoSansSC-Regular.ttf",
)
NOTICES = ("LICENSE", "THIRD_PARTY_NOTICES.md", "licenses/OFL-Noto.txt", "licenses/MIT-ImGui.txt")
ARTWORK = (
    "sce_sys/icon0.png",
    "sce_sys/livearea/contents/bg0.png",
    "sce_sys/livearea/contents/startup.png",
    "sce_sys/livearea/contents/config.png",
    "sce_sys/livearea/contents/template.xml",
)
SHADERS = tuple("shaders/" + name + suffix for name in (
    "1_Negative", "2_FXAA", "3_Sepia", "4_Greyscale", "5_CRT",
    "6_CRT-Bleeding", "7_CRT-Bleeding-Flat") for suffix in ("_v.cg", "_f.cg"))
REQUIRED = {"eboot.bin", "companion.bin", "sce_sys/param.sfo",
            *FONTS, *NOTICES, *ARTWORK, *SHADERS}
RELEASE_MARKER = b"FF6_BUILD_PROFILE=release\0"
DEBUG_MARKER = b"FF6_BUILD_PROFILE=debug\0"
DIAGNOSTIC_MARKERS = (
    b"ux0:data/ff6/boot.log",
    b"ux0:data/ff6/upscale-status.txt",
    b"ux0:data/ff6/autoplay.trigger",
    b"ux0:data/ff6/autoplay-stop-battle.trigger",
    b"ux0:data/ff6/save.trigger",
    b"ux0:data/ff6/dump.trigger",
    b"[frame-time]",
    b"holding 15s",
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def cache_values(path):
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.split(":", 1)[0]] = value
    return values


def source_state(source):
    try:
        revision = subprocess.run(
            ["git", "-C", str(source), "rev-parse", "--verify", "HEAD"],
            capture_output=True, text=True, timeout=10, check=True)
        status = subprocess.run(
            ["git", "-C", str(source), "status", "--porcelain=v1",
             "--untracked-files=normal"],
            capture_output=True, text=True, timeout=10, check=True)
        return revision.stdout.strip(), bool(status.stdout)
    except (OSError, subprocess.SubprocessError):
        # A source archive can still be checked without a Git checkout.
        return None, None


def linked_archives(build, cache, elf, target="FF6.elf"):
    link = build / ("CMakeFiles/" + target + ".dir/link.txt")
    require(link.is_file() and link.stat().st_size <= 65536,
            "Missing or oversized CMake link.txt; use the documented Makefiles build")
    command = shlex.split(link.read_text(encoding="utf-8"))
    compiler = Path(cache["CMAKE_CXX_COMPILER"]).resolve()
    require(command and Path(command[0]).resolve() == compiler,
            "Link command compiler differs from the validated build cache; rebuild")
    require("-mfloat-abi=hard" in command and not any(
        arg.startswith(("-L", "-B", "--sysroot", "-specs", "@")) for arg in command[1:]),
        "Custom library search flags need explicit archive paths for provenance")
    archives = []
    inputs = dict.fromkeys(arg for arg in command[1:]
                           if arg.startswith("-l") or arg.endswith(".a"))
    require(len(inputs) <= 128, "Unexpected number of linked archives")
    for argument in inputs:
        if argument.startswith("-l"):
            name = argument[2:]
            require(re.fullmatch(r"[A-Za-z0-9_+.-]+", name),
                    "Unsupported library argument; use an explicit archive: " + argument)
            result = subprocess.run(
                [str(compiler), "-mfloat-abi=hard", "-print-file-name=lib" + name + ".a"],
                capture_output=True, text=True, check=True, timeout=10)
            path = Path(result.stdout.strip())
            require(path.is_absolute() and path.is_file(),
                    "Cannot resolve linked archive " + argument + "; restore the build SDK")
        else:
            path = Path(argument)
            if not path.is_absolute():
                path = build / path
        require(path.is_file(), "Missing linked archive: " + Path(argument).name)
        require(path.stat().st_mtime_ns <= elf.stat().st_mtime_ns,
                "Linked archive is newer than FF6.elf; rebuild: " + path.name)
        archives.append({"input": argument if argument.startswith("-l") else path.name,
                         "file": path.name, "sha256": sha256(path)})
    return {"command_sha256": sha256(link), "archives": archives,
            "scope": "Archives named on the CMake link command; compiler-added runtime defaults are not enumerated."}


def read_sfo(data):
    require(20 <= len(data) <= 65536, "Invalid param.sfo size")
    magic, version, keys, values, count = struct.unpack_from("<4sIIII", data)
    require(magic == b"\0PSF" and version == 0x101, "Invalid param.sfo header")
    require(count <= 128 and 20 + 16 * count <= keys <= values <= len(data),
            "Invalid param.sfo tables")
    result = {}
    for index in range(count):
        key_offset, fmt, length, capacity, value_offset = struct.unpack_from(
            "<HHIII", data, 20 + 16 * index)
        key_start = keys + key_offset
        key_end = data.find(b"\0", key_start, values)
        require(keys <= key_start < key_end < values, "Invalid param.sfo key")
        key = data[key_start:key_end].decode("ascii")
        require(key not in result, "Duplicate param.sfo key: " + key)
        start = values + value_offset
        require(length <= capacity and values <= start <= len(data)
                and capacity <= len(data) - start, "Invalid param.sfo value")
        raw = data[start:start + length]
        if fmt == 0x0204:
            require(raw.endswith(b"\0"), "Unterminated param.sfo string")
            result[key] = raw[:-1].decode("utf-8")
        elif fmt == 0x0404:
            require(length == 4, "Invalid param.sfo integer")
            result[key] = struct.unpack("<I", raw)[0]
        else:
            result[key] = raw.hex()
    return result


def inspect_elf(path):
    data = path.read_bytes()
    require(len(data) >= 52 and data[:6] == b"\x7fELF\x01\x01",
            "FF6.elf must be a 32-bit little-endian ELF")
    header = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    require(header[1] == 40, "FF6.elf is not an ARM binary")
    require(RELEASE_MARKER in data and DEBUG_MARKER not in data,
            "FF6.elf is instrumented or lacks the release build marker; rebuild")
    for marker in DIAGNOSTIC_MARKERS:
        require(marker not in data,
                "FF6.elf contains a development diagnostic: " + marker.decode("ascii"))
    section_offset, entry_size, count, names_index = (
        header[5], header[10], header[11], header[12])
    require(entry_size == 40 and 0 < names_index < count
            and section_offset + count * entry_size <= len(data),
            "FF6.elf lacks a valid section table")
    names_header = struct.unpack_from(
        "<IIIIIIIIII", data, section_offset + names_index * entry_size)
    names_start, names_size = names_header[4], names_header[5]
    require(names_start + names_size <= len(data), "Invalid ELF section names")
    names = data[names_start:names_start + names_size]
    sections = set()
    for index in range(count):
        name_offset = struct.unpack_from(
            "<I", data, section_offset + index * entry_size)[0]
        end = names.find(b"\0", name_offset)
        require(0 <= name_offset <= end, "Invalid ELF section name")
        sections.add(names[name_offset:end].decode("ascii"))
    require(".symtab" in sections and ".debug_info" in sections,
            "Retain the unstripped FF6.elf with debug symbols for crash reports")
    return {"machine": "ARM", "class": 32, "entry": hex(header[3]),
            "flags": hex(header[6]), "debug_symbols": True}


def validate(build):
    source = Path(__file__).resolve().parents[1]
    cache = cache_values(build / "CMakeCache.txt")
    require(cache.get("FF6_DEBUG", "").upper() in ("OFF", "FALSE", "0", "NO"),
            "FF6_DEBUG must be explicitly OFF in this build")
    require(cache.get("CMAKE_BUILD_TYPE") in ("Release", "RelWithDebInfo", "MinSizeRel"),
            "Use an optimized CMAKE_BUILD_TYPE for the release candidate")
    require(Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve() == source,
            "Build cache belongs to another checkout; rebuild from this source tree")

    vpk, elf, eboot = (build / name for name in ("FF6.vpk", "FF6.elf", "eboot.bin"))
    for path in (vpk, elf, eboot):
        require(path.is_file(), "Missing build artifact: " + str(path))
    require(elf.stat().st_mtime_ns <= eboot.stat().st_mtime_ns
            <= vpk.stat().st_mtime_ns,
            "Package or eboot is older than its input; finish the CMake build")
    elf_info = inspect_elf(elf)
    companion = build / "companion.bin"
    companion_elf = build / "companion.elf"
    require(companion.is_file() and companion_elf.is_file(),
            "Missing Configurator binary or matching ELF")
    require(companion_elf.stat().st_mtime_ns <= companion.stat().st_mtime_ns
            <= vpk.stat().st_mtime_ns, "Rebuild the Configurator and VPK")
    companion_info = inspect_elf(companion_elf)

    contents = []
    with zipfile.ZipFile(vpk) as archive:
        entries = archive.infolist()
        require(len(entries) <= 128, "Unexpected number of VPK entries")
        require(sum(item.file_size for item in entries) <= 256 * 1024 * 1024,
                "Unexpected VPK uncompressed size")
        names = [item.filename for item in entries]
        require(len(names) == len(set(names)), "Duplicate VPK entry names")
        for item in entries:
            name = item.filename
            relative = PurePosixPath(name)
            require(not relative.is_absolute() and ".." not in relative.parts
                    and "\\" not in name, "Unsafe VPK entry: " + name)
            require(not name.lower().endswith((".so", ".obb", ".apk", ".sav")),
                    "Commercial game data or saved games must not be packaged: " + name)
            if item.is_dir():
                continue
            extra_license = (name.startswith("licenses/")
                             and relative.suffix in (".txt", ".md"))
            require(name in REQUIRED or name in ARTWORK or extra_license,
                    "Unexpected VPK file: " + name)
        require(REQUIRED.issubset(names),
                "Missing VPK files: " + ", ".join(sorted(REQUIRED - set(names))))
        require(archive.testzip() is None, "VPK CRC check failed")
        sfo = read_sfo(archive.read("sce_sys/param.sfo"))
        require(sfo.get("TITLE_ID") == "FF6000000", "Incorrect VPK TITLE_ID")
        require(sfo.get("TITLE") == "Final Fantasy VI", "Incorrect VPK title")
        require(re.fullmatch(r"\d{2}\.\d{2}", str(sfo.get("APP_VER", ""))),
                "Incorrect VPK APP_VER format")
        require(sfo.get("ATTRIBUTE2") == 12, "Incorrect VPK ATTRIBUTE2")
        for item in entries:
            if item.is_dir():
                continue
            digest = hashlib.sha256(archive.read(item.filename)).hexdigest()
            contents.append({"path": item.filename, "bytes": item.file_size,
                             "sha256": digest})
            if item.filename == "eboot.bin":
                require(digest == sha256(eboot), "VPK contains a different eboot.bin")
            elif item.filename == "companion.bin":
                require(digest == sha256(companion), "VPK contains a different companion.bin")
            elif item.filename in FONTS + NOTICES + ARTWORK + SHADERS or item.filename.startswith("licenses/"):
                original = source / item.filename
                require(original.is_file() and digest == sha256(original),
                        "VPK resource differs from source: " + item.filename)

    dependency = {}
    for key, label in (("VITAGL_LIBRARY", "vitagl_archive"),
                       ("CMAKE_C_COMPILER", "c_compiler"),
                       ("CMAKE_CXX_COMPILER", "cxx_compiler")):
        path = Path(cache.get(key, ""))
        require(path.is_file(), "Cannot fingerprint configured dependency: " + key)
        require(path.stat().st_mtime_ns <= elf.stat().st_mtime_ns,
                "Configured dependency is newer than FF6.elf; rebuild: " + key)
        dependency[label] = {"file": path.name, "sha256": sha256(path)}
        if label in ("c_compiler", "cxx_compiler"):
            version = subprocess.run([str(path), "--version"], capture_output=True,
                                     text=True, check=True, timeout=10)
            require(version.stdout.strip(), "Compiler version output was empty")
            dependency[label]["version"] = version.stdout.splitlines()[0][:240]
    header = Path(cache.get("VITAGL_INCLUDE_DIR", "")) / "vitaGL.h"
    require(header.is_file(), "Cannot fingerprint configured vitaGL header")
    require(header.stat().st_mtime_ns <= elf.stat().st_mtime_ns,
            "Configured vitaGL header is newer than FF6.elf; rebuild")
    dependency["vitagl_header"] = {"file": header.name, "sha256": sha256(header)}
    revision, dirty = source_state(source)
    dependency_sources = [
        {"file": name, "sha256": sha256(source / name)}
        for name in ("deps/vitagl-ff6.patch", "deps/vitagl.lock.json", "deps/build.lock.json")
        if (source / name).is_file()
    ]
    manifest = {
        "schema_version": 1,
        "scope": "Build and packaging checks only; hardware and gameplay require separate validation.",
        "title_id": sfo["TITLE_ID"],
        "title": sfo["TITLE"],
        "app_version": sfo["APP_VER"],
        "build_type": cache["CMAKE_BUILD_TYPE"],
        "ff6_debug": False,
        "source_revision": revision,
        "source_dirty": dirty,
        "source_provenance_scope": "Checkout state at validation, not an embedded build revision. Rebuild after changing source.",
        "elf": elf_info,
        "companion_elf": companion_info,
        "companion_sha256": sha256(companion),
        "artifacts": [
            {"file": path.name, "bytes": path.stat().st_size, "sha256": sha256(path)}
            for path in (vpk, elf, companion_elf)
        ],
        "eboot_sha256": sha256(eboot),
        "dependencies": dependency,
        "link": linked_archives(build, cache, elf),
        "companion_link": linked_archives(build, cache, companion_elf, "companion.elf"),
        "dependency_sources": dependency_sources,
        "vpk_contents": sorted(contents, key=lambda item: item["path"]),
        "missing_optional_artwork": sorted(set(ARTWORK) - set(names)),
    }
    return manifest


def atomic_write(path, data=None, source=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            if source is None:
                stream.write(data)
            else:
                with source.open("rb") as original:
                    shutil.copyfileobj(original, stream)
        os.replace(temporary, path)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", nargs="?", type=Path, default=Path("build_release"))
    parser.add_argument("--output", type=Path,
                        help="Stage the VPK, matching game/Configurator ELFs, manifest and checksums")
    parser.add_argument("--require-clean-source", action="store_true",
                        help="Reject unknown/dirty Git provenance (required for public CI releases)")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    try:
        manifest = validate(build)
        if args.require_clean_source:
            require(isinstance(manifest["source_revision"], str) and
                    re.fullmatch(r"[0-9a-f]{40}", manifest["source_revision"]) and
                    manifest["source_dirty"] is False,
                    "A public release requires a known, clean Git revision")
        if args.output is not None:
            output = args.output.resolve()
            require(output != build, "Output directory must differ from the build directory")
            for artifact in manifest["artifacts"]:
                name = artifact["file"]
                atomic_write(output / name, source=build / name)
                require(sha256(output / name) == artifact["sha256"],
                        "Build changed during staging; rebuild and retry")
            encoded = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
            atomic_write(output / "manifest.json", data=encoded)
            sums = "".join(sha256(output / name) + "  " + name + "\n"
                           for name in ("FF6.vpk", "FF6.elf", "companion.elf", "manifest.json"))
            atomic_write(output / "SHA256SUMS", data=sums.encode())
            print("Staged verified artifacts in " + str(output))
        print("PASS: " + manifest["title_id"] + " " + manifest["app_version"]
              + ", instrumentation disabled, package and matching build eboot verified.")
        if manifest["missing_optional_artwork"]:
            print("Optional LiveArea artwork missing: "
                  + ", ".join(manifest["missing_optional_artwork"]))
        print("Hardware and gameplay validation remain separate release gates.")
        return 0
    except (OSError, ValueError, KeyError, struct.error, zipfile.BadZipFile,
            subprocess.SubprocessError,
            UnicodeError) as error:
        print("Release check failed: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
