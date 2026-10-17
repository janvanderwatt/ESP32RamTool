/* Slow, mode-0 software SPI: no RTOS, DMA, flash, or IDF driver dependency. */
#include "rom.h"
#include "sdspi.h"

static uint8_t bus[4];
static void level(unsigned pin, int high)
{
    uintptr_t reg = GPIO_BASE + (pin < 32 ? (high ? 0x08 : 0x0c)
                                        : (high ? 0x14 : 0x18));
    *(volatile uint32_t *)reg = 1u << (pin & 31);
}
#if RAMTOOL_SD_PIN_ARGS
static int valid(unsigned pin)
{
    /* Exclude UART, boot GPIO0, flash/PSRAM, and nonexistent pads. Only the
     * runtime override can supply a bad pin, so this is compiled out with it. */
    return pin == 2 || pin == 4 || pin == 5 ||
           (pin >= 12 && pin <= 15) || pin == 18 || pin == 19 ||
           (pin >= 21 && pin <= 23) || (pin >= 25 && pin <= 27) ||
           pin == 32 || pin == 33;
}
#endif
static void output(unsigned pin, int high)
{
    esp_rom_gpio_pad_select_gpio((uint8_t)pin);
    esp_rom_gpio_connect_out_signal(pin, 256, 0, 0); /* GPIO_OUT_IDX */
    level(pin, high); /* Set the latch before enabling the driver. */
    *(volatile uint32_t *)(GPIO_BASE + (pin < 32 ? 0x24 : 0x30)) =
        1u << (pin & 31);
}
int sdport_setup(const uint8_t pins[6])
{
#if RAMTOOL_SD_PIN_ARGS
    /* Reject unsupported pads and duplicate assignments. The built-in defaults
     * are known good, so with the override compiled out there is nothing to
     * check and this loop disappears. */
    for (unsigned i = 0; i < 6; i++) {
        if (i >= 4 && pins[i] == 255) continue;
        if (!valid(pins[i])) return -1;
        for (unsigned j = 0; j < i; j++)
            if (pins[i] == pins[j]) return -1;
    }
#endif
    /* Deselect every slave BEFORE producing the first clock edge. */
    for (unsigned i = 3; i < 6; i++)
        if (pins[i] != 255) output(pins[i], 1);
    for (unsigned i = 0; i < 4; i++) bus[i] = pins[i];
    output(bus[0], 0);
    output(bus[1], 1);
    esp_rom_gpio_pad_select_gpio(bus[2]);
    *(volatile uint32_t *)(GPIO_BASE + (bus[2] < 32 ? 0x28 : 0x34)) =
        1u << (bus[2] & 31);
    /* IO_MUX offsets for the allowed pads, from ESP32 io_mux_reg.h.
     * Explicitly enable input and its pull-up; disable the pull-down. */
    static const uint8_t mux[34] = {
        [2]=0x40, [4]=0x48, [5]=0x6c, [12]=0x34, [13]=0x38,
        [14]=0x30, [15]=0x3c, [18]=0x70, [19]=0x74, [21]=0x7c,
        [22]=0x80, [23]=0x8c, [25]=0x24, [26]=0x28, [27]=0x2c,
        [32]=0x1c, [33]=0x20
    };
    volatile uint32_t *iomux = (volatile uint32_t *)(0x3ff49000u + mux[bus[2]]);
    *iomux = (*iomux & ~(1u << 7)) | (1u << 8) | (1u << 9);
    esp_rom_delay_us(2000);
    return 0;
}
uint8_t sdport_exchange(uint8_t value)
{
    uint8_t received = 0;
    for (unsigned bit = 0; bit < 8; bit++) {
        level(bus[1], value & 0x80);
        esp_rom_delay_us(2); /* <=250 kHz, including initialization */
        level(bus[0], 1);
        esp_rom_delay_us(2);
        received = (uint8_t)((received << 1) |
            ((*(volatile uint32_t *)(GPIO_BASE + (bus[2] < 32 ? 0x3c : 0x40))
              >> (bus[2] & 31)) & 1u));
        level(bus[0], 0);
        value <<= 1;
    }
    return received;
}
void sdport_select(int selected) { level(bus[3], !selected); }
void sdport_delay_ms(unsigned ms) { esp_rom_delay_us(ms * 1000u); }
