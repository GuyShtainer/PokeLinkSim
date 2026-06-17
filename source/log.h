#ifndef LOG_H
#define LOG_H

/*
 * Three log sinks so we can see what's happening wherever we run:
 *   1. an in-RAM text buffer the UI prints on screen (works on real hardware)
 *   2. mGBA's debug console (only when running under mGBA)
 *   3. a file on the SD card (the persistent artifact for hardware debugging)
 */

void log_init(void);                  /* probe/open the mGBA debug channel   */
int  log_under_mgba(void);            /* 1 if running under mGBA, else 0      */

void log_line(const char* fmt, ...);  /* append a line (RAM buffer + mGBA)    */
void log_clear(void);

const char* log_text(void);           /* the accumulated buffer (for the UI)  */

/* Write the buffer to `path` (FA_CREATE_ALWAYS). Returns 0 on success,
 * otherwise the FRESULT from FatFs. Call this after the SD is mounted. */
int  log_flush_to_sd(const char* path);

#endif /* LOG_H */
