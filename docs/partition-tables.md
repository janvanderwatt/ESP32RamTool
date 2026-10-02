# Partition labels and subtypes are not evidence

**PlatformIO writes a littlefs image into the partition its table labels `spiffs`.
Neither the label nor the subtype tells you what is actually on the flash.**

## What happens

The partition table on the board used for development reads:

```
nvs       @0x00009000  0x00005000  data sub 0x02
otadata   @0x0000e000  0x00002000  data sub 0x00
app0      @0x00010000  0x00330000  app  sub 0x10
app1      @0x00340000  0x00330000  app  sub 0x11
spiffs    @0x00670000  0x00180000  data sub 0x82
coredump  @0x007f0000  0x00010000  data sub 0x03
```

`sub 0x82` is `ESP_PARTITION_SUBTYPE_SPIFFS`, and the label says `spiffs`. The
contents are a littlefs filesystem, because PlatformIO builds a `littlefs.bin` and
writes it into the partition its own tooling expects to find. Reading the label and
concluding "this is SPIFFS" would be wrong, and the two formats are not even
readable by each other.

## What to do

Treat the label and subtype as hints, and the mount as the test. This project
tries candidates and keeps whichever one actually mounts.

Ordering, cheapest-confidence first:

1. data partitions whose subtype is a declared filesystem (`0x83` littlefs,
   `0x82` SPIFFS),
2. then any other data partition,
3. trying a short list of block sizes for each.

A failed mount reads only a handful of metadata blocks over SPI, so trying several
candidates is not as slow as it sounds.

## Reading the table itself

The table lives at `0x8000` — a ROM convention, not something a payload has to be
told. Entries are 32 bytes, and the scan stops at the first one without the magic.

### The magic is 16 bits, not 32

This is an easy self-inflicted wound. The first two bytes are the magic `AA 50`
(little-endian `0x50AA`), and bytes 2 and 3 are type and subtype. Reading four
bytes as one little-endian word and comparing against `0x50AA` **never matches**:

```c
/* wrong: gives 0x020150aa for the first nvs entry */
if (rd32le(raw) != 0x50AAu) { break; }

/* right */
if (rd16le(raw) != 0x50AAu) { break; }
```

This presents as "the table is empty" with no error anywhere, which looks
convincingly like a device with no partition table.

### Entry layout

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | magic `0x50AA` |
| 2 | 1 | type (`0x00` app, `0x01` data) |
| 3 | 1 | subtype |
| 4 | 4 | offset |
| 8 | 4 | size |
| 12 | 16 | label, NUL-padded |
| 28 | 4 | flags |

### Subtypes worth knowing

From `esp_partition.h`: `OTA 0x00`, `PHY 0x01`, `NVS 0x02`, `COREDUMP 0x03`,
`FAT 0x80`, `SPIFFS 0x82`, `LITTLEFS 0x83`.

## Align the buffer, not the cast

The ROM reader takes a `uint32_t *`. A `static uint8_t[32]` has no alignment
guarantee, so casting it is relying on the compiler happening to place a 32-byte
object on a 4-byte boundary. Declare the buffer as `uint32_t` and index through a
`uint8_t` pointer instead:

```c
static uint32_t part_raw[32 / 4];
#define PART_RAW ((const uint8_t *)part_raw)
```