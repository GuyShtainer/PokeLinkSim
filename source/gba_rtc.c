/*
 * GBA cartridge RTC (Seiko S-3511A) reader, accessed through the gamepak GPIO
 * port. This is the same RTC interface Pokémon Ruby/Sapphire/Emerald carts use,
 * and that the EZ-Flash Omega DE emulates. Protocol + timing follow the
 * hardware-proven pokeemerald `siirtc.c` (the redundant GPIO writes are the
 * settling delay the slow serial clock needs).
 *
 * GPIO (mapped in ROM space):
 *   0x080000C4 data, 0x080000C6 direction (1=output), 0x080000C8 read-enable.
 * Pin bits: 0 = SCK (clock), 1 = SIO (data), 2 = CS (chip select).
 *
 * Caveat: the Omega DE only answers this GPIO for ROMs it treats as RTC-enabled.
 * If it doesn't answer for this homebrew, the reads return garbage and the range
 * check below rejects them (gba_rtc_get returns false).
 */
#include "gba_rtc.h"

#define GPIO_DATA (*(volatile uint16_t*)0x080000C4)
#define GPIO_DIR  (*(volatile uint16_t*)0x080000C6)
#define GPIO_CTRL (*(volatile uint16_t*)0x080000C8)

#define PIN_SCK 0x1u
#define PIN_SIO 0x2u
#define PIN_CS  0x4u

/* S-3511A commands: high nibble fixed 0110, then 3-bit reg + R/W bit (1=read). */
#define CMD_STATUS_READ   0x63u  /* control/status register, 1 byte  */
#define CMD_DATETIME_READ 0x65u  /* date + time, 7 BCD bytes         */

#define STATUS_24HOUR 0x40u      /* status bit 6: 1 = 24-hour mode   */

/* Send a command byte MSB-first; SIO must be an output. */
static void write_cmd(uint8_t value) {
  for (int i = 0; i < 8; i++) {
    uint16_t b = (uint16_t)((value >> (7 - i)) & 1) << 1;  /* on SIO */
    GPIO_DATA = b | PIN_CS;            /* SCK=0, present bit  */
    GPIO_DATA = b | PIN_CS;            /* (settling delay)    */
    GPIO_DATA = b | PIN_CS;
    GPIO_DATA = b | PIN_CS | PIN_SCK;  /* SCK=1 -> clocked in */
  }
}

/* Read one byte LSB-first; SIO must be an input. */
static uint8_t read_byte(void) {
  uint8_t v = 0;
  for (int i = 0; i < 8; i++) {
    GPIO_DATA = PIN_CS;                /* SCK=0 */
    GPIO_DATA = PIN_CS;
    GPIO_DATA = PIN_CS;
    GPIO_DATA = PIN_CS;
    GPIO_DATA = PIN_CS | PIN_SCK;      /* SCK=1 */
    uint8_t b = (uint8_t)((GPIO_DATA >> 1) & 1);  /* sample SIO */
    v = (uint8_t)((v >> 1) | (b << 7));
  }
  return v;
}

static uint8_t bcd(uint8_t v) { return (uint8_t)(((v >> 4) * 10) + (v & 0x0F)); }

/* Send one command, then read `n` reply bytes into `buf`. */
static void rtc_txn(uint8_t cmd, uint8_t* buf, int n) {
  GPIO_DATA = PIN_SCK;                       /* SCK=1, CS=0 */
  GPIO_DATA = PIN_SCK | PIN_CS;              /* CS=1 -> select */
  GPIO_DIR  = PIN_SCK | PIN_SIO | PIN_CS;   /* SIO output while sending cmd */
  write_cmd(cmd);
  GPIO_DIR  = PIN_SCK | PIN_CS;             /* SIO -> input for the reply */
  for (int i = 0; i < n; i++) buf[i] = read_byte();
  GPIO_DATA = PIN_SCK;                       /* CS=0 -> deselect */
  GPIO_DATA = 0;
}

bool gba_rtc_get(GbaRtcTime* out) {
  GPIO_CTRL = 1;   /* make the GPIO pins readable/writable */

  uint8_t status;
  rtc_txn(CMD_STATUS_READ, &status, 1);

  uint8_t raw[7];  /* year, month, day, dayOfWeek, hour, minute, second (BCD) */
  rtc_txn(CMD_DATETIME_READ, raw, 7);

  uint8_t y  = bcd(raw[0]);
  uint8_t mo = bcd(raw[1] & 0x1F);
  uint8_t d  = bcd(raw[2] & 0x3F);
  uint8_t mi = bcd(raw[5] & 0x7F);
  uint8_t se = bcd(raw[6] & 0x7F);

  uint8_t h;
  if (status & STATUS_24HOUR) {
    h = bcd(raw[4] & 0x3F);
  } else {
    bool pm = (raw[4] & 0x80) != 0;
    uint8_t h12 = bcd(raw[4] & 0x1F) % 12;
    h = (uint8_t)(pm ? h12 + 12 : h12);
  }

  if (y > 99 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
      h > 23 || mi > 59 || se > 59)
    return false;   /* implausible -> treat as "no RTC" */

  out->year   = (uint16_t)(2000 + y);
  out->month  = mo;
  out->day    = d;
  out->hour   = h;
  out->minute = mi;
  out->second = se;
  return true;
}
