# littlefs API traps

**Four assumptions that look completely reasonable, and that littlefs answers with
a hang, a link error, or 8 KB of wasted DRAM.**

## `lfs_dir_read` returns *positive* on success

Not `0`. The API is:

- **positive** — an entry was read
- **0** — end of directory
- **negative** — error

Both obvious idioms are wrong:

```c
/* wrong: stops before reading anything */
while ((err = lfs_dir_read(&lfs, &dir, &info)) == 0) { ... }

/* wrong: fires on the very first, successful, read */
if ((err = lfs_dir_read(&lfs, &dir, &info))) { /* error */ }
```

The second one is the more expensive mistake, because a successful read is
indistinguishable from a failure if you only test truthiness. Check the sign:

```c
for (;;) {
    int err = lfs_dir_read(&lfs, &dir, &info);
    if (err < 0) { /* error */ break; }
    if (err == 0) { break; }        /* end of directory */
    /* success */
}
```

## Buffers are `cache_size` bytes, **not** `block_size`

`lfs_mount` uses `cfg->read_buffer` and `cfg->prog_buffer` at `cfg->cache_size`
bytes each. Sizing them at `block_size` — which is what a first reading suggests,
and what looks safer — costs 8 KB of DRAM for nothing on a 4096-byte block size
and two buffers.

```c
#define LFSVOL_CACHE_SIZE 128

static uint8_t read_buf[LFSVOL_CACHE_SIZE];
static uint8_t prog_buf[LFSVOL_CACHE_SIZE];
static uint8_t lookahead_buf[16];
```

With that, the three together come to 272 bytes.

### `cache_size = 0` is not "no cache"

It is an invalid configuration. `cache_size` must be a non-zero multiple of both
`read_size` and `prog_size`, and must divide `block_size`. Zero is rejected, and
being rejected looks like a hang rather than an error.

## Under `LFS_NO_MALLOC`, every buffer is your problem

`LFS_NO_MALLOC` is not optional here — there is no heap in a RAM payload. But it
also means littlefs allocates nothing on your behalf, so three things must be
supplied by hand and each fails in a different unhelpful way.

### `prog_buffer` is required even when nothing is written

A read-only build still has to supply `cfg->prog_buffer`. With `LFS_NO_MALLOC`,
`lfs_mount` asserts on a `NULL` one. It does not have to be large — `cache_size` is
enough — it just has to be there.

### `lfs_file_open` does not exist

This is a link error, not a runtime failure. `lfs.c` wraps it in:

```c
#ifndef LFS_NO_MALLOC
int lfs_file_open(lfs_t *lfs, lfs_file_t *file, const char *path, int flags) {
```

so under our flags the symbol is simply absent:

```
undefined reference to `lfs_file_open'
```

`lfs.h` warns about it in advance:

> if LFS_NO_MALLOC is defined, lfs_file_open() will fail with LFS_ERR_NOMEM, thus
> use lfs_file_opencfg() with config.buffer set.

Which it does, and the buffer wants `cache_size` bytes — the same size as the mount
caches. Zero the config first, because `attr_count` of 0 is what littlefs checks to
decide there are no attributes to look for:

```c
static int open_file(lfs_file_t *f, const char *path, int flags)
{
    struct lfs_file_config fc;
    memset(&fc, 0, sizeof(fc));
    fc.buffer = file_buf;              /* cache_size bytes */
    return lfs_file_opencfg(&lfs, f, path, flags, &fc);
}
```

### One open file at a time

`LFS_NO_THREADSAFE` is on (one task, no mutexes) and the file buffer is a single
static allocation, so concurrent open files would share it. The design here mounts,
does one thing, and unmounts, which sidesteps the question entirely.

## Why mount/unmount per command is the right shape anyway

An unmounted filesystem holds no state on the flash. Mounting, doing one operation
and unmounting means a command that dies partway through cannot leave the volume
needing repair on the next boot. There is no long-lived session to lose.

It is wasteful in principle and correct in practice.