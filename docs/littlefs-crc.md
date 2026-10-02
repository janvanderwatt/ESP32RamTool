# `lfs_crc` is not `esp_rom_crc32_le`

**Same polynomial, opposite convention, and littlefs reports the result as
`Corrupted dir pair` — which mentions neither checksums nor the cause. This one
cost the longest to find.**

## The temptation

littlefs needs a CRC. The ROM has a CRC helper, `esp_rom_crc32_le`. Aliasing one
to the other costs **zero bytes** of payload:

```ld
/* do not do this */
lfs_crc = 0x4005cfec;   /* esp_rom_crc32_le */
```

Both are reflected CRC-32 with polynomial `0xEDB88320`. The alias looks free and
correct. It is neither.

## What differs

| | Inversion |
|---|---|
| littlefs `lfs_crc` | never inverts — seed in raw, result out raw |
| `esp_rom_crc32_le` | zlib style — inverts the seed on entry, the result on exit |

littlefs's is a 16-entry nibble-table CRC. The ROM's is the same polynomial driven
a byte at a time, wrapped in the zlib convention.

## What you see

Not a checksum error. Every metadata block fails verification, so
`lfs_dir_fetchmatch` rejects both halves of the superblock pair and reports:

```
Corrupted dir pair at {0x0, 0x1}
```

which points at neither checksums nor the cause. A full 8 MB scan of the flash
finds the `littlefs` magic perfectly happily, and the superblock decodes to
sensible values — the image is fine, the checksum is wrong.

## Proving it rather than reasoning about it

The on-flash metadata blocks end in a CCRC tag holding a CRC of everything before
it. Recompute that CRC both ways and see which one matches what is on the disk:

```
block 0: stored dcrc=0x0bbfbaf6
   lfs_crc (no invert)      = 0x0bbfbaf6  MATCH
   zlib/rom crc32_le       = 0xf9370963
block 1: stored dcrc=0x4d6d8f92
   lfs_crc (no invert)      = 0x4d6d8f92  MATCH
   zlib/rom crc32_le       = 0xbfe53c07
```

Two independent superblock copies, two matches for the no-invert form, two misses
for the ROM form. That settles it.

## What to do

Implement `lfs_crc` properly. It is short, and `lfs.c` does not define it — it
normally lives in `lfs_util.c`, which is not compiled here — so a definition is
required or the link fails.

```c
uint32_t lfs_crc(uint32_t crc, const void *buffer, size_t size)
{
    static const uint32_t rtable[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    const uint8_t *data = (const uint8_t *)buffer;
    size_t i;

    for (i = 0; i < size; i++) {
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 0)) & 0xf];
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 4)) & 0xf];
    }
    return crc;
}
```

About 150 bytes including the table, against 22 KB of payload. Cheap insurance.

## The general lesson

`lfs_util.h` only defines its logging macros `#ifndef`, which means you can replace
them from the command line. That turns littlefs's own diagnostics into something
usable without dragging in a printf:

```sh
-DLFS_ERROR(...)=my_reporter(__LINE__)
```

The `LFS_ERROR` at the line that actually failed is worth more than the message it
would have printed, because it names the check:

```
>>> littlefs LFS_ERROR at lfs.c:1369      /* "Corrupted dir pair" */
```

There is no `printf` in this payload, so the default macros cannot be used — which
is why this project compiles littlefs with `LFS_NO_DEBUG`, `LFS_NO_WARN` and
`LFS_NO_ERROR` and reinstates only `LFS_ERROR` when debugging.