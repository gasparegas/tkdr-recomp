// In-browser recompiler: the player's decrypted EBOOT -> the files the
// runtime (RECOMP_DYNAMIC build) reads from /game. Nothing leaves the device.
import { Program } from "./analyze.mjs";
import { generate } from "./wasmgen.mjs";

const SOURCES = ["elf.mjs", "mips.mjs", "analyze.mjs", "wasm.mjs", "wasmgen.mjs", "index.mjs"];
export const EBOOT_SHA1 = "b9a0e3292738a833f4f8077e3959c3a0652c42cd";  // ULUS-10139 v1.00

const hex = (v) => (v >>> 0).toString(16).padStart(8, "0");

export async function sha1(bytes) {
    const d = await crypto.subtle.digest("SHA-1", bytes);
    return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// Identifies this recompiler: cached translations made by another version are not reused.
export async function version() {
    const texts = await Promise.all(SOURCES.map((f) => fetch(new URL("./" + f, import.meta.url)).then((r) => r.text())));
    return (await sha1(new TextEncoder().encode(texts.join("\0")))).slice(0, 12);
}

// names: { library: { "0xNID": "name" } } (recomp/nids.json)
export function recompile(eboot, names, onProgress = () => {}) {
    onProgress("analysing", 0);
    const p = new Program(eboot);
    onProgress("generating", 0);
    const code = generate(p, { onProgress: (k, n) => onProgress("generating", k / n) });
    const stubs = [...p.stubs.keys()].sort((a, b) => a - b);
    const lines = [`${hex(p.img.base)} ${hex(p.img.mem.length)} ${hex(p.img.entry)} ${stubs.length}`];
    for (const a of stubs) {
        const [lib, nid] = p.stubs.get(a);
        const key = "0x" + hex(nid).toUpperCase();
        lines.push(`${hex(a)} ${lib} ${names[lib]?.[key] ?? `${lib}_${hex(nid).toUpperCase()}`} ${hex(nid)}`);
    }
    onProgress("done", 1);
    return { image: p.img.mem, gameTxt: lines.join("\n") + "\n", code, functions: p.functions.length };
}
