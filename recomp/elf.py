"""Load a PSP PRX (ET_SCE_PRX) and relocate it to a fixed base address."""
import struct
from dataclasses import dataclass, field

BASE = 0x08804000  # where the PSP kernel loads the first user PRX

SHT_PRXRELOC = 0x700000A0
R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6


@dataclass
class Section:
    name: str
    type: int
    addr: int  # relocated address (0 if not allocated)
    offset: int
    size: int
    info: int


@dataclass
class Segment:
    vaddr: int  # relocated
    offset: int
    filesz: int
    memsz: int


@dataclass
class Image:
    base: int
    mem: bytearray  # covers [base, base + len(mem))
    segments: list
    sections: dict
    entry: int
    # address -> (type, target) for every relocated location
    relocs: dict = field(default_factory=dict)

    def u32(self, addr):
        return struct.unpack_from("<I", self.mem, addr - self.base)[0]

    def cstr(self, addr):
        o = addr - self.base
        return self.mem[o:self.mem.index(b"\0", o)].decode(errors="replace")

    def contains(self, addr):
        return self.base <= addr < self.base + len(self.mem)


def load(path, base=BASE):
    d = open(path, "rb").read()
    entry, phoff, shoff = struct.unpack_from("<III", d, 0x18)
    phnum, = struct.unpack_from("<H", d, 0x2C)
    shnum, shstrndx = struct.unpack_from("<HH", d, 0x30)

    segs = []
    for i in range(phnum):
        p_type, off, vaddr, _paddr, filesz, memsz, _flags, _align = struct.unpack_from(
            "<8I", d, phoff + 32 * i)
        if p_type == 1:
            segs.append(Segment(base + vaddr, off, filesz, memsz))

    size = max(s.vaddr + s.memsz for s in segs) - base
    mem = bytearray(size)
    for s in segs:
        mem[s.vaddr - base:s.vaddr - base + s.filesz] = d[s.offset:s.offset + s.filesz]

    raw = [struct.unpack_from("<10I", d, shoff + 40 * i) for i in range(shnum)]
    strtab = raw[shstrndx][4]
    sections = {}
    for name_off, typ, flags, addr, off, sz, _link, info, _align, _entsz in raw:
        name = d[strtab + name_off:d.index(b"\0", strtab + name_off)].decode()
        alloc = flags & 2
        sections[name] = Section(name, typ, base + addr if alloc else 0, off, sz, info)

    img = Image(base, mem, segs, sections, base + entry)
    for sec in sections.values():
        if sec.type == SHT_PRXRELOC:
            _apply_relocs(img, d[sec.offset:sec.offset + sec.size])
    return img


def _sext16(x):
    return x - 0x10000 if x & 0x8000 else x


def _apply_relocs(img, data):
    """PSP-style relocation: each entry names the segment holding the location
    (ofs_base) and the segment the stored value is relative to (addr_base).

    Patching follows the PSP kernel loader: a HI16 takes its low half from the
    next LO16 in list order; a LO16 just adds the base to its own 16 bits.
    """
    m, b = img.mem, img.base
    entries = []
    for i in range(0, len(data), 8):
        r_offset, info = struct.unpack_from("<II", data, i)
        loc = img.segments[(info >> 8) & 0xFF].vaddr + r_offset
        entries.append((info & 0xFF, loc, img.segments[(info >> 16) & 0xFF].vaddr))
    orig = {loc: struct.unpack_from("<I", m, loc - b)[0] for _, loc, _ in entries}
    next_lo, nxt = [None] * len(entries), None
    for k in range(len(entries) - 1, -1, -1):
        next_lo[k] = nxt
        if entries[k][0] == R_MIPS_LO16:
            nxt = entries[k]

    for k, (typ, loc, rel_to) in enumerate(entries):
        word = orig[loc]
        if typ == R_MIPS_32:
            word = (word + rel_to) & 0xFFFFFFFF
        elif typ == R_MIPS_26:
            target = ((word & 0x03FFFFFF) << 2) + rel_to
            word = (word & 0xFC000000) | ((target >> 2) & 0x03FFFFFF)
        elif typ == R_MIPS_HI16:
            lo = _sext16(orig[next_lo[k][1]] & 0xFFFF) if next_lo[k] else 0
            full = ((word & 0xFFFF) << 16) + lo + rel_to
            hi = ((full >> 16) + (1 if full & 0x8000 else 0)) & 0xFFFF
            word = (word & 0xFFFF0000) | hi
        elif typ == R_MIPS_LO16:
            word = (word & 0xFFFF0000) | ((word + rel_to) & 0xFFFF)
        else:
            raise ValueError(f"unsupported reloc type {typ} at {loc:#x}")
        struct.pack_into("<I", m, loc - b, word)

    # Targets for analysis, from the patched code. A LO16 user reads the
    # register the matching lui wrote; pair them by register in list order.
    hi_val = {}
    for typ, loc, _ in entries:
        word = img.u32(loc)
        if typ == R_MIPS_32:
            img.relocs[loc] = (typ, word)
        elif typ == R_MIPS_26:
            img.relocs[loc] = (typ, ((loc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2))
        elif typ == R_MIPS_HI16:
            hi_val[(word >> 16) & 31] = (word & 0xFFFF) << 16
        elif typ == R_MIPS_LO16:
            reg = (word >> 21) & 31
            if reg in hi_val:
                img.relocs[loc] = (typ, (hi_val[reg] + _sext16(word & 0xFFFF)) & 0xFFFFFFFF)
    # every HI16 gets the target of the LO16 sharing its register, if any
    for typ, loc, _ in entries:
        if typ == R_MIPS_HI16:
            word = img.u32(loc)
            img.relocs.setdefault(loc, (typ, (word & 0xFFFF) << 16))
