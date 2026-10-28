# RamTool

Run a small utility on a bare ESP32 **in RAM only**. No reflashing between edits,
and the flash-resident application is never started. You build a payload, push it
into RAM, and talk to it over the UART the chip is already using.

```
> part
partitions:
  nvs  @0x00009000  0x00005000  data sub 0x02
  app0  @0x00010000  0x00330000  app  sub 0x10
  spiffs  @0x00670000  0x00180000  data sub 0x82
> lfs ls
  [f] config.csv           69
  [f] pins.csv             153
```

Target: classic ESP32 (Xtensa LX6), silicon revision 0 through 3.

## Why

Almost every embedded debugging session ends with the same loop: edit, rebuild,
flash, reboot, reconnect, repeat. Flashing is slow, it is destructive to whatever
was on the chip, and it means you cannot work on a board whose firmware you care
about.

RamTool inverts that. The host uploads a payload into RAM through the serial
bootloader, jumps to it, and leaves it running. The chip stays alive in RAM while
you type at it. Nothing reflashes, nothing is overwritten, and the application on
flash is untouched.

It is also a genuinely pleasant way to poke at a chip: registers, GPIOs, the
partition table, and files on an internal filesystem, from one prompt.

## Quick start

```powershell
.\build.ps1                    # compile to build\iram.bin and build\dram.bin
python .\run.py --port COM7    # upload to RAM, start it, attach the console
python .\run.py --boot         # just restart the chip into the bootloader
```

`--port` is usually unnecessary. `run.py` uses `$RAMTOOL_PORT` if set, otherwise
the only serial port it can find, and asks if there is more than one. On Linux
that looks like `/dev/ttyUSB0`; check with `esptool.py --list-ports`.

`run.py` stays attached: type commands at the `>` prompt, or Ctrl-C to detach and
leave the payload running. `--seconds N` bounds the session instead.

No toolchain on this machine? [Build in a container](#in-a-container), or take the
standalone `dist/` folder the build produces — see
[scripting and the distribution](#scripting-and-the-distribution).

## Commands

`help`, or `?`, prints the same list on the chip, and the payload is the authority
on what it accepts.

| Command | Aliases | What it does |
|---|---|---|
| `info` | | MAC address and silicon revision; also printed at startup |
| `g <pin>` | `gpio`, `s` | read a GPIO (pins 0–31) |
| `g <pin> <0\|1>` | `gpio`, `s` | drive a GPIO high or low; any non-zero value is high |
| `blink <pin> [n] [ms]` | `b` | blink a pin; defaults are 10 blinks at 100 ms |
| `r <addr>` | `read` | read a 32-bit register |
| `w <addr> <val>` | `write` | write a 32-bit register |
| `delay <ms>` | `d` | sleep |
| `part` | `parts` | show the partition table |
| `lfs ls [path]` | | list a littlefs directory; `lfs ls @<offset>` for an explicit flash base |
| `lfs cat <path>` | | print a file |
| `lfs put <path> <line>` | | replace a file with one line |
| `lfs add <path> <line>` | | append one line |
| `lfs rm <path>` | | delete a file |
| `lfs rw` | | report the unlocked write range |
| `lfs rw on` | | unlock internal-flash writing; `lfs rw on @<offset> <size>` for an explicit region |
| `lfs rw off` | | lock writing |
| `lfs format` | | format the unlocked littlefs volume; `lfs rw on` first |
| `sd init [pins]` | | mount the SPI SD card; `sd` alone prints usage |
| `sd ls [path]` | | list a FAT directory |
| `sd cat <path>` | | print an SD file |
| `sd put <path> <line>` | | replace an SD file with one line |
| `sd add <path> <line>` | | append one line to an SD file |
| `sd mkdir <path>` | | create one SD directory level |
| `sd rm <path>` | | remove an SD file or empty directory |
| `sd rw [on\|off]` | | the SD write lock, separate from internal flash |
| `sd off` | | unmount the card and lock writing |
| `reset` | | unmount SD and reset; the chip then boots the flashed application |
| `reboot` | `x` | reset into the download bootloader |
| `help` | `?` | the command list |

Argument rules that apply everywhere:

- Numbers are decimal, or hex with a `0x` prefix. Parsing is lenient: an argument
  with no digits is 0, so `g zzz` reads GPIO 0. `r` and `w` refuse 0 and any
  address that is not 4-byte aligned.
- Paths cannot contain spaces — the console splits on whitespace and there is no
  quoting. The whole command line is limited to 199 bytes.
- littlefs paths are relative to the volume root and must not begin with `/`:
  `lfs cat config.csv`, not `lfs cat /config.csv`. `lfs ls` accepts `/` for the
  root. Names are limited to 63 bytes.
- SD paths may be absolute (`/LOGS/TODAY.TXT`) and are limited to 127 bytes.
- `blink` and `delay` hold the console for their whole duration.
- Errors print as `<command>: <reason> (<number>)`, and SD errors as
  `SD: <reason> (<number>)`.

## Writes and safety

**Loading the payload, running it, and reading with it write nothing to flash.**
`part`, `lfs ls`, and `lfs cat` are free. Writing files is the only thing that
changes content, and it is gated.

littlefs (`lfs put`, `lfs add`, `lfs rm`, `lfs format`) needs `lfs rw on`, which
prints the exact byte range it unlocked and does not survive a reboot. Every
program and erase is checked against that range, because littlefs takes its idea
of its own size from the superblock without validating it — see
[safe-flash-writes.md](docs/safe-flash-writes.md).

`lfs format` is a logical format: it writes a fresh superblock and root directory
and bumps the revision, so the old tree disappears from mount, but its blocks are
not wiped. It needs the unlock and leaves it in place. A volume too damaged to
mount can be unlocked explicitly with `lfs rw on @<offset> <size>` (both values
from `part`); that path assumes 4096-byte blocks, which is the geometry format
writes.

The SD card has its own lock. `sd init` mounts with writing locked, and `sd rw on`
unlocks it independently of internal flash; `sd off`, another `sd init`, an
unmount, a reboot, and any transport error all lock it again. Mounting never
formats.

Writes are one line at a time and the command adds the trailing newline; there is
no multi-line or binary write path. `lfs put` truncates the old file before
writing its replacement, and FAT writes are not atomic either, so an interrupted
write can leave a partial file. New SD files get the fixed timestamp 2026-01-01,
because there is no RTC.

## SPI SD cards

Run `sd init`, then `sd rw on` to enable SD file writes. The defaults match
FlotSense: SCLK 18, MOSI 23, MISO 19, SD CS 2; screen CS 12 and PT100 CS 32 are
held high on the shared bus.

```text
sd init
sd rw on
sd put /TEST.TXT hello from RamTool
sd cat /TEST.TXT
sd rw off
sd off
```

Supports existing FAT12/16/32 volumes, long UTF-8 filenames, and SDSC/SDHC/SDXC
over software SPI. exFAT is unsupported. Mounting never formats the card; SD
access starts only at `sd init`. Custom wiring takes six decimal pins, in the
order SCLK/MOSI/MISO/SD-CS/idle-CS1/idle-CS2, with `255` for an unused chip
select: `sd init 18 23 19 5 255 255`. See [SD card support](docs/sd-card.md) for
custom pins, limitations, research references, and hardware checks. The SD
feature passes simulated-card tests and has been verified on physical hardware;
see [Verified on hardware](#verified-on-hardware).

## Scripting and the distribution

`run.py` is an interactive console, not a command runner, so the simplest
automation is a bounded session:

```sh
printf 'info\npart\nlfs ls\n' | python run.py --port COM7 --seconds 5
```

Send one command at a time and wait for the `> ` prompt. The console has no flow
control and a 128-byte FIFO, and a command doing flash or SD I/O is not draining
it, so a blasted batch loses characters silently — which looks like a flaky link
rather than an overrun. Attaching with a serial client of your own needs DTR and
RTS deasserted before the port opens, or an auto-reset board resets and the
payload is gone. Both traps are written up in
[serial-from-container.md](docs/serial-from-container.md).

Every build also produces a standalone `dist/` folder: the two payload images,
`manifest.txt`, `run.py`, and an `AGENTS.md` written for agents and scripts. That
file carries the complete command reference, a ready-made load-once-then-own-the-
port client, and the traps. It is copied from `packaging/AGENTS.md` at build time,
so it stays in step with the payload; copy the folder anywhere and run it, with no
toolchain at all.

## How it works

- **The esptool stub is the launcher.** The ROM's own loader records the entry
  address and then ignores it; only the stub performs the call. This is the single
  most important thing to know about the whole approach.
- **Two memory windows.** Code and `l32r` literal pools go in IRAM at
  `0x40080000`. Everything dereferenced at run time — including strings — goes in
  DRAM at `0x3FFB0000`, because reading a byte out of the IRAM window raises
  `LoadStoreError` on this part.
- **No runtime.** No FreeRTOS, no ESP-IDF, no C library. `memcpy` and friends are
  pointed at the ROM's copies, so they cost zero bytes of payload.
- **littlefs, read-only by default.** Vendored unmodified; the integration is a
  block device over the ROM's SPI flash functions, a CRC, and some buffers.

Both windows have `ASSERT`s in the linker script, so an overflow is a link error
rather than silent corruption at run time.

## Verified on hardware

The internal-flash and SPI SD paths were both exercised against a silicon
revision 3 part. The claims that matter, all checked rather than assumed:

- **Loading and running write nothing.** A hash over the partition table and the
  head of the littlefs volume is byte-identical before and after a session that
  mounts the filesystem, lists it, reads files, and has a write attempt refused.
- **Writing works, and stays inside the volume.** With `lfs rw on`, the full
  cycle was run against a real image: `lfs put` created a file, `lfs cat` read it
  back, `lfs add` appended and `lfs cat` showed both lines, `lfs ls` tracked the
  size at each step, and `lfs rm` removed it. Pre-existing files were intact
  afterwards, with original sizes and contents.
- **The SPI SD card reads and writes on real hardware.** Against an ESP32 rev 3.1
  and a card reporting 61,069,312 sectors, `sd init` mounted the existing FAT
  volume, `sd ls` listed its files and directories, and with `sd rw on` the cycle
  was run end to end: `sd put` created `/RAMTEST.TXT`, `sd cat` read it back,
  `sd add` appended and `sd cat` showed both lines, `sd ls` tracked the size, and
  `sd rm` removed it. Pre-existing card files and directories were intact
  afterwards, and the default FlotSense pins (SCK 18, MOSI 23, MISO 19, SD CS 2,
  screen CS 12, PT100 CS 32) were used unchanged.
- **The payload outlives the host.** After `run.py` exits and the port is closed, a
  fresh serial connection still gets a reply. Nothing reset it.
- **`reboot` lands where it claims**, in the serial bootloader, so the next run
  needs no keypress.

## Memory footprint

| | IRAM | DRAM image | `.bss` | stack | total RAM |
|---|---|---|---|---|---|
| base payload, no littlefs | 1,868 | 764 | 72 | 2,048 | 4,752 |
| read-only (`part`, `lfs ls`, `lfs cat`) | 17,988 | 1,744 | 592 | 2,048 | 22,372 |
| with writing (`lfs put`, `lfs add`, `lfs rm`) | 19,728 | 2,348 | 992 | 2,048 | **25,116** |
| with SPI SD / FatFs (current release) | 32,444 | 5,040 | 2,600 | 2,048 | **42,132** |

The current release uses 41.1 KiB of the 92 KiB the two windows offer.
The figures above include the reserved stack; linker assertions enforce both
memory windows. Filesystem objects and working buffers are static, with no heap.

## All the gotchas discovered

Every one of these was found by testing on real hardware, and every one presents as
something other than what it is. Each has its own write-up.

**[The ROM loader never calls your entry point](docs/launching-from-ram.md).**
`MEM_END` writes the address down and returns to the command loop. The esptool
stub is not a convenience; it is the only thing that jumps to your code.

**[IRAM is for fetching, DRAM is for touching](docs/memory-windows.md).** A byte
load from the IRAM window raises `LoadStoreError`, so every string literal you
dereference has to live in DRAM — including `.rodata`.

**[Taking the UART away from the ROM needs two moves](docs/uart-handover.md).**
Clear the peripheral's interrupt enable *and* mask the ISR level. Either alone
still loses bytes.

**[`uart_rx_one_char` returns 0 on success](docs/uart-handover.md).** And it reads
through the ROM's own descriptor rather than the hardware FIFO.

**[`waiti` is a trap at an idle prompt](docs/uart-handover.md).** With the UART
interrupt disabled, it parks the core forever and the console dies at the first
prompt.

**[`lfs_crc` is not `esp_rom_crc32_le`](docs/littlefs-crc.md).** Same polynomial,
opposite inversion convention. Every metadata checksum fails and littlefs reports
only `Corrupted dir pair`.

**[`lfs_dir_read` returns positive on success](docs/littlefs-api-traps.md).** Not
zero. Both obvious loop idioms are wrong, and one of them treats a successful read
as a failure.

**[littlefs buffers are `cache_size` bytes, not `block_size`](docs/littlefs-api-traps.md).**
The obvious reading costs 8 KB of DRAM. And `cache_size = 0` is not "no cache",
it is a hang.

**[`lfs_file_open` does not exist under `LFS_NO_MALLOC`](docs/littlefs-api-traps.md).**
A link error, not a runtime failure. Use `lfs_file_opencfg` with your own buffer.

**[littlefs tags are big-endian and XOR-chained](docs/littlefs-disk-format.md).**
Decode them as little-endian words and you get plausible, confident nonsense that
sends you debugging the wrong layer.

**[Partition labels and subtypes are not evidence](docs/partition-tables.md).**
PlatformIO writes a littlefs image into the partition its table labels `spiffs`,
and the two formats are not readable by each other.

**[The ROM `PROVIDE`s less than you would expect](docs/rom-symbols.md).**
`SPIRead`, `SPIWrite`, `SPIEraseArea` and the entire libc string family are all in
there, and none of them is exported.

**[`esp_rom_spiflash_read_user_cmd` is not a block read](docs/flash-access.md).** It
takes `(status *, cmd)` and *hangs* rather than faulting when given block
arguments.

**[littlefs's idea of its own size comes off the disk, unchecked](docs/safe-flash-writes.md).**
Which is why flash writes need a clamp and not merely a lock.

**[The console has no flow control and the FIFO is 128 bytes](docs/serial-from-container.md).**
Type faster than the payload can drain it and characters are lost silently, which
looks like a flaky link rather than an overrun.

**[`--device` cannot pass a COM port into a Linux container](docs/serial-from-container.md).**
Three spellings, three different errors. Bridge it over TCP instead, and mind what
the reset lines cost you.

## Building on a clean machine

The build needs an Xtensa ESP32 toolchain. ROM addresses are pinned in
[`rom/esp32.ld`](rom/esp32.ld), so an ESP-IDF checkout and ROM ELF extraction
are unnecessary. `build.ps1` finds the toolchain in this order:

1. `-ToolchainPath`, if given;
2. the toolchain under `$env:IDF_TOOLS_PATH`;
3. `xtensa-esp32-elf-gcc` on `PATH`.

```powershell
.\build.ps1 -ToolchainPath C:\path\to\xtensa-esp-elf\bin
```

`.\build.ps1 -NoSdPinArgs` compiles out the `sd init` pin-override parser and its
pad checks, trimming about 200 bytes; `sd init` then always uses the fixed
18/23/19/2/12/32 defaults and rejects arguments. The default build keeps the
override.

`-RomLdPath` is no longer needed or accepted. The checked-in table covers classic
ESP32 revisions 0 through 3; see [ROM provenance and verification](rom/README.md).
Recheck addresses only when maintaining that table or adding chip support.
The payload's entry/load addresses are still extracted from each build's ELF
into `build/manifest.txt`. The same build also writes `dist/`, the standalone
distribution described under
[scripting and the distribution](#scripting-and-the-distribution).

If esptool is not installed in the active interpreter, `run.py` falls back to an
ESP-IDF checkout's own environment; otherwise `pip install esptool` is enough.

### In a container

`Dockerfile` builds the payload with **the same `build.ps1` the host uses**, so a
green container build is real evidence the repository has no machine-specific
dependencies left in it — not a parallel build script that could quietly drift.

```sh
docker build -t ramtool .
docker run --rm -v "$PWD/build:/src/build" ramtool ls -l /src/build
```

It pins ESP-IDF v5.5.4 (xtensa-esp-elf 14.2.0) and installs PowerShell so the
build script runs unmodified. The runtime stage carries the build's `dist/` folder
and the notes, so a container can hand out the standalone distribution as well as
drive hardware. `docker-compose.yml` wraps the two things people actually want:

```sh
docker compose build                    # build the payload in the container
docker compose run --rm tools           # list the artifacts it produced
docker compose --profile flash run --rm flash   # drive real hardware
```

Driving hardware from the container needs the serial device passed through, which
is reliable on a Linux Docker host and unreliable under Docker Desktop on Windows
and macOS. There, build in the container and run `run.py` on the host - the
artifacts land in `./build` either way, so nothing is lost.

**On Windows, `--device` cannot reach a COM port at all.** Bridge it over TCP
instead. `tools/serial_bridge.py` does this and needs no changes to `run.py`,
because esptool passes its port string to pyserial and pyserial understands
`socket://` URLs:

```sh
# host
python tools/serial_bridge.py --port COM7 --listen 5000

# container
docker run --rm --add-host host.docker.internal:host-gateway -it \
    ramtool python3 run.py --port socket://host.docker.internal:5000
```

Verified end to end, including the file write cycle. Two costs are worth reading
before relying on it: DTR/RTS cannot cross a socket, so the bridge owns the chip
reset; and the reset truth table is board-specific, so there is a `--reset tight`
option. Both, plus the WSL `usbipd-win` alternative, are in
[serial-from-container.md](docs/serial-from-container.md).

The container builds and verifies the artifacts; it does **not** pretend to test
against hardware. Runtime behaviour is the part only a real chip can confirm, and
that is what the [Verified on hardware](#verified-on-hardware) section is for.

## Repository layout

```
build.ps1       cross-compile the payload; also writes dist/
run.py          push it into RAM and start it
Dockerfile      reproducible build on a clean machine
docker-compose.yml  wrapper for building, and for driving hardware
packaging/AGENTS.md  instructions for agents, copied to dist/AGENTS.md
start.S         entry point: cache flush, stack, bss, UART handover
payload.c       the command interpreter (the part you will edit)
lfsvol.c/.h     littlefs over SPI flash: the file and partition commands
littlefs/       vendored littlefs, unmodified (see ORIGIN.md)
sdgpio.c        software SPI and shared-bus GPIO setup
sdspi.c/.h      SD card protocol and FatFs block-device adapter
sdvol.c/.h      FAT file commands and independent write lock
fatfs/          vendored FatFs R0.16 (see ORIGIN.md)
tests/          simulated SD protocol and FAT regression tests
rom.h           ROM function declarations and peripheral addresses
rom/esp32.ld    pinned ROM ABI, independent of installed SDKs
ramtool.ld      linker script: where code and data land
tools/serial_bridge.py  reach a host serial port from a container
tools/verify_rom_symbols.py  optional ROM address maintenance audit
docs/           the notes linked above
dist/           the standalone distribution, produced by build.ps1 (not committed)
LICENSE         BSD-3-Clause
```

## Notes on style

Deliberate choices that look odd and are not:

- **The entry point is `ramtool_entry`, not `_start`.** Older SDK-based builds
  imported a ROM `_start` symbol at `0x40000704`. The pinned table deliberately
  excludes that symbol; the distinct payload entry name is retained.
- **Commands dispatch on whole words.** A first-character switch quietly mis-routes
  anything sharing a first letter.
- **The console polls; it never uses `waiti`.**
- **littlefs is vendored unmodified.** All integration lives outside it.

## Licence

BSD-3-Clause, matching the vendored littlefs. See [LICENSE](LICENSE).

littlefs itself is BSD-3-Clause, copyright the littlefs authors and Arm Limited;
its notice is in [`littlefs/LICENSE.md`](littlefs/LICENSE.md) and its provenance in
[`littlefs/ORIGIN.md`](littlefs/ORIGIN.md).

FatFs is copyright ChaN and uses its own permissive license; see
[`fatfs/LICENSE.txt`](fatfs/LICENSE.txt) and [`fatfs/ORIGIN.md`](fatfs/ORIGIN.md).
