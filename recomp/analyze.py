"""Whole-program analysis: decode .text, find functions, gather stats."""
import bisect
import struct
import sys
from collections import Counter

from . import elf, mips


def eh_frame_functions(img):
    """(start, size) of every FDE; pc_begin is absolute after relocation."""
    sec = img.sections[".eh_frame"]
    m, o = img.mem, sec.addr - img.base
    end = o + sec.size
    out = []
    while o < end:
        length, = struct.unpack_from("<I", m, o)
        if length == 0:
            break
        cie_id, = struct.unpack_from("<I", m, o + 4)
        if cie_id != 0:
            out.append(struct.unpack_from("<II", m, o + 8))
        o += 4 + length
    return out


def import_stubs(img):
    """stub address -> (library, nid)"""
    sec = img.sections[".lib.stub"]
    stubs = {}
    o, end = sec.addr, sec.addr + sec.size
    while o < end:
        name_va, _ver, _attr, entsz, _nv, nfuncs, nids_va, stubs_va = struct.unpack_from(
            "<IHHBBHII", img.mem, o - img.base)
        lib = img.cstr(name_va)
        for k in range(nfuncs):
            stubs[stubs_va + 8 * k] = (lib, img.u32(nids_va + 4 * k))
        o += (entsz or 5) * 4
    return stubs


class Program:
    def __init__(self, path):
        self.img = img = elf.load(path)
        text = img.sections[".text"]
        self.text_start, self.text_end = text.addr, text.addr + text.size
        self.insns = {}
        for a in range(self.text_start, self.text_end, 4):
            self.insns[a] = mips.decode(a, img.u32(a))
        self.stubs = import_stubs(img)
        self.jump_tables = self._find_jump_tables()
        self._find_functions()

    def in_text(self, a):
        return self.text_start <= a < self.text_end

    def _find_jump_tables(self):
        """jr address -> list of case targets.

        GCC emits: sltiu at,idx,N / beqz at,default / sll / lui+addu / lw r,%lo(T)(at) / jr r
        """
        img, tables = self.img, {}
        for a, i in self.insns.items():
            if i.op != "jr" or i.rs == 31:
                continue
            table = count = lw_off = None
            for back in range(1, 24):
                j = self.insns.get(a - 4 * back)
                if j is None or (j.op in ("jr", "j") and back > 1):
                    break
                rel = img.relocs.get(j.addr)
                lo = rel[1] if rel and rel[0] == elf.R_MIPS_LO16 else None
                if lw_off is None and j.op == "lw" and j.rt == i.rs:
                    if lo is not None:  # lw r, %lo(T)(at)
                        table = lo
                    lw_off = 0 if lo is not None else j.imm
                elif table is None and lw_off is not None and lo is not None and j.op == "addiu":
                    table = lo + lw_off  # addiu at, at, %lo(T); lw r, 0(at)
                elif table is not None and j.op == "sltiu":
                    count = j.imm
                    break
            if table is None or count is None:
                continue
            tables[a] = [img.u32(table + 4 * k) for k in range(count)]
        return tables

    def _find_functions(self):
        img = self.img
        case_labels = {t for targets in self.jump_tables.values() for t in targets}
        starts = {}
        for pc, size in eh_frame_functions(img):
            if self.in_text(pc):
                starts[pc] = "eh_frame"
        for i in self.insns.values():
            if i.op == "jal" and self.in_text(i.target):
                starts.setdefault(i.target, "jal")
        # pointers to code stored in data (vtables, callbacks, ctors)
        for loc, (typ, target) in img.relocs.items():
            if target in case_labels:
                continue
            if typ == elf.R_MIPS_32 and self.in_text(target) and not self.in_text(loc):
                starts.setdefault(target, "dataptr")
            # la/addiu pairs building a code address (callbacks passed as args)
            if typ == elf.R_MIPS_LO16 and self.in_text(target) and self.in_text(loc):
                starts.setdefault(target, "codeptr")
        for s in (".init", ".fini"):
            starts.setdefault(img.sections[s].addr, "section")
        starts.setdefault(img.entry, "entry")

        # .eh_frame boundaries are authoritative: anything else landing strictly
        # inside one is a label (unrecognised jump table, computed goto), not a function
        eh = sorted((a, a + sz) for a, sz in eh_frame_functions(img) if self.in_text(a))
        eh_starts = [a for a, _ in eh]
        self.internal_labels = set(case_labels)
        for a in [a for a, src in starts.items() if src != "eh_frame"]:
            k = bisect.bisect_right(eh_starts, a) - 1
            if k >= 0 and eh[k][0] < a < eh[k][1]:
                del starts[a]
                self.internal_labels.add(a)
        self.func_starts = starts
        ordered = sorted(starts)
        self.functions = [(a, (ordered[k + 1] if k + 1 < len(ordered) else self.text_end) - a)
                          for k, a in enumerate(ordered)]


def main(path):
    p = Program(path)
    ops = Counter(i.op for i in p.insns.values())
    total = len(p.insns)
    print(f".text: {total} instructions, {len(p.functions)} functions")
    print("  by source:", dict(Counter(p.func_starts.values())))
    unknown = ops.pop("?", 0)
    print(f"  undecoded: {unknown}")
    vfpu = sum(n for o, n in ops.items() if o.startswith(("vfpu", "lv", "sv", "mfv", "mtv", "bv")))
    fpu = sum(n for o, n in ops.items() if o.endswith(".s") or o in ("lwc1", "swc1", "mfc1", "mtc1", "cvt.s.w") or o.startswith("bc1"))
    print(f"  FPU: {fpu} ({100 * fpu / total:.1f}%)  VFPU: {vfpu} ({100 * vfpu / total:.1f}%)")
    indirect = sum(1 for i in p.insns.values() if i.op == "jr" and i.rs != 31)
    print(f"  jr non-$ra: {indirect}, recognised jump tables: {len(p.jump_tables)}")
    print("  jalr (indirect calls):", ops.get("jalr", 0))
    print("  syscall:", ops.get("syscall", 0), " imports:", len(p.stubs))
    print("  top ops:", ", ".join(f"{o} {n}" for o, n in ops.most_common(40)))


if __name__ == "__main__":
    main(sys.argv[1])
