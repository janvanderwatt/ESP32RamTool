#!/usr/bin/env python3
"""Optional ROM table audit. Requires ROM ELFs only when explicitly invoked."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT))
from run import elf_symbols


def verify(table: Path, roms: list[Path], payload: Path | None = None) -> None:
    pattern = re.compile(
        r"PROVIDE\s*\(\s*(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*\)\s*;"
        r"\s*/\* rom: (\w+) \*/"
    )
    text = table.read_text(encoding="utf-8")
    entries = pattern.findall(text)
    if not entries or len(entries) != text.count("PROVIDE"):
        raise ValueError("every PROVIDE must have a literal address and rom: annotation")
    if len({alias for alias, _, _ in entries}) != len(entries):
        raise ValueError("duplicate aliases in ROM table")
    for path in roms:
        symbols = elf_symbols(str(path))
        for alias, address, native in entries:
            actual = symbols.get(native)
            expected = int(address, 16)
            if actual != expected:
                found = "missing" if actual is None else f"0x{actual:08x}"
                raise ValueError(f"{path.name}: {alias} / {native}: expected {address}, found {found}")
        print(f"{path.name}: all {len(entries)} ROM bindings match")
    if payload:
        symbols = elf_symbols(str(payload))
        checked = 0
        for alias, address, _ in entries:
            if alias in symbols:
                if symbols[alias] != int(address, 16):
                    raise ValueError(f"{payload.name}: linked {alias} differs from pinned address")
                checked += 1
        if not checked:
            raise ValueError("payload contains no pinned ROM symbols")
        print(f"{payload.name}: all {checked} referenced ROM bindings match")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rev0", type=Path, required=True, help="esp32_rev0_rom.elf")
    parser.add_argument("--rev3", type=Path, required=True, help="esp32_rev300_rom.elf")
    parser.add_argument("--table", type=Path, default=ROOT / "rom/esp32.ld")
    parser.add_argument("--payload", type=Path, help="also audit a built ramtool.elf")
    args = parser.parse_args()
    try:
        verify(args.table, [args.rev0, args.rev3], args.payload)
    except (OSError, ValueError, IndexError, KeyError) as exc:
        print(f"ROM verification failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
