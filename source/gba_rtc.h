#ifndef GBA_RTC_H
#define GBA_RTC_H

#include <stdint.h>
#include <stdbool.h>

/* A decoded wall-clock reading from the cartridge RTC. */
typedef struct {
  uint16_t year;    /* full year, e.g. 2026 */
  uint8_t  month;   /* 1..12 */
  uint8_t  day;     /* 1..31 */
  uint8_t  hour;    /* 0..23 */
  uint8_t  minute;  /* 0..59 */
  uint8_t  second;  /* 0..59 */
} GbaRtcTime;

/* Read the GBA cartridge real-time clock (Seiko S-3511A) over the gamepak GPIO
 * port at 0x080000C4. The EZ-Flash Omega DE emulates this RTC. Returns true and
 * fills *out on a plausible reading, false if the value is out of range — which
 * is what happens if the flashcart doesn't expose its RTC to homebrew. */
bool gba_rtc_get(GbaRtcTime* out);

#endif /* GBA_RTC_H */
