/* Reuse the simulated card and exercise the actual console parser. Compile
 * this INSTEAD of sd_test.c. Hardware and internal-flash calls are guarded
 * stubs so an accidentally dispatched command fails the test. */
#define main protocol_tests
#include "sd_test.c"
#undef main
#include "../payload.c"

void esp_rom_delay_us(uint32_t us) { (void)us; assert(0); }
void esp_rom_uart_flush_tx(uint8_t uart) { (void)uart; assert(0); }
void esp_rom_install_uart_printf(void) { assert(0); }
void esp_rom_gpio_pad_select_gpio(uint8_t gpio) { (void)gpio; assert(0); }
void esp_rom_software_reset_system(void) { assert(0); }
int lfsvol_list(uint32_t base, const char *p, lfsvol_visit_fn fn, void *ctx)
{ (void)base; (void)p; (void)fn; (void)ctx; assert(0); return -1; }
int lfsvol_cat(uint32_t base, const char *p, lfsvol_line_fn fn, void *ctx)
{ (void)base; (void)p; (void)fn; (void)ctx; assert(0); return -1; }
int lfsvol_partitions(lfsvol_part_fn fn, void *ctx)
{ (void)fn; (void)ctx; assert(0); return -1; }
int lfsvol_write_enable(uint32_t base, uint32_t size)
{ (void)base; (void)size; assert(0); return -1; }
void lfsvol_write_disable(void) { assert(0); }
int lfsvol_write_region(uint32_t *base, uint32_t *size)
{ (void)base; (void)size; assert(0); return 0; }
int lfsvol_put(const char *p, const char *text)
{ (void)p; (void)text; assert(0); return -1; }
int lfsvol_add(const char *p, const char *text)
{ (void)p; (void)text; assert(0); return -1; }
int lfsvol_rm(const char *p) { (void)p; assert(0); return -1; }
int lfsvol_format(uint32_t *base, uint32_t *size, uint32_t *block_size)
{ (void)base; (void)size; (void)block_size; assert(0); return -1; }
const char *lfsvol_strerror(int err) { (void)err; assert(0); return ""; }

static char console[4096];
static unsigned console_len;
void esp_rom_uart_tx_one_char(uint8_t c)
{
    assert(console_len + 1 < sizeof(console));
    console[console_len++] = (char)c;
    console[console_len] = 0;
}
static void command_test(const char *s, const char *expected)
{
    console_len=0; console[0]=0;
    cmd_sd(s);
    assert(strstr(console, expected));
}
int main(void)
{
    protocol_tests();
    /* protocol_tests freed the image; avoid freeing it twice in format_image. */
    image=0;
    format_image(16,0);
    command_test("init", "SD mounted");
    command_test("ls /", "");
    assert(writes == 0);
    command_test("put /TEST.TXT locked", "SD writing locked");
    assert(writes == 0);
    command_test("rw on", "SD writing enabled");
    command_test("put /TEST.TXT absolute path", "SD ok");
    command_test("cat /TEST.TXT", "absolute path\n");
    command_test("add /TEST.TXT another line", "SD ok");
    command_test("cat /TEST.TXT", "absolute path\nanother line\n");
    command_test("mkdir /logs", "SD ok");
    command_test("put /logs/a.txt nested", "SD ok");
    command_test("ls /logs", "a.txt");
    command_test("rm /logs/a.txt", "SD ok");
    command_test("rm /logs", "SD ok");
    command_test("init 18 23 19 2 12 4294967328", "sd init [");
    command_test("init 18 23 19 2 12", "sd init [");
    command_test("init 18 23 19 2 12 32 garbage", "sd init [");
    command_test("cat /TEST.TXT extra", "sd init [");
    command_test("rw off extra", "sd init [");
    assert(sdvol_writable()); /* Malformed commands cannot change lock state. */
    command_test("rw off", "SD writing locked");
    command_test("rm /TEST.TXT", "SD writing locked");
    command_test("init 18 23 19 2 12 32", "SD mounted");
    assert(!sdvol_writable());
    command_test("off", "SD unmounted");
    command_test("rw on", "card not ready");
    free(image);
    puts("SD console parser tests passed");
    return 0;
}
