#ifndef SAVEFILE_H
#define SAVEFILE_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_save.h"
#include "record_mix.h"  /* MixStats */
#include "trade.h"       /* TradeGame */

/* Max save path length. Sibling-file scratch (.tmp/.bak/.selftest/.mixtest)
 * adds a short suffix, so internal buffers are sized SF_PATH_MAX. */
#define SF_PATH_MAX 272

typedef enum {
  SF_OK = 0,
  SF_ERR_OPEN,     /* could not open a file                          */
  SF_ERR_READ,     /* read error / short read                        */
  SF_ERR_WRITE,    /* write error / short write                      */
  SF_ERR_SIZE,     /* file too small / wrong size                    */
  SF_ERR_VERIFY,   /* written bytes != intended bytes (round-trip)   */
  SF_ERR_BACKUP,   /* backup copy failed or mismatched               */
  SF_ERR_PARSE,    /* save did not validate                          */
  SF_ERR_RENAME,   /* final rename failed (temp left for recovery)   */
  SF_ERR_LAYOUT    /* internal: bad offsets / sector math            */
} SfStatus;

const char* sf_status_str(SfStatus s);

/* Read an entire file into buf (<= cap). Reports byte count via *out_size. */
SfStatus sf_read_full(const char* path, uint8_t* buf, uint32_t cap,
                      uint32_t* out_size);

/* Make an IMMUTABLE backup of src_path. Copies to "<src>.bak", or .bak1/.bak2…
 * if earlier ones exist (never overwrites an existing backup), then verifies
 * the copy byte-for-byte. The chosen path is written to out_bak. */
SfStatus sf_backup(const char* src_path, char* out_bak, unsigned out_bak_cap);

/* Write buf(len) to path safely:
 *   write "<path>.tmp" -> re-read & byte-compare to buf -> unlink(path)
 *   -> rename(tmp -> path). The original is untouched unless verification
 *   passed, so a failure never corrupts it. */
SfStatus sf_write_verified(const char* path, const uint8_t* buf, uint32_t len);

/* Non-destructive integrity self-test (the pre-hardware check):
 *   load -> validate -> reassemble SaveBlock1 -> round-trip the secret-base
 *   sectors through the packed struct (identity) -> confirm those sectors stay
 *   bit-identical (bar a corrected checksum) -> write the whole image to a temp
 *   file -> re-read & byte-compare -> delete temp.
 * The real .sav is NEVER modified. `work` is a caller-owned 128 KiB buffer. */
SfStatus sf_self_test(const char* path, Gen3Version version, uint8_t* work);

/* Bidirectional record-mix between two saves A and B: A's secret bases are
 * mixed into B AND B's into A, and BOTH files are updated.
 *
 * For each save the OWN secret-base party (base[0].party) is first regenerated
 * from that save's live party (the game only refreshes it at link-mix time, so
 * a normally-saved file carries a stale team) — this is what makes the shared
 * bases reflect the current team.
 *
 * Both saves' bases are snapshotted BEFORE either file is written, so each
 * direction mixes from un-mixed inputs. Then both spliced images are validated;
 * only if both pass does anything get written.
 *
 * If `commit` is false this is a DRY RUN: each merged image is round-tripped
 * through a throwaway temp and verified, but neither `.sav` is touched.
 * If `commit` is true: each save is written with the verified write path. Order
 * is A then B; if A fails nothing else is written.
 *
 * `make_backup` (only meaningful when `commit`): true = make an immutable `.bak`
 * before each write (the safe default). false = QUICK MODE — write in place with
 * NO backup. The merged image is still checksum-validated and round-tripped, so a
 * corrupt merge aborts; only the recoverable backup is skipped. DANGEROUS: there
 * is no undo. Callers must warn the user.
 *
 * `omitA`/`omitB` are 6-bit masks (bit = raw party slot) of mons to EXCLUDE from
 * each save's regenerated base party (0 = include the whole team). Ignored for a
 * save whose `ovr` is non-NULL with count>0.
 *
 * `ovrA`/`ovrB` (optional, NULL to skip) give an EXPLICIT base party chosen from
 * the live party and/or the PC boxes; when present (count>0) it replaces that
 * save's live-party regen entirely (the omit mask is then unused for that save).
 *
 * `work` is a caller-owned 128 KiB buffer. `statsAtoB`/`statsBtoA` (optional)
 * report each direction's merge. */
SfStatus sf_mix_bidir(const char* pathA, Gen3Version verA, uint8_t omitA,
                      const char* pathB, Gen3Version verB, uint8_t omitB,
                      bool commit, bool make_backup,
                      const SbPartyChoice* ovrA, const SbPartyChoice* ovrB,
                      uint8_t* work, MixStats* statsAtoB, MixStats* statsBtoA);

/* Mix 2..4 saves' secret bases together (real games mix up to 4 players): every
 * picked save receives every OTHER picked save's bases, and all are written. Same
 * dry-run/commit/backup/verified-write safety as sf_mix_bidir (all validated before
 * any write). `paths/vers/omits` are n-entry arrays; `ovrs` (or NULL) gives each
 * save's explicit base party (ovrs[i] NULL = its live-party regen with omits[i]).
 * `stats_out` (optional, n entries) reports each save's incoming-merge stats. */
SfStatus sf_mix_multi(const char* const paths[], const Gen3Version vers[], const uint8_t omits[],
                      const SbPartyChoice* const ovrs[], int n,
                      bool commit, bool make_backup, uint8_t* work, MixStats stats_out[]);

/* Genuine 1-for-1 trade between two saves (any Gen-3 game incl. FRLG): the mon at
 * `locA` of A swaps with the mon at `locB` of B. Each endpoint may be a live party
 * slot OR a PC-box slot (see TradeLoc), in any combination; the received mon lands
 * back in the slot its owner gave from. Each received mon keeps its OT (outsider =>
 * EXP boost) but gets friendship reset, trade evolution, the receiver's Pokedex
 * seen+caught, and a trade-counter bump. `commit == false` is a DRY RUN (validates
 * both spliced images, nothing written); `commit == true` backs up each save
 * (immutable .bak) then writes it with the verified-write path. Omega-only at the
 * call site. `work` is the caller's 128 KiB buffer; `out` (optional) reports what
 * each side gave/received. */
typedef struct {
  TradeGame gameA, gameB;
  uint16_t  givenA, givenB;            /* species each side handed over          */
  uint16_t  a_recv_final, b_recv_final;/* species each side ended up holding      */
  bool      a_evolved, b_evolved;      /* did the received mon trade-evolve?      */
} TradeResult;

SfStatus sf_trade(const char* pathA, const TradeLoc* locA,
                  const char* pathB, const TradeLoc* locB,
                  bool commit, uint8_t* work, TradeResult* out);

#endif /* SAVEFILE_H */
