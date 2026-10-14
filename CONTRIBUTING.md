# Contributing

Contributions are welcome: bug reports, fixes, HLE functions, renderer work,
input devices, other platforms, docs.

## The one hard rule: no game data

Nothing derived from a game may enter this repository, an issue or a pull
request:

- no ISO/CSO, EBOOT, PRX, assets, save files or screenshots dumps of game files;
- no translated code (`build/gen/`) or builds made from it (`build/web/`,
  `build/native/`);
- no encryption keys and no tools that decrypt PSP executables;
- no links to places where any of the above can be downloaded.

Pull requests that contain any of it are closed. Logs, stack traces, addresses,
function names you discovered and short descriptions of what the game does are
fine.

## Building and testing

See `README.md` to build from your own dump. Useful while working (all in
`NOTES.md`):

- `HEADLESS=1 RUN_SECONDS=n INPUT="118:START,118.3:" SHOTS=dir`: scripted runs
  with a screenshot every second;
- `GE_CAPTURE` / `GE_REPLAY`: save one frame of GPU commands and draw it again
  offline (benchmarks, before/after images);
- `HIDDEN_WINDOW=1 GL_SHOTS=dir`: the GPU renderer without a window;
- `RECOMP_CHECK=1`, `RECOMP_PROBE`, `HLE_LOG`, `HLE_BT`: recompiler and HLE
  diagnostics;
- for the in-browser recompiler (`web/recomp/`): its analysis must stay
  identical to the Python one in `recomp/`.

## Style

Match the code around you: C11/C++17, 4-space indentation, short comments that
say why. Keep commits focused, with a message that says what changed and why.

## License

By contributing you agree that your contribution is licensed under the MIT
License of this project (`LICENSE`).
