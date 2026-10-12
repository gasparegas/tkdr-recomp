// Translate analysed MIPS functions straight into a WebAssembly module.
// Same semantics as recomp/emit.py (which writes C): one wasm function per
// guest function taking the Ctx pointer; registers live in the Ctx struct in
// shared memory; guest memory is the runtime's g_mem buffer (imported global).
//
// Control flow inside a function: when it has labels, the body is a loop
// around nested blocks, one per segment between labels; a jump sets the
// segment number and restarts the loop, whose br_table enters that segment
// (straight-line code falls from one segment into the next).
import { Code, I32, I64, Module, OP } from "./wasm.mjs";

// struct Ctx (runtime/recomp.h)
export const CTX = { r: 0, hi: 128, lo: 132, f: 136, fcr31: 264, fcc: 268, v: 272, vctrl: 784, ic: 848, size: 852 };
const R = (n) => CTX.r + 4 * n, F = (n) => CTX.f + 4 * n;

// locals of a guest function; GPRs $1..$31 are cached in locals REG + n - 1
// (loaded on entry, written back before calls and returns, reloaded after calls)
const C = 0, L = 1, T = 2, A = 3, B = 4, P = 5, REG = 6;
const LOCALS = [[4, I32], [1, I64], [31, I32]];

const RD_WRITERS = new Set(["addu", "add", "subu", "sub", "and", "or", "xor", "nor", "slt", "sltu", "max", "min",
    "movz", "movn", "sll", "srl", "sra", "rotr", "sllv", "srlv", "srav", "rotrv", "clz", "clo", "mfhi", "mflo",
    "seb", "seh", "wsbh", "wsbw", "bitrev"]);
const RT_WRITERS = new Set(["addiu", "addi", "slti", "sltiu", "andi", "ori", "xori", "lui", "lb", "lbu", "lh", "lhu",
    "lw", "ll", "lwl", "lwr", "sc", "mfic", "ext", "ins", "mfc1", "cfc1"]);

// GPR written by generated code for this instruction (0: none)
function written(i) {
    if (RD_WRITERS.has(i.op)) return i.rd;
    if (RT_WRITERS.has(i.op)) return i.rt;
    if (["jal", "bltzal", "bgezal", "bltzall", "bgezall"].includes(i.op)) return 31;
    if (i.op === "jalr") return i.rd || 31;
    return 0;
}

// FPU arithmetic and VFPU instructions do not read GPRs in generated code
// (the VFPU interpreter reads the Ctx, which is written back before it runs)
const noGprRead = (i) => /\.s$|^cvt|^(vfpu|lv|sv|mfv|mtv)/.test(i.op) && i.op !== "lwc1" && i.op !== "swc1";
const GMEM = 0;  // global: host address of guest address 0

// trap codes passed to the runtime (recomp_trap_code)
export const TRAP = { syscall: 1, break: 2, halt: 3, cop0: 4, undecoded: 5, unsupported: 6, delay_slot_branch: 7 };

const BRANCH_BASE = {
    beql: "beq", bnel: "bne", blezl: "blez", bgtzl: "bgtz", bltzl: "bltz", bgezl: "bgez",
    bltzal: "bltz", bgezal: "bgez", bltzall: "bltz", bgezall: "bgez",
    bc1fl: "bc1f", bc1tl: "bc1t", bvfl: "bvf", bvtl: "bvt",
};

export function generate(prog, { onProgress } = {}) {
    const m = new Module();
    const tGuest = m.type([I32]);
    const tCI = m.type([I32, I32]);
    const tCII = m.type([I32, I32, I32]);
    const tIIrI = m.type([I32, I32], [I32]);
    const tIrI = m.type([I32], [I32]);
    const tFIrI = m.type([0x7D, I32], [I32]);

    // imports: runtime helpers, memory, g_mem
    const fHle = m.importFunc("env", "hle_call", tCI);  // (ctx, import index)
    const fVfpu = m.importFunc("env", "vfpu_exec", tCII);  // (ctx, pc, insn)
    const fTrap = m.importFunc("env", "recomp_trap_code", tCII);  // (ctx, pc, code)
    const fMiss = m.importFunc("env", "recomp_call_miss", tCI);  // (ctx, addr): stubs and unknown targets
    m.importMemory("env", "memory", 1, 65536, true);
    m.importGlobal("env", "gmem", I32);

    // module-internal helpers
    const fDispatch = m.declare(tCI);
    const H = {
        div: m.declare(tCII), divu: m.declare(tCII),
        lwl: m.declare(tIIrI), lwr: m.declare(tIIrI), swl: m.declare(tCI), swr: m.declare(tCI),
        bswap: m.declare(tIrI), bitrev: m.declare(tIrI), f2i: m.declare(tFIrI),
    };
    defineHelpers(m, H);

    // one function per guest function
    const funcs = prog.functions;
    const index = new Map();
    for (const [a] of funcs) index.set(a, m.declare(tGuest));
    const stubIndex = new Map([...prog.stubs.keys()].sort((x, y) => x - y).map((a, k) => [a, k]));
    const ctx = { prog, index, stubIndex, fHle, fVfpu, fTrap, fDispatch, H };
    funcs.forEach(([start, size], k) => {
        m.define(index.get(start), LOCALS, emitFunction(ctx, start, size));
        if (onProgress && k % 500 === 0) onProgress(k, funcs.length);
    });

    // dispatch(ctx, addr): binary search over the function starts
    m.define(fDispatch, [], emitDispatch(funcs.map(([a]) => a), index, fMiss));
    m.exportFunc("dispatch", fDispatch);
    return m.encode();
}

// ---------------------------------------------------------------- helpers

function hostAddr(c) {  // [guest address] -> [host address]
    c.i32(0x1FFFFFFF).op(OP.i32_and).gget(GMEM).op(OP.i32_add);
}

function defineHelpers(m, H) {
    let c;
    // div(ctx, a, b) with MIPS results for b == 0 and INT_MIN / -1
    c = new Code();
    c.get(2).op(OP.i32_eqz);
    c.open(OP.if);
    c.get(0).i32(1).i32(-1).get(1).i32(0).op(OP.i32_lt_s).op(OP.select).mem(OP.i32_store, CTX.lo, 2);
    c.get(0).get(1).mem(OP.i32_store, CTX.hi, 2);
    c.op(OP.return);
    c.end();
    c.get(1).i32(-0x80000000).op(OP.i32_eq).get(2).i32(-1).op(OP.i32_eq).op(OP.i32_and);
    c.open(OP.if);
    c.get(0).get(1).mem(OP.i32_store, CTX.lo, 2);
    c.get(0).i32(0).mem(OP.i32_store, CTX.hi, 2);
    c.op(OP.return);
    c.end();
    c.get(0).get(1).get(2).op(OP.i32_div_s).mem(OP.i32_store, CTX.lo, 2);
    c.get(0).get(1).get(2).op(OP.i32_rem_s).mem(OP.i32_store, CTX.hi, 2);
    m.define(H.div, [], c.result());

    // divu(ctx, a, b)
    c = new Code();
    c.get(2).op(OP.i32_eqz);
    c.open(OP.if);
    c.get(0).i32(0xFFFF).i32(-1).get(1).i32(0xFFFF).op(OP.i32_le_u).op(OP.select).mem(OP.i32_store, CTX.lo, 2);
    c.get(0).get(1).mem(OP.i32_store, CTX.hi, 2);
    c.op(OP.return);
    c.end();
    c.get(0).get(1).get(2).op(OP.i32_div_u).mem(OP.i32_store, CTX.lo, 2);
    c.get(0).get(1).get(2).op(OP.i32_rem_u).mem(OP.i32_store, CTX.hi, 2);
    m.define(H.divu, [], c.result());

    // lwl(rt, a) / lwr(rt, a): locals 2 = sh, 3 = aligned word
    const unalignedLoad = (left) => {
        const k = new Code();
        k.get(1).i32(3).op(OP.i32_and).i32(3).op(OP.i32_shl).set(2);  // sh = (a & 3) * 8
        k.get(1).i32(~3).op(OP.i32_and);
        hostAddr(k);
        k.mem(OP.i32_load, 0, 2).set(3);
        if (left) {  // (rt & (0x00FFFFFF >> sh)) | (w << (24 - sh))
            k.get(0).i32(0x00FFFFFF).get(2).op(OP.i32_shr_u).op(OP.i32_and);
            k.get(3).i32(24).get(2).op(OP.i32_sub).op(OP.i32_shl).op(OP.i32_or);
        } else {  // (rt & ~(0xFFFFFFFF >> sh)) | (w >> sh)
            k.get(0).i32(-1).get(2).op(OP.i32_shr_u).i32(-1).op(OP.i32_xor).op(OP.i32_and);
            k.get(3).get(2).op(OP.i32_shr_u).op(OP.i32_or);
        }
        return k.result();
    };
    m.define(H.lwl, [[2, I32]], unalignedLoad(true));
    m.define(H.lwr, [[2, I32]], unalignedLoad(false));

    // swl(rt, a) / swr(rt, a): locals 2 = sh, 3 = host address of the aligned word
    const unalignedStore = (left) => {
        const k = new Code();
        k.get(1).i32(3).op(OP.i32_and).i32(3).op(OP.i32_shl).set(2);
        k.get(1).i32(~3).op(OP.i32_and);
        hostAddr(k);
        k.set(3);
        k.get(3);
        if (left) {  // (w & ~(0xFFFFFFFF >> (24 - sh))) | (rt >> (24 - sh))
            k.get(3).mem(OP.i32_load, 0, 2).i32(-1).i32(24).get(2).op(OP.i32_sub).op(OP.i32_shr_u)
                .i32(-1).op(OP.i32_xor).op(OP.i32_and);
            k.get(0).i32(24).get(2).op(OP.i32_sub).op(OP.i32_shr_u).op(OP.i32_or);
        } else {  // (w & ~(0xFFFFFFFF << sh)) | (rt << sh)
            k.get(3).mem(OP.i32_load, 0, 2).i32(-1).get(2).op(OP.i32_shl).i32(-1).op(OP.i32_xor).op(OP.i32_and);
            k.get(0).get(2).op(OP.i32_shl).op(OP.i32_or);
        }
        k.mem(OP.i32_store, 0, 2);
        return k.result();
    };
    m.define(H.swl, [[2, I32]], unalignedStore(true));
    m.define(H.swr, [[2, I32]], unalignedStore(false));

    // bswap(x)
    c = new Code();
    c.get(0).i32(24).op(OP.i32_shl);
    c.get(0).i32(0xFF00).op(OP.i32_and).i32(8).op(OP.i32_shl).op(OP.i32_or);
    c.get(0).i32(8).op(OP.i32_shr_u).i32(0xFF00).op(OP.i32_and).op(OP.i32_or);
    c.get(0).i32(24).op(OP.i32_shr_u).op(OP.i32_or);
    m.define(H.bswap, [], c.result());

    // bitrev(x): swap bits within bytes, then the bytes
    c = new Code();
    for (const [sh, mask] of [[1, 0x55555555], [2, 0x33333333], [4, 0x0F0F0F0F]]) {
        c.get(0).i32(sh).op(OP.i32_shr_u).i32(mask).op(OP.i32_and);
        c.get(0).i32(mask).op(OP.i32_and).i32(sh).op(OP.i32_shl).op(OP.i32_or).set(0);
    }
    c.get(0).call(H.bswap);
    m.define(H.bitrev, [], c.result());

    // f2i(f, mode): MIPS float -> int32 (saturating, NaN -> 0x7FFFFFFF);
    // mode 0 nearest even, 1 truncate, 2 ceil, 3 floor
    c = new Code();
    c.get(0).get(0).op(OP.f32_ne);
    c.open(OP.if);
    c.i32(0x7FFFFFFF).op(OP.return);
    c.end();
    c.get(1).op(OP.i32_eqz);
    c.open(OP.if, 0x7D);
    c.get(0).op(OP.f32_nearest);
    c.else_();
    c.get(1).i32(1).op(OP.i32_eq);
    c.open(OP.if, 0x7D);
    c.get(0).op(OP.f32_trunc);
    c.else_();
    c.get(1).i32(2).op(OP.i32_eq);
    c.open(OP.if, 0x7D);
    c.get(0).op(OP.f32_ceil);
    c.else_();
    c.get(0).op(OP.f32_floor);
    c.end();
    c.end();
    c.end();
    c.satTruncF32S();
    m.define(H.f2i, [], c.result());
}

function emitDispatch(addrs, index, fMiss) {
    const c = new Code();
    const tree = (lo, hi) => {
        if (hi - lo <= 4) {
            for (let k = lo; k < hi; k++) {
                c.get(1).i32(addrs[k]).op(OP.i32_eq);
                c.open(OP.if);
                c.get(0).call(index.get(addrs[k])).op(OP.return);
                c.end();
            }
            return;
        }
        const mid = (lo + hi) >> 1;
        c.get(1).i32(addrs[mid]).op(OP.i32_lt_u);
        c.open(OP.if);
        tree(lo, mid);
        c.else_();
        tree(mid, hi);
        c.end();
    };
    tree(0, addrs.length);
    c.get(0).get(1).call(fMiss);
    return c.result();
}

// ---------------------------------------------------------------- functions

function emitFunction(g, start, size) {
    const { prog } = g;
    const end = start + size;
    const inside = (a) => a >= start && a < end;
    const insns = [];
    for (let a = start; a < end; a += 4) insns.push(prog.insns.get(a));

    // labels: branch targets, jump table cases, computed-goto targets
    const labels = new Set();
    for (const i of insns) {
        if ((i.isBranch || i.op === "j") && inside(i.target)) labels.add(i.target);
        if (i.op === "jr" && prog.jumpTables.has(i.addr))
            for (const t of prog.jumpTables.get(i.addr)) if (inside(t)) labels.add(t);
    }
    for (const a of prog.internalLabels) if (inside(a)) labels.add(a);
    // a jump straight into a delay slot: that slot is emitted again on its own,
    // then continues at the next instruction, which needs a label too
    for (let k = 0; k + 1 < insns.length; k++)
        if (insns[k].hasDelaySlot && labels.has(insns[k + 1].addr)) labels.add(insns[k + 1].addr + 4);

    const segStarts = [...new Set([start, ...[...labels].filter(inside)])].sort((x, y) => x - y);
    const segIndex = new Map(segStarts.map((a, k) => [a, k]));
    const c = new Code();
    const looped = labels.size > 0;
    let top = 0;

    // registers this function touches: cached in locals
    const W = new Set(), U = new Set();
    for (const i of insns) {
        const w = written(i);
        if (w) W.add(w);
        if (!noGprRead(i)) for (const r of [i.rs, i.rt, i.rd]) if (r) U.add(r);
    }
    for (const r of W) U.add(r);
    const e = new Emitter(g, c, start, end, inside, segIndex, () => top, [...W], [...U]);
    e.reload();
    if (looped) {
        top = c.open(OP.loop);
        const levels = [];
        for (let k = segStarts.length - 1; k >= 0; k--) levels[k] = c.open(OP.block);
        c.get(L);
        c.brTable(levels, levels[0]);
        c.end();  // segment 0 follows the innermost block
    }
    let seg = 0;
    const beginSegment = (addr) => {
        // close the block whose end leads into this segment
        if (!looped) return;
        while (seg < segIndex.get(addr)) {
            c.end();
            seg++;
        }
    };
    if (looped) beginSegment(start);

    for (let k = 0; k < insns.length;) {
        const i = insns[k];
        if (looped && segIndex.has(i.addr) && segIndex.get(i.addr) > seg) beginSegment(i.addr);
        if (!i.hasDelaySlot) {
            e.body(i);
            k++;
            continue;
        }
        const ds = k + 1 < insns.length ? insns[k + 1] : null;
        e.transfer(i, ds);
        k += 2;
        if (ds && labels.has(ds.addr)) {
            e.goto(ds.addr + 4);
            beginSegment(ds.addr);
            e.dsBody(ds);
        }
    }
    // fell off the end: continue into the next function
    if (end < prog.textEnd) e.tail(end);
    else e.ret();
    if (looped) {
        while (c.depth > 0) c.end();
    }
    return c.result();
}

class Emitter {
    constructor(g, c, start, end, inside, segIndex, top, W, U) {
        Object.assign(this, { g, c, start, end, inside, segIndex, top, W, U });
    }

    rd(n) {
        if (n === 0) this.c.i32(0);
        else this.c.get(REG + n - 1);
    }
    // r[n] = value (value() pushes one i32)
    set(n, value) {
        if (n === 0) return;
        value();
        this.c.set(REG + n - 1);
    }
    // cached registers <-> Ctx, around anything that reads or writes the Ctx
    flush() { for (const n of this.W) this.c.get(C).get(REG + n - 1).mem(OP.i32_store, R(n), 2); }
    reload() { for (const n of this.U) this.c.get(C).mem(OP.i32_load, R(n), 2).set(REG + n - 1); }
    // call something that uses the Ctx: push its arguments in args()
    callOut(fn, args, thenReturn = false) {
        this.flush();
        args();
        this.c.call(fn);
        if (thenReturn) this.c.op(OP.return);
        else this.reload();
    }
    ret() {
        this.flush();
        this.c.op(OP.return);
    }
    ctxLoad(off) { this.c.get(C).mem(OP.i32_load, off, 2); }
    ctxStore(off, value) {
        this.c.get(C);
        value();
        this.c.mem(OP.i32_store, off, 2);
    }
    fload(n) { this.c.get(C).mem(OP.f32_load, F(n), 2); }
    fstore(n, value) {
        this.c.get(C);
        value();
        this.c.mem(OP.f32_store, F(n), 2);
    }
    ea(i) {  // host address of rs + imm
        this.rd(i.rs);
        if (i.imm) this.c.i32(i.imm).op(OP.i32_add);
        hostAddr(this.c);
    }
    bin(i, opc, a, b) { this.set(i.rd, () => { this.rd(a); this.rd(b); this.c.op(opc); }); }

    trap(i, code) { this.callOut(this.g.fTrap, () => this.c.get(C).i32(i.addr).i32(code)); }

    goto(target) {  // jump to a label of this function
        if (!this.segIndex.has(target)) return this.tail(target);
        this.c.i32(this.segIndex.get(target)).set(L).br(this.top());
    }
    callTarget(target, thenReturn = false) {
        const { index, stubIndex, fHle, fDispatch } = this.g, c = this.c;
        if (stubIndex.has(target)) this.callOut(fHle, () => c.get(C).i32(stubIndex.get(target)), thenReturn);
        else if (index.has(target)) this.callOut(index.get(target), () => c.get(C), thenReturn);
        else this.callOut(fDispatch, () => c.get(C).i32(target), thenReturn);
    }
    tail(target) {
        if (this.inside(target)) return this.goto(target);
        this.callTarget(target, true);
    }

    dsBody(ds) {
        if (ds.hasDelaySlot) this.trap(ds, TRAP.delay_slot_branch);
        else this.body(ds);
    }

    cond(i) {  // push the branch condition (nonzero = taken)
        const c = this.c, base = BRANCH_BASE[i.op] ?? i.op;
        switch (base) {
        case "beq": this.rd(i.rs); this.rd(i.rt); c.op(OP.i32_eq); break;
        case "bne": this.rd(i.rs); this.rd(i.rt); c.op(OP.i32_ne); break;
        case "blez": this.rd(i.rs); c.i32(0).op(OP.i32_le_s); break;
        case "bgtz": this.rd(i.rs); c.i32(0).op(OP.i32_gt_s); break;
        case "bltz": this.rd(i.rs); c.i32(0).op(OP.i32_lt_s); break;
        case "bgez": this.rd(i.rs); c.i32(0).op(OP.i32_ge_s); break;
        case "bc1f": this.ctxLoad(CTX.fcc); c.op(OP.i32_eqz); break;
        case "bc1t": this.ctxLoad(CTX.fcc); break;
        case "bvf": this.ctxLoad(CTX.vctrl + 12); c.i32((i.rt >> 2) & 7).op(OP.i32_shr_u).i32(1).op(OP.i32_and).op(OP.i32_eqz); break;
        case "bvt": this.ctxLoad(CTX.vctrl + 12); c.i32((i.rt >> 2) & 7).op(OP.i32_shr_u).i32(1).op(OP.i32_and); break;
        default: throw new Error("no condition for " + i.op);
        }
    }

    transfer(i, ds) {
        const c = this.c;
        const doDs = () => { if (ds) this.dsBody(ds); };
        switch (i.op) {
        case "j": doDs(); this.tail(i.target); return;
        case "jal": this.set(31, () => c.i32(i.addr + 8)); doDs(); this.callTarget(i.target); return;
        case "jr": {
            if (i.rs === 31) {
                doDs();
                this.ret();
                return;
            }
            this.rd(i.rs);
            c.set(T);
            doDs();
            const table = this.g.prog.jumpTables.get(i.addr);
            const cases = table
                ? [...new Set(table)].filter((a) => this.inside(a)).sort((x, y) => x - y)
                : [...this.g.prog.internalLabels].filter((a) => this.inside(a)).sort((x, y) => x - y);
            for (const a of cases) {
                c.get(T).i32(a).op(OP.i32_eq);
                c.open(OP.if);
                this.goto(a);
                c.end();
            }
            this.callOut(this.g.fDispatch, () => c.get(C).get(T), true);
            return;
        }
        case "jalr":
            this.rd(i.rs);
            c.set(T);
            this.set(i.rd || 31, () => c.i32(i.addr + 8));
            doDs();
            this.callOut(this.g.fDispatch, () => c.get(C).get(T));
            return;
        }
        // conditional branches
        const link = ["bltzal", "bgezal", "bltzall", "bgezall"].includes(i.op);
        const go = () => (link ? this.callTarget(i.target) : this.tail(i.target));
        const pre = () => { if (link) this.set(31, () => c.i32(i.addr + 8)); };
        if (i.isLikely) {  // the delay slot runs only when taken
            this.cond(i);
            c.open(OP.if);
            pre();
            doDs();
            go();
            c.end();
            return;
        }
        this.cond(i);
        c.set(T);
        pre();
        doDs();
        c.get(T);
        c.open(OP.if);
        go();
        c.end();
    }

    body(i) {
        const c = this.c, { rs, rt, rd, sa } = i, u16 = i.raw & 0xFFFF;
        const set = (n, v) => this.set(n, v);
        const rdv = (n) => () => this.rd(n);
        switch (i.op) {
        case "addu": case "add": return this.bin(i, OP.i32_add, rs, rt);
        case "subu": case "sub": return this.bin(i, OP.i32_sub, rs, rt);
        case "and": return this.bin(i, OP.i32_and, rs, rt);
        case "or": return this.bin(i, OP.i32_or, rs, rt);
        case "xor": return this.bin(i, OP.i32_xor, rs, rt);
        case "nor": return set(rd, () => { this.rd(rs); this.rd(rt); c.op(OP.i32_or).i32(-1).op(OP.i32_xor); });
        case "slt": return this.bin(i, OP.i32_lt_s, rs, rt);
        case "sltu": return this.bin(i, OP.i32_lt_u, rs, rt);
        case "max": case "min": return set(rd, () => {
            this.rd(rs); c.tee(A); this.rd(rt); c.tee(B);
            c.get(A).get(B).op(i.op === "max" ? OP.i32_gt_s : OP.i32_lt_s).op(OP.select);
        });
        case "movz": case "movn":
            this.rd(rt);
            if (i.op === "movz") c.op(OP.i32_eqz);
            c.open(OP.if);
            set(rd, rdv(rs));
            c.end();
            return;
        case "sll": return set(rd, () => { this.rd(rt); c.i32(sa).op(OP.i32_shl); });
        case "srl": return set(rd, () => { this.rd(rt); c.i32(sa).op(OP.i32_shr_u); });
        case "sra": return set(rd, () => { this.rd(rt); c.i32(sa).op(OP.i32_shr_s); });
        case "rotr": return set(rd, () => { this.rd(rt); c.i32(sa).op(OP.i32_rotr); });
        case "sllv": return this.bin(i, OP.i32_shl, rt, rs);
        case "srlv": return this.bin(i, OP.i32_shr_u, rt, rs);
        case "srav": return this.bin(i, OP.i32_shr_s, rt, rs);
        case "rotrv": return this.bin(i, OP.i32_rotr, rt, rs);
        case "clz": return set(rd, () => { this.rd(rs); c.op(OP.i32_clz); });
        case "clo": return set(rd, () => { this.rd(rs); c.i32(-1).op(OP.i32_xor).op(OP.i32_clz); });
        case "mfhi": return set(rd, () => this.ctxLoad(CTX.hi));
        case "mflo": return set(rd, () => this.ctxLoad(CTX.lo));
        case "mthi": return this.ctxStore(CTX.hi, rdv(rs));
        case "mtlo": return this.ctxStore(CTX.lo, rdv(rs));
        case "mult": case "multu": case "madd": case "maddu": case "msub": case "msubu": {
            const signed = !i.op.endsWith("u");
            const ext = signed ? OP.i64_extend_i32_s : OP.i64_extend_i32_u;
            if (i.op.startsWith("madd") || i.op.startsWith("msub")) {  // hi:lo as a 64-bit accumulator
                this.ctxLoad(CTX.hi); c.op(OP.i64_extend_i32_u).i64(32).op(OP.i64_shl);
                this.ctxLoad(CTX.lo); c.op(OP.i64_extend_i32_u).op(OP.i64_or);
            }
            this.rd(rs); c.op(ext); this.rd(rt); c.op(ext).op(OP.i64_mul);
            if (i.op.startsWith("madd")) c.op(OP.i64_add);
            else if (i.op.startsWith("msub")) c.op(OP.i64_sub);
            c.set(P);
            this.ctxStore(CTX.lo, () => c.get(P).op(OP.i32_wrap_i64));
            this.ctxStore(CTX.hi, () => c.get(P).i64(32).op(OP.i64_shr_u).op(OP.i32_wrap_i64));
            return;
        }
        case "div": case "divu":
            c.get(C); this.rd(rs); this.rd(rt);
            c.call(i.op === "div" ? this.g.H.div : this.g.H.divu);
            return;
        case "addiu": case "addi": return set(rt, () => { this.rd(rs); if (i.imm) c.i32(i.imm).op(OP.i32_add); });
        case "slti": return set(rt, () => { this.rd(rs); c.i32(i.imm).op(OP.i32_lt_s); });
        case "sltiu": return set(rt, () => { this.rd(rs); c.i32(i.imm).op(OP.i32_lt_u); });
        case "andi": return set(rt, () => { this.rd(rs); c.i32(u16).op(OP.i32_and); });
        case "ori": return set(rt, () => { this.rd(rs); if (u16) c.i32(u16).op(OP.i32_or); });
        case "xori": return set(rt, () => { this.rd(rs); c.i32(u16).op(OP.i32_xor); });
        case "lui": return set(rt, () => c.i32(u16 << 16));
        case "lb": return set(rt, () => { this.ea(i); c.mem(OP.i32_load8_s, 0, 0); });
        case "lbu": return set(rt, () => { this.ea(i); c.mem(OP.i32_load8_u, 0, 0); });
        case "lh": return set(rt, () => { this.ea(i); c.mem(OP.i32_load16_s, 0, 1); });
        case "lhu": return set(rt, () => { this.ea(i); c.mem(OP.i32_load16_u, 0, 1); });
        case "lw": case "ll": return set(rt, () => { this.ea(i); c.mem(OP.i32_load, 0, 2); });
        case "lwl": case "lwr": return set(rt, () => {
            this.rd(rt);
            this.rd(rs); if (i.imm) c.i32(i.imm).op(OP.i32_add);
            c.call(i.op === "lwl" ? this.g.H.lwl : this.g.H.lwr);
        });
        case "sb": this.ea(i); this.rd(rt); c.mem(OP.i32_store8, 0, 0); return;
        case "sh": this.ea(i); this.rd(rt); c.mem(OP.i32_store16, 0, 1); return;
        case "sw": this.ea(i); this.rd(rt); c.mem(OP.i32_store, 0, 2); return;
        case "sc": this.ea(i); this.rd(rt); c.mem(OP.i32_store, 0, 2); set(rt, () => c.i32(1)); return;
        case "swl": case "swr":
            this.rd(rt);
            this.rd(rs); if (i.imm) c.i32(i.imm).op(OP.i32_add);
            c.call(i.op === "swl" ? this.g.H.swl : this.g.H.swr);
            return;
        case "nop": case "sync": case "cache": return;
        case "mfic": return set(rt, () => this.ctxLoad(CTX.ic));
        case "mtic": return this.ctxStore(CTX.ic, rdv(rt));
        case "ext": return set(rt, () => {
            this.rd(rs);
            if (sa) c.i32(sa).op(OP.i32_shr_u);
            const size = rd + 1;
            if (size < 32) c.i32(size === 31 ? 0x7FFFFFFF : (1 << size) - 1).op(OP.i32_and);
        });
        case "ins": {
            const pos = sa, size = rd - sa + 1;
            const mask = size >= 32 ? -1 : (((1 << size) - 1) << pos) | 0;
            return set(rt, () => {
                this.rd(rt); c.i32(~mask).op(OP.i32_and);
                this.rd(rs); c.i32(pos).op(OP.i32_shl).i32(mask).op(OP.i32_and).op(OP.i32_or);
            });
        }
        case "seb": return set(rd, () => { this.rd(rt); c.op(OP.i32_extend8_s); });
        case "seh": return set(rd, () => { this.rd(rt); c.op(OP.i32_extend16_s); });
        case "wsbh": return set(rd, () => {
            this.rd(rt); c.tee(A).i32(0x00FF00FF).op(OP.i32_and).i32(8).op(OP.i32_shl);
            c.get(A).i32(8).op(OP.i32_shr_u).i32(0x00FF00FF).op(OP.i32_and).op(OP.i32_or);
        });
        case "wsbw": return set(rd, () => { this.rd(rt); c.call(this.g.H.bswap); });
        case "bitrev": return set(rd, () => { this.rd(rt); c.call(this.g.H.bitrev); });
        // FPU (fmt S: fs = rd, ft = rt, fd = sa); raw bits moved as i32
        case "lwc1": c.get(C); this.ea(i); c.mem(OP.i32_load, 0, 2).mem(OP.i32_store, F(rt), 2); return;
        case "swc1": this.ea(i); c.get(C).mem(OP.i32_load, F(rt), 2).mem(OP.i32_store, 0, 2); return;
        case "mfc1": return set(rt, () => this.ctxLoad(F(rd)));
        case "mtc1": return this.ctxStore(F(rd), rdv(rt));
        case "cfc1": return set(rt, () => {
            if (rd !== 31) { c.i32(0x3351); return; }
            this.ctxLoad(CTX.fcr31); this.ctxLoad(CTX.fcc); c.i32(23).op(OP.i32_shl).op(OP.i32_or);
        });
        case "ctc1":
            if (rd !== 31) return;
            this.ctxStore(CTX.fcr31, () => { this.rd(rt); c.i32(~(1 << 23)).op(OP.i32_and); });
            this.ctxStore(CTX.fcc, () => { this.rd(rt); c.i32(23).op(OP.i32_shr_u).i32(1).op(OP.i32_and); });
            return;
        case "add.s": case "sub.s": case "mul.s": case "div.s": {
            const o = { "add.s": OP.f32_add, "sub.s": OP.f32_sub, "mul.s": OP.f32_mul, "div.s": OP.f32_div }[i.op];
            return this.fstore(sa, () => { this.fload(rd); this.fload(rt); c.op(o); });
        }
        case "sqrt.s": return this.fstore(sa, () => { this.fload(rd); c.op(OP.f32_sqrt); });
        case "abs.s": return this.fstore(sa, () => { this.fload(rd); c.op(OP.f32_abs); });
        case "mov.s": return this.ctxStore(F(sa), () => this.ctxLoad(F(rd)));
        case "neg.s": return this.ctxStore(F(sa), () => { this.ctxLoad(F(rd)); c.i32(-0x80000000).op(OP.i32_xor); });
        case "round.w.s": case "trunc.w.s": case "ceil.w.s": case "floor.w.s": case "cvt.w.s":
            return this.ctxStore(F(sa), () => {
                this.fload(rd);
                const mode = { "round.w.s": 0, "trunc.w.s": 1, "ceil.w.s": 2, "floor.w.s": 3 }[i.op];
                if (mode !== undefined) c.i32(mode);
                else { this.ctxLoad(CTX.fcr31); c.i32(3).op(OP.i32_and); }
                c.call(this.g.H.f2i);
            });
        case "cvt.s.w": return this.fstore(sa, () => { this.ctxLoad(F(rd)); c.op(OP.f32_convert_i32_s); });
        }
        if (i.op.startsWith("c.") && i.op.endsWith(".s")) {  // fcc = unordered/equal/less per cond bits
            const cond = i.raw & 7;
            return this.ctxStore(CTX.fcc, () => {
                let terms = 0;
                const or = () => { if (terms++) c.op(OP.i32_or); };
                if (cond & 1) {
                    this.fload(rd); this.fload(rd); c.op(OP.f32_ne);
                    this.fload(rt); this.fload(rt); c.op(OP.f32_ne).op(OP.i32_or);
                    or();
                }
                if (cond & 2) { this.fload(rd); this.fload(rt); c.op(OP.f32_eq); or(); }
                if (cond & 4) { this.fload(rd); this.fload(rt); c.op(OP.f32_lt); or(); }
                if (!terms) c.i32(0);
            });
        }
        if (/^(vfpu|lv|sv|mfv|mtv)/.test(i.op)) {
            this.callOut(this.g.fVfpu, () => c.get(C).i32(i.addr).i32(i.raw));
            return;
        }
        if (i.op in TRAP) return this.trap(i, TRAP[i.op]);
        if (i.op === "?") return this.trap(i, TRAP.undecoded);
        return this.trap(i, TRAP.unsupported);
    }
}
