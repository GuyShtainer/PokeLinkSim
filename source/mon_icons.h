/* PokeLinkSim — species icon API (art-free).
 *
 * This build bundles NO sprite art: mon_icon_for() returns NULL unless a local
 * icon pack has been generated into source/mon_icons_data.h (which is gitignored
 * and must never be committed/redistributed). The UI falls back to short text
 * tags when no icon is present, so the public repo and the released ROM ship
 * zero copyrighted artwork.
 *
 * To build YOUR OWN copy with icons: run `python3 tools/gen_icons.py` with a
 * local Gen-3 sprite pack present; it writes the gitignored mon_icons_data.h. */
#ifndef MON_ICONS_H
#define MON_ICONS_H
#include <stdint.h>

#define MON_ICON_W 32
#define MON_ICON_H 32
#define MON_ICON_PIX 1024
#define MON_ICON_NONE 0xFFFFu
#define MON_ICON_MAX_SPECIES 411

/* 32x32 icon (1024 RGB15+alpha halfwords) for an internal Gen-3 species id,
   or NULL when no icon data is built in (the default, art-free, build). */
const uint16_t* mon_icon_for(uint16_t species);

#endif
