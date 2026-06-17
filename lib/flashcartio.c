#include "sys.h"

#include "flashcartio.h"

#if FLASHCARTIO_ED_ENABLE != 0
#include "everdrivegbax5/disk.h"
#include "everdrivegbax5/everdrive.h"
#endif

#if FLASHCARTIO_EZFO_ENABLE != 0
#include "ezflashomega/io_ezfo.h"
#endif

ActiveFlashcart active_flashcart = NO_FLASHCART;
volatile bool flashcartio_is_reading = false;

bool flashcartio_activate(void) {
#if FLASHCARTIO_ED_ENABLE != 0

#if FLASHCARTIO_ED_DISABLE_IRQ != 0
  u16 ime = REG_IME;
  REG_IME = 0;
#endif

  // Everdrive GBA X5
  if (ed_init_sd_only()) {
    ed_init();
    ed_set_save_type(FLASHCARTIO_ED_SAVE_TYPE);
    bool success = diskInit() == 0;
    ed_lock_regs();
    if (!success) {
#if FLASHCARTIO_ED_DISABLE_IRQ != 0
      REG_IME = ime;
#endif

      return false;
    }

    active_flashcart = EVERDRIVE_GBA_X5;

#if FLASHCARTIO_ED_DISABLE_IRQ != 0
    REG_IME = ime;
#endif

    return true;
  }
#endif

#if FLASHCARTIO_EZFO_ENABLE != 0
  // EZ Flash Omega
  if (_EZFO_startUp()) {
    active_flashcart = EZ_FLASH_OMEGA;
    return true;
  }
#endif

  return false;
}

bool flashcartio_read_sector(u32 sector, u8* destination, u16 count) {
  switch (active_flashcart) {
#if FLASHCARTIO_ED_ENABLE != 0
    case EVERDRIVE_GBA_X5: {
#if FLASHCARTIO_ED_DISABLE_IRQ != 0
      u16 ime = REG_IME;
      REG_IME = 0;
#endif

      flashcartio_is_reading = true;
      ed_unlock_regs();
      bool success = diskRead(sector, destination, count) == 0;
      ed_lock_regs();
      flashcartio_is_reading = false;

#if FLASHCARTIO_ED_DISABLE_IRQ != 0
      REG_IME = ime;
#endif

      return success;
    }
#endif
#if FLASHCARTIO_EZFO_ENABLE != 0
    case EZ_FLASH_OMEGA: {
      flashcartio_is_reading = true;
      bool success = _EZFO_readSectors(sector, count, destination);
      flashcartio_is_reading = false;
      return success;
    }
#endif
    default:
      return false;
  }
}

// Soft-reboot toward the flashcart's loader/menu (Omega kernel) or OS (EverDrive),
// falling back to a BIOS SoftReset with no cart. Does not return. EXPERIMENTAL:
// the landing spot is hardware-dependent (Omega DE confirmed reaching the kernel menu).
void flashcartio_reboot(void) {
  if (flashcartio_is_reading) return;  // never reset mid-transfer (rule #1)
  REG_IME = 0;                         // not coming back; keep IRQs off through the reset
  switch (active_flashcart) {
#if FLASHCARTIO_EZFO_ENABLE != 0
    case EZ_FLASH_OMEGA:
      _EZFO_reboot();  // SetRompage(BOOTLOADER) + SoftReset -> kernel; no return
      return;
#endif
#if FLASHCARTIO_ED_ENABLE != 0
    case EVERDRIVE_GBA_X5:
      // activate() left the EverDrive registers LOCKED; ed_reboot writes REG_CFG,
      // ignored while locked, so unlock first or the reboot is a no-op. quick_boot=0
      // => swi 0x26 HardReset, cold-booting toward the EverDrive OS menu.
      ed_unlock_regs();
      ed_reboot(0);  // no return
      return;
#endif
    default:
      break;
  }
  asm volatile("swi 0x00" ::: "memory");  // no/unknown cart: plain BIOS SoftReset
  for (;;) {}                             // unreachable
}
