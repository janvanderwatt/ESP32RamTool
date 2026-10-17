/* Fixed ROM ABI for classic ESP32, silicon revisions 0 through 3.
 * Verified against esp-rom-elfs 20241011 rev0 and rev300; see README.md.
 * Maintenance: python tools/verify_rom_symbols.py --help
 * Keep rom: comments: the verifier uses them to match native ROM names.
 */

PROVIDE ( esp_rom_printf = 0x40007d54 ); /* rom: ets_printf */
PROVIDE ( esp_rom_delay_us = 0x40008534 ); /* rom: ets_delay_us */
PROVIDE ( esp_rom_uart_tx_one_char = 0x40009200 ); /* rom: uart_tx_one_char */
PROVIDE ( esp_rom_uart_flush_tx = 0x40009258 ); /* rom: uart_tx_flush */
PROVIDE ( esp_rom_uart_rx_one_char = 0x400092d0 ); /* rom: uart_rx_one_char */
PROVIDE ( esp_rom_install_uart_printf = 0x40007d28 ); /* rom: ets_install_uart_printf */
PROVIDE ( esp_rom_gpio_pad_select_gpio = 0x40009fdc ); /* rom: gpio_pad_select_gpio */
PROVIDE ( esp_rom_gpio_pad_set_drv = 0x4000a11c ); /* rom: gpio_pad_set_drv */
PROVIDE ( esp_rom_gpio_connect_out_signal = 0x40009f0c ); /* rom: gpio_matrix_out */
PROVIDE ( esp_rom_software_reset_system = 0x4000824c ); /* rom: software_reset */
PROVIDE ( esp_rom_spiflash_attach = 0x40062a6c ); /* rom: spi_flash_attach */
PROVIDE ( esp_rom_spiflash_read = 0x40062ed8 ); /* rom: SPIRead */
PROVIDE ( esp_rom_spiflash_write = 0x40062d50 ); /* rom: SPIWrite */
PROVIDE ( esp_rom_spiflash_erase_area = 0x400631ac ); /* rom: SPIEraseArea */
PROVIDE ( ets_isr_mask = 0x400067fc ); /* rom: ets_isr_mask */
PROVIDE ( ets_isr_unmask = 0x40006808 ); /* rom: ets_isr_unmask */
PROVIDE ( ramtool_rom_cache_flush = 0x40009a14 ); /* rom: Cache_Flush */
PROVIDE ( memcpy = 0x4000c2c8 ); /* rom: memcpy */
PROVIDE ( memmove = 0x4000c3c0 ); /* rom: memmove */
PROVIDE ( memset = 0x4000c44c ); /* rom: memset */
PROVIDE ( memcmp = 0x4000c260 ); /* rom: memcmp */
PROVIDE ( strcpy = 0x400013ac ); /* rom: strcpy */
PROVIDE ( strlen = 0x400014c0 ); /* rom: strlen */
PROVIDE ( strcat = 0x4000c518 ); /* rom: strcat */
PROVIDE ( strchr = 0x4000c53c ); /* rom: strchr */
PROVIDE ( strcspn = 0x4000c558 ); /* rom: strcspn */
PROVIDE ( strspn = 0x4000c648 ); /* rom: strspn */
PROVIDE ( strncmp = 0x4000c5f4 ); /* rom: strncmp */
PROVIDE ( __ashldi3 = 0x4000c818 ); /* rom: __ashldi3 */
PROVIDE ( __bswapsi2 = 0x40064ae0 ); /* rom: __bswapsi2 */
PROVIDE ( __popcountsi2 = 0x40002ed0 ); /* rom: __popcountsi2 */
