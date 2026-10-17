# SD regression tests

These tests use a simulated SPI card; they never open a serial port or write
physical media. They link the production SD protocol, filesystem wrapper,
and vendored FatFs. Assertions must remain enabled.

On Linux/macOS, from the repository root:

```sh
mkdir -p build
cc -std=c11 -Wall -Wextra -Werror -I. tests/sd_test.c sdspi.c sdvol.c \
  fatfs/ff.c fatfs/ffunicode.c -o build/sd_test
./build/sd_test
```

From a Visual Studio x64 Native Tools command prompt on Windows:

```bat
if not exist build mkdir build
cl /nologo /std:c11 /W4 /I. tests\sd_test.c sdspi.c sdvol.c fatfs\ff.c fatfs\ffunicode.c /Fobuild\ /Febuild\sd_test.exe
build\sd_test.exe
```

To also test the actual SD console parser, substitute `tests/sd_console_test.c`
for `tests/sd_test.c` in either build command. For that host-only test, omit
`-Werror` on 64-bit GCC hosts: the included ESP32 register commands use 32-bit
addresses and produce pointer-size warnings even though those commands are
never executed. The console suite checks absolute paths, nested file commands,
pin parsing overflow, extra/missing arguments, and lock changes. It also runs
the complete protocol/filesystem suite. Hardware and internal-flash function
stubs assert if called.

Coverage includes FAT12/16/32 with and without an MBR, long names, replacement,
append, directory creation/removal, multi-cluster files, persistence on remount,
SDSC byte addressing, SDHC block addressing, write locking, sector bounds,
write rejection, programming errors, busy timeout, read error tokens, corrupt
CRC, missing cards, invalid CMD8 echo, initialization timeout, unknown
filesystems, and full-disk short writes.

The software GPIO timing and electrical behavior still require a real ESP32.
