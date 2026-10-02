#!/usr/bin/env python
"""Download the RamTool payload into RAM on a bare ESP32 and start it.

The whole point is that nothing is ever written to flash. The chip is put into
the ROM serial bootloader (download mode), the payload is copied into IRAM and
DRAM with MEM_BEGIN/MEM_DATA, and the entry point is launched. From then on the
payload owns the chip until it is reset.

Why the esptool stub has to be in the loop
------------------------------------------
The obvious approach is to talk to the ROM loader directly: connect, then
MEM_BEGIN / MEM_DATA / MEM_END with our entry address. That does not work, and
the reason is worth recording.

MEM_END with a non-zero entry does *not* call the entry. The ROM's
MemDwnLdStopReqMsgProc only stores the address into the `user_code_start` word at
0x3ffe0400 and returns into the command loop. On a normal boot, `main` reads that
word and `callx8`es it. But the download-mode path never reaches that code: it
loops back into UartDwnLdProc's message receive, so the entry is recorded and
then ignored. The chip sits there answering nothing, which looks exactly like a
crash.

The esptool *stub* is what performs the call. `run_stub()` uploads a ~3.5 KB
loader to IRAM 0x400be000, and the stub's own MEM_END handler jumps to the entry
we hand it. So the stub is not optional here; it is the launch mechanism.

That costs IRAM at 0x400be000, which is why the payload is linked at 0x40080000
instead: the two never overlap.

Why this script exists rather than a one-line esptool invocation
---------------------------------------------------------------
esptool's CLI resets the chip when it finishes. That is the default
(`--after hard_reset`) and it is correct for flashing and wrong for this. The
usual workaround, `--after no_reset_stub`, still leaves you needing a second
command to actually run the code.

So this drives the esptool Python API directly and:

  * never calls hard_reset() or soft_reset()
  * deasserts DTR and RTS before closing the port, so the CP210x auto-reset
    circuit does not pull EN low behind our back and undo the whole thing
"""

from __future__ import annotations

import argparse
import glob
import os
import struct
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "build")

# The ROM's MEM_DATA block granularity (esptool's ESP32ROM.ESP_RAM_BLOCK).
ROM_BLOCK = 0x1800


# --------------------------------------------------------------------------
# minimal ELF32 symbol reader, so the host needs no toolchain at run time
# --------------------------------------------------------------------------

SHT_SYMTAB = 2


def elf_symbols(path: str) -> dict[str, int]:
    """Return {name: value} for a 32-bit little-endian ELF."""
    with open(path, "rb") as fh:
        blob = fh.read()

    if blob[:4] != b"\x7fELF" or blob[4] != 1 or blob[5] != 1:
        raise ValueError(f"{path}: not a 32-bit little-endian ELF")

    (e_shoff,) = struct.unpack_from("<I", blob, 0x20)
    e_shentsize, e_shnum, _e_shstrndx = struct.unpack_from("<HHH", blob, 0x2E)

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name, stype, _flags, _addr, offset, size, link, _info, _align, entsize = (
            struct.unpack_from("<10I", blob, off)
        )
        sections.append(
            dict(type=stype, offset=offset, size=size, link=link, entsize=entsize)
        )
        del name

    out: dict[str, int] = {}
    for sec in sections:
        if sec["type"] != SHT_SYMTAB:
            continue
        strsec = sections[sec["link"]]
        symstr = blob[strsec["offset"] : strsec["offset"] + strsec["size"]]
        for i in range(sec["size"] // sec["entsize"]):
            off = sec["offset"] + i * sec["entsize"]
            st_name, st_value = struct.unpack_from("<II", blob, off)
            if st_name == 0:
                continue
            end = symstr.index(b"\0", st_name)
            out[symstr[st_name:end].decode()] = st_value
    return out


# --------------------------------------------------------------------------
# image loading
# --------------------------------------------------------------------------


def round_up(n: int, align: int) -> int:
    return (n + align - 1) // align * align


def transfer(loader, addr: int, data: bytes, label: str) -> None:
    """MEM_BEGIN + MEM_DATA for one region, at `addr`."""
    if not data:
        print(f"  {label:9} 0 bytes at 0x{addr:08x}, skipped")
        return

    # MEM_DATA rejects a length that is not a multiple of 4. Sections are
    # 4-aligned so this is normally a no-op, but a trailing pad keeps the host
    # from ever being the thing that breaks.
    data = data + b"\x00" * ((-len(data)) % 4)

    blocks = round_up(len(data), ROM_BLOCK) // ROM_BLOCK
    loader.mem_begin(len(data), blocks, ROM_BLOCK, addr)
    for seq in range(blocks):
        loader.mem_block(data[seq * ROM_BLOCK : (seq + 1) * ROM_BLOCK], seq)

    print(f"  {label:9} {len(data):6d} bytes -> 0x{addr:08x}")


def read_console(port, stop: threading.Event) -> None:
    """Print whatever the payload says, until `stop` is set."""
    while not stop.is_set():
        try:
            chunk = port.read(64)
            if chunk:
                sys.stdout.write(chunk.decode("latin-1"))
                sys.stdout.flush()
        except Exception:
            pass


# --------------------------------------------------------------------------
# locating esptool
# --------------------------------------------------------------------------

# esptool is a normal pip package, so the only thing worth doing beyond a plain
# import is falling back to an ESP-IDF checkout's own environment when one is
# pointed at by $IDF_PATH. Nothing here is tied to a particular install layout.
def esptool_globs():
    """Site-packages directories that might contain esptool."""
    tools = esptool_home()
    patterns = []

    # A flat layout, e.g. <tools>/python/vX/venv/lib/site-packages.
    for lib in ("lib", "Lib"):
        patterns.append(os.path.join(tools, "*", lib, "site-packages"))
        patterns.append(os.path.join(tools, "python", "*", "venv", lib,
                                     "site-packages"))
        patterns.append(os.path.join(tools, "python", "*", lib,
                                     "site-packages"))

    # The sibling python_env an ESP-IDF checkout keeps next to its tools
    # directory, and the tools directory itself when both are given.
    idf = os.environ.get("IDF_PATH")
    roots = [tools]
    if idf:
        base = os.path.dirname(idf.rstrip("/\\"))
        roots.append(base)
        roots.append(os.path.join(base, "tools"))
    for root in roots:
        for lib in ("lib", "Lib"):
            patterns.append(os.path.join(root, "python_env", "*", lib,
                                         "site-packages"))
            patterns.append(os.path.join(root, ".espressif", "python_env", "*",
                                         lib, "site-packages"))

    # Keep order stable and drop duplicates.
    seen = set()
    unique = []
    for p in patterns:
        if p not in seen:
            seen.add(p)
            unique.append(p)
    return unique


def esptool_home():
    """Where ESP-IDF keeps its managed tools, if the environment says so."""
    # IDF_TOOLS_PATH is the documented location; fall back to the conventional
    # one rather than guessing at a drive letter.
    return os.environ.get(
        "IDF_TOOLS_PATH",
        os.path.join(os.path.expanduser("~"), ".espressif", "tools"),
    )


def import_esp32rom():
    """Return ESP32ROM, having found esptool wherever it lives."""
    try:
        from esptool.targets.esp32 import ESP32ROM
        return ESP32ROM
    except ImportError:
        pass

    tried = []
    for pattern in esptool_globs():
        for path in sorted(glob.glob(pattern), reverse=True):
            tried.append(path)
            if not os.path.isdir(os.path.join(path, "esptool")):
                continue
            if path not in sys.path:
                sys.path.insert(0, path)
            try:
                from esptool.targets.esp32 import ESP32ROM
                print(f"using esptool from {path}")
                return ESP32ROM
            except ImportError:
                sys.path.remove(path)

    raise SystemExit(
        "could not import esptool.\n"
        "tried the current interpreter, then:\n"
        + "".join(f"  {p}\n" for p in tried)
        + "\nSimplest fix:\n"
        "  pip install esptool\n"
        "or point IDF_PATH at an ESP-IDF checkout and use its python."
    )


def default_port():
    """Best guess at the serial port, so -p is usually unnecessary.

    Order: $RAMTOOL_PORT, then esptool's own port enumeration if exactly one
    device looks like a serial port. Returns None rather than guessing wrongly.
    """
    env = os.environ.get("RAMTOOL_PORT")
    if env:
        return env
    try:
        from serial.tools import list_ports

        ports = [p.device for p in list_ports.comports()]
        if len(ports) == 1:
            return ports[0]
        if ports:
            print(f"several serial ports found: {', '.join(ports)}", file=sys.stderr)
            print("pick one with --port", file=sys.stderr)
    except Exception:  # noqa: BLE001 - pyserial is optional at this point
        pass
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-p", "--port", default=None,
                    help="serial port; defaults to $RAMTOOL_PORT, or the only "
                         "serial port found")
    ap.add_argument("--seconds", type=float, default=0.0, metavar="N",
                    help="stay attached and print payload output for N seconds")
    ap.add_argument("--boot", action="store_true",
                    help="reset into the download bootloader and stop there")
    args = ap.parse_args()

    if args.port is None:
        args.port = default_port()
    if not args.port:
        ap.error("no serial port given and none could be detected; "
                 "pass --port (Windows: COM3, Linux: /dev/ttyUSB0)")

    elf = os.path.join(BUILD, "ramtool.elf")
    iram_bin = os.path.join(BUILD, "iram.bin")
    dram_bin = os.path.join(BUILD, "dram.bin")

    ESP32ROM = import_esp32rom()

    if args.boot:
        esp = ESP32ROM(args.port, baud=115200)
        esp.connect("default_reset")
        print("chip is in the download bootloader")
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass
        finally:
            esp._port.dtr = False
            esp._port.rts = False
            esp._port.close()
        return 0

    for path in (elf, iram_bin, dram_bin):
        if not os.path.exists(path):
            print(f"missing {path}\nrun build.ps1 first", file=sys.stderr)
            return 1

    # The ELF is the authority for both load addresses and the entry point, so
    # the host and ramtool.ld cannot drift apart silently.
    syms = elf_symbols(elf)
    try:
        entry = syms["ramtool_entry"]
        iram_addr = syms["_ramtool_iram_origin"]
        dram_addr = syms["_ramtool_dram_origin"]
    except KeyError as exc:
        print(f"{elf}: missing symbol {exc}", file=sys.stderr)
        return 1

    with open(iram_bin, "rb") as fh:
        iram = fh.read()
    with open(dram_bin, "rb") as fh:
        dram = fh.read()

    print(f"connecting to {args.port}")
    esp = ESP32ROM(args.port, baud=115200)
    esp.connect("default_reset")

    # The stub is the launch mechanism; see the module docstring. It is a RAM
    # upload like everything else, so this still writes nothing to flash.
    loader = esp.run_stub()
    print("stub resident (launch mechanism), still no flash writes")

    print("uploading payload")
    transfer(loader, iram_addr, iram, "iram")
    transfer(loader, dram_addr, dram, "dram")

    print(f"starting at 0x{entry:08x}")

    # From here the payload owns the chip and never returns, so this is a one-way
    # trip and there is no "check the result" step to do afterwards.
    port = loader._port
    port.dtr = False
    port.rts = False

    # Launch before attaching the reader.
    #
    # The stub does answer MEM_END, so let esptool read that reply normally and
    # only start echoing once it is done. The reply is four SLIP bytes and it is
    # on the wire long before the payload can print anything (our entry flushes
    # the cache, sets up a stack, zeroes bss and calls into the ROM first), so
    # there is no race and no banner bytes get eaten. A reader thread running
    # during mem_finish, on the other hand, races esptool for the same bytes and
    # silently drops characters from the banner.
    try:
        loader.mem_finish(entry)
    except Exception as exc:  # noqa: BLE001
        # Not fatal. If the reply was missed the payload has still started, and
        # its own output is the thing that matters.
        print(f"(mem_finish reported: {exc})")

    port.timeout = 0.05
    stop = threading.Event()
    reader = threading.Thread(target=read_console, args=(port, stop), daemon=True)
    reader.start()

    print("\npayload running. nothing was written to flash.")
    print("type here and press enter, or Ctrl-C to detach (the chip keeps running)")

    # Forward stdin on its own thread so --seconds still lets you drive the
    # payload and Ctrl-C stops both directions. Forwarding only in the
    # no-timeout branch made --seconds silently drop everything typed at it,
    # which is precisely how a broken command goes unnoticed.
    def forward_stdin():
        while not stop.is_set():
            try:
                line = sys.stdin.readline()
            except (KeyboardInterrupt, ValueError, OSError):
                return
            if not line:
                return
            try:
                port.write(line.encode())
            except Exception:
                return

    sender = threading.Thread(target=forward_stdin, daemon=True)
    sender.start()

    try:
        if args.seconds > 0:
            time.sleep(args.seconds)
        else:
            # Stay attached until stdin closes or the user interrupts.
            while sender.is_alive():
                time.sleep(0.1)
    except KeyboardInterrupt:
        pass

    stop.set()
    reader.join(timeout=1.0)
    port.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
