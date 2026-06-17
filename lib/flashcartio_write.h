#ifndef FLASHCARTIO_WRITE_H
#define FLASHCARTIO_WRITE_H

#include "sys.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Write `count` 512-byte sectors from `source` to LBA `sector`.
 * Returns true on success.
 *
 * Backed by _EZFO_writeSectors() (EZ-Flash Omega / Omega DE). The Everdrive
 * write path is intentionally not wired here; this project targets the Omega DE.
 *
 * NOTE: like reads, a write briefly puts the cart in OS mode where ROM is
 * inaccessible, so the underlying routine runs from EWRAM with IRQs disabled.
 */
bool flashcartio_write_sector(u32 sector, const u8* source, u16 count);

#ifdef __cplusplus
}
#endif

#endif /* FLASHCARTIO_WRITE_H */
