// Allegrex (PSP MIPS32 subset + Sony extensions) instruction decoder.
// Port of recomp/mips.py. VFPU instructions are only classified: the runtime
// interprets them (vfpu_exec).

export const BRANCHES = new Set([
    "beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal",
    "beql", "bnel", "blezl", "bgtzl", "bltzl", "bgezl", "bltzall", "bgezall",
    "bc1f", "bc1t", "bc1fl", "bc1tl", "bvf", "bvt", "bvfl", "bvtl",
]);
export const LIKELY = new Set(["beql", "bnel", "blezl", "bgtzl", "bltzl", "bgezl", "bltzall", "bgezall",
    "bc1fl", "bc1tl", "bvfl", "bvtl"]);
const JUMPS = new Set(["j", "jal", "jr", "jalr"]);

export class Insn {
    constructor(addr, raw, op, rs, rt, rd, sa, imm) {
        this.addr = addr; this.raw = raw; this.op = op;
        this.rs = rs; this.rt = rt; this.rd = rd; this.sa = sa;
        this.imm = imm;  // sign-extended where the instruction uses it signed
        this.target = 0;  // branch/jump destination
    }
    get isBranch() { return BRANCHES.has(this.op); }
    get isLikely() { return LIKELY.has(this.op); }
    get isJump() { return JUMPS.has(this.op); }
    get hasDelaySlot() { return this.isBranch || this.isJump; }
}

const SPECIAL = {
    0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav",
    0x08: "jr", 0x09: "jalr", 0x0A: "movz", 0x0B: "movn", 0x0C: "syscall", 0x0D: "break",
    0x0F: "sync", 0x10: "mfhi", 0x11: "mthi", 0x12: "mflo", 0x13: "mtlo", 0x16: "clz",
    0x17: "clo", 0x18: "mult", 0x19: "multu", 0x1A: "div", 0x1B: "divu", 0x1C: "madd",
    0x1D: "maddu", 0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu", 0x24: "and",
    0x25: "or", 0x26: "xor", 0x27: "nor", 0x2A: "slt", 0x2B: "sltu", 0x2C: "max",
    0x2D: "min", 0x2E: "msub", 0x2F: "msubu",
};
const REGIMM = { 0x00: "bltz", 0x01: "bgez", 0x02: "bltzl", 0x03: "bgezl",
    0x10: "bltzal", 0x11: "bgezal", 0x12: "bltzall", 0x13: "bgezall" };
const PRIMARY = {
    0x02: "j", 0x03: "jal", 0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz",
    0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi", 0x0D: "ori",
    0x0E: "xori", 0x0F: "lui", 0x14: "beql", 0x15: "bnel", 0x16: "blezl", 0x17: "bgtzl",
    0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr",
    0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr", 0x2F: "cache",
    0x30: "ll", 0x31: "lwc1", 0x38: "sc", 0x39: "swc1",
    0x32: "lv.s", 0x35: "lvl/r.q", 0x36: "lv.q", 0x3A: "sv.s", 0x3D: "svl/r.q", 0x3E: "sv.q",
    0x18: "vfpu0", 0x19: "vfpu1", 0x1B: "vfpu3", 0x34: "vfpu4", 0x37: "vfpu5",
    0x3C: "vfpu6", 0x3F: "vfpu7",
};
const FPU_S = {
    0x00: "add.s", 0x01: "sub.s", 0x02: "mul.s", 0x03: "div.s", 0x04: "sqrt.s",
    0x05: "abs.s", 0x06: "mov.s", 0x07: "neg.s", 0x0C: "round.w.s", 0x0D: "trunc.w.s",
    0x0E: "ceil.w.s", 0x0F: "floor.w.s", 0x24: "cvt.w.s",
};
const FPU_COND = ["f", "un", "eq", "ueq", "olt", "ult", "ole", "ule",
    "sf", "ngle", "seq", "ngl", "lt", "nge", "le", "ngt"];
const BSHFL = { 0x10: "seb", 0x18: "seh", 0x02: "wsbh", 0x03: "wsbw", 0x14: "bitrev" };

const sext16 = (x) => (x & 0x8000 ? x - 0x10000 : x);

export function decode(addr, w) {
    const op = w >>> 26;
    const rs = (w >>> 21) & 31, rt = (w >>> 16) & 31, rd = (w >>> 11) & 31, sa = (w >>> 6) & 31, fn = w & 63;
    const imm = w & 0xFFFF;
    const i = new Insn(addr, w, "?", rs, rt, rd, sa, imm);
    const branch = (name) => {
        i.op = name;
        i.imm = sext16(imm);
        i.target = (addr + 4 + i.imm * 4) >>> 0;
        return i;
    };
    if (op === 0) {
        let name = SPECIAL[fn];
        if (name === undefined) return i;
        if (fn === 0x02 && rs === 1) name = "rotr";
        else if (fn === 0x06 && sa === 1) name = "rotrv";
        else if (fn === 0x00 && w === 0) name = "nop";
        else if (fn === 0x0C) i.imm = (w >>> 6) & 0xFFFFF;
        i.op = name;
        return i;
    }
    if (op === 1) return REGIMM[rt] ? branch(REGIMM[rt]) : i;
    if (op === 2 || op === 3) {
        i.op = op === 2 ? "j" : "jal";
        i.target = (((addr + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)) >>> 0;
        return i;
    }
    if ([0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17].includes(op)) return branch(PRIMARY[op]);
    if (op === 0x10) { i.op = "cop0"; return i; }
    if (op === 0x11) {  // FPU
        if (rs === 0x00) i.op = "mfc1";
        else if (rs === 0x02) i.op = "cfc1";
        else if (rs === 0x04) i.op = "mtc1";
        else if (rs === 0x06) i.op = "ctc1";
        else if (rs === 0x08) return branch(["bc1f", "bc1t", "bc1fl", "bc1tl"][rt & 3]);
        else if (rs === 0x10) i.op = fn >= 0x30 ? "c." + FPU_COND[fn & 15] + ".s" : (FPU_S[fn] ?? "?");
        else if (rs === 0x14 && fn === 0x20) i.op = "cvt.s.w";
        return i;  // fmt S: fs = rd, ft = rt, fd = sa
    }
    if (op === 0x12) {  // COP2 = VFPU transfer / branch
        if (rs === 0x03) i.op = imm & 0x80 ? "mfvc" : "mfv";
        else if (rs === 0x07) i.op = imm & 0x80 ? "mtvc" : "mtv";
        else if (rs === 0x08) return branch(["bvf", "bvt", "bvfl", "bvtl"][rt & 3]);
        else i.op = "vfpu-cop2";
        return i;
    }
    if (op === 0x1C) { i.op = { 0x00: "halt", 0x24: "mfic", 0x26: "mtic" }[fn] ?? "?"; return i; }
    if (op === 0x1F) {  // SPECIAL3
        if (fn === 0x00) i.op = "ext";  // pos = sa, size = rd + 1
        else if (fn === 0x04) i.op = "ins";  // pos = sa, size = rd - sa + 1
        else if (fn === 0x20) i.op = BSHFL[sa] ?? "?";
        return i;
    }
    const name = PRIMARY[op];
    if (name === undefined) return i;
    i.op = name;
    if (["addi", "addiu", "slti", "sltiu"].includes(name) || (op >= 0x20 && op <= 0x3E)) i.imm = sext16(imm);
    return i;
}
