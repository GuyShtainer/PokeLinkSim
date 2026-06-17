#include "sys.h"

#include "flashcartio.h"
#include "flashcartio_write.h"

#if FLASHCARTIO_EZFO_ENABLE != 0
#include "ezflashomega/io_ezfo.h"
#endif

bool flashcartio_write_sector(u32 sector, const u8* source, u16 count) {
  switch (active_flashcart) {
#if FLASHCARTIO_EZFO_ENABLE != 0
    case EZ_FLASH_OMEGA: {
      /* Same guard reads use: blocks SoftReset / ROM-touching IRQs while the
       * cart is in OS mode. _EZFO_writeSectors disables IRQs internally too. */
      flashcartio_is_reading = true;
      bool success = _EZFO_writeSectors(sector, count, source);
      flashcartio_is_reading = false;
      return success;
    }
#endif
    default:
      /* Everdrive write not implemented in this project. */
      return false;
  }
}
