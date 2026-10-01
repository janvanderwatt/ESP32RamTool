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

ASSERT(__stack_top <= ORIGIN(dram) + LENGTH(dram),
       "RamTool: DRAM overflow, shrink .data/.bss or the stack")
ASSERT(SIZEOF(.iram_img) <= LENGTH(iram),
       "RamTool: IRAM overflow, the payload will not fit at 0x40080000")
