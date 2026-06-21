#include "savefile.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "log.h"
#include "record_mix.h" /* SecretBase (packed, 160 bytes) */
#include "sys.h"        /* EWRAM_BSS */
#include "trade.h"      /* trade_* (genuine trade) */
#include "gen3_mon.h"   /* pk_decode_mon */

/* Big scratch lives in EWRAM (.bss), never on the IWRAM stack. */
static uint8_t EWRAM_BSS s_sb1[G3_SAVEBLOCK1_BYTES];  /* reassembled SaveBlock1 */
static uint8_t EWRAM_BSS s_snap[2 * G3_SECTOR_SIZE];  /* snapshot of touched sectors */
static uint8_t EWRAM_BSS s_cmp[4096];                 /* re-read compare chunk */

/* Secret-base working sets (real SecretBase[] so they're naturally aligned).
 * s_basesA/B  : each save's ORIGINAL 20 bases (post own-party regen), captured
 *               before either file is written, so both mix directions read
 *               un-mixed inputs.
 * s_mergedA/B : each direction's merge output.
 * s_friend    : the mutable friend-scratch recordmix_run consumes per direction. */
static SecretBase EWRAM_BSS s_friend[G3_SECRET_BASES_COUNT];
static SecretBase EWRAM_BSS s_basesA[G3_SECRET_BASES_COUNT];
static SecretBase EWRAM_BSS s_basesB[G3_SECRET_BASES_COUNT];
static SecretBase EWRAM_BSS s_mergedA[G3_SECRET_BASES_COUNT];
static SecretBase EWRAM_BSS s_mergedB[G3_SECRET_BASES_COUNT];

const char* sf_status_str(SfStatus s) {
  switch (s) {
    case SF_OK:         return "OK";
    case SF_ERR_OPEN:   return "open failed";
    case SF_ERR_READ:   return "read error";
    case SF_ERR_WRITE:  return "write error";
    case SF_ERR_SIZE:   return "bad size";
    case SF_ERR_VERIFY: return "verify mismatch";
    case SF_ERR_BACKUP: return "backup failed";
    case SF_ERR_PARSE:  return "parse/validate failed";
    case SF_ERR_RENAME: return "rename failed";
    case SF_ERR_LAYOUT: return "layout error";
    default:            return "?";
  }
}

SfStatus sf_read_full(const char* path, uint8_t* buf, uint32_t cap,
                      uint32_t* out_size) {
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) return SF_ERR_OPEN;
  UINT br = 0;
  FRESULT fr = f_read(&f, buf, cap, &br);
  f_close(&f);
  if (fr != FR_OK) return SF_ERR_READ;
  if (out_size) *out_size = (uint32_t)br;
  return SF_OK;
}

/* Compare two open files byte-for-byte using the shared compare buffer. */
static SfStatus files_equal(const char* a, const char* b, bool* equal) {
  *equal = false;
  FIL fa, fb;
  if (f_open(&fa, a, FA_READ) != FR_OK) return SF_ERR_OPEN;
  if (f_open(&fb, b, FA_READ) != FR_OK) { f_close(&fa); return SF_ERR_OPEN; }

  static uint8_t EWRAM_BSS bufb[4096];
  SfStatus st = SF_OK;
  bool same = true;
  if (f_size(&fa) != f_size(&fb)) same = false;

  while (same) {
    UINT ra = 0, rb = 0;
    if (f_read(&fa, s_cmp, sizeof(s_cmp), &ra) != FR_OK) { st = SF_ERR_READ; break; }
    if (f_read(&fb, bufb, sizeof(bufb), &rb) != FR_OK)   { st = SF_ERR_READ; break; }
    if (ra != rb) { same = false; break; }
    if (ra == 0) break; /* both EOF */
    if (memcmp(s_cmp, bufb, ra) != 0) { same = false; break; }
  }
  f_close(&fa);
  f_close(&fb);
  if (st != SF_OK) return st;
  *equal = same;
  return SF_OK;
}

/* Copy src -> dst in 4 KiB chunks. */
static SfStatus copy_file(const char* src, const char* dst) {
  FIL fs, fd;
  if (f_open(&fs, src, FA_READ) != FR_OK) return SF_ERR_OPEN;
  if (f_open(&fd, dst, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
    f_close(&fs);
    return SF_ERR_OPEN;
  }
  SfStatus st = SF_OK;
  for (;;) {
    UINT br = 0, bw = 0;
    if (f_read(&fs, s_cmp, sizeof(s_cmp), &br) != FR_OK) { st = SF_ERR_READ; break; }
    if (br == 0) break;
    if (f_write(&fd, s_cmp, br, &bw) != FR_OK || bw != br) { st = SF_ERR_WRITE; break; }
  }
  f_close(&fs);
  if (f_close(&fd) != FR_OK && st == SF_OK) st = SF_ERR_WRITE;
  return st;
}

static bool file_exists(const char* path) {
  FILINFO fno;
  return f_stat(path, &fno) == FR_OK;
}

SfStatus sf_backup(const char* src_path, char* out_bak, unsigned out_bak_cap) {
  char bak[SF_PATH_MAX];
  bool chosen = false;
  for (int n = 0; n <= 20 && !chosen; n++) {
    if (n == 0) siprintf(bak, "%s.bak", src_path);
    else        siprintf(bak, "%s.bak%d", src_path, n);
    if (!file_exists(bak)) chosen = true; /* never overwrite an existing backup */
  }
  if (!chosen) {
    log_line("backup: no free .bak slot (kept existing backups)");
    return SF_ERR_BACKUP;
  }

  SfStatus st = copy_file(src_path, bak);
  if (st != SF_OK) {
    log_line("backup: copy failed (%s)", sf_status_str(st));
    return SF_ERR_BACKUP;
  }
  bool eq = false;
  st = files_equal(src_path, bak, &eq);
  if (st != SF_OK || !eq) {
    log_line("backup: verify failed");
    return SF_ERR_BACKUP;
  }
  if (out_bak && out_bak_cap) {
    strncpy(out_bak, bak, out_bak_cap - 1);
    out_bak[out_bak_cap - 1] = 0;
  }
  log_line("backup OK -> %s", bak);
  return SF_OK;
}

SfStatus sf_write_verified(const char* path, const uint8_t* buf, uint32_t len) {
  char tmp[SF_PATH_MAX];
  siprintf(tmp, "%s.tmp", path);

  /* 1) write temp */
  FIL f;
  if (f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return SF_ERR_OPEN;
  UINT bw = 0;
  FRESULT fr = f_write(&f, buf, len, &bw);
  FRESULT fc = f_close(&f);
  if (fr != FR_OK || bw != len || fc != FR_OK) {
    f_unlink(tmp);
    return SF_ERR_WRITE;
  }

  /* 2) re-read temp and byte-compare to the intended buffer */
  if (f_open(&f, tmp, FA_READ) != FR_OK) { f_unlink(tmp); return SF_ERR_OPEN; }
  uint32_t off = 0;
  bool ok = true;
  while (off < len) {
    UINT br = 0;
    uint32_t want = len - off;
    if (want > sizeof(s_cmp)) want = sizeof(s_cmp);
    if (f_read(&f, s_cmp, want, &br) != FR_OK || br != want) { ok = false; break; }
    if (memcmp(s_cmp, buf + off, br) != 0) { ok = false; break; }
    off += br;
  }
  f_close(&f);
  if (!ok) { f_unlink(tmp); return SF_ERR_VERIFY; }

  /* 3) swap into place (original only disappears once temp is verified) */
  f_unlink(path); /* ignore error if absent */
  if (f_rename(tmp, path) != FR_OK) return SF_ERR_RENAME;
  return SF_OK;
}

SfStatus sf_self_test(const char* path, Gen3Version version, uint8_t* work) {
  uint32_t size = 0;
  SfStatus st = sf_read_full(path, work, G3_SAVE_FILE_SIZE, &size);
  if (st != SF_OK) { log_line("selftest: read failed (%s)", sf_status_str(st)); return st; }
  log_line("selftest: read %u bytes", (unsigned)size);
  if (size < (uint32_t)G3_SLOT_BYTES) return SF_ERR_SIZE;

  Gen3SaveInfo info;
  if (!gen3_parse(work, size, &info)) { log_line("selftest: parse failed"); return SF_ERR_PARSE; }

  int fail = -1;
  if (!gen3_verify_full_checksums(work, info.slot, &fail))
    log_line("selftest: WARNING source checksum bad at section %d", fail);
  else
    log_line("selftest: source full-section checksums OK");

  if (!info.sb1_ok) { log_line("selftest: SaveBlock1 incomplete"); return SF_ERR_PARSE; }
  if (version == G3_VER_UNKNOWN) {
    log_line("selftest: version unknown, skipping secret-base round-trip");
  } else {
    int first_id, last_id;
    gen3_sb1_touch_sections(version, &first_id, &last_id);
    if (first_id != 2 || last_id != 3) { /* expected for both RS and Emerald */
      log_line("selftest: unexpected SB1 span %d..%d", first_id, last_id);
      return SF_ERR_LAYOUT;
    }

    /* snapshot the touched sectors (id 2, id 3) before we re-encode */
    uint32_t base = (uint32_t)info.slot * G3_SLOT_BYTES;
    int s2 = gen3_find_section(work, info.slot, 2);
    int s3 = gen3_find_section(work, info.slot, 3);
    if (s2 < 0 || s3 < 0) return SF_ERR_LAYOUT;
    memcpy(s_snap,                 work + base + (uint32_t)s2 * G3_SECTOR_SIZE, G3_SECTOR_SIZE);
    memcpy(s_snap + G3_SECTOR_SIZE, work + base + (uint32_t)s3 * G3_SECTOR_SIZE, G3_SECTOR_SIZE);

    /* reassemble -> round-trip every record through the packed struct (identity) */
    if (gen3_read_saveblock1(work, info.slot, s_sb1) != G3_SAVEBLOCK1_BYTES)
      return SF_ERR_LAYOUT;
    uint32_t off = gen3_secret_base_offset(version);
    for (int i = 0; i < G3_SECRET_BASES_COUNT; i++) {
      SecretBase rec;
      uint8_t* slot_ptr = s_sb1 + off + (uint32_t)i * G3_SECRET_BASE_SIZE;
      memcpy(&rec, slot_ptr, sizeof(rec));   /* decode  */
      memcpy(slot_ptr, &rec, sizeof(rec));   /* re-encode (identity) */
    }

    /* write the two SaveBlock1 sections back (recomputes their checksums) */
    if (gen3_write_full_section(work, info.slot, 2, s_sb1 + 1 * G3_SECTOR_DATA_SIZE) == 0xFFFFFFFFu ||
        gen3_write_full_section(work, info.slot, 3, s_sb1 + 2 * G3_SECTOR_DATA_SIZE) == 0xFFFFFFFFu)
      return SF_ERR_LAYOUT;

    /* compare rewritten sectors vs snapshot: identical, or differing ONLY in
     * the 2-byte checksum field (which means the source checksum was stale). */
    const uint8_t* now2 = work + base + (uint32_t)s2 * G3_SECTOR_SIZE;
    const uint8_t* now3 = work + base + (uint32_t)s3 * G3_SECTOR_SIZE;
    int bad = 0, cs_only = 0;
    for (int k = 0; k < G3_SECTOR_SIZE; k++) {
      bool is_cs = (k == G3_OFF_CHECKSUM || k == G3_OFF_CHECKSUM + 1);
      if (now2[k] != s_snap[k])                 { if (is_cs) cs_only++; else bad++; }
      if (now3[k] != s_snap[G3_SECTOR_SIZE + k]) { if (is_cs) cs_only++; else bad++; }
    }
    if (bad != 0) {
      log_line("selftest: FAIL secret-base round-trip (%d data byte diffs)", bad);
      return SF_ERR_VERIFY;
    }
    log_line("selftest: secret-base round-trip bit-exact%s",
             cs_only ? " (checksum corrected)" : "");
  }

  /* full-image write integrity: write to a temp, re-read, byte-compare. */
  char tmp[SF_PATH_MAX];
  siprintf(tmp, "%s.selftest", path);
  FIL f;
  if (f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return SF_ERR_OPEN;
  UINT bw = 0;
  FRESULT fr = f_write(&f, work, size, &bw);
  FRESULT fc = f_close(&f);
  if (fr != FR_OK || bw != size || fc != FR_OK) { f_unlink(tmp); return SF_ERR_WRITE; }

  if (f_open(&f, tmp, FA_READ) != FR_OK) { f_unlink(tmp); return SF_ERR_OPEN; }
  uint32_t pos = 0;
  bool ok = true;
  while (pos < size) {
    UINT br = 0;
    uint32_t want = size - pos;
    if (want > sizeof(s_cmp)) want = sizeof(s_cmp);
    if (f_read(&f, s_cmp, want, &br) != FR_OK || br != want) { ok = false; break; }
    if (memcmp(s_cmp, work + pos, br) != 0) { ok = false; break; }
    pos += br;
  }
  f_close(&f);
  f_unlink(tmp); /* clean up; never leave a stray file */
  if (!ok) { log_line("selftest: FAIL write/read round-trip"); return SF_ERR_VERIFY; }

  log_line("selftest: write/read round-trip OK (%u bytes)", (unsigned)size);
  log_line("selftest: PASS - original NOT modified");
  return SF_OK;
}

/* Write `work[0..size)` to "<path><suffix>", re-read it and byte-compare to
 * `work`, then delete it. Proves the SD write path on the real bytes without
 * touching the original. Returns SF_OK iff the round-trip matched. */
static SfStatus verify_image_roundtrip(const char* path, const char* suffix,
                                       const uint8_t* work, uint32_t size) {
  char tmp[SF_PATH_MAX];
  siprintf(tmp, "%s%s", path, suffix);
  FIL f;
  if (f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return SF_ERR_OPEN;
  UINT bw = 0;
  FRESULT fr = f_write(&f, work, size, &bw);
  FRESULT fc = f_close(&f);
  if (fr != FR_OK || bw != size || fc != FR_OK) { f_unlink(tmp); return SF_ERR_WRITE; }

  if (f_open(&f, tmp, FA_READ) != FR_OK) { f_unlink(tmp); return SF_ERR_OPEN; }
  uint32_t pos = 0;
  bool ok = true;
  while (pos < size) {
    UINT br = 0;
    uint32_t want = size - pos;
    if (want > sizeof(s_cmp)) want = sizeof(s_cmp);
    if (f_read(&f, s_cmp, want, &br) != FR_OK || br != want) { ok = false; break; }
    if (memcmp(s_cmp, work + pos, br) != 0) { ok = false; break; }
    pos += br;
  }
  f_close(&f);
  f_unlink(tmp);
  return ok ? SF_OK : SF_ERR_VERIFY;
}

/* Capture a player's identity (raw, charset-encoded bytes) from SaveBlock2 of
 * the image currently in `work`. */
static SfStatus capture_identity(const uint8_t* work, int slot, PlayerIdentity* id) {
  int s0 = gen3_find_section(work, slot, G3_SID_SAVEBLOCK2);
  if (s0 < 0) return SF_ERR_LAYOUT;
  const uint8_t* sb2 = work + (uint32_t)slot * G3_SLOT_BYTES + (uint32_t)s0 * G3_SECTOR_SIZE;
  id->gender = sb2[SB2_OFF_GENDER];
  memcpy(id->trainerId,   sb2 + SB2_OFF_TRAINER_ID, 4);
  memcpy(id->trainerName, sb2 + SB2_OFF_PLAYER_NAME, 7);
  return SF_OK;
}

/* Freshen base[0].party in s_sb1 from this save's LIVE party, replicating
 * SetPlayerSecretBaseParty. s_sb1 must already hold the reassembled SaveBlock1.
 * Only mutates record 0's party (inside section id 2 — already in the write
 * set). No-op if the player has no base. */
static void regen_own_party(uint8_t* sb1, Gen3Version ver, uint8_t omit_mask) {
  uint32_t off = gen3_secret_base_offset(ver);
  SecretBase* base0 = (SecretBase*)(sb1 + off);
  if (base0->secretBaseId == 0) return;
  Gen3LiveParty live;
  gen3_read_live_party(sb1, omit_mask, &live);
  sb_set_party_from_live(base0, &live);
}

/* Load a save into `work`, capture its identity, reassemble SaveBlock1 into
 * s_sb1, regenerate its OWN base party from the live party, and copy its 20
 * (now-freshened) secret bases out to `out`. */
static SfStatus load_save_bases(const char* path, Gen3Version ver, uint8_t omit_mask,
                                const SbPartyChoice* ovr,
                                uint8_t* work, Gen3SaveInfo* info, PlayerIdentity* id,
                                SecretBase* out) {
  uint32_t size = 0;
  SfStatus st = sf_read_full(path, work, G3_SAVE_FILE_SIZE, &size);
  if (st != SF_OK) return st;
  if (size < (uint32_t)G3_SLOT_BYTES) return SF_ERR_SIZE;
  if (!gen3_parse(work, size, info)) return SF_ERR_PARSE;
  if (!info->sb1_ok) return SF_ERR_PARSE;
  if (ver == G3_VER_UNKNOWN) return SF_ERR_LAYOUT;
  uint32_t off = gen3_secret_base_offset(ver);
  if (off == 0) return SF_ERR_LAYOUT;

  int fail = -1;
  if (!gen3_verify_full_checksums(work, info->slot, &fail))
    log_line("mix: WARNING %s source checksum bad at section %d", path, fail);

  if ((st = capture_identity(work, info->slot, id)) != SF_OK) return st;
  if (gen3_read_saveblock1(work, info->slot, s_sb1) != G3_SAVEBLOCK1_BYTES)
    return SF_ERR_LAYOUT;
  if (ovr && ovr->count > 0) {
    /* Explicit user choice (live party and/or PC boxes) overrides the default
     * live-party regen. base[0] (at `off`) is the player's own base. */
    memcpy(out, s_sb1 + off, (size_t)G3_SECRET_BASES_COUNT * G3_SECRET_BASE_SIZE);
    sb_set_party_from_choice(&out[0], ovr);
  } else {
    regen_own_party(s_sb1, ver, omit_mask);    /* freshen base[0] from live party */
    memcpy(out, s_sb1 + off, (size_t)G3_SECRET_BASES_COUNT * G3_SECRET_BASE_SIZE);
  }
  return SF_OK;
}

/* Splice a merged 20-base array back into a save and either validate-only
 * (dry-run: temp round-trip, original untouched) or commit (backup + write).
 * Re-reads `path` fresh so the merge result overwrites whatever is on disk. */
static SfStatus splice_one(const char* path, Gen3Version ver,
                           const SecretBase* merged, uint8_t* work,
                           bool do_write, bool do_backup) {
  uint32_t off = gen3_secret_base_offset(ver);
  if (off == 0) return SF_ERR_LAYOUT;

  uint32_t size = 0;
  SfStatus st = sf_read_full(path, work, G3_SAVE_FILE_SIZE, &size);
  if (st != SF_OK) return st;
  if (size < (uint32_t)G3_SLOT_BYTES) return SF_ERR_SIZE;
  Gen3SaveInfo info;
  if (!gen3_parse(work, size, &info) || !info.sb1_ok) return SF_ERR_PARSE;

  if (gen3_read_saveblock1(work, info.slot, s_sb1) != G3_SAVEBLOCK1_BYTES)
    return SF_ERR_LAYOUT;
  /* overwrite the WHOLE 20-base array (incl. the freshened base[0]) */
  memcpy(s_sb1 + off, merged, (size_t)G3_SECRET_BASES_COUNT * G3_SECRET_BASE_SIZE);

  if (gen3_write_full_section(work, info.slot, 2, s_sb1 + 1 * G3_SECTOR_DATA_SIZE) == 0xFFFFFFFFu ||
      gen3_write_full_section(work, info.slot, 3, s_sb1 + 2 * G3_SECTOR_DATA_SIZE) == 0xFFFFFFFFu)
    return SF_ERR_LAYOUT;

  int fail = -1;
  if (!gen3_verify_full_checksums(work, info.slot, &fail)) {
    log_line("mix: FAIL merged checksum bad at section %d (%s)", fail, path);
    return SF_ERR_VERIFY;
  }
  if (!gen3_parse(work, size, &info)) {
    log_line("mix: FAIL merged image no longer parses (%s)", path);
    return SF_ERR_PARSE;
  }

  if (!do_write) {
    st = verify_image_roundtrip(path, ".mixtest", work, size);
    if (st != SF_OK) { log_line("mix: dry-run round-trip failed (%s)", sf_status_str(st)); return st; }
    log_line("mix: validate OK %s (%u bytes, untouched)", path, (unsigned)size);
    return SF_OK;
  }

  if (do_backup) {
    char bak[SF_PATH_MAX];
    st = sf_backup(path, bak, sizeof(bak));
    if (st != SF_OK) { log_line("mix: backup failed for %s, aborting", path); return st; }
    st = sf_write_verified(path, work, size);
    if (st != SF_OK) { log_line("mix: write failed for %s (%s)", path, sf_status_str(st)); return st; }
    log_line("mix: COMMIT OK -> %s (backup %s)", path, bak);
  } else {
    /* QUICK MODE: caller opted out of the backup. The merged image is still
     * checksum-validated and round-tripped above, so a corrupt merge aborts;
     * only the recoverable .bak is skipped. No undo if the user dislikes it. */
    log_line("mix: QUICK COMMIT (NO BACKUP) -> %s", path);
    st = sf_write_verified(path, work, size);
    if (st != SF_OK) { log_line("mix: write failed for %s (%s)", path, sf_status_str(st)); return st; }
    log_line("mix: COMMIT OK (no backup) -> %s", path);
  }
  return SF_OK;
}

static void log_dir(const char* dir, const MixStats* s) {
  log_line("mix %s: import=%d dup=%d refreshed=%d used=%d%s%s", dir,
           s->imported, s->duplicates, s->refreshed, s->host_used,
           s->host_base_evicted ? " evict-own" : "", s->overflow ? " OVERFLOW" : "");
}

SfStatus sf_mix_bidir(const char* pathA, Gen3Version verA, uint8_t omitA,
                      const char* pathB, Gen3Version verB, uint8_t omitB,
                      bool commit, bool make_backup,
                      const SbPartyChoice* ovrA, const SbPartyChoice* ovrB,
                      uint8_t* work, MixStats* statsAtoB, MixStats* statsBtoA) {
  /* the secret-base array must live in SaveBlock1 sections 2..3 for both games */
  int f, l;
  gen3_sb1_touch_sections(verA, &f, &l); if (f != 2 || l != 3) return SF_ERR_LAYOUT;
  gen3_sb1_touch_sections(verB, &f, &l); if (f != 2 || l != 3) return SF_ERR_LAYOUT;

  /* --- Phase 1: snapshot BOTH saves' bases (post own-party regen, minus any
   *     user-omitted party mons) before any write, so each direction mixes from
   *     un-mixed inputs. --- */
  Gen3SaveInfo infoA, infoB;
  PlayerIdentity idA, idB;
  SfStatus st = load_save_bases(pathA, verA, omitA, ovrA, work, &infoA, &idA, s_basesA);
  if (st != SF_OK) { log_line("mix: load A failed (%s)", sf_status_str(st)); return st; }
  st = load_save_bases(pathB, verB, omitB, ovrB, work, &infoB, &idB, s_basesB);
  if (st != SF_OK) { log_line("mix: load B failed (%s)", sf_status_str(st)); return st; }
  log_line("mix: A slot=%d, B slot=%d snapshotted", infoA.slot, infoB.slot);

  /* --- Phase 2: merge BOTH directions in RAM from the originals. --- */
  MixStats sab, sba;
  memcpy(s_mergedA, s_basesA, sizeof(s_mergedA));
  memcpy(s_friend,  s_basesB, sizeof(s_friend));
  if (!recordmix_run(s_mergedA, &idA, verA, s_friend, verB, &sab)) return SF_ERR_LAYOUT;
  memcpy(s_mergedB, s_basesB, sizeof(s_mergedB));
  memcpy(s_friend,  s_basesA, sizeof(s_friend));
  if (!recordmix_run(s_mergedB, &idB, verB, s_friend, verA, &sba)) return SF_ERR_LAYOUT;
  log_dir("A<-B", &sab);
  log_dir("B<-A", &sba);
  if (statsAtoB) *statsAtoB = sab;
  if (statsBtoA) *statsBtoA = sba;

  /* --- Phase 3: validate BOTH spliced images (no writes yet). --- */
  st = splice_one(pathA, verA, s_mergedA, work, false, make_backup);
  if (st != SF_OK) return st;
  st = splice_one(pathB, verB, s_mergedB, work, false, make_backup);
  if (st != SF_OK) return st;

  if (!commit) {
    log_line("mix: DRY-RUN OK - neither save modified");
    return SF_OK;
  }

  /* --- Phase 4: both validated -> write both (each backed up first unless
   *     make_backup is false, i.e. quick mode). --- */
  st = splice_one(pathA, verA, s_mergedA, work, true, make_backup);
  if (st != SF_OK) return st;
  st = splice_one(pathB, verB, s_mergedB, work, true, make_backup);
  if (st != SF_OK) { log_line("mix: B write failed AFTER A committed%s", make_backup ? " (A has .bak)" : " (NO BACKUP)"); return st; }
  log_line("mix: COMMIT OK - both saves updated");
  return SF_OK;
}

/* ===================== genuine trade ==================================== */

/* Read `path`, splice `foreign` into `loc` (party slot or PC box), apply the genuine
 * receive effects, and validate the whole image; if do_commit, back up then
 * verified-write it. `has_tail` says whether foreign[80..99] is a real party tail
 * (party source) or must be computed for a party destination. Reuses the savefile
 * work buffer + s_sb1 scratch. */
static SfStatus trade_one_side_loc(const char* path, const TradeLoc* loc, const uint8_t* foreign,
                                   bool has_tail, bool do_commit, uint8_t* work,
                                   uint16_t* recv_final, bool* recv_evo) {
  uint32_t size = 0;
  SfStatus st = sf_read_full(path, work, G3_SAVE_FILE_SIZE, &size);
  if (st != SF_OK) return st;
  Gen3SaveInfo info;
  if (!gen3_parse(work, size, &info) || !info.sb1_ok) return SF_ERR_PARSE;
  int slot = info.slot;
  if (gen3_read_saveblock1(work, slot, s_sb1) != G3_SAVEBLOCK1_BYTES) return SF_ERR_LAYOUT;
  TradeGame g = trade_detect_game(work, slot, s_sb1);
  if (g == TG_UNKNOWN) return SF_ERR_LAYOUT;
  if (!trade_sections_safe_loc(work, slot, loc)) return SF_ERR_LAYOUT; /* zero-padding precond */
  if (!trade_receive_at(work, slot, g, loc, foreign, has_tail, recv_final, recv_evo))
    return SF_ERR_LAYOUT;
  /* corruption guard: the whole image must still validate */
  if (!gen3_verify_full_checksums(work, slot, NULL)) return SF_ERR_VERIFY;
  if (!trade_sections_safe_loc(work, slot, loc)) return SF_ERR_VERIFY;
  if (do_commit) {
    char bak[SF_PATH_MAX];
    st = sf_backup(path, bak, sizeof(bak));
    if (st != SF_OK) { log_line("trade: backup failed for %s", path); return st; }
    st = sf_write_verified(path, work, size);
    if (st != SF_OK) { log_line("trade: write failed for %s (%s)", path, sf_status_str(st)); return st; }
    log_line("trade: COMMIT OK -> %s (backup %s)", path, bak);
  }
  return SF_OK;
}

/* Extract one save's chosen mon (party slot or PC box) + game + species. Fills the
 * verbatim 100-byte record for a party loc (has_tail=true) or the 80-byte core for a
 * box loc (has_tail=false). */
static SfStatus trade_extract_loc(const char* path, const TradeLoc* loc, uint8_t* work,
                                  uint8_t out_rec[100], bool* out_has_tail,
                                  TradeGame* out_game, uint16_t* out_species) {
  uint32_t size = 0;
  SfStatus st = sf_read_full(path, work, G3_SAVE_FILE_SIZE, &size);
  if (st != SF_OK) return st;
  Gen3SaveInfo info;
  if (!gen3_parse(work, size, &info) || !info.sb1_ok) return SF_ERR_PARSE;
  int slot = info.slot;
  if (gen3_read_saveblock1(work, slot, s_sb1) != G3_SAVEBLOCK1_BYTES) return SF_ERR_LAYOUT;
  TradeGame g = trade_detect_game(work, slot, s_sb1);
  if (g == TG_UNKNOWN) return SF_ERR_LAYOUT;
  if (!trade_read_core(work, slot, g, loc, out_rec, out_has_tail, out_species))
    return SF_ERR_PARSE;   /* empty slot / egg / bad layout */
  *out_game = g;
  return SF_OK;
}

static int loc_tag(const TradeLoc* loc) { return loc->kind == TLOC_BOX ? 100 + loc->box : loc->pslot; }

SfStatus sf_trade(const char* pathA, const TradeLoc* locA,
                  const char* pathB, const TradeLoc* locB,
                  bool commit, uint8_t* work, TradeResult* out) {
  static uint8_t EWRAM_BSS monA[100], monB[100];
  bool tailA = false, tailB = false;
  TradeGame gA, gB; uint16_t givenA, givenB;
  SfStatus st;

  st = trade_extract_loc(pathA, locA, work, monA, &tailA, &gA, &givenA);
  if (st != SF_OK) { log_line("trade: extract A failed (%s)", sf_status_str(st)); return st; }
  st = trade_extract_loc(pathB, locB, work, monB, &tailB, &gB, &givenB);
  if (st != SF_OK) { log_line("trade: extract B failed (%s)", sf_status_str(st)); return st; }

  if (out) { out->gameA = gA; out->gameB = gB; out->givenA = givenA; out->givenB = givenB; }
  log_line("=== TRADE %s[%s]@%d <-> %s[%s]@%d (%s) ===",
           pathA, trade_game_name(gA), loc_tag(locA), pathB, trade_game_name(gB), loc_tag(locB),
           commit ? "commit" : "dry-run");

  /* --- dry-run BOTH sides first (validate spliced images; nothing written).
   * A receives B's mon at locA; B receives A's mon at locB. --- */
  uint16_t aF, bF; bool aE, bE;
  st = trade_one_side_loc(pathA, locA, monB, tailB, false, work, &aF, &aE);
  if (st != SF_OK) { log_line("trade: dry-run A failed (%s)", sf_status_str(st)); return st; }
  st = trade_one_side_loc(pathB, locB, monA, tailA, false, work, &bF, &bE);
  if (st != SF_OK) { log_line("trade: dry-run B failed (%s)", sf_status_str(st)); return st; }
  if (out) { out->a_recv_final = aF; out->b_recv_final = bF; out->a_evolved = aE; out->b_evolved = bE; }
  log_line("trade: dry-run OK (A gets %u%s, B gets %u%s)", aF, aE ? " evo" : "", bF, bE ? " evo" : "");

  if (!commit) return SF_OK;

  /* --- commit BOTH (each backed up first, verified write) --- */
  st = trade_one_side_loc(pathA, locA, monB, tailB, true, work, &aF, &aE);
  if (st != SF_OK) return st;
  st = trade_one_side_loc(pathB, locB, monA, tailA, true, work, &bF, &bE);
  if (st != SF_OK) { log_line("trade: B write failed AFTER A committed (A has .bak; mon may be cloned)"); return st; }
  log_line("trade: COMMIT OK - both saves updated");
  return SF_OK;
}
