# Safe flash writes

**`lfs_mount` takes `block_count` from the superblock and checks it against
nothing. Once erase is live, a corrupt superblock can reach your application — so
writing needs a clamp, not just a lock.**

## The lock, and why a lock alone is nearly worthless

Writing is gated behind an explicit `rw on`, which is the right first line of
defence. But the interesting half is what the unlock has to promise.

### Where the size comes from — and why it is dangerous

`lfs_mount` is deliberately handed `block_count` 0, which tells littlefs to read the
real value out of the superblock. That is exactly right for **mounting**: there is
then no partition size to keep in step with, and no way for the two to disagree.

It is exactly wrong for **writing**, because it means the filesystem's idea of how
big it is comes off the disk and is validated against nothing. Consider a superblock
claiming:

```
block_size 4096, block_count 100000     ->  400 MB
```

Every one of those blocks is a legal erase as far as littlefs is concerned,
including the running application, NVS, and the partition table itself.

### The clamp

`bd_prog` and `bd_erase` therefore check every address against the region that
`rw on` established. The region comes from the partition table's own size, rounded
**down** to a whole number of blocks:

```c
wr_size = (psize / bsize) * bsize;
```

Rounding down to a block edge means the clamp can never disagree with littlefs
about where the volume ends. The unlocked region is printed when it is granted, so
it is visible rather than implied:

```
> rw on
writing enabled, 0x00670000 + 0x00180000. this does not survive a reboot.
```

### Which is why explicit offsets need an explicit size

`rw on @<offset>` has no partition size behind it, so there is nothing to clamp
against. Rather than guess, `lfsvol_write_enable` refuses that combination unless a
size is given:

```
rw on @<offset> <size>
```

Guessing here would be worse than useless — it would look like the safety mechanism
was present when it was not.

## Where the lock lives

In the block device, not in the command layer.

This is the important structural decision. If the check lived in `payload.c`, then a
bug in a command handler could reach an erase. Putting it in `bd_prog`/`bd_erase`
means the only path to a programmed byte goes through the check, regardless of what
the caller does.

## The lock does not survive a reboot

The state is in `.bss`, which `start.S` zeroes. So re-locking is the default, and
so is the state after every `run.py`. There is no way to leave the chip armed.

## Writing is not atomic, and the tool is honest about that

Flash erase-then-program means a write interrupted partway leaves a torn block.
littlefs is designed for exactly this — the metadata CRC catches it and the
filesystem relocates on the next mount — but it does mean a power cut during a
write can cost you the file being written.

This is why the unlock prints a warning and why it does not persist.

## What is still never written

- **Loading and running the payload.** The esptool stub is a RAM upload.
- **Reading.** `ls`, `cat`, `part` and the partition scan are all `SPIRead`.

Verified by hashing the partition table and the head of the volume before and after
a session that mounts, lists, reads, and has a write attempt refused: byte-identical.
With `rw on`, the same regions change, as they must.

## Commands

| | |
|---|---|
| `rw` | report whether writing is unlocked, and the region |
| `rw on` | unlock, discovering the volume and its size |
| `rw on @<offset> <size>` | unlock an explicit region with explicit bounds |
| `rw off` | re-lock |
| `put <path> <line>` | replace a file with one line, creating it if absent |
| `add <path> <line>` | append one line, creating the file if absent |
| `rm <path>` | delete a file |

`put`, `add` and `rm` need the unlock; `cat`, `ls` and `part` do not.

The trailing newline is added by `lfsvol`, not the caller, so a file cannot end up
without line structure by forgetting it.