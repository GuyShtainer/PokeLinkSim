#include "record_mix.h"
#include <string.h>

/*
 * Single-friend re-implementation of the in-game Gen 3 record mix, written by
 * reference to the public pret decompilation (NOT bundled in this repo):
 *   pokeemerald/secret_base.c  : ReceiveSecretBasesData, SaveRecordMixBases,
 *                                TrySaveFriendsSecretBase, the dedup/sort/evict
 *                                helpers, ClearSecretBase, SaveSecretBase.
 *   pokeemerald/record_mixing.c: how those are driven for a link session.
 *
 * The game mixes up to four players at once (one host + three friends A/B/C).
 * This tool mixes exactly two saves, so the three-friend fan-out collapses to a
 * single friend. The logic below follows the decompiled behaviour; only the
 * empty B/C friend slots (which no-op on secretBaseId==0) are dropped.
 *
 * IMPORTANT: this file is also compiled by tests/host_test.c on a PC, so it must
 * stay pure C (stdint + string.h only) — no tonc / GBA headers.
 */

/* Catch any layout drift early: the packed struct must be exactly 160 bytes. */
_Static_assert(sizeof(SecretBase) == G3_SECRET_BASE_SIZE,
               "SecretBase must be 160 bytes to match the cartridge layout");

#define SB_COUNT      G3_SECRET_BASES_COUNT  /* 20 */
#define SB_NAME_LEN   7                      /* PLAYER_NAME_LENGTH */
#define SB_TID_LEN    4                      /* TRAINER_ID_LENGTH  */

#define LANGUAGE_JAPANESE 1
#define LANGUAGE_ENGLISH  2

/* Stats are threaded through the deep static call tree via a file-local pointer,
 * set once at the top of recordmix_run (single-threaded; matches GBA usage). */
static MixStats*    s_stats;
static const PlayerIdentity* s_host_id;
static uint8_t      s_host_language;   /* GAME_LANGUAGE of the host save */

/* ---- low-level record helpers (mirror secret_base.c) -------------------- */

static void ClearSecretBase(SecretBase* b) {
  memset(b, 0, sizeof(*b));
  for (int i = 0; i < SB_NAME_LEN; i++) b->trainerName[i] = SB_EOS;
}

static bool SameTrainerId(const SecretBase* a, const SecretBase* b) {
  for (int i = 0; i < SB_TID_LEN; i++)
    if (a->trainerId[i] != b->trainerId[i]) return false;
  return true;
}

/* Name compare stops once BOTH names have terminated (faithful to the decomp,
 * which walks until both hit EOS). */
static bool SameTrainerName(const SecretBase* a, const SecretBase* b) {
  for (int i = 0; i < SB_NAME_LEN &&
                  (a->trainerName[i] != SB_EOS || b->trainerName[i] != SB_EOS); i++)
    if (a->trainerName[i] != b->trainerName[i]) return false;
  return true;
}

static bool BelongToSamePlayer(const SecretBase* a, const SecretBase* b) {
  return sb_gender(a) == sb_gender(b) && SameTrainerId(a, b) && SameTrainerName(a, b);
}

/* Does this record belong to the HOST player (used to strip the host's own base
 * out of the friend's incoming list)? Mirrors SecretBaseBelongsToPlayer, which
 * compares against gSaveBlock2Ptr (the receiving player == our host). */
static bool BelongsToHost(const SecretBase* b) {
  if (b->secretBaseId == 0) return false;
  if (sb_gender(b) != s_host_id->gender) return false;
  for (int i = 0; i < SB_TID_LEN; i++)
    if (b->trainerId[i] != s_host_id->trainerId[i]) return false;
  for (int i = 0; i < SB_NAME_LEN &&
                  (b->trainerName[i] != SB_EOS || s_host_id->trainerName[i] != SB_EOS); i++)
    if (b->trainerName[i] != s_host_id->trainerName[i]) return false;
  return true;
}

static int GetSecretBaseIndexFromId(const SecretBase* host, uint8_t id) {
  for (int i = 0; i < SB_COUNT; i++)
    if (host[i].secretBaseId == id) return i;
  return -1;
}

static int FindAvailableSecretBaseIndex(const SecretBase* host) {
  for (int i = 1; i < SB_COUNT; i++)
    if (host[i].secretBaseId == 0) return i;
  return 0;
}

static int FindUnregisteredSecretBaseIndex(const SecretBase* host) {
  for (int i = 1; i < SB_COUNT; i++)
    if (sb_registryStatus(&host[i]) == SB_REG_UNREGISTERED && sb_toRegister(&host[i]) == 0)
      return i;
  return 0;
}

/* Copy a friend base into host[idx], marking it NEW, and normalising the
 * language exactly like the game does (RS sources adopt the host's language;
 * a Japanese name on an Emerald source is also normalised). */
static void SaveSecretBase(SecretBase* host, int idx, const SecretBase* src,
                           Gen3Version friend_version) {
  host[idx] = *src;
  sb_set_registryStatus(&host[idx], SB_REG_NEW);

  if (friend_version == G3_VER_RS) {
    host[idx].language = s_host_language;
  } else if (friend_version == G3_VER_EMERALD && src->language == LANGUAGE_JAPANESE) {
    int len = 0;
    while (len < SB_NAME_LEN && host[idx].trainerName[len] != SB_EOS) len++;
    if (len > 5) host[idx].language = s_host_language;
  }
}

/* Try to place one friend base into the host array. Returns the host index it
 * was saved at (1..19), or 0 if it could not be saved. Faithful to
 * TrySaveFriendsSecretBase. */
static int TrySaveFriendsSecretBase(SecretBase* host, const SecretBase* src,
                                    Gen3Version friend_version) {
  if (src->secretBaseId == 0) return 0;

  int index = GetSecretBaseIndexFromId(host, src->secretBaseId);
  if (index != 0) {
    if (index != -1) {
      /* An existing base already occupies this location. */
      if (sb_toRegister(&host[index])) return 0;
      if (sb_registryStatus(&host[index]) != SB_REG_NEW || sb_toRegister(src)) {
        SaveSecretBase(host, index, src, friend_version);
        return index;
      }
    } else {
      /* Location is free: take an empty slot, else evict an unregistered one. */
      index = FindAvailableSecretBaseIndex(host);
      if (index != 0) { SaveSecretBase(host, index, src, friend_version); return index; }
      index = FindUnregisteredSecretBaseIndex(host);
      if (index != 0) { SaveSecretBase(host, index, src, friend_version); return index; }
      /* No room at all -> this friend base is dropped. */
      if (s_stats) s_stats->overflow = true;
    }
  }
  return 0;
}

static void TrySaveFriendsSecretBasesByStatus(SecretBase* host, SecretBase* friend,
                                              uint8_t registryStatus,
                                              Gen3Version friend_version) {
  for (int i = 1; i < SB_COUNT; i++)
    if (sb_registryStatus(&friend[i]) == registryStatus)
      TrySaveFriendsSecretBase(host, &friend[i], friend_version);
}

/* Move registered bases to the front so they survive future mixes. */
static void SortSecretBasesByRegistryStatus(SecretBase* host) {
  for (int i = 1; i < SB_COUNT - 1; i++) {
    for (int j = i + 1; j < SB_COUNT; j++) {
      uint8_t ri = sb_registryStatus(&host[i]);
      uint8_t rj = sb_registryStatus(&host[j]);
      if ((ri == SB_REG_UNREGISTERED && rj == SB_REG_REGISTERED) ||
          (ri == SB_REG_NEW && rj != SB_REG_NEW)) {
        SecretBase t = host[i];
        host[i] = host[j];
        host[j] = t;
      }
    }
  }
}

/* In the friend's list, find the FIRST base that is the host's own and delete
 * it (you never re-import your own base). */
static void DeleteHostsOwnBaseFromFriend(SecretBase* friend) {
  for (int i = 0; i < SB_COUNT; i++) {
    if (BelongsToHost(&friend[i])) {
      ClearSecretBase(&friend[i]);
      if (s_stats) s_stats->host_base_evicted = true;
      return;
    }
  }
}

/* If host base `hb` (at index idx) duplicates a friend base (same trainer),
 * keep whichever has the higher numSecretBasesReceived. Returns true if the
 * host base was cleared (friend's copy wins). Mirrors ClearDuplicateOwnedSecretBase. */
static bool ClearDuplicateOwnedSecretBase(SecretBase* hb, SecretBase* friend, int idx) {
  for (int i = 0; i < SB_COUNT; i++) {
    if (friend[i].secretBaseId != 0 && BelongToSamePlayer(hb, &friend[i])) {
      if (s_stats) s_stats->duplicates++;
      if (idx == 0) { ClearSecretBase(&friend[i]); return false; }
      if (hb->numSecretBasesReceived > friend[i].numSecretBasesReceived) {
        ClearSecretBase(&friend[i]);          /* host's copy is newer -> drop friend's */
        return false;
      }
      sb_set_toRegister(&friend[i], sb_toRegister(hb)); /* carry registration intent */
      ClearSecretBase(hb);                    /* friend's copy is newer -> drop host's */
      return true;
    }
  }
  return false;
}

static void ClearDuplicateOwnedSecretBases(SecretBase* host, SecretBase* friend) {
  for (int i = 1; i < SB_COUNT; i++) {
    if (host[i].secretBaseId) {
      if (sb_registryStatus(&host[i]) == SB_REG_REGISTERED)
        sb_set_toRegister(&host[i], 1);  /* re-register if deleted as a dup later */
      ClearDuplicateOwnedSecretBase(&host[i], friend, i);
    }
  }
  /* Every surviving friend base becomes immediately battleable. */
  for (int i = 0; i < SB_COUNT; i++) {
    if (friend[i].secretBaseId) {
      sb_set_battledToday(&friend[i], 0);
      if (s_stats) s_stats->refreshed++;
    }
  }
}

static void TrySaveRegisteredDuplicates(SecretBase* host, SecretBase* friend,
                                        Gen3Version friend_version) {
  for (int i = 0; i < SB_COUNT; i++) {
    if (sb_toRegister(&friend[i])) {
      TrySaveFriendsSecretBase(host, &friend[i], friend_version);
      ClearSecretBase(&friend[i]);
    }
  }
}

/* SaveRecordMixBases, single-friend reduction. */
static void SaveRecordMixBases(SecretBase* host, SecretBase* friend,
                               Gen3Version friend_version) {
  DeleteHostsOwnBaseFromFriend(friend);
  ClearDuplicateOwnedSecretBases(host, friend);

  /* 1) registered bases deleted as duplicates get re-saved first */
  TrySaveRegisteredDuplicates(host, friend, friend_version);
  /* 2) the friend's OWN base (slot 0) */
  TrySaveFriendsSecretBase(host, &friend[0], friend_version);
  /* 3) the friend's registered, then unregistered, collected bases */
  TrySaveFriendsSecretBasesByStatus(host, friend, SB_REG_REGISTERED, friend_version);
  TrySaveFriendsSecretBasesByStatus(host, friend, SB_REG_UNREGISTERED, friend_version);
}

void sb_set_party_from_live(SecretBase* base, const Gen3LiveParty* live) {
  if (base->secretBaseId == 0) return;     /* matches the game's guard */

  /* Zero every party slot first. */
  for (int i = 0; i < 6; i++) {
    base->species[i]     = 0;
    base->heldItems[i]   = 0;
    base->levels[i]      = 0;
    base->personality[i] = 0;
    base->evs[i]         = 0;
    for (int m = 0; m < 4; m++) base->moves[i * 4 + m] = 0;
  }

  int n = live->count;
  if (n > 6) n = 6;
  for (int i = 0; i < n; i++) {
    const Gen3PartyMon* s = &live->mon[i];
    base->species[i]     = s->species;
    base->heldItems[i]   = s->heldItem;
    base->levels[i]      = s->level;
    base->personality[i] = s->personality;
    base->evs[i]         = s->avgEV;
    for (int m = 0; m < 4; m++) base->moves[i * 4 + m] = s->moves[m];
  }
}

void sb_set_party_from_choice(SecretBase* base, const SbPartyChoice* c) {
  if (base->secretBaseId == 0) return;     /* matches the game's guard */

  for (int i = 0; i < 6; i++) {
    base->species[i]     = 0;
    base->heldItems[i]   = 0;
    base->levels[i]      = 0;
    base->personality[i] = 0;
    base->evs[i]         = 0;
    for (int m = 0; m < 4; m++) base->moves[i * 4 + m] = 0;
  }

  int n = c->count;
  if (n > 6) n = 6;
  for (int i = 0; i < n; i++) {
    base->species[i]     = c->species[i];
    base->heldItems[i]   = c->heldItems[i];
    base->levels[i]      = c->levels[i];
    base->personality[i] = c->personality[i];
    base->evs[i]         = c->evs[i];
    for (int m = 0; m < 4; m++) base->moves[i * 4 + m] = c->moves[i * 4 + m];
  }
}

bool recordmix_run(SecretBase* host,
                   const PlayerIdentity* host_id,
                   Gen3Version host_version,
                   SecretBase* friend,
                   Gen3Version friend_version,
                   MixStats* stats) {
  if (!host || !host_id || !friend) return false;

  s_stats   = stats;
  s_host_id = host_id;
  /* GAME_LANGUAGE: only Emerald uses the per-record language byte (RS treats
   * 0x0D as padding), so derive it from the host's own base on Emerald and
   * fall back to English everywhere else. */
  s_host_language = (host_version == G3_VER_EMERALD && host[0].secretBaseId != 0)
                      ? host[0].language : LANGUAGE_ENGLISH;

  int used_before = 0;
  if (stats) {
    memset(stats, 0, sizeof(*stats));
    for (int i = 0; i < SB_COUNT; i++) if (host[i].secretBaseId) used_before++;
  }

  SaveRecordMixBases(host, friend, friend_version);

  /* Re-register bases flagged during dedup, then sort and demote NEW->UNREG. */
  for (int i = 1; i < SB_COUNT; i++) {
    if (sb_toRegister(&host[i])) {
      sb_set_registryStatus(&host[i], SB_REG_REGISTERED);
      sb_set_toRegister(&host[i], 0);
    }
  }
  SortSecretBasesByRegistryStatus(host);
  for (int i = 1; i < SB_COUNT; i++)
    if (sb_registryStatus(&host[i]) == SB_REG_NEW)
      sb_set_registryStatus(&host[i], SB_REG_UNREGISTERED);

  /* Make EVERY received base battleable again right after a mix. The friend-side
   * clear (ClearDuplicateOwnedSecretBases) only resets battledOwnerToday on bases
   * the friend's copy contributes; when you've already mixed these saves the
   * partner's base is a duplicate whose HOST copy wins the dedup and survives with
   * its battledOwnerToday still set -- so a re-mix never let you re-battle it. Clear
   * the flag on all of the host's friend bases (slot 0 = own base -> skip). */
  for (int i = 1; i < SB_COUNT; i++)
    if (host[i].secretBaseId) sb_set_battledToday(&host[i], 0);

  /* Bump the host's own received counter (saturating at 0xFFFF). */
  if (host[0].secretBaseId != 0 && host[0].numSecretBasesReceived != 0xFFFF)
    host[0].numSecretBasesReceived++;

  if (stats) {
    int used_after = 0;
    for (int i = 0; i < SB_COUNT; i++) if (host[i].secretBaseId) used_after++;
    stats->host_used = used_after;
    stats->imported  = used_after - used_before;
    if (stats->imported < 0) stats->imported = 0;
  }

  s_stats = NULL;
  s_host_id = NULL;
  return true;
}
