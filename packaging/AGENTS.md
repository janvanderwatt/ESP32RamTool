# RamTool — agent and scripting guide

RamTool runs a small command interpreter **in RAM** on a bare classic ESP32 and
talks to it over the UART the chip is already using. The payload is uploaded
through the ROM serial bootloader. Loading it, running it, and reading with it
write nothing to flash, and the flash-resident application is never started.

This folder is the whole tool: the payload, its load addresses, and the host
script that puts it in RAM. No toolchain and no chip-side install are needed.

## Ground rules

- **Classic ESP32 only** (Xtensa LX6, silicon revision 0 through 3). Not the
  ESP32-S2, -S3, -C2, -C3, or -P4.
- **RAM only.** The payload lives until the chip resets or loses power. Any
  reset — `reset`, `reboot`, EN pressed, a brownout — loses it; run `run.py`
  again to reload.
- **One owner per serial port.** A terminal, esptool, or a second `run.py`
  holding the port will conflict. Close the other side first.
- **Requirements:** Python 3.8+ and `pip install esptool pyserial`.
- The console is **115200 8N1**, with no flow control.

## Files here

| File | What it is |
|---|---|
| `iram.bin` | IRAM payload; loads at `0x40080000` |
| `dram.bin` | DRAM payload; loads at `0x3FFB0000` |
| `manifest.txt` | Load addresses and entry point, read by `run.py` |
| `run.py` | Host loader: uploads the payload, launches it, attaches the console |
| `README.txt` | The short version of this file |
| `AGENTS.md` | This file |

## Load it

```sh
python run.py --port COM7          # Windows
python run.py --port /dev/ttyUSB0  # Linux, macOS
```

`--port` is usually unnecessary: `run.py` uses `$RAMTOOL_PORT` if set, otherwise
the only serial port it can find, and asks when there is more than one.

- `--seconds N` — stay attached for N seconds, then exit.
- `--boot` — reset the chip into the download bootloader and stop there.
- Ctrl-C — detach. The payload keeps running either way.

`run.py` never resets the chip on the way out and never writes flash. It launches
the payload through the esptool **stub**, which is the launch mechanism: the
ROM loader records an entry address and then ignores it, so only the stub
performs the jump. Do not substitute `esptool write_flash` or another uploader;
those write flash and reset the chip when they finish.

## Drive it from a script

### The console protocol

- Line based. Send one command terminated by `\n`, then read until the next
  `> ` prompt. The prompt is the only end-of-reply marker; there are no status
  codes.
- The payload echoes what you send, so a reply is the echoed command, the
  output, and the next `> `.
- Decode output as latin-1 (command replies are ASCII in practice).
- There is no per-command time limit: `part`, `ls`, `sd`, and `cat` of a large
  file hold the console for as long as they take. Treat the prompt as
  completion and set a generous read timeout.
- **Pacing matters.** The console has no flow control and a 128-byte receive
  FIFO. While a command does flash or SD I/O it is not draining the FIFO, so
  never send the next command until the prompt for the previous one arrives.
  Blasting a batch of commands silently loses characters and produces mangled
  commands that look like a flaky link.

### Fast path: one bounded attached session

For a handful of short commands, pipe them in and bound the session:

```sh
printf 'info\npart\nls\n' | python run.py --port COM7 --seconds 5
```

Everything, including `run.py`'s own messages and the command echoes, goes to
stdout. Fine for a few short commands; anything multi-step is easier with an
explicit client.

### Recommended: load once, then own the port

Upload the payload once with `run.py`, then talk to it with pyserial. Loading is
a one-off cost; no command needs a reload, and the payload outlives the host.

```python
import time
import serial

PORT = "COM7"          # or /dev/ttyUSB0, or $RAMTOOL_PORT

def open_console(port):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    # Set both before open(). pyserial starts with DTR and RTS asserted, and on
    # the usual auto-reset wiring an asserted RTS pulls EN low, which resets the
    # chip and destroys the running payload.
    s.dtr = False
    s.rts = False
    s.open()
    return s

def read_until_prompt(s, timeout=15.0):
    buf = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        chunk = s.read(256)
        if chunk:
            buf += chunk
            if buf.endswith(b"> "):
                return bytes(buf).decode("latin-1")
    raise TimeoutError("no prompt from the payload")

def command(s, line, timeout=15.0):
    s.write((line + "\n").encode("ascii"))
    return read_until_prompt(s, timeout)

s = open_console(PORT)
read_until_prompt(s)                 # banner, chip info, first prompt
print(command(s, "info"))
print(command(s, "ls"))
s.dtr = False
s.rts = False
s.close()                            # the chip keeps running
```

A `cat` of a file whose contents include a literal `> ` can end the read early,
because the prompt is the only delimiter there is; prefer files whose content
you know when parsing `cat` output automatically.

## Commands

`help` (or `?`) prints this list in-session and is the runtime authority.

| Command | Aliases | Notes |
|---|---|---|
| `info` | — | MAC address and silicon revision; also printed at startup |
| `g <pin>` | `gpio`, `s` | read a GPIO; pins 0–31 |
| `g <pin> <0\|1>` | `gpio`, `s` | set a GPIO; any non-zero value is high; `g <pin> read` also reads |
| `blink <pin> [n] [ms]` | `b` | `n` defaults to 10, `ms` to 100; blocks for 2·n·ms |
| `r <addr>` | `read` | read a 32-bit register |
| `w <addr> <val>` | `write` | write a 32-bit register |
| `delay <ms>` | `d` | blocks for that long |
| `part` | `parts` | show the partition table |
| `ls [path]` | `dir` | list a littlefs directory; `ls @<offset>` uses an explicit flash offset |
| `cat <path>` | `type` | print a file |
| `put <path> <line>` | — | replace a file with one line |
| `add <path> <line>` | `append` | append one line |
| `rm <path>` | `del` | delete a file |
| `rw` | — | report the currently unlocked write range |
| `rw on` | — | unlock internal-flash writing; also `rw on @<offset> <size>` |
| `rw off` | — | lock writing |
| `sd init [pins]` | — | mount SPI SD; `sd` alone prints usage |
| `sd ls [path]` | — | list a FAT directory |
| `sd cat <path>` | — | print an SD file |
| `sd put <path> <line>` | — | replace an SD file with one line |
| `sd add <path> <line>` | — | append one line to an SD file |
| `sd mkdir <path>` | — | create one SD directory level |
| `sd rm <path>` | — | remove an SD file or empty directory |
| `sd rw [on\|off]` | — | SD write lock, separate from internal flash |
| `sd off` | — | unmount SD and lock writing |
| `reset` | — | unmount SD and reset; the chip then boots the flashed application |
| `reboot` | `x` | reset into the download bootloader |
| `help` | `?` | the command list |

Argument rules:

- Numbers are decimal, or hex with a `0x` prefix.
- Parsing is lenient: an argument with no digits is treated as 0. `g zzz` reads
  GPIO 0. `r` and `w` refuse 0 and anything not 4-byte aligned.
- `w` writes arbitrary memory-mapped registers, including peripherals and the
  flash controller, so it can reset or wedge the running chip.
- Paths cannot contain spaces or tabs; there is no quoting.
- littlefs paths are relative to the volume root and must not begin with `/` for
  `cat`, `put`, `add`, and `rm` (`cat config.csv`, not `cat /config.csv`).
  `ls` accepts `/` for the root. Names are limited to 63 bytes.
- SD paths may be absolute (`/LOGS/TODAY.TXT`) and are limited to 127 bytes.
- The whole command line is limited to 199 bytes; longer input is cut at the
  limit, so keep lines short and write long content across several `add` calls.
- `blink` and `delay` hold the console for their whole duration.
- Errors print as `<command>: <reason> (<number>)`; SD errors as
  `SD: <reason> (<number>)`; an unknown command prints `unknown. 'help'`.

## Writes, locks, and what is safe

Loading the payload, running it, and reading with it write nothing to flash.
Writing files does, and only after it is unlocked.

- **littlefs** (`put`, `add`, `rm`) needs `rw on`, which prints the exact byte
  range it unlocked. Every program and erase is checked against that range,
  because littlefs takes its idea of its own size from the on-disk superblock
  without validating it. The lock is in RAM: it does not survive a reset or
  reboot, and `rw off` locks again.
- **SD** writes need their own `sd rw on`. `sd init` mounts with writing locked;
  writing locks again on `sd off`, another `sd init`, an unmount, a reboot, or a
  transport error.
- `sd init` never formats. FAT12/16/32 only; exFAT, MMC, and SDUC are not
  supported. New files get the timestamp 2026-01-01, because there is no RTC.
- Writes are one line at a time and the command adds the trailing newline.
  There is no multi-line or binary write path.
- `put` truncates the existing file before writing its replacement. FAT writes
  are not atomic either, so an interrupted write can leave a partial file.
- Do not remove the SD card mid-command; wait for the prompt and `sd off` first.

## Tricks of the trade

- **Load once, command many times.** Reload only after a reset.
- **`reboot` (or `--boot`) leaves the chip in the download bootloader**, so a
  following esptool flash or `run.py` needs no button and no keypress.
- **`reset` hands the chip back to the flashed application.** That firmware may
  claim the UART, so type at it only if you mean to; run `run.py` again for
  RamTool.
- **Ask the tool instead of guessing.** `rw` reports the unlocked range, `part`
  the partition table, `info` the chip, and `ls @<offset>` finds a littlefs
  volume at a raw flash offset when the partition table does not describe it.
- **Partition labels and subtypes are not evidence of what is on them.** A
  partition labelled `spiffs` commonly holds a littlefs image; `ls` tries the
  candidates and mounts the one that works.
- **Bound automated sessions** with `--seconds` and a read timeout, so a wedged
  command cannot hold the serial port forever.
- **Set `$RAMTOOL_PORT`** to pin the device when a machine has several ports.

## Gotchas

- **Do not assert DTR/RTS when you attach.** An asserted RTS pulls `EN` low on
  the usual auto-reset wiring, resetting the chip and destroying the payload.
  Set both false before opening the port, as the client above does.
- **Typing or sending faster than the payload drains silently mangles
  commands**, and the result looks like a flaky link rather than an overrun.
- **The payload is gone after any reset.** There is no persistence and no
  session to resume; reload with `run.py`.
- **A second `run.py` resets the chip** on its way into the bootloader, which
  destroys the running payload. Expected, not a fault.
- **SD transport errors invalidate the mount.** Check wiring and power, then run
  `sd init` again; a card that stays stuck may need a power cycle.
- **GPIO 34–39 are input-only and unreachable here**: `g`, `blink`, and the pin
  arguments accept 0–31 only.
- **`sd` is slow** (software SPI at up to 250 kHz), so large files take a long
  time and a `cat` of one holds the console throughout.
- **Windows: a COM port cannot be passed into a Linux container.** Run `run.py`
  on the host, or bridge the port over TCP with `tools/serial_bridge.py` from
  the repository; DTR/RTS cannot cross a socket, so the bridge owns the reset.

## Licence

BSD-3-Clause. The payload vendors littlefs (BSD-3-Clause) and FatFs (ChaN's
permissive licence); see the RamTool repository for their notices.

## About this file

This file is copied verbatim from `packaging/AGENTS.md` in the RamTool
repository at build time, so it describes the payload in this folder. When a
command changes, this file, the payload's `help` text, and the repository
README's command table are updated together.
