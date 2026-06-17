#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>

/* Tiny INI-style persistent config for the record-mixer. Today it holds one
 * value — last_dir, the folder the SD browser was last in — so the next launch
 * reopens it (the saves usually live in the same place). Stored on the SD card
 * root next to the team-prefs JSON. GBA-only (uses FatFs); not host-tested. */

void config_load(const char* ini_path);       /* read + parse the INI (defaults if absent) */
bool config_get_last_dir(char* out, int cap);  /* copy stored last dir; false if none      */
void config_set_last_dir(const char* dir);     /* update the in-memory last dir             */
bool config_get_quick_mode(void);              /* quick (one-confirm, no-backup) mix toggle */
void config_set_quick_mode(bool on);           /* update the in-memory quick-mode flag       */
bool config_get_anim_enabled(void);            /* trade/mix animations on?                   */
void config_set_anim_enabled(bool on);
bool config_get_sfx_enabled(void);             /* sound effects on?                          */
void config_set_sfx_enabled(bool on);
bool config_save(const char* ini_path);        /* write the INI back to the SD card         */

#endif /* APP_CONFIG_H */
