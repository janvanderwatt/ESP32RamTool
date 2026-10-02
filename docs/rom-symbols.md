# The ROM `PROVIDE`s less than you would expect

**`SPIRead`, `SPIWrite`, `SPIEraseArea` and the whole libc string family are in the
ROM, and not one of them is exported by ESP-IDF's linker scripts.**

## Why this bites

ESP-IDF ships a set of linker scripts under
`components/esp_rom/esp32/ld/` that map C names onto ROM addresses. Most of what a
freestanding payload needs comes from there — `esp_rom_printf`, `esp_rom_delay_us`,
`esp_rom_uart_tx_one_char`, `esp_rom_gpio_pad_select_gpio`, and so on. You just call
them and the linker resolves them.

But some ROM functions have no `PROVIDE` anywhere, because in normal IDF use they
are reached through C code in `esp_rom`'s own sources rather than directly. Call
one from a bare payload and you get an undefined reference to a name that looks
perfectly reasonable.

The fix is a `PROVIDE` with a literal address, in your own linker script:

```ld
PROVIDE ( esp_rom_spiflash_read = 0x40062ed8 ); /* SPIRead */
```

## Which ones this project needs

| C name | ROM symbol | Address | Provided by IDF? |
|---|---|---|---|
| `esp_rom_spiflash_read` | `SPIRead` | `0x40062ED8` | no |
| `esp_rom_spiflash_write` | `SPIWrite` | `0x40062D50` | no |
| `esp_rom_spiflash_erase_area` | `SPIEraseArea` | `0x400631AC` | no |
| `esp_rom_spiflash_attach` | — | `0x40062A6C` | yes |
| `ets_isr_mask` | — | `0x400067FC` | no |
| `ets_isr_unmask` | — | `0x40006808` | no |
| `memcpy` | — | `0x4000C2C8` | no |
| `memmove` | — | `0x4000C3C0` | no |
| `memset` | — | `0x4000C44C` | no |
| `memcmp` | — | `0x4000C260` | no |
| `strcpy` | — | `0x400013AC` | no |
| `strlen` | — | `0x400014C0` | no |
| `strcat` | — | `0x4000C518` | no |
| `strchr` | — | `0x4000C53C` | no |
| `strcspn` | — | `0x4000C558` | no |
| `strspn` | — | `0x4000C648` | no |
| `strncmp` | — | `0x4000C5F4` | no |

Pointing the string routines at the ROM is free: they cost zero bytes of payload,
which matters when littlefs alone calls `memcpy`, `memset`, `memcmp`, `strlen` and
`strcpy`. The linker discards the `PROVIDE`s nothing references, so the list is
cheap to keep complete.

## Verifying an address yourself

Do not trust a table, including this one. ESP-IDF ships the ROM ELFs with
matching symbol tables; read the address straight out of them.

```powershell
& "$env:IDF_TOOLS_PATH\xtensa-esp-elf\*\bin\xtensa-esp32-elf-nm.exe" `
    "$env:IDF_TOOLS_PATH\esp-rom-elfs\*\esp32_rev0_rom.elf" |
    Select-String 'SPIRead|SPIWrite|SPIEraseArea|memcpy'
```

Two files matter: `esp32_rev0_rom.elf` and `esp32_rev300_rom.elf`. **Check both.**
Every address in the table above is identical in both, which is what lets one
table cover the whole family — but that is a property to verify per address, not
to assume.

The `PROVIDE`s for `ets_isr_mask` and `ets_isr_unmask` are needed for a different
reason: the names are the ROM's own, and nothing exports them under those names
even though the functions exist.

## Why so many of these are invisible in normal IDF use

It is worth keeping in mind that every one of these gaps is invisible until you
leave the IDF world. `esp_rom_spiflash_read` is a perfectly ordinary-looking call
that ESP-IDF's own `esp_rom` component implements in C, on top of the ROM. Bypass
the component and the name resolves to nothing, even though the function is
demonstrably there at a known address.