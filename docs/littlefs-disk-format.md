# littlefs on-disk format

**Metadata tags are stored big-endian and XOR-chained with the previous tag.
Decode them as little-endian words and you get confident nonsense.**

## The tag chain

Every tag on disk is stored **big-endian** and XORed with the tag before it. The
chain is seeded at `0xFFFFFFFF`:

```c
tag = frombe32(raw) ^ ptag;
```

So a tag read from disk is meaningless in isolation. Walking a block looks like
this — the first tag is the superblock, and it is recognisable because
`0x0FF00008` XOR `0xFFFFFFFF` is what the chain gives you:

```
=== block 1 ===
  off=0x0004 tag=0x0ff00008 SUPERBLOCK   name=b'littlefs'
  off=0x0010 tag=0x20100018 INLINESTRUCT  struct version=0x00020000 ...
```

A walker needs, for each tag: `type = tag >> 20 & 0x7ff`, `id = (tag >> 10) &
0x3ff`, `size = tag & 0x3ff`, and the advance is `4 + size`. Validity is
`!(tag & 0x80000000)`.

## Why this is worth knowing

Only needed when reading a raw image without littlefs — which is exactly what you
end up doing when a mount fails and you want to know whether the image or the code
is at fault.

Getting it wrong is worse than having no decoder, because the numbers are
*plausible*. A block size that decodes as `0x00100000` is really `4096` once the
chain is undone. A superblock that looks like version `1` at offset 0 is actually
a revision word followed by a tag. It is very easy to spend a long time debugging
the wrong layer.

## The superblock

Block 0 and block 1 are a redundant pair; the one with the higher revision wins.
Each holds:

1. a 4-byte revision word,
2. a `SUPERBLOCK` tag naming the volume `littlefs`,
3. an `INLINESTRUCT` tag carrying 24 bytes: `version`, `block_size`,
   `block_count`, `name_max`, `file_max`, `attr_max`.

A real image from `mklittlefs` decodes as:

```
version=0x00020000 (major=2 minor=0)
block_size=4096  block_count=384
name_max=64  file_max=2147483647  attr_max=1022
```

That `block_size` and `block_count` are load-bearing for this project:

- `block_count` is passed to `lfs_mount` as **0**, which tells littlefs to read the
  real value from the superblock. Convenient for mounting; see
  [safe-flash-writes.md](safe-flash-writes.md) for why it is a problem for writing.
- `block_size` **cannot** be defaulted the same way. A mismatch is a hard
  `LFS_ERR_INVAL`, so the mount code tries a short list of likely values (4096
  first, then 512, 1024, 8192, 16384).

The cross-check worth doing: `384 × 4096 == 0x180000`, exactly the size of the
partition. When the arithmetic closes like that, the geometry is right.

## Version numbers

`LFS_VERSION` is the library version; `LFS_DISK_VERSION` is the format it writes,
and that is the one that governs whether a build can mount a given image. A 2.0
image mounts fine on any later 2.x library.

littlefs 2.9.3 also carries v1 compatibility behind `LFS_MULTIVERSION`, with
`LFS1_DISK_VERSION 0x00010001`. It is not enabled here.

## Reading a whole image for analysis

`esptool`'s stub can dump regions without touching the reset behaviour, which is
the easy way to get an image to a file:

```python
loader = esp.run_stub()
open("head.bin", "wb").write(bytes(loader.read_flash(0x670000, 0x40000)))
```

A few tens of KB is usually enough — small filesystems put their metadata and file
contents in the first few blocks. Scan the whole device only when you have reason
to; over serial it takes minutes.

One warning from experience: `esptool` verifies a digest per read, and a single
mismatch means a serial transport glitch, not that the flash changed. Retry reads
before concluding anything.