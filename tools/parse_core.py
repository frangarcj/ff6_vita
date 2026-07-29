#!/usr/bin/env python3
"""Read a Vita crash dump and say where the process died.

    tools/parse_core.py psp2core-*.psp2dmp build/FF6.elf

The .psp2dmp files the Vita leaves in ux0:data are gzipped ELF cores carrying
a set of Sony-specific notes. This reads the ones that matter: which thread
took the fault, its registers, and a stack walk resolved against the homebrew
ELF, correcting for the fact that the module is not loaded at its link
address.

vita-parse-core does more (semaphores, memory blocks, a disassembly window)
but is Python 2 and needs patching to run at all. This is dependency-free and
answers the first question you have after a crash. Reach for the other one
when you need the extra detail.
"""

import gzip
import os
import struct
import subprocess
import sys

# Thread stop reasons seen in the wild; the low half is the ARM abort kind.
STOP_REASONS = {
    0x00000: "no reason (thread was not the one that died)",
    0x30002: "undefined instruction",
    0x30003: "prefetch abort (executed a bad address)",
    0x30004: "data abort (read or wrote a bad address)",
    0x30005: "alignment fault",
    0x60080: "division by zero",
}


def read_core(path):
    with open(path, "rb") as fh:
        head = fh.read(2)
        fh.seek(0)
        return gzip.open(path, "rb").read() if head == b"\x1f\x8b" else fh.read()


def parse_notes(core):
    """Return {note_name: payload} plus the PT_LOAD table."""
    phoff = struct.unpack_from("<I", core, 28)[0]
    phentsize, phnum = struct.unpack_from("<HH", core, 42)

    notes, loads = {}, []
    for i in range(phnum):
        off = phoff + i * phentsize
        ptype, poff, vaddr, _paddr, filesz, _memsz, _flags, _align = struct.unpack_from(
            "<8I", core, off
        )
        if ptype == 1:  # PT_LOAD
            loads.append((vaddr, poff, filesz))
            continue
        if ptype != 4:  # PT_NOTE
            continue

        pos, end = poff, poff + filesz
        while pos + 12 <= end:
            nsz, dsz, _ntype = struct.unpack_from("<3I", core, pos)
            if nsz == 0 and dsz == 0:
                break
            name = core[pos + 12 : pos + 12 + nsz].split(b"\0")[0].decode("ascii", "replace")
            start = pos + 12 + (nsz + 3) // 4 * 4
            notes[name] = core[start : start + dsz]
            pos = start + (dsz + 3) // 4 * 4

    return notes, loads


def entries(note):
    """Walk a note table.

    The layout is not a fixed stride: word 1 is the entry count, the entries
    begin at offset 8, and each one starts with its own size. Assuming a
    constant stride happens to work for the first few entries and then quietly
    drops the rest -- including, in a crash dump, the thread that faulted.
    """
    if len(note) < 8:
        return
    count = struct.unpack_from("<I", note, 4)[0]
    off = 8
    for _ in range(count):
        if off + 4 > len(note):
            return
        size = struct.unpack_from("<I", note, off)[0]
        if size < 8 or off + size > len(note):
            return
        yield note[off : off + size]
        off += size


def find_module(notes, want):
    """Locate a module by name and return (name, text_base, text_size).

    MODULE_INFO does not follow the count/entry_size shape the other tables
    use, so rather than guess a stride, find the name and take the first
    (base, size) pair that follows it and looks like a loaded code segment.
    """
    note = notes.get("MODULE_INFO", b"")
    lowered = want.lower().encode("ascii", "replace")
    pos = note.lower().find(lowered)
    if pos < 0:
        return None, None, None

    for off in range(pos, min(pos + 256, len(note) - 8), 4):
        base, size = struct.unpack_from("<2I", note, off & ~3)
        if 0x81000000 <= base < 0x82000000 and 0x1000 < size < 0x2000000:
            name = note[pos:].split(b"\0")[0].decode("ascii", "replace")
            return name, base, size
    return None, None, None


def elf_link_base(elf):
    out = subprocess.run(
        ["arm-vita-eabi-readelf", "-S", elf], capture_output=True, text=True
    ).stdout
    for line in out.splitlines():
        if " .text " in line:
            for tok in line.split():
                if len(tok) == 8 and tok.startswith("81"):
                    return int(tok, 16) & ~0xFFF
    return 0x81000000


def symbolize(elf, addrs):
    if not addrs:
        return {}
    out = subprocess.run(
        ["arm-vita-eabi-addr2line", "-f", "-C", "-e", elf] + ["0x%x" % a for a in addrs],
        capture_output=True,
        text=True,
    ).stdout.splitlines()
    resolved = {}
    for i, addr in enumerate(addrs):
        func = out[2 * i].strip() if 2 * i < len(out) else "??"
        loc = out[2 * i + 1].strip() if 2 * i + 1 < len(out) else "??"
        if func != "??":
            resolved[addr] = "%s  (%s)" % (func, loc)
    return resolved


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: parse_core.py <core.psp2dmp> <homebrew.elf>")
    core_path, elf = sys.argv[1], sys.argv[2]

    core = read_core(core_path)
    notes, loads = parse_notes(core)

    modname, mod_base, mod_size = find_module(notes, os.path.basename(elf).split(".")[0])
    if mod_base is None:
        modname, mod_base, mod_size = find_module(notes, "elf")
    if mod_base is None:
        sys.exit("could not find the homebrew module in MODULE_INFO")

    reloc = mod_base - elf_link_base(elf)
    print("module %s loaded at 0x%08x (size 0x%x), relocated by +0x%x"
          % (modname, mod_base, mod_size, reloc))

    # THREAD_INFO carries the stop reason and the pc; the field offsets are
    # the ones vita-parse-core established.
    faulted = []
    print("\n=== threads ===")
    for blob in entries(notes.get("THREAD_INFO", b"")):
        if len(blob) < 0xA0:
            continue
        uid = struct.unpack_from("<I", blob, 4)[0]
        name = blob[8:8 + 32].split(b"\0")[0].decode("ascii", "replace")
        stop = struct.unpack_from("<I", blob, 0x74)[0]
        pc = struct.unpack_from("<I", blob, 0x9C)[0]
        mark = ""
        if stop:
            mark = "  <== %s" % STOP_REASONS.get(stop, "stop reason 0x%x" % stop)
            faulted.append((uid, name, pc))
        print("  0x%08x %-26s pc=0x%08x%s" % (uid, name, pc, mark))

    for uid, name, pc in faulted:
        print("\n=== crashed: %s <0x%08x> ===" % (name, uid))
        sym = symbolize(elf, [pc - reloc])
        print("  pc 0x%08x -> %s" % (pc, sym.get(pc - reloc, "outside the homebrew")))

        # Registers for that thread, if THREAD_REG_INFO carries it.
        for blob in entries(notes.get("THREAD_REG_INFO", b"")):
            w = struct.unpack_from("<%dI" % (len(blob) // 4), blob, 0)
            if len(w) < 20 or w[1] != uid:
                continue
            for i in range(13):
                print("  r%-3d 0x%08x" % (i, w[i + 2]))
            print("  sp   0x%08x" % w[15])
            print("  lr   0x%08x" % w[16])

    # Stack walk: every word on a stack page that lands inside the module.
    print("\n=== call sites found on the stacks ===")
    hits = []
    for vaddr, poff, filesz in loads:
        if not 0x8D000000 <= vaddr < 0x8F000000:
            continue
        for o in range(0, filesz - 3, 4):
            v = struct.unpack_from("<I", core, poff + o)[0] & ~1
            if mod_base <= v < mod_base + mod_size:
                hits.append(v - reloc)

    seen, ordered = set(), []
    for a in hits:
        if a not in seen:
            seen.add(a)
            ordered.append(a)

    for addr, text in symbolize(elf, ordered).items():
        if "/loader/" in text or "vitaGL" in text:
            print("  0x%08x  %s" % (addr, text))


if __name__ == "__main__":
    main()
