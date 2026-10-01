/* Linker script for the RAM-only RamTool payload.
 *
 * The payload is downloaded into RAM by the ESP32 ROM serial bootloader and
 * executed directly. There is no flash image and no C runtime.
 *
 * Two output sections matter, because the host loads them as two separate
 * mem_begin/mem_block transfers:
 *
 *   .iram_img  code + literals + rodata, fetched as instructions
 *   .dram_img  initialised data, loaded verbatim (load address == run address)
 *
 * .bss and the stack are NOLOAD: the startup code zeroes .bss itself, so they
 * never appear in the downloaded image.
 */

/* The entry is called ramtool_entry, not _start, on purpose: ESP-IDF's
 * esp32.rom.ld does `PROVIDE (_start = 0x40000704)`, and letting that name
 * collide is a silent way to end up with the wrong address. */
ENTRY(ramtool_entry)

MEMORY
{
  /* IRAM 0x40080000..0x400D0000 is the instruction-fetch window. The ROM's
   * own text lives below 0x40070000 and esptool's flasher stub is at
   * 0x400be000, so 64 KiB from 0x40080000 is free and safely clear of both. */
  iram (rx)  : ORIGIN = 0x40080000, LENGTH = 64K

  /* DRAM 0x3FFB0000..0x3FFB8000 is below the ROM's own .bss_btdm, which the
   * section headers show starting at 0x3ffb8000. */
  dram (rwx) : ORIGIN = 0x3FFB0000, LENGTH = 28K
}

_ramtool_iram_origin = ORIGIN(iram);
_ramtool_dram_origin = ORIGIN(dram);

SECTIONS
{
  /* ---- fetched as instructions ----
   *
   * Code and the l32r literal pools only. Note that .rodata is NOT here even
   * though it would be the natural place for it: reading a byte out of the
   * IRAM window raises LoadStoreError on this part, so every string and
   * constant table has to live in DRAM. .xt.lit must stay, because it is part
   * of the instruction stream that -mlongcalls emits.
   */
  .iram_img : ALIGN(4)
  {
    KEEP(*(.iram1.entry))
    *(.iram1.text)
    *(.iram1.text.*)
    *(.literal)
    *(.literal.*)
    *(.text)
    *(.text.*)
    *(.xt.lit)
    *(.xt.prop)
    . = ALIGN(4);
  } > iram

  /* ---- loaded verbatim as data ----
   *
   * Read-only data, initialised data, and anything else the payload dereferences
   * at run time. The host sends this as its own MEM_BEGIN.
   */
  .dram_img : ALIGN(4)
  {
    *(.rodata)
    *(.rodata.*)
    *(.srodata)
    *(.srodata.*)
    *(.data)
    *(.data.*)
    *(.sdata)
    *(.sdata.*)
    . = ALIGN(4);
  } > dram

  /* ---- created at run time, never downloaded ---- */
  .bss (NOLOAD) : ALIGN(8)
  {
    __bss_start = .;
    *(.bss)
    *(.bss.*)
    *(.sbss)
    *(.sbss.*)
    *(COMMON)
    . = ALIGN(8);
    __bss_end = .;
  } > dram

  .stack (NOLOAD) : ALIGN(16)
  {
    __stack_bottom = .;
    . += 2K;
    . = ALIGN(16);
    __stack_top = .;
  } > dram

  /DISCARD/ :
  {
    *(.xtensa.info)
    *(.comment)
    *(.note*)
    *(.eh_frame*)
    *(.debug*)
  }
}

/* Not exported by esp32.rom.api.ld, but present and identical in the rev0 and
 * rev3 ROM images. Needed to detach the ROM's UART receive interrupt so it
 * stops stealing bytes from the payload's own console. */
PROVIDE ( ets_isr_mask   = 0x400067fc );
PROVIDE ( ets_isr_unmask = 0x40006808 );

/* The ROM's SPI flash block reader, called SPIRead. ESP-IDF reaches it from C as
 * esp_rom_spiflash_read, but no linker script PROVIDEs that name, so it is given
 * its literal address here. Verified identical in both ROM ELFs.
 *
 * Do not confuse it with SPI_read_status / esp_rom_spiflash_read_user_cmd, which
 * take a status pointer and a command byte and are not block reads at all. */
PROVIDE ( esp_rom_spiflash_read = 0x40062ed8 );

/* The ROM contains newlib's string routines, so there is no need to link a
 * second copy: littlefs calls memcpy/memset/strlen and friends, and pointing
 * them at the ROM's copies costs zero bytes of payload. Addresses read from the
 * ROM ELFs and identical in rev0 and rev3.
 *
 * Only the ones something actually references survive -- the linker discards the
 * rest, so this list is cheap to keep complete. */
PROVIDE ( memcpy   = 0x4000c2c8 );
PROVIDE ( memmove  = 0x4000c3c0 );
PROVIDE ( memset   = 0x4000c44c );
PROVIDE ( memcmp   = 0x4000c260 );
PROVIDE ( strcpy   = 0x400013ac );
PROVIDE ( strlen   = 0x400014c0 );
PROVIDE ( strcat   = 0x4000c518 );
PROVIDE ( strchr   = 0x4000c53c );
PROVIDE ( strcspn  = 0x4000c558 );
PROVIDE ( strspn   = 0x4000c648 );
PROVIDE ( strncmp  = 0x4000c5f4 );

/* lfs_util.c is not compiled: lfs_crc is defined in lfsvol.c instead. See the
 * long note there, because getting it wrong is subtle and expensive. */

ASSERT(__stack_top <= ORIGIN(dram) + LENGTH(dram),
       "RamTool: DRAM overflow, shrink .data/.bss or the stack")
ASSERT(SIZEOF(.iram_img) <= LENGTH(iram),
       "RamTool: IRAM overflow, the payload will not fit at 0x40080000")
