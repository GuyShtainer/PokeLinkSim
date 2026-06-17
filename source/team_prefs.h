#ifndef TEAM_PREFS_H
#define TEAM_PREFS_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_save.h"

/* Persistent memory of which party Pokemon a player chose to register into the
 * secret base, so the picker can pre-fill those choices next time instead of
 * making you re-select. Stored on the SD card as JSON, keyed per game type +
 * player name + player ID. Selections are matched back to a live party by each
 * mon's species + nickname (order-independent, survives party reordering).
 *
 * parse/serialize/find/apply/update are pure (operate on in-memory state and
 * caller buffers) so they can be unit-tested on the host; tp_init/tp_save are
 * thin SD-card wrappers (compiled out when TP_HOST is defined). */

#define TP_MAX_TEAMS 24
#define TP_MAX_MON   6
#define TP_NICK_LEN  11   /* 10 chars + NUL */
#define TP_TRN_LEN   8    /* 7 chars + NUL  */

typedef struct {
  uint16_t species;
  char     nick[TP_NICK_LEN];
  uint8_t  chosen;        /* 1 = registered into secret base, 0 = omitted */
} TpMon;

typedef struct {
  uint8_t  used;
  uint8_t  game;          /* 0 Emerald, 1 Ruby, 2 Sapphire, 3 Ruby/Sapp */
  char     trainer[TP_TRN_LEN];
  uint16_t tid_public;
  uint16_t tid_secret;
  uint8_t  n;
  TpMon    mon[TP_MAX_MON];
} TpTeam;

/* ---- pure (host-testable) ---------------------------------------------- */

void tp_reset(void);                            /* clear the in-memory store */
int  tp_parse(const char* text, int len);       /* load store from JSON, -> #teams */
int  tp_serialize(char* buf, int cap);          /* dump store to JSON, -> length    */

/* Find a saved team for this (game, trainer, public id), or NULL. */
const TpTeam* tp_find(uint8_t game, const char* trainer, uint16_t tid_public);

/* Pre-fill chk[0..dp->count-1] from saved choices (default true = chosen);
 * a mon with no saved record stays chosen. `t` may be NULL (all default). */
void tp_apply(const TpTeam* t, const Gen3DisplayParty* dp, bool chk[6]);

/* Upsert this team's selection (FIFO-evicts the oldest if the store is full). */
void tp_update(uint8_t game, const char* trainer, uint16_t tid_public,
               uint16_t tid_secret, const Gen3DisplayParty* dp, const bool chk[6]);

/* ---- SD-card backed (GBA only) ----------------------------------------- */

void tp_init(const char* path);    /* load store from a JSON file (clears if absent) */
bool tp_save(const char* path);    /* write store to a JSON file (RTC-stamped)       */

#endif /* TEAM_PREFS_H */
