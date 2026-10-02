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
> ls
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

See [Building on a clean machine](#building-on-a-clean-machine) if you would rather
not install an ESP-IDF toolchain on the host.

## Commands

```
info                 mac, silicon revision
g <pin>              read a gpio
g <pin> <0|1>        set a gpio
blink <pin> [n] [ms] blink a pin
r <addr>             read a 32-bit register
w <addr> <val>       write a 32-bit register
delay <ms>           delay

part                 show the partition table
ls [path]            list a littlefs directory
cat <path>           print a file
put <path> <line>    replace a file with one line
add <path> <line>    append one line to a file
rm <path>            delete a file
rw [on|off]          unlock writing; put/add/rm need this

reboot               reset into the download bootloader
help                 this list
```

Paths cannot contain spaces — the console splits on whitespace and there is no
quoting.

## What it writes, and when

**Loading and running write nothing to flash.** Neither does reading.

Writing files does, and only after you say `rw on`, which prints the exact byte
range it has unlocked and does not survive a reboot. Every program and erase is
additionally checked against that range, because littlefs takes its idea of its
own size from the superblock without validating it — see
[safe-flash-writes.md](docs/safe-flash-writes.md).

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

Tested against a silicon revision 3 part. The claims that matter, all checked
rather than assumed:

- **Loading and running write nothing.** A hash over the partition table and the
  head of the littlefs volume is byte-identical before and after a session that
  mounts the filesystem, lists it, reads files, and has a write attempt refused.
- **Writing works, and stays inside the volume.** With `rw on`, the full cycle was
  run against a real image: `put` created a file, `cat` read it back, `add`
  appended and `cat` showed both lines, `ls` tracked the size at each step, `rm`
  removed it. Pre-existing files were intact afterwards, with original sizes and
  contents.
- **The payload outlives the host.** After `run.py` exits and the port is closed, a
  fresh serial connection still gets a reply. Nothing reset it.
- **`reboot` lands where it claims**, in the serial bootloader, so the next run
  needs no keypress.

## Memory footprint

| | IRAM | DRAM image | `.bss` | stack | total RAM |
|---|---|---|---|---|---|
| base payload, no littlefs | 1,868 | 764 | 72 | 2,048 | 4,752 |
| read-only (`part`, `ls`, `cat`) | 17,988 | 1,744 | 592 | 2,048 | 22,372 |
| with writing (`put`, `add`, `rm`) | 19,728 | 2,348 | 992 | 2,048 | **25,116** |

25 KB of the 94 KB the two windows offer: 30% of IRAM, 12% of DRAM. littlefs is
nearly all code, and `--gc-sections` is what makes it affordable — without it the
same link is around 26 KB before any file handling is added.

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

## Repository layout

```
build.ps1       cross-compile the payload
run.py          push it into RAM and start it
Dockerfile      reproducible build on a clean machine
docker-compose.yml  wrapper for building, and for driving hardware
start.S         entry point: cache flush, stack, bss, UART handover
payload.c       the command interpreter (the part you will edit)
lfsvol.c/.h     littlefs over SPI flash: the file and partition commands
littlefs/       vendored littlefs, unmodified (see ORIGIN.md)
rom.h           ROM function and peripheral addresses
ramtool.ld      linker script: where code and data land
tools/serial_bridge.py  reach a host serial port from a container
docs/           the notes linked above
LICENSE         BSD-3-Clause
```

## Building on a clean machine

The build needs an Xtensa toolchain and ESP-IDF's ROM linker scripts. Rather than
hardcode anyone's install path, `build.ps1` finds them:

1. `-ToolchainPath` / `-RomLdPath` arguments, if given;
2. `$env:IDF_PATH` for the ROM scripts, and the toolchain under `$env:IDF_TOOLS_PATH`;
3. `xtensa-esp32-elf-gcc` on `PATH`.

```sh
export IDF_PATH=/path/to/esp-idf
./build.ps1
```

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
build script runs unmodified. `docker-compose.yml` wraps the two things people
actually want:

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

## Notes on style

Deliberate choices that look odd and are not:

- **The entry point is `ramtool_entry`, not `_start`.** `esp32.rom.ld` does
  `PROVIDE (_start = 0x40000704)`; colliding with that name silently yields the
  wrong address.
- **Commands dispatch on whole words.** A first-character switch quietly mis-routes
  anything sharing a first letter.
- **The console polls; it never uses `waiti`.**
- **littlefs is vendored unmodified.** All integration lives outside it.

## Licence

BSD-3-Clause, matching the vendored littlefs. See [LICENSE](LICENSE).

littlefs itself is BSD-3-Clause, copyright the littlefs authors and Arm Limited;
its notice is in [`littlefs/LICENSE.md`](littlefs/LICENSE.md) and its provenance in
[`littlefs/ORIGIN.md`](littlefs/ORIGIN.md).