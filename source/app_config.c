/*
 * Minimal INI config for the record-mixer. Values: last_dir, the folder the
 * browser was last in, so the next launch reopens it (the saves usually live in
 * the same place); and quick_mode, the one-confirm no-backup mix toggle. Stored
 * at the SD root, next to /pokelinksim_teams.json.
 *
 * GBA-only (uses FatFs). Not part of the host-tested pure-C core. Large buffers
 * go in EWRAM via .sbss (the section the GBA linker maps into EWRAM — same trick
 * team_prefs.c uses) so they never sit on the scarce IWRAM stack.
 */
#include <stdio.h>   /* siprintf */
#include <string.h>

#include "ff.h"
#include "app_config.h"

#define CFG_EWRAM  __attribute__((section(".sbss")))
#define CFG_DIR_MAX 256

static char CFG_EWRAM s_last_dir[CFG_DIR_MAX];
static int            s_quick_mode = 0;
static int            s_anim = 1;   /* trade/mix animations on by default */
static int            s_sfx  = 1;   /* sound effects on by default        */

static void store_dir(const char* dir) {
  int i = 0;
  if (dir) for (; dir[i] && i < CFG_DIR_MAX - 1; i++) s_last_dir[i] = dir[i];
  s_last_dir[i] = 0;
}

void config_set_last_dir(const char* dir) { store_dir(dir); }

bool config_get_quick_mode(void)      { return s_quick_mode != 0; }
void config_set_quick_mode(bool on)   { s_quick_mode = on ? 1 : 0; }
bool config_get_anim_enabled(void)    { return s_anim != 0; }
void config_set_anim_enabled(bool on) { s_anim = on ? 1 : 0; }
bool config_get_sfx_enabled(void)     { return s_sfx != 0; }
void config_set_sfx_enabled(bool on)  { s_sfx = on ? 1 : 0; }

bool config_get_last_dir(char* out, int cap) {
  if (!out || cap <= 0) return false;
  out[0] = 0;
  if (s_last_dir[0] == 0) return false;
  int i = 0;
  for (; s_last_dir[i] && i < cap - 1; i++) out[i] = s_last_dir[i];
  out[i] = 0;
  return out[0] != 0;
}

void config_load(const char* ini_path) {
  s_last_dir[0] = 0;
  s_quick_mode  = 0;
  s_anim = 1;             /* defaults if the file lacks the key */
  s_sfx  = 1;
  FIL f;
  if (f_open(&f, ini_path, FA_READ) != FR_OK) return;     /* no file yet: defaults */
  static char CFG_EWRAM buf[512];
  UINT br = 0;
  FRESULT fr = f_read(&f, buf, sizeof(buf) - 1, &br);
  f_close(&f);
  if (fr != FR_OK || br == 0) return;
  buf[br] = 0;

  /* Walk lines; set whichever known key matches. Section/comment/blank lines are
   * skipped. Tolerant: anything unrecognized is ignored (a corrupt file degrades
   * to defaults). */
  static const char K_DIR[]   = "last_dir=";
  static const char K_QUICK[] = "quick_mode=";
  static const char K_ANIM[]  = "anim=";
  static const char K_SFX[]   = "sfx=";
  char* p = buf;
  while (*p) {
    char* line = p;
    while (*p && *p != '\n' && *p != '\r') p++;
    if (*p) { *p = 0; p++; }
    while (*p == '\n' || *p == '\r') p++;            /* swallow CR/LF runs */

    char* s = line;
    while (*s == ' ' || *s == '\t') s++;             /* trim leading ws */
    if (*s == 0 || *s == '#' || *s == ';' || *s == '[') continue;

    if (strncmp(s, K_DIR, sizeof(K_DIR) - 1) == 0) {
      char* v = s + (sizeof(K_DIR) - 1);
      while (*v == ' ' || *v == '\t') v++;
      int n = (int)strlen(v);
      while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t')) v[--n] = 0;
      store_dir(v);
    } else if (strncmp(s, K_QUICK, sizeof(K_QUICK) - 1) == 0) {
      char* v = s + (sizeof(K_QUICK) - 1);
      while (*v == ' ' || *v == '\t') v++;
      s_quick_mode = (*v == '1') ? 1 : 0;
    } else if (strncmp(s, K_ANIM, sizeof(K_ANIM) - 1) == 0) {
      char* v = s + (sizeof(K_ANIM) - 1);
      while (*v == ' ' || *v == '\t') v++;
      s_anim = (*v == '0') ? 0 : 1;
    } else if (strncmp(s, K_SFX, sizeof(K_SFX) - 1) == 0) {
      char* v = s + (sizeof(K_SFX) - 1);
      while (*v == ' ' || *v == '\t') v++;
      s_sfx = (*v == '0') ? 0 : 1;
    }
  }
}

bool config_save(const char* ini_path) {
  static char CFG_EWRAM out[CFG_DIR_MAX + 64];
  int n = siprintf(out, "[pokelinksim]\nlast_dir=%s\nquick_mode=%d\nanim=%d\nsfx=%d\n",
                   s_last_dir, s_quick_mode, s_anim, s_sfx);
  if (n <= 0) return false;
  FIL f;
  if (f_open(&f, ini_path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
  UINT bw = 0;
  FRESULT fr = f_write(&f, out, (UINT)n, &bw);
  f_close(&f);
  return fr == FR_OK && bw == (UINT)n;
}
