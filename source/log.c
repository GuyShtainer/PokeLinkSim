#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "sys.h"  /* EWRAM_BSS */

#define LOG_CAP 8192

static char EWRAM_BSS s_buf[LOG_CAP];
static unsigned s_len = 0;
static int      s_mgba = 0;

/* --- mGBA debug interface ------------------------------------------------- */
/* Write 0xC0DE to ENABLE; if it reads back 0x1DEA we're under mGBA. Strings
 * up to 255 bytes go in the buffer at 0x4FFF600, then FLAGS = 0x100 | level. */
#define MGBA_REG_ENABLE (*(volatile unsigned short*)0x4FFF780)
#define MGBA_REG_FLAGS  (*(volatile unsigned short*)0x4FFF700)
#define MGBA_LOG_BUF    ((volatile char*)0x4FFF600)
#define MGBA_LEVEL_INFO 3

void log_init(void) {
  MGBA_REG_ENABLE = 0xC0DE;
  s_mgba = (MGBA_REG_ENABLE == 0x1DEA) ? 1 : 0;
  log_clear();
}

int log_under_mgba(void) { return s_mgba; }

void log_clear(void) {
  s_len = 0;
  s_buf[0] = 0;
}

const char* log_text(void) { return s_buf; }

static void mgba_emit(const char* line) {
  if (!s_mgba) return;
  int i = 0;
  while (line[i] && i < 255) {
    MGBA_LOG_BUF[i] = line[i];
    i++;
  }
  MGBA_LOG_BUF[i] = 0;
  MGBA_REG_FLAGS = 0x100 | MGBA_LEVEL_INFO;
}

void log_line(const char* fmt, ...) {
  char tmp[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);

  mgba_emit(tmp);

  unsigned n = (unsigned)strlen(tmp);
  /* If the buffer would overflow, drop the oldest half. */
  if (s_len + n + 2 >= LOG_CAP) {
    unsigned keep = LOG_CAP / 2;
    if (s_len > keep) {
      memmove(s_buf, s_buf + (s_len - keep), keep);
      s_len = keep;
    } else {
      s_len = 0;
    }
  }
  memcpy(s_buf + s_len, tmp, n);
  s_len += n;
  s_buf[s_len++] = '\n';
  s_buf[s_len] = 0;
}

/* APPEND the buffered lines to the SD log (never truncate, so history across runs
 * is preserved), then clear the in-memory buffer so the next flush only writes the
 * new lines (no duplication). Open-always + seek-to-EOF is FatFs-version-agnostic. */
int log_flush_to_sd(const char* path) {
  if (s_len == 0) return 0;                       /* nothing new to append */
  FIL f;
  FRESULT fr = f_open(&f, path, FA_WRITE | FA_OPEN_ALWAYS);
  if (fr != FR_OK) return (int)fr;
  fr = f_lseek(&f, f_size(&f));                   /* append at end of file */
  if (fr != FR_OK) { f_close(&f); return (int)fr; }
  UINT bw = 0;
  fr = f_write(&f, s_buf, s_len, &bw);
  FRESULT fc = f_close(&f);
  if (fr != FR_OK) return (int)fr;
  if (fc != FR_OK) return (int)fc;
  if (bw != s_len) return -1;
  log_clear();          /* persisted -> drop flushed lines; next flush appends only new */
  return 0;
}
