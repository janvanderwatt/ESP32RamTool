/* littlefs, read-only, over the ROM's SPI flash reader.
 *
 * See lfsvol.h for what this is for. This file is mostly notes; the code is
 * small because littlefs needs so little from the host.
 *
 * ---------------------------------------------------------------------------
 * Trap 1: lfs_crc is NOT esp_rom_crc32_le
 * ---------------------------------------------------------------------------
 * It is very tempting to alias littlefs's checksum onto the ROM's CRC helper.
 * Do not. They compute different values.
 *
 *   littlefs's lfs_crc  a 16-entry nibble-table CRC-32 (reflected polynomial
 *                      0xedb88320) that never inverts anything. The seed goes
 *                      in raw and the result comes out raw.
 *
 *   esp_rom_crc32_le   the same polynomial driven a byte at a time, but in
 *                      zlib style: it inverts the seed on entry and inverts the
 *                      result on exit.
 *
 * The difference is not subtle enough to survive a checksum, and it is not
 * subtle enough to notice by inspection either. What it looks like from the
 * outside is that every metadata block fails verification, so lfs_dir_fetchmatch
 * rejects both halves of the superblock pair and reports exactly this:
 *
 *   "Corrupted dir pair at {0x0, 0x1}"
 *
 * which says nothing about checksums. The fix is to implement lfs_crc
 * properly, which is what the table below does. Verified against the on-flash
 * CCRC tags of a real image: the no-invert form reproduces both superblock
 * copies, the ROM form reproduces neither.
 *
 * ---------------------------------------------------------------------------
 * Trap 2: lfs_dir_read's return convention is inverted from the usual
 * ---------------------------------------------------------------------------
 * lfs_dir_read returns a POSITIVE value on success, 0 at end of directory, and
 * a negative value on error. The reflex `while ((err = lfs_dir_read(...)) == 0)`
 * stops before reading anything, and `if (err) { error }` fires on the very
 * first, successful, read. Check the sign explicitly.
 *
 * ---------------------------------------------------------------------------
 * Trap 3: on-flash tags are big-endian and XOR-chained
 * ---------------------------------------------------------------------------
 * Only relevant when reading a raw image without littlefs, but it makes hand
 * decoding produce confident nonsense. Each tag is stored big-endian and XORed
 * with the previous tag, the chain seeded at 0xffffffff:
 *
 *     tag = frombe32(raw) ^ ptag
 *
 * so decoding the bytes as little-endian words yields plausible-looking values
 * that do not correspond to the superblock at all. A size field that appears to
 * say 0x00100000 is really block_size 4096 once the chain is undone.
 *
 * ---------------------------------------------------------------------------
 * Buffers
 * ---------------------------------------------------------------------------
 * lfs_mount uses cfg->read_buffer and cfg->prog_buffer at cfg->cache_size bytes
 * each -- NOT at block_size. Sizing them at block_size, which looks safer and is
 * what a first reading of the docs suggests, costs 8 KiB of DRAM here for
 * nothing.
 *
 * prog_buffer must be supplied even though nothing is ever written, because with
 * LFS_NO_MALLOC lfs_mount asserts on a NULL one. It can be tiny.
 */

#include <stddef.h>
#include <stdint.h>

#include "lfs.h"
#include "lfs_util.h"
#include "lfsvol.h"
#include "rom.h"

/* --------------------------------------------------------------------------
 * The CRC littlefs expects from the host.
 *
 * lfs.h declares this; the definition normally lives in lfs_util.c, which is
 * not compiled here. This is a verbatim copy of the upstream algorithm,
 * including the absence of any inversion. See Trap 1 above before touching it.
 * ------------------------------------------------------------------------ */
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

/* --------------------------------------------------------------------------
 * Block device, and the write lock
 *
 * Reading is unrestricted. Writing is not, and the restriction is here rather
 * than in the command layer on purpose: this is the only code in the payload
 * that can destroy the contents of the chip, so it should not depend on every
 * caller remembering to check first.
 *
 * Two things must both hold before a byte is programmed or a sector erased:
 *
 *   1. lfsvol_write_enable has been called, naming a region. It lives in .bss,
 *      so it does not survive a reboot -- re-locking is the default.
 *
 *   2. The address falls inside that region. This is not belt and braces, it is
 *      the important half. littlefs takes block_count from the superblock,
 *      because lfs_mount is handed block_count 0 so it will read the real value
 *      off the disk. That is convenient for mounting and dangerous for writing:
 *      a corrupt or hostile superblock claiming block_count 100000 at
 *      block_size 4096 describes 400 MB, and every one of those blocks would
 *      otherwise be a legal erase. Clamping to the partition size the scan
 *      actually reported keeps a bad superblock from reaching past the volume.
 *
 *      This is also why an explicit offset cannot simply be unlocked on its
 *      own: with no partition size there is nothing to clamp against, so
 *      lfsvol_write_enable refuses that combination rather than guessing.
 * ------------------------------------------------------------------------ */

/* The unlocked region. wr_size of 0 means locked, which also makes "unknown
 * size" unrepresentable -- see lfsvol_write_enable. */
static uint32_t wr_base;
static uint32_t wr_size;
static lfs_size_t wr_block_size;

static int write_permitted(uint32_t addr, uint32_t len)
{
    if (wr_size == 0) {
        return 0;
    }
    if (addr < wr_base) {
        return 0;
    }
    /* Written as a subtraction so a huge len cannot wrap the comparison. */
    if (len > (wr_base + wr_size) - addr) {
        return 0;
    }
    return 1;
}

static int bd_read(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, void *dst, lfs_size_t len)
{
    uint32_t addr = (uint32_t)c->context + (uint32_t)block * c->block_size + off;

    /* esp_rom_spiflash_read is the ROM's SPIRead. Note the trap here too: the
     * similarly named esp_rom_spiflash_read_user_cmd is a raw single-command
     * reader taking (status*, cmd) and is nothing like a block read. Calling it
     * with block arguments does not fault, it just hangs. The linker script
     * supplies the address because esp32.rom.ld does not PROVIDE it. */
    if (esp_rom_spiflash_read(addr, (uint32_t *)dst, (int32_t)len) != 0) {
        return LFS_ERR_IO;
    }
    return 0;
}

static int bd_prog(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, const void *src, lfs_size_t len)
{
    uint32_t addr = (uint32_t)c->context + (uint32_t)block * c->block_size + off;

    if (!write_permitted(addr, (uint32_t)len)) {
        return LFS_ERR_IO;
    }
    /* The ROM wants a uint32_t source. littlefs hands over a whole number of
     * prog_size units, 16 here, so the buffer is always 4-byte aligned and the
     * length always a multiple of 4. */
    if (esp_rom_spiflash_write(addr, (const uint32_t *)src, (int32_t)len) != 0) {
        return LFS_ERR_IO;
    }
    return 0;
}

static int bd_erase(const struct lfs_config *c, lfs_block_t block)
{
    uint32_t addr = (uint32_t)c->context + (uint32_t)block * c->block_size;

    if (!write_permitted(addr, (uint32_t)c->block_size)) {
        return LFS_ERR_IO;
    }
    /* erase_area takes an address and a length, not a sector number. Assuming
     * block_size == sector size, which holds for the volumes seen here. A
     * volume with sub-sector blocks would need the containing sector erased,
     * and would need write_permitted widened to match. */
    if (esp_rom_spiflash_erase_area(addr, (uint32_t)c->block_size) != 0) {
        return LFS_ERR_IO;
    }
    return 0;
}

static int bd_sync(const struct lfs_config *c)
{
    (void)c;
    return 0; /* nothing is buffered */
}

/* --------------------------------------------------------------------------
 * Configuration and storage
 *
 * cache_size must be a non-zero multiple of both read_size and prog_size, and
 * must divide block_size. Zero is not "no cache", it is a configuration
 * littlefs rejects, and rejecting it looks like a hang rather than an error.
 *
 * block_count is left at 0 on purpose: lfs_mount reads the real value out of
 * the superblock in that case, so there is no need to know the partition size
 * here or to keep the two in step. block_size cannot be defaulted the same way,
 * because a mismatch is a hard LFS_ERR_INVAL, so it is guessed per attempt.
 * ------------------------------------------------------------------------ */

#define LFSVOL_CACHE_SIZE      128
#define LFSVOL_READ_SIZE       16
#define LFSVOL_PROG_SIZE       16
#define LFSVOL_LOOKAHEAD_SIZE  16

static uint8_t lfsvol_read_buf[LFSVOL_CACHE_SIZE];
static uint8_t lfsvol_prog_buf[LFSVOL_CACHE_SIZE];
static uint8_t lfsvol_lookahead_buf[LFSVOL_LOOKAHEAD_SIZE];

static struct lfs_config lfsvol_cfg;
static lfs_t lfsvol_fs;

/* Block sizes to try, in order. 4096 is what mklittlefs and the Arduino ESP32
 * core use; 512 is the other common choice and is what a very small
 * partition would end up with. */
static const lfs_size_t lfsvol_block_sizes[] = { 4096, 512, 1024, 8192, 16384 };

/* --------------------------------------------------------------------------
 * Partition table
 *
 * The table lives at 0x8000, which is a ROM convention rather than anything
 * this payload has to be told. Each entry is 32 bytes and starts with a 16-bit
 * magic of 0x50AA, stored little-endian, so the first two bytes on the wire are
 * AA 50. The scan stops at the first entry without the magic.
 *
 * Partition type 0x01 is "data". The subtype matters for guessing where a
 * filesystem is, but not much: PlatformIO in particular writes a littlefs image
 * into the partition its table labels SPIFFS, because that is the subtype its
 * own tooling expects to find. So the label and the subtype are both treated as
 * hints and the real test is whether littlefs mounts.
 * ------------------------------------------------------------------------ */

#define PARTITION_TABLE_OFFSET  0x8000
#define PARTITION_ENTRY_SIZE    32
#define PARTITION_MAGIC         0x50AA
#define PARTITION_TYPE_DATA     0x01
#define PART_TABLE_MAX_ENTRIES  32

/* Subtypes from esp_partition.h. LITTLEFS is 0x83 and SPIFFS is 0x82. */
#define PARTITION_SUBTYPE_DATA_OTA      0x00
#define PARTITION_SUBTYPE_DATA_NVS      0x02
#define PARTITION_SUBTYPE_DATA_COREDUMP 0x03
#define PARTITION_SUBTYPE_SPIFFS        0x82
#define PARTITION_SUBTYPE_DATA_LITTLEFS 0x83

struct lfsvol_part {
    uint32_t offset;
    uint32_t size;
    uint8_t type;
    uint8_t subtype;
    char label[17];
};

/* One 32-byte entry at a time, rather than slurping the whole 4 KiB region, to
 * keep this out of .bss.
 *
 * Typed as uint32_t rather than uint8_t on purpose. The ROM reader takes a
 * uint32_t *, and a static uint8_t array has no alignment guarantee, so relying
 * on the compiler happening to place a 32-byte object on a 4-byte boundary is
 * not a thing to leave to chance. Indexed through a uint8_t pointer below. */
static uint32_t part_raw[PARTITION_ENTRY_SIZE / 4];

/* The same bytes, read as characters. */
#define PART_RAW ((const uint8_t *)part_raw)

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* The magic is a 16-bit field occupying only the first two bytes; type and
 * subtype follow in the same 32-bit word. Reading it with rd32le gives
 * 0x020150aa for the first nvs entry and never matches. */
static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* Collect every entry in the table into `out`, not just the data ones: 'part'
 * should show the table as it actually is, and filtering belongs to the caller.
 * Returns how many were found. */
static int part_scan(struct lfsvol_part *out, int max)
{
    int n = 0;
    uint32_t base = PARTITION_TABLE_OFFSET;
    int i;

    for (i = 0; i < PART_TABLE_MAX_ENTRIES && n < max; i++) {
        uint32_t entry = base + (uint32_t)i * PARTITION_ENTRY_SIZE;
        int j;

        if (esp_rom_spiflash_read(entry, part_raw, PARTITION_ENTRY_SIZE) != 0) {
            break;
        }
        if (rd16le(PART_RAW) != PARTITION_MAGIC) {
            break; /* end of the table */
        }

        out[n].type = PART_RAW[2];
        out[n].subtype = PART_RAW[3];
        out[n].offset = rd32le(&PART_RAW[4]);
        out[n].size = rd32le(&PART_RAW[8]);
        for (j = 0; j < 16; j++) {
            char c = (char)PART_RAW[12 + j];
            if (c == '\0') {
                break;
            }
            out[n].label[j] = c;
        }
        out[n].label[j] = '\0';
        n++;
    }
    return n;
}

/* Is this a partition that could plausibly hold a filesystem?
 *
 * Requires type 0x01 (data), which is what excludes the application slots, and
 * then rules out the data subtypes that are known not to be filesystems. */
static int part_is_fs_candidate(const struct lfsvol_part *p)
{
    if (p->type != PARTITION_TYPE_DATA) {
        return 0;
    }
    switch (p->subtype) {
    case PARTITION_SUBTYPE_DATA_OTA:
    case PARTITION_SUBTYPE_DATA_NVS:
    case PARTITION_SUBTYPE_DATA_COREDUMP:
        return 0;
    default:
        return 1;
    }
}

/* Ordering for the search. Partitions declared as a filesystem come first,
 * because when one of those mounts it is almost certainly the intended one and
 * the trial-and-error below can be skipped. */
static int part_preference(const struct lfsvol_part *p)
{
    if (p->subtype == PARTITION_SUBTYPE_DATA_LITTLEFS) {
        return 0;
    }
    if (p->subtype == PARTITION_SUBTYPE_SPIFFS) {
        return 1;
    }
    return 2;
}

/* --------------------------------------------------------------------------
 * Mounting
 * ------------------------------------------------------------------------ */

static void lfsvol_cfg_defaults(uint32_t base)
{
    lfsvol_cfg.context = (void *)base; /* bd_read adds this to every address */
    lfsvol_cfg.read = bd_read;
    lfsvol_cfg.prog = bd_prog;
    lfsvol_cfg.erase = bd_erase;
    lfsvol_cfg.sync = bd_sync;
    lfsvol_cfg.read_size = LFSVOL_READ_SIZE;
    lfsvol_cfg.prog_size = LFSVOL_PROG_SIZE;
    lfsvol_cfg.block_size = 4096; /* replaced per attempt */
    lfsvol_cfg.block_count = 0;    /* 0 means "take it from the superblock" */
    lfsvol_cfg.cache_size = LFSVOL_CACHE_SIZE;
    lfsvol_cfg.lookahead_size = LFSVOL_LOOKAHEAD_SIZE;
    lfsvol_cfg.read_buffer = lfsvol_read_buf;
    lfsvol_cfg.prog_buffer = lfsvol_prog_buf;
    lfsvol_cfg.lookahead_buffer = lfsvol_lookahead_buf;
}

/* Bring the flash peripheral up. Verified unnecessary on this part -- reads
 * work before it is called -- but it costs one call and removes a variable. */
static void lfsvol_flash_init(void)
{
    static int done;
    if (!done) {
        done = 1;
        esp_rom_spiflash_attach(0, 0);
    }
}

/* Try to mount at `base` with a given block size. Returns 0 on success. */
static int lfsvol_try_mount(uint32_t base, lfs_size_t block_size)
{
    lfsvol_cfg_defaults(base);
    lfsvol_cfg.block_size = block_size;
    return lfs_mount(&lfsvol_fs, &lfsvol_cfg);
}

/* Locate a mountable volume, trying every candidate partition and block size
 * until one works.
 *
 * Nothing is left mounted: the winning offset and block size are reported and
 * the caller mounts deliberately. That matters for the write path, which has to
 * know where the volume is before it can decide whether writing is allowed, and
 * a lookup that leaves a filesystem open behind it is poor form regardless.
 *
 * `psize` receives the partition size when the volume came from the partition
 * table, and 0 when the caller named an offset directly. Only `psize` bounds
 * writes, so a 0 here is what makes an unlocked explicit offset impossible.
 *
 * Trying everything is not as wasteful as it sounds: a failed mount reads only a
 * handful of metadata blocks over SPI, and there are at most a few partitions
 * worth trying. It also means the commands work without being told where the
 * filesystem is, which matters when the partition is labelled SPIFFS but holds
 * a littlefs image.
 *
 * Returns 0, or the last error seen. */
static int lfsvol_locate(uint32_t force_base, uint32_t *out_off,
                         lfs_size_t *out_bsize, uint32_t *out_psize,
                         struct lfsvol_part *out_part)
{
    struct lfsvol_part parts[PART_TABLE_MAX_ENTRIES];
    int nbs = (int)(sizeof(lfsvol_block_sizes) / sizeof(lfsvol_block_sizes[0]));
    int n;
    int i;
    int pass;
    int err = LFS_ERR_INVAL;

    lfsvol_flash_init();

    /* An explicit base overrides discovery entirely, which makes the commands
     * usable on a layout this scan has never seen. */
    if (force_base != 0) {
        for (i = 0; i < nbs; i++) {
            if (lfsvol_try_mount(force_base, lfsvol_block_sizes[i]) == 0) {
                (void)lfs_unmount(&lfsvol_fs);
                *out_off = force_base;
                *out_bsize = lfsvol_block_sizes[i];
                *out_psize = 0; /* unknown: not from a table */
                if (out_part) {
                    out_part->offset = force_base;
                    out_part->size = 0;
                    out_part->type = 0xff;
                    out_part->subtype = 0xff; /* unknown / user supplied */
                    out_part->label[0] = '\0';
                }
                return 0;
            }
        }
        return LFS_ERR_INVAL;
    }

    n = part_scan(parts, PART_TABLE_MAX_ENTRIES);
    if (n == 0) {
        return LFS_ERR_INVAL;
    }

    /* Three ordered passes so a declared filesystem partition is tried before
     * anything else, without having to sort. */
    for (pass = 0; pass < 3; pass++) {
        for (i = 0; i < n; i++) {
            int b;

            if (!part_is_fs_candidate(&parts[i])) {
                continue;
            }
            if (part_preference(&parts[i]) != pass) {
                continue;
            }

            for (b = 0; b < nbs; b++) {
                int e = lfsvol_try_mount(parts[i].offset, lfsvol_block_sizes[b]);
                if (e == 0) {
                    (void)lfs_unmount(&lfsvol_fs);
                    *out_off = parts[i].offset;
                    *out_bsize = lfsvol_block_sizes[b];
                    *out_psize = parts[i].size;
                    if (out_part) {
                        *out_part = parts[i];
                    }
                    return 0;
                }
                err = e;
            }
        }
    }
    return err;
}

/* Mount a volume located by lfsvol_locate. */
static int lfsvol_mount_at(uint32_t off, lfs_size_t bsize)
{
    return lfsvol_try_mount(off, bsize);
}

/* --------------------------------------------------------------------------
 * Listing
 * ------------------------------------------------------------------------ */

/* "." and ".." are noise in a listing. Matched explicitly rather than by
 * comparing to the first character, so a file genuinely called ".hidden" is not
 * silently dropped. */
static int name_is_dot(const char *name)
{
    if (name[0] != '.') {
        return 0;
    }
    if (name[1] == '\0') {
        return 1;
    }
    return (name[1] == '.' && name[2] == '\0');
}

int lfsvol_list(uint32_t base, const char *path, lfsvol_visit_fn visit,
                void *ctx)
{
    struct lfs_dir dir;
    struct lfs_info info;
    struct lfsvol_part found;
    uint32_t off;
    uint32_t psize;
    lfs_size_t bsize;
    int err;
    int count = 0;

    err = lfsvol_locate(base, &off, &bsize, &psize, &found);
    if (err) {
        return err;
    }
    err = lfsvol_mount_at(off, bsize);
    if (err) {
        return err;
    }

    err = lfs_dir_open(&lfsvol_fs, &dir, path);
    if (err) {
        lfs_unmount(&lfsvol_fs);
        return err;
    }

    for (;;) {
        /* Trap 2: positive on success, 0 at the end, negative on error. */
        err = lfs_dir_read(&lfsvol_fs, &dir, &info);
        if (err < 0) {
            lfs_dir_close(&lfsvol_fs, &dir);
            lfs_unmount(&lfsvol_fs);
            return err;
        }
        if (err == 0) {
            break;
        }
        if (name_is_dot(info.name)) {
            continue;
        }
        visit(ctx, info.name, (uint32_t)info.size,
              info.type == LFS_TYPE_DIR ? 1 : 0);
        count++;
    }

    lfs_dir_close(&lfsvol_fs, &dir);
    lfs_unmount(&lfsvol_fs);
    return count;
}

/* --------------------------------------------------------------------------
 * File access
 *
 * Every entry point here mounts, does one thing, and unmounts. That is
 * wasteful in principle and right in practice: an unmounted filesystem holds no
 * state on the flash, so a command that dies partway through cannot leave the
 * volume needing repair on the next boot. There is no long-lived session to
 * lose.
 *
 * One buffer each, both cache_size, both reused:
 *   file_buf  littlefs's per-open-file cache, which lfs_file_opencfg demands
 *   io_buf    the transfer buffer between littlefs and the console
 * ------------------------------------------------------------------------ */

static uint8_t lfsvol_file_buf[LFSVOL_CACHE_SIZE];
static uint8_t lfsvol_io_buf[LFSVOL_CACHE_SIZE];

/* Open a file with our own buffer.
 *
 * lfs_file_open is not merely discouraged here, it does not exist: lfs.c guards
 * it with #ifndef LFS_NO_MALLOC, so under our flags the symbol is absent and the
 * link fails outright. lfs.h says as much -- "lfs_file_open() will fail with
 * LFS_ERR_NOMEM, thus use lfs_file_opencfg() with config.buffer set". The buffer
 * wants cache_size bytes, the same size as the mount caches.
 *
 * Zeroing the config matters: attr_count 0 is what littlefs checks to decide
 * there are no attributes to look for. */
static int lfsvol_open(lfs_file_t *f, const char *path, int flags)
{
    struct lfs_file_config fc;

    memset(&fc, 0, sizeof(fc));
    fc.buffer = lfsvol_file_buf;
    return lfs_file_opencfg(&lfsvol_fs, f, path, flags, &fc);
}

int lfsvol_cat(uint32_t base, const char *path, lfsvol_line_fn line, void *ctx)
{
    struct lfsvol_part found;
    uint32_t off;
    uint32_t psize;
    lfs_size_t bsize;
    lfs_file_t f;
    int err;
    int total = 0;

    err = lfsvol_locate(base, &off, &bsize, &psize, &found);
    if (err) {
        return err;
    }
    err = lfsvol_mount_at(off, bsize);
    if (err) {
        return err;
    }

    err = lfsvol_open(&f, path, LFS_O_RDONLY);
    if (err) {
        lfs_unmount(&lfsvol_fs);
        return err;
    }

    for (;;) {
        lfs_ssize_t n = lfs_file_read(&lfsvol_fs, &f, lfsvol_io_buf,
                                      sizeof(lfsvol_io_buf));
        if (n < 0) {
            err = (int)n;
            break;
        }
        if (n == 0) {
            break;
        }
        line(ctx, (const char *)lfsvol_io_buf, (uint32_t)n);
        total += (int)n;
    }

    (void)lfs_file_close(&lfsvol_fs, &f);
    lfs_unmount(&lfsvol_fs);
    return (err < 0) ? err : total;
}

/* --------------------------------------------------------------------------
 * The write lock
 * ------------------------------------------------------------------------ */

int lfsvol_write_enable(uint32_t base, uint32_t size)
{
    struct lfsvol_part found;
    uint32_t off;
    uint32_t psize;
    lfs_size_t bsize;
    int err;

    err = lfsvol_locate(base, &off, &bsize, &psize, &found);
    if (err) {
        /* An explicit region can still be unlocked when nothing mounts: both
         * bounds are known, so only the block geometry is assumed. That is what
         * lets 'lfs format' repair a volume too damaged to mount -- lfs_format
         * rewrites the superblock with this geometry. */
        if (base == 0 || size == 0) {
            return err;
        }
        off = base;
        psize = size;
        bsize = 4096;
    }

    /* An explicit offset with no size is refused rather than guessed. The clamp
     * in write_permitted is the only thing standing between a bad superblock
     * and an erased application, so a region with no known end is not something
     * to hand out. */
    if (base != 0) {
        if (size == 0) {
            return LFSVOL_ERR_LOCKED;
        }
        psize = size;
    }
    if (psize == 0) {
        return LFSVOL_ERR_LOCKED;
    }

    /* Round the region down to a whole number of blocks. littlefs will not
     * touch the tail, but keeping the boundary on a block edge means the clamp
     * can never disagree with littlefs about where the volume ends. */
    wr_size = (psize / bsize) * bsize;
    if (wr_size == 0) {
        return LFSVOL_ERR_LOCKED;
    }

    wr_base = off;
    wr_block_size = bsize;
    return 0;
}

void lfsvol_write_disable(void)
{
    wr_base = 0;
    wr_size = 0;
}

int lfsvol_write_region(uint32_t *base, uint32_t *size)
{
    if (base) {
        *base = wr_base;
    }
    if (size) {
        *size = wr_size;
    }
    return (wr_size != 0) ? 1 : 0;
}

/* Mount the unlocked region, or explain why not. Shared by the three mutators
 * so none of them can forget the check. */
static int lfsvol_mount_unlocked(void)
{
    if (wr_size == 0) {
        return LFSVOL_ERR_LOCKED;
    }
    return lfsvol_mount_at(wr_base, wr_block_size);
}

/* Write one line, creating the file if needed. `flags` chooses replace or
 * append. */
static int lfsvol_write_line(const char *path, const char *text, int flags)
{
    lfs_file_t f;
    lfs_ssize_t n;
    int err;

    err = lfsvol_mount_unlocked();
    if (err) {
        return err;
    }

    err = lfsvol_open(&f, path, flags | LFS_O_WRONLY | LFS_O_CREAT);
    if (err) {
        lfs_unmount(&lfsvol_fs);
        return err;
    }

    n = lfs_file_write(&lfsvol_fs, &f, text, strlen(text));
    if (n < 0) {
        err = (int)n;
    } else {
        /* One line means one newline, added here so callers cannot forget it
         * and cannot accidentally write a file with no line structure. */
        n = lfs_file_write(&lfsvol_fs, &f, "\n", 1);
        err = (n < 0) ? (int)n : 0;
    }

    /* Close either way: on the error path it is what flushes the cache and, more
     * importantly, is where littlefs would report a deferred failure. */
    {
        int cerr = lfs_file_close(&lfsvol_fs, &f);
        if (err == 0 && cerr < 0) {
            err = cerr;
        }
    }

    lfs_unmount(&lfsvol_fs);
    return err;
}

int lfsvol_put(const char *path, const char *text)
{
    return lfsvol_write_line(path, text, LFS_O_TRUNC);
}

int lfsvol_add(const char *path, const char *text)
{
    return lfsvol_write_line(path, text, LFS_O_APPEND);
}

int lfsvol_rm(const char *path)
{
    int err = lfsvol_mount_unlocked();
    if (err) {
        return err;
    }
    err = lfs_remove(&lfsvol_fs, path);
    lfs_unmount(&lfsvol_fs);
    return err;
}

/* Format the unlocked region.
 *
 * The block count is set explicitly here, unlike every mount path: lfs_format
 * writes the count it is given into the new superblock, and a zero count would
 * produce a volume whose allocator divides by zero on the next mount. The
 * geometry comes from the unlock, so format never reaches beyond it -- every
 * program and erase still goes through write_permitted.
 *
 * lfs_format deinitializes the filesystem itself, so there is nothing to
 * unmount afterwards. */
int lfsvol_format(uint32_t *base, uint32_t *size, uint32_t *block_size)
{
    uint32_t blocks;
    int err;

    if (wr_size == 0 || wr_block_size == 0) {
        return LFSVOL_ERR_LOCKED;
    }

    blocks = wr_size / (uint32_t)wr_block_size;
    if (blocks == 0) {
        return LFSVOL_ERR_LOCKED;
    }

    lfsvol_cfg_defaults(wr_base);
    lfsvol_cfg.block_size = wr_block_size;
    lfsvol_cfg.block_count = blocks;

    err = lfs_format(&lfsvol_fs, &lfsvol_cfg);
    if (err) {
        return err;
    }

    if (base) {
        *base = wr_base;
    }
    if (size) {
        *size = wr_size;
    }
    if (block_size) {
        *block_size = (uint32_t)wr_block_size;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Error strings
 *
 * littlefs's own LFS_ERROR would be the obvious route, but it expands to printf
 * and there is no printf here. A short table is smaller than a formatter and
 * says the useful thing.
 * ------------------------------------------------------------------------ */

const char *lfsvol_strerror(int err)
{
    if (err >= 0) {
        return "ok";
    }
    if (err == LFSVOL_ERR_LOCKED) {
        return "writing is locked; 'lfs rw on' first";
    }
    /* Switch on err itself, not on -err: the case labels are the negative enum
     * values, so negating here silently falls through to "unknown" for every
     * code. */
    switch (err) {
    case LFS_ERR_IO:
        return "flash read failed";
    case LFS_ERR_CORRUPT:
        return "not a littlefs volume, or metadata is damaged";
    case LFS_ERR_NOENT:
        return "no such file or directory";
    case LFS_ERR_EXIST:
        return "already exists";
    case LFS_ERR_NOTDIR:
        return "not a directory";
    case LFS_ERR_ISDIR:
        return "is a directory";
    case LFS_ERR_NOTEMPTY:
        return "directory is not empty";
    case LFS_ERR_BADF:
        return "bad file number";
    case LFS_ERR_FBIG:
        return "file too large";
    case LFS_ERR_INVAL:
        return "no filesystem found";
    case LFS_ERR_NOSPC:
        return "no space left";
    case LFS_ERR_NOMEM:
        return "no memory";
    case LFS_ERR_NOATTR:
        return "no data available";
    case LFS_ERR_NAMETOOLONG:
        return "name too long";
    default:
        return "unknown error";
    }
}

/* --------------------------------------------------------------------------
 * Partition listing
 *
 * Shares the scan with the discovery path so that what 'part' prints and what
 * 'ls' searches can never drift apart.
 * ------------------------------------------------------------------------ */

int lfsvol_partitions(lfsvol_part_fn fn, void *ctx)
{
    struct lfsvol_part parts[PART_TABLE_MAX_ENTRIES];
    int n;
    int i;

    lfsvol_flash_init();

    n = part_scan(parts, PART_TABLE_MAX_ENTRIES);
    for (i = 0; i < n; i++) {
        fn(ctx, parts[i].label, parts[i].offset, parts[i].size,
           parts[i].type, parts[i].subtype);
    }
    return n;
}