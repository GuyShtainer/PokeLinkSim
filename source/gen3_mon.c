#include "gen3_mon.h"
#include "gen3_save.h"   /* gen3_decode_char (pure) */
#include <string.h>

/* --- little-endian readers (saves are LE) --- */
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Substruct slot order by personality % 24 — [Growth, Attacks, EVs]; the Misc
 * slot is the remaining one (slots sum to 0+1+2+3 = 6). Same table as
 * gen3_save.c, validated bit-exact against real saves. */
static const uint8_t k_substruct_pos[24][3] = {
  {0,1,2},{0,1,3},{0,2,1},{0,3,1},{0,2,3},{0,3,2},
  {1,0,2},{1,0,3},{2,0,1},{3,0,1},{2,0,3},{3,0,2},
  {1,2,0},{1,3,0},{2,1,0},{3,1,0},{2,3,0},{3,2,0},
  {1,2,3},{1,3,2},{2,1,3},{3,1,2},{2,3,1},{3,2,1},
};

static void decode_name(char* out, const uint8_t* src, int maxlen) {
  int k = 0;
  for (; k < maxlen; k++) {
    char ch = gen3_decode_char(src[k]);
    if (ch == 0) break;
    out[k] = ch;
  }
  out[k] = 0;
}

uint8_t pk_nature(uint32_t personality) { return (uint8_t)(personality % 25); }

bool pk_is_shiny(uint32_t personality, uint16_t tid, uint16_t sid) {
  uint16_t lo = (uint16_t)(personality & 0xFFFF);
  uint16_t hi = (uint16_t)(personality >> 16);
  return (uint16_t)(tid ^ sid ^ lo ^ hi) < 8;
}

uint8_t pk_gender_from(uint32_t personality, uint8_t gender_ratio) {
  if (gender_ratio == 0xFF) return 2;          /* genderless */
  if (gender_ratio == 0xFE) return 1;          /* always female */
  if (gender_ratio == 0x00) return 0;          /* always male */
  return (gender_ratio > (uint8_t)(personality & 0xFF)) ? 1 : 0;
}

uint16_t pk_calc_hp(uint8_t base, uint8_t iv, uint8_t ev, uint8_t level) {
  uint32_t v = (((uint32_t)(2 * base + iv + ev / 4) * level) / 100) + level + 10;
  return (uint16_t)v;
}

uint16_t pk_calc_stat(uint8_t base, uint8_t iv, uint8_t ev, uint8_t level, int nature_mod) {
  uint32_t v = (((uint32_t)(2 * base + iv + ev / 4) * level) / 100) + 5;
  if (nature_mod > 0)      v = v * 110 / 100;
  else if (nature_mod < 0) v = v * 90 / 100;
  return (uint16_t)v;
}

bool pk_decode_mon(const uint8_t* mon, bool is_party, PkMon* out) {
  memset(out, 0, sizeof(*out));
  out->isParty = is_party;
  out->raw = mon;

  uint8_t flags = mon[0x13];                     /* BoxPokemon flags byte */
  out->isBadEgg = (flags & 0x01) != 0;

  uint32_t pers = rd32(mon + 0x00);
  uint32_t otid = rd32(mon + 0x04);
  out->personality = pers;
  out->otId = otid;
  uint32_t key = pers ^ otid;

  uint8_t dec[48];
  for (int w = 0; w < 12; w++) {
    uint32_t word = rd32(mon + 0x20 + (uint32_t)w * 4) ^ key;
    dec[w * 4 + 0] = (uint8_t)word;
    dec[w * 4 + 1] = (uint8_t)(word >> 8);
    dec[w * 4 + 2] = (uint8_t)(word >> 16);
    dec[w * 4 + 3] = (uint8_t)(word >> 24);
  }

  uint16_t sum = 0;
  for (int h = 0; h < 24; h++) sum = (uint16_t)(sum + rd16(dec + h * 2));
  bool checksum_ok = (sum == rd16(mon + 0x1C));

  const uint8_t* pos = k_substruct_pos[pers % 24];
  int gslot = pos[0], aslot = pos[1], eslot = pos[2];
  int mslot = 6 - (gslot + aslot + eslot);
  const uint8_t* g = dec + (uint32_t)gslot * 12;   /* Growth  */
  const uint8_t* a = dec + (uint32_t)aslot * 12;   /* Attacks */
  const uint8_t* e = dec + (uint32_t)eslot * 12;   /* EVs     */
  const uint8_t* m = dec + (uint32_t)mslot * 12;   /* Misc    */

  uint16_t sp = rd16(g + 0);
  uint32_t ivword = rd32(m + 0x04);
  bool egg = ((flags & 0x04) != 0) || ((ivword >> 30) & 1);
  out->isEgg = egg;

  /* Empty slot: a zeroed record (pers/otid 0 → checksum_ok, species 0). */
  if (checksum_ok && sp == 0 && !egg) return false;

  /* Corrupt record → mark bad egg; fields below may be garbage but harmless. */
  if (!checksum_ok) out->isBadEgg = true;

  out->species     = sp;
  out->heldItem    = rd16(g + 0x02);
  out->experience  = rd32(g + 0x04);
  out->ppBonuses   = g[0x08];
  out->friendship  = g[0x09];
  for (int i = 0; i < 4; i++) { out->moves[i] = rd16(a + i * 2); out->pp[i] = a[0x08 + i]; }
  for (int i = 0; i < 6; i++) out->evs[i] = e[i];
  out->evSum = (uint16_t)(e[0] + e[1] + e[2] + e[3] + e[4] + e[5]);
  for (int i = 0; i < 6; i++) out->contest[i] = e[0x06 + i];

  out->pokerus     = m[0x00];
  out->metLocation = m[0x01];
  uint16_t origins = rd16(m + 0x02);
  out->metLevel = (uint8_t)(origins & 0x7F);
  out->metGame  = (uint8_t)((origins >> 7) & 0x0F);
  out->pokeball = (uint8_t)((origins >> 11) & 0x0F);
  out->otGender = (uint8_t)((origins >> 15) & 0x01);
  out->ivs[PK_HP]  = (uint8_t)(ivword & 0x1F);
  out->ivs[PK_ATK] = (uint8_t)((ivword >> 5) & 0x1F);
  out->ivs[PK_DEF] = (uint8_t)((ivword >> 10) & 0x1F);
  out->ivs[PK_SPE] = (uint8_t)((ivword >> 15) & 0x1F);
  out->ivs[PK_SPA] = (uint8_t)((ivword >> 20) & 0x1F);
  out->ivs[PK_SPD] = (uint8_t)((ivword >> 25) & 0x1F);
  out->abilityNum  = (uint8_t)((ivword >> 31) & 1);
  out->ribbons     = rd32(m + 0x08);

  out->nature  = (uint8_t)(pers % 25);
  out->isShiny = pk_is_shiny(pers, (uint16_t)(otid & 0xFFFF), (uint16_t)(otid >> 16));

  decode_name(out->nickname, mon + 0x08, 10);
  decode_name(out->otName, mon + 0x14, 7);

  if (is_party) {
    out->level = mon[0x54];
    out->stats[PK_HP]  = rd16(mon + 0x58);
    out->stats[PK_ATK] = rd16(mon + 0x5A);
    out->stats[PK_DEF] = rd16(mon + 0x5C);
    out->stats[PK_SPE] = rd16(mon + 0x5E);
    out->stats[PK_SPA] = rd16(mon + 0x60);
    out->stats[PK_SPD] = rd16(mon + 0x62);
  }
  /* box records: level + stats are computed later (needs base stats / growth rate). */
  return true;
}

static int read_party_at(const uint8_t* sb1, uint16_t count_off, uint16_t party_off,
                         PkMon out[6], int* goodness) {
  uint8_t count = sb1[count_off];
  int good = 0, kept = 0;
  if (count == 0 || count > 6) { if (goodness) *goodness = -1; return 0; }
  for (int i = 0; i < count; i++) {
    PkMon m;
    if (!pk_decode_mon(sb1 + party_off + (uint32_t)i * 100, true, &m)) continue;
    out[kept++] = m;
    if (!m.isBadEgg && m.species >= 1 && m.species <= 411 && m.level >= 1 && m.level <= 100)
      good++;
  }
  if (goodness) *goodness = good;
  return kept;
}

int pk_read_party(const uint8_t* sb1, bool frlg, PkMon out[6]) {
  return read_party_at(sb1, frlg ? 0x0034 : 0x0234, frlg ? 0x0038 : 0x0238, out, NULL);
}

int pk_read_party_auto(const uint8_t* sb1, PkMon out[6], bool* is_frlg) {
  PkMon rse[6], frlg[6];
  int g_rse = -1, g_frlg = -1;
  int n_rse  = read_party_at(sb1, 0x0234, 0x0238, rse, &g_rse);   /* R/S/E */
  int n_frlg = read_party_at(sb1, 0x0034, 0x0038, frlg, &g_frlg); /* FireRed/LeafGreen */
  if (g_frlg > g_rse) {
    memcpy(out, frlg, sizeof(frlg));
    if (is_frlg) *is_frlg = true;
    return n_frlg;
  }
  memcpy(out, rse, sizeof(rse));
  if (is_frlg) *is_frlg = false;
  return n_rse;
}

/* ---- in-place mon edits (for trading) ---------------------------------------
 * Operate on a raw 80/100-byte mon record. The 48-byte secure block is XOR-
 * encrypted with (personality ^ otId) and ordered by personality % 24; the 0x1C
 * checksum is the sum of its 24 decrypted halfwords. Edits decrypt, modify the
 * Growth substruct, re-encrypt, and recompute the checksum (so the record stays
 * valid). OT is never touched — that is what keeps a traded mon an "outsider". */

static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Decrypt rec's secure block into dec[48]; return the Growth substruct's byte
 * offset within dec (species@+0, heldItem@+2, exp@+4, ppBonuses@+8, friendship@+9). */
static int mon_decrypt_block(const uint8_t* rec, uint8_t dec[48]) {
  uint32_t key = rd32(rec) ^ rd32(rec + 4);
  for (int w = 0; w < 12; w++) wr32(dec + w * 4, rd32(rec + 0x20 + (uint32_t)w * 4) ^ key);
  return k_substruct_pos[rd32(rec) % 24][0] * 12;   /* Growth slot */
}

/* Re-encrypt dec[48] back into rec and recompute the 0x1C checksum. */
static void mon_encrypt_block(uint8_t* rec, const uint8_t dec[48]) {
  uint32_t key = rd32(rec) ^ rd32(rec + 4);
  uint16_t sum = 0;
  for (int h = 0; h < 24; h++) sum = (uint16_t)(sum + rd16(dec + h * 2));
  wr16(rec + 0x1C, sum);
  for (int w = 0; w < 12; w++) wr32(rec + 0x20 + (uint32_t)w * 4, rd32(dec + w * 4) ^ key);
}

void pk_set_friendship(uint8_t* rec, uint8_t value) {
  uint8_t dec[48];
  int g = mon_decrypt_block(rec, dec);
  dec[g + 0x09] = value;
  mon_encrypt_block(rec, dec);
}

void pk_evolve(uint8_t* rec, uint16_t new_species, bool consume_item) {
  uint8_t dec[48];
  int g = mon_decrypt_block(rec, dec);
  wr16(dec + g + 0x00, new_species);
  if (consume_item) wr16(dec + g + 0x02, 0);   /* trade-with-item evos use up the item */
  mon_encrypt_block(rec, dec);
}

bool pk_checksum_ok(const uint8_t* rec) {
  uint8_t dec[48];
  (void)mon_decrypt_block(rec, dec);
  uint16_t sum = 0;
  for (int h = 0; h < 24; h++) sum = (uint16_t)(sum + rd16(dec + h * 2));
  return sum == rd16(rec + 0x1C);
}
