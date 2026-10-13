// The player's game files in the browser: a plain ISO, a CSO, or a .zip holding
// them (with the EBOOT). The disc is kept in the browser's own storage (OPFS) as
// a CSO with large blocks: compressed, yet readable at any offset, one block at
// a time. Uses the browser's built-in deflate (CompressionStream).

export const BLOCK = 256 * 1024;  // CSO block size we write (the runtime reads 256 KB at a time)

const u8 = (b) => new Uint8Array(b);
const readSlice = async (blob, start, end) => u8(await blob.slice(start, end).arrayBuffer());

async function inflate(data) {  // raw deflate
    const s = new Blob([data]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
    return u8(await new Response(s).arrayBuffer());
}
async function deflate(data) {
    const s = new Blob([data]).stream().pipeThrough(new CompressionStream("deflate-raw"));
    return u8(await new Response(s).arrayBuffer());
}

// ---------------------------------------------------------------- reading a disc

// A disc image: read(off, len) -> Promise<Uint8Array> of exactly len bytes (zeros past the end).
export async function openDisc(blob) {
    const head = await readSlice(blob, 0, 24);
    const dv = new DataView(head.buffer);
    if (String.fromCharCode(...head.subarray(0, 4)) !== "CISO") {
        return {
            kind: "iso",
            size: blob.size,
            read: async (off, len) => {
                const out = new Uint8Array(len);
                out.set(await readSlice(blob, off, Math.min(off + len, blob.size)));
                return out;
            },
        };
    }
    const total = Number(dv.getBigUint64(8, true)), bs = dv.getUint32(16, true), align = dv.getUint8(21);
    const nblocks = Math.ceil(total / bs);
    const index = new Uint32Array((await readSlice(blob, 24, 24 + 4 * (nblocks + 1))).buffer);
    const pos = (k) => (index[k] & 0x7FFFFFFF) * 2 ** align;
    return {
        kind: "cso",
        size: total,
        read: async (off, len) => {
            const out = new Uint8Array(len);
            const end = Math.min(off + len, total);
            if (off >= end) return out;
            const b0 = Math.floor(off / bs), b1 = Math.floor((end - 1) / bs);
            const raw = await readSlice(blob, pos(b0), pos(b1 + 1));  // one read for all the blocks
            const base = pos(b0);
            for (let b = b0; b <= b1; b++) {
                const chunk = raw.subarray(pos(b) - base, pos(b + 1) - base);
                const plain = index[b] & 0x80000000 ? chunk : await inflate(chunk);
                const bstart = b * bs, from = Math.max(off, bstart), to = Math.min(end, bstart + plain.length);
                if (to > from) out.set(plain.subarray(from - bstart, to - bstart), from - off);
            }
            return out;
        },
    };
}

// ---------------------------------------------------------------- zip archives

// Entries of a .zip (zip64 included): {name, method, csize, usize, local}
export async function zipEntries(file) {
    const tailLen = Math.min(file.size, 65536 + 22);
    const tail = await readSlice(file, file.size - tailLen, file.size);
    const tv = new DataView(tail.buffer);
    let e = -1;
    for (let i = tail.length - 22; i >= 0; i--)
        if (tv.getUint32(i, true) === 0x06054b50) { e = i; break; }
    if (e < 0) throw new Error("not a zip file");
    let count = tv.getUint16(e + 10, true), cdSize = tv.getUint32(e + 12, true), cdOff = tv.getUint32(e + 16, true);
    if (e >= 20 && tv.getUint32(e - 20, true) === 0x07064b50) {  // zip64 end of central directory
        const z64 = Number(tv.getBigUint64(e - 20 + 8, true));
        const r = new DataView((await readSlice(file, z64, z64 + 56)).buffer);
        count = Number(r.getBigUint64(32, true));
        cdSize = Number(r.getBigUint64(40, true));
        cdOff = Number(r.getBigUint64(48, true));
    }
    const cd = await readSlice(file, cdOff, cdOff + cdSize), v = new DataView(cd.buffer);
    const entries = [];
    for (let o = 0, k = 0; k < count && o + 46 <= cd.length; k++) {
        if (v.getUint32(o, true) !== 0x02014b50) break;
        const method = v.getUint16(o + 10, true);
        let csize = v.getUint32(o + 20, true), usize = v.getUint32(o + 24, true), local = v.getUint32(o + 42, true);
        const nlen = v.getUint16(o + 28, true), xlen = v.getUint16(o + 30, true), clen = v.getUint16(o + 32, true);
        const name = new TextDecoder().decode(cd.subarray(o + 46, o + 46 + nlen));
        for (let x = o + 46 + nlen; x + 4 <= o + 46 + nlen + xlen;) {  // zip64 sizes and offset
            const id = v.getUint16(x, true), sz = v.getUint16(x + 2, true);
            if (id === 1) {
                let p = x + 4;
                if (usize === 0xFFFFFFFF) { usize = Number(v.getBigUint64(p, true)); p += 8; }
                if (csize === 0xFFFFFFFF) { csize = Number(v.getBigUint64(p, true)); p += 8; }
                if (local === 0xFFFFFFFF) local = Number(v.getBigUint64(p, true));
            }
            x += 4 + sz;
        }
        if (!name.endsWith("/")) entries.push({ name, method, csize, usize, local });
        o += 46 + nlen + xlen + clen;
    }
    return entries;
}

async function entryData(file, ent) {  // [start, end) of the entry's stored bytes
    const h = new DataView((await readSlice(file, ent.local, ent.local + 30)).buffer);
    const start = ent.local + 30 + h.getUint16(26, true) + h.getUint16(28, true);
    return [start, start + ent.csize];
}

export async function entryStream(file, ent) {
    const [a, b] = await entryData(file, ent);
    const s = file.slice(a, b).stream();
    if (ent.method === 0) return s;
    if (ent.method === 8) return s.pipeThrough(new DecompressionStream("deflate-raw"));
    throw new Error(`${ent.name}: unsupported zip compression (method ${ent.method})`);
}

export async function entryBytes(file, ent) {
    return u8(await new Response(await entryStream(file, ent)).arrayBuffer());
}

// ---------------------------------------------------------------- sorting out what was picked

const DISC_MIN = 64 << 20, EBOOT_MAX = 64 << 20, EBOOT_MIN = 512 << 10;

// files: File[] (picked or dropped). Returns {disc: {blob} | {zip, entry}, ebootCandidates: [() => Promise<Uint8Array>]}
export async function sortFiles(files) {
    const out = { disc: null, ebootCandidates: [] };
    for (const f of files) {
        const lower = f.name.toLowerCase();
        if (lower.endsWith(".zip")) {
            const entries = await zipEntries(f);
            const discs = entries.filter((e) => /\.(iso|cso)$/i.test(e.name) || e.usize >= DISC_MIN)
                .sort((a, b) => b.usize - a.usize);
            if (discs.length && !out.disc) out.disc = { zip: f, entry: discs[0] };
            entries.filter((e) => e.usize >= EBOOT_MIN && e.usize <= EBOOT_MAX && !/\.(iso|cso)$/i.test(e.name))
                .sort((a, b) => (/eboot/i.test(b.name) ? 1 : 0) - (/eboot/i.test(a.name) ? 1 : 0))
                .forEach((e) => out.ebootCandidates.push(() => entryBytes(f, e)));
        } else if (/\.(iso|cso)$/i.test(lower) || f.size >= DISC_MIN) {
            if (!out.disc) out.disc = { blob: f };
        } else if (f.size >= EBOOT_MIN && f.size <= EBOOT_MAX) {
            out.ebootCandidates.push(async () => u8(await f.arrayBuffer()));
        }
    }
    return out;
}

// ---------------------------------------------------------------- compressed copy in OPFS

// Write `stream` (an uncompressed disc image of `total` bytes) as a CSO into the
// OPFS file `name`. A CSO given as input is copied as it is.
export async function storeDisc(stream, total, name, onProgress = () => {}) {
    const root = await navigator.storage.getDirectory();
    const fh = await root.getFileHandle(name, { create: true });
    await writeCso(stream, total, await fh.createWritable(), onProgress);
}

// w: a FileSystemWritableFileStream (or anything with the same write/close)
export async function writeCso(stream, total, w, onProgress = () => {}) {
    const reader = stream.getReader();
    // peek: already a CSO?
    let pending = [], have = 0;
    const take = async (n) => {  // exactly n bytes (fewer at the end of the stream)
        while (have < n) {
            const { value, done } = await reader.read();
            if (done) break;
            pending.push(value);
            have += value.length;
        }
        const len = Math.min(n, have), out = new Uint8Array(len);
        let o = 0;
        while (o < len) {
            const c = pending[0], k = Math.min(c.length, len - o);
            out.set(c.subarray(0, k), o);
            o += k;
            if (k === c.length) pending.shift();
            else pending[0] = c.subarray(k);
        }
        have -= len;
        return out;
    };
    const first = await take(4);
    if (String.fromCharCode(...first) === "CISO") {
        await w.write(first);
        let done = 4;
        for (;;) {
            const chunk = await take(4 << 20);
            if (!chunk.length) break;
            await w.write(chunk);
            done += chunk.length;
            onProgress(done / total);
        }
        await w.close();
        return;
    }
    // compress: blocks are deflated a few at a time, written in order after room for header + index
    const nblocks = Math.ceil(total / BLOCK);
    const align = total > 0x7FFFFFFF ? 2 : 0;  // offsets are stored >> align in 31 bits
    const index = new Uint32Array(nblocks + 1);
    const headerSize = 24 + 4 * (nblocks + 1);
    let pos = Math.ceil(headerSize / 2 ** align) * 2 ** align;
    let carry = first;
    const PARALLEL = 4;
    for (let b = 0; b < nblocks; b += PARALLEL) {
        const raw = [];
        for (let k = b; k < Math.min(nblocks, b + PARALLEL); k++) {
            const want = Math.min(BLOCK, total - k * BLOCK);
            let blk;
            if (carry.length) {
                const rest = await take(want - carry.length);
                blk = new Uint8Array(carry.length + rest.length);
                blk.set(carry);
                blk.set(rest, carry.length);
                carry = new Uint8Array(0);
            } else blk = await take(want);
            raw.push(blk);
        }
        const packed = await Promise.all(raw.map(deflate));
        for (let i = 0; i < raw.length; i++) {
            const k = b + i, keepRaw = packed[i].length >= raw[i].length;
            const data = keepRaw ? raw[i] : packed[i];
            index[k] = (pos / 2 ** align) | (keepRaw ? 0x80000000 : 0);
            await w.write({ type: "write", position: pos, data });
            pos += data.length;
            const pad = (2 ** align - (pos % 2 ** align)) % 2 ** align;
            if (pad) {
                await w.write({ type: "write", position: pos, data: new Uint8Array(pad) });
                pos += pad;
            }
        }
        onProgress(Math.min(1, ((b + raw.length) * BLOCK) / total));
    }
    index[nblocks] = pos / 2 ** align;
    const header = new Uint8Array(headerSize), hv = new DataView(header.buffer);
    header.set([0x43, 0x49, 0x53, 0x4F]);  // "CISO"
    hv.setUint32(4, 24, true);
    hv.setBigUint64(8, BigInt(total), true);
    hv.setUint32(16, BLOCK, true);
    hv.setUint8(20, 1);
    hv.setUint8(21, align);
    header.set(new Uint8Array(index.buffer), 24);
    await w.write({ type: "write", position: 0, data: header });
    await w.close();
}
