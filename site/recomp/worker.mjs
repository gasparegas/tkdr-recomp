// Runs the recompiler off the page's main thread.
import { recompile } from "./index.mjs";

self.onmessage = async (e) => {
    const { eboot } = e.data;
    try {
        const names = await (await fetch(new URL("./nids.json", import.meta.url))).json();
        const out = recompile(new Uint8Array(eboot), names, (stage, frac) => self.postMessage({ progress: { stage, frac } }));
        self.postMessage({ result: out }, [out.image.buffer, out.code.buffer]);
    } catch (err) {
        self.postMessage({ error: String(err && err.stack || err) });
    }
};
