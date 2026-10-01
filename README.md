# RamTool

Run a small utility on a bare ESP32 **out of RAM**. Nothing is written to flash,
nothing is reflashed between edits, and the flash-resident application is never
started. You build a payload, push it into RAM, and talk to it over the UART the
chip is already using.

Target is a classic ESP32 (Xtensa LX6), revision 0 through 3.

```
D:\temp\projects\RamTool
├── build.ps1     cross-compile the payload
├── run.py        push it into RAM and start it
├── start.S       entry point: stack, bss, UART handover
├── payload.c     the utility itself (the part you will edit)
├── rom.h         ROM function and peripheral addresses
└── ramtool.ld    linker script: where code and data land
```

## Usage

```powershell
.\build.ps1                    # compile to build\iram.bin, build\dram.bin
python .\run.py -p COM7         # upload to RAM, start it, attach the console
python .\run.py -p COM7 --boot  # just restart the chip into the bootloader
```

Any `python` works. esptool lives in the ESP-IDF venv rather than in whatever
interpreter happens to be first on `PATH` (on this machine that is a PlatformIO
`penv`), so `run.py` tries a normal import first and then falls back to the
standard ESP-IDF install locations, printing which one it used. If it finds
nothing it lists every path it tried instead of failing with a bare
`ModuleNotFoundError`.

`run.py` stays attached. Type commands at the `>` prompt, or Ctrl-C to detach and
leave the payload running. `--seconds N` bounds the session instead of waiting
for you; stdin is forwarded either way. To get back to normal firmware, type
`reboot` (which lands in the bootloader, ready for another `run.py`) or just
power-cycle.

Commands once it is up:

```
info                 mac, silicon revision
g <pin>              read a gpio
g <pin> <0|1>        set a gpio   ('s <pin> <0|1>' does the same)
blink <pin> [n] [ms] blink a pin
r <addr>             read a 32-bit register
w <addr> <val>       write a 32-bit register
delay <ms>           delay
reboot               reset into the download bootloader
help                 command list
```

The current payload is around 1.9 KB of code and 0.8 KB of read-only data. Edit
`payload.c`, re-run `build.ps1` and `run.py`, and you have a new utility in a
couple of seconds.

## Verified on hardware

Tested against a rev 3.01 part on COM7. The two claims that matter, both checked
rather than assumed:

- **Flash is untouched.** SHA-256 of 16 KB from `0x1000` is byte-identical before
  and after a full build-and-launch cycle.
- **The payload outlives the host.** After `run.py` exits and the port is closed,
  a fresh raw serial connection still gets a reply to `g 2`. Nothing reset it.

`reboot` also does what it claims: it resets into
`boot:0x3 (DOWNLOAD_BOOT(UART0))`, and the chip can then be reconnected with
`connect("no_reset")`, so the next `run.py` needs no keypress.

## Why there is a host script at all

esptool's CLI resets the chip when it finishes. That is the default
(`--after hard_reset`) and it is right for flashing and wrong for this: the reset
undoes the whole point. `--after no_reset_stub` helps, but it still leaves the
flasher stub resident and still needs a second command to actually run the code.

`run.py` drives the esptool Python API instead, and never calls `hard_reset()` or
`soft_reset()`. It also deasserts DTR and RTS before closing the port, because on
the usual CP210x auto-reset wiring RTS low pulls EN low and would reset the part
behind your back.

## Two things that will bite you

Both of these were found by testing on real hardware and both are silent
failures if you do not know about them.

### The ROM loader does not call your entry point

The natural approach is to skip esptool's stub entirely: connect to the ROM,
then MEM_BEGIN / MEM_DATA / MEM_END with the entry address. That does not work.

MEM_END with a non-zero entry does **not** call the entry. The ROM's
`MemDwnLdStopReqMsgProc` only stores the address into the `user_code_start` word
at `0x3ffe0400` and returns into the command loop. On a normal boot, `main` reads
that word and `callx8`es it — but the download-mode path never reaches that code,
so the address is recorded and then ignored. The chip sits there answering
nothing, which looks exactly like a crash.

The esptool **stub** is what performs the call. `run_stub()` uploads a ~3.5 KB
loader to IRAM `0x400be000`, and the stub's own MEM_END handler jumps to the entry
you hand it. The stub is not optional here; it is the launch mechanism. It is
still a RAM upload, so nothing touches flash.

### IRAM and DRAM are different windows onto different memory

Reading a byte out of the IRAM window at `0x40080000` raises `LoadStoreError` on
this part, even though the bytes are demonstrably there (verified by reading them
back through the stub after upload). The instruction and data ports address IRAM
through different aliases.

The practical consequence: **code goes in IRAM, and anything you dereference at
run time goes in DRAM.** That includes strings. `ramtool.ld` puts `.rodata` in
the DRAM region for exactly this reason, and `run.py` sends the two regions as two
separate MEM_BEGIN transfers.

## Memory map

| Region | Address | Size | Contents |
|---|---|---|---|
| IRAM | `0x40080000` | 64 KB | code, `l32r` literal pools |
| DRAM | `0x3FFB0000` | 28 KB | `.rodata`, `.bss`, stack |

Both are clear of everything else that is resident at run time:

- The esptool stub sits at `0x400BE000` (text), `0x3FFCC000` (bss) and
  `0x3FFD EBAC` (data). The IRAM window above `0x40080000` with 64 KB of room
  never reaches it, and the DRAM window stops well below `0x3FFCC000`.
- The ROM's own `.bss_btdm` starts at `0x3FFB8000`, so the 28 KB DRAM window
  starting at `0x3FFB0000` ends before it.
- The linker script `ASSERT`s both bounds, so an overflow is a link error rather
  than a silent memory corruption at run time.

The entry point is `ramtool_entry`, deliberately not `_start`: ESP-IDF's
`esp32.rom.ld` does `PROVIDE (_start = 0x40000704)`, and colliding with that name
is a quiet way to end up with the wrong address.

## Startup

`start.S` does four things, in this order:

1. **Flushes the I-cache** (`Cache_Flush_rom`). The first fetch after this
   refills from memory, which is where the host just wrote the code.
2. **Installs its own stack** from the DRAM window. The ROM's stack is live above
   us and we do not know how deep it has already gone.
3. **Zeroes `.bss`.** It is `NOLOAD`, so it is not in the downloaded image and the
   ROM does not clear it for us.
4. **Takes UART0 away from the ROM.** This needs two steps and neither alone is
   enough. Masking the CPU interrupt level stops `uart_rx_intr_handler` being
   scheduled, but the UART's own interrupt-enable register is still set and the
   handler is still attached, so bytes still went missing. Clearing `INT_ENA` and
   `INT_CLR` at the peripheral is what actually makes the FIFO ours to poll. With
   only the level mask, the console dropped bytes; with both, it is solid.

It then calls `ramtool_main`, which never returns. That is deliberate: the entry
is reached via `callx8`, so there is a live return address, and returning would
drop us back into the ROM's normal flash-boot path.

## Things the payload deliberately does not do

**`waiti` at the console prompt.** It looks like the obvious way to idle while
waiting for input, and it is a trap. `waiti` parks the core until an interrupt
arrives, and startup deliberately cleared the UART interrupt enable. With no
interrupt source left, `waiti` never returns and the console dies the moment it
goes idle. `read_line` polls instead. The cost is a spinning core while you sit at
the prompt, which is the right trade for a tool with nothing else to do.

**ROM `uart_rx_one_char`.** It returns 0 on success and non-zero when the FIFO is
empty — the opposite of what the name suggests — and it reads through the ROM's
own `UartDev` descriptor rather than the hardware port. The payload polls the
status register's low byte (the receive-FIFO count) directly.

**First-character dispatch.** Matching on one letter silently mis-routes anything
that starts the same way. `s 2 1` (set a gpio) came out as an unknown command
while `bogus` was cheerfully treated as `blink`. `word_is()` matches whole words
and rejects prefixes.

**A reader thread running during `mem_finish`.** The stub does answer MEM_END, so
esptool reads a four-byte SLIP reply. A console reader started early races
esptool for those same bytes and drops characters from the banner without any
error. The launch happens first, then the reader attaches.

## Growing it

The payload is a resident command loop, which is the useful shape for this: the
chip is alive in RAM and you can poke it over the UART. To add a capability, add a
function in `payload.c` and a `word_is()` arm in the dispatch chain.

Two constraints to keep in mind when you do:

- Everything you dereference at run time has to be in the DRAM region. If you add
  a `.data` initialiser or a string, the linker script already places it there;
  if you add an assembly section by hand, put it in `.dram_img`, not
  `.iram_img`.
- `ramtool.ld` only carries the code and read-only sections into the downloaded
  images. Anything else has to be added to one of them or it will read as zeros.

Build flags worth knowing about, all in `build.ps1`:

- `-mlongcalls` is mandatory. The payload is at `0x40080000` and the ROM functions
  it calls are around `0x4000xxxx`, well outside the range of a plain `call`. The
  literal pools this forces live in `.xt.lit`, which the linker script keeps in
  IRAM.
- `-fno-builtin -ffreestanding` and `-nostdlib` because there is no C runtime.
  `memcpy`, `memset`, 64-bit division and the rest resolve to the copies in ROM
  via `esp32.rom.libgcc.ld`.

The ROM addresses in `rom.h` were read out of the ELF files that ship with
ESP-IDF (`C:\Espressif\tools\esp-rom-elfs\20241011\esp32_rev0_rom.elf` and
`esp32_rev300_rom.elf`) and cross-checked against
`components/esp_rom/esp32/ld/esp32.rom.ld`. Both revisions agree on every address
used here, so one table covers the family. To verify for yourself:

```powershell
& 'C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin\xtensa-esp32-elf-nm.exe' `
    'C:\Espressif\tools\esp-rom-elfs\20241011\esp32_rev0_rom.elf' | Select-String 'ets_printf|uart_tx_one_char'
```