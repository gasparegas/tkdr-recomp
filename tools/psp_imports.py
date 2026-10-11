#!/usr/bin/env python3
"""List the libraries and function NIDs a PSP PRX/ELF imports (.lib.stub)."""
import struct
import sys


def load_sections(d):
    shoff, = struct.unpack_from("<I", d, 0x20)
    shnum, shstrndx = struct.unpack_from("<HH", d, 0x30)
    secs = [struct.unpack_from("<10I", d, shoff + 40 * i) for i in range(shnum)]
    strtab = secs[shstrndx][4]
    out = {}
    for s in secs:
        name = d[strtab + s[0]:d.index(b"\0", strtab + s[0])].decode()
        out[name] = {"addr": s[3], "off": s[4], "size": s[5]}
    return out


def imports(path):
    d = open(path, "rb").read()
    secs = load_sections(d)
    alloc = [s for s in secs.values() if s["addr"] or s["off"]]

    def v2o(va):  # virtual address -> file offset
        for s in alloc:
            if s["addr"] and s["addr"] <= va < s["addr"] + s["size"]:
                return s["off"] + va - s["addr"]
        raise ValueError(hex(va))

    def cstr(va):
        o = v2o(va)
        return d[o:d.index(b"\0", o)].decode()

    stub = secs[".lib.stub"]
    pos, end = stub["off"], stub["off"] + stub["size"]
    libs = []
    while pos < end:
        name_va, ver, attr, entsz, nvars, nfuncs, nids_va, stubs_va = struct.unpack_from(
            "<IHHBBHII", d, pos)
        entsz = entsz or 5
        nids = struct.unpack_from(f"<{nfuncs}I", d, v2o(nids_va)) if nfuncs else ()
        libs.append({
            "name": cstr(name_va) if name_va else "?",
            "nids": nids,
            "stubs": [stubs_va + 8 * i for i in range(nfuncs)],
        })
        pos += entsz * 4
    return libs


if __name__ == "__main__":
    libs = imports(sys.argv[1])
    total = sum(len(l["nids"]) for l in libs)
    print(f"{len(libs)} libraries, {total} imported functions")
    for l in libs:
        print(f"  {l['name']:<28} {len(l['nids']):3d}")
        if "-v" in sys.argv:
            for nid, st in zip(l["nids"], l["stubs"]):
                print(f"      0x{nid:08X}  stub @ 0x{st:08X}")
