#!/usr/bin/env python3
"""Disassemble any VA range of Whacked! Default.xbe, resolving call/jmp targets
into their owning section so cross-section calls are obvious.

Usage:
    python tools/xbe_disasm.py 0x1FA946            # 24 instructions
    python tools/xbe_disasm.py 0x1FA946 60         # 60 instructions
    python tools/xbe_disasm.py 0x1D9F05 -n 20 --comment-map
"""
import argparse
import struct
import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

ROOT = Path(__file__).resolve().parent.parent
XBE_PATH = ROOT / "build_work" / "extracted_meta" / "Default.xbe"

# (va, vsize, raw_off, name, flags)
SECS = []


def load():
    xbe = XBE_PATH.read_bytes()
    base = struct.unpack_from("<I", xbe, 0x104)[0]
    nsec = struct.unpack_from("<I", xbe, 0x11C)[0]
    shva = struct.unpack_from("<I", xbe, 0x120)[0]
    SECS.clear()
    for i in range(nsec):
        o = shva - base + i * 56
        flags, va, vsz, roff, rsz, nva = struct.unpack_from("<6I", xbe, o)
        name = xbe[nva - base:nva - base + 32].split(b"\x00", 1)[0].decode("ascii", "replace")
        SECS.append((va, vsz, roff, rsz, name, flags))
    return xbe


def section_of(va):
    for va0, vsz, roff, rsz, name, flags in SECS:
        if va0 <= va < va0 + max(vsz, rsz):
            return name, va0, roff, rsz, flags
    return None


def read(rva, n):
    s = section_of(rva)
    if not s:
        return None
    name, va0, roff, rsz, flags = s
    off = roff + (rva - va0)
    return XBE[off:off + n]


def cstr(rva, maxlen=96):
    b = read(rva, maxlen)
    if not b:
        return None
    return b.split(b"\x00", 1)[0].decode("latin1", "replace")


def is_exec(flags):
    return bool(flags & 0x04)


def main():
    global XBE
    ap = argparse.ArgumentParser()
    ap.add_argument("va", help="start VA, e.g. 0x1FA946")
    ap.add_argument("count", nargs="?", type=int, default=30, help="instruction count")
    ap.add_argument("-n", "--num", type=int, default=None, help="instruction count (alias)")
    ap.add_argument("--bytes", action="store_true", help="also dump raw bytes")
    ap.add_argument("--noreloc", action="store_true", help="don't annotate absolute addresses")
    args = ap.parse_args()

    XBE = load()
    va = int(args.va, 16)
    n = args.num or args.count
    code = read(va, n * 16 + 32)
    if code is None:
        sys.exit(f"VA 0x{va:08X} is not inside any section")

    s = section_of(va)
    print(f"# 0x{va:08X}  section={s[0]}  flags=0x{s[4]:08X}  exec={is_exec(s[4])}")

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    shown = 0
    for ins in md.disasm(code, va):
        note = ""
        if not args.noreloc:
            for op in ins.operands:
                if op.type == X86_OP_IMM and ins.mnemonic in ("call", "jmp", "jcc") or \
                   (op.type == X86_OP_IMM and ins.mnemonic.startswith("j")):
                    t = op.imm & 0xFFFFFFFF
                    ts = section_of(t)
                    if ts:
                        note += f"   ; -> {ts[0]}" + ("" if is_exec(ts[4]) else " (DATA!)")
                    break
            # absolute memory operand that points into a known section -> maybe a vtable/global
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and op.mem.disp:
                    d = op.mem.disp & 0xFFFFFFFF
                    ds = section_of(d)
                    if ds:
                        st = cstr(d, 24)
                        note += f"   ; [{ds[0]}+0x{d - ds[1]:X}]"
                        if st and st.isprintable() and len(st) > 3:
                            note += f' = "{st}"'
                    break
        raw = " ".join(f"{b:02x}" for b in ins.bytes)
        print(f"0x{ins.address:08X}  {raw:<24} {ins.mnemonic:<8} {ins.op_str}{note}")
        shown += 1
        if shown >= n:
            break


if __name__ == "__main__":
    main()
