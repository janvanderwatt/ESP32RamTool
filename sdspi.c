/* SD v1/v2 SPI block device for FatFs. MMC and SDUC are not supported. */
#include "sdspi.h"
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include <limits.h>

static int configured, ready, block_addressed, writable;
static uint32_t sectors;
#define xchg sdport_exchange
static void release(void)
{
    sdport_select(0);
    (void)xchg(0xff); /* Allow the card to release MISO. */
}
static int wait_ready(void)
{
    for (unsigned i = 0; i < 25000; i++)
        if (xchg(0xff) == 0xff) return 1;
    return 0;
}
static uint8_t command(uint8_t cmd, uint32_t arg)
{
    release();
    sdport_select(1);
    if (!wait_ready()) return 0xff;
    xchg((uint8_t)(0x40 | cmd));
    xchg((uint8_t)(arg >> 24)); xchg((uint8_t)(arg >> 16));
    xchg((uint8_t)(arg >> 8)); xchg((uint8_t)arg);
    xchg(cmd == 0 ? 0x95 : cmd == 8 ? 0x87 : 0x01);
    for (unsigned i = 0; i < 10; i++) {
        uint8_t r = xchg(0xff);
        if (!(r & 0x80)) return r;
    }
    return 0xff;
}
static uint16_t crc16(uint16_t crc, uint8_t byte)
{
    crc ^= (uint16_t)byte << 8;
    for (unsigned i = 0; i < 8; i++)
        crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    return crc;
}
static int receive(uint8_t *buf, unsigned len)
{
    uint8_t token = 0xff;
    uint16_t crc = 0;
    for (unsigned i = 0; i < 10000; i++) {
        token = xchg(0xff);
        if (token != 0xff) break;
    }
    if (token != 0xfe) return 0;
    for (unsigned i = 0; i < len; i++) {
        buf[i] = xchg(0xff);
        crc = crc16(crc, buf[i]);
    }
    uint16_t actual = (uint16_t)xchg(0xff) << 8;
    actual |= xchg(0xff);
    return actual == crc;
}
int sdspi_configure(const uint8_t pins[6])
{
    if (configured) release();
    ready = writable = configured = 0;
    sectors = 0;
    if (sdport_setup(pins)) return -1;
    configured = 1;
    return 0;
}
void sdspi_lock(int unlocked) { writable = ready && unlocked; }
uint32_t sdspi_sectors(void) { return sectors; }
DSTATUS disk_status(BYTE drive)
{
    if (drive || !ready) return STA_NOINIT;
    return writable ? 0 : STA_PROTECT;
}
DSTATUS disk_initialize(BYTE drive)
{
    uint8_t csd[16], r, ocr[4];
    int v2;
    if (drive || !configured) return STA_NOINIT;
    ready = writable = 0;
    sectors = 0;
    release();
    for (unsigned i = 0; i < 10; i++) xchg(0xff);
    if (command(0, 0) != 1) goto fail;
    r = command(8, 0x1aa);
    v2 = r == 1;
    if (v2) {
        for (unsigned i = 0; i < 4; i++) ocr[i] = xchg(0xff);
        if (ocr[0] || ocr[1] || ocr[2] != 1 || ocr[3] != 0xaa) goto fail;
    } else if (r != 5) goto fail;
    for (unsigned i = 0; i < 1000; i++) {
        if (command(55, 0) > 1) goto fail;
        r = command(41, v2 ? 0x40000000u : 0);
        if (r == 0) break;
        if (r != 1) goto fail;
        release();
        sdport_delay_ms(1);
    }
    if (r != 0 || command(58, 0) != 0) goto fail;
    for (unsigned i = 0; i < 4; i++) ocr[i] = xchg(0xff);
    if (!(ocr[0] & 0x80) || !(ocr[1] & 0x30)) goto fail;
    block_addressed = v2 && (ocr[0] & 0x40);
    if (!block_addressed && command(16, 512) != 0) goto fail;
    if (command(9, 0) != 0 || !receive(csd, 16)) goto fail;
    uint64_t count;
    if ((csd[0] >> 6) == 1 && block_addressed) {
        uint32_t size = ((uint32_t)(csd[7] & 63) << 16) |
                        ((uint32_t)csd[8] << 8) | csd[9];
        count = ((uint64_t)size + 1) * 1024;
    } else if ((csd[0] >> 6) == 0 && !block_addressed) {
        unsigned read_bl = csd[5] & 15;
        unsigned mult = ((csd[9] & 3) << 1) | (csd[10] >> 7);
        uint32_t size = ((uint32_t)(csd[6] & 3) << 10) |
                        ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
        if (read_bl < 9 || read_bl > 11) goto fail;
        count = ((uint64_t)size + 1) << (mult + 2 + read_bl - 9);
        if (count > 0x800000u) goto fail; /* Byte address must fit uint32. */
    } else goto fail;
    if (!count || count > UINT32_MAX) goto fail;
    sectors = (uint32_t)count;
    ready = 1;
    release();
    return STA_PROTECT;
fail:
    release();
    return STA_NOINIT;
}
static int in_range(LBA_t sector, UINT count)
{
    return count && sector < sectors && count <= sectors - sector;
}
DRESULT disk_read(BYTE drive, BYTE *buf, LBA_t sector, UINT count)
{
    if (drive || !buf) return RES_PARERR;
    if (!ready) return RES_NOTRDY;
    if (!in_range(sector, count)) return RES_PARERR;
    for (UINT i = 0; i < count; i++, sector++, buf += 512) {
        int ok = command(17, block_addressed ? sector : sector * 512u) == 0 &&
                 receive(buf, 512);
        release();
        if (!ok) { ready = writable = 0; return RES_ERROR; }
    }
    return RES_OK;
}
DRESULT disk_write(BYTE drive, const BYTE *buf, LBA_t sector, UINT count)
{
    if (drive || !buf) return RES_PARERR;
    if (!ready) return RES_NOTRDY;
    if (!writable) return RES_WRPRT;
    if (!in_range(sector, count)) return RES_PARERR;
    for (UINT i = 0; i < count; i++, sector++, buf += 512) {
        int ok = command(24, block_addressed ? sector : sector * 512u) == 0;
        if (ok) {
            uint16_t crc = 0;
            xchg(0xff); xchg(0xfe);
            for (unsigned j = 0; j < 512; j++) {
                crc = crc16(crc, buf[j]);
                xchg(buf[j]);
            }
            xchg((uint8_t)(crc >> 8)); xchg((uint8_t)crc);
            ok = (xchg(0xff) & 0x1f) == 5;
            /* Eight clocks start programming; then wait for busy to clear. */
            int finished = wait_ready();
            ok = ok && finished;
        }
        release();
        /* CMD13's R2 catches programming and write-protect errors. */
        if (ok) {
            ok = command(13, 0) == 0;
            uint8_t status = xchg(0xff);
            ok = ok && status == 0;
            release();
        }
        if (!ok) { ready = writable = 0; return RES_ERROR; }
    }
    return RES_OK;
}
DRESULT disk_ioctl(BYTE drive, BYTE cmd, void *buf)
{
    if (drive) return RES_PARERR;
    if (!ready) return RES_NOTRDY;
    if (cmd == CTRL_SYNC) {
        sdport_select(1);
        int ok = wait_ready();
        release();
        if (!ok) { ready = writable = 0; return RES_ERROR; }
        return RES_OK;
    }
    if (!buf) return RES_PARERR;
    if (cmd == GET_SECTOR_COUNT) *(LBA_t *)buf = sectors;
    else if (cmd == GET_SECTOR_SIZE) *(WORD *)buf = 512;
    else if (cmd == GET_BLOCK_SIZE) *(DWORD *)buf = 1;
    else return RES_PARERR;
    return RES_OK;
}
