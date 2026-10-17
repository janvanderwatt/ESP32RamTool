#ifndef RAMTOOL_SDSPI_H
#define RAMTOOL_SDSPI_H
#include <stdint.h>

/* Compile-time knob: may `sd init` take a pin list to override the built-in
 * SD defaults?
 *
 * Default (1) keeps the runtime override: `sd init [sck mosi miso cs idle-cs1
 * idle-cs2]`. Define RAMTOOL_SD_PIN_ARGS=0 for a leaner payload that drops the
 * argument parser and the GPIO pad-validation table; `sd init` then always uses
 * the fixed defaults (18/23/19/2/12/32) and rejects any arguments. Nothing
 * touches the SD pins until `sd init` runs, with or without this override. */
#ifndef RAMTOOL_SD_PIN_ARGS
#define RAMTOOL_SD_PIN_ARGS 1
#endif

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
