/*
 * Gen-3 trade genuineness data: the trade-evolution table and base-friendship.
 * Pure C (host-testable). Species in the save are INTERNAL Gen-3 indices; the
 * table below is written in NATIONAL dex numbers (readable/verifiable) and
 * converted via pk_national_no (data_tables.c), so it works for both the
 * identity-mapped Kanto/Johto species and the Hoenn-reordered ones (Clamperl).
 */
#include "trade.h"
#include "data_tables.h"   /* pk_national_no */
#include "gen3_save.h"     /* sizes, gen3_find_section/checksum, section ids */
#include "gen3_mon.h"      /* PkMon, pk_decode_mon, pk_read_party_auto, pk_set_friendship, pk_evolve */
#include <string.h>

/* Item ids verified against the decomp item table:
 *   King's Rock 187, DeepSeaTooth 192, DeepSeaScale 193,
 *   Metal Coat 199, Dragon Scale 201, Up-Grade 218. */
typedef struct { uint16_t from_nat, item, to_nat; } TradeEvo;

static const TradeEvo k_trade_evos[] = {
  { 64,   0,  65},  /* Kadabra              -> Alakazam */
  { 67,   0,  68},  /* Machoke              -> Machamp  */
  { 75,   0,  76},  /* Graveler             -> Golem    */
  { 93,   0,  94},  /* Haunter              -> Gengar   */
  { 61, 187, 186},  /* Poliwhirl + King's Rock  -> Politoed */
  { 79, 187, 199},  /* Slowpoke  + King's Rock  -> Slowking */
  { 95, 199, 208},  /* Onix      + Metal Coat   -> Steelix  */
  {123, 199, 212},  /* Scyther   + Metal Coat   -> Scizor   */
  {117, 201, 230},  /* Seadra    + Dragon Scale -> Kingdra  */
  {137, 218, 233},  /* Porygon   + Up-Grade     -> Porygon2 */
  {366, 192, 367},  /* Clamperl  + DeepSeaTooth -> Huntail  */
  {366, 193, 368},  /* Clamperl  + DeepSeaScale -> Gorebyss */
};

static uint16_t internal_from_national(uint16_t nat) {
  if (nat == 0) return 0;
  if (nat <= 251) return nat;                          /* Kanto/Johto: identity */
  for (uint16_t i = 1; i <= 411; i++)
    if (pk_national_no(i) == nat) return i;
  return 0;
}

uint16_t trade_evolution(uint16_t species, uint16_t held_item, bool* consume_item) {
  if (consume_item) *consume_item = false;
  uint16_t nat = pk_national_no(species);
  if (nat == 0) return 0;
  for (unsigned i = 0; i < sizeof(k_trade_evos) / sizeof(k_trade_evos[0]); i++) {
    const TradeEvo* t = &k_trade_evos[i];
    if (t->from_nat == nat && (t->item == 0 || t->item == held_item)) {
      if (consume_item) *consume_item = (t->item != 0);
      return internal_from_national(t->to_nat);
    }
  }
  return 0;
}

uint8_t species_base_friendship(uint16_t species) {
  (void)species;
  /* Gen-3 base friendship is 70 (STANDARD_FRIENDSHIP) for the vast majority of
   * species, and a trade resets to the species base. NOTE: a minority (some
   * legendaries/others) have a different base in gSpeciesInfo, but that exact
   * per-species table isn't in the local decomp data — they reset to 70 too.
   * Refine here if the friendship column is added to gen_data.py/data_tables. */
  return 70;
}

/* ===================== genuine-trade save mutations ===================== */

#define TRADE_MON_SIZE      100
#define GAME_STAT_TRADES    21          /* same index in R/S, Emerald, FRLG */
/* Pokedex lives at SaveBlock2 + 0x18; owned/seen are +0x10/+0x44 within it. */
#define SB2_DEX_OWNED_OFF   (0x18 + 0x10)   /* = 0x28 */
#define SB2_DEX_SEEN_OFF    (0x18 + 0x44)   /* = 0x5C */

bool trade_layout(TradeGame g, TradeLayout* o) {
  switch (g) {
    case TG_RS:      o->party_count_off=0x234; o->party_off=0x238; o->gamestats_off=0x1540; o->seen1_off=0x938; o->seen2_off=0x3A8C; return true;
    case TG_EMERALD: o->party_count_off=0x234; o->party_off=0x238; o->gamestats_off=0x159C; o->seen1_off=0x988; o->seen2_off=0x3B24; return true;
    case TG_FRLG:    o->party_count_off=0x034; o->party_off=0x038; o->gamestats_off=0x1200; o->seen1_off=0x5F8; o->seen2_off=0x3A18; return true;
    default:         return false;
  }
}

const char* trade_game_name(TradeGame g) {
  switch (g) { case TG_RS: return "Ruby/Sapphire"; case TG_EMERALD: return "Emerald";
               case TG_FRLG: return "FireRed/LeafGreen"; default: return "Unknown"; }
}

TradeGame trade_detect_game(const uint8_t* save, int slot, const uint8_t* sb1) {
  bool is_frlg = false;
  PkMon tmp[6];
  pk_read_party_auto(sb1, tmp, &is_frlg);
  if (is_frlg) return TG_FRLG;                                  /* FRLG party @ 0x34 */
  /* RSE: RS validates section-0's checksum at SaveBlock2 size 0x890; Emerald only
   * from ~0xEFC, so 0x890 distinguishes them (FRLG already excluded). */
  if (gen3_section_checksum_ok(save, slot, G3_SID_SAVEBLOCK2, 0x0890)) return TG_RS;
  if (gen3_section_checksum_ok(save, slot, G3_SID_SAVEBLOCK2, 0x0F24)) return TG_EMERALD;
  return TG_UNKNOWN;
}

/* Pointer into the physical sector backing a SaveBlock1 logical offset. */
static uint8_t* sb1_ptr(uint8_t* save, int slot, uint32_t off) {
  int sec = (int)(off / G3_SECTOR_DATA_SIZE);
  uint32_t within = off % G3_SECTOR_DATA_SIZE;
  int s = gen3_find_section(save, slot, G3_SID_SAVEBLOCK1_START + sec);
  if (s < 0) return 0;
  return save + (uint32_t)slot * G3_SLOT_BYTES + (uint32_t)s * G3_SECTOR_SIZE + within;
}
static uint8_t* sb2_ptr(uint8_t* save, int slot, uint32_t off) {
  int s = gen3_find_section(save, slot, G3_SID_SAVEBLOCK2);
  if (s < 0) return 0;
  return save + (uint32_t)slot * G3_SLOT_BYTES + (uint32_t)s * G3_SECTOR_SIZE + off;
}

uint32_t trade_encryption_key(const uint8_t* save, int slot, TradeGame g) {
  uint32_t off;
  if (g == TG_EMERALD)   off = 0x00AC;   /* SaveBlock2.encryptionKey */
  else if (g == TG_FRLG) off = 0x0F20;
  else                   return 0;       /* Ruby/Sapphire: no security key */
  const uint8_t* p = sb2_ptr((uint8_t*)save, slot, off);
  if (!p) return 0;
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void set_bit(uint8_t* base, uint16_t idx0) {        /* idx0 = national-1 */
  if (base) base[idx0 >> 3] |= (uint8_t)(1u << (idx0 & 7));
}
/* Recompute a touched section's footer checksum over the full 3968 data region.
 * Correct only when the section is zero-padded past its real data — guaranteed by
 * trade_sections_safe(), checked before any edit. */
static void recompute_section(uint8_t* save, int slot, int section_id) {
  int s = gen3_find_section(save, slot, section_id);
  if (s < 0) return;
  uint8_t* sec = save + (uint32_t)slot * G3_SLOT_BYTES + (uint32_t)s * G3_SECTOR_SIZE;
  uint16_t cs = gen3_checksum(sec, G3_SECTOR_DATA_SIZE);
  sec[G3_OFF_CHECKSUM]     = (uint8_t)cs;
  sec[G3_OFF_CHECKSUM + 1] = (uint8_t)(cs >> 8);
}

bool trade_sections_safe(const uint8_t* save, int slot) {
  /* Sections 1,2 are full (always 0xF80); 0 (SaveBlock2) and 4 (last SaveBlock1)
   * are partial — confirm their padding is zero so a recompute-over-3968 matches. */
  static const int ids[] = {0, 1, 2, 4};
  for (unsigned i = 0; i < sizeof(ids)/sizeof(ids[0]); i++)
    if (!gen3_section_checksum_ok(save, slot, ids[i], G3_SECTOR_DATA_SIZE)) return false;
  return true;
}

bool trade_apply_received(uint8_t* save, int slot, TradeGame g, int pslot,
                          uint16_t* out_final_species, bool* out_evolved) {
  TradeLayout L;
  if (!trade_layout(g, &L)) return false;
  if (pslot < 0 || pslot >= 6) return false;

  uint8_t* rec = sb1_ptr(save, slot, (uint32_t)(L.party_off + pslot * TRADE_MON_SIZE));
  if (!rec) return false;

  PkMon p;
  if (!pk_decode_mon(rec, true, &p)) return false;   /* empty/bad slot */

  /* friendship -> species base */
  pk_set_friendship(rec, species_base_friendship(p.species));

  /* trade evolution (re-decode to see the post-friendship state isn't needed; use p) */
  bool consume = false, evolved = false;
  uint16_t evo = trade_evolution(p.species, p.heldItem, &consume);
  uint16_t final_species = p.species;
  if (evo) { pk_evolve(rec, evo, consume); final_species = evo; evolved = true; }

  /* receiver Pokedex: seen (3 copies) + caught (1), by national number */
  uint16_t nat = pk_national_no(final_species);
  if (nat) {
    uint16_t i0 = (uint16_t)(nat - 1);
    set_bit(sb2_ptr(save, slot, SB2_DEX_OWNED_OFF), i0);
    set_bit(sb2_ptr(save, slot, SB2_DEX_SEEN_OFF),  i0);
    set_bit(sb1_ptr(save, slot, L.seen1_off), i0);
    set_bit(sb1_ptr(save, slot, L.seen2_off), i0);
  }

  /* bump the trade game-stat (cap 999, matching the game). Emerald & FRLG store
   * gameStats XOR'd with the SaveBlock2 security key; R/S stores them plaintext. */
  uint32_t key = trade_encryption_key(save, slot, g);
  uint8_t* gs = sb1_ptr(save, slot, (uint32_t)(L.gamestats_off + GAME_STAT_TRADES * 4));
  if (gs) {
    uint32_t v = ((uint32_t)gs[0] | ((uint32_t)gs[1] << 8) |
                  ((uint32_t)gs[2] << 16) | ((uint32_t)gs[3] << 24)) ^ key;   /* decrypt */
    if (v < 999) v++;
    v ^= key;                                                                 /* re-encrypt */
    gs[0] = (uint8_t)v; gs[1] = (uint8_t)(v >> 8); gs[2] = (uint8_t)(v >> 16); gs[3] = (uint8_t)(v >> 24);
  }

  /* recompute every touched section: 0 (dex SB2), 1 (party + seen1), 2 (gameStats),
   * 4 (seen2). party_off/seen1 are in section 1; gameStats in 2; seen2 in 4. */
  recompute_section(save, slot, 0);
  recompute_section(save, slot, 1);
  recompute_section(save, slot, 2);
  recompute_section(save, slot, 4);

  if (out_final_species) *out_final_species = final_species;
  if (out_evolved)       *out_evolved = evolved;
  return true;
}

bool trade_place_mon(uint8_t* save, int slot, TradeGame g, int pslot, const uint8_t* mon100) {
  TradeLayout L;
  if (!trade_layout(g, &L)) return false;
  if (pslot < 0 || pslot >= 6) return false;
  uint8_t* rec = sb1_ptr(save, slot, (uint32_t)(L.party_off + pslot * TRADE_MON_SIZE));
  if (!rec) return false;
  memcpy(rec, mon100, TRADE_MON_SIZE);   /* section checksum recomputed by trade_apply_received */
  return true;
}
