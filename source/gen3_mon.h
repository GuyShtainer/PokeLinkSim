#ifndef GEN3_MON_H
#define GEN3_MON_H

#include <stdint.h>
#include <stdbool.h>

/* Full-fidelity Gen-3 Pokémon decode (pure C, host-testable). Unlike
 * gen3_save.c's gen3_read_live_party*, this keeps EVERYTHING the viewer needs:
 * IVs, the full 6-EV spread, moves+PP, OT, met data, ability slot, shininess,
 * contest stats and ribbons. Works on both the 100-byte party record (which
 * also carries plaintext battle stats + level) and the 80-byte PC-box record
 * (level + stats must be COMPUTED — see pk_resolve / pk_calc_*).
 *
 * All six-element arrays use the SAVE-NATIVE order: HP, Atk, Def, Spe, SpA, SpD.
 */

enum { PK_HP = 0, PK_ATK, PK_DEF, PK_SPE, PK_SPA, PK_SPD, PK_NSTATS };

typedef struct {
  uint16_t species;           /* INTERNAL Gen-3 index (1..411); 0 = none        */
  char     nickname[11];      /* decoded, 10 chars + NUL                         */
  char     otName[8];         /* decoded, 7 chars + NUL                          */
  uint32_t personality;
  uint32_t otId;              /* low 16 = public TID, high 16 = secret SID       */
  uint32_t experience;
  uint8_t  level;             /* party: plaintext; box: computed (0 until set)   */
  uint8_t  nature;            /* personality % 25                                */
  uint8_t  abilityNum;        /* 0 or 1 (which of the species' two abilities)    */
  uint8_t  friendship;
  uint8_t  ppBonuses;         /* 2 bits per move                                 */
  bool     isShiny, isEgg, isBadEgg, isParty;
  uint8_t  ivs[PK_NSTATS];    /* 0..31                                           */
  uint8_t  evs[PK_NSTATS];    /* 0..255                                          */
  uint16_t evSum;
  uint16_t moves[4];
  uint8_t  pp[4];             /* current PP                                      */
  uint16_t heldItem;
  uint8_t  pokerus, metLocation, metLevel, metGame, pokeball, otGender;
  uint8_t  contest[6];        /* cool, beauty, cute, smart, tough, sheen         */
  uint32_t ribbons;
  uint16_t stats[PK_NSTATS];  /* party: plaintext; box: computed                 */
  uint8_t  gender;            /* 0=M, 1=F, 2=genderless (filled by pk_resolve)   */
  const uint8_t* raw;         /* back-ref to the 80/100-byte record (edit later) */
} PkMon;

/* Decode one record. is_party => 100-byte (plaintext stats/level); else 80-byte
 * box record. Returns false for a truly empty slot; returns true for real mons
 * AND eggs/bad-eggs (with isEgg/isBadEgg set) so the viewer can surface them. */
bool pk_decode_mon(const uint8_t* mon, bool is_party, PkMon* out);

/* Read the live party from a reassembled SaveBlock1. FRLG moved the party block
 * to the start of SaveBlock1 (count@0x034/data@0x038) vs R/S/E (0x234/0x238). */
int  pk_read_party(const uint8_t* sb1, bool frlg, PkMon out[6]);

/* Auto-detect the party layout (R/S/E 0x234/0x238 vs FRLG 0x034/0x038) by
 * decoding at both offsets and keeping whichever yields valid mons. Robust where
 * SaveBlock2-size game detection is ambiguous. Sets *is_frlg if non-NULL. */
int  pk_read_party_auto(const uint8_t* sb1, PkMon out[6], bool* is_frlg);

/* Derivations (pure, no data tables). */
uint8_t pk_nature(uint32_t personality);
bool    pk_is_shiny(uint32_t personality, uint16_t tid, uint16_t sid);
uint8_t pk_gender_from(uint32_t personality, uint8_t gender_ratio); /* 0xFF genderless, 0xFE F, 0x00 M */

/* Gen-3 stat formulas (for box mons + cross-checking party plaintext).
 * nature_mod: +1 boosted (×1.1), -1 hindered (×0.9), 0 neutral. */
uint16_t pk_calc_hp(uint8_t base, uint8_t iv, uint8_t ev, uint8_t level);
uint16_t pk_calc_stat(uint8_t base, uint8_t iv, uint8_t ev, uint8_t level, int nature_mod);

/* ---- in-place edits on a raw 80/100-byte mon record (for trading) ----
 * Each decrypts the secure block, edits the Growth substruct, re-encrypts, and
 * recomputes the 0x1C checksum so the record stays valid. OT is never changed. */
void pk_set_friendship(uint8_t* rec, uint8_t value);
void pk_evolve(uint8_t* rec, uint16_t new_species, bool consume_item);
bool pk_checksum_ok(const uint8_t* rec);   /* secure-block checksum still valid? */

#endif /* GEN3_MON_H */
