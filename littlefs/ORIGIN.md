# Vendored littlefs

These files are **unmodified upstream littlefs**. Do not edit them; see
"Local changes" below for what is added around them instead.

| | |
|---|---|
| Project | littlefs, a little filesystem for embedded systems |
| Upstream | <https://github.com/littlefs-project/littlefs> |
| Version | v2.9.3 (`LFS_VERSION` = `0x00020009`) |
| Commit | `d01280e64934a09ba16cac60cf9d3a37e228bb66` (tag `v2.9.3`) |
| Licence | BSD-3-Clause, see `LICENSE.md` |
| Retrieved | 2026-10-01 |

## What is here, and what is not

Only the three files this build actually compiles:

```
lfs.c        the implementation
lfs.h        public API and types
lfs_util.h   CRC, memcpy and small helpers
LICENSE.md   the licence text, verbatim
```

Upstream also ships `lfs.cmp`, `lfs_migrate*.c`, `lfs_migrate.h` and a test
suite. None of it is copied here, because none of it is built. The on-disk
format this payload reads is littlefs v2, and `LFS_MULTIVERSION` support (which
is what would pull in the v1 migration path) is not enabled.

`lfs_util.c` is deliberately absent. It exists upstream to hold `lfs_crc`, and
`lfs_crc` is defined in `../lfsvol.c` instead — see below, it is not a
mechanical substitution.

## Local changes

None to these files. The integration lives entirely outside this directory:

- `../lfsvol.c` provides the block device, `lfs_crc` and the buffers, and does
  the partition discovery and directory walk.
- `../build.ps1` compiles `lfs.c` with its own flag set rather than the
  project's `-Werror` set. Those flags are mandatory, not cosmetic:
  `LFS_NO_DEBUG`, `LFS_NO_WARN` and `LFS_NO_ERROR` all expand to `printf`,
  which does not exist in this payload, and without all three the link fails on
  undefined references. `LFS_NO_MALLOC` and `LFS_NO_THREADSAFE` remove the
  allocator and the mutex; there is no heap and only one task. `LFS_NO_ASSERT`
  drops `assert()`, which is also absent.

To upgrade, replace the three files and check the version macros in `lfs.h`.
`LFS_DISK_VERSION` is the one that matters: it is what decides whether a given
build can mount a given on-disk image, and the check lives in `lfs_mount`.

## Verifying these files are still pristine

Blob SHAs match upstream v2.9.3. `git hash-object` normalises line endings on
the way in, so this works even though `.gitattributes` checks the tree out as
CRLF:

```powershell
cd littlefs
git hash-object -- lfs.c lfs.h lfs_util.h LICENSE.md
```

Expected:

```
d35d5d6dbb62b08ebe2a52a0c600ab50dfec097f  lfs.c
8473897373042fbd4d5fa951cfaa56fc34b87586  lfs.h
4e5770090b03cf67d0806bbb6e0d568113a065a9  lfs_util.h
e6c3a7bac1d8b3ed2ff3160a5bb49abebdd6fe7e  LICENSE.md
```

The upstream values come from the GitHub API, which is also how the commit id
above was pinned:

```powershell
(Invoke-RestMethod -Uri 'https://api.github.com/repos/littlefs-project/littlefs/git/trees/d01280e64934a09ba16cac60cf9d3a37e228bb66').tree |
    Where-Object { $_.path -in 'lfs.c','lfs.h','lfs_util.h','LICENSE.md' } |
    Select-Object path, sha, size
```

## Why `lfs_crc` is not taken from upstream

Upstream puts `lfs_crc` in `lfs_util.c`, which would be the obvious thing to
vendor along with the rest. It is not vendored, and `lfs_crc` is reimplemented
in `../lfsvol.c`, because the obvious thing to do instead is worse.

It is very tempting to alias littlefs's checksum onto the ROM's
`esp_rom_crc32_le` and save the function entirely. Both are reflected CRC-32
with polynomial `0xedb88320`, so the alias looks free and correct. It is not:
littlefs's version never inverts its seed or its result, while the ROM helper
inverts both. Every metadata block then fails verification, and littlefs
reports only

```
Corrupted dir pair at {0x0, 0x1}
```

which mentions neither checksums nor the cause. If the copy had been vendored
along with everything else, this would have been an invisible alias in the
linker script rather than a documented implementation. Reimplementing it keeps
the decision visible, in one place, next to the explanation.

This was confirmed against real data rather than reasoned about: recomputing the
CCRC tag stored in each of the two on-flash superblock copies, the no-invert
form reproduces both (`0x0bbfbaf6` and `0x4d6d8f92`) and the ROM form
reproduces neither.

Note also that `lfs_crc` is declared in `lfs.h`, so a definition is *required*;
`lfs.c` alone does not provide one.