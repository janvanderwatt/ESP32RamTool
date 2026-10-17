/* Byte-level SD simulator + real FatFs integration. No physical card is written.
 * Native: cc -std=c11 -I. tests/sd_test.c sdspi.c sdvol.c fatfs/ff.c
 *             fatfs/ffunicode.c -o build/sd_test && build/sd_test
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdspi.h"
#include "sdvol.h"
#include "fatfs/ff.h"
#include "fatfs/diskio.h"

static const uint8_t pins[6] = {18,23,19,2,12,32};
static uint8_t *image, reply[600], packet[6], writebuf[514];
static unsigned capacity, head, tail, cmdlen, writepos, writes, commands;
static uint32_t target;
static int selected, idle, legacy, writing, bad_crc, reject_write, stuck_busy;
static int missing, programming_error, bad_echo, init_stuck, read_error;
static int busy_after_write;
static uint32_t last_arg;
static unsigned last_cmd;
static uint16_t crc16(const uint8_t *data, unsigned len)
{
    uint16_t crc = 0;
    while (len--) {
        crc ^= (uint16_t)*data++ << 8;
        for (unsigned b=0; b<8; b++)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}
static void push(uint8_t value) { assert(tail < sizeof(reply)); reply[tail++] = value; }
static void data_reply(const uint8_t *data, unsigned len)
{
    push(0xff); push(0xfe);
    for (unsigned i=0; i<len; i++) push(data[i]);
    uint16_t crc = crc16(data, len);
    if (bad_crc) crc ^= 1;
    push((uint8_t)(crc >> 8)); push((uint8_t)crc);
}
int sdport_setup(const uint8_t *p)
{
    selected = cmdlen = head = tail = writing = 0;
    return p[0] == 255 ? -1 : 0;
}
void sdport_select(int on)
{
    selected = on;
    if (!on) { cmdlen = head = tail = writing = 0; }
}
void sdport_delay_ms(unsigned ms) { (void)ms; }
uint8_t sdport_exchange(uint8_t value)
{
    if (!selected || missing) return 0xff;
    if (head < tail) return reply[head++];
    head = tail = 0;
    if (stuck_busy) return 0;
    if (writing) {
        if (writing == 1) {
            if (value == 0xfe) { writing = 2; writepos = 0; }
        } else {
            writebuf[writepos++] = value;
            if (writepos == 514) {
                writing = 0;
                uint16_t crc = crc16(writebuf, 512);
                assert(writebuf[512] == (crc >> 8) && writebuf[513] == (crc & 255));
                if (!reject_write) { memcpy(image + target*512u, writebuf, 512); writes++; }
                push(reject_write ? 0x0b : 5);
                push(0); push(0); push(0xff);
                if (busy_after_write) { tail--; stuck_busy=1; }
            }
        }
        return 0xff;
    }
    if (!cmdlen && (value & 0xc0) != 0x40) return 0xff;
    packet[cmdlen++] = value;
    if (cmdlen == 6) {
        cmdlen = 0;
        unsigned cmd = packet[0] & 63;
        uint32_t arg = ((uint32_t)packet[1]<<24) | ((uint32_t)packet[2]<<16) |
                       ((uint32_t)packet[3]<<8) | packet[4];
        last_cmd = cmd; last_arg = arg; commands++;
        switch (cmd) {
        case 0: assert(packet[5] == 0x95); idle = 1; push(1); break;
        case 8:
            assert(packet[5] == 0x87);
            push(legacy ? 5 : 1);
            if (!legacy) { push(0); push(0); push(1); push(bad_echo ? 0 : 0xaa); }
            break;
        case 55: push(idle ? 1 : 0); break;
        case 41:
            assert(arg == (legacy ? 0 : 0x40000000u));
            if (!init_stuck) idle = 0;
            push(idle ? 1 : 0); break;
        case 58: push(0); push(legacy ? 0x80 : 0xc0); push(0xff); push(0x80); push(0); break;
        case 16: assert(legacy && arg == 512); push(0); break;
        case 9: {
            uint8_t csd[16] = {0};
            if (legacy) { /* 8MB SDSC: (4095+1) * 4 * 512. */
                csd[5]=9; csd[6]=3; csd[7]=255; csd[8]=0xc0;
            } else {
                uint32_t size = capacity/1024 - 1;
                csd[0]=0x40; csd[7]=(uint8_t)(size>>16);
                csd[8]=(uint8_t)(size>>8); csd[9]=(uint8_t)size;
            }
            push(0); data_reply(csd,16); break;
        }
        case 17:
            target = legacy ? arg/512 : arg;
            assert(target < capacity && (!legacy || arg%512 == 0));
            push(0);
            if (read_error) push(0x04);
            else data_reply(image + target*512u, 512);
            break;
        case 24:
            target = legacy ? arg/512 : arg;
            assert(target < capacity && (!legacy || arg%512 == 0));
            push(0); writing = 1; break;
        case 13: push(0); push(programming_error ? 0x20 : 0); break;
        default: assert(!"unexpected SD command");
        }
    }
    return 0xff;
}
static void le16(uint8_t *p, unsigned v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void le32(uint8_t *p, uint32_t v) { le16(p,v); le16(p+2,v>>16); }
static void format_image(int type, int partitioned)
{
    sdvol_off(); free(image);
    capacity = type == 12 ? 4096 : type == 16 ? 16384 : 131072;
    image = calloc(capacity,512); assert(image);
    unsigned base = partitioned ? 2048 : 0;
    if (base) {
        image[446+4] = type == 32 ? 0x0c : type == 16 ? 0x06 : 0x01;
        le32(image+446+8,base); le32(image+446+12,capacity-base);
        le16(image+510,0xaa55);
    }
    uint8_t *b = image + base*512u;
    unsigned reserved = type == 32 ? 32 : 1;
    unsigned fatsize = type == 32 ? 1024 : type == 16 ? 64 : 12;
    b[0]=0xeb; b[1]=0x58; b[2]=0x90; memcpy(b+3,"RAMTEST ",8);
    le16(b+11,512); b[13]=1; le16(b+14,reserved); b[16]=2;
    le16(b+17,type == 32 ? 0 : 512);
    if (capacity-base < 65536) le16(b+19,capacity-base);
    else le32(b+32,capacity-base);
    b[21]=0xf8; le16(b+22,type == 32 ? 0 : fatsize); le16(b+510,0xaa55);
    if (type == 32) {
        le32(b+36,fatsize); le32(b+44,2); le16(b+48,0xffff); le16(b+50,0xffff);
        memcpy(b+82,"FAT32   ",8);
    } else memcpy(b+54,type == 16 ? "FAT16   " : "FAT12   ",8);
    for (unsigned f=0; f<2; f++) {
        uint8_t *fat = image + (base+reserved+f*fatsize)*512u;
        if (type == 32) { le32(fat,0x0ffffff8); le32(fat+4,0xffffffff); le32(fat+8,0x0fffffff); }
        else if (type == 16) { le16(fat,0xfff8); le16(fat+2,0xffff); }
        else { fat[0]=0xf8; fat[1]=255; fat[2]=255; }
    }
    writes = 0;
}
static char output[1024];
static unsigned outputlen, visits;
static void emit(void *ctx, const char *s, uint32_t n)
{
    (void)ctx; assert(outputlen+n < sizeof(output));
    memcpy(output+outputlen,s,n); outputlen += n; output[outputlen]=0;
}
static void visit(void *ctx, const char *name, uint32_t size, int dir)
{
    (void)ctx; (void)size; (void)dir; assert(*name); visits++;
}
static void expect_file(const char *path, const char *text)
{
    outputlen=0; assert(sdvol_cat(path,emit,0) == FR_OK); assert(!strcmp(output,text));
}
static void filesystem_test(int type, int partitioned)
{
    format_image(type,partitioned);
    assert(sdvol_init(pins) == FR_OK);
    assert(sdvol_mounted() && !sdvol_writable() && writes == 0);
    assert(sdvol_line("locked.txt","blocked",0) == FR_WRITE_PROTECTED);
    assert(sdvol_rm("missing.txt") == FR_WRITE_PROTECTED);
    assert(sdvol_mkdir("blocked") == FR_WRITE_PROTECTED && writes == 0);
    visits=0; assert(sdvol_ls("/",visit,0) == FR_OK && visits == 0 && writes == 0);
    assert(sdvol_unlock(1) == FR_OK);
    assert(sdvol_mkdir("logs") == FR_OK);
    const char *path = "logs/long-file-name.txt";
    assert(sdvol_line(path,"first",0) == FR_OK);
    assert(sdvol_line(path,"second",1) == FR_OK);
    expect_file(path,"first\nsecond\n");
    /* Crossing sector and cluster boundaries, with persistent FAT updates. */
    char text[901]; memset(text,'x',900); text[900]=0;
    assert(sdvol_line("large.txt",text,0) == FR_OK);
    assert(sdvol_init(pins) == FR_OK && !sdvol_writable());
    outputlen=0; assert(sdvol_cat("large.txt",emit,0) == FR_OK);
    assert(outputlen == 901 && output[900] == '\n');
    for (unsigned i=0;i<900;i++) assert(output[i]=='x');
    expect_file(path,"first\nsecond\n");
    assert(sdvol_unlock(1) == FR_OK);
    assert(sdvol_line(path,"replacement",0) == FR_OK);
    expect_file(path,"replacement\n");
    visits=0; assert(sdvol_ls("logs",visit,0) == FR_OK && visits == 1);
    assert(sdvol_rm(path) == FR_OK);
    assert(sdvol_cat(path,emit,0) == FR_NO_FILE);
    assert(sdvol_rm("logs") == FR_OK);
    assert(sdvol_unlock(0) == FR_OK && !sdvol_writable());
    unsigned before=writes;
    outputlen=0;
    assert(sdvol_cat("large.txt",emit,0) == FR_OK && outputlen == 901);
    assert(writes == before);
    sdvol_off(); assert(!sdvol_mounted());
    assert(sdvol_unlock(1) == FR_NOT_READY);
    printf("FAT%d %s file cycle passed\n",type,partitioned ? "MBR" : "superfloppy");
}
int main(void)
{
    assert(sdvol_line("x","y",0) == FR_NOT_READY);
    for (int mbr=0;mbr<2;mbr++) {
        filesystem_test(12,mbr); filesystem_test(16,mbr); filesystem_test(32,mbr);
    }
    format_image(16,0); legacy=1;
    assert(sdvol_init(pins) == FR_OK && sdspi_sectors() == capacity);
    assert(sdvol_unlock(1) == FR_OK && sdvol_line("SDSC.TXT","legacy",0) == FR_OK);
    expect_file("SDSC.TXT","legacy\n");
    legacy=0;
    uint8_t sector[512] = {0};
    assert(sdvol_init(pins) == FR_OK);
    unsigned before=commands;
    assert(disk_write(0,sector,0,1) == RES_WRPRT && before == commands);
    assert(sdvol_unlock(1) == FR_OK);
    assert(disk_write(0,sector,capacity,1) == RES_PARERR);
    assert(disk_write(0,sector,capacity-1,2) == RES_PARERR);
    assert(disk_read(0,sector,0,0) == RES_PARERR);
    assert(disk_read(1,sector,0,1) == RES_PARERR);
    assert(before == commands);
    assert(disk_write(0,sector,capacity-1,1) == RES_OK);
    assert(last_cmd == 13);
    assert(disk_read(0,sector,capacity-1,1) == RES_OK && last_arg == capacity-1);
    reject_write=1;
    assert(disk_write(0,sector,capacity-1,1) == RES_ERROR);
    assert(disk_status(0) & STA_NOINIT); reject_write=0;
    assert(sdvol_init(pins) == FR_OK && sdvol_unlock(1) == FR_OK);
    busy_after_write=1;
    assert(disk_write(0,sector,capacity-1,1) == RES_ERROR);
    assert(disk_status(0) & STA_NOINIT);
    busy_after_write=stuck_busy=0;
    assert(sdvol_init(pins) == FR_OK && sdvol_unlock(1) == FR_OK);
    programming_error=1;
    assert(disk_write(0,sector,capacity-1,1) == RES_ERROR); programming_error=0;
    assert(sdvol_init(pins) == FR_OK); bad_crc=1;
    assert(disk_read(0,sector,0,1) == RES_ERROR); bad_crc=0;
    assert(sdvol_init(pins) == FR_OK); read_error=1;
    assert(sdvol_cat("SDSC.TXT",emit,0) == FR_DISK_ERR && !sdvol_mounted()); read_error=0;
    assert(sdvol_init(pins) == FR_OK && sdvol_unlock(1) == FR_OK);
    reject_write=1;
    assert(sdvol_line("fail.txt","must report I/O error",0) == FR_DISK_ERR);
    assert(!sdvol_mounted() && !sdvol_writable()); reject_write=0;
    assert(sdvol_init(pins) == FR_OK); stuck_busy=1;
    assert(disk_ioctl(0,CTRL_SYNC,0) == RES_ERROR); stuck_busy=0;
    missing=1; assert(sdvol_init(pins) == FR_NOT_READY); missing=0;
    bad_echo=1; assert(sdvol_init(pins) == FR_NOT_READY); bad_echo=0;
    init_stuck=1; assert(sdvol_init(pins) == FR_NOT_READY); init_stuck=0;
    bad_crc=1; assert(sdvol_init(pins) == FR_NOT_READY); bad_crc=0;
    format_image(16,0); memset(image,0,512);
    assert(sdvol_init(pins) == FR_NO_FILESYSTEM && writes == 0);
    /* Full FAT16: ensure a short f_write cannot be reported as success. */
    format_image(16,0);
    for (unsigned f=0;f<2;f++)
        memset(image+(1+f*64)*512u,0xff,64*512u);
    assert(sdvol_init(pins) == FR_OK && sdvol_unlock(1) == FR_OK);
    assert(sdvol_line("full.txt","no space",0) == FR_DENIED);
    sdvol_off(); free(image);
    puts("SD protocol, bounds, lock, CRC, full disk, and failure tests passed");
    return 0;
}
