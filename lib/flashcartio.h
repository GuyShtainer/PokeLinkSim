#ifndef FLASHCARTIO_H
#define FLASHCARTIO_H

#include <stdbool.h>
#include "fatfs/ff.h"

typedef enum { NO_FLASHCART, EVERDRIVE_GBA_X5, EZ_FLASH_OMEGA } ActiveFlashcart;

extern ActiveFlashcart active_flashcart;
extern volatile bool flashcartio_is_reading;

bool flashcartio_activate(void);
bool flashcartio_read_sector(unsigned int sector,
                             unsigned char* destination,
                             unsigned short count);

/* Reboot toward the flashcart loader/menu (EZ-Flash Omega) or OS (EverDrive),
 * else a BIOS SoftReset. Does not return. Gated on !flashcartio_is_reading.
 * EXPERIMENTAL: landing spot is HW-dependent (Omega DE confirmed -> kernel menu). */
void flashcartio_reboot(void);

#endif  // FLASHCARTIO_H
