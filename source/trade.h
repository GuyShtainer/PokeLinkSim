#ifndef TRADE_H
#define TRADE_H

#include <stdint.h>
#include <stdbool.h>

/* Gen-3 trade genuineness data (pure C, host-testable). Species are the INTERNAL
 * Gen-3 index the save stores; items are standard item ids. */

/* If the (internal) `species` holding `held_item` trade-evolves, return the
 * evolved INTERNAL species and set *consume_item (true for trade-with-item
 * evolutions, which use the item up); otherwise return 0. */
uint16_t trade_evolution(uint16_t species, uint16_t held_item, bool* consume_item);

/* The base friendship a trade resets a received mon's friendship to. */
uint8_t species_base_friendship(uint16_t species);

/* ---- genuine-trade save mutations (4 game families incl. FRLG) ---- */

typedef enum { TG_UNKNOWN = 0, TG_RS, TG_EMERALD, TG_FRLG } TradeGame;

/* Per-game SaveBlock1 field offsets (Pokedex lives in SaveBlock2 at fixed offsets
 * shared by all games). */
typedef struct {
  uint16_t party_count_off;  /* SaveBlock1: playerPartyCount */
  uint16_t party_off;        /* SaveBlock1: playerParty[6]   */
  uint16_t gamestats_off;    /* SaveBlock1: gameStats[]      */
  uint16_t seen1_off;        /* SaveBlock1: dex seen copy 1  */
  uint16_t seen2_off;        /* SaveBlock1: dex seen copy 2  */
} TradeLayout;

TradeGame   trade_detect_game(const uint8_t* save, int slot, const uint8_t* sb1);
bool        trade_layout(TradeGame g, TradeLayout* out);   /* false for UNKNOWN */
const char* trade_game_name(TradeGame g);

/* SaveBlock2 security key that Emerald (@0xAC) and FRLG (@0xF20) XOR into money,
 * coins, and gameStats. Returns 0 for Ruby/Sapphire (no key) — i.e. plaintext. */
uint32_t    trade_encryption_key(const uint8_t* save, int slot, TradeGame g);

/* True if every section a trade will touch (0,1,2,4) is zero-padded past its real
 * data — i.e. its stored checksum equals the checksum over the full 3968-byte data
 * region. Must hold before editing so a recompute-over-3968 is bit-correct; if
 * false the caller MUST abort (don't risk corrupting the save). */
bool trade_sections_safe(const uint8_t* save, int slot);

/* Apply the genuine "received in a trade" effects to the mon now sitting in party
 * slot `pslot` of `save`/`slot` (game `g`): reset friendship to species base,
 * apply any trade evolution (consuming the held item where applicable), set the
 * receiver's Pokedex seen+caught (all copies) for the final species, and bump the
 * trade game-stat. Recomputes the touched section checksums (over 3968; the caller
 * must have confirmed trade_sections_safe). OT is untouched (outsider => EXP boost).
 * Returns false on a structural problem. */
bool trade_apply_received(uint8_t* save, int slot, TradeGame g, int pslot,
                          uint16_t* out_final_species, bool* out_evolved);

/* Copy a 100-byte party mon record into party slot `pslot` of `save`/`slot`
 * (game `g`). The mon keeps its own encryption (self-contained), so it's valid in
 * any Gen-3 game. Follow with trade_apply_received to apply genuine effects +
 * recompute checksums. Returns false on a structural problem. */
bool trade_place_mon(uint8_t* save, int slot, TradeGame g, int pslot, const uint8_t* mon100);

/* ---- trade endpoints that can be a live party slot OR a PC-box slot ----
 * The exchanged unit is the self-contained 80-byte mon "core" (byte-identical in
 * box and party records); a PARTY destination additionally gets the 20-byte
 * plaintext battle-stats tail (level + the 6 stats, computed for a box source). */
typedef enum { TLOC_PARTY = 0, TLOC_BOX } TradeLocKind;
typedef struct {
  uint8_t kind;    /* TLOC_PARTY | TLOC_BOX                            */
  uint8_t pslot;   /* party slot 0..5            (kind == TLOC_PARTY)  */
  uint8_t box;     /* PC box 0..13               (kind == TLOC_BOX)    */
  uint8_t bslot;   /* PC box slot 0..29          (kind == TLOC_BOX)    */
} TradeLoc;

/* Like trade_sections_safe, but also covers the PC-storage section(s) a box trade
 * will rewrite. For a party loc it's exactly trade_sections_safe. */
bool trade_sections_safe_loc(const uint8_t* save, int slot, const TradeLoc* loc);

/* Read the giveable mon at `loc`. A party loc fills the verbatim 100-byte record
 * (*out_has_tail=true); a box loc fills the 80-byte core and zeroes [80..99]
 * (*out_has_tail=false). Rejects empty slots and eggs/bad-eggs. Out-pointers may
 * be NULL. Returns false on a structural problem. */
bool trade_read_core(const uint8_t* save, int slot, TradeGame g, const TradeLoc* loc,
                     uint8_t out_rec[100], bool* out_has_tail, uint16_t* out_species);

/* Place `foreign` at `loc` and apply the genuine "received in a trade" effects
 * (friendship reset, trade evolution, receiver Pokedex seen+caught, trade-counter
 * bump), recomputing every touched section. `foreign_has_tail` says whether
 * foreign[80..99] is a real party battle-stats tail (party source) or must be
 * computed when the destination is a party slot (box source). The caller MUST have
 * confirmed trade_sections_safe_loc. Friendship/evolution are applied to the mon's
 * own self-contained core, so a box record that straddles a section boundary is
 * handled correctly. Returns false on a structural problem. */
bool trade_receive_at(uint8_t* save, int slot, TradeGame g, const TradeLoc* loc,
                      const uint8_t foreign[100], bool foreign_has_tail,
                      uint16_t* out_final_species, bool* out_evolved);

#endif /* TRADE_H */
