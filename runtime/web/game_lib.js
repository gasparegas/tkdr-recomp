// Calls from the runtime into the game's code (RECOMP_DYNAMIC builds).
//
// The game's code is a WebAssembly module generated in the browser from the
// player's own EBOOT (web/recomp). Every thread that runs guest code (each PSP
// thread is a Web Worker) needs its own instance, wired to this thread's
// runtime exports and to the shared memory. The page compiles the module once
// and posts it to the workers (tkShareGameModule) before starting the game; a
// worker that did not get it compiles the copy the runtime keeps in memory.
addToLibrary({
    $tkGame: { module: null, instance: null },
    $tkGame__postset: `
if (ENVIRONMENT_IS_PTHREAD) {
  self.addEventListener('message', (e) => {
    if (e.data && e.data.tkGameModule) tkGame.module = e.data.tkGameModule;
  });
}`,

    $tkGameInstance__deps: ['$tkGame'],
    $tkGameInstance: () => {
        if (tkGame.instance) return tkGame.instance;
        let mod = tkGame.module;
        if (!mod) {
            const p = wasmExports['game_code_ptr'](), n = wasmExports['game_code_len']();
            mod = tkGame.module = new WebAssembly.Module(HEAPU8.slice(p, p + n));
        }
        const env = {
            'memory': wasmMemory,
            'gmem': new WebAssembly.Global({ value: 'i32', mutable: false }, wasmExports['game_gmem']()),
            'hle_call': wasmExports['hle_call'],
            'vfpu_exec': wasmExports['vfpu_exec'],
            'recomp_trap_code': wasmExports['recomp_trap_code'],
            'recomp_call_miss': wasmExports['recomp_call_miss'],
        };
        return (tkGame.instance = new WebAssembly.Instance(mod, { 'env': env }));
    },

    game_dispatch__deps: ['$tkGameInstance'],
    game_dispatch: (ctx, addr) => {
        tkGameInstance().exports['dispatch'](ctx, addr);
    },

    // Page side: give the compiled module to every worker (call before main()).
    $tkShareGameModule__deps: ['$tkGame', '$PThread'],
    $tkShareGameModule: (mod) => {
        tkGame.module = mod;
        const workers = PThread.unusedWorkers.concat(Object.values(PThread.pthreads || {}), PThread.runningWorkers || []);
        for (const w of new Set(workers)) w.postMessage({ tkGameModule: mod });
    },
    $tkShareGameModule__postset: "Module['tkShareGameModule'] = tkShareGameModule;",
});
