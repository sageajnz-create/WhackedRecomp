#!/usr/bin/env python3
"""
Static x86-to-C++20 Recompiler for Whacked! (Original Xbox Default.xbe).
Lifts .text game/engine/XVOICE/XAPILIB/LIBCMT code using Capstone + Ghidra CFG DB
and emits 32 partitioned C++ translation units + dispatch registration.
"""

import json
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import (
    X86_OP_REG, X86_OP_IMM, X86_OP_MEM,
    X86_REG_EAX, X86_REG_ECX, X86_REG_EDX, X86_REG_EBX,
    X86_REG_ESP, X86_REG_EBP, X86_REG_ESI, X86_REG_EDI,
    X86_REG_AX, X86_REG_CX, X86_REG_DX, X86_REG_BX,
    X86_REG_SP, X86_REG_BP, X86_REG_SI, X86_REG_DI,
    X86_REG_AL, X86_REG_CL, X86_REG_DL, X86_REG_BL,
    X86_REG_AH, X86_REG_CH, X86_REG_DH, X86_REG_BH,
    X86_REG_FS, X86_REG_ES, X86_REG_SS, X86_REG_DS, X86_REG_CS, X86_REG_GS,
    X86_REG_MM0, X86_REG_MM7,
    X86_REG_XMM0, X86_REG_XMM7,
    X86_REG_ST0, X86_REG_ST7,
)

ROOT = Path(__file__).resolve().parent.parent
XBE_PATH = ROOT / "build_work" / "extracted_meta" / "Default.xbe"
GHIDRA_DB_PATH = ROOT / "build_work" / "ghidra_recomp_db.json"
OUT_DIR = ROOT / "src" / "recomp"
NUM_SHARDS = 32

# Executable sections that get lifted to native C++.
# Hardware layers (D3D, XGRPH, DSOUND, WMADEC) are deliberately excluded: those
# are the real NV2A/DSP drivers and must stay behind the HLE layer, which needs
# emulated hardware.  Pure-software XDK libraries (XOnline, XNet, XPP, Bink) are
# lifted so indirect calls held in .data resolve to real code instead of aborting.
LIFT_SECTIONS = (
    ".text",
    "XONLINE",
    "XNET",
    "XPP",
    "BINK*",
)

# The XBE flags several pure-data blobs as executable (BINKDATA is 0x07).
# They contain no real code, so a wildcard must never lift them.
NOT_CODE_SECTIONS = {"BINKDATA", "$$XTIMAGE", "dspimage", "XON_RD", "DOLBY"}

REG32_NAMES = {
    X86_REG_EAX: "eax", X86_REG_ECX: "ecx", X86_REG_EDX: "edx", X86_REG_EBX: "ebx",
    X86_REG_ESP: "esp", X86_REG_EBP: "ebp", X86_REG_ESI: "esi", X86_REG_EDI: "edi",
}
REG16_NAMES = {
    X86_REG_AX: "ax", X86_REG_CX: "cx", X86_REG_DX: "dx", X86_REG_BX: "bx",
    X86_REG_SP: "sp", X86_REG_BP: "bp", X86_REG_SI: "si", X86_REG_DI: "di",
}
REG8_NAMES = {
    X86_REG_AL: "al", X86_REG_CL: "cl", X86_REG_DL: "dl", X86_REG_BL: "bl",
    X86_REG_AH: "ah", X86_REG_CH: "ch", X86_REG_DH: "dh", X86_REG_BH: "bh",
}

BAD_DATA_MNEMONICS = {
    "retf", "hlt", "in", "out", "insb", "insw", "insd", "outsb", "outsw", "outsd",
    "les", "lds", "lss", "lfs", "lgs", "arpl", "bound", "daa", "das", "aaa", "aas",
    "aam", "aad", "iretd", "loop", "loope", "loopne", "ljmp", "lcall", "into",
    "int1", "salc", "ltr", "ud0", "fisttp", "bnd jmp",
}

def is_bad_insn(ins):
    if ins.mnemonic in BAD_DATA_MNEMONICS:
        return True
    if ins.mnemonic in ("push", "pop") and ins.operands:
        op = ins.operands[0]
        if op.type == X86_OP_REG and op.reg in (X86_REG_ES, X86_REG_CS, X86_REG_SS, X86_REG_DS, X86_REG_GS):
            return True
    if ins.mnemonic == "mov" and len(ins.operands) == 2:
        for op in ins.operands:
            if op.type == X86_OP_REG and op.reg in (X86_REG_ES, X86_REG_CS, X86_REG_SS, X86_REG_DS, X86_REG_GS):
                return True
    return False

JCC_MAP = {
    "jo": "cond_o(ctx)", "jno": "cond_no(ctx)",
    "jb": "cond_b(ctx)", "jc": "cond_b(ctx)", "jnae": "cond_b(ctx)",
    "jae": "cond_ae(ctx)", "jnb": "cond_ae(ctx)", "jnc": "cond_ae(ctx)",
    "je": "cond_e(ctx)", "jz": "cond_e(ctx)",
    "jne": "cond_ne(ctx)", "jnz": "cond_ne(ctx)",
    "jbe": "cond_be(ctx)", "jna": "cond_be(ctx)",
    "ja": "cond_a(ctx)", "jnbe": "cond_a(ctx)",
    "js": "cond_s(ctx)", "jns": "cond_ns(ctx)",
    "jp": "cond_p(ctx)", "jpe": "cond_p(ctx)",
    "jnp": "cond_np(ctx)", "jpo": "cond_np(ctx)",
    "jl": "cond_l(ctx)", "jnge": "cond_l(ctx)",
    "jge": "cond_ge(ctx)", "jnl": "cond_ge(ctx)",
    "jle": "cond_le(ctx)", "jng": "cond_le(ctx)",
    "jg": "cond_g(ctx)", "jnle": "cond_g(ctx)",
}

SETCC_MAP = {k.replace("j", "set", 1): v for k, v in JCC_MAP.items()}
CMOVCC_MAP = {k.replace("j", "cmov", 1): v for k, v in JCC_MAP.items()}

FPU_CONDS = {
    "fcmovb": "ctx.cf()",
    "fcmove": "ctx.zf()",
    "fcmovbe": "(ctx.cf() || ctx.zf())",
    "fcmovu": "ctx.pf()",
    "fcmovnb": "!ctx.cf()",
    "fcmovne": "!ctx.zf()",
    "fcmovnbe": "(!ctx.cf() && !ctx.zf())",
    "fcmovnu": "!ctx.pf()",
}

def u32(v):
    return v & 0xFFFFFFFF

def load_xbe():
    xbe = XBE_PATH.read_bytes()
    base_addr = struct.unpack_from("<I", xbe, 0x104)[0]
    num_sec = struct.unpack_from("<I", xbe, 0x11C)[0]
    sec_hdr_va = struct.unpack_from("<I", xbe, 0x120)[0]
    secs = {}
    for i in range(num_sec):
        off = sec_hdr_va - base_addr + i * 56
        flags, va, vsz, roff, rsz, nva = struct.unpack_from("<6I", xbe, off)
        name = xbe[nva - base_addr : nva - base_addr + 32].split(b"\x00", 1)[0].decode("ascii", "replace")
        uniq, k = name, 2
        while uniq in secs:          # several sections share a name (BINKYUY2 x4)
            uniq = f"{name}#{k}"
            k += 1
        secs[uniq] = (va, vsz, roff, rsz, flags)
    tva, tvsz, troff, trsz, _tflags = secs[".text"]
    tb = xbe[troff : troff + trsz]
    return xbe, secs, tva, tva + trsz, tb

def st_index(reg_id):
    return reg_id - X86_REG_ST0 if X86_REG_ST0 <= reg_id <= X86_REG_ST7 else 0

def mm_index(reg_id):
    return reg_id - X86_REG_MM0

def xmm_index(reg_id):
    return reg_id - X86_REG_XMM0

def ea_expr(op):
    assert op.type == X86_OP_MEM
    m = op.mem
    parts = []
    if m.segment == X86_REG_FS:
        parts.append("ctx.fs_base")
    if m.base != 0:
        parts.append(f"ctx.{REG32_NAMES[m.base]}")
    if m.index != 0:
        idx = f"ctx.{REG32_NAMES[m.index]}"
        if m.scale != 1:
            idx = f"({idx} * {m.scale}u)"
        parts.append(idx)
    if m.disp != 0 or not parts:
        parts.append(f"0x{u32(m.disp):x}u")
    if len(parts) == 1:
        return parts[0]
    return "(" + " + ".join(parts) + ")"

def read_op(op, size_override=None):
    sz = size_override or op.size
    if op.type == X86_OP_REG:
        r = op.reg
        if r in REG32_NAMES:
            return f"ctx.{REG32_NAMES[r]}"
        if r in REG16_NAMES:
            return f"ctx.{REG16_NAMES[r]}()"
        if r in REG8_NAMES:
            return f"ctx.{REG8_NAMES[r]}()"
        if X86_REG_MM0 <= r <= X86_REG_MM7:
            return f"ctx.mm[{mm_index(r)}]"
        if X86_REG_XMM0 <= r <= X86_REG_XMM7:
            return f"ctx.xmm[{xmm_index(r)}]"
        if X86_REG_ST0 <= r <= X86_REG_ST7:
            return f"ctx.fpu_get({st_index(r)})"
        raise ValueError(f"Unsupported read reg {r}")
    if op.type == X86_OP_IMM:
        mask = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}[sz]
        return f"0x{op.imm & mask:x}u"
    if op.type == X86_OP_MEM:
        ea = ea_expr(op)
        if sz == 1: return f"rd8({ea})"
        if sz == 2: return f"rd16({ea})"
        if sz == 4: return f"rd32({ea})"
        if sz == 8: return f"rd64({ea})"
        if sz == 16: return f"rd_xmm({ea})"
        raise ValueError(f"Unsupported mem read sz {sz}")
    raise ValueError(f"Unsupported op type {op.type}")

def write_op(op, val_expr, size_override=None):
    sz = size_override or op.size
    if op.type == X86_OP_REG:
        r = op.reg
        if r in REG32_NAMES:
            return f"ctx.{REG32_NAMES[r]} = ({val_expr});"
        if r in REG16_NAMES:
            return f"ctx.set_{REG16_NAMES[r]}({val_expr});"
        if r in REG8_NAMES:
            return f"ctx.set_{REG8_NAMES[r]}({val_expr});"
        if X86_REG_MM0 <= r <= X86_REG_MM7:
            return f"ctx.mm[{mm_index(r)}] = ({val_expr});"
        if X86_REG_XMM0 <= r <= X86_REG_XMM7:
            return f"ctx.xmm[{xmm_index(r)}] = ({val_expr});"
        if X86_REG_ST0 <= r <= X86_REG_ST7:
            return f"ctx.fpu_reg({st_index(r)}) = ({val_expr});"
        raise ValueError(f"Unsupported write reg {r}")
    if op.type == X86_OP_MEM:
        ea = ea_expr(op)
        if sz == 1: return f"wr8({ea}, static_cast<uint8_t>({val_expr}));"
        if sz == 2: return f"wr16({ea}, static_cast<uint16_t>({val_expr}));"
        if sz == 4: return f"wr32({ea}, static_cast<uint32_t>({val_expr}));"
        if sz == 8: return f"wr64({ea}, static_cast<uint64_t>({val_expr}));"
        if sz == 16: return f"wr_xmm({ea}, {val_expr});"
        raise ValueError(f"Unsupported mem write sz {sz}")
    raise ValueError(f"Unsupported write op type {op.type}")

def c_type(sz):
    return {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}[sz]

def sc_type(sz):
    return {1: "int8_t", 2: "int16_t", 4: "int32_t", 8: "int64_t"}[sz]

def fpu_mem_read(op):
    ea = ea_expr(op)
    if op.size == 4: return f"static_cast<double>(rdf32({ea}))"
    if op.size == 8: return f"rdf64({ea})"
    if op.size == 10: return f"rd_ld80({ea})"
    raise ValueError(f"bad fpu read size {op.size}")

def fpu_mem_int_read(op):
    ea = ea_expr(op)
    if op.size == 2: return f"static_cast<double>(static_cast<int16_t>(rd16({ea})))"
    if op.size == 4: return f"static_cast<double>(static_cast<int32_t>(rd32({ea})))"
    if op.size == 8: return f"static_cast<double>(static_cast<int64_t>(rd64({ea})))"
    raise ValueError(f"bad fpu int size {op.size}")

def discover_extra_switch_tables(tb, tva, tend, known_sw, existing_insns_by_addr):
    extra = defaultdict(list)
    for addr, ins in existing_insns_by_addr.items():
        if ins.mnemonic == "jmp" and ins.operands and ins.operands[0].type == X86_OP_MEM:
            m = ins.operands[0].mem
            if m.scale == 4 and m.disp != 0 and m.segment == 0 and m.base == 0:
                tbl_va = u32(m.disp)
                if tva <= tbl_va < tend and addr not in known_sw:
                    targets = []
                    cur = tbl_va
                    while cur + 4 <= tend:
                        tgt = struct.unpack_from("<I", tb, cur - tva)[0]
                        if tva <= tgt < tend and not (tbl_va <= tgt < tbl_va + 0x400):
                            targets.append(tgt)
                            cur += 4
                            if len(targets) >= 512:
                                break
                        else:
                            break
                    if targets:
                        extra[addr] = targets
    return extra

def discover_data_fn_seeds(xbe, secs, tb, tva, tend, md):
    seeds = {
        0x001DE03C, 0x001DDFC8, 0x0008FBE0, 0x00038790, 0x0003C110,
        0x001F15A4, 0x001F1609, 0x0001D107, 0x00028FA0, 0x0002B630,
        0x00108D10, 0x00108E70, 0x00146870, 0x0017AFB0, 0x0018E2C0,
        0x001E3766, 0x001E9E14, 0x001F43B7,
        0x001DD180, 0x001ECFD0, 0x0016D850,
    }
    def va_off(va):
        for _sva, _svsz, sroff, srsz, _fl in secs.values():
            if _sva <= va < _sva + srsz:
                return sroff + (va - _sva)
        return None
    for s_va, e_va, strict_bnd in [(0x002C37E0, 0x002D3300, True), (0x002DEBA0, 0x002DEC80, False)]:
        for va in range(s_va, e_va, 4):
            o = va_off(va)
            if o is None:
                continue
            t = struct.unpack_from("<I", xbe, o)[0]
            if not (tva <= t < tend):
                continue
            off = t - tva
            if strict_bnd:
                is_bnd = (off == 0) or (tb[off - 1] in (0xC3, 0x90, 0xCC)) or (off >= 3 and tb[off - 3] == 0xC2 and tb[off - 1] == 0x00)
                if not is_bnd:
                    continue
            ins_l = list(md.disasm(tb[off : min(len(tb), off + 24)], t))
            if ins_l and ins_l[0].mnemonic not in ("nop", "int3", "add") and not any(is_bad_insn(x) for x in ins_l[:3]):
                seeds.add(t)
    return seeds

def recover_functions(tb, tva, tend, db, extra_seeds=None):
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    ghidra_funcs = {}
    all_switches = {}
    for f in db["functions"]:
        ea = int(f["entry"], 16)
        if tva <= ea < tend:
            ghidra_funcs[ea] = f
            for sw in f.get("switches", []):
                all_switches[int(sw["addr"], 16)] = [int(t, 16) for t in sw["targets"]]

    # Collect initial candidate entries, discarding poisoned data chunks
    func_entries = set()
    poisoned = set()
    for ea, f in ghidra_funcs.items():
        bad = False
        for rs_h, re_h in f.get("ranges", []):
            s, e = int(rs_h, 16), int(re_h, 16) + 1
            for ins in md.disasm(tb[s - tva : e - tva], s):
                if is_bad_insn(ins):
                    bad = True
                    break
            if bad:
                break
        if bad:
            poisoned.add(ea)
        else:
            func_entries.add(ea)

    # Also drop Ghidra gap guesses that sit inside an obvious non-pad immediate / mid-instruction if preceded by single CC/non-pad unless aligned cleanly with valid prologue
    for ea in list(func_entries):
        if ea in (0x001DE03C, 0x001DDFC8, 0x0008FBE0):
            continue
        off = ea - tva
        if off >= 2 and (ea & 0xF) == 0:
            # If Ghidra guessed an isolated tiny FUN_ right after a non-padding byte, validate it has a full clean trace
            pass

    # Also ensure known CRT/engine gap functions are included if preceded by padding/ret and valid
    def is_valid_entry(va):
        if not (tva <= va < tend):
            return False
        if va in poisoned:
            return False
        off = va - tva
        insns = list(md.disasm(tb[off : min(tend, va + 32) - tva], va))
        if not insns or is_bad_insn(insns[0]) or insns[0].mnemonic in ("nop", "int3"):
            return False
        if va % 4 == 0 and off + 8 <= len(tb):
            w0, w1 = struct.unpack_from("<2I", tb, off)
            if tva <= w0 < tend and tva <= w1 < tend:
                return False
        return True

    # Always force key + vtable + CRT entries (never mid-instruction unless verified clean)
    for must_va in sorted(extra_seeds or (0x001DE03C, 0x001DDFC8, 0x0008FBE0, 0x00038790, 0x0003C110)):
        if is_valid_entry(must_va):
            func_entries.add(must_va)

    # Known non-returning fatal stubs (NOT Assert!)
    FATAL_CALLS = {0x002C3194, 0x002C30A8} # KeBugCheck, PsTerminateSystemThread

    # First pass disassemble known ranges to find extra [reg*4 + tbl] switches
    seed_insns = {}
    for ea in list(func_entries):
        f = ghidra_funcs.get(ea)
        if not f:
            continue
        for rs_h, re_h in f.get("ranges", []):
            s, e = int(rs_h, 16), int(re_h, 16) + 1
            for ins in md.disasm(tb[s - tva : e - tva], s):
                seed_insns[ins.address] = ins

    extra_sw = discover_extra_switch_tables(tb, tva, tend, all_switches, seed_insns)
    all_switches.update(extra_sw)

    # Trace per-function CFG cleanly
    # Iterative discovery: trace all functions, add any newly called/missing .text targets, find valid gap entries
    lifted_funcs = {}

    def trace_function(entry):
        insns = {}
        queue = [entry]
        # Seed known Ghidra ranges for this function (that aren't inside switch tables)
        gf = ghidra_funcs.get(entry)
        # Note: only queue entry + switch targets + discovered branch/fallthrough flows so we NEVER trace garbage bytes!
        sw_used = {}
        new_calls = set()
        seen_blocks = set()

        while queue:
            cur = queue.pop()
            if cur in insns or not (tva <= cur < tend):
                continue
            if cur != entry and cur in func_entries:
                # Reached start of another function via fallthrough or jump -> skip (caller handles tail dispatch)
                continue
            while cur not in insns and tva <= cur < tend:
                if cur != entry and cur in func_entries:
                    break
                decoded = list(md.disasm(tb[cur - tva : min(tend, cur + 15) - tva], cur, 1))
                if not decoded:
                    break
                ins = decoded[0]
                if is_bad_insn(ins):
                    # Poisoned / mid-data decode -> abort trace
                    return {}, {}, set()
                insns[cur] = ins
                nxt = cur + ins.size
                m = ins.mnemonic

                if m.startswith("ret"):
                    break
                if m == "int3":
                    # sometimes MSVC emits int3 after non-returning fatal call, stop block
                    break
                if m == "jmp":
                    op = ins.operands[0]
                    if op.type == X86_OP_IMM:
                        tgt = u32(op.imm)
                        if tva <= tgt < tend and tgt not in func_entries:
                            queue.append(tgt)
                    elif cur in all_switches:
                        sw_used[cur] = list(all_switches[cur])
                        for t in all_switches[cur]:
                            if tva <= t < tend and t not in func_entries:
                                queue.append(t)
                    elif op.type == X86_OP_MEM and op.mem.scale == 4 and op.mem.disp != 0 and op.mem.base == 0 and op.mem.segment == 0:
                        tbl_va = u32(op.mem.disp)
                        if tva <= tbl_va < tend:
                            targets = []
                            p = tbl_va
                            while p + 4 <= tend:
                                t = struct.unpack_from("<I", tb, p - tva)[0]
                                if tva <= t < tend and not (tbl_va <= t < tbl_va + 0x400):
                                    targets.append(t)
                                    p += 4
                                    if len(targets) >= 512: break
                                else:
                                    break
                            if targets:
                                sw_used[cur] = targets
                                all_switches[cur] = targets
                                for t in targets:
                                    if t not in func_entries:
                                        queue.append(t)
                    break
                if m in JCC_MAP or m == "jecxz":
                    tgt = u32(ins.operands[0].imm)
                    if tva <= tgt < tend and tgt not in func_entries:
                        queue.append(tgt)
                    cur = nxt
                    continue
                if m == "call":
                    op = ins.operands[0]
                    if op.type == X86_OP_IMM:
                        tgt = u32(op.imm)
                        if tva <= tgt < tend and tgt not in poisoned:
                            new_calls.add(tgt)
                        if tgt in FATAL_CALLS:
                            break
                    # Stop cleanly at int3 trap/padding or NOP padding that leads into a known function entry
                    if nxt < tend and tb[nxt - tva] in (0x90, 0xCC) and nxt not in insns:
                        if tb[nxt - tva] == 0xCC:
                            break
                        p = nxt
                        while p < tend and tb[p - tva] in (0x90, 0xCC):
                            p += 1
                        if p in func_entries:
                            break
                    cur = nxt
                    continue
                if nxt < tend and tb[nxt - tva] in (0x90, 0xCC) and nxt not in insns:
                    if tb[nxt - tva] == 0xCC:
                        break
                    p = nxt
                    while p < tend and tb[p - tva] in (0x90, 0xCC):
                        p += 1
                    if p in func_entries:
                        break
                cur = nxt
            if len(insns) > 12000:
                break
        return insns, sw_used, new_calls

    pending = sorted(func_entries)
    processed = set()
    all_called = set()
    round_idx = 0
    while pending and round_idx < 10:
        round_idx += 1
        newly_discovered = set()
        for ea in pending:
            if ea in processed or ea in poisoned:
                continue
            processed.add(ea)
            insns, sw_used, new_calls = trace_function(ea)
            if insns:
                lifted_funcs[ea] = (insns, sw_used)
                all_called.update(new_calls)
                for c in new_calls:
                    if c not in func_entries and is_valid_entry(c):
                        func_entries.add(c)
                        newly_discovered.add(c)
        pending = sorted(newly_discovered)

    # Purge bogus Ghidra mid-instruction entries and re-trace parents that were clipped by them!
    def has_intra_overlap(insns):
        prev_end = 0
        for a in sorted(insns.keys()):
            if a < prev_end:
                return True
            prev_end = max(prev_end, a + insns[a].size)
        return False

    for _purge_pass in range(4):
        bad_entries = set()
        # Any function whose own trace overlaps itself is a mid-instruction entry
        for ea, (insns, _) in lifted_funcs.items():
            if has_intra_overlap(insns) and ea not in (0x001DE03C, 0x001DDFC8):
                bad_entries.add(ea)
        # Any function entry sitting strictly inside another function's instruction
        mid_bytes = {}  # addr -> owner_ea (only middle bytes a+1 .. a+sz-1)
        for ea, (insns, _) in lifted_funcs.items():
            if ea in bad_entries:
                continue
            for a, ins in insns.items():
                for k in range(1, ins.size):
                    mid_bytes[a + k] = ea
        for ea in list(func_entries):
            if ea in bad_entries or ea in (0x001DE03C, 0x001DDFC8):
                continue
            owner = mid_bytes.get(ea)
            if owner is not None and owner != ea:
                bad_entries.add(ea)
        if not bad_entries:
            break
        print(f"Purging {len(bad_entries)} mid-instruction bogus entries: {[hex(x) for x in sorted(bad_entries)[:10]]}")
        for ea in bad_entries:
            poisoned.add(ea)
            func_entries.discard(ea)
            lifted_funcs.pop(ea, None)
        # Re-trace any function that previously stopped adjacent to a purged entry
        retrace = []
        for ea, (insns, _) in list(lifted_funcs.items()):
            for a, ins in insns.items():
                nxt = a + ins.size
                if nxt in bad_entries or any(nxt <= b < nxt + 16 for b in bad_entries):
                    retrace.append(ea)
                    break
        for ea in retrace:
            insns, sw_used, new_calls = trace_function(ea)
            if insns:
                lifted_funcs[ea] = (insns, sw_used)
                for c in new_calls:
                    if c not in func_entries and is_valid_entry(c):
                        func_entries.add(c)
                        ins2, sw2, _ = trace_function(c)
                        if ins2:
                            lifted_funcs[c] = (ins2, sw2)
                            processed.add(c)

    # Clean up: if a function falls straight into the entry of another function without jmp/ret,
    # its last instruction's next address == another entry; emitting tail call handles that!
    # Also resolve all remaining uncovered valid aligned gap prologues once!
    covered_bytes = bytearray(len(tb))
    cov_insn_sizes = {}
    for ea, (insns, _) in lifted_funcs.items():
        for a, ins in insns.items():
            cov_insn_sizes[a] = ins.size
            off = a - tva
            for k in range(ins.size):
                if 0 <= off + k < len(covered_bytes):
                    covered_bytes[off + k] = 1
    # Mark known switch tables inside .text as covered data
    for sw_addr, tgts in all_switches.items():
        if sw_addr in seed_insns:
            ins = seed_insns[sw_addr]
            if ins.operands and ins.operands[0].type == X86_OP_MEM and ins.operands[0].mem.disp != 0:
                tbl = u32(ins.operands[0].mem.disp)
                if tva <= tbl < tend:
                    for k in range(len(tgts) * 4):
                        if tbl - tva + k < len(covered_bytes):
                            covered_bytes[tbl - tva + k] = 1

    # Multi-pass recovery: (a) code imm32 callback pointers (threads, atexit, EH handlers, game callbacks)
    # and (b) safe gap boundaries (16-aligned / post-ret / post-pad / local covered boundary) without overlap.
    def accept_speculative(va, allow_shared_tail=False):
        if va in processed or va in func_entries or not (tva <= va < tend) or covered_bytes[va - tva]:
            return False
        if not is_valid_entry(va):
            return False
        insns, sw_used, new_calls = trace_function(va)
        if not insns or has_intra_overlap(insns) or any(is_bad_insn(ins) for ins in insns.values()):
            return False
        for a, ins in insns.items():
            off = a - tva
            has_cov = any(0 <= off + k < len(covered_bytes) and covered_bytes[off + k] for k in range(ins.size))
            if has_cov:
                if not allow_shared_tail or a == va or cov_insn_sizes.get(a) != ins.size:
                    return False
        func_entries.add(va)
        lifted_funcs[va] = (insns, sw_used)
        processed.add(va)
        for a, ins in insns.items():
            cov_insn_sizes[a] = ins.size
            off = a - tva
            for k in range(ins.size):
                if 0 <= off + k < len(covered_bytes):
                    covered_bytes[off + k] = 1
        for c in new_calls:
            if c not in func_entries and is_valid_entry(c):
                accept_speculative(c, allow_shared_tail=allow_shared_tail)
        return True

    for _pass in range(3):
        added = 0
        # Pass A1: push/mov imm32 callback addresses from already-lifted code (run BEFORE code_pointer_targets)
        push_imms = set()
        for ea, (insns, _) in list(lifted_funcs.items()):
            for a, ins in insns.items():
                if ins.mnemonic in ("push", "mov") and ins.operands:
                    op = ins.operands[-1]
                    if op.type == X86_OP_IMM:
                        imm = u32(op.imm)
                        if tva <= imm < tend and (imm & (imm - 1)) != 0 and imm not in func_entries and not covered_bytes[imm - tva]:
                            push_imms.add(imm)
        for va in sorted(push_imms):
            if accept_speculative(va, allow_shared_tail=True):
                added += 1

        # Pass A2: remaining uncovered Ghidra code pointer targets
        if _pass == 0:
            cpt_cands = set()
            for _pc in db.get("code_pointer_targets", []):
                _tgt = int(_pc, 16)
                if tva <= _tgt < tend and _tgt not in poisoned and _tgt not in func_entries and not covered_bytes[_tgt - tva]:
                    cpt_cands.add(_tgt)
            for va in sorted(cpt_cands):
                if accept_speculative(va, allow_shared_tail=True):
                    added += 1

        # Pass B: aligned / post-ret / post-pad gap entries (no sticky ratchet!)
        gap_seeds = []
        i = 0
        while i < len(tb):
            if covered_bytes[i]:
                i += 1
                continue
            va = tva + i
            after_ret = (i >= 1 and covered_bytes[i - 1] and tb[i - 1] == 0xC3) or (i >= 3 and tb[i - 3] == 0xC2 and tb[i - 1] == 0x00 and covered_bytes[i - 3])
            after_pad = (i >= 2 and tb[i - 1] in (0x90, 0xCC) and tb[i - 2] in (0x90, 0xCC, 0xC3))
            local_cov_end = (i >= 1 and covered_bytes[i - 1] == 1 and tb[i] not in (0x90, 0xCC, 0x00))
            test_aligned = (va % 16 == 0) or after_ret or after_pad
            if not test_aligned:
                i += 1
                continue
            if va not in func_entries and (after_pad or after_ret or local_cov_end) and is_valid_entry(va):
                gap_seeds.append(va)
            i += 1
        for va in gap_seeds:
            if accept_speculative(va):
                added += 1
        if added == 0:
            break

    return lifted_funcs, all_switches, func_entries

def translate_insn(ins, func_insns, all_funcs, switches_in_func):
    addr = ins.address
    m = ins.mnemonic
    ops = ins.operands
    nxt = addr + ins.size
    lines = []

    def emit(s):
        lines.append(f"    {s}")

    # Jumps / Control Flow
    if m == "ret":
        if ops:
            imm = ops[0].imm & 0xFFFF
            emit(f"ctx.esp += {4 + imm}u; return;")
        else:
            emit("ctx.esp += 4u; return;")
        return lines

    if m == "call":
        op = ops[0]
        if op.type == X86_OP_IMM:
            tgt = u32(op.imm)
            emit(f"ctx.push32(0x{nxt:08x}u); recomp_dispatch_call(ctx, 0x{tgt:08x}u);")
        else:
            val = read_op(op)
            emit(f"{{ uint32_t _t = {val}; ctx.push32(0x{nxt:08x}u); recomp_dispatch_call(ctx, _t); }}")
        return lines

    if m == "jmp":
        op = ops[0]
        if op.type == X86_OP_IMM:
            tgt = u32(op.imm)
            if tgt in func_insns:
                emit(f"goto L_{tgt:08x};")
            else:
                emit(f"recomp_dispatch_call(ctx, 0x{tgt:08x}u); return;")
        elif addr in switches_in_func:
            addr_expr = read_op(op)
            emit(f"switch ({addr_expr}) {{")
            seen_cases = set()
            for t in switches_in_func[addr]:
                if t in seen_cases:
                    continue
                seen_cases.add(t)
                if t in func_insns:
                    emit(f"    case 0x{t:08x}u: goto L_{t:08x};")
                else:
                    emit(f"    case 0x{t:08x}u: recomp_dispatch_call(ctx, 0x{t:08x}u); return;")
            emit(f"    default: recomp_dispatch_call(ctx, {addr_expr}); return;")
            emit("}")
        else:
            val = read_op(op)
            emit(f"recomp_dispatch_call(ctx, {val}); return;")
        return lines

    if m in JCC_MAP:
        tgt = u32(ops[0].imm)
        cond = JCC_MAP[m]
        if tgt in func_insns:
            emit(f"if ({cond}) goto L_{tgt:08x};")
        else:
            emit(f"if ({cond}) {{ recomp_dispatch_call(ctx, 0x{tgt:08x}u); return; }}")
        return lines

    if m == "jecxz":
        tgt = u32(ops[0].imm)
        if tgt in func_insns:
            emit(f"if (ctx.ecx == 0) goto L_{tgt:08x};")
        else:
            emit(f"if (ctx.ecx == 0) {{ recomp_dispatch_call(ctx, 0x{tgt:08x}u); return; }}")
        return lines

    # No-ops & hints
    if m in ("nop", "wait", "prefetchnta", "prefetcht0", "sfence", "cli", "sti"):
        return lines
    if m == "int3":
        emit("// int3")
        return lines
    if m == "int":
        emit("// int")
        return lines
    if m == "cpuid":
        emit("{ uint32_t _leaf = ctx.eax; if (_leaf == 0) { ctx.eax = 1; ctx.ebx = 0x756e6547u; ctx.edx = 0x49656e69u; ctx.ecx = 0x6c65746eu; } else { ctx.eax = 0x0000068Au; ctx.ebx = 0; ctx.ecx = 0; ctx.edx = 0x0383F9FFu; } }")
        return lines
    if m == "rdtsc":
        emit("{ uint64_t _t = cpu_rdtsc(); ctx.eax = static_cast<uint32_t>(_t); ctx.edx = static_cast<uint32_t>(_t >> 32); }")
        return lines

    # Moves
    if m == "mov":
        seg_regs = (X86_REG_ES, X86_REG_CS, X86_REG_SS, X86_REG_DS, X86_REG_FS, X86_REG_GS)
        # Segment-register source/dest has no numeric meaning in our flat address model.
        if ops[1].type == X86_OP_REG and ops[1].reg in seg_regs:
            emit(write_op(ops[0], "0", ops[0].size))
            return lines
        if ops[0].type == X86_OP_REG and ops[0].reg in seg_regs:
            return lines
        emit(write_op(ops[0], read_op(ops[1], ops[0].size)))
        return lines
    if m == "movzx":
        src = read_op(ops[1])
        emit(write_op(ops[0], f"static_cast<{c_type(ops[0].size)}>({src})"))
        return lines
    if m == "movsx":
        src = read_op(ops[1])
        emit(write_op(ops[0], f"static_cast<{c_type(ops[0].size)}>(static_cast<{sc_type(ops[0].size)}>(static_cast<{sc_type(ops[1].size)}>({src})))"))
        return lines
    if m == "lea":
        if ops[0].reg in REG32_NAMES:
            # Skip redundant alignment nops like lea esp, [esp]
            if ops[1].mem.base == ops[0].reg and ops[1].mem.index == 0 and ops[1].mem.disp == 0 and ops[1].mem.segment == 0:
                return lines
            emit(f"ctx.{REG32_NAMES[ops[0].reg]} = {ea_expr(ops[1])};")
        else:
            emit(write_op(ops[0], ea_expr(ops[1])))
        return lines
    if m == "xchg":
        sz = ops[0].size
        a_expr = read_op(ops[0], sz)
        b_expr = read_op(ops[1], sz)
        emit(f"{{ {c_type(sz)} _a = {a_expr}; {c_type(sz)} _b = {b_expr}; {write_op(ops[0], '_b', sz)} {write_op(ops[1], '_a', sz)} }}")
        return lines
    if m in CMOVCC_MAP:
        emit(f"if ({CMOVCC_MAP[m]}) {{ {write_op(ops[0], read_op(ops[1], ops[0].size))} }}")
        return lines
    if m in SETCC_MAP:
        emit(write_op(ops[0], f"({SETCC_MAP[m]} ? 1u : 0u)", 1))
        return lines
    if m == "xlatb":
        emit("ctx.set_al(rd8(ctx.ebx + ctx.al()));")
        return lines

    # Stack / frame
    if m == "push":
        sz = ops[0].size
        if sz == 2:
            emit(f"ctx.push16(static_cast<uint16_t>({read_op(ops[0], 2)}));")
        else:
            val = read_op(ops[0], 4)
            # If push imm8, capstone already provides sign-extended imm for push
            emit(f"ctx.push32({val});")
        return lines
    if m == "pop":
        sz = ops[0].size
        if sz == 2:
            emit(f"{{ uint16_t _v = ctx.pop16(); {write_op(ops[0], '_v', 2)} }}")
        else:
            emit(f"{{ uint32_t _v = ctx.pop32(); {write_op(ops[0], '_v', 4)} }}")
        return lines
    if m == "pushal":
        emit("{ uint32_t _esp = ctx.esp; ctx.push32(ctx.eax); ctx.push32(ctx.ecx); ctx.push32(ctx.edx); ctx.push32(ctx.ebx); ctx.push32(_esp); ctx.push32(ctx.ebp); ctx.push32(ctx.esi); ctx.push32(ctx.edi); }")
        return lines
    if m == "popal":
        emit("ctx.edi = ctx.pop32(); ctx.esi = ctx.pop32(); ctx.ebp = ctx.pop32(); ctx.esp += 4u; ctx.ebx = ctx.pop32(); ctx.edx = ctx.pop32(); ctx.ecx = ctx.pop32(); ctx.eax = ctx.pop32();")
        return lines
    if m == "pushfd":
        emit("ctx.push32(ctx.eflags);")
        return lines
    if m == "popfd":
        emit("ctx.eflags = (ctx.pop32() & 0x00200FD5u) | 0x202u;")
        return lines
    if m == "leave":
        emit("ctx.esp = ctx.ebp; ctx.ebp = ctx.pop32();")
        return lines
    if m == "enter":
        sz = ops[0].imm & 0xFFFF
        emit(f"ctx.push32(ctx.ebp); ctx.ebp = ctx.esp; ctx.esp -= 0x{sz:x}u;")
        return lines

    # Flags / Extensions / ALU
    if m == "cld": emit("ctx.set_flag(EFL_DF, false);"); return lines
    if m == "std": emit("ctx.set_flag(EFL_DF, true);"); return lines
    if m == "sahf":
        emit("{ uint32_t _ah = ctx.ah(); ctx.eflags = (ctx.eflags & ~0xD5u) | (_ah & 0xD5u); }")
        return lines
    if m == "cdq":
        emit("ctx.edx = (static_cast<int32_t>(ctx.eax) < 0) ? 0xFFFFFFFFu : 0u;")
        return lines
    if m == "cwde":
        emit("ctx.eax = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(ctx.ax())));")
        return lines

    if m in ("add", "adc", "sub", "sbb", "cmp"):
        sz = ops[0].size
        T = c_type(sz)
        a = read_op(ops[0], sz)
        b = read_op(ops[1], sz)
        if m == "add":
            res = f"alu_add<{T}>(ctx, {a}, {b}, false)"
            emit(write_op(ops[0], res, sz))
        elif m == "adc":
            res = f"alu_add<{T}>(ctx, {a}, {b}, ctx.cf())"
            emit(write_op(ops[0], res, sz))
        elif m == "sub":
            res = f"alu_sub<{T}>(ctx, {a}, {b}, false)"
            emit(write_op(ops[0], res, sz))
        elif m == "sbb":
            res = f"alu_sub<{T}>(ctx, {a}, {b}, ctx.cf())"
            emit(write_op(ops[0], res, sz))
        elif m == "cmp":
            emit(f"alu_sub<{T}>(ctx, {a}, {b}, false);")
        return lines

    if m in ("and", "or", "xor", "test"):
        sz = ops[0].size
        T = c_type(sz)
        a = read_op(ops[0], sz)
        b = read_op(ops[1], sz)
        op_c = {"and": "&", "or": "|", "xor": "^", "test": "&"}[m]
        res = f"alu_logic<{T}>(ctx, static_cast<{T}>(({a}) {op_c} ({b})))"
        if m == "test":
            emit(f"{res};")
        else:
            emit(write_op(ops[0], res, sz))
        return lines

    if m == "not":
        sz = ops[0].size
        emit(write_op(ops[0], f"static_cast<{c_type(sz)}>(~({read_op(ops[0], sz)}))", sz))
        return lines
    if m == "neg":
        sz = ops[0].size
        T = c_type(sz)
        emit(write_op(ops[0], f"alu_sub<{T}>(ctx, 0, {read_op(ops[0], sz)}, false)", sz))
        return lines
    if m == "inc":
        sz = ops[0].size
        T = c_type(sz)
        emit(write_op(ops[0], f"alu_inc<{T}>(ctx, {read_op(ops[0], sz)})", sz))
        return lines
    if m == "dec":
        sz = ops[0].size
        T = c_type(sz)
        emit(write_op(ops[0], f"alu_dec<{T}>(ctx, {read_op(ops[0], sz)})", sz))
        return lines

    if m in ("shl", "sal", "shr", "sar", "rol", "ror", "rcr", "rcl"):
        sz = ops[0].size
        T = c_type(sz)
        fn = "alu_shl" if m == "sal" else f"alu_{m}"
        emit(write_op(ops[0], f"{fn}<{T}>(ctx, {read_op(ops[0], sz)}, {read_op(ops[1], 1)})", sz))
        return lines

    if m in ("shld", "shrd"):
        emit(write_op(ops[0], f"alu_{m}32(ctx, {read_op(ops[0], 4)}, {read_op(ops[1], 4)}, {read_op(ops[2], 1)})", 4))
        return lines

    if m == "bsf":
        emit(write_op(ops[0], f"alu_bsf32(ctx, {read_op(ops[0], 4)}, {read_op(ops[1], 4)})", 4))
        return lines

    if m in ("bt", "bts"):
        # ops[0] mem32 or reg32, ops[1] reg32 or imm
        idx = read_op(ops[1], 4)
        if ops[0].type == X86_OP_MEM:
            ea = ea_expr(ops[0])
            emit(f"{{ uint32_t _idx = {idx}; uint32_t _ea = ({ea}) + ((int32_t(_idx) >> 5) * 4); uint32_t _bit = (_idx & 31u); uint32_t _v = rd32(_ea); ctx.set_flag(EFL_CF, (_v >> _bit) & 1u); " +
                 ("wr32(_ea, _v | (1u << _bit)); }" if m == "bts" else "}"))
        else:
            emit(f"{{ uint32_t _bit = ({idx}) & 31u; uint32_t _v = {read_op(ops[0], 4)}; ctx.set_flag(EFL_CF, (_v >> _bit) & 1u); " +
                 (f"{write_op(ops[0], '_v | (1u << _bit)', 4)} }}" if m == "bts" else "}"))
        return lines

    if m == "xadd" or m == "lock xadd":
        sz = ops[0].size
        T = c_type(sz)
        a = read_op(ops[0], sz)
        b = read_op(ops[1], sz)
        emit(f"{{ {T} _a = {a}; {T} _b = {b}; {T} _s = alu_add<{T}>(ctx, _a, _b, false); {write_op(ops[0], '_s', sz)} {write_op(ops[1], '_a', sz)} }}")
        return lines

    if m == "mul":
        sz = ops[0].size
        emit(f"alu_mul{sz*8}(ctx, {read_op(ops[0], sz)});")
        return lines
    if m == "imul":
        if len(ops) == 1:
            sz = ops[0].size
            emit(f"alu_imul1_{sz*8}(ctx, {read_op(ops[0], sz)});")
        elif len(ops) == 2:
            sz = ops[0].size
            emit(write_op(ops[0], f"alu_imul2_{sz*8}(ctx, {read_op(ops[0], sz)}, {read_op(ops[1], sz)})", sz))
        else:
            sz = ops[0].size
            emit(write_op(ops[0], f"alu_imul2_{sz*8}(ctx, {read_op(ops[1], sz)}, {read_op(ops[2], sz)})", sz))
        return lines
    if m == "div":
        sz = ops[0].size
        emit(f"alu_div{sz*8}(ctx, {read_op(ops[0], sz)});")
        return lines
    if m == "idiv":
        sz = ops[0].size
        emit(f"alu_idiv{sz*8}(ctx, {read_op(ops[0], sz)});")
        return lines

    # String operations
    if m in ("movsb", "movsw", "movsd"):
        sz = {"movsb": 1, "movsw": 2, "movsd": 4}[m]
        emit(f"{{ int32_t _d = ctx.df() ? -{sz} : {sz}; wr{sz*8}(ctx.edi, rd{sz*8}(ctx.esi)); ctx.esi += _d; ctx.edi += _d; }}")
        return lines
    if m in ("stosb", "stosw", "stosd"):
        sz = {"stosb": 1, "stosw": 2, "stosd": 4}[m]
        val = {1: "ctx.al()", 2: "ctx.ax()", 4: "ctx.eax"}[sz]
        emit(f"{{ int32_t _d = ctx.df() ? -{sz} : {sz}; wr{sz*8}(ctx.edi, {val}); ctx.edi += _d; }}")
        return lines
    if m in ("rep movsb", "rep movsw", "rep movsd"):
        sz = {"b": 1, "w": 2, "d": 4}[m[-1]]
        emit(f"{{ int32_t _d = ctx.df() ? -{sz} : {sz}; while (ctx.ecx) {{ wr{sz*8}(ctx.edi, rd{sz*8}(ctx.esi)); ctx.esi += _d; ctx.edi += _d; --ctx.ecx; }} }}")
        return lines
    if m in ("rep stosb", "rep stosw", "rep stosd"):
        sz = {"b": 1, "w": 2, "d": 4}[m[-1]]
        val = {1: "ctx.al()", 2: "ctx.ax()", 4: "ctx.eax"}[sz]
        emit(f"{{ int32_t _d = ctx.df() ? -{sz} : {sz}; auto _v = {val}; while (ctx.ecx) {{ wr{sz*8}(ctx.edi, _v); ctx.edi += _d; --ctx.ecx; }} }}")
        return lines
    if m in ("repe cmpsb", "repe cmpsw", "repe cmpsd",
             "repne cmpsb", "repne cmpsw", "repne cmpsd"):
        # REPE keeps going while equal; REPNE stops as soon as equal.
        sz = {"b": 1, "w": 2, "d": 4}[m[-1]]
        T = c_type(sz)
        brk = "if (!ctx.zf()) break;" if m.startswith("repe") else "if (ctx.zf()) break;"
        emit(f"{{ int32_t _d = ctx.df() ? -{sz} : {sz}; while (ctx.ecx) {{ "
             f"{T} _a = rd{sz*8}(ctx.esi), _b = rd{sz*8}(ctx.edi); "
             f"alu_sub<{T}>(ctx, _a, _b, false); ctx.esi += _d; ctx.edi += _d; --ctx.ecx; {brk} }} }}")
        return lines
    if m in ("repe scasb", "repe scasw", "repe scasd",
             "repne scasb", "repne scasw", "repne scasd"):
        sz = {"b": 1, "w": 2, "d": 4}[m[-1]]
        T = c_type(sz)
        acc = {1: "ctx.al()", 2: "ctx.ax()", 4: "ctx.eax"}[sz]
        brk = "if (!ctx.zf()) break;" if m.startswith("repe") else "if (ctx.zf()) break;"
        emit(f"{{ int32_t _d = ctx.df() ? -{sz} : {sz}; while (ctx.ecx) {{ "
             f"alu_sub<{T}>(ctx, static_cast<{T}>({acc}), rd{sz*8}(ctx.edi), false); "
             f"ctx.edi += _d; --ctx.ecx; {brk} }} }}")
        return lines

    # x87 FPU instructions
    if m == "fld":
        if ops[0].type == X86_OP_MEM:
            emit(f"ctx.fpu_push({fpu_mem_read(ops[0])});")
        else:
            emit(f"ctx.fpu_push(ctx.fpu_get({st_index(ops[0].reg)}));")
        return lines
    if m == "fild":
        emit(f"ctx.fpu_push({fpu_mem_int_read(ops[0])});")
        return lines
    if m == "fldz": emit("ctx.fpu_push(0.0);"); return lines
    if m == "fld1": emit("ctx.fpu_push(1.0);"); return lines
    if m == "fldpi": emit("ctx.fpu_push(3.14159265358979323846);"); return lines
    if m == "fldl2e": emit("ctx.fpu_push(1.44269504088896340736);"); return lines
    if m == "fldlg2": emit("ctx.fpu_push(0.30102999566398119521);"); return lines
    if m == "fldln2": emit("ctx.fpu_push(0.69314718055994530942);"); return lines

    if m in ("fst", "fstp"):
        pop = (m == "fstp")
        if ops[0].type == X86_OP_REG:
            i = st_index(ops[0].reg)
            if pop:
                emit(f"{{ double _v = ctx.fpu_get(0); ctx.fpu_reg({i}) = _v; ctx.fpu_pop(); }}")
            else:
                emit(f"ctx.fpu_reg({i}) = ctx.fpu_get(0);")
        else:
            ea = ea_expr(ops[0])
            getter = "ctx.fpu_pop()" if pop else "ctx.fpu_get(0)"
            if ops[0].size == 4:
                emit(f"wrf32({ea}, static_cast<float>({getter}));")
            elif ops[0].size == 8:
                emit(f"wrf64({ea}, {getter});")
            elif ops[0].size == 10:
                emit(f"wr_ld80({ea}, {getter});")
        return lines

    if m in ("fist", "fistp"):
        pop = (m == "fistp")
        ea = ea_expr(ops[0])
        getter = "ctx.fpu_pop()" if pop else "ctx.fpu_get(0)"
        if ops[0].size == 2:
            emit(f"wr16({ea}, static_cast<uint16_t>(static_cast<int16_t>(ctx.fpu_round_to_int({getter}))));")
        elif ops[0].size == 4:
            emit(f"wr32({ea}, static_cast<uint32_t>(static_cast<int32_t>(ctx.fpu_round_to_int({getter}))));")
        elif ops[0].size == 8:
            emit(f"wr64({ea}, static_cast<uint64_t>(static_cast<int64_t>(ctx.fpu_round_to_int({getter}))));")
        return lines

    # Binary FPU arithmetic
    if m in ("fadd", "fmul", "fsub", "fsubr", "fdiv", "fdivr",
             "faddp", "fmulp", "fsubp", "fsubrp", "fdivp", "fdivrp",
             "fiadd", "fimul", "fisub", "fisubr", "fidiv", "fidivr"):
        base_op = m.rstrip("p")
        is_pop = m.endswith("p")
        is_int = m.startswith("fi")
        op_char = {"add": "+", "mul": "*", "sub": "-", "div": "/"}[(base_op[2:] if is_int else base_op[1:]).rstrip("r")]
        is_rev = base_op.endswith("r")

        if ops[0].type == X86_OP_MEM:
            rhs = fpu_mem_int_read(ops[0]) if is_int else fpu_mem_read(ops[0])
            if is_rev:
                emit(f"ctx.fpu_reg(0) = ({rhs}) {op_char} ctx.fpu_reg(0);")
            else:
                emit(f"ctx.fpu_reg(0) = ctx.fpu_reg(0) {op_char} ({rhs});")
        elif len(ops) == 1:
            # 1 reg operand: op st(i) or opp st(i) => ST(i) = ST(i) op ST(0) for pop?
            # Wait: Capstone uses 1 operand for fadd st(i) -> ST(0) = ST(0) op ST(i), and faddp st(i) -> ST(i) = ST(i) op ST(0), pop!
            i = st_index(ops[0].reg)
            if is_pop:
                if is_rev:
                    emit(f"{{ double _r = ctx.fpu_get(0) {op_char} ctx.fpu_get({i}); ctx.fpu_reg({i}) = _r; ctx.fpu_pop(); }}")
                else:
                    emit(f"{{ double _r = ctx.fpu_get({i}) {op_char} ctx.fpu_get(0); ctx.fpu_reg({i}) = _r; ctx.fpu_pop(); }}")
            else:
                if is_rev:
                    emit(f"ctx.fpu_reg(0) = ctx.fpu_get({i}) {op_char} ctx.fpu_get(0);")
                else:
                    emit(f"ctx.fpu_reg(0) = ctx.fpu_get(0) {op_char} ctx.fpu_get({i});")
        else:
            # 2 reg operands: dst, src
            d = st_index(ops[0].reg)
            s = st_index(ops[1].reg)
            if is_rev:
                emit(f"{{ double _r = ctx.fpu_get({s}) {op_char} ctx.fpu_get({d}); ctx.fpu_reg({d}) = _r;" + (" ctx.fpu_pop(); }" if is_pop else " }"))
            else:
                emit(f"{{ double _r = ctx.fpu_get({d}) {op_char} ctx.fpu_get({s}); ctx.fpu_reg({d}) = _r;" + (" ctx.fpu_pop(); }" if is_pop else " }"))
        return lines

    if m == "fchs": emit("ctx.fpu_reg(0) = -ctx.fpu_reg(0);"); return lines
    if m == "fabs": emit("ctx.fpu_reg(0) = std::fabs(ctx.fpu_reg(0));"); return lines
    if m == "fsqrt": emit("ctx.fpu_reg(0) = std::sqrt(ctx.fpu_reg(0));"); return lines
    if m == "fsin": emit("ctx.fpu_reg(0) = std::sin(ctx.fpu_reg(0));"); return lines
    if m == "fcos": emit("ctx.fpu_reg(0) = std::cos(ctx.fpu_reg(0));"); return lines
    if m == "fsincos": emit("{ double _a = ctx.fpu_reg(0); ctx.fpu_reg(0) = std::sin(_a); ctx.fpu_push(std::cos(_a)); }"); return lines
    if m == "fptan": emit("{ ctx.fpu_reg(0) = std::tan(ctx.fpu_reg(0)); ctx.fpu_push(1.0); }"); return lines
    if m == "fpatan": emit("{ double _x = ctx.fpu_pop(); double _y = ctx.fpu_get(0); ctx.fpu_reg(0) = std::atan2(_y, _x); }"); return lines
    if m == "f2xm1": emit("ctx.fpu_reg(0) = std::exp2(ctx.fpu_reg(0)) - 1.0;"); return lines
    if m == "fyl2x": emit("{ double _x = ctx.fpu_pop(); ctx.fpu_reg(0) = ctx.fpu_get(0) * std::log2(_x); }"); return lines
    if m == "fscale": emit("ctx.fpu_reg(0) = std::ldexp(ctx.fpu_reg(0), static_cast<int>(std::trunc(ctx.fpu_get(1))));"); return lines
    if m == "frndint": emit("ctx.fpu_reg(0) = ctx.fpu_round_to_int(ctx.fpu_reg(0));"); return lines
    if m == "fprem": emit("ctx.fpu_reg(0) = std::fmod(ctx.fpu_reg(0), ctx.fpu_get(1));"); return lines
    if m == "fprem1": emit("ctx.fpu_reg(0) = std::remainder(ctx.fpu_reg(0), ctx.fpu_get(1));"); return lines
    if m == "fxtract":
        emit("{ int _e = 0; double _m = std::frexp(ctx.fpu_reg(0), &_e); ctx.fpu_reg(0) = static_cast<double>(_e - 1); ctx.fpu_push(_m * 2.0); }")
        return lines
    if m == "fxam": emit("ctx.fpu_fxam();"); return lines
    if m == "ftst": emit("ctx.fpu_com(ctx.fpu_get(0), 0.0);"); return lines
    if m == "fnop": return lines
    if m in ("fcom", "fcomp", "fucom", "fucomp", "fcompp"):
        if m == "fcompp":
            emit("ctx.fpu_com(ctx.fpu_get(0), ctx.fpu_get(1)); ctx.fpu_pop(); ctx.fpu_pop();")
        else:
            rhs = fpu_mem_read(ops[0]) if ops[0].type == X86_OP_MEM else f"ctx.fpu_get({st_index(ops[0].reg)})"
            emit(f"ctx.fpu_com(ctx.fpu_get(0), {rhs});{(' ctx.fpu_pop();' if m.endswith('p') else '')}")
        return lines
    if m in ("fcomi", "fcompi", "fucomi", "fucompi"):
        i = st_index(ops[1].reg) if len(ops) >= 2 else st_index(ops[0].reg)
        emit(f"ctx.fpu_comi(ctx.fpu_get(0), ctx.fpu_get({i}));{(' ctx.fpu_pop();' if 'p' in m else '')}")
        return lines
    if m == "fnstsw":
        emit(write_op(ops[0], "ctx.fpu_sw", 2))
        return lines
    if m == "fnstcw":
        emit(write_op(ops[0], "ctx.fpu_cw", 2))
        return lines
    if m == "fldcw":
        emit(f"ctx.fpu_cw = {read_op(ops[0], 2)};")
        return lines
    if m == "fnclex":
        emit("ctx.fpu_sw &= 0x7F00u;")
        return lines
    if m == "fninit":
        emit("ctx.fpu_top = 0; ctx.fpu_cw = 0x037Fu; ctx.fpu_sw = 0;")
        return lines
    if m == "ffree":
        return lines
    if m == "fxch":
        i = st_index(ops[0].reg) if ops else 1
        emit(f"{{ double _t = ctx.fpu_reg(0); ctx.fpu_reg(0) = ctx.fpu_reg({i}); ctx.fpu_reg({i}) = _t; }}")
        return lines
    if m in ("ficom", "ficomp"):
        rhs = fpu_mem_int_read(ops[0])
        emit(f"ctx.fpu_com(ctx.fpu_get(0), {rhs});{(' ctx.fpu_pop();' if m == 'ficomp' else '')}")
        return lines
    if m in ("fcmovb", "fcmove", "fcmovbe", "fcmovu", "fcmovnb", "fcmovne", "fcmovnbe", "fcmovnu"):
        cond = {
            "fcmovb": "ctx.cf()",
            "fcmove": "ctx.zf()",
            "fcmovbe": "(ctx.cf() || ctx.zf())",
            "fcmovu": "ctx.pf()",
            "fcmovnb": "!ctx.cf()",
            "fcmovne": "!ctx.zf()",
            "fcmovnbe": "(!ctx.cf() && !ctx.zf())",
            "fcmovnu": "!ctx.pf()",
        }[m]
        i = st_index(ops[1].reg)
        emit(f"if ({cond}) ctx.fpu_reg(0) = ctx.fpu_get({i});")
        return lines
    if m in ("fnsave", "fsave"):
        ea = ea_expr(ops[0])
        emit(f"{{ uint32_t _ea = {ea}; wr32(_ea, ctx.fpu_cw); wr32(_ea + 4u, static_cast<uint32_t>(ctx.fpu_sw) | ((static_cast<uint32_t>(ctx.fpu_top) & 7u) << 11)); for (int _k = 0; _k < 8; ++_k) wr_ld80(_ea + 28u + static_cast<uint32_t>(_k * 10), ctx.fpu_get(_k)); ctx.fpu_top = 0; ctx.fpu_cw = 0x037Fu; ctx.fpu_sw = 0; }}")
        return lines
    if m == "frstor":
        ea = ea_expr(ops[0])
        emit(f"{{ uint32_t _ea = {ea}; ctx.fpu_cw = static_cast<uint16_t>(rd32(_ea)); uint32_t _sw = rd32(_ea + 4u); ctx.fpu_sw = static_cast<uint16_t>(_sw); ctx.fpu_top = static_cast<int>((_sw >> 11) & 7u); for (int _k = 0; _k < 8; ++_k) ctx.fpu_reg(_k) = rd_ld80(_ea + 28u + static_cast<uint32_t>(_k * 10)); }}")
        return lines
    if m == "lahf":
        emit("ctx.set_ah(static_cast<uint8_t>((ctx.eflags & 0xD5u) | 0x02u));")
        return lines


    # --- Legacy x86: string, I/O, flag and segment-register ops (not emulated) ---
    if m == "fxch":
        # fxch st(0) is implicit; fxch st(i) swaps with a register
        idx = ops[0].index if len(ops) else 0
        emit(f"std::swap(ctx.fpu_reg(0), ctx.fpu_reg({idx}));")
        return lines
    if m == "lcall":
        # Far call: push eip (+ cs) and jump to target (segment load not emulated).
        tgt = read_op(ops[0], 4)
        emit(f"{{ uint32_t _t = {tgt}; ctx.push32(0x{nxt:08x}u); ctx.eip = _t; }}")
        return lines
    if m in ("int1", "int3", "into", "iretd", "retf"):
        if m == "int1":
            emit("// int1 (debug trap, no-op)")
        elif m == "int3":
            emit("// int3 (breakpoint, no-op)")
        elif m == "into":
            emit("// into (no-op)")
        elif m == "iretd":
            emit("ctx.esp += 4;")
        elif m == "retf":
            emit("ctx.esp += 4; ctx.eip = ctx.pop32();")
        return lines
    if m == "mov":
        # Segment-register source has no numeric meaning in our flat address model.
        if ops[1].type == X86_OP_REG and ops[1].reg in (X86_REG_ES, X86_REG_CS, X86_REG_SS, X86_REG_DS, X86_REG_GS):
            emit(write_op(ops[0], "0"))
            return lines
        emit(write_op(ops[0], read_op(ops[1], ops[0].size)))
        return lines


    # MMX
    if m == "emms": return lines
    if m in ("movq", "movntq"):
        if ops[0].type == X86_OP_REG:
            emit(f"ctx.mm[{mm_index(ops[0].reg)}] = {read_op(ops[1], 8)};")
        else:
            emit(write_op(ops[0], f"ctx.mm[{mm_index(ops[1].reg)}]", 8))
        return lines
    if m == "movd":
        if ops[0].type == X86_OP_REG and X86_REG_MM0 <= ops[0].reg <= X86_REG_MM7:
            emit(f"ctx.mm[{mm_index(ops[0].reg)}] = static_cast<uint64_t>({read_op(ops[1], 4)});")
        else:
            emit(write_op(ops[0], f"static_cast<uint32_t>(ctx.mm[{mm_index(ops[1].reg)}])", 4))
        return lines
    if m in ("pxor", "por", "pand"):
        op_c = {"pxor": "^", "por": "|", "pand": "&"}[m]
        d = mm_index(ops[0].reg)
        rhs = read_op(ops[1], 8)
        emit(f"ctx.mm[{d}] {op_c}= ({rhs});")
        return lines
    if m in ("pmaddwd", "packssdw", "packsswb", "packuswb", "paddd", "psubd", "psubw",
             "paddw", "paddsw", "paddsb", "paddb", "psubb", "psubsw", "psubsb",
             "psubusb", "psubusw", "pmulhw", "pmullw", "pandn", "pavgb", "pavgw",
             "pcmpeqd", "pcmpeqw", "pcmpeqb", "pcmpgtb", "pcmpgtw", "pcmpgtd",
             "pmaxsw", "pminsw", "punpcklwd", "punpckldq", "punpckhdq",
             "punpcklbw", "punpckhbw", "punpckhwd"):
        d = mm_index(ops[0].reg)
        if ops[1].type == X86_OP_MEM and ops[1].size == 4:
            rhs = f"static_cast<uint64_t>({read_op(ops[1], 4)})"
        else:
            rhs = read_op(ops[1], 8)
        emit(f"ctx.mm[{d}] = mmx_{m}(ctx.mm[{d}], {rhs});")
        return lines
    if m == "cmpxchg":
        # CMPXCHG r/m, r : if accumulator == dst then ZF=1, dst=src
        #                  else ZF=0, accumulator=dst
        sz = ops[0].size
        acc_read = {1: "ctx.al()", 2: "ctx.ax()", 4: "ctx.eax"}[sz]
        acc_write = {1: "ctx.set_al", 2: "ctx.set_ax", 4: "ctx.eax = "}[sz]
        dst_r, dst_w = read_op(ops[0]), write_op(ops[0], "_csrc")
        src_r = read_op(ops[1])
        emit("{")
        emit(f"    {c_type(sz)} _cdst = static_cast<{c_type(sz)}>({dst_r});")
        emit(f"    {c_type(sz)} _csrc = static_cast<{c_type(sz)}>({src_r});")
        emit(f"    {c_type(sz)} _cacc = static_cast<{c_type(sz)}>({acc_read});")
        emit("    if (_cacc == _cdst) { ctx.set_flag(EFL_ZF, true);")
        emit(f"        {dst_w}")
        emit("    } else { ctx.set_flag(EFL_ZF, false);")
        if sz == 4:
            emit("        ctx.eax = _cdst;")
        else:
            emit(f"        {acc_write}(_cdst);")
        emit("    }")
        emit("}")
        return lines
    if m in ("psllw", "psrlw", "pslld", "psrld", "psrad"):
        d = mm_index(ops[0].reg)
        cnt = f"static_cast<uint32_t>({read_op(ops[1], 8 if ops[1].type == X86_OP_REG else 1)})"
        emit(f"ctx.mm[{d}] = mmx_{m}(ctx.mm[{d}], {cnt});")
        return lines
    if m == "psllq":
        d = mm_index(ops[0].reg)
        cnt = read_op(ops[1], 8 if ops[1].type == X86_OP_REG else 1)
        emit(f"{{ uint64_t _c = {cnt}; ctx.mm[{d}] = (_c < 64) ? (ctx.mm[{d}] << _c) : 0ull; }}")
        return lines
    if m == "psrlq":
        d = mm_index(ops[0].reg)
        cnt = read_op(ops[1], 8 if ops[1].type == X86_OP_REG else 1)
        emit(f"{{ uint64_t _c = {cnt}; ctx.mm[{d}] = (_c < 64) ? (ctx.mm[{d}] >> _c) : 0ull; }}")
        return lines
    if m == "pmovmskb":
        emit(write_op(ops[0], f"mmx_pmovmskb(ctx.mm[{mm_index(ops[1].reg)}])", 4))
        return lines
    if m == "pshufw":
        d = mm_index(ops[0].reg)
        emit(f"ctx.mm[{d}] = mmx_pshufw(ctx.mm[{d}] != 0 || true ? {read_op(ops[1], 8)} : 0, {read_op(ops[2], 1)});")
        return lines
    if m == "pinsrw":
        d = mm_index(ops[0].reg)
        emit(f"ctx.mm[{d}] = mmx_pinsrw(ctx.mm[{d}], {read_op(ops[1], 2)}, {read_op(ops[2], 1)});")
        return lines

    # SSE
    if m in ("movaps", "movups", "movntps"):
        if ops[0].type == X86_OP_REG:
            emit(f"ctx.xmm[{xmm_index(ops[0].reg)}] = {read_op(ops[1], 16)};")
        else:
            emit(write_op(ops[0], f"ctx.xmm[{xmm_index(ops[1].reg)}]", 16))
        return lines
    if m == "xorps":
        d = xmm_index(ops[0].reg)
        rhs = f"ctx.xmm[{xmm_index(ops[1].reg)}]" if ops[1].type == X86_OP_REG else f"rd_xmm({ea_expr(ops[1])})"
        emit(f"{{ XmmReg _r = {rhs}; for (int _k=0;_k<4;++_k) ctx.xmm[{d}].u[_k] ^= _r.u[_k]; }}")
        return lines
    if m == "movss":
        if ops[0].type == X86_OP_REG and ops[1].type == X86_OP_REG:
            emit(f"ctx.xmm[{xmm_index(ops[0].reg)}].u[0] = ctx.xmm[{xmm_index(ops[1].reg)}].u[0];")
        elif ops[0].type == X86_OP_REG:
            d = xmm_index(ops[0].reg)
            emit(f"ctx.xmm[{d}].u[0] = rd32({ea_expr(ops[1])}); ctx.xmm[{d}].u[1]=ctx.xmm[{d}].u[2]=ctx.xmm[{d}].u[3]=0;")
        else:
            emit(f"wr32({ea_expr(ops[0])}, ctx.xmm[{xmm_index(ops[1].reg)}].u[0]);")
        return lines
    if m == "movlps":
        if ops[0].type == X86_OP_REG:
            emit(f"ctx.xmm[{xmm_index(ops[0].reg)}].q[0] = rd64({ea_expr(ops[1])});")
        else:
            emit(f"wr64({ea_expr(ops[0])}, ctx.xmm[{xmm_index(ops[1].reg)}].q[0]);")
        return lines
    if m == "movhps":
        if ops[0].type == X86_OP_REG:
            emit(f"ctx.xmm[{xmm_index(ops[0].reg)}].q[1] = rd64({ea_expr(ops[1])});")
        else:
            emit(f"wr64({ea_expr(ops[0])}, ctx.xmm[{xmm_index(ops[1].reg)}].q[1]);")
        return lines
    if m == "movlhps":
        emit(f"ctx.xmm[{xmm_index(ops[0].reg)}].q[1] = ctx.xmm[{xmm_index(ops[1].reg)}].q[0];")
        return lines
    if m == "movhlps":
        emit(f"ctx.xmm[{xmm_index(ops[0].reg)}].q[0] = ctx.xmm[{xmm_index(ops[1].reg)}].q[1];")
        return lines
    if m in ("addss", "subss", "mulss", "divss"):
        d = xmm_index(ops[0].reg)
        op_c = {"addss": "+", "subss": "-", "mulss": "*", "divss": "/"}[m]
        rhs = f"ctx.xmm[{xmm_index(ops[1].reg)}].f[0]" if ops[1].type == X86_OP_REG else f"rdf32({ea_expr(ops[1])})"
        emit(f"ctx.xmm[{d}].f[0] {op_c}= ({rhs});")
        return lines
    if m in ("maxss", "minss"):
        d = xmm_index(ops[0].reg)
        fn = "std::max" if m == "maxss" else "std::min"
        rhs = f"ctx.xmm[{xmm_index(ops[1].reg)}].f[0]" if ops[1].type == X86_OP_REG else f"rdf32({ea_expr(ops[1])})"
        emit(f"ctx.xmm[{d}].f[0] = {fn}(ctx.xmm[{d}].f[0], {rhs});")
        return lines
    if m == "sqrtss":
        d = xmm_index(ops[0].reg)
        rhs = f"ctx.xmm[{xmm_index(ops[1].reg)}].f[0]" if ops[1].type == X86_OP_REG else f"rdf32({ea_expr(ops[1])})"
        emit(f"ctx.xmm[{d}].f[0] = std::sqrt({rhs});")
        return lines
    if m in ("addps", "subps", "mulps", "divps"):
        d = xmm_index(ops[0].reg)
        op_c = {"addps": "+", "subps": "-", "mulps": "*", "divps": "/"}[m]
        rhs = f"ctx.xmm[{xmm_index(ops[1].reg)}]" if ops[1].type == X86_OP_REG else f"rd_xmm({ea_expr(ops[1])})"
        emit(f"{{ XmmReg _r = {rhs}; for (int _k=0;_k<4;++_k) ctx.xmm[{d}].f[_k] {op_c}= _r.f[_k]; }}")
        return lines
    if m in ("maxps", "minps"):
        d = xmm_index(ops[0].reg)
        fn = "std::max" if m == "maxps" else "std::min"
        rhs = f"ctx.xmm[{xmm_index(ops[1].reg)}]" if ops[1].type == X86_OP_REG else f"rd_xmm({ea_expr(ops[1])})"
        emit(f"{{ XmmReg _r = {rhs}; for (int _k=0;_k<4;++_k) ctx.xmm[{d}].f[_k] = {fn}(ctx.xmm[{d}].f[_k], _r.f[_k]); }}")
        return lines
    if m == "shufps":
        d = xmm_index(ops[0].reg)
        rhs = f"ctx.xmm[{xmm_index(ops[1].reg)}]" if ops[1].type == X86_OP_REG else f"rd_xmm({ea_expr(ops[1])})"
        imm = ops[2].imm & 0xFF
        emit(f"{{ XmmReg _a = ctx.xmm[{d}], _b = {rhs}; uint8_t _i = 0x{imm:02x}u; ctx.xmm[{d}].u[0]=_a.u[_i&3]; ctx.xmm[{d}].u[1]=_a.u[(_i>>2)&3]; ctx.xmm[{d}].u[2]=_b.u[(_i>>4)&3]; ctx.xmm[{d}].u[3]=_b.u[(_i>>6)&3]; }}")
        return lines
    if m == "unpcklps":
        d = xmm_index(ops[0].reg)
        rhs = f"ctx.xmm[{xmm_index(ops[1].reg)}]" if ops[1].type == X86_OP_REG else f"rd_xmm({ea_expr(ops[1])})"
        emit(f"{{ XmmReg _a = ctx.xmm[{d}], _b = {rhs}; ctx.xmm[{d}].u[0]=_a.u[0]; ctx.xmm[{d}].u[1]=_b.u[0]; ctx.xmm[{d}].u[2]=_a.u[1]; ctx.xmm[{d}].u[3]=_b.u[1]; }}")
        return lines
    if m == "unpckhps":
        d = xmm_index(ops[0].reg)
        rhs = f"ctx.xmm[{xmm_index(ops[1].reg)}]" if ops[1].type == X86_OP_REG else f"rd_xmm({ea_expr(ops[1])})"
        emit(f"{{ XmmReg _a = ctx.xmm[{d}], _b = {rhs}; ctx.xmm[{d}].u[0]=_a.u[2]; ctx.xmm[{d}].u[1]=_b.u[2]; ctx.xmm[{d}].u[2]=_a.u[3]; ctx.xmm[{d}].u[3]=_b.u[3]; }}")
        return lines
    if m == "cvtsi2ss":
        d = xmm_index(ops[0].reg)
        emit(f"ctx.xmm[{d}].f[0] = static_cast<float>(static_cast<int32_t>({read_op(ops[1], 4)}));")
        return lines
    if m == "cvttss2si":
        rhs = f"ctx.xmm[{xmm_index(ops[1].reg)}].f[0]" if ops[1].type == X86_OP_REG else f"rdf32({ea_expr(ops[1])})"
        emit(write_op(ops[0], f"static_cast<uint32_t>(static_cast<int32_t>(std::trunc({rhs})))", 4))
        return lines

    raise ValueError(f"Unhandled mnemonic at 0x{addr:08X}: {m} {ins.op_str}")

def main():
    print("Loading XBE and Ghidra DB...")
    xbe, secs, tva, tend, tb = load_xbe()
    db = json.loads(GHIDRA_DB_PATH.read_text(encoding="utf-8"))

    # ---- Section selection -------------------------------------------------
    want = list(LIFT_SECTIONS)
    argv = sys.argv[1:]
    for i, a in enumerate(argv):
        if a == "--sections" and i + 1 < len(argv):
            want = [s.strip() for s in argv[i + 1].split(",") if s.strip()]
    targets = []
    for spec in want:
        if spec.endswith("*"):
            targets += [n for n in secs if n.startswith(spec[:-1])]
        elif spec in secs:
            targets.append(spec)
        else:
            print(f"  [skip] no section named {spec!r}")
    targets = sorted(dict.fromkeys(targets), key=lambda n: secs[n][0])
    # A section is only liftable if the XBE marks it executable (flags bit 0x4),
    # and never if it is a known data blob that merely carries the exec bit.
    exec_targets = [n for n in targets
                    if (secs[n][4] & 0x04) and n.split("#")[0] not in NOT_CODE_SECTIONS]
    for n in targets:
        if n not in exec_targets:
            print(f"  [skip] {n} is data, not code (flags=0x{secs[n][4]:X})")
    targets = exec_targets
    print(f"Lifting {len(targets)} section(s): {', '.join(targets)}")

    print("Tracing CFG and recovering functions...")
    lifted_funcs: dict = {}
    all_switches: dict = {}
    func_entries: set = set()
    md_seed = Cs(CS_ARCH_X86, CS_MODE_32)
    md_seed.detail = True
    for name in targets:
        sva, svsz, sroff, srsz, _sflags = secs[name]
        sec_tb = xbe[sroff : sroff + srsz]
        extra_s = discover_data_fn_seeds(xbe, secs, sec_tb, sva, sva + srsz, md_seed) if name == ".text" else None
        f, sw, ent = recover_functions(sec_tb, sva, sva + srsz, db, extra_s)
        print(f"  {name:12} 0x{sva:08X}..0x{sva + srsz:08X}  "
              f"bytes={srsz:8d}  funcs={len(f):5d}")
        lifted_funcs.update(f)
        all_switches.update(sw)
        func_entries |= ent

    sorted_entries = sorted(lifted_funcs.keys())
    total_insns = sum(len(insns) for insns, _ in lifted_funcs.values())
    cov_bytes = len({a + k for insns, _ in lifted_funcs.values() for a, i in insns.items() for k in range(i.size)})
    lift_bytes = sum(secs[n][3] for n in targets)
    print(f"Lifted functions: {len(sorted_entries)}, total instructions: {total_insns}, "
          f"covered {cov_bytes}/{lift_bytes} lifted-section bytes ({100.0 * cov_bytes / max(1, lift_bytes):.2f}%)")

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for old in OUT_DIR.glob("recomp_*.cpp"):
        old.unlink()

    # Partition evenly by instruction count into NUM_SHARDS shards
    shards = [[] for _ in range(NUM_SHARDS)]
    shard_load = [0] * NUM_SHARDS
    for idx, ea in enumerate(sorted_entries):
        # Greedy balance OR contiguous
        s_idx = min(range(NUM_SHARDS), key=lambda k: shard_load[k])
        shards[s_idx].append(ea)
        shard_load[s_idx] += len(lifted_funcs[ea][0])

    unhandled = []
    # Emit each shard
    for s_idx, f_list in enumerate(shards):
        f_list.sort()
        lines = [
            '#include "cpu_helpers.h"',
            "",
        ]
        for ea in f_list:
            insns, sw_used = lifted_funcs[ea]
            addrs = sorted(insns.keys())
            huge_fn = len(insns) >= 600
            if huge_fn:
                lines.append("#if defined(_MSC_VER)")
                lines.append('#pragma optimize("", off)')
                lines.append("#endif")
            lines.append(f"void fn_{ea:08x}(CpuContext& ctx) {{")
            # Emit every instruction with its label if jumped to (or reached via non-adjacent fallthrough)
            label_targets = set()
            for i_idx, a in enumerate(addrs):
                ins = insns[a]
                if ins.mnemonic == "jmp" or ins.mnemonic in JCC_MAP or ins.mnemonic == "jecxz":
                    if ins.operands and ins.operands[0].type == X86_OP_IMM:
                        t = u32(ins.operands[0].imm)
                        if t in insns:
                            label_targets.add(t)
                if a in sw_used:
                    for t in sw_used[a]:
                        if t in insns:
                            label_targets.add(t)
                nxt_a = a + ins.size
                is_term = (ins.mnemonic.startswith("ret") or ins.mnemonic == "jmp" or ins.mnemonic == "int3")
                if not is_term:
                    next_list_a = addrs[i_idx + 1] if i_idx + 1 < len(addrs) else None
                    if nxt_a != next_list_a and nxt_a in insns:
                        label_targets.add(nxt_a)

            # Check entry falls into instruction order: if entry != addrs[0], jump to entry!
            if addrs and addrs[0] != ea:
                label_targets.add(ea)
                lines.append(f"    goto L_{ea:08x};")

            prev_end = None
            for i_idx, a in enumerate(addrs):
                ins = insns[a]
                if prev_end is not None and a != prev_end and ins.address not in label_targets:
                    pass
                if a in label_targets:
                    lines.append(f"L_{a:08x}:;")
                lines.append(f"    ctx.last_pc = 0x{a:08x}u;")
                try:
                    stmt = translate_insn(ins, insns, lifted_funcs, sw_used)
                    lines.extend(stmt)
                except Exception as ex:
                    unhandled.append((a, ins.mnemonic, ins.op_str, str(ex)))
                    lines.append(f"    // ERR: {ins.mnemonic} {ins.op_str}")
                # Check if previous/current instruction falls through across a gap or into another function entry!
                nxt_a = a + ins.size
                is_term = (ins.mnemonic.startswith("ret") or ins.mnemonic == "jmp" or ins.mnemonic == "int3")
                if not is_term:
                    next_list_a = addrs[i_idx + 1] if i_idx + 1 < len(addrs) else None
                    if nxt_a != next_list_a:
                        if nxt_a in insns:
                            lines.append(f"    goto L_{nxt_a:08x};")
                        elif nxt_a in lifted_funcs:
                            lines.append(f"    recomp_dispatch_call(ctx, 0x{nxt_a:08x}u); return;")
                prev_end = nxt_a
            lines.append("}")
            if huge_fn:
                lines.append("#if defined(_MSC_VER)")
                lines.append('#pragma optimize("", on)')
                lines.append("#endif")
            lines.append("")

        lines.append(f"void recomp_register_shard_{s_idx:02d}() {{")
        for ea in f_list:
            lines.append(f"    recomp_register_fn(0x{ea:08x}u, fn_{ea:08x});")
        lines.append("}")
        lines.append("")
        (OUT_DIR / f"recomp_{s_idx:02d}.cpp").write_text("\n".join(lines), encoding="utf-8")

    # Emit recomp_dispatch.cpp (NOTE: HLE registration happens first or after so HLE overrides win!)
    sec_tbl = [
        f'    {{ 0x{secs[n][0]:08x}u, 0x{secs[n][0] + secs[n][3]:08x}u, "{n}" }},'
        for n in sorted(secs, key=lambda k: secs[k][0])
    ]
    disp = [
        '#include "cpu_context.h"',
        "#include <cstdio>",
        "#include <cstdlib>",
        "#include <cstring>",
        "#include <unordered_map>",
        "",
        "extern void hle_register_all();",
        "",
        "namespace {",
        "struct SecRange { uint32_t lo, hi; const char* name; };",
        "// Every section of the loaded XBE, in address order (generated).",
        "static const SecRange kSecRanges[] = {",
        *sec_tbl,
        "};",
        "",
        "const char* recomp_section_of(uint32_t va) {",
        "    for (const auto& s : kSecRanges)",
        "        if (va >= s.lo && va < s.hi) return s.name;",
        "    return nullptr;",
        "}",
        "",
        "constexpr uint32_t kLutBase = 0x00010000u;",
        "constexpr uint32_t kLutEnd  = 0x002D0000u;",
        "RecompFn g_fn_lut[kLutEnd - kLutBase]{};",
        "",
        "std::unordered_map<uint32_t, RecompFn>& fn_table() {",
        "    static std::unordered_map<uint32_t, RecompFn> t;",
        "    return t;",
        "}",
        "} // namespace",
        "",
        "const char* recomp_section_name(uint32_t va) { return recomp_section_of(va); }",
        "",
        "bool recomp_has_fn(uint32_t va) {",
        "    uint32_t off = va - kLutBase;",
        "    if (off < (kLutEnd - kLutBase)) return g_fn_lut[off] != nullptr;",
        "    auto& tbl = fn_table();",
        "    return tbl.find(va) != tbl.end();",
        "}",
        "",
        "void recomp_register_fn(uint32_t va, RecompFn fn) {",
        "    uint32_t off = va - kLutBase;",
        "    if (off < (kLutEnd - kLutBase)) g_fn_lut[off] = fn;",
        "    fn_table()[va] = fn;",
        "}",
        "",
        "// Log the guest stack words that look like return addresses into a known function.",
        "// Guest calls push a literal return VA, so a stack scan recovers an approximate",
        "// backtrace even though the lifted code has no frame pointers.",
        "static void dump_guest_backtrace(CpuContext& ctx, uint32_t depth_bytes) {",
        "    std::fprintf(stderr, \"        guest stack candidates (esp=0x%08X):\\n\", ctx.esp);",
        "    int shown = 0;",
        "    for (uint32_t off = 0; off < depth_bytes; off += 4) {",
        "        uint32_t v = rd32(ctx.esp + off);",
        "        const char* sec = recomp_section_of(v);",
        "        if (!sec) continue;",
        "        bool known = recomp_has_fn(v);",
        "        std::fprintf(stderr, \"          [esp+0x%03X] = 0x%08X  %-8s %s\\n\",",
        "                     off, v, sec, known ? \"(function entry)\" : \"(code, not an entry)\");",
        "        if (++shown >= 24) break;",
        "    }",
        "    if (!shown) std::fprintf(stderr, \"          <none - stack not yet written>\\n\");",
        "}",
        "",
        "void recomp_dispatch_call(CpuContext& ctx, uint32_t target_va) {",
        "    uint32_t off = target_va - kLutBase;",
        "    if (off < (kLutEnd - kLutBase)) {",
        "        RecompFn fn = g_fn_lut[off];",
        "        if (fn) { fn(ctx); return; }",
        "    } else {",
        "        auto& tbl = fn_table();",
        "        auto it = tbl.find(target_va);",
        "        if (it != tbl.end()) { it->second(ctx); return; }",
        "    }",
        "    const char* sec = recomp_section_of(target_va);",
        "    std::fprintf(stderr,",
        "        \"\\n[FATAL] Unregistered call target 0x%08X (%s)\\n\"",
        "        \"        from last_pc=0x%08X (%s)  eax=0x%08X ecx=0x%08X edx=0x%08X ebx=0x%08X\\n\"",
        "        \"        esi=0x%08X edi=0x%08X ebp=0x%08X esp=0x%08X\\n\",",
        "        target_va, sec ? sec : \"outside every XBE section\",",
        "        ctx.last_pc, recomp_section_of(ctx.last_pc) ? recomp_section_of(ctx.last_pc) : \"?\",",
        "        ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esi, ctx.edi, ctx.ebp, ctx.esp);",
        "    dump_guest_backtrace(ctx, 0x300u);",
        "    std::fflush(stderr);",
        "    if (const char* mode = std::getenv(\"WHACKED_ON_UNKNOWN\")) {",
        "        if (std::strcmp(mode, \"zero\") == 0) { ctx.eax = 0; return; }",
        "    }",
        "    std::abort();",
        "}",
        "",
        "uint32_t recomp_invoke_cdecl(CpuContext& ctx, uint32_t va) {",
        "    ctx.push32(0xDEAD0001u);",
        "    recomp_dispatch_call(ctx, va);",
        "    return ctx.eax;",
        "}",
        "",
        "uint32_t recomp_invoke_stdcall(CpuContext& ctx, uint32_t va) {",
        "    ctx.push32(0xDEAD0002u);",
        "    recomp_dispatch_call(ctx, va);",
        "    return ctx.eax;",
        "}",
        "",
    ]
    for s_idx in range(NUM_SHARDS):
        disp.append(f"extern void recomp_register_shard_{s_idx:02d}();")
    disp.append("")
    disp.append("void recomp_register_all() {")
    disp.append("    fn_table().reserve(16384);")
    for s_idx in range(NUM_SHARDS):
        disp.append(f"    recomp_register_shard_{s_idx:02d}();")
    disp.append("    hle_register_all(); // HLE hooks override recompiled thunks")
    disp.append("}")
    disp.append("")
    (OUT_DIR / "recomp_dispatch.cpp").write_text("\n".join(disp), encoding="utf-8")

    print(f"Generated {NUM_SHARDS} shards + recomp_dispatch.cpp in {OUT_DIR}")
    print(f"Unhandled instructions: {len(unhandled)}")
    for u in unhandled[:20]:
        print(" ", u)
    if unhandled:
        sys.exit(2)

if __name__ == "__main__":
    main()
