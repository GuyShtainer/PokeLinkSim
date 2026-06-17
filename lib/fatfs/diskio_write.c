/*-----------------------------------------------------------------------*/
/* Write path for the Flashcart FatFs disk module.                       */
/*                                                                       */
/* afska's diskio.c ships read-only (disk_status/initialize/read). This  */
/* file adds the three things FatFs needs once FF_FS_READONLY == 0:      */
/* disk_write, disk_ioctl (CTRL_SYNC), and get_fattime. Kept separate so */
/* the vendored diskio.c is untouched.                                   */
/*-----------------------------------------------------------------------*/

#include <string.h>

#include "../flashcartio.h"
#include "../flashcartio_write.h"
#include "../sys.h"

#include "ff.h"     /* integer types (WORD/DWORD/LBA_t) */
#include "diskio.h" /* DRESULT, command codes */
#include "gba_rtc.h" /* cartridge RTC (resolved via -Isource) */

#define ALIGNED __attribute__((aligned(4)))

/* The EZ-Flash DMA path wants a word-aligned buffer; mirror disk_read. */
static u8 EWRAM_BSS w_aligned_buff[512 * 4] ALIGNED;

/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
  (void)pdrv;

  if ((u32)buff & 0x1) {
    /* Unaligned source: stage through the aligned buffer, 4 sectors at a time. */
    for (UINT i = 0; i < count; i += 4) {
      const u16 blocks = (count - i > 4) ? 4 : (u16)(count - i);
      memcpy(w_aligned_buff, buff + i * 512, (size_t)blocks * 512);
      if (!flashcartio_write_sector((u32)sector + i, w_aligned_buff, blocks))
        return RES_ERROR;
    }
    return RES_OK;
  } else {
    return flashcartio_write_sector((u32)sector, (const u8*)buff, (u16)count)
               ? RES_OK
               : RES_ERROR;
  }
}

/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
  (void)pdrv;

  switch (cmd) {
    case CTRL_SYNC:
      /* Writes are synchronous (Write_SD_sectors blocks), nothing to flush. */
      return RES_OK;

    case GET_SECTOR_SIZE:
      *(WORD*)buff = 512;
      return RES_OK;

    case GET_BLOCK_SIZE:
      *(DWORD*)buff = 1;
      return RES_OK;

    /* GET_SECTOR_COUNT is only needed by f_mkfs, which we never call. */
    default:
      return RES_PARERR;
  }
}

/*-----------------------------------------------------------------------*/
/* Timestamp for created/modified files                                  */
/*-----------------------------------------------------------------------*/
/* Read the flashcart's RTC and pack it into the FatFs DOS datetime. Per the   */
/* "RTC only, no fallback" choice, if the cart doesn't expose its RTC we return */
/* 0 (an unset timestamp) rather than a fabricated date.                        */

DWORD get_fattime(void) {
  GbaRtcTime t;
  if (gba_rtc_get(&t)) {
    return ((DWORD)(t.year - 1980) << 25)
         | ((DWORD)t.month  << 21)
         | ((DWORD)t.day    << 16)
         | ((DWORD)t.hour   << 11)
         | ((DWORD)t.minute << 5)
         | ((DWORD)(t.second / 2));
  }
  return 0;
}
