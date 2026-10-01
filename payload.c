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

#include "rom.h"

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

#define LINE_MAX 64

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
        } else if (word_is(&p, "reboot") || word_is(&p, "x")) {
            cmd_reboot(p);
        } else {
            puts_("unknown. 'help'\n");
        }
    }
}
