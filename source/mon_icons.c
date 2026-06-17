/* PokeLinkSim — species icon accessor (art-free shim).
 *
 * No sprite art is bundled in this repository or the released ROM. If a local
 * icon pack was generated into source/mon_icons_data.h (gitignored; produced by
 * tools/gen_icons.py from a sprite pack you supply), it is compiled in for a
 * personal icon build. Otherwise mon_icon_for() returns NULL and the UI shows
 * short text tags instead of icons. */
#include "mon_icons.h"

#if defined(__has_include)
#  if __has_include("mon_icons_data.h")
#    include "mon_icons_data.h"   /* static mon_icon_data[][1024] + mon_icon_index[]; GITIGNORED */
#    define PLS_HAVE_ICON_DATA 1
#  endif
#endif

#ifdef PLS_HAVE_ICON_DATA
const uint16_t* mon_icon_for(uint16_t species) {
  if (species > MON_ICON_MAX_SPECIES) return 0;
  uint16_t idx = mon_icon_index[species];
  return (idx == MON_ICON_NONE) ? 0 : mon_icon_data[idx];
}
#else
/* Art-free build: no bundled icons. */
const uint16_t* mon_icon_for(uint16_t species) { (void)species; return 0; }
#endif
