# Pinned classic ESP32 ROM ABI

`esp32.ld` is RamTool's complete linker table for the ROM functions it uses,
including libc routines, startup calls and compiler helpers. It is checked in
and linked directly. Normal builds require no SDK linker scripts, ROM ELFs,
network access, or address extraction from the chip.

Target: **classic ESP32 (Xtensa LX6), silicon revisions 0 through 3**. This is
not a table for ESP32-S2/S3/C-series parts. The revision 0 and revision 3 ROM
ELFs agree on all 31 entries; the project's existing revision 0–3 target is
retained. This is symbol-table verification, not a new hardware test of each
silicon revision or ROM routine.

## Provenance

Verified on 2026-10-17 against the locally installed Espressif
`esp-rom-elfs/20241011` package:

| File | SHA-256 |
|---|---|
| `esp32_rev0_rom.elf` | `bef39889393de40d7ae8dcf1cfe0bba32435a96a0b6ec35839724a9fddd6049f` |
| `esp32_rev300_rom.elf` | `920b70635440517866aab2230964a570d2cf2b676658d93c52fbac108c1cca31` |

Alias names were cross-checked against the installed ESP-IDF package
`5.3.2.250228`, specifically `components/esp_rom/esp32/ld/esp32.rom.api.ld`,
`esp32.rom.ld` and `esp32.rom.libgcc.ld`. Addresses are stored directly rather
than importing those large tables. Each `rom:` comment identifies the native
ROM symbol used to verify that entry. Preserve these comments.

The table includes the existing libc bindings and declarations in `rom.h`.
The compiler helpers currently required by Release and Debug builds are
`__ashldi3`, `__bswapsi2`, and `__popcountsi2`. An unknown helper or new ROM call
fails at link time; maintainers should verify it before extending this table.
No fallback silently imports symbols from an installed SDK.

## Optional verification

Only run this when changing the table, supporting a new revision, or auditing
the ABI. Python's standard library is sufficient; `nm` and esptool are not
required. Supply explicit file paths to avoid wildcard/tool installation issues:

```powershell
python tools/verify_rom_symbols.py `
  --rev0 C:\path\to\esp32_rev0_rom.elf `
  --rev3 C:\path\to\esp32_rev300_rom.elf `
  --payload build/ramtool.elf
```

Both ROM files must contain every native symbol at the pinned address. Missing
or mismatched symbols fail with a nonzero exit code. The optional `--payload`
also checks every pinned binding present in the linked payload.

For a new symbol, confirm its function signature/calling convention as well as
its address, verify both ROM revisions, add the literal binding and annotation,
then rebuild Release and Debug. These are maintenance steps, not prerequisites
for another agent to compile or run RamTool.

## What remains generated

`build.ps1` still extracts `ramtool_entry` and the two payload load origins from
the newly linked ELF into `build/manifest.txt`. Those describe the built RAM
payload and must remain tied to its binaries. They are separate from the fixed
chip ROM ABI. `start.S` refers to the pinned linker symbols for cache flush and
interrupt masking, rather than duplicating their numeric addresses.
