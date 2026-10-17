#ifndef RAMTOOL_SDVOL_H
#define RAMTOOL_SDVOL_H
#include <stdint.h>
typedef void (*sdvol_visit_fn)(void *, const char *, uint32_t, int);
typedef void (*sdvol_emit_fn)(void *, const char *, uint32_t);
/* Return 0 on success, a positive FatFs FRESULT on failure. */
int sdvol_init(const uint8_t pins[6]);
void sdvol_off(void);
int sdvol_mounted(void);
int sdvol_writable(void);
int sdvol_unlock(int on);
int sdvol_ls(const char *path, sdvol_visit_fn visit, void *ctx);
int sdvol_cat(const char *path, sdvol_emit_fn emit, void *ctx);
int sdvol_line(const char *path, const char *text, int append);
int sdvol_rm(const char *path);
int sdvol_mkdir(const char *path);
const char *sdvol_error(int err);
#endif
