#ifndef RAMTOOL_SDSPI_H
#define RAMTOOL_SDSPI_H
#include <stdint.h>
/* sck, mosi, miso, sd-cs, other-cs1, other-cs2 (255 disables other-cs). */
int sdspi_configure(const uint8_t pins[6]);
void sdspi_lock(int unlocked);
uint32_t sdspi_sectors(void);
/* Platform boundary: protocol tests supply a simulated card. */
int sdport_setup(const uint8_t pins[6]);
uint8_t sdport_exchange(uint8_t value);
void sdport_select(int selected);
void sdport_delay_ms(unsigned ms);
#endif
