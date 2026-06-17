#include "ui.h"
#include <string.h>

void ui_init(void) {
  REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;
  tte_init_bmp(DCNT_MODE3, &sys8Font, NULL);  /* fixed 8x8 font on the M3 bitmap */
  tte_set_paper(UI_BG);
}

void ui_clear(void) {
  m3_fill(UI_BG);
}

void ui_panel(int x, int y, int w, int h, u16 fill, u16 border) {
  m3_rect(x, y, x + w, y + h, fill);
  m3_frame(x, y, x + w - 1, y + h - 1, border);
}

void ui_hline(int x, int y, int w, u16 color) {
  m3_line(x, y, x + w - 1, y, color);
}

void ui_text(int x, int y, u16 ink, const char* s) {
  tte_set_ink(ink);
  tte_set_pos(x, y);
  tte_write(s);
}

void ui_text_sel(int x, int y, int w, bool selected, u16 ink, const char* s) {
  if (selected) m3_rect(x, y, x + w, y + UI_ROW_H, UI_SEL);
  tte_set_ink(selected ? UI_SELTEXT : ink);
  tte_set_pos(x + 1, y);
  tte_write(s);
}

/* true if (px,py) is on the Mode-3 screen; m3_plot has no clipping of its own, so
 * off-screen icon pixels (e.g. mid-slide trade animation) would smear onto the
 * wrong scanline without this guard. */
static inline bool px_on_screen(int px, int py) {
  return (unsigned)px < 240u && (unsigned)py < 160u;
}

void ui_icon32(int x, int y, const u16* icon) {
  if (!icon) return;
  for (int j = 0; j < 32; j++) {
    for (int i = 0; i < 32; i++) {
      u16 p = icon[j * 32 + i];
      if ((p & 0x8000) && px_on_screen(x + i, y + j)) m3_plot(x + i, y + j, (u16)(p & 0x7FFF));
    }
  }
}

void ui_icon32_grey(int x, int y, const u16* icon) {
  if (!icon) return;
  for (int j = 0; j < 32; j++) {
    for (int i = 0; i < 32; i++) {
      u16 p = icon[j * 32 + i];
      if (!(p & 0x8000)) continue;
      if (!px_on_screen(x + i, y + j)) continue;
      int r = p & 31, g = (p >> 5) & 31, b = (p >> 10) & 31;
      int lum = (r * 77 + g * 150 + b * 29) >> 8;   /* luma, 0..31 */
      lum = (lum * 5) >> 3;                          /* dim to ~62% = clearly "off" */
      m3_plot(x + i, y + j, (u16)RGB15(lum, lum, lum));
    }
  }
}

void ui_icon16(int x, int y, const u16* icon) {
  if (!icon) return;
  for (int j = 0; j < 16; j++) {
    for (int i = 0; i < 16; i++) {
      u16 p = icon[(j * 2) * 32 + (i * 2)];   /* subsample the 32x32 source */
      if ((p & 0x8000) && px_on_screen(x + i, y + j)) m3_plot(x + i, y + j, (u16)(p & 0x7FFF));
    }
  }
}

void ui_truncate(char* out, const char* in, int max_cols) {
  if (max_cols < 1) { out[0] = 0; return; }
  int cols = 0, i = 0, o = 0, last_start = 0;
  while (in[i] && cols < max_cols) {
    unsigned char c = (unsigned char)in[i];
    if ((c & 0xC0) != 0x80) { last_start = o; cols++; }  /* UTF-8 lead = new col */
    out[o++] = in[i++];
  }
  if (in[i]) {                 /* more remained -> turn the last column into '~' */
    o = last_start;
    out[o++] = '~';
  }
  out[o] = 0;
}
