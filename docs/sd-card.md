# SPI SD card files

RamTool can read and write an existing FAT12/16/32 SD card without starting
the flash application. The new commands use the `sd` prefix; the original
`ls`, `put`, `add`, `rm`, and `rw` commands still address internal littlefs.
No card access happens until `sd init`. There is no formatting command.

## FlotSense wiring

Defaults were checked against
`cl_flotsense_minimal/include/cl_flotsense/env/boards/flotsense_board_v5.hpp`
on 2026-10-17 (the macros, rather than the older wiring comment).

| Signal | ESP32 GPIO |
|---|---|
| SCLK | 18 |
| MOSI / card DI | 23 |
| MISO / card DO | 19 |
| SD CS | 2 |
| Screen CS, held high | 12 |
| PT100 CS, held high | 32 |

The board's card supply is always powered. RamTool drives all three chip
selects high before clocking the bus, including disabled peripherals.
For an external socket, use 3.3 V signaling and power, common ground, a
stable supply, and a MISO pull-up. The driver also enables the ESP32's weak
internal MISO pull-up.

## Usage

```text
sd init
sd ls
sd cat /ICo.csv
sd rw on
sd put /TEST.TXT first line
sd add /TEST.TXT second line
sd cat /TEST.TXT
sd mkdir /logs
sd put /logs/notes.txt a note
sd ls /logs
sd rm /logs/notes.txt
sd rm /logs
sd rw off
sd off
```

`put` replaces or creates a file; `add` appends or creates it. Each adds a
newline and closes/synchronizes the file before reporting success. `rm`
also removes empty directories. `mkdir` creates one directory level.
Names are UTF-8 and long names are supported. Console paths cannot contain
whitespace, must fit in 127 bytes, and the entire command must fit in 199
bytes. As with existing commands, pace serial input and wait for the prompt.

`sd rw on` unlocks only the SD card. Internal flash's `rw on` does not unlock
it. Initialization, unmount, reboot, and transport failures lock SD writing.
Listing and reading leave card data unchanged. Writes check the physical
card capacity, including overflow-safe multi-sector bounds checks.

Custom wiring takes **six decimal pins**, in this order:

```text
sd init 18 23 19 5 255 255
```

The first four are SCLK/MOSI/MISO/SD-CS. The last two identify other slaves
whose CS must stay high; use 255 for an unused slave. All active pins must be
distinct. Supported pads are 2, 4, 5, 12–15, 18, 19, 21–23, 25–27, 32, 33.
UART, boot GPIO0, flash/PSRAM, nonexistent and input-only pads are excluded.
Strapping GPIOs must still have suitable reset-time wiring.

The override parser is compiled in by default. Build with
`.\build.ps1 -NoSdPinArgs` (equivalently `-DRAMTOOL_SD_PIN_ARGS=0`) to drop it
and the pad-validation checks; `sd init` then always uses the fixed defaults and
rejects arguments. This trims about 200 bytes. Either way, nothing touches the
SD pins until `sd init` runs.

## Implementation and limits

`sdgpio.c` implements mode-0 software SPI with a delay in each half cycle,
at no more than 250 kHz. It favors a small, freestanding implementation;
large transfers will be slow. It explicitly routes GPIO outputs and enables
the MISO input, including support for the high GPIO bank used by PT100 CS.

`sdspi.c` implements SD v1/v2 initialization, byte addressing for SDSC,
sector addressing for SDHC/SDXC, CSD capacity discovery, and single-sector
reads/writes. It checks read CRC16, sends write CRC16, checks the write response,
waits through programming busy, then checks CMD13 status. Polls and retries
are bounded; their elapsed duration depends on software SPI overhead.

FatFs R0.16 handles FAT12/16/32, MBR partitions and unpartitioned volumes.
There is one mount and one synchronous operation at a time, no heap, and no
filesystem formatting, exFAT, MMC, or SDUC support. SDXC requires an existing
FAT volume. New timestamps are fixed to 2026-01-01 because RamTool has no RTC.

A transport error invalidates the mount. Check wiring/power and run `sd init`
again. A card which remains stuck may need a power cycle. Do not remove the
card during an operation; wait for completion and use `sd off` first. FAT
writes are not atomic: an error, full disk, or lost power may leave a partial
file, and `put` truncates the old file before writing its replacement.

## Research references

- [ChaN's SPI SD protocol guide](https://elm-chan.org/docs/mmc/mmc_e.html):
  initialization, addressing, data tokens, CRC and programming busy.
- [FatFs porting guide](https://elm-chan.org/fsw/ff/doc/appnote.html):
  freestanding block-device integration and static memory options.
- [FatFs write contract](https://elm-chan.org/fsw/ff/doc/write.html):
  short writes must be checked even when the return code indicates success.
- [Espressif SD SPI host](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/sdspi_host.html):
  shared SPI buses and independent chip selects. RamTool uses its own driver.
- ESP-IDF `components/soc/esp32/include/soc/io_mux_reg.h` and
  `components/esp_rom/esp32/ld/esp32.rom.api.ld`: register offsets and ROM GPIO
  routing symbols, checked against the locally installed SDK.

## Verification

The release payload cross-compiles with warnings as errors and fits the
existing IRAM/DRAM assertions. `tests/sd_test.c` exercises the real driver and
FatFs over a simulated card at the SPI byte boundary. See
[tests/README.md](../tests/README.md) for instructions.

**The SD feature has also been verified on physical hardware.** On 2026-10-17,
against an ESP32 rev 3.1 and a card reporting 61,069,312 sectors, the default
FlotSense pins (SCK 18, MOSI 23, MISO 19, SD CS 2, screen CS 12, PT100 CS 32)
were used to mount the existing FAT volume and run the full write cycle:
`sd init`, `sd ls`, `sd rw on`, `sd put`/`sd cat`/`sd add`/`sd cat` on
`/RAMTEST.TXT`, `sd ls`, `sd rm`, `sd rw off`, `sd off`. Pre-existing card
files and directories were intact afterwards. The internal littlefs volume was
exercised with the same `rw on` … `rw off` cycle in the same session and its
pre-existing files were unchanged. This confirms the transport and file paths
on one card and board; it is not a substitute for checking a specific setup.

When bringing up a new socket or card, use a backed-up card, run the example
write/read/append/delete cycle, unmount, then check the files and filesystem
on a PC. Confirm that unrelated files and the internal flash contents remain
intact. Check bus behavior with the screen and PT100 connected, including after
a warm MCU reset.
