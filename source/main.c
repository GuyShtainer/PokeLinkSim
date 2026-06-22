/*
 * GBA Record Mixer — graphical multi-panel UI.
 *
 * Browse the whole SD card (folders + only valid Ruby/Sapphire/Emerald saves,
 * each tagged with game + last-modified date, newest first). Two bottom panels
 * preview the highlighted save (game, time, trainer, full party, and how many
 * secret bases are battleable now). Pick SAVE 1 then SAVE 2, confirm against a
 * two-panel party summary, choose which Pokemon are registered into the secret
 * base sent to the other game, dry-run, then commit (both saves backed up).
 *
 * Browser keys: UP/DOWN move (hold to repeat); LEFT/RIGHT jump to the first/last
 * row on screen; L/R jump to the very top/bottom; A enter-folder / pick; B up a
 * folder; SELECT self-test the highlighted save; START toggles QUICK MODE (before
 * SAVE 1 is picked) or undoes SAVE 1 (after). The browser reopens the last folder
 * visited (remembered in /pokelinksim.ini).
 *
 * QUICK MODE (red "QUICK" badge, persisted in /pokelinksim.ini): after picking two
 * saves it skips the picker, mixes using each save's saved team preset, and after
 * one RED no-backup confirmation writes both in place (NO backup) and reboots to
 * the EZ-Flash menu. Dangerous by design — there is no undo.
 *
 * Team-picker keys: D-pad move the cursor; A toggle a Pokemon in/out of the
 * secret-base team; SELECT view that Pokemon's stats; START begin the mix; B back.
 * In the stats view the D-pad flips between team members by their grid position
 * (no need to back out first), A toggles the shown mon in/out of the team (the
 * sprite greys out to match the grid); B or SELECT returns to the picker.
 *
 * The merge + live-team regeneration live in record_mix.c / gen3_save.c; the
 * safe write pipeline + bidirectional driver in savefile.c; the bitmap UI in ui.c.
 */

#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "flashcartio.h"
#include "ff.h"
#include "gen3_save.h"
#include "savefile.h"
#include "gba_rtc.h"
#include "log.h"
#include "ui.h"
#include "mon_icons.h"
#include "gen3_names.h"
#include "team_prefs.h"
#include "app_config.h"
#include "gen3_mon.h"     /* PkMon, pk_decode_mon, pk_read_party */
#include "gen3_box.h"     /* G3_IN_BOX, G3_TOTAL_BOXES, pk_resolve */
#include "data_tables.h"  /* pk_nature_name */
#include "sfx.h"          /* PSG jingles (Phase 3) */

/* All app files live in one folder (created at startup) so the SD root stays tidy.
 * Put PokeLinkSim.gba in /pokelinksim/ too and everything sits together. */
#define APP_DIR     "/pokelinksim"
#define LOG_PATH    APP_DIR "/log.txt"
#define PREFS_PATH  APP_DIR "/teams.json"
#define CONFIG_PATH APP_DIR "/settings.ini"
#define BR_MAX    128
#define BR_NAME   64
#define PATH_MAX  256

/* ---- layout (pixels) ---------------------------------------------------- */
#define HDR_Y       0
#define LIST_BOX_Y  8
#define LIST_BOX_H  70
#define LROW0_Y     11
#define LROW_H      8
#define LIST_ROWS   8
#define BP_Y        80
#define BP_H        71
#define LP_X        0
#define RP_X        121
#define BP_W        119
#define FOOT_Y      152

typedef struct {
  char        name[BR_NAME];
  bool        is_dir;
  u32         dosdt;            /* (fdate<<16)|ftime */
  Gen3Version ver;
  u8          game;            /* 0 Emerald,1 Ruby,2 Sapphire,3 Ruby/Sapp */
  char        trainer[8];
  u16         tid_public, tid_secret;
  u8          party_n;
  char        pnick[6][9];
  u8          plevel[6];
  u16         pspecies[6];
  u8          bases_total, bases_battleable;
  bool        day_passed;
  u8          tgame;          /* TradeGame (trade-mode browse: RS/E/FRLG)        */
} BrEntry;

static u8      EWRAM_BSS g_save[G3_SAVE_FILE_SIZE];
static u8      EWRAM_BSS g_sb1[G3_SAVEBLOCK1_BYTES];
static BrEntry EWRAM_BSS g_entries[BR_MAX];
static int     g_nentries = 0;
static char    EWRAM_BSS g_cwd[PATH_MAX];
static int     g_trade_mode = 0;   /* browse accepts ANY Gen-3 save (incl. FRLG) */

/* ---- small utilities ---------------------------------------------------- */

static char up1(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static void copy_trunc(char* dst, const char* src, int cap) {
  int i = 0;
  for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
  dst[i] = 0;
}

static bool contains_ci(const char* hay, const char* needle) {
  for (; *hay; hay++) {
    const char* a = hay; const char* b = needle;
    while (*b && up1(*a) == up1(*b)) { a++; b++; }
    if (!*b) return true;
  }
  return false;
}

static const char* ver_name(Gen3Version v) {
  switch (v) {
    case G3_VER_EMERALD: return "Emerald";
    case G3_VER_RS:      return "Ruby/Sapphire";
    default:             return "unknown";
  }
}

static const char* game_tag(const BrEntry* e) {
  if (e->is_dir) return "DIR";
  switch (e->game) {
    case 0:  return "Emer";
    case 1:  return "Ruby";
    case 2:  return "Sapp";
    default: return "R/S";
  }
}

static u8 derive_game(const char* name, Gen3Version ver) {
  if (ver == G3_VER_EMERALD) return 0;
  if (contains_ci(name, "RUBY") || contains_ci(name, "AXVE")) return 1;
  if (contains_ci(name, "SAPP") || contains_ci(name, "AXPE")) return 2;
  return 3;
}

static void dos_date(u32 d, int* mo, int* dy) {
  u32 fd = d >> 16; *mo = (int)((fd >> 5) & 0xF); *dy = (int)(fd & 0x1F);
}
static void dos_time(u32 d, int* hh, int* mm) {
  u32 ft = d & 0xFFFF; *hh = (int)((ft >> 11) & 0x1F); *mm = (int)((ft >> 5) & 0x3F);
}

static const char* base_name(const char* path) {
  const char* p = strrchr(path, '/');
  return p ? p + 1 : path;
}

static void vsync(void) { VBlankIntrWait(); key_poll(); }

static u16 wait_keys(u16 mask) {
  u16 hit;
  do { vsync(); hit = key_hit(mask); } while (!hit);
  return hit;
}

static int has_sav_ext(const char* n) {
  unsigned L = (unsigned)strlen(n);
  if (L < 4 || n[L - 4] != '.') return 0;
  return (n[L - 3] == 's' || n[L - 3] == 'S') &&
         (n[L - 2] == 'a' || n[L - 2] == 'A') &&
         (n[L - 1] == 'v' || n[L - 1] == 'V');
}

static bool at_root(void) { return g_cwd[0] == '/' && g_cwd[1] == 0; }

static bool path_join(const char* dir, const char* name, char* out) {
  unsigned dl = (unsigned)strlen(dir), nl = (unsigned)strlen(name);
  if (dl == 1 && dir[0] == '/') {
    if (1 + nl + 1 > PATH_MAX) return false;
    siprintf(out, "/%s", name);
  } else {
    if (dl + 1 + nl + 1 > PATH_MAX) return false;
    siprintf(out, "%s/%s", dir, name);
  }
  return true;
}

static void path_up(void) {
  int l = (int)strlen(g_cwd);
  if (l <= 1) return;
  int i = l - 1;
  while (i > 0 && g_cwd[i] != '/') i--;
  if (i == 0) g_cwd[1] = 0; else g_cwd[i] = 0;
}

static bool dir_exists(const char* path) {
  DIR d;
  if (f_opendir(&d, path) != FR_OK) return false;
  f_closedir(&d);
  return true;
}

/* Remember the current folder so the next launch reopens it (saves usually live
 * in the same place). Best-effort: a failed write just isn't remembered. */
static void persist_cwd(void) {
  config_set_last_dir(g_cwd);
  config_save(CONFIG_PATH);
}

static u32 load_path(const char* path) {
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) { log_line("open %s failed", path); return 0; }
  UINT br = 0;
  FRESULT fr = f_read(&f, g_save, sizeof(g_save), &br);
  f_close(&f);
  return (fr == FR_OK) ? (u32)br : 0;
}

static void show_msg(const char* title, const char* body) {
  ui_clear();
  ui_text(6, 70, UI_TITLE, title);
  if (body) ui_text(6, 84, UI_TEXT, body);
}

static void halt_msg(const char* msg) {
  log_line("HALT: %s", msg);
  log_flush_to_sd(LOG_PATH);
  ui_clear();
  ui_text(6, 60, UI_WARN, "HALT");
  ui_text(6, 76, UI_TEXT, msg);
  while (1) vsync();
}

/* ===================== browser scan + preview =========================== */

static bool fill_save_preview(BrEntry* e, const char* fname, u32 sz) {
  Gen3SaveInfo info;
  if (sz < (u32)G3_SLOT_BYTES || !gen3_parse(g_save, sz, &info) || !info.sb1_ok)
    return false;
  if (gen3_read_saveblock1(g_save, info.slot, g_sb1) != G3_SAVEBLOCK1_BYTES)
    return false;

  if (g_trade_mode) {
    /* Trade accepts ANY recognizable Gen-3 save (incl. FRLG, which has no secret
     * bases). Just enough preview for the list: trainer + a game tag. */
    TradeGame tg = trade_detect_game(g_save, info.slot, g_sb1);
    if (tg == TG_UNKNOWN) return false;
    e->tgame = (u8)tg;
    e->ver = (tg == TG_EMERALD) ? G3_VER_EMERALD : (tg == TG_RS ? G3_VER_RS : G3_VER_UNKNOWN);
    e->game = 0;            /* trade list tags from tgame; derive_game() is RSE-only */
    e->tid_public = info.tid_public;
    e->tid_secret = info.tid_secret;
    copy_trunc(e->trainer, info.trainer_name, sizeof(e->trainer));
    e->party_n = 0;
    return true;
  }

  Gen3Version ver = gen3_detect_game(g_sb1);   /* RS / Emerald, else exclude */
  if (ver == G3_VER_UNKNOWN) return false;      /* FRLG / no secret bases -> hide */

  e->ver  = ver;
  e->game = derive_game(fname, ver);
  e->tid_public = info.tid_public;
  e->tid_secret = info.tid_secret;
  copy_trunc(e->trainer, info.trainer_name, sizeof(e->trainer));

  {
    Gen3DisplayParty dp;
    gen3_read_live_party_display(g_sb1, &dp);
    e->party_n = (u8)dp.count;
    for (int i = 0; i < dp.count && i < 6; i++) {
      copy_trunc(e->pnick[i], dp.mon[i].nickname, 9);
      e->plevel[i] = dp.mon[i].level;
      e->pspecies[i] = dp.mon[i].species;
    }
    int total = 0;
    e->bases_battleable = (u8)gen3_count_battleable(g_sb1, ver, &total);
    e->bases_total = (u8)total;
  }

  GbaRtcTime t;
  if (gba_rtc_get(&t)) {
    int s0 = gen3_find_section(g_save, info.slot, G3_SID_SAVEBLOCK2);
    if (s0 >= 0) {
      const u8* sb2 = g_save + (u32)info.slot * G3_SLOT_BYTES + (u32)s0 * G3_SECTOR_SIZE;
      e->day_passed = gen3_day_passed(sb2, gen3_rtc_days(t.year, t.month, t.day));
    }
  }
  return true;
}

static int browse_scan(void) {
  show_msg("Scanning...", g_cwd);

  g_nentries = 0;
  DIR dir; FILINFO fno;
  if (f_opendir(&dir, g_cwd) != FR_OK) { log_line("opendir %s failed", g_cwd); return -1; }

  while (g_nentries < BR_MAX && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
    bool is_dir = (fno.fattrib & AM_DIR) != 0;
    if (!is_dir && !has_sav_ext(fno.fname)) continue;

    BrEntry e;
    memset(&e, 0, sizeof(e));
    copy_trunc(e.name, fno.fname, BR_NAME);
    e.is_dir = is_dir;
    e.dosdt  = ((u32)fno.fdate << 16) | fno.ftime;

    if (!is_dir) {
      char path[PATH_MAX];
      if (!path_join(g_cwd, fno.fname, path)) continue;
      u32 sz = load_path(path);
      if (!sz || !fill_save_preview(&e, fno.fname, sz)) continue;  /* skip non-RSE */
    }
    g_entries[g_nentries++] = e;
  }
  f_closedir(&dir);

  for (int i = 1; i < g_nentries; i++) {           /* newest-first */
    BrEntry tmp = g_entries[i];
    int j = i - 1;
    while (j >= 0 && g_entries[j].dosdt < tmp.dosdt) { g_entries[j + 1] = g_entries[j]; j--; }
    g_entries[j + 1] = tmp;
  }
  log_line("scan %s: %d entries", g_cwd, g_nentries);
  return g_nentries;
}

static int br_rows(void) { return g_nentries + (at_root() ? 0 : 1); }

static BrEntry* br_entry(int row) {
  int ei = at_root() ? row : row - 1;
  if (!at_root() && row == 0) return NULL;
  if (ei < 0 || ei >= g_nentries) return NULL;
  return &g_entries[ei];
}

/* Species "icon" with an art-free fallback. The public build bundles no sprite
 * art, so mon_icon_for() returns NULL and these draw a short text tag instead, so
 * Pokemon stay identifiable. A local icon build (tools/gen_icons.py ->
 * source/mon_icons_data.h, gitignored) restores real icons automatically. */
static void draw_mon16(int x, int y, uint16_t species) {
  const u16* ic = mon_icon_for(species);
  if (ic) { ui_icon16(x, y, ic); return; }
  char t[5]; const char* n = gen3_species_name(species);
  int i = 0; for (; i < 3 && n[i]; i++) t[i] = n[i]; t[i] = 0;
  ui_text(x, y + 4, UI_TEXT, t);
}
static void draw_mon32(int x, int y, uint16_t species, bool grey) {
  const u16* ic = mon_icon_for(species);
  if (ic) { if (grey) ui_icon32_grey(x, y, ic); else ui_icon32(x, y, ic); return; }
  char t[7]; const char* n = gen3_species_name(species);
  int i = 0; for (; i < 6 && n[i]; i++) t[i] = n[i]; t[i] = 0;
  ui_text(x, y + 12, grey ? UI_DIM : UI_TEXT, t);
}

/* Draw a save summary panel (trainer + game, time + battle status, full party). */
static void render_save_panel(int px, int py, int ph, const BrEntry* e, bool active,
                              const char* empty) {
  ui_panel(px, py, BP_W, ph, UI_PANEL, active ? UI_TITLE : UI_BORDER);
  int x = px + 3, y = py + 3;
  char line[40];

  if (!e || e->is_dir) { ui_text(x, y, UI_DIM, e && e->is_dir ? "(folder)" : empty); return; }

  siprintf(line, "%s %s", e->trainer[0] ? e->trainer : "?", game_tag(e));
  ui_text(x, y, UI_TITLE, line); y += 9;

  int hh, mm; dos_time(e->dosdt, &hh, &mm);
  siprintf(line, "%02d:%02d  btl %d/%d%s", hh, mm, e->bases_battleable, e->bases_total,
           e->day_passed ? " +day" : "");
  ui_text(x, y, (e->bases_battleable > 0 || e->day_passed) ? UI_OK : UI_DIM, line);
  y += 11;

  /* party as a strip of 16x16 icons (level under each) */
  int nshow = e->party_n > 6 ? 6 : e->party_n;
  for (int i = 0; i < nshow; i++) {
    draw_mon16(px + 2 + i * 19, y, e->pspecies[i]);
    siprintf(line, "%d", e->plevel[i]);
    ui_text(px + 2 + i * 19, y + 16, UI_DIM, line);
  }
  if (e->party_n == 0) ui_text(x, y, UI_DIM, "(no team)");

  /* full filename (wrapped across two lines) at the panel bottom, so near-
   * identical save names are distinguishable -- the list rows truncate it. */
  {
    const char* nm = e->name;
    char l1[16], l2[16];
    int a = 0; for (; a < 14 && nm[a]; a++)        l1[a] = nm[a];      l1[a] = 0;
    int b = 0; for (; b < 14 && nm[14 + b]; b++)   l2[b] = nm[14 + b]; l2[b] = 0;
    if (nm[14 + b]) l2[13] = '~';                  /* >28 chars: mark cut-off */
    ui_text(x, py + 52, UI_OK, l1);
    if (l2[0]) ui_text(x, py + 61, UI_OK, l2);
  }
}

/* QUICK MIX is persistent + destructive (no backup, overwrites in place, reboots).
 * Never flip it ON silently: explain it once and require an explicit A. */
static bool quick_enable_confirm(void) {
  ui_clear();
  ui_text(6,  8, UI_DANGER, "ENABLE QUICK MIX?");
  ui_text(6, 28, UI_TEXT,   "Uses each save's saved");
  ui_text(6, 40, UI_TEXT,   "team preset, skips picking,");
  ui_text(6, 52, UI_DANGER, "makes NO BACKUP, overwrites");
  ui_text(6, 64, UI_DANGER, "both saves, then reboots.");
  ui_text(6, 90, UI_OK,     "A = enable");
  ui_text(6,102, UI_WARN,   "B = keep it off");
  return (wait_keys(KEY_A | KEY_B) & KEY_A) != 0;
}

static void render_browser(int sel, int top, int stage, const BrEntry* lockA) {
  ui_clear();

  char line[48], nbuf[40], hdr[40];
  bool quick = config_get_quick_mode();
  siprintf(line, "MIXER  %s", g_cwd);
  ui_truncate(hdr, line, quick ? 22 : 29);
  ui_text(2, HDR_Y, UI_TITLE, hdr);
  if (quick) ui_text(192, HDR_Y, UI_DANGER, "QUICK");

  ui_panel(0, LIST_BOX_Y, 240, LIST_BOX_H, UI_PANEL, UI_BORDER);
  int rows = br_rows();
  for (int r = 0; r < LIST_ROWS; r++) {
    int row = top + r;
    if (row >= rows) break;
    int y = LROW0_Y + r * LROW_H;
    BrEntry* e = br_entry(row);
    u16 ink;
    if (!e) { siprintf(line, "[..] up"); ink = UI_WARN; }
    else if (e->is_dir) {
      ui_truncate(nbuf, e->name, 22);
      siprintf(line, "%-22s    DIR", nbuf); ink = UI_DIRCLR;
    } else {
      int mo, dy; dos_date(e->dosdt, &mo, &dy);
      ui_truncate(nbuf, e->name, 17);
      siprintf(line, "%-17s %02d-%02d %s", nbuf, mo, dy, game_tag(e)); ink = UI_SAVECLR;
    }
    ui_text_sel(3, y, 234, row == sel, ink, line);
  }
  if (g_nentries == 0)            /* folder may hold FRLG/other saves we hide */
    ui_text(40, LROW0_Y + 2 * LROW_H, UI_DIM, "No R/S/E saves here");

  BrEntry* cur = br_entry(sel);
  if (stage == 0) {
    render_save_panel(LP_X, BP_Y, BP_H, cur, true, "(folder)");
    render_save_panel(RP_X, BP_Y, BP_H, NULL, false, "SAVE 2 next");
  } else {
    render_save_panel(LP_X, BP_Y, BP_H, lockA, false, "");
    render_save_panel(RP_X, BP_Y, BP_H, cur, true, "(folder)");
  }

  ui_text(2, FOOT_Y, UI_DIM, stage == 1 ? "A pick SEL test START undo1"
                                        : "A pick SEL test START quick");
}

static void run_self_test(const BrEntry* e) {
  char path[PATH_MAX];
  if (!e || e->is_dir || !path_join(g_cwd, e->name, path)) return;
  show_msg("Self-test...", base_name(path));
  log_line("=== self-test: %s as %s ===", path, ver_name(e->ver));
  SfStatus st = sf_self_test(path, e->ver, g_save);
  log_line("self-test: %s", st == SF_OK ? "PASS" : sf_status_str(st));
  log_flush_to_sd(LOG_PATH);
  ui_clear();
  ui_text(6, 60, st == SF_OK ? UI_OK : UI_WARN,
          st == SF_OK ? "SELF-TEST PASS" : "SELF-TEST FAIL");
  ui_text(6, 76, UI_TEXT, st == SF_OK ? "(original untouched)" : sf_status_str(st));
  ui_text(6, 100, UI_DIM, "B = back");
  wait_keys(KEY_B);
}

/* Browse + pick two saves; also outputs each chosen entry (for the summaries). */
static bool browse_two(char* pathA, Gen3Version* verA, BrEntry* outA,
                       char* pathB, Gen3Version* verB, BrEntry* outB) {
  if (browse_scan() < 0) halt_msg("Cannot read SD root");
  int sel = 0, top = 0, stage = 0;
  BrEntry lockA;
  memset(&lockA, 0, sizeof(lockA));
  bool dirty = true;

  while (1) {
    int rows = br_rows();
    if (rows == 0) sel = 0; else if (sel >= rows) sel = rows - 1;
    if (sel < 0) sel = 0;
    if (sel < top) top = sel;
    if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;
    if (top < 0) top = 0;

    if (dirty) { render_browser(sel, top, stage, &lockA); dirty = false; }

    vsync();
    u16 mv  = key_repeat(KEY_UP | KEY_DOWN);
    u16 hit = key_hit(KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R | KEY_A | KEY_B | KEY_SELECT | KEY_START);
    if (!mv && !hit) continue;
    dirty = true;

    if (mv & KEY_DOWN)       { if (rows) sel = (sel + 1) % rows; }
    else if (mv & KEY_UP)    { if (rows) sel = (sel == 0) ? rows - 1 : sel - 1; }
    else if (hit & KEY_RIGHT){ sel += 7; if (sel >= rows) sel = rows - 1; }  /* page down */
    else if (hit & KEY_LEFT) { sel -= 7; if (sel < 0) sel = 0; }             /* page up   */
    else if (hit & KEY_L)    { sel = 0; }                                    /* top       */
    else if (hit & KEY_R)    { sel = rows ? rows - 1 : 0; }                  /* bottom    */
    else if (hit & KEY_SELECT) { run_self_test(br_entry(sel)); }
    else if (hit & KEY_START)  {
      if (stage == 1) stage = 0;                         /* undo SAVE 1 */
      else if (config_get_quick_mode()) {                /* turn OFF freely */
        config_set_quick_mode(false); config_save(CONFIG_PATH);
      } else if (quick_enable_confirm()) {               /* turn ON only after confirm */
        config_set_quick_mode(true); config_save(CONFIG_PATH);
      }
    }
    else if (hit & KEY_B)    { if (!at_root()) { path_up(); browse_scan(); sel = 0; top = 0; persist_cwd(); } else return false; }  /* B at root -> back to main menu */
    else if (hit & KEY_A) {
      BrEntry* e = br_entry(sel);
      if (!e) { if (!at_root()) { path_up(); browse_scan(); sel = 0; top = 0; persist_cwd(); } }
      else if (e->is_dir) {
        char np[PATH_MAX];
        if (path_join(g_cwd, e->name, np)) { strcpy(g_cwd, np); browse_scan(); sel = 0; top = 0; persist_cwd(); }
      } else {
        if (stage == 0) {
          if (!path_join(g_cwd, e->name, pathA)) continue;
          *verA = e->ver; lockA = *e; *outA = *e; stage = 1;
        } else {
          char tmp[PATH_MAX];
          if (!path_join(g_cwd, e->name, tmp)) continue;
          if (strcmp(tmp, pathA) == 0) continue;
          strcpy(pathB, tmp); *verB = e->ver; *outB = *e;
          return true;
        }
      }
    }
  }
}

/* ===================== party select + stats ============================= */

static bool read_display_party(const char* path, Gen3DisplayParty* dp) {
  u32 sz = load_path(path);
  Gen3SaveInfo info;
  if (!sz || !gen3_parse(g_save, sz, &info) || !info.sb1_ok) return false;
  if (gen3_read_saveblock1(g_save, info.slot, g_sb1) != G3_SAVEBLOCK1_BYTES) return false;
  gen3_read_live_party_display(g_sb1, dp);
  return true;
}

/* ---- unified party + PC-box team picker (Phase 1) ----------------------------
 * Per save, the user chooses UP TO 6 mons from the live party AND/OR any of the
 * 14 PC boxes, combined, to register into the secret base. PkMon-based so party
 * and box mons share one model; box levels/stats are computed (pk_resolve). */

#define TP_PARTY 0
#define TP_PC    1

typedef struct {
  uint8_t in_use;   /* 1 if this chosen slot holds a mon                       */
  uint8_t src;      /* TP_PARTY or TP_PC                                       */
  uint8_t box;      /* PC box index (src==TP_PC)                              */
  uint8_t slot;     /* party index, or box slot 0..29                          */
  PkMon   mon;      /* full snapshot, so the source needn't stay resident      */
} ChosenMon;

/* Mixer picks BOTH saves on one screen, each registering up to 6 from its own
 * party + PC boxes. Both parties are decoded up front (PkMon snapshots are value
 * copies that survive a g_save reload); only one save's file lives in g_save at a
 * time, reloaded when the user opens the OTHER save's PC box. */
static ChosenMon EWRAM_BSS g_chosen[2][6];             /* [save] -> chosen team */
static int                 g_nchosen[2];
static PkMon    EWRAM_BSS  g_pty[2][6];                 /* both decoded parties  */
static int                 g_npty[2];
static const char*         g_mpath[2];                  /* each save's path      */
static int                 g_mslot[2];                  /* each save's slot      */
static int                 g_loaded;                    /* save now in g_save (-1=none) */
static int                 g_curbox[2];                 /* current PC box per save */
static uint8_t  EWRAM_BSS  g_boxbytes[G3_IN_BOX * 80];  /* one box, raw (2400) */
static PkMon    EWRAM_BSS  g_boxmons[G3_IN_BOX];        /* one decoded box     */
static PkMon    EWRAM_BSS  g_party[6];                  /* trade picker's party */

/* Copy `len` bytes at logical offset `off` of the reassembled PC storage
 * (sections 5..13) straight out of the raw save — no 36 KiB reassembly buffer.
 * A box (2400 B) can straddle the 3968-B section boundary, so walk sections. */
static bool pc_read_range(const uint8_t* save, int slot, uint32_t off, uint32_t len, uint8_t* out) {
  const uint32_t nsec = (uint32_t)(G3_SID_PKMN_STORAGE_END - G3_SID_PKMN_STORAGE_START + 1);
  while (len) {
    uint32_t sec = off / G3_SECTOR_DATA_SIZE;
    uint32_t ino = off % G3_SECTOR_DATA_SIZE;
    if (sec >= nsec) return false;
    int s = gen3_find_section(save, slot, G3_SID_PKMN_STORAGE_START + (int)sec);
    if (s < 0) return false;
    const uint8_t* sd = save + (uint32_t)slot * G3_SLOT_BYTES + (uint32_t)s * G3_SECTOR_SIZE;
    uint32_t chunk = G3_SECTOR_DATA_SIZE - ino;
    if (chunk > len) chunk = len;
    memcpy(out, sd + ino, chunk);
    out += chunk; off += chunk; len -= chunk;
  }
  return true;
}

/* Decode box `box` into g_boxmons (empty slots -> species 0); returns occupancy. */
static int read_box_into(const uint8_t* save, int slot, int box) {
  for (int i = 0; i < G3_IN_BOX; i++) g_boxmons[i].species = 0;
  uint32_t off = 0x0004u + (uint32_t)box * G3_IN_BOX * 80u;  /* PokemonStorage.boxes */
  if (!pc_read_range(save, slot, off, (uint32_t)G3_IN_BOX * 80u, g_boxbytes)) return 0;
  int n = 0;
  for (int i = 0; i < G3_IN_BOX; i++) {
    if (pk_decode_mon(g_boxbytes + (uint32_t)i * 80u, false, &g_boxmons[i])) {
      pk_resolve(&g_boxmons[i]); n++;
    } else {
      g_boxmons[i].species = 0;
    }
  }
  return n;
}

static int chosen_find(int s, uint8_t src, uint8_t box, uint8_t slot) {
  for (int i = 0; i < 6; i++)
    if (g_chosen[s][i].in_use && g_chosen[s][i].src == src && g_chosen[s][i].slot == slot &&
        (src == TP_PARTY || g_chosen[s][i].box == box))
      return i;
  return -1;
}

/* Add/remove a mon from save `s`'s <=6 chosen set. Returns false only on a
 * failed ADD because that save's team is already full. */
static bool chosen_toggle(int s, uint8_t src, uint8_t box, uint8_t slot, const PkMon* m) {
  int idx = chosen_find(s, src, box, slot);
  if (idx >= 0) {                               /* remove + compact */
    for (int i = idx; i < 5; i++) g_chosen[s][i] = g_chosen[s][i + 1];
    g_chosen[s][5].in_use = 0;
    g_nchosen[s]--;
    return true;
  }
  if (g_nchosen[s] >= 6) return false;          /* full */
  ChosenMon* c = &g_chosen[s][g_nchosen[s]++];
  c->in_use = 1; c->src = src; c->box = box; c->slot = slot; c->mon = *m;
  return true;
}

static void build_choice(int s, SbPartyChoice* out) {
  memset(out, 0, sizeof(*out));
  int n = g_nchosen[s]; if (n > 6) n = 6;
  out->count = (uint8_t)n;
  for (int i = 0; i < n; i++) {
    const PkMon* m = &g_chosen[s][i].mon;
    out->species[i]     = m->species;
    out->heldItems[i]   = m->heldItem;
    out->levels[i]      = m->level;
    out->personality[i] = m->personality;
    out->evs[i]         = (uint8_t)(m->evSum / 6);   /* matches GetAverageEVs */
    for (int k = 0; k < 4; k++) out->moves[i * 4 + k] = m->moves[k];
  }
}

/* Full-screen stats for one mon (richer than party-only: nature + IV/EV sums).
 * `chosen` greys the sprite + dims the header when the mon is toggled out. */
static void render_mon_stats(const PkMon* m, const char* owner, bool chosen) {
  char line[48];
  ui_clear();
  ui_text(4, 2, UI_TITLE, "POKEMON");
  if (owner) ui_text(64, 2, UI_DIM, owner);
  draw_mon32(8, 14, m->species, !chosen);
  ui_text(48, 14, chosen ? UI_TEXT : UI_DIM, m->nickname[0] ? m->nickname : "?");
  ui_text(48, 26, UI_TITLE, gen3_species_name(m->species));
  siprintf(line, "Lv%d  %s  %s", m->level, pk_nature_name(m->nature), chosen ? "(in)" : "(out)");
  ui_text(48, 38, chosen ? UI_TEXT : UI_DIM, line);
  siprintf(line, "Held: %s", m->heldItem ? gen3_item_name(m->heldItem) : "(none)");
  ui_text(8, 52, UI_DIM, line);

  ui_text(8, 66, UI_TITLE, "Stats");
  siprintf(line, "HP %3d   Atk %3d", m->stats[0], m->stats[1]); ui_text(8, 78, UI_TEXT, line);
  siprintf(line, "Def %3d  Spe %3d", m->stats[2], m->stats[3]); ui_text(8, 90, UI_TEXT, line);
  siprintf(line, "SpA %3d  SpD %3d", m->stats[4], m->stats[5]); ui_text(8, 102, UI_TEXT, line);
  int ivsum = m->ivs[0]+m->ivs[1]+m->ivs[2]+m->ivs[3]+m->ivs[4]+m->ivs[5];
  siprintf(line, "IVsum %d   EVsum %d", ivsum, m->evSum); ui_text(8, 114, UI_DIM, line);

  ui_text(8, 124, UI_TITLE, "Moves");
  ui_text(8,   134, UI_TEXT, gen3_move_name(m->moves[0]));
  ui_text(8,   144, UI_TEXT, gen3_move_name(m->moves[1]));
  ui_text(124, 134, UI_TEXT, gen3_move_name(m->moves[2]));
  ui_text(124, 144, UI_TEXT, gen3_move_name(m->moves[3]));
  ui_text(2, FOOT_Y, UI_DIM, "D-pad next  A in/out  B back");
}

/* Flip through the occupied mons of the current view (party or one box) with the
 * D-pad; A toggles the shown mon in/out of the team; B or SELECT returns. */
static void team_stats_screen(int s, PkMon* arr, int nslots, uint8_t src, uint8_t box,
                              const char* owner, int* cur) {
  int c = *cur;
  bool dirty = true;
  while (1) {
    if (arr[c].species == 0) {                  /* land on the first occupied */
      int f = -1;
      for (int i = 0; i < nslots; i++) if (arr[i].species) { f = i; break; }
      if (f < 0) return;
      c = f;
    }
    bool chosen = chosen_find(s, src, box, (uint8_t)c) >= 0;
    if (dirty) { render_mon_stats(&arr[c], owner, chosen); dirty = false; }
    vsync();
    u16 hit = key_hit(KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_SELECT);
    if (!hit) continue;
    if (hit & (KEY_B | KEY_SELECT)) { *cur = c; return; }
    if (hit & KEY_A) {
      chosen_toggle(s, src, box, (uint8_t)c, &arr[c]); dirty = true;
    } else if (hit & (KEY_RIGHT | KEY_DOWN)) {
      for (int i = c + 1; i < nslots; i++) if (arr[i].species) { c = i; dirty = true; break; }
    } else if (hit & (KEY_LEFT | KEY_UP)) {
      for (int i = c - 1; i >= 0; i--) if (arr[i].species) { c = i; dirty = true; break; }
    }
  }
}

/* ---- combined two-save register screen (side-by-side) ----------------------
 * Both parties are shown at once (left = save 0, right = save 1). Shoulder L/R
 * opens the PC box of whichever save the cursor is on (its party row + box show
 * below). A toggles a mon in/out of THAT save's team (<=6 each); START commits. */

/* One save's 3x2 party grid + N/6 count, in a panel. fcell>=0 highlights a cell. */
static void render_team_side(int s, int bx, const char* owner, int fcell) {
  ui_panel(bx, 16, BP_W, 128, UI_PANEL, fcell >= 0 ? UI_TITLE : UI_BORDER);
  char hbuf[16], lab[12];
  ui_truncate(hbuf, owner, 12);
  ui_text(bx + 4, 19, UI_TITLE, hbuf);
  siprintf(lab, "%d/6", g_nchosen[s]);
  ui_text(bx + BP_W - 26, 19, g_nchosen[s] ? UI_OK : UI_WARN, lab);
  for (int cell = 0; cell < 6; cell++) {
    int cx = bx + 5 + (cell % 3) * 38;
    int cy = 34 + (cell / 3) * 50;
    if (cell == fcell) m3_frame(cx - 2, cy - 2, cx + 34, cy + 44, UI_TITLE);
    if (cell >= g_npty[s]) continue;
    bool ch = chosen_find(s, TP_PARTY, 0, (uint8_t)cell) >= 0;
    draw_mon32(cx, cy, g_pty[s][cell].species, !ch);
    siprintf(lab, "Lv%d", g_pty[s][cell].level);
    ui_text(cx, cy + 34, ch ? UI_TEXT : UI_DIM, lab);
  }
  if (g_npty[s] == 0) ui_text(bx + 4, 36, UI_DIM, "(no party)");
}

static void render_parties(int s, int cell, const char* ownerA, const char* ownerB,
                           const char* warn) {
  ui_clear();
  ui_text(2, 0, UI_TITLE, "REGISTER TEAMS");
  ui_text(140, 0, UI_DIM, "up to 6 each");
  render_team_side(0, LP_X, ownerA, s == 0 ? cell : -1);
  render_team_side(1, RP_X, ownerB, s == 1 ? cell : -1);
  ui_text(2, FOOT_Y, warn ? UI_WARN : UI_DIM,
          warn ? warn : "A pick L/R PC SEL stat START");
}

/* Box mode: the active save's party as a context row + its PC box grid below. */
static void render_box_view(int s, const char* owner, int bcur, const char* warn) {
  ui_clear();
  char line[40];
  { char ob[24]; ui_truncate(ob, owner, 22); ui_text(2, 0, UI_TITLE, ob); }
  siprintf(line, "%d/6", g_nchosen[s]);
  ui_text(206, 0, g_nchosen[s] ? UI_OK : UI_WARN, line);

  for (int i = 0; i < g_npty[s]; i++) {          /* party row (green = in team) */
    int x = 8 + i * 20, y = 12;
    draw_mon16(x, y, g_pty[s][i].species);
    if (chosen_find(s, TP_PARTY, 0, (uint8_t)i) >= 0) m3_rect(x + 11, y, x + 15, y + 4, UI_OK);
  }

  siprintf(line, "PC BOX %d", g_curbox[s] + 1);
  ui_text(2, 32, UI_TITLE, line);
  for (int sl = 0; sl < G3_IN_BOX; sl++) {       /* 6x5 box grid */
    int col = sl % 6, row = sl / 6;
    int x = 8 + col * 22, y = 44 + row * 18;
    if (sl == bcur) m3_frame(x - 1, y - 1, x + 17, y + 17, UI_TITLE);
    if (g_boxmons[sl].species) {
      draw_mon16(x, y, g_boxmons[sl].species);
      if (chosen_find(s, TP_PC, (uint8_t)g_curbox[s], (uint8_t)sl) >= 0)
        m3_rect(x + 11, y, x + 15, y + 4, UI_OK);
    }
  }
  const PkMon* f = g_boxmons[bcur].species ? &g_boxmons[bcur] : NULL;
  if (f) {
    ui_text(8, 138, UI_TITLE, gen3_species_name(f->species));
    siprintf(line, "Lv%d  %s", f->level, pk_nature_name(f->nature));
    ui_text(120, 138, UI_DIM, line);
  } else {
    ui_text(8, 138, UI_DIM, "(empty)");
  }
  ui_text(2, FOOT_Y, warn ? UI_WARN : UI_DIM,
          warn ? warn : "A in/out L/R back <>box SEL");
}

/* Load save `s`, verify it has its own Secret Base, decode its party, default-
 * select the live party. Leaves save `s` resident in g_save. */
static bool setup_save(int s, const BrEntry* e, const char* path, Gen3Version ver) {
  u32 sz = load_path(path);
  Gen3SaveInfo info;
  if (!sz || !gen3_parse(g_save, sz, &info) || !info.sb1_ok) {
    show_msg("Cannot read save", base_name(path));
    ui_text(6, 100, UI_DIM, "B = back"); wait_keys(KEY_B);
    return false;
  }
  int slot = info.slot;
  gen3_read_saveblock1(g_save, slot, g_sb1);

  /* The team only registers into the player's OWN secret base (record 0). If they
   * never built one in-game, sb_set_party_from_choice() would silently drop the
   * whole pick (guard on secretBaseId==0) -- refuse up front instead. */
  uint32_t sb_off = gen3_secret_base_offset(ver);
  if (sb_off == 0 || g_sb1[sb_off] == 0) {
    char who[28]; siprintf(who, "%s has no base.", e->trainer);
    show_msg("No Secret Base", who);
    ui_text(6, 96, UI_TEXT, "Build one in-game first.");
    ui_text(6, 116, UI_DIM, "B = back"); wait_keys(KEY_B);
    return false;
  }

  g_mpath[s] = path; g_mslot[s] = slot; g_loaded = s; g_curbox[s] = 0;
  for (int i = 0; i < 6; i++) g_pty[s][i].species = 0;
  g_npty[s] = pk_read_party(g_sb1, false, g_pty[s]);   /* RSE party (not FRLG) */
  for (int i = 0; i < 6; i++) g_chosen[s][i].in_use = 0;
  g_nchosen[s] = 0;

  /* Remember the last mix: pre-fill the selection from this player's saved team
   * preset (matched by species + nickname, exactly as QUICK MODE's omit_from_prefs
   * does), so the Pokemon you registered last time start selected and the rest grey
   * out. With no saved preset tp_apply keeps chk[]=true -> the whole party is
   * selected (first-time behaviour). You can still toggle freely before mixing. */
  {
    Gen3DisplayParty dp;
    memset(&dp, 0, sizeof(dp));
    int n = g_npty[s] < 6 ? g_npty[s] : 6;
    dp.count = n;
    for (int i = 0; i < n; i++) {
      dp.mon[i].species = g_pty[s][i].species;
      copy_trunc(dp.mon[i].nickname, g_pty[s][i].nickname, (int)sizeof(dp.mon[i].nickname));
      dp.mon[i].original_slot = (uint8_t)i;
    }
    bool chk[6];
    for (int i = 0; i < 6; i++) chk[i] = true;
    tp_apply(tp_find(e->game, e->trainer, e->tid_public), &dp, chk);
    for (int i = 0; i < n && g_nchosen[s] < 6; i++)
      if (chk[i]) chosen_toggle(s, TP_PARTY, 0, (uint8_t)i, &g_pty[s][i]);
  }
  return true;
}

/* Remember each save's PARTY keep/omit selection (matched by species+nick) so it
 * pre-fills next time and QUICK MODE reflects real choices. PC-box picks are not
 * part of the party-preset model, so only the live party's slots are recorded. */
static void remember_party_choice(int s, const BrEntry* e) {
  Gen3DisplayParty dp;
  memset(&dp, 0, sizeof(dp));
  int n = g_npty[s] < 6 ? g_npty[s] : 6;
  dp.count = n;
  bool chk[6];
  for (int i = 0; i < n; i++) {
    dp.mon[i].species = g_pty[s][i].species;
    copy_trunc(dp.mon[i].nickname, g_pty[s][i].nickname, (int)sizeof(dp.mon[i].nickname));
    dp.mon[i].original_slot = (uint8_t)i;
    chk[i] = chosen_find(s, TP_PARTY, 0, (uint8_t)i) >= 0;
  }
  tp_update(e->game, e->trainer, e->tid_public, e->tid_secret, &dp, chk);
}

/* Ensure save `s`'s file is the one resident in g_save (needed for PC box reads). */
static void ensure_loaded(int s) {
  if (g_loaded == s) return;
  load_path(g_mpath[s]);     /* reloads g_save; slot already cached in g_mslot[s] */
  g_loaded = s;
}

/* Pick BOTH saves' teams on one screen. False = user cancelled the whole mix. */
static bool pick_two_teams(const BrEntry* eA, const char* pathA, Gen3Version verA, SbPartyChoice* outA,
                           const BrEntry* eB, const char* pathB, Gen3Version verB, SbPartyChoice* outB) {
  g_loaded = -1;
  if (!setup_save(0, eA, pathA, verA)) return false;
  if (!setup_save(1, eB, pathB, verB)) return false;

  char ownerA[20], ownerB[20];
  siprintf(ownerA, "%s %s", eA->trainer, game_tag(eA));
  siprintf(ownerB, "%s %s", eB->trainer, game_tag(eB));

  int view = TP_PARTY;     /* TP_PARTY = both parties; TP_PC = active save's box */
  int gcol = 0, grow = 0;  /* party cursor spans both 3x2 grids (cols 0..5)       */
  int bcur = 0;            /* box cursor 0..29                                    */
  const char* warn = NULL;
  bool dirty = true;
  while (1) {
    int s    = gcol < 3 ? 0 : 1;
    int cell = grow * 3 + (gcol % 3);
    const char* owner = (s == 0) ? ownerA : ownerB;
    if (dirty) {
      if (view == TP_PARTY) render_parties(s, cell, ownerA, ownerB, warn);
      else                  render_box_view(s, owner, bcur, warn);
      dirty = false;
    }
    vsync();
    u16 mv  = key_repeat(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
    u16 hit = key_hit(KEY_A | KEY_B | KEY_START | KEY_SELECT | KEY_L | KEY_R);
    if (!mv && !hit) continue;
    dirty = true; warn = NULL;

    if (hit & KEY_START) {
      if (g_nchosen[0] == 0 || g_nchosen[1] == 0) { warn = "Pick >=1 per save!"; continue; }
      build_choice(0, outA); build_choice(1, outB);
      return true;
    }

    if (view == TP_PARTY) {
      if (hit & KEY_B) return false;               /* cancel the whole mix */
      else if (hit & (KEY_L | KEY_R)) {            /* open this save's PC box */
        ensure_loaded(s);
        read_box_into(g_save, g_mslot[s], g_curbox[s]);
        bcur = 0; view = TP_PC;
      }
      else if (hit & KEY_SELECT) {
        if (g_npty[s]) {
          int cur = cell;
          team_stats_screen(s, g_pty[s], 6, TP_PARTY, 0, owner, &cur);
          gcol = s * 3 + (cur % 3); grow = cur / 3;
        }
      }
      else if (hit & KEY_A) {
        if (cell < g_npty[s] && !chosen_toggle(s, TP_PARTY, 0, (uint8_t)cell, &g_pty[s][cell]))
          warn = "Team is full (6)!";
      }
      else if (mv & KEY_LEFT)  { if (gcol > 0) gcol--; }
      else if (mv & KEY_RIGHT) { if (gcol < 5) gcol++; }
      else if (mv & (KEY_UP | KEY_DOWN)) grow ^= 1;
    } else {                                       /* box mode for save s */
      if (hit & (KEY_B | KEY_L | KEY_R)) { view = TP_PARTY; }
      else if (hit & KEY_SELECT) {
        int cur = bcur;
        team_stats_screen(s, g_boxmons, G3_IN_BOX, TP_PC, (uint8_t)g_curbox[s], owner, &cur);
        bcur = cur;
      }
      else if (hit & KEY_A) {
        if (g_boxmons[bcur].species &&
            !chosen_toggle(s, TP_PC, (uint8_t)g_curbox[s], (uint8_t)bcur, &g_boxmons[bcur]))
          warn = "Team is full (6)!";
      }
      else {
        int col = bcur % 6, row = bcur / 6;
        if (mv & KEY_LEFT) {
          if (col > 0) bcur--;
          else { g_curbox[s] = (g_curbox[s] + G3_TOTAL_BOXES - 1) % G3_TOTAL_BOXES;
                 read_box_into(g_save, g_mslot[s], g_curbox[s]); bcur = row * 6 + 5; }
        } else if (mv & KEY_RIGHT) {
          if (col < 5) bcur++;
          else { g_curbox[s] = (g_curbox[s] + 1) % G3_TOTAL_BOXES;
                 read_box_into(g_save, g_mslot[s], g_curbox[s]); bcur = row * 6; }
        } else if (mv & KEY_UP)   { bcur = (row > 0) ? bcur - 6 : bcur + 24; }
        else if (mv & KEY_DOWN)   { bcur = (row < 4) ? bcur + 6 : bcur - 24; }
      }
    }
  }
}

/* ===================== mix flow ========================================= */

static void result_screen(const char* title, u16 ink, const char* l1, const char* l2) {
  ui_clear();
  ui_text(6, 50, ink, title);
  if (l1) ui_text(6, 70, UI_TEXT, l1);
  if (l2) ui_text(6, 84, UI_TEXT, l2);
  char line[48];
  siprintf(line, "See %s", LOG_PATH);
  ui_text(6, 108, UI_DIM, line);
  ui_text(6, 124, UI_DIM, "B = back");
  wait_keys(KEY_B);
}

/* ---- optional animations + sound (Phase 3) ---------------------------------
 * All of this runs in the normal ROM-mapped, IRQs-on UI loop AROUND the SD write
 * (never during it). Toggleable via settings; hold R to fast-forward. */

static int  anim_fast(void) { return key_is_down(KEY_R) ? 3 : 1; }   /* hold R = speed up */
static void anim_frame(void) { vsync(); sfx_tick(); }

/* Play a jingle for `frames` frames (used after a mix; no visuals). Always a fixed
 * step so R (fast-forward, for visuals) never truncates an audio-only cue. */
static void play_jingle(int id, int frames) {
  sfx_play(id);
  for (int t = 0; t < frames; t++) anim_frame();
  sfx_silence();
}

/* Trade animation: the two mons slide in, flash, and swap out. */
static void trade_animation(uint16_t spA, uint16_t spB) {
  if (!config_get_anim_enabled()) { play_jingle(SFX_TRADE, 30); return; }  /* sound only */
  const u16* iA = mon_icon_for(spA);
  const u16* iB = mon_icon_for(spB);
  sfx_play(SFX_TRADE);
  for (int t = 0; t <= 30; t += anim_fast()) {              /* slide toward centre */
    ui_clear();
    ui_text(84, 8, UI_TITLE, "TRADING...");
    if (iA) ui_icon32(-32 + (132 * t) / 30, 64, iA);
    if (iB) ui_icon32(240 - (132 * t) / 30, 64, iB);
    ui_text(2, FOOT_Y, UI_DIM, "hold R = fast");
    anim_frame();
  }
  for (int t = 0; t < 6; t += anim_fast()) {                /* flash */
    m3_fill((t & 1) ? UI_TEXT : UI_BG);
    anim_frame();
  }
  for (int t = 0; t <= 30; t += anim_fast()) {              /* swap outward */
    ui_clear();
    ui_text(84, 8, UI_TITLE, "TRADING...");
    if (iA) ui_icon32(100 + (140 * t) / 30, 64, iA);
    if (iB) ui_icon32(108 - (140 * t) / 30, 64, iB);
    anim_frame();
  }
  sfx_silence();
}

/* Post-write celebration showing what each side received. */
static void trade_success_anim(uint16_t spA_recv, uint16_t spB_recv) {
  if (!config_get_anim_enabled()) { play_jingle(SFX_SUCCESS, 45); return; }
  const u16* iA = mon_icon_for(spA_recv);
  const u16* iB = mon_icon_for(spB_recv);
  sfx_play(SFX_SUCCESS);
  for (int t = 0; t < 55; t += anim_fast()) {
    ui_clear();
    ui_text(66, 24, UI_OK, "TRADE COMPLETE!");
    if (iA) ui_icon32(66, 78, iA);
    if (iB) ui_icon32(138, 78, iB);
    if (t & 8) { m3_frame(62, 74, 102, 114, UI_TITLE); m3_frame(134, 74, 174, 114, UI_TITLE); }
    anim_frame();
  }
  sfx_silence();
}

/* ---- quick mode: saved presets, one red confirm, no backup, reboot to menu ---- */

/* Omit mask for one save derived from its saved team preset (no preset => keep all;
 * never omit the whole team — that would leave an empty base party). */
static u8 omit_from_prefs(const BrEntry* e, const char* path) {
  Gen3DisplayParty dp;
  memset(&dp, 0, sizeof(dp));
  if (!read_display_party(path, &dp)) return 0;     /* unreadable -> keep all */
  bool chk[6];
  for (int i = 0; i < 6; i++) chk[i] = true;
  tp_apply(tp_find(e->game, e->trainer, e->tid_public), &dp, chk);
  int keep = 0; u8 omit = 0;
  for (int i = 0; i < dp.count; i++) {
    if (chk[i]) keep++; else omit |= (u8)(1u << dp.mon[i].original_slot);
  }
  if (dp.count > 0 && keep == 0) return 0;          /* never omit everyone */
  return omit;
}

static bool quick_confirm(const BrEntry* a, const BrEntry* b) {
  ui_clear();
  ui_text(6, 4, UI_TITLE, "QUICK MIX");
  char line[48];
  siprintf(line, "1: %s %s", a->trainer, game_tag(a)); ui_text(6, 24, UI_TEXT, line);
  siprintf(line, "2: %s %s", b->trainer, game_tag(b)); ui_text(6, 36, UI_TEXT, line);
  ui_text(6, 54, UI_DIM, "Uses saved team presets.");
  ui_text(6, 78,  UI_DANGER, "DANGER: NO BACKUP is made.");
  ui_text(6, 90,  UI_DANGER, "Both saves overwritten in");
  ui_text(6, 102, UI_DANGER, "place - there is NO undo.");
  ui_text(6, 128, UI_OK,   "A = mix now");
  ui_text(6, 140, UI_WARN, "B = cancel");
  u16 c = wait_keys(KEY_A | KEY_B);
  return (c & KEY_A) != 0;
}

static void quick_mix_flow(const char* pathA, Gen3Version verA, const BrEntry* a,
                           const char* pathB, Gen3Version verB, const BrEntry* b) {
  if (!quick_confirm(a, b)) return;
  u8 omitA = omit_from_prefs(a, pathA);
  u8 omitB = omit_from_prefs(b, pathB);
  show_msg("Quick mixing...", "(NO backup!)");
  log_line("=== QUICK MIX (no backup): %s[%s] <-> %s[%s] ===",
           pathA, ver_name(verA), pathB, ver_name(verB));
  log_line("QUICK omit masks (excluded party slots): SAVE1=0x%02x SAVE2=0x%02x",
           (unsigned)omitA, (unsigned)omitB);
  MixStats qa, qb;
  SfStatus cst = sf_mix_bidir(pathA, verA, omitA, pathB, verB, omitB,
                              true /*commit*/, false /*make_backup*/,
                              NULL, NULL /*no party override (quick uses prefs)*/,
                              g_save, &qa, &qb);
  log_line("quick mix: %s", cst == SF_OK ? "OK" : sf_status_str(cst));
  if (cst == SF_OK) {
    log_line("QUICK 1<-2: imported %d, dup %d, host %d/20%s",
             qa.imported, qa.duplicates, qa.host_used, qa.overflow ? " (OVERFLOW)" : "");
    log_line("QUICK 2<-1: imported %d, dup %d, host %d/20%s",
             qb.imported, qb.duplicates, qb.host_used, qb.overflow ? " (OVERFLOW)" : "");
  }
  log_flush_to_sd(LOG_PATH);
  if (cst != SF_OK) {
    result_screen("QUICK MIX FAILED", UI_DANGER, sf_status_str(cst), "Saves may be unmodified.");
    return;
  }
  ui_clear();
  ui_text(6, 60, UI_OK,   "MIX COMPLETE (no backup)");
  ui_text(6, 78, UI_TEXT, "Rebooting to flashcart...");
  sfx_play(SFX_SUCCESS);
  for (int i = 0; i < 120; i++) { vsync(); sfx_tick(); }   /* ~2s so the message is seen */
  sfx_silence();
  flashcartio_reboot();                     /* no return on Omega/EverDrive */
  /* Only reached if reboot is unsupported (no/unknown cart). */
  result_screen("MIX COMPLETE", UI_OK, "Both saves updated (no backup).",
                "Reboot-to-menu not supported here.");
}

/* Log a save's identity (path, game, trainer, IDs) so the SD log unambiguously
 * tells the two save files apart. Shared by the mix and trade flows. */
static void log_save_identity(const char* tag, const char* path, const char* game_str,
                              const BrEntry* e) {
  log_line("%s %s | game %s | trainer '%s' | TID %u (sec %u)",
           tag, path, game_str, e->trainer[0] ? e->trainer : "?",
           (unsigned)e->tid_public, (unsigned)e->tid_secret);
}

static void run_mix_flow(void) {
  if (active_flashcart != EZ_FLASH_OMEGA) {     /* mix writes -> Omega-only (rule #4) */
    show_msg("Record Mix is Omega-only", "EverDrive runs read-only.");
    ui_text(6, 100, UI_DIM, "B = back"); wait_keys(KEY_B);
    return;
  }
  char pathA[PATH_MAX], pathB[PATH_MAX];
  Gen3Version verA = G3_VER_UNKNOWN, verB = G3_VER_UNKNOWN;
  static BrEntry entA, entB;
  if (!browse_two(pathA, &verA, &entA, pathB, &verB, &entB)) return;

  log_line("=== RECORD MIX (%s mode) ===", config_get_quick_mode() ? "QUICK" : "regular");
  log_save_identity("MIX SAVE1", pathA, game_tag(&entA), &entA);
  log_save_identity("MIX SAVE2", pathB, game_tag(&entB), &entB);
  if (entA.tid_public == entB.tid_public && strcmp(entA.trainer, entB.trainer) == 0)
    log_line("MIX WARNING: SAVE1 & SAVE2 are the SAME trainer ('%s', TID %u) -- a secret "
             "base cannot mix into its own owner, so nothing new will ever import.",
             entA.trainer, (unsigned)entA.tid_public);
  log_flush_to_sd(LOG_PATH);

  if (config_get_quick_mode()) {
    quick_mix_flow(pathA, verA, &entA, pathB, verB, &entB);
    return;
  }

  /* Per-save team builder: pick up to 6 from the party AND/OR PC boxes (Phase 1).
   * These explicit choices drive each save's registered base party; the omit
   * masks are unused on this path (kept 0 for the call signature). */
  u8 omitA = 0, omitB = 0;
  static SbPartyChoice choiceA, choiceB;
  if (!pick_two_teams(&entA, pathA, verA, &choiceA,
                      &entB, pathB, verB, &choiceB)) return;
  const SbPartyChoice* ovrA = &choiceA;
  const SbPartyChoice* ovrB = &choiceB;

  show_msg("Dry-run mixing...", "(nothing written)");
  log_line("=== MIX2 dry-run: %s[%s] <-> %s[%s] ===",
           pathA, ver_name(verA), pathB, ver_name(verB));
  MixStats ab, ba;
  SfStatus st = sf_mix_bidir(pathA, verA, omitA, pathB, verB, omitB, false, true,
                             ovrA, ovrB, g_save, &ab, &ba);
  log_line("mix2 dry-run: %s", st == SF_OK ? "PASS" : sf_status_str(st));
  if (st == SF_OK) {
    log_line("MIX 1<-2: imported %d, dup %d, host %d/20%s",
             ab.imported, ab.duplicates, ab.host_used, ab.overflow ? " (OVERFLOW)" : "");
    log_line("MIX 2<-1: imported %d, dup %d, host %d/20%s",
             ba.imported, ba.duplicates, ba.host_used, ba.overflow ? " (OVERFLOW)" : "");
    if (ab.imported == 0 && ba.imported == 0)
      log_line("MIX note: 0 new bases either way -- these saves are already mixed (dup>0) "
               "or are the same trainer; re-mixing imports nothing new (this is normal).");
  }
  log_flush_to_sd(LOG_PATH);
  if (st != SF_OK) { result_screen("DRY-RUN FAILED", UI_WARN, sf_status_str(st), "Nothing written."); return; }

  ui_clear();
  ui_text(6, 6, UI_TITLE, "DRY-RUN OK (untouched)");
  char line[48];
  siprintf(line, "1<-2: +%d new  dup %d  %d/20%s", ab.imported, ab.duplicates, ab.host_used, ab.overflow ? " FULL" : "");
  ui_text(6, 28, UI_TEXT, line);
  siprintf(line, "2<-1: +%d new  dup %d  %d/20%s", ba.imported, ba.duplicates, ba.host_used, ba.overflow ? " FULL" : "");
  ui_text(6, 40, UI_TEXT, line);
  ui_text(6, 64, UI_OK,   "A = COMMIT (writes both,");
  ui_text(6, 76, UI_OK,   "    .bak each first)");
  ui_text(6, 92, UI_WARN, "B = cancel");
  u16 c = wait_keys(KEY_A | KEY_B);
  if (c & KEY_B) return;

  show_msg("Writing both saves...", NULL);
  log_line("=== MIX2 commit ===");
  SfStatus cst = sf_mix_bidir(pathA, verA, omitA, pathB, verB, omitB, true, true,
                              ovrA, ovrB, g_save, NULL, NULL);
  log_line("mix2 commit: %s", cst == SF_OK ? "OK" : sf_status_str(cst));
  log_flush_to_sd(LOG_PATH);

  if (cst == SF_OK) {
    /* Remember each side's party selection so it pre-fills (greyscale) next time
     * and QUICK MODE uses the real choices. Best-effort: a failed write just isn't
     * remembered and never blocks the completed mix. */
    remember_party_choice(0, &entA);
    remember_party_choice(1, &entB);
    tp_save(PREFS_PATH);
    play_jingle(SFX_SUCCESS, 45);
    result_screen("MIX COMPLETE", UI_OK, "Both saves updated & backed up.",
                  "Load in-game; bases battleable.");
  } else {
    result_screen("COMMIT FAILED", UI_WARN, sf_status_str(cst), "Backups kept.");
  }
}

/* ===================== trade flow ====================================== */

static const char* trade_tag(const BrEntry* e) {
  switch ((TradeGame)e->tgame) {
    case TG_RS:      return "R/S";
    case TG_EMERALD: return "Emer";
    case TG_FRLG:    return "FRLG";
    default:         return "?";
  }
}

#define TRADE_LIST_ROWS 13   /* one row reserved for the full-filename line */

static void render_trade_list(const char* title, int sel, int top) {
  ui_clear();
  char line[48], nbuf[40], hdr[40];
  siprintf(line, "%s  %s", title, g_cwd);
  ui_truncate(hdr, line, 29);
  ui_text(2, HDR_Y, UI_TITLE, hdr);
  ui_panel(0, 12, 240, 134, UI_PANEL, UI_BORDER);
  int rows = br_rows();
  for (int r = 0; r < TRADE_LIST_ROWS; r++) {
    int row = top + r;
    if (row >= rows) break;
    int y = 15 + r * 9;
    BrEntry* e = br_entry(row);
    u16 ink;
    if (!e) { siprintf(line, "[..] up"); ink = UI_WARN; }
    else if (e->is_dir) { ui_truncate(nbuf, e->name, 24); siprintf(line, "%-24s  DIR", nbuf); ink = UI_DIRCLR; }
    else { ui_truncate(nbuf, e->name, 15); siprintf(line, "%-15s %-7s %s", nbuf, e->trainer, trade_tag(e)); ink = UI_SAVECLR; }
    ui_text_sel(3, y, 234, row == sel, ink, line);
  }
  if (rows == 0) ui_text(40, 60, UI_DIM, "No Gen-3 saves here");
  BrEntry* curT = br_entry(sel);     /* full filename of the highlighted save */
  if (curT && !curT->is_dir) { char fn[32]; ui_truncate(fn, curT->name, 29); ui_text(4, 135, UI_OK, fn); }
  ui_text(2, FOOT_Y, UI_DIM, "A pick/open B up L/R end");
}

/* Pick one save (any Gen-3 game incl. FRLG). `exclude` (or NULL) is rejected so
 * SAVE 2 can't equal SAVE 1. Returns false if the user cancels. */
static bool trade_browse_pick(const char* title, const char* exclude, char* out_path, BrEntry* out) {
  if (browse_scan() < 0) halt_msg("Cannot read SD");
  int sel = 0, top = 0;
  bool dirty = true;
  while (1) {
    int rows = br_rows();
    if (rows == 0) sel = 0; else if (sel >= rows) sel = rows - 1;
    if (sel < 0) sel = 0;
    if (sel < top) top = sel;
    if (sel >= top + TRADE_LIST_ROWS) top = sel - TRADE_LIST_ROWS + 1;
    if (top < 0) top = 0;
    if (dirty) { render_trade_list(title, sel, top); dirty = false; }
    vsync();
    u16 mv  = key_repeat(KEY_UP | KEY_DOWN);
    u16 hit = key_hit(KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R | KEY_A | KEY_B);
    if (!mv && !hit) continue;
    dirty = true;
    if (mv & KEY_DOWN)       { if (rows) sel = (sel + 1) % rows; }
    else if (mv & KEY_UP)    { if (rows) sel = (sel == 0) ? rows - 1 : sel - 1; }
    else if (hit & KEY_RIGHT){ sel += 7; if (sel >= rows) sel = rows - 1; }
    else if (hit & KEY_LEFT) { sel -= 7; if (sel < 0) sel = 0; }
    else if (hit & KEY_L)    { sel = 0; }
    else if (hit & KEY_R)    { sel = rows ? rows - 1 : 0; }
    else if (hit & KEY_B)    { if (!at_root()) { path_up(); browse_scan(); sel = 0; top = 0; persist_cwd(); } else return false; }
    else if (hit & KEY_A) {
      BrEntry* e = br_entry(sel);
      if (!e) { if (!at_root()) { path_up(); browse_scan(); sel = 0; top = 0; persist_cwd(); } }
      else if (e->is_dir) {
        char np[PATH_MAX];
        if (path_join(g_cwd, e->name, np)) { strcpy(g_cwd, np); browse_scan(); sel = 0; top = 0; persist_cwd(); }
      } else {
        if (path_join(g_cwd, e->name, out_path)) {
          if (exclude && strcmp(out_path, exclude) == 0) {
            show_msg("Already SAVE 1", "Pick a different save.");
            ui_text(6, 100, UI_DIM, "B = back"); wait_keys(KEY_B);
            continue;                       /* re-render, stay in the picker */
          }
          *out = *e; return true;
        }
      }
    }
  }
}

/* Pick one Pokemon to trade -- from the live party OR any of the 14 PC boxes.
 * Shoulder L/R flips between the party row and the PC-box grid (D-pad <> changes
 * box at the grid edges). Eggs/empties aren't selectable. Fills `out_loc`. */
static bool trade_pick_mon(const char* path, const BrEntry* e, TradeLoc* out_loc) {
  u32 sz = load_path(path);
  Gen3SaveInfo info;
  if (!sz || !gen3_parse(g_save, sz, &info) || !info.sb1_ok) {
    show_msg("Cannot read save", base_name(path)); ui_text(6, 100, UI_DIM, "B = back"); wait_keys(KEY_B);
    return false;
  }
  int slot = info.slot;
  gen3_read_saveblock1(g_save, slot, g_sb1);
  TradeGame g = trade_detect_game(g_save, slot, g_sb1);
  TradeLayout L;
  if (!trade_layout(g, &L)) {
    show_msg("Unknown game", base_name(path)); ui_text(6, 100, UI_DIM, "B = back"); wait_keys(KEY_B);
    return false;
  }
  bool occ[6]; int first = -1;
  for (int i = 0; i < 6; i++) {
    occ[i] = pk_decode_mon(g_sb1 + L.party_off + i * 100, true, &g_party[i]) &&
             g_party[i].species && !g_party[i].isEgg && !g_party[i].isBadEgg;
    if (occ[i] && first < 0) first = i;
  }
  char owner[20]; siprintf(owner, "%s %s", e->trainer, trade_tag(e));

  int view = (first < 0) ? TP_PC : TP_PARTY;   /* no party mon -> open the PC straight away */
  int cur  = (first < 0) ? 0 : first;          /* party cursor                              */
  int curbox = 0, bcur = 0;
  if (view == TP_PC) read_box_into(g_save, slot, curbox);
  bool dirty = true;
  while (1) {
    if (dirty) {
      ui_clear();
      ui_text(2, 0, UI_TITLE, "PICK A POKEMON");
      { char ob[16]; ui_truncate(ob, owner, 14); ui_text(140, 0, UI_DIM, ob); }
      if (view == TP_PARTY) {
        for (int i = 0; i < 6; i++) {
          int x = 8 + i * 38, y = 40;
          if (i == cur) m3_frame(x - 2, y - 2, x + 34, y + 44, UI_TITLE);
          if (!occ[i]) continue;
          draw_mon32(x, y, g_party[i].species, false);
          char lab[8]; siprintf(lab, "Lv%d", g_party[i].level);
          ui_text(x, y + 34, UI_TEXT, lab);
        }
        const PkMon* f = occ[cur] ? &g_party[cur] : NULL;
        if (f) {
          char l[40];
          ui_text(8, 100, UI_TITLE, gen3_species_name(f->species));
          siprintf(l, "Lv%d  %s", f->level, pk_nature_name(f->nature));
          ui_text(8, 112, UI_DIM, l);
        } else {
          ui_text(8, 100, UI_DIM, "(no party) - L/R opens PC");
        }
        ui_text(2, FOOT_Y, UI_DIM, "A pick  L/R PC  SEL stats  B");
      } else {
        char line[40];
        siprintf(line, "PC BOX %d", curbox + 1);
        ui_text(2, 18, UI_TITLE, line);
        for (int sl = 0; sl < G3_IN_BOX; sl++) {     /* 6x5 box grid */
          int col = sl % 6, row = sl / 6;
          int x = 8 + col * 22, y = 32 + row * 18;
          if (sl == bcur) m3_frame(x - 1, y - 1, x + 17, y + 17, UI_TITLE);
          if (g_boxmons[sl].species) draw_mon16(x, y, g_boxmons[sl].species);
        }
        const PkMon* f = g_boxmons[bcur].species ? &g_boxmons[bcur] : NULL;
        if (f) {
          ui_text(8, 132, UI_TITLE, gen3_species_name(f->species));
          siprintf(line, "Lv%d  %s", f->level, pk_nature_name(f->nature));
          ui_text(8, 144, UI_DIM, line);
        } else {
          ui_text(8, 132, UI_DIM, "(empty)");
        }
        ui_text(2, FOOT_Y, UI_DIM, "A pick  L/R party  <>box  SEL");
      }
      dirty = false;
    }
    vsync();
    u16 mv  = key_repeat(KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN);
    u16 hit = key_hit(KEY_A | KEY_B | KEY_SELECT | KEY_L | KEY_R);
    if (!mv && !hit) continue;
    dirty = true;

    if (view == TP_PARTY) {
      if (hit & KEY_B) return false;
      else if (hit & (KEY_L | KEY_R)) { read_box_into(g_save, slot, curbox); bcur = 0; view = TP_PC; }
      else if (hit & KEY_A) { if (occ[cur]) { out_loc->kind = TLOC_PARTY; out_loc->pslot = (uint8_t)cur;
                                              out_loc->box = 0; out_loc->bslot = 0; return true; }
                              else { read_box_into(g_save, slot, curbox); bcur = 0; view = TP_PC; } }
      else if (hit & KEY_SELECT) { if (occ[cur]) { render_mon_stats(&g_party[cur], owner, true); wait_keys(KEY_B | KEY_SELECT); } }
      else if (mv & (KEY_LEFT | KEY_UP))    { for (int i = cur - 1; i >= 0; i--) if (occ[i]) { cur = i; break; } }
      else if (mv & (KEY_RIGHT | KEY_DOWN)) { for (int i = cur + 1; i < 6; i++)  if (occ[i]) { cur = i; break; } }
    } else {                                    /* PC box view */
      if (hit & (KEY_B | KEY_L | KEY_R)) {
        if (first < 0) { if (hit & KEY_B) return false; }   /* no party to return to */
        else view = TP_PARTY;
      }
      else if (hit & KEY_A) {
        if (g_boxmons[bcur].species) { out_loc->kind = TLOC_BOX; out_loc->box = (uint8_t)curbox;
                                       out_loc->bslot = (uint8_t)bcur; out_loc->pslot = 0; return true; }
      }
      else if (hit & KEY_SELECT) { if (g_boxmons[bcur].species) { render_mon_stats(&g_boxmons[bcur], owner, true); wait_keys(KEY_B | KEY_SELECT); } }
      else {
        int col = bcur % 6, row = bcur / 6;
        if (mv & KEY_LEFT) {
          if (col > 0) bcur--;
          else { curbox = (curbox + G3_TOTAL_BOXES - 1) % G3_TOTAL_BOXES;
                 read_box_into(g_save, slot, curbox); bcur = row * 6 + 5; }
        } else if (mv & KEY_RIGHT) {
          if (col < 5) bcur++;
          else { curbox = (curbox + 1) % G3_TOTAL_BOXES;
                 read_box_into(g_save, slot, curbox); bcur = row * 6; }
        } else if (mv & KEY_UP)   { bcur = (row > 0) ? bcur - 6 : bcur + 24; }
        else if (mv & KEY_DOWN)   { bcur = (row < 4) ? bcur + 6 : bcur - 24; }
      }
    }
  }
}

static bool trade_confirm(const BrEntry* a, const BrEntry* b, const TradeResult* tr) {
  ui_clear();
  char line[48];
  ui_text(6, 4, UI_TITLE, "CONFIRM TRADE");
  siprintf(line, "%s gives %s", a->trainer, gen3_species_name(tr->givenA)); ui_text(6, 24, UI_TEXT, line);
  siprintf(line, "%s gives %s", b->trainer, gen3_species_name(tr->givenB)); ui_text(6, 36, UI_TEXT, line);
  siprintf(line, "%s gets %s%s", a->trainer, gen3_species_name(tr->a_recv_final), tr->a_evolved ? " (EVOLVES!)" : "");
  ui_text(6, 56, UI_OK, line);
  siprintf(line, "%s gets %s%s", b->trainer, gen3_species_name(tr->b_recv_final), tr->b_evolved ? " (EVOLVES!)" : "");
  ui_text(6, 68, UI_OK, line);
  ui_text(6, 90,  UI_DIM,  "Genuine: friendship reset,");
  ui_text(6, 102, UI_DIM,  "Pokedex + trade counter set.");
  ui_text(6, 122, UI_WARN, "Writes BOTH saves (backups).");
  ui_text(6, 140, UI_TEXT, "A = trade   B = cancel");
  u16 c = wait_keys(KEY_A | KEY_B);
  return (c & KEY_A) != 0;
}

static void run_trade_flow(void) {
  if (active_flashcart != EZ_FLASH_OMEGA) {
    show_msg("Trade is Omega-only", "EverDrive runs read-only.");
    ui_text(6, 100, UI_DIM, "B = back"); wait_keys(KEY_B);
    return;
  }
  char pathA[PATH_MAX], pathB[PATH_MAX];
  static BrEntry eA, eB;
  TradeLoc locA = {0,0,0,0}, locB = {0,0,0,0};
  g_trade_mode = 1;
  bool ok = trade_browse_pick("TRADE: pick SAVE 1", NULL, pathA, &eA) &&
            trade_pick_mon(pathA, &eA, &locA) &&
            trade_browse_pick("TRADE: pick SAVE 2", pathA, pathB, &eB) &&
            trade_pick_mon(pathB, &eB, &locB);
  g_trade_mode = 0;
  if (!ok) return;
  if (strcmp(pathA, pathB) == 0) {
    show_msg("Pick two DIFFERENT saves", NULL); ui_text(6, 100, UI_DIM, "B = back"); wait_keys(KEY_B);
    return;
  }

  {
    char la[16], lb[16];
    if (locA.kind == TLOC_BOX) siprintf(la, "box%d slot%d", locA.box + 1, locA.bslot + 1);
    else                       siprintf(la, "party#%d", locA.pslot + 1);
    if (locB.kind == TLOC_BOX) siprintf(lb, "box%d slot%d", locB.box + 1, locB.bslot + 1);
    else                       siprintf(lb, "party#%d", locB.pslot + 1);
    log_line("=== TRADE ===");
    log_save_identity("TRADE SAVE1", pathA, trade_tag(&eA), &eA);
    log_save_identity("TRADE SAVE2", pathB, trade_tag(&eB), &eB);
    log_line("TRADE: SAVE1 gives from %s; SAVE2 gives from %s", la, lb);
    log_flush_to_sd(LOG_PATH);
  }

  show_msg("Checking trade...", "(nothing written)");
  log_line("=== TRADE dry-run: %s <-> %s ===", pathA, pathB);
  TradeResult tr;
  SfStatus st = sf_trade(pathA, &locA, pathB, &locB, false, g_save, &tr);
  log_flush_to_sd(LOG_PATH);
  if (st != SF_OK) { result_screen("TRADE CHECK FAILED", UI_WARN, sf_status_str(st), "Nothing written."); return; }

  if (!trade_confirm(&eA, &eB, &tr)) { play_jingle(SFX_CANCEL, 18); return; }

  trade_animation(tr.givenA, tr.givenB);       /* before the write (ROM mapped) */
  show_msg("Trading (writing both)...", NULL);
  log_line("=== TRADE commit ===");
  st = sf_trade(pathA, &locA, pathB, &locB, true, g_save, &tr);   /* silent write */
  log_line("trade commit: %s", st == SF_OK ? "OK" : sf_status_str(st));
  log_flush_to_sd(LOG_PATH);
  if (st == SF_OK) {
    trade_success_anim(tr.a_recv_final, tr.b_recv_final);   /* after the write */
    result_screen("TRADE COMPLETE", UI_OK, "Both saves updated & backed up.",
                  "Genuine: friendship/dex/evo.");
  } else {
    result_screen("TRADE FAILED", UI_WARN, sf_status_str(st), "Backups kept.");
  }
}

/* ===================== main ============================================= */

/* Settings: toggle animations / sound / quick-mix; persisted to /pokelinksim/settings.ini. */
static void app_settings(void) {
  int sel = 0;
  bool dirty = true;
  while (1) {
    if (dirty) {
      ui_clear();
      ui_text(36, 16, UI_TITLE, "SETTINGS");
      char l[40];
      siprintf(l, "Animations : %s", config_get_anim_enabled() ? "ON" : "OFF");
      ui_text_sel(20, 50, 200, sel == 0, UI_TEXT, l);
      siprintf(l, "Sound      : %s", config_get_sfx_enabled() ? "ON" : "OFF");
      ui_text_sel(20, 64, 200, sel == 1, UI_TEXT, l);
      siprintf(l, "Quick mix  : %s NObak", config_get_quick_mode() ? "ON" : "OFF");
      ui_text_sel(20, 78, 200, sel == 2, UI_TEXT, l);
      ui_text(20, 110, UI_DIM, "A toggle   B save+back");
      ui_text(20, 122, UI_DIM, "(hold R speeds animations)");
      dirty = false;
    }
    vsync(); sfx_tick();
    u16 mv  = key_repeat(KEY_UP | KEY_DOWN);
    u16 hit = key_hit(KEY_A | KEY_B);
    if (!mv && !hit) continue;
    dirty = true;
    if (hit & KEY_B) { config_save(CONFIG_PATH); return; }
    else if (hit & KEY_A) {
      if (sel == 0)      config_set_anim_enabled(!config_get_anim_enabled());
      else if (sel == 1) { config_set_sfx_enabled(!config_get_sfx_enabled());
                           if (config_get_sfx_enabled()) sfx_play(SFX_BLIP);
                           else sfx_silence(); }            /* kill any sounding note now */
      else { if (config_get_quick_mode()) config_set_quick_mode(false);   /* off freely */
             else if (quick_enable_confirm()) config_set_quick_mode(true); /* on: confirm */ }
    }
    else if (mv & KEY_UP)   { sel = (sel + 2) % 3; }
    else if (mv & KEY_DOWN) { sel = (sel + 1) % 3; }
  }
}

/* Top menu: a D-pad list confirmed with A (matches every other screen), with a
 * one-line subtitle per item so Mix vs the destructive Trade is never ambiguous. */
static int app_menu(void) {
  static const char* items[4] = { "Record Mix", "Trade Pokemon", "Settings",
                                  "Exit to flashcart" };
  static const char* subs[4]  = { "share secret-base teams",
                                  "swap a party mon (perm.)",
                                  "anim / sound / quick mix",
                                  "reboot to EZ-Flash menu" };
  int sel = 0; bool dirty = true;
  while (1) {
    if (dirty) {
      ui_clear();
      ui_text(36, 10, UI_TITLE, "PokeLinkSim");
      for (int i = 0; i < 4; i++) {
        ui_text_sel(20, 34 + i * 24, 200, sel == i, UI_TEXT, items[i]);
        ui_text(30, 34 + i * 24 + 10, UI_DIM, subs[i]);
      }
      ui_text(20, 134, UI_DIM,
              active_flashcart == EZ_FLASH_OMEGA ? "Omega: read + write"
                                                 : "EverDrive: read-only (no write)");
      ui_text(20, 148, UI_DIM, "D-pad move   A select");
      dirty = false;
    }
    vsync(); sfx_tick();
    u16 mv  = key_repeat(KEY_UP | KEY_DOWN);
    u16 hit = key_hit(KEY_A);
    if (!mv && !hit) continue;
    dirty = true;
    if (hit & KEY_A) {
      if (sel == 2) { app_settings(); continue; }
      if (sel == 3) {                            /* exit to the flashcart loader */
        show_msg("Rebooting...", "Returning to flashcart menu.");
        for (int i = 0; i < 30; i++) vsync();
        flashcartio_reboot();                    /* no return on Omega/EverDrive */
        show_msg("Reboot unsupported", "Power-cycle to exit.");
        ui_text(6, 110, UI_DIM, "B = back"); wait_keys(KEY_B);
        continue;
      }
      return sel;                       /* 0 = mix, 1 = trade */
    }
    else if (mv & KEY_UP)   sel = (sel + 3) % 4;
    else if (mv & KEY_DOWN) sel = (sel + 1) % 4;
  }
}

static void init_system(void) {
  irq_init(NULL);
  irq_add(II_VBLANK, NULL);
  ui_init();
  sfx_init();                 /* enable the PSG for jingles (Phase 3) */
  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);  /* horizontal pickers need L/R repeat */
  key_repeat_limits(16, 4);   /* hold ~0.27s, then repeat ~15/s */
}

static const char* flashcart_name(void) {
  switch (active_flashcart) {
    case EVERDRIVE_GBA_X5: return "EverDrive GBA X5";
    case EZ_FLASH_OMEGA:   return "EZ-Flash Omega/DE";
    default:               return "none";
  }
}

int main(void) {
  init_system();
  g_cwd[0] = '/'; g_cwd[1] = 0;
  log_init();
  log_line("=== GBA Record Mixer (graphical build) ===");
  log_line("mGBA debug log: %s", log_under_mgba() ? "active" : "absent");

  show_msg("Detecting flashcart...", NULL);
  if (!flashcartio_activate()) halt_msg("No flashcart detected!");
  log_line("flashcart: %s", flashcart_name());

  FATFS fs;
  FRESULT fr = f_mount(&fs, "", 1);
  if (fr != FR_OK) { log_line("f_mount failed (fr=%d)", fr); halt_msg("SD mount failed!"); }
  log_line("SD mounted OK");

  /* Keep the SD root tidy: all app files live in APP_DIR (created if missing). */
  f_mkdir(APP_DIR);                 /* FR_EXIST when it already exists -- fine */

  /* The log APPENDS across runs; stamp a dated banner so each session is dated. */
  {
    GbaRtcTime t;
    log_line(" ");
    if (gba_rtc_get(&t))
      log_line("===== SESSION %04u-%02u-%02u %02u:%02u:%02u =====",
               (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
               (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second);
    else
      log_line("===== SESSION (RTC unavailable) =====");
  }

  tp_init(PREFS_PATH);              /* load remembered secret-base selections */
  log_line("team prefs loaded");

  /* Reopen the folder we were last in (saves usually live in the same place). */
  config_load(CONFIG_PATH);
  char last_dir[PATH_MAX];
  if (config_get_last_dir(last_dir, sizeof(last_dir)) &&
      last_dir[0] == '/' && dir_exists(last_dir)) {
    strcpy(g_cwd, last_dir);
    log_line("restored last dir: %s", g_cwd);
  }

  int wr = log_flush_to_sd(LOG_PATH);
  log_line("log flush -> %s", wr == 0 ? "OK" : "FAILED");

  while (1) {
    if (app_menu() == 0) run_mix_flow();
    else                 run_trade_flow();
  }
  return 0;
}
