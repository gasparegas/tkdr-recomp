"""Translate analysed MIPS functions into C.

Each guest function becomes `void f_XXXXXXXX(Ctx *c)`. Branches inside a
function become gotos, jal becomes a direct C call, jr $ra a return. Delay
slots are emitted before the transfer, with the branch condition captured first.
"""
import json
import os
import sys
from collections import Counter

from . import analyze

R = "c->r[{}]".format

# RECOMP_CHECK=1: verify every function preserves $s0-$s7, $sp and $fp
CHECK_CALLEE_SAVED = os.environ.get("RECOMP_CHECK") == "1"
# RECOMP_PROBE=addr,addr: log arguments on entry to these functions
PROBES = {int(a, 16) for a in os.environ.get("RECOMP_PROBE", "").split(",") if a}


def S(n):
    return f"(int32_t)c->r[{n}]"


def F(n):
    return f"c->f[{n}].f"


def FU(n):
    return f"c->f[{n}].u"


def addr_of(i):
    return f"{R(i.rs)} + {i.imm}" if i.imm else R(i.rs)


def body(i):
    """C for one non-control-flow instruction (None = unsupported)."""
    op, rs, rt, rd, sa = i.op, i.rs, i.rt, i.rd, i.sa
    u16 = i.raw & 0xFFFF

    def setr(reg, expr):
        return f"{R(reg)} = {expr};" if reg else ";"

    simple = {
        "addu": lambda: setr(rd, f"{R(rs)} + {R(rt)}"),
        "add": lambda: setr(rd, f"{R(rs)} + {R(rt)}"),
        "subu": lambda: setr(rd, f"{R(rs)} - {R(rt)}"),
        "sub": lambda: setr(rd, f"{R(rs)} - {R(rt)}"),
        "and": lambda: setr(rd, f"{R(rs)} & {R(rt)}"),
        "or": lambda: setr(rd, f"{R(rs)} | {R(rt)}"),
        "xor": lambda: setr(rd, f"{R(rs)} ^ {R(rt)}"),
        "nor": lambda: setr(rd, f"~({R(rs)} | {R(rt)})"),
        "slt": lambda: setr(rd, f"{S(rs)} < {S(rt)}"),
        "sltu": lambda: setr(rd, f"{R(rs)} < {R(rt)}"),
        "max": lambda: setr(rd, f"{S(rs)} > {S(rt)} ? {R(rs)} : {R(rt)}"),
        "min": lambda: setr(rd, f"{S(rs)} < {S(rt)} ? {R(rs)} : {R(rt)}"),
        "movz": lambda: f"if (!{R(rt)}) {setr(rd, R(rs))}",
        "movn": lambda: f"if ({R(rt)}) {setr(rd, R(rs))}",
        "sll": lambda: setr(rd, f"{R(rt)} << {sa}"),
        "srl": lambda: setr(rd, f"{R(rt)} >> {sa}"),
        "sra": lambda: setr(rd, f"(uint32_t)({S(rt)} >> {sa})"),
        "rotr": lambda: setr(rd, f"({R(rt)} >> {sa}) | ({R(rt)} << {(32 - sa) & 31})") if sa else setr(rd, R(rt)),
        "sllv": lambda: setr(rd, f"{R(rt)} << ({R(rs)} & 31)"),
        "srlv": lambda: setr(rd, f"{R(rt)} >> ({R(rs)} & 31)"),
        "srav": lambda: setr(rd, f"(uint32_t)({S(rt)} >> ({R(rs)} & 31))"),
        "rotrv": lambda: setr(rd, f"({R(rt)} >> ({R(rs)} & 31)) | ({R(rt)} << ((32 - ({R(rs)} & 31)) & 31))"),
        "clz": lambda: setr(rd, f"clz32({R(rs)})"),
        "clo": lambda: setr(rd, f"clz32(~{R(rs)})"),
        "mfhi": lambda: setr(rd, "c->hi"),
        "mflo": lambda: setr(rd, "c->lo"),
        "mthi": lambda: f"c->hi = {R(rs)};",
        "mtlo": lambda: f"c->lo = {R(rs)};",
        "mult": lambda: f"{{ int64_t p = (int64_t){S(rs)} * {S(rt)}; c->lo = (uint32_t)p; c->hi = (uint32_t)((uint64_t)p >> 32); }}",
        "multu": lambda: f"{{ uint64_t p = (uint64_t){R(rs)} * {R(rt)}; c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); }}",
        "madd": lambda: f"acc_add(c, (int64_t){S(rs)} * {S(rt)});",
        "maddu": lambda: f"acc_add(c, (int64_t)((uint64_t){R(rs)} * {R(rt)}));",
        "msub": lambda: f"acc_add(c, -((int64_t){S(rs)} * {S(rt)}));",
        "msubu": lambda: f"acc_add(c, -(int64_t)((uint64_t){R(rs)} * {R(rt)}));",
        "div": lambda: f"do_div(c, {S(rs)}, {S(rt)});",
        "divu": lambda: f"do_divu(c, {R(rs)}, {R(rt)});",
        "addiu": lambda: setr(rt, f"{R(rs)} + {i.imm}"),
        "addi": lambda: setr(rt, f"{R(rs)} + {i.imm}"),
        "slti": lambda: setr(rt, f"{S(rs)} < {i.imm}"),
        "sltiu": lambda: setr(rt, f"{R(rs)} < {i.imm & 0xFFFFFFFF}u"),
        "andi": lambda: setr(rt, f"{R(rs)} & {u16:#x}"),
        "ori": lambda: setr(rt, f"{R(rs)} | {u16:#x}"),
        "xori": lambda: setr(rt, f"{R(rs)} ^ {u16:#x}"),
        "lui": lambda: setr(rt, f"{u16 << 16:#010x}u"),
        "lb": lambda: setr(rt, f"(uint32_t)(int8_t)mem_r8({addr_of(i)})"),
        "lbu": lambda: setr(rt, f"mem_r8({addr_of(i)})"),
        "lh": lambda: setr(rt, f"(uint32_t)(int16_t)mem_r16({addr_of(i)})"),
        "lhu": lambda: setr(rt, f"mem_r16({addr_of(i)})"),
        "lw": lambda: setr(rt, f"mem_r32({addr_of(i)})"),
        "ll": lambda: setr(rt, f"mem_r32({addr_of(i)})"),
        "lwl": lambda: setr(rt, f"do_lwl({R(rt)}, {addr_of(i)})"),
        "lwr": lambda: setr(rt, f"do_lwr({R(rt)}, {addr_of(i)})"),
        "sb": lambda: f"mem_w8({addr_of(i)}, (uint8_t){R(rt)});",
        "sh": lambda: f"mem_w16({addr_of(i)}, (uint16_t){R(rt)});",
        "sw": lambda: f"mem_w32({addr_of(i)}, {R(rt)});",
        "sc": lambda: f"mem_w32({addr_of(i)}, {R(rt)}); {setr(rt, '1')}",
        "swl": lambda: f"do_swl({R(rt)}, {addr_of(i)});",
        "swr": lambda: f"do_swr({R(rt)}, {addr_of(i)});",
        "nop": lambda: ";",
        "sync": lambda: ";",
        "cache": lambda: ";",
        "mfic": lambda: setr(rt, "c->ic"),
        "mtic": lambda: f"c->ic = {R(rt)};",
        "ext": lambda: setr(rt, f"({R(rs)} >> {sa}) & {(1 << (rd + 1)) - 1 & 0xFFFFFFFF:#x}u"),
        "ins": lambda: _ins(i),
        "seb": lambda: setr(rd, f"(uint32_t)(int8_t){R(rt)}"),
        "seh": lambda: setr(rd, f"(uint32_t)(int16_t){R(rt)}"),
        "wsbh": lambda: setr(rd, f"(({R(rt)} & 0x00FF00FFu) << 8) | (({R(rt)} >> 8) & 0x00FF00FFu)"),
        "wsbw": lambda: setr(rd, f"__builtin_bswap32({R(rt)})"),
        "bitrev": lambda: setr(rd, f"bitrev32({R(rt)})"),
        # FPU: fmt S uses fs = rd, ft = rt, fd = sa
        "lwc1": lambda: f"{FU(rt)} = mem_r32({addr_of(i)});",
        "swc1": lambda: f"mem_w32({addr_of(i)}, {FU(rt)});",
        "mfc1": lambda: setr(rt, FU(rd)),
        "mtc1": lambda: f"{FU(rd)} = {R(rt)};",
        "cfc1": lambda: setr(rt, "c->fcr31 | (c->fcc << 23)" if rd == 31 else "0x3351"),
        "ctc1": lambda: f"c->fcr31 = {R(rt)} & ~(1u << 23); c->fcc = ({R(rt)} >> 23) & 1;" if rd == 31 else ";",
        "add.s": lambda: f"{F(sa)} = {F(rd)} + {F(rt)};",
        "sub.s": lambda: f"{F(sa)} = {F(rd)} - {F(rt)};",
        "mul.s": lambda: f"{F(sa)} = {F(rd)} * {F(rt)};",
        "div.s": lambda: f"{F(sa)} = {F(rd)} / {F(rt)};",
        "sqrt.s": lambda: f"{F(sa)} = sqrtf({F(rd)});",
        "abs.s": lambda: f"{F(sa)} = fabsf({F(rd)});",
        "mov.s": lambda: f"{FU(sa)} = {FU(rd)};",
        "neg.s": lambda: f"{FU(sa)} = {FU(rd)} ^ 0x80000000u;",
        "round.w.s": lambda: f"{FU(sa)} = (uint32_t)f2i({F(rd)}, 0);",
        "trunc.w.s": lambda: f"{FU(sa)} = (uint32_t)f2i({F(rd)}, 1);",
        "ceil.w.s": lambda: f"{FU(sa)} = (uint32_t)f2i({F(rd)}, 2);",
        "floor.w.s": lambda: f"{FU(sa)} = (uint32_t)f2i({F(rd)}, 3);",
        "cvt.w.s": lambda: f"{FU(sa)} = (uint32_t)f2i({F(rd)}, (int)(c->fcr31 & 3));",
        "cvt.s.w": lambda: f"{F(sa)} = (float){c_s(FU(rd))};",
    }
    if op in simple:
        return simple[op]()
    if op.startswith("c.") and op.endswith(".s"):
        return f"c->fcc = (uint32_t)fcmp({F(rd)}, {F(rt)}, {i.raw & 7});"
    if op.startswith(("vfpu", "lv", "sv", "mfv", "mtv")):
        return f"vfpu_exec(c, {i.addr:#010x}u, {i.raw:#010x}u);"
    if op in ("syscall", "break", "halt", "cop0", "?"):
        return f'recomp_trap(c, {i.addr:#010x}u, "{op}");'
    return None


def c_s(expr):
    return f"(int32_t){expr}"


def _ins(i):
    pos, size = i.sa, i.rd - i.sa + 1
    mask = ((1 << size) - 1) << pos & 0xFFFFFFFF
    if not i.rt:
        return ";"
    return (f"{R(i.rt)} = ({R(i.rt)} & {~mask & 0xFFFFFFFF:#x}u) | "
            f"(({R(i.rs)} << {pos}) & {mask:#x}u);")


BRANCH_BASE = {
    "beql": "beq", "bnel": "bne", "blezl": "blez", "bgtzl": "bgtz", "bltzl": "bltz",
    "bgezl": "bgez", "bltzal": "bltz", "bgezal": "bgez", "bltzall": "bltz", "bgezall": "bgez",
    "bc1fl": "bc1f", "bc1tl": "bc1t", "bvfl": "bvf", "bvtl": "bvt",
}


def cond(i):
    """C condition of a conditional branch."""
    base = BRANCH_BASE.get(i.op, i.op)
    rs, rt = i.rs, i.rt
    return {
        "beq": f"{R(rs)} == {R(rt)}",
        "bne": f"{R(rs)} != {R(rt)}",
        "blez": f"{S(rs)} <= 0",
        "bgtz": f"{S(rs)} > 0",
        "bltz": f"{S(rs)} < 0",
        "bgez": f"{S(rs)} >= 0",
        "bc1f": "!c->fcc",
        "bc1t": "c->fcc",
        "bvf": f"!((c->vfpu_ctrl[3] >> {(rt >> 2) & 7}) & 1)",
        "bvt": f"((c->vfpu_ctrl[3] >> {(rt >> 2) & 7}) & 1)",
    }[base]


class FunctionEmitter:
    def __init__(self, prog, start, size, stats):
        self.p, self.start, self.end, self.stats = prog, start, start + size, stats
        self.lines = []

    def inside(self, a):
        return self.start <= a < self.end

    def fname(self, a):
        return f"f_{a:08X}"

    def call(self, target, ret_addr):
        p = self.p
        if target in p.stubs:
            return f"hle_call(c, {p.stub_index[target]});"
        if target in p.func_starts:
            return f"{self.fname(target)}(c);"
        self.stats["call_to_non_function"] += 1
        return f"call_indirect(c, {target:#010x}u);"

    def tail(self, target):
        if self.inside(target):
            return f"goto L_{target:08X};"
        if target in self.p.func_starts or target in self.p.stubs:
            return self.call(target, 0) + " return;"
        self.stats["jump_to_non_function"] += 1
        return f"call_indirect(c, {target:#010x}u); return;"

    def ins_body(self, i):
        b = body(i)
        if b is None:
            self.stats["unsupported:" + i.op] += 1
            return f'recomp_trap(c, {i.addr:#010x}u, "unsupported {i.op}");'
        return b

    def emit(self):
        p, out = self.p, self.lines
        insns = [p.insns[a] for a in range(self.start, self.end, 4)]
        labels = set()
        for i in insns:
            if i.is_branch or i.op == "j":
                if self.inside(i.target):
                    labels.add(i.target)
            if i.op == "jr" and i.addr in p.jump_tables:
                labels.update(t for t in p.jump_tables[i.addr] if self.inside(t))
        labels |= {a for a in p.internal_labels if self.inside(a)}

        out.append(f"void {self.fname(self.start)}(Ctx *c) {{")
        if self.start in PROBES:
            out.append(f"    recomp_probe(c, {self.start:#010x}u);")
        first = p.insns[self.start]
        self.checked = CHECK_CALLEE_SAVED and first.op == "addiu" and first.rs == 29 and first.rt == 29 and first.imm < 0
        if self.checked:
            out.append("    uint32_t saved_[10]; for (int k_ = 0; k_ < 8; k_++) saved_[k_] = c->r[16 + k_];"
                       " saved_[8] = c->r[29]; saved_[9] = c->r[30];")
        k, n = 0, len(insns)
        while k < n:
            i = insns[k]
            if i.addr in labels:
                out.append(f"L_{i.addr:08X}:;")
            if not i.has_delay_slot:
                out.append("    " + self.ins_body(i))
                k += 1
                continue
            ds = insns[k + 1] if k + 1 < n else None
            ds_c = self.ins_body(ds) if ds else ";"
            if ds and ds.has_delay_slot:
                self.stats["branch_in_delay_slot"] += 1
                ds_c = f'recomp_trap(c, {ds.addr:#010x}u, "branch in delay slot");'
            out.append("    " + self.transfer(i, ds_c))
            k += 2
            if ds and ds.addr in labels:
                # something jumps straight into the delay slot: emit it again
                out.append(f"    goto L_{ds.addr + 4:08X};")
                out.append(f"L_{ds.addr:08X}:;")
                out.append("    " + ds_c)
                labels.add(ds.addr + 4)
        # fell off the end: continue into the next function
        out.append(f"    {self.tail(self.end)}" if self.end < p.text_end else "    return;")
        out.append("}")
        return out

    def transfer(self, i, ds):
        op = i.op
        if op == "j":
            return f"{{ {ds} {self.tail(i.target)} }}"
        if op == "jal":
            return f"{{ {R(31)} = {i.addr + 8:#010x}u; {ds} {self.call(i.target, i.addr + 8)} }}"
        if op == "jr":
            if i.rs == 31:
                if self.start in PROBES:
                    return f"{{ {ds} recomp_probe_ret(c, {self.start:#010x}u); return; }}"
                if self.checked:
                    return (f"{{ {ds} check_saved(c, saved_, {self.start:#010x}u, {i.addr:#010x}u); return; }}")
                return f"{{ {ds} return; }}"
            t = f"uint32_t t = {R(i.rs)};"
            table = self.p.jump_tables.get(i.addr)
            if table:
                cases = " ".join(f"case {a:#010x}u: goto L_{a:08X};" for a in sorted(set(table)) if self.inside(a))
                return f"{{ {t} {ds} switch (t) {{ {cases} default: call_indirect(c, t); return; }} }}"
            self.stats["unresolved_jr"] += 1
            inner = sorted(a for a in self.p.internal_labels if self.inside(a))
            cases = " ".join(f"case {a:#010x}u: goto L_{a:08X};" for a in inner)
            return f"{{ {t} {ds} switch (t) {{ {cases} default: call_indirect(c, t); return; }} }}"
        if op == "jalr":
            rd = i.rd or 31
            return (f"{{ uint32_t t = {R(i.rs)}; {R(rd)} = {i.addr + 8:#010x}u; {ds} "
                    f"call_indirect(c, t); }}")
        # conditional branches
        link = op in ("bltzal", "bgezal", "bltzall", "bgezall")
        go = self.call(i.target, i.addr + 8) if link else self.tail(i.target)
        pre = f"{R(31)} = {i.addr + 8:#010x}u; " if link else ""
        if i.is_likely:
            return f"if ({cond(i)}) {{ {pre}{ds} {go} }}"
        if link:
            return f"{{ int t = {cond(i)}; {pre}{ds} if (t) {{ {go} }} }}"
        return f"{{ int t = {cond(i)}; {ds} if (t) {go} }}"


def write_if_changed(path, text):
    """Keep timestamps of unchanged files so rebuilds stay incremental."""
    try:
        if open(path).read() == text:
            return
    except OSError:
        pass
    with open(path, "w") as fh:
        fh.write(text)


def generate(elf_path, out_dir, per_file=400):
    p = analyze.Program(elf_path)
    p.stub_index = {a: k for k, a in enumerate(sorted(p.stubs))}
    os.makedirs(out_dir, exist_ok=True)
    stats = Counter()
    funcs = p.functions
    files = []
    for f0 in range(0, len(funcs), per_file):
        chunk = funcs[f0:f0 + per_file]
        name = f"funcs_{f0 // per_file:03d}.c"
        lines = ['#include "recomp.h"', ""]
        lines += [f"void f_{a:08X}(Ctx *c);" for a, _ in funcs]
        lines.append("")
        for start, size in chunk:
            lines += FunctionEmitter(p, start, size, stats).emit()
            lines.append("")
        write_if_changed(os.path.join(out_dir, name), "\n".join(lines))
        files.append(name)

    # address -> function table for indirect calls
    with open(os.path.join(out_dir, "func_table.c"), "w") as fh:
        fh.write('#include "recomp.h"\n#include "func_table.h"\n')
        fh.writelines(f"void f_{a:08X}(Ctx *c);\n" for a, _ in funcs)
        fh.write(f"const FuncEntry g_funcs[] = {{\n")
        fh.writelines(f"    {{{a:#010x}u, f_{a:08X}}},\n" for a, _ in funcs)
        fh.write(f"}};\nconst unsigned g_func_count = {len(funcs)};\n")
        fh.write(f"const uint32_t g_entry = {p.img.entry:#010x}u;\n")
        names = json.load(open(os.path.join(os.path.dirname(__file__), "nids.json")))
        fh.write("const ImportEntry g_imports[] = {\n")
        for a in sorted(p.stubs):
            lib, nid = p.stubs[a]
            name = names.get(lib, {}).get(f"0x{nid:08X}", f"{lib}_{nid:08X}")
            fh.write(f'    {{"{lib}", "{name}", {nid:#010x}u, {a:#010x}u}},\n')
        fh.write(f"}};\nconst unsigned g_import_count = {len(p.stubs)};\n")
    files.append("func_table.c")

    # relocated image (game data! stays in the build dir)
    with open(os.path.join(out_dir, "image.bin"), "wb") as fh:
        fh.write(p.img.mem)
    with open(os.path.join(out_dir, "image_base.h"), "w") as fh:
        fh.write(f"#define IMAGE_BASE {p.img.base:#010x}u\n#define IMAGE_SIZE {len(p.img.mem):#x}u\n")
    with open(os.path.join(out_dir, "sources.cmake"), "w") as fh:
        fh.write("set(GENERATED_SOURCES\n" + "".join(f"    ${{GEN_DIR}}/{f}\n" for f in files) + ")\n")
    return p, stats


def main():
    elf_path, out_dir = sys.argv[1], sys.argv[2]
    p, stats = generate(elf_path, out_dir)
    print(f"{len(p.functions)} functions written to {out_dir}")
    for k, v in sorted(stats.items()):
        print(f"  {k}: {v}")


if __name__ == "__main__":
    main()
