# PokeLinkSim — licensing & attribution

## Project code
Everything under `source/` and the additive files `lib/flashcartio_write.{c,h}`,
`lib/fatfs/diskio_write.c` is licensed **GPL-3.0-or-later** — see [LICENSE](LICENSE)
for the full text. Copyright (c) 2025 Guy Shtainer.

## Save-format / link logic
The Gen-3 save parsing, the Record-Mixing merge (`source/record_mix.c`), and the
genuine-trade mutations were **re-implemented by reference to the public
[pret](https://github.com/pret) decompilations** (github.com/pret/pokeemerald,
github.com/pret/pokeruby) — used as a reference for structure layouts, offsets, and
algorithm behaviour. This is **not a clean-room process**: the structure and helper
naming follow the decompilation. However, **no decompilation source is bundled in
this repository or compiled into the ROM** — only this project's own
re-implementation is. pokeruby covers BOTH Pokémon Ruby and Sapphire. The pret
decompilations are unofficial reverse-engineering projects; consult them upstream.

## Artwork: bundled in releases, not in the source tree
This **repository / source tree** ships **no Pokémon sprite/icon art** and **no
verbatim in-game text** (item/move/ability descriptions) — both are gitignored, not
committed. The downloadable **release `.gba` bundles ripped Pokémon box-icon sprites**
(© Game Freak / The Pokémon Company) so the download "just works" — the same way
community tools like PKHeX ship sprites. The source builds with short text fallbacks
when the art is absent. Species/move/item *names* and factual data tables (base stats,
types, catch rates) are retained. To build the icons yourself, drop a Gen-3 sprite pack
locally and regenerate the gitignored blob (`source/mon_icons_data.h`) with
`python3 tools/gen_icons.py`; that generated art must not be redistributed.

## Vendored: gba-flashcartio  (`lib/`: flashcartio.*, sys.h, fatfs/, ezflashomega/, everdrivegbax5/)
Vendored from afska/gba-flashcartio. Copyright (c) afska. See
[LICENSE.gba-flashcartio](LICENSE.gba-flashcartio). It in turn bundles third-party
components credited below. The two FatFs config flags `FF_FS_READONLY` and
`FF_FS_MINIMIZE` were changed to `0` to enable writing; no other vendored source
was modified.

- **FatFs** (`lib/fatfs/`) — © ELM-ChaN, under the BSD-style FatFs license; the
  copyright/notice text is retained in the `lib/fatfs/ff.h` header.
- **EverDrive GBA X5 SD access** (`lib/everdrivegbax5/`) — originally authored by
  **krikzz** (Igor Golubovskiy, EverDrive; see the "Author: krik" file headers,
  2015), vendored via afska/gba-flashcartio. Refer to krikzz / gba-flashcartio
  upstream for the applicable license terms.
- **EZ-Flash Omega SD access** (`lib/ezflashomega/io_ezfo.c`) — derived from
  **ez-flash/omega-de-kernel** (`source/Ezcard_OP.c`, see the SOURCE comment in
  `io_ezfo.c`), © EZ-FLASH, under **Apache-2.0** — see
  [LICENSE.ezfo-disc_io](LICENSE.ezfo-disc_io). The technique was popularised by
  felixjones/ezfo-disc_io and forks.

## Trademarks & affiliation
Pokémon, Game Boy Advance, EZ-Flash, and EverDrive are trademarks of their
respective owners. This project is an **unofficial fan homebrew tool**, is **not
affiliated with, sponsored by, or endorsed by** Nintendo, Game Freak, or The Pokémon
Company. It bundles **no game ROM, BIOS, or decompilation source**; the release `.gba`
includes ripped Game Freak box-icon sprites (gitignored, not committed to this repo —
see "Artwork" above). You supply your own save files.
