# IRAM is for fetching, DRAM is for touching

**A byte load from the IRAM window raises `LoadStoreError` on this part, so every
string and constant your code dereferences at run time has to live in DRAM.**

## What happens

The payload sits at `0x40080000`. Code there fetches and executes perfectly.
Read a byte *out* of that same window and the CPU takes a load/store exception —
even though the bytes are demonstrably present, which you can confirm by reading
them back through the stub after upload.

The instruction and data ports address IRAM through different aliases. Code works;
data does not.

## What to do

Put code and literal pools in IRAM, and everything else in DRAM:

| Section | Region | Why |
|---|---|---|
| `.text`, `.literal`, `.xt.lit` | IRAM `0x40080000`, 64 KB | fetched as instructions |
| `.rodata`, `.data`, `.bss`, stack | DRAM `0x3FFB0000`, 28 KB | dereferenced at run time |

`.xt.lit` **must** stay in IRAM. It is part of the instruction stream that
`-mlongcalls` emits, not data, despite the name.

The host loads these as two separate `MEM_BEGIN` transfers, one per region.

## Why `.rodata` is in the DRAM section

It is counter-intuitive enough to be worth stating plainly: the natural place for
string literals is `.rodata`, and the natural place for `.rodata` is beside the
code. Here that is wrong. `ramtool.ld` deliberately puts `.rodata` in the DRAM
output section.

Anything you add that is dereferenced at run time has to end up in `.dram_img`. An
assembly section added by hand goes in `.dram_img`, not `.iram_img`.

## Four smaller traps in the same area

### Name your entry point something the ROM does not already claim

Older SDK-based builds imported `PROVIDE (_start = 0x40000704)` from
`esp32.rom.ld`, which could silently bind a payload entry to the ROM. The current
pinned table excludes `_start`; RamTool retains its distinct `ramtool_entry`
name, and the generated manifest records the actual build entry address.

### `l32r` resolves backwards only

The literal pool for a `l32r` has to appear **before** the instruction that
references it, in assembler source order. Placing the pool after the code — which
is what every other architecture's assemblers accept — produces a load of
whatever happens to be there.

### `MEM_DATA` length must be a multiple of 4

The ROM's `MEM_DATA` rejects a length that is not word-aligned. Pad on the host
side rather than trimming the payload.

### Xtensa flag and mnemonic spellings

- `-mlongcalls` is mandatory. Code at `0x40080000` calling ROM functions around
  `0x4000xxxx` is outside the range of a plain `CALL`.
- The ABI flag is `-mabi=windowed`. There is no `-mno-windowed`; guessing that
  spelling wastes a build.
- `callx` requires an explicit size suffix: `callx8`, not `callx`.

## Memory map

| Region | Address | Size | Contents |
|---|---|---|---|
| IRAM | `0x40080000` | 64 KB | code, `l32r` literal pools |
| DRAM | `0x3FFB0000` | 28 KB | `.rodata`, `.data`, `.bss`, stack |

Both windows are clear of everything else resident at run time:

- esptool's stub occupies `0x400BE000` (text), `0x3FFCC000` (bss) and
  `0x3FFDEBAC` (data). IRAM at `0x40080000` with 64 KB never reaches it, and the
  DRAM window stops well below `0x3FFCC000`.
- The ROM's own `.bss_btdm` starts at `0x3FFB8000`, so 28 KB from `0x3FFB0000`
  ends before it.

`ramtool.ld` `ASSERT`s both bounds, so an overflow is a link error rather than
silent corruption at run time.
