// Load a PSP PRX (ET_SCE_PRX) and relocate it to a fixed base address.
// Port of recomp/elf.py; runs in the browser and in Node.

export const BASE = 0x08804000;  // where the PSP kernel loads the first user PRX
const SHT_PRXRELOC = 0x700000A0;
export const R_MIPS_32 = 2, R_MIPS_26 = 4, R_MIPS_HI16 = 5, R_MIPS_LO16 = 6;

const sext16 = (x) => (x & 0x8000 ? x - 0x10000 : x);

export class Image {
    constructor(base, mem, segments, sections, entry) {
        this.base = base;
        this.mem = mem;  // Uint8Array covering [base, base + mem.length)
        this.view = new DataView(mem.buffer, mem.byteOffset, mem.byteLength);
        this.segments = segments;
        this.sections = sections;
        this.entry = entry;
        this.relocs = new Map();  // location -> [type, target], for analysis
    }
    u32(addr) { return this.view.getUint32(addr - this.base, true); }
    setU32(addr, v) { this.view.setUint32(addr - this.base, v >>> 0, true); }
    cstr(addr) {
        let o = addr - this.base, e = o;
        while (this.mem[e]) e++;
        return new TextDecoder().decode(this.mem.subarray(o, e));
    }
    contains(addr) { return addr >= this.base && addr < this.base + this.mem.length; }
}

export function load(bytes, base = BASE) {
    const d = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const u32 = (o) => d.getUint32(o, true), u16 = (o) => d.getUint16(o, true);
    const entry = u32(0x18), phoff = u32(0x1C), shoff = u32(0x20);
    const phnum = u16(0x2C), shnum = u16(0x30), shstrndx = u16(0x32);

    const segs = [];
    for (let i = 0; i < phnum; i++) {
        const o = phoff + 32 * i;
        if (u32(o) === 1) segs.push({ vaddr: base + u32(o + 8), offset: u32(o + 4), filesz: u32(o + 16), memsz: u32(o + 20) });
    }
    const size = Math.max(...segs.map((s) => s.vaddr + s.memsz)) - base;
    const mem = new Uint8Array(size);
    for (const s of segs) mem.set(bytes.subarray(s.offset, s.offset + s.filesz), s.vaddr - base);

    const raw = [];
    for (let i = 0; i < shnum; i++) {
        const o = shoff + 40 * i, f = [];
        for (let k = 0; k < 10; k++) f.push(u32(o + 4 * k));
        raw.push(f);
    }
    const strtab = raw[shstrndx][4];
    const sections = {};
    for (const [nameOff, type, flags, addr, off, sz, , info] of raw) {
        let e = strtab + nameOff;
        while (bytes[e]) e++;
        const name = new TextDecoder().decode(bytes.subarray(strtab + nameOff, e));
        sections[name] = { name, type, addr: flags & 2 ? base + addr : 0, offset: off, size: sz, info };
    }
    const img = new Image(base, mem, segs, sections, (base + entry) >>> 0);
    for (const sec of Object.values(sections))
        if (sec.type === SHT_PRXRELOC) applyRelocs(img, bytes.subarray(sec.offset, sec.offset + sec.size));
    return img;
}

// PSP-style relocation: each entry names the segment holding the location
// (ofs_base) and the segment the stored value is relative to (addr_base).
// Patching follows the PSP kernel loader: a HI16 takes its low half from the
// next LO16 in list order; a LO16 just adds the base to its own 16 bits.
function applyRelocs(img, data) {
    const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const entries = [];
    for (let i = 0; i + 8 <= data.length; i += 8) {
        const rOffset = dv.getUint32(i, true), info = dv.getUint32(i + 4, true);
        const loc = (img.segments[(info >> 8) & 0xFF].vaddr + rOffset) >>> 0;
        entries.push([info & 0xFF, loc, img.segments[(info >> 16) & 0xFF].vaddr]);
    }
    const orig = new Map();
    for (const [, loc] of entries) orig.set(loc, img.u32(loc));
    const nextLo = new Array(entries.length);
    let nxt = null;
    for (let k = entries.length - 1; k >= 0; k--) {
        nextLo[k] = nxt;
        if (entries[k][0] === R_MIPS_LO16) nxt = entries[k];
    }
    entries.forEach(([typ, loc, relTo], k) => {
        let word = orig.get(loc);
        if (typ === R_MIPS_32) word = (word + relTo) >>> 0;
        else if (typ === R_MIPS_26) {
            const target = ((word & 0x03FFFFFF) << 2) + relTo;
            word = ((word & 0xFC000000) | ((target >>> 2) & 0x03FFFFFF)) >>> 0;
        } else if (typ === R_MIPS_HI16) {
            const lo = nextLo[k] ? sext16(orig.get(nextLo[k][1]) & 0xFFFF) : 0;
            const full = (word & 0xFFFF) * 65536 + lo + relTo;
            const hi = (Math.floor(full / 65536) + (full & 0x8000 ? 1 : 0)) & 0xFFFF;
            word = ((word & 0xFFFF0000) | hi) >>> 0;
        } else if (typ === R_MIPS_LO16) word = ((word & 0xFFFF0000) | ((word + relTo) & 0xFFFF)) >>> 0;
        else throw new Error(`unsupported reloc type ${typ} at ${loc.toString(16)}`);
        img.setU32(loc, word);
    });

    // Targets for analysis, from the patched code. A LO16 user reads the
    // register the matching lui wrote; pair them by register in list order.
    const hiVal = new Map();
    for (const [typ, loc] of entries) {
        const word = img.u32(loc);
        if (typ === R_MIPS_32) img.relocs.set(loc, [typ, word]);
        else if (typ === R_MIPS_26) img.relocs.set(loc, [typ, ((((loc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)) >>> 0)]);
        else if (typ === R_MIPS_HI16) hiVal.set((word >>> 16) & 31, ((word & 0xFFFF) << 16) >>> 0);
        else if (typ === R_MIPS_LO16) {
            const reg = (word >>> 21) & 31;
            if (hiVal.has(reg)) img.relocs.set(loc, [typ, (hiVal.get(reg) + sext16(word & 0xFFFF)) >>> 0]);
        }
    }
    // every HI16 gets the target of the LO16 sharing its register, if any
    for (const [typ, loc] of entries)
        if (typ === R_MIPS_HI16 && !img.relocs.has(loc)) img.relocs.set(loc, [typ, ((img.u32(loc) & 0xFFFF) << 16) >>> 0]);
}
