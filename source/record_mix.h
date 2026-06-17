#ifndef RECORD_MIX_H
#define RECORD_MIX_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_save.h"

/*
 * On-cartridge secret-base record, mirrored byte-for-byte. 160 bytes, packed.
 *
 * This ONE layout matches BOTH games:
 *   Emerald   -> struct SecretBase       @ SaveBlock1 0x1A9C (reference/pokeemerald/global.h)
 *   Ruby/Sapp -> struct SecretBaseRecord @ SaveBlock1 0x1A08 (reference/pokeruby/global.h)
 * Every field offset is identical between them; only offset 0x0D differs:
 *   Emerald uses it as `language`, Ruby/Sapphire treat it as padding (unused).
 * So copying a record verbatim is correct for same-game mixing; for cross-game
 * mixing, `language` is the only field that may need adjusting.
 *
 * flags byte (offset 0x01) bitfield, low bit first (matches the decomp's
 *   bool8 toRegister:4; u8 gender:1; u8 battledOwnerToday:1; u8 registryStatus:2;
 * laid out LSB-first by GCC on little-endian ARM):
 *   bits 0..3 toRegister, bit 4 gender, bit 5 battledOwnerToday, bits 6..7 registryStatus
 */
typedef struct __attribute__((packed)) {
  uint8_t  secretBaseId;             /* 0x00 */
  uint8_t  flags;                    /* 0x01 */
  uint8_t  trainerName[7];           /* 0x02 */
  uint8_t  trainerId[4];             /* 0x09 */
  uint8_t  language;                 /* 0x0D */
  uint16_t numSecretBasesReceived;   /* 0x0E */
  uint8_t  numTimesEntered;          /* 0x10 */
  uint8_t  unused;                   /* 0x11 */
  uint8_t  decorations[16];          /* 0x12 */
  uint8_t  decorationPositions[16];  /* 0x22 */
  uint8_t  padding[2];               /* 0x32 */
  /* struct SecretBaseParty @ 0x34 */
  uint32_t personality[6];           /* 0x34 */
  uint16_t moves[6 * 4];             /* 0x4C */
  uint16_t species[6];               /* 0x7C */
  uint16_t heldItems[6];             /* 0x88 */
  uint8_t  levels[6];                /* 0x94 */
  uint8_t  evs[6];                   /* 0x9A */
} SecretBase;                        /* 0xA0 = 160 bytes total */

/* flags-byte bitfield positions (see struct comment above). */
#define SB_TOREGISTER_MASK  0x0Fu
#define SB_GENDER_BIT       (1u << 4)
#define SB_BATTLED_BIT      (1u << 5)
#define SB_REGSTATUS_SHIFT  6
#define SB_REGSTATUS_MASK   0xC0u

/* registryStatus values (reference/pokeemerald/secret_base.c enum). */
#define SB_REG_UNREGISTERED 0u
#define SB_REG_REGISTERED   1u
#define SB_REG_NEW          2u  /* transient: bases mixed this round, demoted after */

/* Gen 3 string terminator (used as the empty-name marker in a cleared base). */
#define SB_EOS 0xFFu

/* ---- flags-byte accessors (keep struct layout decode/encode in one place) -- */
static inline uint8_t sb_toRegister(const SecretBase* b) { return b->flags & SB_TOREGISTER_MASK; }
static inline uint8_t sb_gender(const SecretBase* b)     { return (b->flags & SB_GENDER_BIT) ? 1u : 0u; }
static inline uint8_t sb_battledToday(const SecretBase* b){ return (b->flags & SB_BATTLED_BIT) ? 1u : 0u; }
static inline uint8_t sb_registryStatus(const SecretBase* b) {
  return (uint8_t)((b->flags & SB_REGSTATUS_MASK) >> SB_REGSTATUS_SHIFT);
}
static inline void sb_set_toRegister(SecretBase* b, uint8_t v) {
  b->flags = (uint8_t)((b->flags & ~SB_TOREGISTER_MASK) | (v & SB_TOREGISTER_MASK));
}
static inline void sb_set_battledToday(SecretBase* b, uint8_t v) {
  b->flags = (uint8_t)((b->flags & ~SB_BATTLED_BIT) | (v ? SB_BATTLED_BIT : 0u));
}
static inline void sb_set_registryStatus(SecretBase* b, uint8_t v) {
  b->flags = (uint8_t)((b->flags & ~SB_REGSTATUS_MASK) |
                       ((uint32_t)(v & 3u) << SB_REGSTATUS_SHIFT));
}

/* Host player's identity, raw (charset-encoded) bytes straight from SaveBlock2.
 * Needed to recognise (and skip) the host's OWN base inside the friend's list
 * exactly the way the game compares them. */
typedef struct {
  uint8_t gender;          /* 0 male, 1 female (SaveBlock2 0x08) */
  uint8_t trainerId[4];    /* SaveBlock2 0x0A: TID lo/hi, SID lo/hi */
  uint8_t trainerName[7];  /* SaveBlock2 0x00, EOS-terminated */
} PlayerIdentity;

/* What the merge did, for the UI / log (purely informational). */
typedef struct {
  int  imported;        /* friend bases newly saved into the host array        */
  int  duplicates;      /* same-trainer bases the host already had              */
  int  refreshed;       /* friend bases whose battledOwnerToday was cleared     */
  int  host_used;       /* host base slots in use after the merge (1..20)       */
  bool host_base_evicted; /* host's own base was deleted from the friend's list */
  bool overflow;        /* >=1 friend base could not fit (host array was full)  */
} MixStats;

/* --------------------------------------------------------------------------
 * Re-implementation of the in-game record mix for the single-friend case, by
 * reference to the public pret decompilation (pokeemerald record_mixing.c +
 * secret_base.c; not bundled in this repo).
 *
 * host        : the player's 20-record array, IN PLACE (this is what gets saved)
 * host_id     : the player's identity (raw SaveBlock2 bytes)
 * host_version: G3_VER_RS or G3_VER_EMERALD (the game whose save we write back)
 * friend      : the friend's 20-record array; MUTATED as a scratch copy
 * friend_ver  : the friend save's version (drives the RS language rewrite)
 * stats       : optional, may be NULL
 *
 * It merges the friend's bases into `host`: skips the host's own location,
 * dedups by trainer keeping the most-recently-updated copy, fills empty slots
 * then evicts unregistered ones, preserves registered bases, clears
 * battledOwnerToday on every incoming base (so they are immediately battleable),
 * re-registers and sorts, and bumps the host's numSecretBasesReceived.
 *
 * Returns true if the merge ran (always, given valid inputs).
 * ------------------------------------------------------------------------ */
bool recordmix_run(SecretBase* host,
                   const PlayerIdentity* host_id,
                   Gen3Version host_version,
                   SecretBase* friend,
                   Gen3Version friend_version,
                   MixStats* stats);

/* Replicate SetPlayerSecretBaseParty: rebuild a base's own party snapshot from
 * the player's LIVE party (decrypted via gen3_read_live_party). Zeroes all 6
 * slots, then writes the kept mons compacted to the front. No-op when the base
 * is empty (secretBaseId == 0), matching the game.
 *
 * This is the fix for stale base-party snapshots: the game only refreshes
 * base[0].party at link-mix start, so a normally-saved .sav carries an outdated
 * team; calling this before the merge makes the shared base reflect the team
 * actually in the save right now. */
void sb_set_party_from_live(SecretBase* base, const Gen3LiveParty* live);

/* An explicit secret-base party the user chose from the live party AND/OR the PC
 * boxes (Phase 1), to register instead of the default "regen from live party".
 * Plain data (no PkMon/tonc) so the mix core stays host-testable; the UI fills it
 * from decoded mons. `count == 0` means "no override — regen from live". */
typedef struct {
  uint8_t  count;             /* 0..6 chosen mons                               */
  uint16_t species[6];
  uint16_t heldItems[6];
  uint16_t moves[6 * 4];
  uint8_t  levels[6];
  uint8_t  evs[6];            /* average EV per mon (matches GetAverageEVs)      */
  uint32_t personality[6];
} SbPartyChoice;

/* Set `base`'s party from an explicit choice (Phase 1). No-op if the base is
 * empty (secretBaseId == 0), like sb_set_party_from_live. */
void sb_set_party_from_choice(SecretBase* base, const SbPartyChoice* choice);

#endif /* RECORD_MIX_H */
