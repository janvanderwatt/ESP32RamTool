# Reading and writing SPI flash from the ROM

**`esp_rom_spiflash_read_user_cmd` is not a block read — it takes
`(status *, cmd)` and *hangs* when handed block arguments. The real one is
`SPIRead`.**

## The trap

The ROM exposes a family of flash functions whose names differ by a suffix and
whose behaviour differs completely:

| Name | Signature | What it is |
|---|---|---|
| `esp_rom_spiflash_read` | `(src_addr, dest*, len)` | the block reader |
| `esp_rom_spiflash_read_user_cmd` | `(status*, cmd)` | one raw SPI command |
| `esp_rom_spiflash_write` | `(dest_addr, src*, len)` | the block writer |
| `esp_rom_spiflash_erase_area` | `(start_addr, area_len)` | erase a byte range |

Calling `read_user_cmd` with a source address and a length does not fault and does
not return an error. It reads whatever it interprets the arguments as, and the
payload hangs. That is a genuinely bad failure mode: it presents as a dead program
rather than a bad argument, and the only clue is the name.

Confirm signatures against
`components/esp_rom/esp32/include/esp32/rom/spi_flash.h` before trusting any
guess. See [rom-symbols.md](rom-symbols.md) for addresses.

## Erase takes an address, not a block

`esp_rom_spiflash_erase_area(start_addr, area_len)` is given a **byte range**,
where littlefs thinks in **blocks**. The block device callback has to translate.

This implementation assumes `block_size == flash sector size`, which holds for
every volume encountered (littlefs images made by `mklittlefs` and by the Arduino
ESP32 core both use 4096-byte blocks on 4096-byte sectors). A volume with
sub-sector blocks would need the containing sector erased instead, and the
address range check widened to match. That is an assumption, not a general
solution.

## Interrupts being masked is fine

`start.S` masks ISR levels so the ROM's UART handler stops stealing console bytes.
A reasonable worry is whether the flash *writer* needs interrupts.

It does not. The ROM's `SPIWrite` polls the SPI peripheral rather than waiting on
an interrupt, so it works with interrupts masked. Verified on hardware rather than
assumed — this project had already lost an afternoon to a flash call that hung for
a superficially similar reason.

## Bringing the flash peripheral up

`esp_rom_spiflash_attach(ishspi, legacy)` with `(0, 0)` before first use. On the
part tested here reads work fine without it, so it is belt and braces, but it costs
one call and removes a variable from the debugging surface.

## Why this works at all from a bare payload

Because there is no filesystem, no VFS and no driver in the way. The payload calls
a ROM function that copies bytes out of the flash chip over SPI, and hands that to
littlefs as a block device. No cache coherency concerns arise, because data is
never read through the memory-mapped flash alias — it always goes via SPI. (This is
also why writing needs no cache flush: the instruction cache holds code from IRAM,
and flash data is never mapped into the instruction-fetch path.)