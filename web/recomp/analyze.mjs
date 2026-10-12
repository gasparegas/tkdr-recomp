// Whole-program analysis: decode the code, find functions and jump tables.
// Port of recomp/analyze.py.
import * as elf from "./elf.mjs";
import { decode } from "./mips.mjs";

export function ehFrameFunctions(img) {
    const sec = img.sections[".eh_frame"];
    const v = img.view;
    let o = sec.addr - img.base;
    const end = o + sec.size, out = [];
    while (o < end) {
        const length = v.getUint32(o, true);
        if (length === 0) break;
        if (v.getUint32(o + 4, true) !== 0) out.push([v.getUint32(o + 8, true), v.getUint32(o + 12, true)]);
        o += 4 + length;
    }
    return out;
}

// stub address -> [library, nid]
export function importStubs(img) {
    const sec = img.sections[".lib.stub"];
    const stubs = new Map(), v = img.view;
    for (let o = sec.addr; o < sec.addr + sec.size;) {
        const b = o - img.base;
        const nameVa = v.getUint32(b, true), entsz = v.getUint8(b + 8), nfuncs = v.getUint16(b + 10, true);
        const nidsVa = v.getUint32(b + 12, true), stubsVa = v.getUint32(b + 16, true);
        const lib = img.cstr(nameVa);
        for (let k = 0; k < nfuncs; k++) stubs.set((stubsVa + 8 * k) >>> 0, [lib, img.u32(nidsVa + 4 * k)]);
        o += (entsz || 5) * 4;
    }
    return stubs;
}

// index of the last element <= x in a sorted array (-1 if none)
function bisectRight(arr, x) {
    let lo = 0, hi = arr.length;
    while (lo < hi) {
        const mid = (lo + hi) >> 1;
        if (arr[mid] <= x) lo = mid + 1;
        else hi = mid;
    }
    return lo - 1;
}

export class Program {
    constructor(elfBytes) {
        const img = (this.img = elf.load(elfBytes));
        // code = .init + .text + .fini (contiguous); import stubs are handled apart
        const fini = img.sections[".fini"];
        this.textStart = img.sections[".init"].addr;
        this.textEnd = fini.addr + fini.size;
        this.insns = new Map();
        for (let a = this.textStart; a < this.textEnd; a += 4) this.insns.set(a, decode(a, img.u32(a)));
        this.stubs = importStubs(img);
        this.jumpTables = this.findJumpTables();
        this.findFunctions();
    }

    inText(a) { return a >= this.textStart && a < this.textEnd; }

    // jr address -> case targets.
    // GCC emits: sltiu at,idx,N / beqz at,default / sll / lui+addu / lw r,%lo(T)(at) / jr r
    findJumpTables() {
        const img = this.img, tables = new Map();
        for (const [a, i] of this.insns) {
            if (i.op !== "jr" || i.rs === 31) continue;
            let table = null, count = null, lwOff = null;
            for (let back = 1; back < 24; back++) {
                const j = this.insns.get(a - 4 * back);
                if (!j || ((j.op === "jr" || j.op === "j") && back > 1)) break;
                const rel = img.relocs.get(j.addr);
                const lo = rel && rel[0] === elf.R_MIPS_LO16 ? rel[1] : null;
                if (lwOff === null && j.op === "lw" && j.rt === i.rs) {
                    if (lo !== null) table = lo;  // lw r, %lo(T)(at)
                    lwOff = lo !== null ? 0 : j.imm;
                } else if (table === null && lwOff !== null && lo !== null && j.op === "addiu") {
                    table = (lo + lwOff) >>> 0;  // addiu at, at, %lo(T); lw r, 0(at)
                } else if (table !== null && j.op === "sltiu") {
                    count = j.imm;
                    break;
                }
            }
            if (table === null || count === null) continue;
            const targets = [];
            for (let k = 0; k < count; k++) targets.push(img.u32(table + 4 * k));
            tables.set(a, targets);
        }
        return tables;
    }

    findFunctions() {
        const img = this.img;
        const caseLabels = new Set();
        for (const t of this.jumpTables.values()) for (const a of t) caseLabels.add(a);
        const starts = new Map();
        const eh = ehFrameFunctions(img);
        for (const [pc] of eh) if (this.inText(pc)) starts.set(pc, "eh_frame");
        const setDefault = (a, why) => { if (!starts.has(a)) starts.set(a, why); };
        for (const i of this.insns.values()) if (i.op === "jal" && this.inText(i.target)) setDefault(i.target, "jal");
        // pointers to code stored in data (vtables, callbacks, ctors)
        for (const [loc, [typ, target]] of img.relocs) {
            if (caseLabels.has(target)) continue;
            if (typ === elf.R_MIPS_32 && this.inText(target) && !this.inText(loc)) setDefault(target, "dataptr");
            // la/addiu pairs building a code address (callbacks passed as args)
            if (typ === elf.R_MIPS_LO16 && this.inText(target) && this.inText(loc)) setDefault(target, "codeptr");
        }
        for (const s of [".init", ".fini"]) setDefault(img.sections[s].addr, "section");
        setDefault(img.entry, "entry");

        // .eh_frame boundaries are authoritative: anything else landing strictly
        // inside one is a label (unrecognised jump table, computed goto), not a function
        const ehs = eh.filter(([a]) => this.inText(a)).map(([a, sz]) => [a, a + sz]).sort((x, y) => x[0] - y[0]);
        const ehStarts = ehs.map((e) => e[0]);
        this.internalLabels = new Set(caseLabels);
        for (const [a, src] of [...starts]) {
            if (src === "eh_frame") continue;
            const k = bisectRight(ehStarts, a);
            if (k >= 0 && ehs[k][0] < a && a < ehs[k][1]) {
                starts.delete(a);
                this.internalLabels.add(a);
            }
        }
        // Cross-jumping: GCC lets functions jump (j / branches) into code of
        // another function, typically a shared epilogue. Every such target
        // becomes an entry point of its own ("fragment"); repeat until stable.
        for (;;) {
            const ordered = [...starts.keys()].sort((x, y) => x - y);
            let added = 0;
            for (const i of this.insns.values()) {
                if (!(i.op === "j" || i.isBranch) || !this.inText(i.target)) continue;
                const k = bisectRight(ordered, i.addr);
                const lo = k >= 0 ? ordered[k] : this.textStart;
                const hi = k + 1 < ordered.length ? ordered[k + 1] : this.textEnd;
                if (!(lo <= i.target && i.target < hi) && !starts.has(i.target)) {
                    starts.set(i.target, "fragment");
                    added++;
                }
            }
            if (!added) break;
        }
        this.funcStarts = starts;
        const ordered = [...starts.keys()].sort((x, y) => x - y);
        this.functions = ordered.map((a, k) => [a, (k + 1 < ordered.length ? ordered[k + 1] : this.textEnd) - a]);
    }
}
