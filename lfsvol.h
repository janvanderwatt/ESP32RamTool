/* Read-only littlefs access, straight off SPI flash.
 *
 * The payload has no ESP-IDF, no VFS and no filesystem at all, so anything that
 * wants to see files on the chip has to talk to littlefs directly. That is
 * entirely doable: littlefs is a portable C library that needs a block device,
 * a CRC, and some buffers, and the ROM already knows how to read SPI flash.
 *
 * What this module provides is deliberately small:
 *
 *   - a read-only block device over the ROM's flash reader
 *   - the one function littlefs expects the host to supply, lfs_crc
 *   - partition-table scanning, so 'ls' finds the filesystem on its own
 *   - a directory walk that hands each entry to a callback
 *
 * There is no writing. See the notes on lfs_crc and on the return convention of
 * lfs_dir_read below; both cost real debugging time and both are easy to get
 * wrong again.
 */

#ifndef RAMTOOL_LFSVOL_H
#define RAMTOOL_LFSVOL_H

#include <stdint.h>

/* Called once per directory entry with the name and size. "." and ".." are
 * filtered out before the callback sees them. */
typedef void (*lfsvol_visit_fn)(void *ctx, const char *name, uint32_t size,
                                int is_dir);

/* Called once per entry in the partition table. `type` is 0x00 for an
 * application slot and 0x01 for a data partition; `subtype` refines that. */
typedef void (*lfsvol_part_fn)(void *ctx, const char *label, uint32_t offset,
                               uint32_t size, uint8_t type, uint8_t subtype);

/* Mount the filesystem, list `path`, unmount.
 *
 * `base` is the flash offset of the filesystem, or 0 to find it by scanning the
 * partition table at 0x8000. Discovery is the normal case; an explicit base is
 * the escape hatch for a layout the scan does not recognise.
 *
 * Returns the number of entries visited, or a negative littlefs error code.
 * A non-negative return of 0 means the directory was empty. */
int lfsvol_list(uint32_t base, const char *path, lfsvol_visit_fn visit,
                void *ctx);

/* Walk the partition table, calling `fn` for each entry. Returns the number of
 * entries reported. Shares the scanner with the 'ls' discovery path. */
int lfsvol_partitions(lfsvol_part_fn fn, void *ctx);

/* A short description of a negative littlefs error code, for messages.
 * Never returns NULL. */
const char *lfsvol_strerror(int err);

#endif /* RAMTOOL_LFSVOL_H */