/* FatFs integration. All objects/buffers are static: 2 KB target stack, no heap. */
#include "sdvol.h"
#include "sdspi.h"
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include <string.h>

static FATFS fs;
static FIL file;
static DIR dir;
static FILINFO info;
static char buffer[128];
static int mounted;

void sdvol_off(void)
{
    sdspi_lock(0);
    (void)f_mount(0, "", 0);
    mounted = 0;
}
int sdvol_init(const uint8_t pins[6])
{
    sdvol_off();
    if (sdspi_configure(pins)) return FR_INVALID_PARAMETER;
    FRESULT r = f_mount(&fs, "", 1);
    mounted = r == FR_OK;
    if (!mounted) sdvol_off();
    return r;
}
int sdvol_mounted(void) { return mounted && !(disk_status(0) & STA_NOINIT); }
int sdvol_writable(void) { return sdvol_mounted() && !(disk_status(0) & STA_PROTECT); }
int sdvol_unlock(int on)
{
    if (!on) { sdspi_lock(0); return FR_OK; }
    if (!sdvol_mounted()) return FR_NOT_READY;
    sdspi_lock(1);
    return FR_OK;
}
static int finish(FRESULT result)
{
    /* Discard cached metadata after a transport failure. A fresh init is required. */
    if (result == FR_DISK_ERR || result == FR_INT_ERR || result == FR_NOT_READY)
        sdvol_off();
    return result;
}
static int check(int write)
{
    if (!sdvol_mounted()) return FR_NOT_READY;
    if (write && !sdvol_writable()) return FR_WRITE_PROTECTED;
    return FR_OK;
}
int sdvol_ls(const char *path, sdvol_visit_fn visit, void *ctx)
{
    int r = check(0);
    if (r) return r;
    r = f_opendir(&dir, path);
    if (r) return finish((FRESULT)r);
    while ((r = f_readdir(&dir, &info)) == FR_OK && info.fname[0])
        visit(ctx, info.fname, (uint32_t)info.fsize, !!(info.fattrib & AM_DIR));
    FRESULT closed = f_closedir(&dir);
    return finish(r ? (FRESULT)r : closed);
}
int sdvol_cat(const char *path, sdvol_emit_fn emit, void *ctx)
{
    int r = check(0);
    if (r) return r;
    r = f_open(&file, path, FA_READ);
    if (r) return finish((FRESULT)r);
    for (;;) {
        UINT got = 0;
        r = f_read(&file, buffer, sizeof(buffer), &got);
        if (r || !got) break;
        emit(ctx, buffer, got);
    }
    FRESULT closed = f_close(&file);
    return finish(r ? (FRESULT)r : closed);
}
int sdvol_line(const char *path, const char *text, int append)
{
    int r = check(1);
    UINT wrote;
    if (r) return r;
    r = f_open(&file, path, FA_WRITE | (append ? FA_OPEN_APPEND : FA_CREATE_ALWAYS));
    if (r) return finish((FRESULT)r);
    UINT len = (UINT)strlen(text);
    r = f_write(&file, text, len, &wrote);
    /* FatFs reports a full disk as a short successful write. */
    if (!r && wrote != len) r = FR_DENIED;
    if (!r) {
        r = f_write(&file, "\n", 1, &wrote);
        if (!r && wrote != 1) r = FR_DENIED;
    }
    /* f_close includes f_sync; its error must not be hidden. */
    FRESULT closed = f_close(&file);
    /* A failed transport may make close return INVALID_OBJECT; preserve the
     * original I/O error so finish invalidates the mount and reports the cause. */
    if (r && r != FR_DENIED) return finish((FRESULT)r);
    return finish(closed ? closed : (FRESULT)r);
}
int sdvol_rm(const char *path)
{
    int r = check(1);
    return r ? r : finish(f_unlink(path));
}
int sdvol_mkdir(const char *path)
{
    int r = check(1);
    return r ? r : finish(f_mkdir(path));
}
const char *sdvol_error(int err)
{
    switch (err) {
    case FR_DISK_ERR: return "card I/O failed; run sd init again";
    case FR_INT_ERR: return "invalid filesystem state; run sd init again";
    case FR_NOT_READY: return "card not ready; run sd init (check wiring/power)";
    case FR_NO_FILE: return "file not found";
    case FR_NO_PATH: return "directory not found";
    case FR_INVALID_NAME: return "invalid path or filename";
    case FR_DENIED: return "disk/directory full, read-only file, or nonempty directory";
    case FR_EXIST: return "already exists";
    case FR_WRITE_PROTECTED: return "SD writing locked; use sd rw on";
    case FR_NO_FILESYSTEM: return "no FAT12/16/32 volume (exFAT unsupported)";
    case FR_INVALID_PARAMETER: return "invalid or conflicting GPIO pins";
    default: return "FatFs error";
    }
}
