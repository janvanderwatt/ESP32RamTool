/* RamTool: a small command interpreter that runs from RAM on a bare ESP32.
 *
 * No FreeRTOS, no ESP-IDF, no C runtime. The host downloads this into IRAM and
 * DRAM with the ROM serial bootloader and calls the entry point. Everything
 * else is a handful of ROM function calls and register pokes.
 *
 * The design goal is a tool you can actually use over a serial console, not a
 * minimal demo: if you can type it, you can do it to the chip.
 */

#include <stdint.h>

#include "lfsvol.h"
#include "rom.h"
#include "sdvol.h"
#include "sdspi.h"

/* The ROM's printf is newlib's, so it supports the usual conversions. Keeping
 * the format strings in .rodata (which the linker script puts in IRAM) is what
 * makes them reachable. */

/* ------------------------------------------------------------------ output */

static void putch_(char c)
{
    esp_rom_uart_tx_one_char((uint8_t)c);
}

static void puts_(const char *s)
{
    while (*s) {
        putch_(*s++);
    }
}

static void print_u32(uint32_t v)
{
    char buf[11];
    int i = (int)sizeof(buf);

    buf[--i] = '\0';
    if (v == 0) {
        buf[--i] = '0';
    }
    while (v) {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    puts_(&buf[i]);
}

static void print_hex32(uint32_t v, int digits)
{
    static const char hexd[] = "0123456789abcdef";
    int i;

    for (i = digits - 1; i >= 0; i--) {
        putch_(hexd[(v >> (i * 4)) & 0xf]);
    }
}

/* ------------------------------------------------------------- line editor */

/* Sized for the longest line we want to be able to type at once. It went from
 * 64 to 200 when file writing arrived, because a 64-character line is a
 * reasonable register poke and a cramped log entry. */
#define LINE_MAX 200

static char line[LINE_MAX];
static uint32_t line_len;

/* Read one line with echo and backspace. Blocks until CR or LF.
 *
 * The payload is a RAM tool, so there is no notion of a session ending and no
 * EOF to report; the function only returns once a full line has arrived. */
static void read_line(void)
{
    line_len = 0;
    puts_("> ");

    for (;;) {
        uint8_t c = 0;

        /* Poll the FIFO register directly instead of calling the ROM.
         *
         * The ROM's uart_rx_one_char returns 0 on success and non-zero when the
         * FIFO was empty, and it reads through the ROM's own UartDev descriptor
         * rather than the hardware port. The status register's low byte is the
         * receive-FIFO count, so testing it is unambiguous. _start has already
         * detached the ROM from this UART, so there is no second reader to race.
         */
        /* Straight poll, no waiti.
         *
         * waiti would be the obvious way to idle here, and it is a trap: waiti
         * parks the core until an interrupt arrives, and _start deliberately
         * cleared the UART interrupt enable so the ROM's handler would stop
         * stealing our bytes. With no interrupt source left, waiti never returns
         * and the console dies the moment it goes idle. Poll instead. The cost is
         * a spinning core while you are at the prompt, which is the right
         * trade for a tool that has nothing else to do.
         */
        if ((*(volatile uint32_t *)UART_REG_STATUS & 0xffu) == 0u) {
            continue;
        }
        c = (uint8_t)(*(volatile uint32_t *)UART_REG_FIFO & 0xffu);

        if (c == '\r' || c == '\n') {
            putch_('\n');
            line[line_len] = '\0';
            return;
        }
        if (c == 8 || c == 127) { /* backspace */
            if (line_len) {
                line_len--;
                puts_("\b \b");
            }
            continue;
        }
        if (c >= ' ' && line_len + 1 < LINE_MAX) {
            line[line_len++] = (char)c;
            putch_((char)c);
        }
    }
}

/* ------------------------------------------------------------ tiny helpers */

/* Parse a decimal or 0x-prefixed unsigned value. Returns 0 if no digits. */
static uint32_t parse_u32(const char **pp)
{
    const char *p = *pp;
    uint32_t v = 0;
    int digits = 0;

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        while (*p) {
            uint8_t d;
            if (*p >= '0' && *p <= '9') {
                d = (uint8_t)(*p - '0');
            } else if (*p >= 'a' && *p <= 'f') {
                d = (uint8_t)(*p - 'a' + 10);
            } else if (*p >= 'A' && *p <= 'F') {
                d = (uint8_t)(*p - 'A' + 10);
            } else {
                break;
            }
            v = (v << 4) | d;
            p++;
            digits++;
        }
    } else {
        while (*p >= '0' && *p <= '9') {
            v = (v * 10u) + (uint32_t)(*p - '0');
            p++;
            digits++;
        }
    }

    *pp = p;
    return digits ? v : 0;
}

static void skip_space(const char **pp)
{
    while (**pp == ' ' || **pp == '\t') {
        (*pp)++;
    }
}

/* Test whether the input begins with `word`, ignoring case, and if so advance
 * the cursor past it and any trailing spaces.
 *
 * Dispatch matches whole words rather than the first character. A first-char
 * switch silently mis-routes anything that starts with the same letter, which
 * is how "s 2 1" (set a gpio) ended up as an unknown command while "bogus" was
 * cheerfully treated as "blink". */
static int word_is(const char **pp, const char *word)
{
    const char *p = *pp;
    const char *w = word;

    while (*w) {
        char c = *p;

        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != *w) {
            return 0;
        }
        p++;
        w++;
    }

    /* Reject a prefix match: "help" must not swallow "helper". */
    if (*p && *p != ' ' && *p != '\t') {
        return 0;
    }

    *pp = p;
    skip_space(pp);
    return 1;
}

/* ------------------------------------------------------------------- GPIO */

static int gpio_valid(uint32_t pin)
{
    return pin < 32;
}

static void gpio_write(uint32_t pin, int level)
{
    uint32_t bit;

    if (!gpio_valid(pin)) {
        return;
    }
    bit = 1u << pin;

    /* Route the pad to the plain GPIO function, i.e. make the out/enable
     * registers below actually drive the pin rather than a matrix signal. */
    esp_rom_gpio_pad_select_gpio((uint8_t)pin);

    if (level) {
        *(volatile uint32_t *)GPIO_OUT_W1TS = bit;
    } else {
        *(volatile uint32_t *)GPIO_OUT_W1TC = bit;
    }
    *(volatile uint32_t *)GPIO_ENABLE_W1TS = bit;
}

static int gpio_read(uint32_t pin)
{
    if (!gpio_valid(pin)) {
        return -1;
    }
    return (int)((*((volatile uint32_t *)GPIO_IN_REG) >> pin) & 1u);
}

/* -------------------------------------------------------------- chip facts */

static void cmd_info(void)
{
    /* MAC, assembled the way esptool does it: eFuse words 1 and 2, with the
     * leading two CRC bytes of the first word discarded. */
    uint32_t w1 = EFUSE_RD_WORD(1);
    uint32_t w2 = EFUSE_RD_WORD(2);
    uint8_t mac[6];

    mac[0] = (uint8_t)(w1 >> 8);
    mac[1] = (uint8_t)(w1);
    mac[2] = (uint8_t)(w2 >> 24);
    mac[3] = (uint8_t)(w2 >> 16);
    mac[4] = (uint8_t)(w2 >> 8);
    mac[5] = (uint8_t)(w2);

    puts_("mac  ");
    for (int i = 0; i < 6; i++) {
        print_hex32(mac[i], 2);
        putch_(i == 5 ? '\n' : ':');
    }

    /* Silicon revision, from the same bits esptool reads. */
    {
        uint32_t r3 = EFUSE_RD_WORD(3);
        uint32_t r5 = EFUSE_RD_WORD(5);
        uint32_t bit0 = (r3 >> 15) & 1u;
        uint32_t bit1 = (r5 >> 20) & 1u;
        uint32_t comb = (bit1 << 1) | bit0;
        puts_("rev  ");
        print_u32(comb);
        putch_('\n');
    }

    puts_("regs read/write with 'r <addr>' and 'w <addr> <val>'\n");
}

/* --------------------------------------------------------------- commands */

static void cmd_blink(const char *p)
{
    uint32_t pin, count, ms;

    skip_space(&p);
    pin = parse_u32(&p);
    skip_space(&p);
    count = parse_u32(&p);
    skip_space(&p);
    ms = parse_u32(&p);

    if (!ms) {
        ms = 100;
    }
    if (!count) {
        count = 10;
    }
    if (!gpio_valid(pin)) {
        puts_("pin out of range (0..31)\n");
        return;
    }

    puts_("blinking\n");
    for (uint32_t i = 0; i < count; i++) {
        gpio_write(pin, 1);
        esp_rom_delay_us(ms * 1000u);
        gpio_write(pin, 0);
        esp_rom_delay_us(ms * 1000u);
    }
}

static void cmd_gpio(const char *p)
{
    uint32_t pin;
    int level;

    skip_space(&p);
    pin = parse_u32(&p);
    if (!gpio_valid(pin)) {
        puts_("pin out of range (0..31)\n");
        return;
    }
    skip_space(&p);

    /* No second argument means a read, so "g 2" and "g 2 read" both read. */
    if (*p == '\0' || word_is(&p, "r") || word_is(&p, "read")) {
        level = gpio_read(pin);
        puts_("gpio");
        print_u32(pin);
        puts_(" = ");
        if (level < 0) {
            puts_("n/a\n");
        } else {
            print_u32((uint32_t)level);
            putch_('\n');
        }
        return;
    }

    level = (int)parse_u32(&p);
    gpio_write(pin, level);
    puts_("gpio");
    print_u32(pin);
    puts_(" -> ");
    print_u32((uint32_t)level);
    putch_('\n');
}

static void cmd_read(const char *p)
{
    uint32_t addr;

    skip_space(&p);
    addr = parse_u32(&p);
    if (!addr || (addr & 3u)) {
        puts_("need a non-zero 4-byte aligned address\n");
        return;
    }

    puts_("[");
    print_hex32(addr, 8);
    puts_("] = ");
    print_hex32(*(volatile uint32_t *)addr, 8);
    putch_('\n');
}

static void cmd_write(const char *p)
{
    uint32_t addr, val;

    skip_space(&p);
    addr = parse_u32(&p);
    skip_space(&p);
    val = parse_u32(&p);

    if (!addr || (addr & 3u)) {
        puts_("need a non-zero 4-byte aligned address\n");
        return;
    }
    *(volatile uint32_t *)addr = val;
    puts_("wrote ");
    print_hex32(val, 8);
    puts_(" to [");
    print_hex32(addr, 8);
    puts_("]\n");
}

static void cmd_delay(const char *p)
{
    uint32_t ms;

    skip_space(&p);
    ms = parse_u32(&p);
    esp_rom_delay_us(ms * 1000u);
    puts_("slept ");
    print_u32(ms);
    puts_(" ms\n");
}

static void cmd_reset(const char *p)
{
    (void)p;
    if (sdvol_mounted()) {
        puts_("unmounting SD...\n");
        sdvol_off();
    }

    puts_("resetting...\n");
    esp_rom_uart_flush_tx(0);
    esp_rom_delay_us(20000);

    /* Plain reset: the chip will boot from flash on next start. */
    esp_rom_software_reset_system();
}

static void cmd_reboot(const char *p)
{
    (void)p;
    puts_("rebooting into download mode\n");
    /* Let the line drain before the part goes away, otherwise the message is
     * lost in the reset. uart_tx_flush waits on the FIFO but not on the shift
     * register, so without the trailing delay the last couple of characters
     * still get cut. */
    esp_rom_uart_flush_tx(0);
    esp_rom_delay_us(20000);

    /* Pull IO0 low so the reset lands in the serial bootloader, then reset.
     * This is the one way out of RAM code: the ROM's normal return path would
     * boot flash, and a plain reset would too. Leaving it in the bootloader
     * means run.py can be re-run immediately without touching the board. */
    gpio_write(BOOTSTRAP_GPIO0, 0);
    esp_rom_software_reset_system();
}

/* ------------------------------------------------------------- littlefs ls */

/* lfsvol reports each entry through a callback rather than filling in an array,
 * because a directory can hold names up to LFS_NAME_MAX (255) bytes and
 * buffering a few dozen of those would cost more DRAM than the whole rest of
 * the payload. */
static void ls_visit(void *ctx, const char *name, uint32_t size, int is_dir)
{
    (void)ctx;
    puts_(is_dir ? "  [d] " : "  [f] ");
    puts_(name);
    /* Pad short names so the sizes line up, rather than trying to compute
     * column widths in two passes over a directory we cannot rewind cheaply. */
    if (!is_dir) {
        int pad = 20;
        const char *n = name;
        while (*n++) {
            pad--;
        }
        while (pad-- > 0) {
            putch_(' ');
        }
        print_u32(size);
    }
    putch_('\n');
}

static void part_visit(void *ctx, const char *label, uint32_t offset,
                       uint32_t size, uint8_t type, uint8_t subtype)
{
    (void)ctx;
    puts_("  ");
    puts_(label[0] ? label : "(unnamed)");
    puts_("  @0x");
    print_hex32(offset, 8);
    puts_("  0x");
    print_hex32(size, 8);
    puts_(type == 0x01 ? "  data sub 0x" : "  app  sub 0x");
    print_hex32(subtype, 2);
    putch_('\n');
}

static void cmd_part(void)
{
    puts_("partitions:\n");
    lfsvol_partitions(part_visit, 0);
}

static void cmd_ls(const char *p)
{
    uint32_t base = 0;
    int n;

    skip_space(&p);

    /* "@<offset>" overrides discovery and points straight at a flash address.
     * Discovery works on any sane partition table, so this is only needed for
     * a layout the table does not describe. */
    if (*p == '@') {
        p++;
        base = parse_u32(&p);
        skip_space(&p);
    }

    if (*p == '\0') {
        p = "/";
    }

    n = lfsvol_list(base, p, ls_visit, 0);
    if (n < 0) {
        puts_("lfs ls: ");
        puts_(lfsvol_strerror(n));
        puts_(" (");
        print_u32((uint32_t)(-n));
        puts_(")\n");
        return;
    }
    if (n == 0) {
        puts_("(empty)\n");
    }
}

/* Pull a path off the front of the line. Paths cannot contain spaces here,
 * because the console splits on whitespace and there is no quoting; a name with
 * a space in it is simply not addressable. Returns 0 if there was no path. */
static int take_path_token(const char **pp, char *out, int out_max)
{
    const char *p = *pp;
    int i = 0;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        return 0;
    }
    while (*p && *p != ' ' && *p != '\t') {
        if (i + 1 >= out_max) {
            return 0; /* too long to be a plausible name; refuse rather than
                        * silently truncate into a different file */
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    *pp = p;
    return 1;
}

/* Preserve the original littlefs command path rules. SD accepts absolute
 * paths, including '/' for listing, through take_path_token directly. */
static int take_path(const char **pp, char *out, int out_max)
{
    skip_space(pp);
    if (**pp == '/') return 0;
    return take_path_token(pp, out, out_max);
}

#define PATH_MAX 64

static void report(const char *what, int err)
{
    puts_(what);
    puts_(": ");
    puts_(lfsvol_strerror(err));
    puts_(" (");
    print_u32((uint32_t)(-err));
    puts_(")\n");
}

/* lfs rw [on|off|@<offset> <size>]
 *
 * Writing is gated because it is the only thing here that can destroy what is
 * on the chip. The lock lives in lfsvol's block device rather than here, so a
 * mistake in this file cannot turn into an erase. */
static void cmd_rw(const char *p)
{
    uint32_t base = 0;
    uint32_t size = 0;
    uint32_t rb;
    uint32_t rs;
    int err;

    skip_space(&p);

    if (*p == '\0') {
        if (lfsvol_write_region(&rb, &rs)) {
            puts_("writing enabled, 0x");
            print_hex32(rb, 8);
            puts_(" + 0x");
            print_hex32(rs, 8);
            putch_('\n');
        } else {
            puts_("writing locked\n");
        }
        return;
    }

    if (word_is(&p, "off")) {
        lfsvol_write_disable();
        puts_("writing locked\n");
        return;
    }

    if (!word_is(&p, "on")) {
        puts_("usage: lfs rw [on|off|@<offset> <size>]\n");
        return;
    }

    /* An explicit region has to be given a size, because lfsvol refuses to
     * unlock a region whose end is unknown. */
    if (*p == '@') {
        p++;
        base = parse_u32(&p);
        skip_space(&p);
        size = parse_u32(&p);
        if (!base || !size) {
            puts_("lfs rw on @<offset> <size> needs both\n");
            return;
        }
    }

    err = lfsvol_write_enable(base, size);
    if (err) {
        report("lfs rw on", err);
        return;
    }

    (void)lfsvol_write_region(&rb, &rs);
    puts_("writing enabled, 0x");
    print_hex32(rb, 8);
    puts_(" + 0x");
    print_hex32(rs, 8);
    puts_(". this does not survive a reboot.\n");
}

/* lfs cat <path> -- emit file content a chunk at a time. Reading needs no unlock. */
static void cat_emit(void *ctx, const char *buf, uint32_t len)
{
    uint32_t i;

    (void)ctx;
    for (i = 0; i < len; i++) {
        putch_(buf[i]);
    }
}

static void cmd_cat(const char *p)
{
    char path[PATH_MAX];
    int err;

    if (!take_path(&p, path, (int)sizeof(path))) {
        puts_("usage: lfs cat <path>\n");
        return;
    }

    err = lfsvol_cat(0, path, cat_emit, 0);
    if (err < 0) {
        report("lfs cat", err);
    }
}

/* lfs put <path> <line>  replace the file, creating it if absent
 * lfs add <path> <line>  append a line, creating the file if absent */
static void cmd_write_line(const char *p, int append)
{
    char path[PATH_MAX];
    int err;

    if (!take_path(&p, path, (int)sizeof(path))) {
        puts_(append ? "usage: lfs add <path> <line>\n" : "usage: lfs put <path> <line>\n");
        return;
    }
    if (*p == '\0') {
        puts_(append ? "lfs add needs some text\n" : "lfs put needs some text\n");
        return;
    }

    err = append ? lfsvol_add(path, p) : lfsvol_put(path, p);
    if (err) {
        report(append ? "lfs add" : "lfs put", err);
        return;
    }
    puts_(append ? "appended to " : "wrote ");
    puts_(path);
    putch_('\n');
}

static void cmd_rm(const char *p)
{
    char path[PATH_MAX];
    int err;

    if (!take_path(&p, path, (int)sizeof(path))) {
        puts_("usage: lfs rm <path>\n");
        return;
    }

    err = lfsvol_rm(path);
    if (err) {
        report("lfs rm", err);
        return;
    }
    puts_("removed ");
    puts_(path);
    putch_('\n');
}

static void cmd_sd(const char *p)
{
    int err = 0;
    char path[128];
    if (word_is(&p, "init")) {
        uint8_t pins[6] = {18, 23, 19, 2, 12, 32};
#if RAMTOOL_SD_PIN_ARGS
        if (*p) {
            for (unsigned i = 0; i < 6; i++) {
                if (*p < '0' || *p > '9') goto usage;
                uint32_t v = 0;
                do {
                    v = v * 10u + (uint32_t)(*p++ - '0');
                    if (v > 255) goto usage;
                } while (*p >= '0' && *p <= '9');
                if (*p && *p != ' ' && *p != '\t') goto usage;
                pins[i] = (uint8_t)v;
                skip_space(&p);
            }
            if (*p) goto usage;
        }
#else
        if (*p) goto usage; /* this build pins the defaults; no override */
#endif
        err = sdvol_init(pins);
        if (!err) {
            puts_("SD mounted, writing locked. pins SCK/MOSI/MISO/CS/idle-CS: ");
            for (unsigned i = 0; i < 6; i++) {
                print_u32(pins[i]); putch_(i == 5 ? '\n' : '/');
            }
            puts_("sectors: "); print_u32(sdspi_sectors()); putch_('\n');
        }
    } else if (word_is(&p, "rw")) {
        if (!*p) {
            puts_(sdvol_writable() ? "SD writing enabled\n" : "SD writing locked\n");
        } else {
            int on;
            if (word_is(&p, "on")) on = 1;
            else if (word_is(&p, "off")) on = 0;
            else goto usage;
            if (*p) goto usage;
            err = sdvol_unlock(on);
            if (!err) puts_(on ? "SD writing enabled until off/init/reboot\n" : "SD writing locked\n");
        }
    } else if (word_is(&p, "off")) {
        if (*p) goto usage;
        sdvol_off();
        puts_("SD unmounted, writing locked\n");
    } else if (word_is(&p, "ls")) {
        if (!*p) err = sdvol_ls("/", ls_visit, 0);
        else {
            if (!take_path_token(&p, path, sizeof(path)) || *p) goto usage;
            err = sdvol_ls(path, ls_visit, 0);
        }
    } else if (word_is(&p, "cat")) {
        if (!take_path_token(&p, path, sizeof(path)) || *p) goto usage;
        err = sdvol_cat(path, cat_emit, 0);
    } else {
        int op;
        if (word_is(&p, "put")) op = 0;
        else if (word_is(&p, "add")) op = 1;
        else if (word_is(&p, "rm")) op = 2;
        else if (word_is(&p, "mkdir")) op = 3;
        else goto usage;
        if (!take_path_token(&p, path, sizeof(path))) goto usage;
        if (op < 2) {
            if (!*p) goto usage;
            err = sdvol_line(path, p, op);
        } else {
            if (*p) goto usage;
            err = op == 2 ? sdvol_rm(path) : sdvol_mkdir(path);
        }
        if (!err) puts_("SD ok\n");
    }
    if (err) {
        puts_("SD: "); puts_(sdvol_error(err));
        puts_(" ("); print_u32((uint32_t)err); puts_(")\n");
    }
    return;
usage:
#if RAMTOOL_SD_PIN_ARGS
    puts_("sd init [sck mosi miso cs idle-cs1 idle-cs2] (255 skips idle-cs)\n"
          "sd ls [path] | cat <path> | put/add <path> <line>\n"
          "sd rm/mkdir <path> | rw [on|off] | off\n");
#else
    puts_("sd init (fixed pins 18/23/19/2; idle CS 12/32)\n"
          "sd ls [path] | cat <path> | put/add <path> <line>\n"
          "sd rm/mkdir <path> | rw [on|off] | off\n");
#endif
}

/* lfs format -- rewrite the superblock of the unlocked region. */
static void cmd_lfs_format(const char *p)
{
    uint32_t base = 0;
    uint32_t size = 0;
    uint32_t bsize = 0;
    int err;

    skip_space(&p);
    if (*p != '\0') {
        puts_("usage: lfs format (unlock with 'lfs rw on' first)\n");
        return;
    }

    err = lfsvol_format(&base, &size, &bsize);
    if (err) {
        report("lfs format", err);
        return;
    }

    puts_("formatted 0x");
    print_hex32(base, 8);
    puts_(" + 0x");
    print_hex32(size, 8);
    puts_(" (");
    print_u32(size / bsize);
    puts_(" blocks of ");
    print_u32(bsize);
    puts_("). writing remains unlocked.\n");
}

/* Every littlefs command lives under 'lfs', mirroring 'sd'. The old bare
 * spellings were removed rather than kept as aliases: one spelling per
 * operation is easier to document and to script against. */
static void cmd_lfs(const char *p)
{
    if (word_is(&p, "ls")) {
        cmd_ls(p);
    } else if (word_is(&p, "cat")) {
        cmd_cat(p);
    } else if (word_is(&p, "put")) {
        cmd_write_line(p, 0);
    } else if (word_is(&p, "add")) {
        cmd_write_line(p, 1);
    } else if (word_is(&p, "rm")) {
        cmd_rm(p);
    } else if (word_is(&p, "rw")) {
        cmd_rw(p);
    } else if (word_is(&p, "format")) {
        cmd_lfs_format(p);
    } else {
        puts_("lfs ls [path] | lfs ls @<offset> [path] | lfs cat <path>\n"
              "lfs put/add <path> <line> | lfs rm <path>\n"
              "lfs rw [on|off|@<offset> <size>] | lfs format (after 'lfs rw on')\n");
    }
}

static void help(void)
{
    puts_(
        "commands:\n"
        "  info                 mac, revision, notes\n"
        "  g <pin>              read a gpio\n"
        "  g <pin> <0|1>        set a gpio ('s' also works)\n"
        "  blink <pin> [n] [ms] blink a pin\n"
        "  r <addr>             read a 32-bit register\n"
        "  w <addr> <val>       write a 32-bit register\n"
        "  delay <ms>           delay\n"
        "  part                 show the partition table\n"
        "  lfs ls [path]        list a littlefs directory\n"
        "  lfs ls @<offset>     same, at an explicit flash offset\n"
        "  lfs cat <path>       print a file\n"
        "  lfs put <path> <line> replace a file with one line\n"
        "  lfs add <path> <line> append one line to a file\n"
        "  lfs rm <path>        delete a file\n"
        "  lfs rw [on|off]      unlock writing (put/add/rm/format need this)\n"
        "  lfs format           format the unlocked littlefs volume\n"
#if RAMTOOL_SD_PIN_ARGS
        "  sd init [pins]       mount SPI SD (18/23/19/2; idle CS 12/32)\n"
#else
        "  sd init              mount SPI SD (18/23/19/2; idle CS 12/32)\n"
#endif
        "  sd ls/cat/put/add/rm/mkdir  FAT files; 'sd' for usage\n"
        "  sd rw [on|off]       separate SD write unlock\n"
        "  sd off               unmount SD and lock writing\n"
        "  reset                unmount SD and reset the chip\n"
        "  reboot               reset into the download bootloader\n"
        "  help                 this text\n");
}

/* ------------------------------------------------------------------- main */

/* Never returns. Everything lives in .bss, which _start zeroes. */
void ramtool_main(void)
{
    /* The ROM has UART0 as its console already, but re-point its putchar
     * explicitly so printf cannot end up on a UART we are not reading. */
    esp_rom_install_uart_printf();
    esp_rom_uart_flush_tx(0);

    puts_("\nRamTool up. RAM only, nothing was flashed. 'help' for commands.\n");
    cmd_info();

    for (;;) {
        const char *p = line;

        read_line();
        skip_space(&p);

        /* A bare "g" reads a pin, "g <pin> <v>" writes it, and the "s" spelling
         * does the same, so both are accepted. */
        if (*p == '\0') {
            continue;
        }
        if (word_is(&p, "help") || word_is(&p, "?")) {
            help();
        } else if (word_is(&p, "info")) {
            cmd_info();
        } else if (word_is(&p, "g") || word_is(&p, "gpio") || word_is(&p, "s")) {
            cmd_gpio(p);
        } else if (word_is(&p, "b") || word_is(&p, "blink")) {
            cmd_blink(p);
        } else if (word_is(&p, "r") || word_is(&p, "read")) {
            cmd_read(p);
        } else if (word_is(&p, "w") || word_is(&p, "write")) {
            cmd_write(p);
        } else if (word_is(&p, "d") || word_is(&p, "delay")) {
            cmd_delay(p);
        } else if (word_is(&p, "part") || word_is(&p, "parts")) {
            cmd_part();
        } else if (word_is(&p, "lfs")) {
            cmd_lfs(p);
        } else if (word_is(&p, "sd")) {
            cmd_sd(p);
        } else if (word_is(&p, "reset")) {
            cmd_reset(p);
        } else if (word_is(&p, "reboot") || word_is(&p, "x")) {
            cmd_reboot(p);
        } else {
            puts_("unknown. 'help'\n");
        }
    }
}
