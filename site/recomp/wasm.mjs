// Minimal WebAssembly binary writer: what the recompiler needs, nothing more.

export const I32 = 0x7F, I64 = 0x7E, F32 = 0x7D;

export class Bytes {
    constructor(cap = 256) {
        this.buf = new Uint8Array(cap);
        this.n = 0;
    }
    grow(k) {
        if (this.n + k <= this.buf.length) return;
        let cap = this.buf.length * 2;
        while (cap < this.n + k) cap *= 2;
        const b = new Uint8Array(cap);
        b.set(this.buf.subarray(0, this.n));
        this.buf = b;
    }
    byte(b) {
        this.grow(1);
        this.buf[this.n++] = b;
    }
    bytes(arr) {
        this.grow(arr.length);
        this.buf.set(arr, this.n);
        this.n += arr.length;
    }
    u32(v) {  // unsigned LEB128
        v >>>= 0;
        do {
            let b = v & 0x7F;
            v >>>= 7;
            if (v) b |= 0x80;
            this.byte(b);
        } while (v);
    }
    s32(v) {  // signed LEB128
        v |= 0;
        for (;;) {
            const b = v & 0x7F;
            v >>= 7;
            if ((v === 0 && !(b & 0x40)) || (v === -1 && b & 0x40)) {
                this.byte(b);
                return;
            }
            this.byte(b | 0x80);
        }
    }
    s64(v) {  // signed LEB128 of a BigInt
        v = BigInt.asIntN(64, BigInt(v));
        for (;;) {
            const b = Number(v & 0x7Fn);
            v >>= 7n;
            if ((v === 0n && !(b & 0x40)) || (v === -1n && b & 0x40)) {
                this.byte(b);
                return;
            }
            this.byte(b | 0x80);
        }
    }
    str(s) {
        const e = new TextEncoder().encode(s);
        this.u32(e.length);
        this.bytes(e);
    }
    result() { return this.buf.subarray(0, this.n); }
}

// Opcodes used by the code generator
export const OP = {
    unreachable: 0x00, block: 0x02, loop: 0x03, if: 0x04, else: 0x05, end: 0x0B, br: 0x0C, br_if: 0x0D,
    br_table: 0x0E, return: 0x0F, call: 0x10, drop: 0x1A, select: 0x1B,
    local_get: 0x20, local_set: 0x21, local_tee: 0x22, global_get: 0x23,
    i32_load: 0x28, f32_load: 0x2A, i32_load8_s: 0x2C, i32_load8_u: 0x2D, i32_load16_s: 0x2E, i32_load16_u: 0x2F,
    i32_store: 0x36, f32_store: 0x38, i32_store8: 0x3A, i32_store16: 0x3B,
    i32_const: 0x41, i64_const: 0x42,
    i32_eqz: 0x45, i32_eq: 0x46, i32_ne: 0x47, i32_lt_s: 0x48, i32_lt_u: 0x49, i32_gt_s: 0x4A, i32_gt_u: 0x4B,
    i32_le_s: 0x4C, i32_le_u: 0x4D, i32_ge_s: 0x4E, i32_ge_u: 0x4F,
    f32_eq: 0x5B, f32_ne: 0x5C, f32_lt: 0x5D,
    i32_clz: 0x67, i32_add: 0x6A, i32_sub: 0x6B, i32_mul: 0x6C, i32_div_s: 0x6D, i32_div_u: 0x6E,
    i32_rem_s: 0x6F, i32_rem_u: 0x70, i32_and: 0x71, i32_or: 0x72, i32_xor: 0x73, i32_shl: 0x74,
    i32_shr_s: 0x75, i32_shr_u: 0x76, i32_rotr: 0x78,
    i64_add: 0x7C, i64_sub: 0x7D, i64_mul: 0x7E, i64_or: 0x84, i64_shl: 0x86, i64_shr_u: 0x88,
    f32_abs: 0x8B, f32_neg: 0x8C, f32_ceil: 0x8D, f32_floor: 0x8E, f32_trunc: 0x8F, f32_nearest: 0x90,
    f32_sqrt: 0x91, f32_add: 0x92, f32_sub: 0x93, f32_mul: 0x94, f32_div: 0x95,
    i32_wrap_i64: 0xA7, i64_extend_i32_s: 0xAC, i64_extend_i32_u: 0xAD, f32_convert_i32_s: 0xB2,
    i32_extend8_s: 0xC0, i32_extend16_s: 0xC1,
};

// A function body under construction. Tracks the nesting depth so branches
// can name enclosing constructs by the level they were opened at.
export class Code extends Bytes {
    constructor() {
        super(1024);
        this.depth = 0;
    }
    op(o) { this.byte(o); return this; }
    i32(v) { this.byte(OP.i32_const); this.s32(v); return this; }
    i64(v) { this.byte(OP.i64_const); this.s64(v); return this; }
    get(l) { this.byte(OP.local_get); this.u32(l); return this; }
    set(l) { this.byte(OP.local_set); this.u32(l); return this; }
    tee(l) { this.byte(OP.local_tee); this.u32(l); return this; }
    gget(g) { this.byte(OP.global_get); this.u32(g); return this; }
    call(f) { this.byte(OP.call); this.u32(f); return this; }
    mem(o, offset = 0, align = 0) { this.byte(o); this.u32(align); this.u32(offset); return this; }
    // structured control: open returns the level, used later by br()
    open(o, type = 0x40) { this.byte(o); this.byte(type); return ++this.depth; }
    else_() { this.byte(OP.else); return this; }
    end() { this.byte(OP.end); this.depth--; return this; }
    br(level) { this.byte(OP.br); this.u32(this.depth - level); return this; }
    brTable(levels, deflt) {
        this.byte(OP.br_table);
        this.u32(levels.length);
        for (const l of levels) this.u32(this.depth - l);
        this.u32(this.depth - deflt);
        return this;
    }
    satTruncF32S() { this.byte(0xFC); this.u32(0); return this; }  // i32.trunc_sat_f32_s
}

// Module assembly
export class Module {
    constructor() {
        this.types = [];  // [params[], results[]]
        this.typeKeys = new Map();
        this.imports = [];  // {mod, name, kind, desc}
        this.nImportFuncs = 0;
        this.funcs = [];  // {type, locals: [[count, type]], code: Uint8Array}
        this.exports = [];  // {name, kind, index}
    }
    type(params, results = []) {
        const k = params.join(",") + "->" + results.join(",");
        if (!this.typeKeys.has(k)) {
            this.typeKeys.set(k, this.types.length);
            this.types.push([params, results]);
        }
        return this.typeKeys.get(k);
    }
    importFunc(mod, name, type) {
        this.imports.push({ mod, name, kind: 0, type });
        return this.nImportFuncs++;
    }
    importMemory(mod, name, min, max, shared) { this.imports.push({ mod, name, kind: 2, min, max, shared }); }
    importGlobal(mod, name, valtype) { this.imports.push({ mod, name, kind: 3, valtype }); }
    // reserve an index now, provide the body later (bodies may call each other)
    declare(type) {
        this.funcs.push({ type, locals: [], code: null });
        return this.nImportFuncs + this.funcs.length - 1;
    }
    define(index, locals, code) {
        const f = this.funcs[index - this.nImportFuncs];
        f.locals = locals;
        f.code = code;
    }
    exportFunc(name, index) { this.exports.push({ name, kind: 0, index }); }

    encode() {
        const out = new Bytes(1 << 20);
        out.bytes([0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00]);
        const section = (id, fill) => {
            const s = new Bytes(1 << 16);
            fill(s);
            out.byte(id);
            out.u32(s.n);
            out.bytes(s.result());
        };
        section(1, (s) => {
            s.u32(this.types.length);
            for (const [p, r] of this.types) {
                s.byte(0x60);
                s.u32(p.length);
                p.forEach((t) => s.byte(t));
                s.u32(r.length);
                r.forEach((t) => s.byte(t));
            }
        });
        section(2, (s) => {
            s.u32(this.imports.length);
            for (const im of this.imports) {
                s.str(im.mod);
                s.str(im.name);
                s.byte(im.kind);
                if (im.kind === 0) s.u32(im.type);
                else if (im.kind === 2) {
                    s.byte(im.shared ? 0x03 : 0x01);
                    s.u32(im.min);
                    s.u32(im.max);
                } else {
                    s.byte(im.valtype);
                    s.byte(0);  // immutable
                }
            }
        });
        section(3, (s) => {
            s.u32(this.funcs.length);
            for (const f of this.funcs) s.u32(f.type);
        });
        section(7, (s) => {
            s.u32(this.exports.length);
            for (const e of this.exports) {
                s.str(e.name);
                s.byte(e.kind);
                s.u32(e.index);
            }
        });
        section(10, (s) => {
            s.u32(this.funcs.length);
            for (const f of this.funcs) {
                const body = new Bytes(f.code.length + 16);
                body.u32(f.locals.length);
                for (const [count, t] of f.locals) {
                    body.u32(count);
                    body.byte(t);
                }
                body.bytes(f.code);
                body.byte(OP.end);
                s.u32(body.n);
                s.bytes(body.result());
            }
        });
        return out.result();
    }
}
