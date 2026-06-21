# PokeLinkSim

> **Unofficial fan homebrew.** PokeLinkSim is **not affiliated with, sponsored by, or
> endorsed by** Nintendo, Game Freak, or The Pokémon Company. *Pokémon*, *Game Boy
> Advance*, *EZ-Flash*, and *EverDrive* are trademarks of their respective owners.
> This **repository** ships **no game ROM, BIOS, or decompilation source** — you bring
> your own save files. The downloadable release `.gba` bundles ripped Game Freak
> box-icon sprites (gitignored, not in the source tree). Licensing & credits:
> [ATTRIBUTION.md](ATTRIBUTION.md).

A Game Boy Advance homebrew that runs **on the cartridge** (EZ-Flash Omega / Omega DE)
and reproduces Pokémon Gen 3 **link features** between two `.sav` files — with no
second console, no link cable, and no PC. The name is **Poke** (Pokémon) + **Link**
(the cable/link features) + **Sim** (it reproduces the real link mechanics):

- **Record Mixing** — merges secret-base teams (species, moves, held items, levels,
  EVs) between two saves, clears `battledOwnerToday` so each base is immediately
  battleable, and updates counters. A faithful port of the in-game algorithm, run
  both ways.
- **Genuine trading** — a real 1-for-1 Pokémon swap between two saves (any Gen 3,
  including FR/LG), with trade-evolution and friendship handled the way the game does.

It reads the saves straight from the flashcart's microSD card and writes the
results back — every write goes through an immutable `.bak` backup + verified write.

---

## Status

**Working and hardware-validated** on a Game Boy Advance SP + EZ-Flash Omega DE:
both **Record Mixing** (bidirectional secret-base merge with a team picker) and
**genuine 1-for-1 trading** run end-to-end on real hardware, reading and writing
real `.sav` files on the microSD. Builds clean; the pure-C cores are covered by
PC-runnable unit tests (`tests/host_test.c`).

> **Future work**
> - Trade Pokémon to/from a **PC** (not just cart-to-cart on the SD card).

**This build: graphical multi-panel browser + bidirectional mix with team picker.**
A bitmap (GBA Mode 3) UI with filled panels and a highlighted selection. You
**browse the whole SD card** — folders plus only the valid Ruby/Sapphire/Emerald
saves, each tagged with its game and last-modified date, **sorted newest-first**.
As you scroll, a bottom panel **previews** the highlighted save (game, date,
trainer, party, and how many secret bases are **battleable now** — with an
approximate RTC "new day" hint). Pick **save 1** then **save 2**, confirm, then a
**two-column screen** lets you toggle (with **L**) exactly which of each save's
Pokémon are registered into the secret base sent to the other game. A
**non-destructive dry-run** previews both directions, then a confirm writes
**both** files (each backed up to an immutable `.bak` first). The merge is a
faithful port of the in-game algorithm and **clears `battledOwnerToday`**, so
every mixed base is immediately battleable.

**Live-team sync (the important fix):** a secret base's party snapshot
(`base[0].party`) is only refreshed by the game at link-mix time, so a normally
saved `.sav` carries a *stale* team. Before mixing, this tool **regenerates each
save's own base party from its live party** — decrypting the actual 6 party
Pokémon (species/moves/held item/level/EVs) — so the shared base reflects the
team that's in the save *right now*. (Validated against real saves: a Ruby save
whose stored base team was an old 3-Chansey snapshot correctly mixes in the
current team, Dragonite included.)

**File timestamps:** written/backed-up files are stamped from the flashcart's
**real-time clock** (the GBA S-3511A GPIO RTC the Omega DE emulates). If the cart
doesn't expose its RTC to homebrew, timestamps are left unset (no fake date).

It still includes the **non-destructive integrity self-test** (open a save with
`A`, press `R`) that runs the whole read → edit → checksum → write → re-read
pipeline on your real save and proves it's bit-exact **without modifying it**.

**Safety built in (so a save can't get corrupted):**
- immutable `.bak` backups (never overwrites an existing backup) of **both** saves,
- both merged images are validated (parse + checksums + temp round-trip) **before
  either file is written**,
- verified atomic-ish writes (write temp → byte-compare → swap; original
  untouched unless the new data verified),
- bit-exact sector writes with recomputed checksums (verified against the pret
  decomp and host tests),
- host-runnable tests (`tests/host_test.c`) you can run on your PC with no
  emulator — covering the merge, the Gen 3 party decryption, and (if you drop
  saves in `tests/fixtures/`) your real saves.

**Ruby and Sapphire** share one save format and are both supported (the secret
base array is at SaveBlock1 `0x1A08` for RS vs `0x1A9C` for Emerald; the player
party is at `0x238` in both).

The merge logic (`source/record_mix.c`) is this project's own re-implementation of
`ReceiveSecretBasesData` and its helpers — written **by reference to** the **public**
[pret](https://github.com/pret) decompilations (used for structure layouts, offsets,
and behaviour; structure and naming follow them — this is *not* a clean-room process,
but **no decomp code is bundled in this repo** or compiled into the ROM), reduced to
the single-friend case and run **both ways**. The Gen 3 party decryption + `SetPlayerSecretBaseParty`
port live in `source/gen3_save.c` / `source/record_mix.c`. The bidirectional
driver (`sf_mix_bidir`) and safe write pipeline are in `source/savefile.*`. All of
it is covered by PC-runnable unit tests in `tests/host_test.c` (no emulator).

> The compiled `PokeLinkSim.gba` is produced by building locally or with Docker
> (see below). The flashcart SD hardware is **not emulated** by mGBA/melonDS, so
> anything touching the card must be tested on the real Omega DE. Keep a backup
> of your microSD while developing.

---

## Build

### Option A — Docker (no toolchain install)
Requires only Docker. The image ships devkitARM + libtonc.

```bash
./build.sh
# -> PokeLinkSim.gba
```

### Option B — local devkitPro
Install devkitPro with the **gba-dev** group (`dkp-pacman -S gba-dev`), so you
have devkitARM and libtonc, with `$DEVKITPRO` / `$DEVKITARM` set, then:

```bash
make rebuild
# -> PokeLinkSim.gba
```

### Art: bundled in releases, not in the source tree
The downloadable release `.gba` includes ripped Pokémon **box-icon sprites** (© Game
Freak / The Pokémon Company) so the download "just works" — the same way community
tools like PKHeX ship sprites. Those assets are **not committed to this repository**:
they are gitignored, and only the release binary carries them. The source builds with
short text fallbacks without them. To build the icons yourself, drop a Gen-3 sprite
pack under `Gen 3 Sprite Pack V1/` and regenerate the gitignored blob:

```bash
python3 tools/gen_icons.py   # -> source/mon_icons_data.h (gitignored; needs Pillow)
make rebuild
```

---

## Run it on the Omega DE

1. Copy `PokeLinkSim.gba` to your microSD and launch it from the EZ-Flash menu
   (Clean Boot is fine).
2. The Gen 3 `.sav` files can be **anywhere on the card** — the app browses the
   whole SD (the Omega keeps saves in `/SAVER`).
3. **Browser:** **UP/DOWN** move (hold to repeat), **LEFT/RIGHT** jump to the
   first/last row on screen, **L/R** jump to the very top/bottom, **A** enters a
   folder or picks the highlighted save, **B** goes up a folder, **SELECT** runs
   the safe self-test on the highlighted save, **START** undoes SAVE 1. Only
   Ruby/Sapphire/Emerald saves are listed (each with its game + date); the bottom
   panel previews the highlighted save (party, time, battleable bases) while you
   pick save 1, then the right panel previews while you pick save 2.
4. **Confirm**, then on the **team screen**: **LEFT/RIGHT** switch between the two
   saves' columns, **UP/DOWN** move, **L** toggles whether a Pokémon is registered
   into the secret base, **A** accepts (you must keep at least one per save).
5. **Dry-run** preview of both directions appears; **A** to **commit** (both saves
   written, a `.bak` of each made first) or **B** to cancel.

> The mixed secret bases reflect each save's **current** party (the tool syncs
> the base team from the live party, minus any you unchecked), and the flashcart's
> RTC sets the written files' timestamps.
6. After a run, pull the SD and read `/pokelinksim_log.txt` — it captures everything
   the app did (handy for debugging on hardware).

---

## Logging (built in for debuggability)

Every step is logged to three places at once:

- **On screen** — the running log/status is drawn with tonc's text engine.
- **mGBA console** — if launched under mGBA, lines go to its debug log
  (auto-detected; ignored on hardware).
- **SD card** — flushed to `/pokelinksim_log.txt`, the persistent artifact you read
  back after testing on the real cart.

See `source/log.*`.

---

## How SD writing works here

GBA homebrew *can* write to the Omega's SD. The EZ-Flash sector write routine
(`_EZFO_writeSectors`, in `lib/ezflashomega/io_ezfo.c`) is real: it switches the
cart to OS mode, DMAs the buffer into the SD controller window, and issues the
write command. The vendored library shipped read-only only because its FatFs
binding never wired the write half. This project enables it additively:

- `lib/flashcartio_write.{c,h}` — `flashcartio_write_sector()` dispatch.
- `lib/fatfs/diskio_write.c` — FatFs `disk_write`, `disk_ioctl` (CTRL_SYNC),
  and `get_fattime()`.
- `lib/fatfs/ffconf.h` — `FF_FS_READONLY` and `FF_FS_MINIMIZE` set to `0`
  (the only edits to vendored files).

The Omega DE is the easy case for this (FRAM, no autosave-vs-SD timing clash the
original Omega had). Full notes and citations in `docs/NOTES.md`.

---

## Repo layout

```
source/                our code
  main.c               app flow: SD browser, previews, team picker, mix wiring
  ui.{c,h}             bitmap (Mode 3) UI: panels, boxes, selection bar, text
  gen3_save.{c,h}      Gen 3 parsing + party decryption + battleable/RTC-day calc
  savefile.{c,h}       backups, verified writes, self-test, sf_mix_bidir
  record_mix.{c,h}     SecretBase struct + merge + SetPlayerSecretBaseParty port
  gba_rtc.{c,h}        cartridge S-3511A GPIO RTC reader (file timestamps)
  log.{c,h}            mGBA + SD logger
tests/host_test.c      PC-runnable tests: merge, party decryption, fixtures
tests/fixtures/        (git-ignored) drop real saves here to test against them
lib/                   vendored gba-flashcartio (FatFs + EZ-Flash/Everdrive)
  flashcartio_write.*  + diskio_write.c   (our additive write path + get_fattime)
docs/NOTES.md          format details, memory plan, SD-write findings, roadmap
Makefile  build.sh     devkitARM/tonc build + Docker one-liner
```

Built with [afska/gba-flashcartio](https://github.com/afska/gba-flashcartio)
(which vendors ELM-ChaN's FatFs and **krikzz**'s EverDrive SD code), the EZ-Flash
Omega SD routine from [ez-flash/omega-de-kernel](https://github.com/ez-flash/omega-de-kernel)
(technique via [felixjones/ezfo-disc_io](https://github.com/felixjones/ezfo-disc_io)),
[libtonc](https://github.com/devkitPro/libtonc), and re-implemented by reference to
the [pret](https://github.com/pret) decompilations. Full credits + licenses in
[ATTRIBUTION.md](ATTRIBUTION.md).
