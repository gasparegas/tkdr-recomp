# Tekken: Dark Resurrection (PSP) recomp — notes

## Base version
- ULUS-10139, DISC_VERSION 1.00, firmware req. 2.60
- ISO SHA-1 `8193a65dee49685db49284408c89bdb35324bc32`, CRC32 `3ffd7a9d`, 1,750,237,184 bytes
- Game files live in `game/` (gitignored). `tools/cso2iso.py` rebuilds the ISO from the CSO.

## Executable (`game/tkpsp.elf`)
- `EBOOT.BIN` is encrypted (`~PSP`); `BOOT.BIN` is all zeros.
- Decrypted via PPSSPP 1.20.4 (`DumpFileTypes = 1` in `ppsspp.ini`) →
  `~/.config/ppsspp/PSP/SYSTEM/DUMP/ULUS10139_EBOOT.BIN`.
- ELF type `ET_SCE_PRX` (0xFFA0), module name `tkpsp`, relocatable (base 0).
- Full relocation sections kept (`.rel.text`, `.rel.rodata`, `.rel.data`, …):
  every code/data pointer is known — big win for static recompilation.
- No symbol table. Code is C++ with exceptions: `.eh_frame` has 7310 FDEs inside
  `.text` → exact function boundaries for ~74% of `.text` (rest: C code without EH).
- Compiler: `GCC 3.3.6+allegrex-2.2.2-psp-1.9.2` (951 objects), plus 6 objects
  from `3.3.3 … psp-1.8.4.internal` (SDK libs).
- Leftover source names in `.rodata`: hagame2d.cpp, haswg*.cpp, tbskinmodel.cpp,
  dwdataman.cpp, owtheater.cpp, zs_efct.cpp, …

## Sections
| Section | Addr | Size |
|---|---|---|
| .text | 0x00000040 | 0x272ae0 (~2.4 MB) |
| .rodata | 0x002c17a0 | 0x833a8 |
| .data | 0x00344b80 | 0x2c58c |
| .bss | 0x00371180 | 0x103b614 (~16 MB) |

## Imports (`tools/psp_imports.py`)
31 libraries, 280 functions. Notable: ThreadManForUser (41), sceMpeg (25),
sceHttp (23), IoFileMgrForUser (23), sceSasCore (21), sceUtility (20),
sceNetAdhoc* (31 total, ad-hoc multiplayer), sceGe_user (6), sceAtrac3plus (9).

## Data
- `USRDIR/TKPDATA0.BIN` (412 MB), `TKPDATA1.BIN` (844 MB): asset archives,
  header looks compressed/encrypted — not analysed yet.
- Sony modules bundled in USRDIR (audiocodec, libatrac3plus, mpeg, sascore,
  videocodec): system code, to be replaced by HLE.
