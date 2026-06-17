# Developer notes

Working reference for this project. Format facts here are taken from the pret
decomps in `reference/` — treat those files as the source of truth and confirm
against them (and against PKHeX) before trusting any offset.

## 1. Can a GBA homebrew write to the EZ-Flash Omega SD? Yes.

Verified by reading the driver source, not just forum posts:

- `_EZFO_writeSectors()` (`lib/ezflashomega/io_ezfo.c`, and the libfat-style
  twin in felixjones/ezfo-disc_io) is a real write: switch to OS mode, DMA the
  buffer into the SD controller window (`0x9E00000`), issue the write command.
  In the libfat variant the read and write commands differ by one bit
  (`*0x9640000 = 0x8000 + blocks` for write vs `blocks` for read), and the
  `DISC_INTERFACE` advertises `FEATURE_MEDIUM_CANREAD | FEATURE_MEDIUM_CANWRITE`.
- felixjones/ezfo-disc_io ships a file-IO example that does `fopen(name,"w")`
  + write + close end to end.
- The DLDI fork (ApacheThunder) works with GodMode9i, which writes/formats.

Constraints to respect:
- During any SD op the cart is in OS mode and **all ROM is inaccessible**, so
  the routines run from EWRAM with IRQs disabled (already handled: the funcs are
  tagged `EWRAM_CODE`, and the EZFO path saves/clears `REG_IME`).
- **PSRAM is read-only in game mode** — can't be used as scratch; we work in
  EWRAM/IWRAM only.
- The original Omega had an autosave-to-SD timing clash; the **Omega DE** does
  not (FRAM) and "works out of the box" for SD I/O.

Sources: gbatemp "EZ Flash Omega disc_io library project" (#511490), "DLDI driver
for EZ Flash Omega" (#657509), "EZ-Flash Omega: Writing in PSRAM" (#660062),
afska/gba-flashcartio and felixjones/ezfo-disc_io READMEs/source.

**Caveat:** the FatFs *write* path in this repo is newly wired and was never
exercised by upstream for EZ-Flash. Validate on hardware with a microSD backup.
The milestone-1 build proves it by writing `/pokelinksim_log.txt`.

## 2. Gen 3 save format (RSE)  — from reference/pokeemerald/save.{c,h}

- File 128 KiB. Two save slots: slot 1 = sectors 0–13, slot 2 = sectors 14–27.
  (28–29 Hall of Fame, 30 Trainer Hill, 31 Recorded Battle.)
- Sector = 4096 B: `SECTOR_DATA_SIZE` = 3968 (0xF80) data, then footer:
  - `0xFF4` u16 section id
  - `0xFF6` u16 checksum
  - `0xFF8` u32 signature == `0x08012025`
  - `0xFFC` u32 save counter (same across all sectors of a slot)
- Section ids: 0 = SaveBlock2 (trainer info), 1–4 = SaveBlock1, 5–13 = PokemonStorage.
- Within a slot the 14 sectors are rotated, so **find a section by scanning for
  its id**, don't assume position (`gen3_find_section`).
- Current slot = the valid slot with the greater counter.
- Checksum: sum the data as u32 words over `size/4`, then fold:
  `(sum>>16) + (sum&0xFFFF)`, truncated to u16 (`gen3_checksum`).

### SaveBlock2 trainer-info offsets
`0x00` playerName[7]+0xFF · `0x08` gender · `0x0A` trainerId[4] (TID lo/hi, SID
lo/hi) · `0x0E` playTimeHours u16 · `0x10` min · `0x11` sec.

### Charset (decode subset)
`0xFF` term · `0x00` space · `0xA1–0xAA` `0`–`9` · `0xBB–0xD4` `A`–`Z` ·
`0xD5–0xEE` `a`–`z`.

## 3. SecretBase struct (Emerald) — from reference/pokeemerald/global.h

Array `secretBases[20]` at **offset 0x1A9C inside SaveBlock1**, 160 bytes each.
Per entry:
`0x00` id · `0x01` flags{toRegister:4, gender:1, **battledOwnerToday:1**,
registryStatus:2} · `0x02` trainerName[7] · `0x09` trainerId[4] · `0x0D`
language · `0x0E` numSecretBasesReceived u16 · `0x10` numTimesEntered · `0x11`
unused · `0x12` decorations[16] · `0x22` decorationPositions[16] · `0x32`
pad[2] · `0x34` party:
`personality[6]u32, moves[24]u16, species[6]u16, heldItems[6]u16, levels[6]u8, EVs[6]u8`.

`0x1A9C` falls in the **2nd** SaveBlock1 sector and the array spans into the 3rd,
so it must be edited on a contiguously reassembled SaveBlock1
(`gen3_read_saveblock1`), then split back and rewritten.

> Ruby/Sapphire **confirmed**: array at SaveBlock1 **0x1A08** (`struct
> SecretBaseRecord`, reference/pokeruby/global.h), 20 x 160 bytes, also spanning
> SaveBlock1 sections 2-3 (both full). The record is byte-identical to Emerald's
> except offset 0x0D (Emerald `language`; RS padding), so one packed struct and
> one write path serve both. pokeruby builds **both Ruby and Sapphire**.

## 3b. Safety pipeline (source/savefile.c)

Built so a write can't silently corrupt a save:
- **Validation first** — `gen3_parse` picks the valid slot by counter, counts
  sections, and `gen3_verify_full_checksums` checks every full section's stored
  checksum; the offered version is a guess the user confirms in the UI.
- **Immutable backup** — `sf_backup` copies `<save>.bak` (or `.bak1`, `.bak2`…
  so an existing backup is never overwritten) and verifies the copy
  byte-for-byte before anything is changed.
- **Verified atomic-ish write** — `sf_write_verified` writes `<save>.tmp`,
  re-reads it and byte-compares to the intended image, and only then
  `unlink(save)` + `rename(tmp -> save)`. The original is intact unless the new
  data verified.
- **Bit-exact edits** — only the two SaveBlock1 sectors holding the array are
  rewritten; `gen3_write_full_section` overwrites just the 3968-byte data region
  and recomputes the checksum, preserving id/signature/counter/padding.
- **Non-destructive self-test** — `sf_self_test` runs the entire
  read→reassemble→round-trip→checksum→write→re-read path on the real save bytes
  and asserts the result is bit-identical, writing only a throwaway temp. This
  is the pre-hardware check; the real `.sav` is never touched.

## 4. Memory plan

Two full 128 KiB saves do not both fit in EWRAM (256 KiB) alongside FatFs and
stack. We don't need them whole: record mixing touches a bounded set of sections
(secret bases are ~3.2 KiB). Read just the needed sections from each file, merge
in RAM, recompute those sectors' checksums, write back only the changed 4 KiB
sectors. The proof build loads one 128 KiB image at a time into `g_save`
(plain `.bss`, EWRAM) which is fine.

## 5c. Milestone 4 — graphical UI, SD browser, team picker (IMPLEMENTED)

**Pending hardware validation** (esp. the new bitmap UI rendering).

- **Bitmap UI** (`source/ui.{c,h}`): switched from tiled-text to GBA **Mode 3**
  (`tte_init_bmp(DCNT_MODE3, &sys8Font, NULL)`); framebuffer is in VRAM (no EWRAM
  cost). Helpers draw filled panels (`m3_rect`/`m3_frame`), a solid
  selection-highlight bar, colored text (`tte_set_ink` + `RGB15`), and UTF-8-safe
  name truncation. Redraw is on-keypress (single buffer; ~2-3 ms/redraw).
- **SD browser** (`source/main.c`): navigates the whole card from `/` (folders +
  only valid Ruby/Sapphire/Emerald saves, each parsed during the scan to validate
  + label its game + cache a preview), **sorted newest-first** by DOS datetime.
  Two bottom **preview panels** show the highlighted save (game, date, trainer,
  party, `gen3_count_battleable`, and an approximate `gen3_day_passed` RTC hint).
  A two-save state machine picks save 1 (left panel) then save 2 (right panel).
  Everything now uses **full paths** (the `savefile.c` sibling-path scratch was
  widened to `SF_PATH_MAX` to avoid truncation/corruption).
- **Team picker**: a two-column checkbox screen (`party_select_screen`) →
  per-save 6-bit omit masks threaded through `gen3_read_live_party(omit_mask)` →
  `regen_own_party` → `sf_mix_bidir(...omitA...omitB...)`. Omit-all is blocked.
- **Battle status**: `gen3_count_battleable` (exact, from `battledOwnerToday`) +
  `gen3_day_passed` (best-effort: RTC day vs SB2 `localTimeOffset`/
  `lastBerryTreeUpdate` days at 0x98/0xA0 — the day epoch isn't fully recoverable
  from a save, so the "new day" hint is heuristic; the count is exact).
- New pure-C functions are host-tested (omit mask, display decode, `rtc_days`,
  `gen3_count_battleable`); the Ruby-fixture species-149 proof still passes.

## 5b. Milestone 3 — live-team fix, bidirectional mix, RTC (IMPLEMENTED)

Hardware testing of Milestone 2 surfaced a real bug and two feature gaps. All
fixed; **pending fresh hardware validation**.

**Bug: mixed teams were stale.** A secret base's party snapshot
(`secretBases[0].party`) is written ONLY by `SetPlayerSecretBaseParty()`, called
once at link-mix start (`record_mixing.c:222` Emerald / `:67` Ruby) — never on a
normal save or team change. So a normally-saved `.sav` carries an outdated team,
and copying it verbatim loses recent changes. Proven on the real fixtures: the
Ruby save's stored base team was an old 3×Chansey snapshot while its LIVE party
had a lv100 Dragonite (species 149).

  - **Fix:** before mixing, regenerate each save's own `base[0].party` from its
    LIVE party (SaveBlock1 `playerPartyCount@0x234`, `playerParty@0x238`, six
    100-byte `struct Pokemon`, same offsets RS/Emerald). `gen3_read_live_party`
    (in `source/gen3_save.c`) decrypts each mon — key = `personality ^ otId`,
    XOR the 48-byte secure block, substructs ordered by `personality % 24`,
    validate the substruct checksum, skip egg/bad-egg/empty — and
    `sb_set_party_from_live` (in `source/record_mix.c`) writes the compacted
    result, exactly like `SetPlayerSecretBaseParty`. The 24-entry substruct-order
    table was validated bit-exact against both real saves (every mon's checksum
    matched). `base[0]` is in SaveBlock1 section 2, already in the write set.

**Bidirectional mix.** `sf_mix_bidir` (in `source/savefile.c`) snapshots BOTH
saves' bases (post-regen) into `s_basesA/s_basesB` BEFORE writing either, merges
both directions in RAM from those originals (so B→A uses A's un-mixed bases),
validates both spliced images, then on commit backs up + writes both (A then B).
The UI (`mix_two_screen` in `source/main.c`) picks two saves, auto-detects each
version (L/R override), previews both directions, and confirms to commit.

**RTC timestamps.** `source/gba_rtc.c` reads the cartridge S-3511A RTC over the
gamepak GPIO (`0x080000C4`), which the Omega DE emulates; `get_fattime()` in
`lib/fatfs/diskio_write.c` packs it into the FatFs DOS datetime. "RTC only, no
fallback": if the cart doesn't answer, range-validation fails and it returns 0
(unset) rather than a fake date. **Risk:** the Omega may not expose GPIO RTC to
homebrew — confirm on hardware.

**Tests.** `tests/host_test.c` adds `test_party` (synthetic decrypt round-trip,
egg/empty/compaction) and `test_fixtures` (guarded on `tests/fixtures/*.sav`):
asserts the Ruby live party has species 149 and that regen restores it where the
stale base lacked it.

## 5. Milestone 2 — the mix (IMPLEMENTED)

Done in `source/record_mix.c` (merge) + `source/savefile.c` (I/O) + the mix flow
in `source/main.c`. Superseded by Milestone 3 above (one-way `sf_mix` replaced by
the bidirectional `sf_mix_bidir`, plus the live-party regeneration).

1. **UI** — open a save (= host), press `START` to mix, pick the friend save,
   confirm versions. A dry-run previews the result; a second confirm commits.
2. **Load** — friend: reassemble SaveBlock1 -> snapshot its `SecretBase[20]`.
   host: reassemble SaveBlock1 + read identity (gender/TID/name) from SaveBlock2.
3. **Merge** (`recordmix_run`) — faithful single-friend port of
   `ReceiveSecretBasesData` / registry helpers from
   `reference/pokeemerald/record_mixing.c` + `secret_base.c`: strip the host's
   own base from the friend's list, dedup by gender+trainerId+name keeping the
   higher `numSecretBasesReceived`, fill empty slots then evict unregistered
   ones (registered bases are preserved), **clear `battledOwnerToday`**,
   re-register + sort, bump host[0]'s `numSecretBasesReceived`.
   Cross-version (RS <-> Emerald) is handled, including the language byte.
4. **Write back** — only SaveBlock1 sections 2 & 3 (the array spans them for both
   games) are rewritten via `gen3_write_full_section`, recomputing checksums;
   footer id/signature/counter/padding preserved. The current slot already holds
   the highest counter, so no counter bump is needed.
5. **Safety** — dry-run round-trips the merged image through a throwaway temp and
   re-validates (parse + full checksums) without touching the `.sav`. Commit makes
   an immutable `.bak` first, then `sf_write_verified` (temp -> byte-compare ->
   swap). The friend save is only ever read.
6. **Tests** — `tests/host_test.c` covers the merge on the PC (import, refresh,
   dedup both directions, skip-own-base, overflow, same-location). Build the
   cartridge and validate the resulting `.sav` in PKHeX and on hardware.
