# FatFs

Vendored from ChaN's official FatFs **R0.16** archive:
[ff16.zip](https://elm-chan.org/fsw/ff/arc/ff16.zip), retrieved 2026-10-17.

Archive SHA-256:
`99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e`

`ff.c`, `ff.h`, `ffunicode.c`, `diskio.h`, and `LICENSE.txt` are unmodified.
`ffconf.h` changes only these upstream defaults:

| Option | Value | Reason |
|---|---|---|
| FF_CODE_PAGE | 437 | Single-byte OEM code page |
| FF_USE_LFN | 1 | Static long-name workspace, no heap or large stack |
| FF_LFN_UNICODE | 2 | UTF-8 paths and directory names |
| FF_FS_TINY | 1 | One shared 512-byte sector buffer |
| FF_FS_NORTC | 1 | No wall clock in the RAM payload |
| FF_NORTC_YEAR | 2026 | Fixed 2026-01-01 file timestamps |

Formatting, exFAT, trim, threading and dynamic allocation remain disabled.
FatFs has its own permissive license; see [LICENSE.txt](LICENSE.txt).
