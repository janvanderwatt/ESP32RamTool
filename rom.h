/* ROM entry points and peripheral addresses for the classic ESP32.
 *
 * Every address here was read out of the ROM ELFs that ship with ESP-IDF
 * (C:\Espressif\tools\esp-rom-elfs\20241011\esp32_rev0_rom.elf and
 * esp32_rev300_rom.elf) and cross-checked against
 * components/esp_rom/esp32/ld/esp32.rom.ld. Both revisions agree on all of
 * them, so one table serves the whole family.
 *
 * The functions declared with the esp_rom_ prefix are resolved by the linker
 * from esp32.rom.api.ld. The few ROM symbols with no PROVIDE anywhere are
 * declared here with their literal addresses instead.
 */

#ifndef RAMTOOL_ROM_H
#define RAMTOOL_ROM_H

#include <stdint.h>

/* ---- resolved by the linker from esp32.rom.api.ld ---- */

extern void esp_rom_printf(const char *fmt, ...);
extern void esp_rom_delay_us(uint32_t us);
extern void esp_rom_uart_tx_one_char(uint8_t c);
extern void esp_rom_uart_flush_tx(uint8_t uart_no);
extern void esp_rom_install_uart_printf(void);

/* Non-blocking: returns non-zero and stores the byte in *c if one was waiting,
 * zero if the FIFO was empty. That is what lets the console poll without
 * spinning on a blocking read. */
extern uint8_t esp_rom_uart_rx_one_char(uint8_t *c);
extern void esp_rom_gpio_pad_select_gpio(uint8_t gpio_num);
extern void esp_rom_gpio_pad_set_drv(uint8_t gpio_num, uint8_t drv);
extern void esp_rom_software_reset_system(void);

/* SPI flash access, used to read a littlefs volume off the chip.
 *
 * esp_rom_spiflash_attach is resolved from esp32.rom.api.ld. esp_rom_spiflash_read
 * is the ROM's SPIRead and is NOT exported by any ESP-IDF linker script, so
 * ramtool.ld gives it its literal address.
 *
 * Mind the near-identical esp_rom_spiflash_read_user_cmd: that is a raw
 * single-command reader taking (status *, cmd), not a block read. Handing it
 * block arguments does not fault, it hangs. */
extern void esp_rom_spiflash_attach(uint32_t ishspi, int legacy);
extern int esp_rom_spiflash_read(uint32_t src_addr, uint32_t *dest, int32_t len);

/* UART receive under its raw ROM name, for cross-reference. The linker resolves
 * esp_rom_uart_rx_one_char to the same 0x400092d0. */
#define ROM_UART_RX_ONE_CHAR_NAME uart_rx_one_char

/* Interrupt masking, added by ramtool.ld. */
extern void ets_isr_mask(uint32_t level);   /* 0x400067fc */
extern void ets_isr_unmask(uint32_t level); /* 0x40006808 */

/* Cross-references for the ROM ELFs, in case you need to call them directly. */
#define ROM_ETS_PRINTF            0x40007d54
#define ROM_UART_TX_SWITCH        0x40009028
#define ROM_UART_TX_ONE_CHAR      0x40009200
#define ROM_UART_RX_ONE_CHAR      0x400092d0
#define ROM_UART_GET_DEVICE       0x40009598
#define ROM_ETS_DELAY_US          0x40008534
#define ROM_ETS_GET_CPU_FREQ      0x4000855c
#define ROM_ETS_INSTALL_PRINTF    0x40007d28
#define ROM_SOFTWARE_RESET_SYSTEM 0x4000824c
#define ROM_SOFTWARE_RESET_CPU    0x40008264
#define ROM_RTC_GET_RESET_REASON  0x400081d4
#define ROM_CACHE_FLUSH           0x40009a14
#define ROM_GPIO_OUTPUT_SET       0x40009b24
#define ROM_GPIO_INPUT_GET        0x40009b88
#define ROM_GPIO_MATRIX_OUT       0x40009f0c
#define ROM_GPIO_PAD_SELECT_GPIO  0x40009fdc
#define ROM_SPIREAD               0x40062ed8
#define ROM_SPIFLASH_ATTACH       0x40062a6c

/* ---- peripherals, from components/soc/esp32/ld/esp32.peripherals.ld ---- */

#define UART0_BASE                0x3FF40000
#define GPIO_BASE                 0x3FF44000
#define RTCCNTL_BASE              0x3FF48000
#define EFUSE_RD_BASE             0x3FF5A000

/* UART0 */
#define UART_REG_FIFO             (UART0_BASE + 0x00)
#define UART_REG_INT_CLR          (UART0_BASE + 0x10)
#define UART_REG_CLKDIV           (UART0_BASE + 0x14)
#define UART_REG_STATUS           (UART0_BASE + 0x1C)
#define UART_REG_CONF0            (UART0_BASE + 0x20)
#define UART_CONF0_UART_EN        (1u << 0)
#define UART_STATUS_RXD_READY     (1u << 0)

/* GPIO */
#define GPIO_OUT_REG              (GPIO_BASE + 0x04)
#define GPIO_OUT_W1TS             (GPIO_BASE + 0x08)
#define GPIO_OUT_W1TC             (GPIO_BASE + 0x0C)
#define GPIO_ENABLE_REG           (GPIO_BASE + 0x20)
#define GPIO_ENABLE_W1TS          (GPIO_BASE + 0x24)
#define GPIO_ENABLE_W1TC          (GPIO_BASE + 0x28)
#define GPIO_IN_REG               (GPIO_BASE + 0x3C)
#define GPIO_PIN_REG(n)           (GPIO_BASE + 0x88 + (n) * 4)

/* eFuse read-only window, word indexed. */
#define EFUSE_RD_WORD(n)          (*(volatile uint32_t *)(EFUSE_RD_BASE + 4 * (n)))

/* Strapping pin, for the reset-into-download-mode command. */
#define BOOTSTRAP_GPIO0           0

#endif /* RAMTOOL_ROM_H */
