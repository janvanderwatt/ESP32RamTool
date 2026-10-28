/* Read and write a littlefs volume straight off SPI flash.
 *
 * The payload has no ESP-IDF, no VFS and no filesystem at all, so anything that
 * wants to see files on the chip has to talk to littlefs directly. That is
 * entirely doable: littlefs is a portable C library that needs a block device,
 * a CRC, and some buffers, and the ROM already knows how to read SPI flash.
 *
 * What this module provides:
 *
 *   - a block device over the ROM's flash reader and writer
 *   - the one function littlefs expects the host to supply, lfs_crc
 *   - partition-table scanning, so commands find the filesystem on their own
 *   - directory walking, whole-file reading, single-line writing, and removal
 *   - formatting the region that writing has been unlocked for
 *
 * Writing is gated. See lfsvol_write_enable before implementing anything that
 * erases flash; the short version is that this is the one part of the tool that
 * can destroy what is on the chip, so it does nothing until it is told exactly
 * which bytes it is allowed to touch.
 *
 * See the notes at the top of lfsvol.c. Four of them are traps that all
 * present as something other than what they are.
 */

#ifndef RAMTOOL_LFSVOL_H
#define RAMTOOL_LFSVOL_H

#include <stdint.h>

/* Returned by the write calls when no write region has been unlocked. Chosen
 * well below any littlefs error code so the two can never be confused. */
#define LFSVOL_ERR_LOCKED (-1000)

/* Called once per entry with a chunk of file content. */
typedef void (*lfsvol_line_fn)(void *ctx, const char *buf, uint32_t len);

/* Called once per directory entry. "." and ".." are filtered out first. */
typedef void (*lfsvol_visit_fn)(void *ctx, const char *name, uint32_t size,
                                int is_dir);

/* Called once per entry in the partition table. `type` is 0x00 for an
 * application slot and 0x01 for a data partition; `subtype` refines that. */
typedef void (*lfsvol_part_fn)(void *ctx, const char *label, uint32_t offset,
                               uint32_t size, uint8_t type, uint8_t subtype);

/* ---------------------------------------------------------------- reading */

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

/* Read a whole file, handing each chunk to `line`. Returns the number of bytes
 * emitted, or a negative error code. Reading needs no unlock. */
int lfsvol_cat(uint32_t base, const char *path, lfsvol_line_fn line, void *ctx);

/* Walk the partition table, calling `fn` for each entry. Returns the number of
 * entries reported. Shares the scanner with the discovery path. */
int lfsvol_partitions(lfsvol_part_fn fn, void *ctx);

/* ---------------------------------------------------------------- writing */

/* Unlock writing, and only within [base, base + size).
 *
 * base 0 means "find the filesystem the usual way" and use its partition size.
 * An explicit base MUST be given with an explicit size, because without one
 * there is no way to know where the volume stops and the clamp below would have
 * nothing to clamp against; that combination is refused. An explicit region
 * that holds nothing mountable is still unlocked, with a 4096-byte block
 * geometry, so that lfsvol_format can repair it.
 *
 * The region survives until lfsvol_write_disable, and does not survive a
 * reboot, since it lives in .bss.
 *
 * Returns 0 if writing is now permitted, or a negative error code. */
int lfsvol_write_enable(uint32_t base, uint32_t size);

/* Re-lock. Safe to call when already locked. */
void lfsvol_write_disable(void);

/* Report the unlocked region. Returns 1 if writes are permitted, 0 if not. */
int lfsvol_write_region(uint32_t *base, uint32_t *size);

/* Write one line (a trailing newline is added) to `path`, replacing any
 * existing content and creating the file if absent. */
int lfsvol_put(const char *path, const char *text);

/* Append one line to `path`, creating the file if absent. */
int lfsvol_add(const char *path, const char *text);

/* Delete `path`. Fails if it is a directory or does not exist. */
int lfsvol_rm(const char *path);

/* Format the region unlocked by lfsvol_write_enable.
 *
 * This is a logical format, not an erase: lfs_format writes a fresh superblock
 * and root directory and bumps the revision, so the previous tree is ignored,
 * but its blocks are not wiped. The unlock is not consumed, so writing works
 * immediately afterwards.
 *
 * Fills base, size and block_size (any may be NULL) with the geometry written.
 * Returns 0, LFSVOL_ERR_LOCKED if nothing is unlocked, or a littlefs error. */
int lfsvol_format(uint32_t *base, uint32_t *size, uint32_t *block_size);

/* --------------------------------------------------------------- messages */

/* A short description of a negative error code, for messages. Never NULL.
 * Accepts both littlefs codes and LFSVOL_ERR_LOCKED. */
const char *lfsvol_strerror(int err);

#endif /* RAMTOOL_LFSVOL_H */