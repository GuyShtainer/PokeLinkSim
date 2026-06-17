#include "team_prefs.h"
#include <string.h>

/* Keep the big buffers in EWRAM (the GBA has 256 KiB of it) rather than the
 * scarce 32 KiB IWRAM that plain statics default to. On the host this is a
 * no-op. ".sbss" is the section the GBA linker script maps into EWRAM (this is
 * what libtonc's EWRAM_BSS expands to). */
#if defined(TP_HOST)
#define TP_EWRAM
#else
#define TP_EWRAM __attribute__((section(".sbss")))
#endif

/* ===================== in-memory store =================================== */

static TpTeam TP_EWRAM g_teams[TP_MAX_TEAMS];
static int    g_nteams;

void tp_reset(void) {
  memset(g_teams, 0, sizeof(g_teams));
  g_nteams = 0;
}

static void copy_trunc(char* dst, const char* src, int cap) {
  int i = 0;
  for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
  dst[i] = 0;
}

/* ===================== minimal JSON reader =============================== */
/* Tolerant enough for our own output and light hand-editing; anything it can't
 * make sense of is skipped, so a malformed file degrades to "no prefs". */

typedef struct { const char* p; const char* end; } Js;

static void js_ws(Js* j) {
  while (j->p < j->end) {
    char c = *j->p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->p++; else break;
  }
}
static bool js_ch(Js* j, char c)   { js_ws(j); if (j->p < j->end && *j->p == c) { j->p++; return true; } return false; }
static bool js_peek(Js* j, char c) { js_ws(j); return j->p < j->end && *j->p == c; }

static bool js_str(Js* j, char* out, int cap) {
  js_ws(j);
  if (j->p >= j->end || *j->p != '"') return false;
  j->p++;
  int o = 0;
  while (j->p < j->end && *j->p != '"') {
    char c = *j->p++;
    if (c == '\\' && j->p < j->end) {
      char e = *j->p++;
      switch (e) {
        case 'n': c = '\n'; break; case 't': c = '\t'; break;
        case '"': c = '"';  break; case '\\': c = '\\'; break; case '/': c = '/'; break;
        case 'u': for (int k = 0; k < 4 && j->p < j->end; k++) j->p++; c = '?'; break;
        default:  c = e; break;
      }
    }
    if (o < cap - 1) out[o++] = c;
  }
  if (j->p < j->end && *j->p == '"') { j->p++; } else return false;
  out[o] = 0;
  return true;
}

static long js_num(Js* j) {
  js_ws(j);
  long sign = 1, v = 0;
  if (j->p < j->end && *j->p == '-') { sign = -1; j->p++; }
  while (j->p < j->end && *j->p >= '0' && *j->p <= '9') { v = v * 10 + (*j->p - '0'); j->p++; }
  return sign * v;
}

/* Skip any single JSON value (string / number / bool / null / object / array). */
static void js_skip(Js* j) {
  js_ws(j);
  if (j->p >= j->end) return;
  char c = *j->p;
  if (c == '"') {
    j->p++;
    while (j->p < j->end && *j->p != '"') { if (*j->p == '\\' && j->p + 1 < j->end) j->p++; j->p++; }
    if (j->p < j->end) j->p++;
  } else if (c == '{' || c == '[') {
    char open = c, close = (c == '{') ? '}' : ']';
    int depth = 0;
    while (j->p < j->end) {
      char d = *j->p++;
      if (d == '"') {
        while (j->p < j->end && *j->p != '"') { if (*j->p == '\\' && j->p + 1 < j->end) j->p++; j->p++; }
        if (j->p < j->end) j->p++;
      } else if (d == open)  depth++;
      else if (d == close) { depth--; if (depth == 0) break; }
    }
  } else {
    while (j->p < j->end) {
      char d = *j->p;
      if (d == ',' || d == '}' || d == ']' || d == ' ' || d == '\t' || d == '\n' || d == '\r') break;
      j->p++;
    }
  }
}

static void parse_mons(Js* j, TpTeam* t) {
  if (!js_peek(j, '[')) { js_skip(j); return; }
  js_ch(j, '[');
  if (js_peek(j, ']')) { js_ch(j, ']'); return; }
  do {
    TpMon m;
    memset(&m, 0, sizeof(m));
    m.chosen = 1;
    if (!js_ch(j, '{')) { js_skip(j); continue; }
    if (!js_peek(j, '}')) {
      do {
        char key[12];
        if (!js_str(j, key, sizeof key)) break;
        if (!js_ch(j, ':')) break;
        if      (strcmp(key, "sp")   == 0) m.species = (uint16_t)js_num(j);
        else if (strcmp(key, "nick") == 0) js_str(j, m.nick, sizeof m.nick);
        else if (strcmp(key, "keep") == 0) m.chosen = (uint8_t)(js_num(j) ? 1 : 0);
        else js_skip(j);
      } while (js_ch(j, ','));
    }
    js_ch(j, '}');
    if (t->n < TP_MAX_MON) t->mon[t->n++] = m;
  } while (js_ch(j, ','));
  js_ch(j, ']');
}

static void parse_team(Js* j) {
  TpTeam t;
  memset(&t, 0, sizeof(t));
  if (!js_ch(j, '{')) { js_skip(j); return; }
  if (!js_peek(j, '}')) {
    do {
      char key[12];
      if (!js_str(j, key, sizeof key)) break;
      if (!js_ch(j, ':')) break;
      if      (strcmp(key, "game")    == 0) t.game       = (uint8_t)js_num(j);
      else if (strcmp(key, "trainer") == 0) js_str(j, t.trainer, sizeof t.trainer);
      else if (strcmp(key, "tidp")    == 0) t.tid_public = (uint16_t)js_num(j);
      else if (strcmp(key, "tids")    == 0) t.tid_secret = (uint16_t)js_num(j);
      else if (strcmp(key, "mons")    == 0) parse_mons(j, &t);
      else js_skip(j);
    } while (js_ch(j, ','));
  }
  js_ch(j, '}');
  t.used = 1;
  if (g_nteams < TP_MAX_TEAMS) g_teams[g_nteams++] = t;
}

int tp_parse(const char* text, int len) {
  tp_reset();
  if (!text || len <= 0) return 0;
  Js j = { text, text + len };
  if (!js_ch(&j, '{')) return 0;
  if (!js_peek(&j, '}')) {
    do {
      char key[12];
      if (!js_str(&j, key, sizeof key)) break;
      if (!js_ch(&j, ':')) break;
      if (strcmp(key, "teams") == 0) {
        if (!js_peek(&j, '[')) { js_skip(&j); }
        else {
          js_ch(&j, '[');
          if (!js_peek(&j, ']')) { do { parse_team(&j); } while (js_ch(&j, ',')); }
          js_ch(&j, ']');
        }
      } else js_skip(&j);
    } while (js_ch(&j, ','));
  }
  js_ch(&j, '}');
  return g_nteams;
}

/* ===================== JSON writer ====================================== */

static int put(char* b, int len, int cap, const char* s) {
  while (*s && len < cap - 1) b[len++] = *s++;
  return len;
}
static int put_uint(char* b, int len, int cap, unsigned v) {
  char tmp[6]; int n = 0;
  if (v == 0) tmp[n++] = '0';
  while (v && n < 6) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
  while (n-- > 0 && len < cap - 1) b[len++] = tmp[n];
  return len;
}
/* Emit a JSON string body with " and \ escaped (our names contain neither, but
 * a hand-edited file might). */
static int put_jstr(char* b, int len, int cap, const char* s) {
  len = put(b, len, cap, "\"");
  for (; *s && len < cap - 2; s++) {
    if (*s == '"' || *s == '\\') b[len++] = '\\';
    b[len++] = *s;
  }
  return put(b, len, cap, "\"");
}

int tp_serialize(char* buf, int cap) {
  int len = 0;
  len = put(buf, len, cap, "{\"v\":1,\"teams\":[");
  bool firstT = true;
  for (int i = 0; i < g_nteams; i++) {
    TpTeam* t = &g_teams[i];
    if (!t->used) continue;
    if (len > cap - 400) break;                 /* leave room to close cleanly */
    if (!firstT) len = put(buf, len, cap, ",");
    firstT = false;
    len = put(buf, len, cap, "{\"game\":");      len = put_uint(buf, len, cap, t->game);
    len = put(buf, len, cap, ",\"trainer\":");   len = put_jstr(buf, len, cap, t->trainer);
    len = put(buf, len, cap, ",\"tidp\":");      len = put_uint(buf, len, cap, t->tid_public);
    len = put(buf, len, cap, ",\"tids\":");      len = put_uint(buf, len, cap, t->tid_secret);
    len = put(buf, len, cap, ",\"mons\":[");
    for (int k = 0; k < t->n; k++) {
      if (len > cap - 80) break;
      if (k) len = put(buf, len, cap, ",");
      len = put(buf, len, cap, "{\"sp\":");      len = put_uint(buf, len, cap, t->mon[k].species);
      len = put(buf, len, cap, ",\"nick\":");    len = put_jstr(buf, len, cap, t->mon[k].nick);
      len = put(buf, len, cap, ",\"keep\":");    len = put_uint(buf, len, cap, t->mon[k].chosen ? 1 : 0);
      len = put(buf, len, cap, "}");
    }
    len = put(buf, len, cap, "]}");
  }
  len = put(buf, len, cap, "]}\n");
  return len;
}

/* ===================== lookup / apply / update ========================== */

const TpTeam* tp_find(uint8_t game, const char* trainer, uint16_t tid_public) {
  for (int i = 0; i < g_nteams; i++) {
    TpTeam* t = &g_teams[i];
    if (t->used && t->game == game && t->tid_public == tid_public &&
        strcmp(t->trainer, trainer) == 0)
      return t;
  }
  return NULL;
}

void tp_apply(const TpTeam* t, const Gen3DisplayParty* dp, bool chk[6]) {
  bool used[TP_MAX_MON];
  memset(used, 0, sizeof(used));
  for (int i = 0; i < dp->count && i < 6; i++) {
    chk[i] = true;                       /* default: keep */
    if (!t) continue;
    for (int k = 0; k < t->n; k++) {
      if (used[k]) continue;
      if (t->mon[k].species == dp->mon[i].species &&
          strncmp(t->mon[k].nick, dp->mon[i].nickname, TP_NICK_LEN - 1) == 0) {
        chk[i] = t->mon[k].chosen ? true : false;
        used[k] = true;
        break;
      }
    }
  }
}

void tp_update(uint8_t game, const char* trainer, uint16_t tid_public,
               uint16_t tid_secret, const Gen3DisplayParty* dp, const bool chk[6]) {
  TpTeam* t = NULL;
  for (int i = 0; i < g_nteams; i++) {
    if (g_teams[i].used && g_teams[i].game == game &&
        g_teams[i].tid_public == tid_public && strcmp(g_teams[i].trainer, trainer) == 0) {
      t = &g_teams[i];
      break;
    }
  }
  if (!t) {
    if (g_nteams >= TP_MAX_TEAMS) {                 /* FIFO-evict the oldest */
      for (int i = 1; i < TP_MAX_TEAMS; i++) g_teams[i - 1] = g_teams[i];
      g_nteams = TP_MAX_TEAMS - 1;
    }
    t = &g_teams[g_nteams++];
  }
  memset(t, 0, sizeof(*t));
  t->used = 1;
  t->game = game;
  t->tid_public = tid_public;
  t->tid_secret = tid_secret;
  copy_trunc(t->trainer, trainer, sizeof t->trainer);
  int n = 0;
  for (int i = 0; i < dp->count && i < TP_MAX_MON; i++) {
    t->mon[n].species = dp->mon[i].species;
    copy_trunc(t->mon[n].nick, dp->mon[i].nickname, TP_NICK_LEN);
    t->mon[n].chosen = chk[i] ? 1 : 0;
    n++;
  }
  t->n = (uint8_t)n;
}

/* ===================== SD-card wrappers (GBA only) ====================== */

#ifndef TP_HOST
#include "ff.h"

static char TP_EWRAM g_iobuf[8192];   /* JSON read/write scratch (EWRAM) */

void tp_init(const char* path) {
  tp_reset();
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) return;     /* no file yet: empty store */
  UINT br = 0;
  FRESULT fr = f_read(&f, g_iobuf, sizeof(g_iobuf) - 1, &br);
  f_close(&f);
  if (fr != FR_OK) return;
  tp_parse(g_iobuf, (int)br);
}

bool tp_save(const char* path) {
  int len = tp_serialize(g_iobuf, sizeof(g_iobuf));
  FIL f;
  if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
  UINT bw = 0;
  FRESULT fr = f_write(&f, g_iobuf, (UINT)len, &bw);
  f_close(&f);
  return fr == FR_OK && bw == (UINT)len;
}
#endif /* TP_HOST */
