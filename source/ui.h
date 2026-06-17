#ifndef UI_H
#define UI_H

#include <tonc.h>
#include <stdbool.h>

/* Bitmap (Mode 3) UI helper layer: filled panels, bordered boxes, colored text,
 * and a solid selection-highlight bar. Text still goes through libtonc TTE
 * (tte_write/tte_set_ink/tte_set_pos), but is rendered into the Mode 3 bitmap.
 * Initialise the mode + TTE in main (see ui_init). */

#define UI_SCR_W   240
#define UI_SCR_H   160
#define UI_ROW_H   8            /* sys8 font line height (px)                   */
#define UI_COLS    30           /* 240/8 fixed-width columns                    */

/* Palette (RGB15). Tuned for a dark, readable look. */
#define UI_BG       RGB15( 1,  2,  4)   /* screen background          */
#define UI_PANEL    RGB15( 2,  4,  9)   /* panel fill                 */
#define UI_BORDER   RGB15(10, 13, 20)   /* panel/divider border       */
#define UI_TEXT     RGB15(31, 31, 31)   /* primary text               */
#define UI_DIM      RGB15(17, 18, 21)   /* secondary/dim text         */
#define UI_TITLE    RGB15( 8, 28, 31)   /* headers (cyan)             */
#define UI_SEL      RGB15( 5, 10, 24)   /* selection highlight bar    */
#define UI_SELTEXT  RGB15(31, 31, 18)   /* text on the highlight bar  */
#define UI_OK       RGB15( 8, 28, 10)   /* good/green                 */
#define UI_WARN     RGB15(31, 18,  3)   /* warning/orange             */
#define UI_DANGER   RGB15(31,  0,  0)   /* danger/red (no-backup etc) */
#define UI_DIRCLR   RGB15(10, 24, 31)   /* directory rows (cyan)      */
#define UI_SAVECLR  RGB15(31, 31, 31)   /* save rows (white)          */

/* Switch to Mode 3 and init bitmap TTE with the fixed 8x8 system font. */
void ui_init(void);

/* Clear the whole screen to UI_BG. */
void ui_clear(void);

/* Filled rectangle with a 1px border. (x,y) top-left, w/h in pixels. */
void ui_panel(int x, int y, int w, int h, u16 fill, u16 border);

/* A horizontal divider line at pixel row y, from x..x+w. */
void ui_hline(int x, int y, int w, u16 color);

/* Draw text at pixel (x,y) in colour `ink`. */
void ui_text(int x, int y, u16 ink, const char* s);

/* Draw a list row of width `w` px at (x,y). When `selected`, paints a UI_SEL
 * bar behind it and uses UI_SELTEXT; otherwise draws the text in `ink`. */
void ui_text_sel(int x, int y, int w, bool selected, u16 ink, const char* s);

/* Blit a Pokemon icon at (x,y). Source data is 32x32; each pixel is a u16:
 * 0 = transparent, else 0x8000 | RGB15 (see tools/gen_icons.py). ui_icon16
 * subsamples every other pixel for a half-size draw. NULL is a no-op. */
void ui_icon32(int x, int y, const u16* icon);
void ui_icon16(int x, int y, const u16* icon);

/* Like ui_icon32 but desaturated + dimmed — marks a "not chosen" Pokemon. */
void ui_icon32_grey(int x, int y, const u16* icon);

/* Copy `in` into `out` clamped to `max_cols` display columns, UTF-8-safe
 * (never splits a codepoint); appends '~' as the last column if truncated.
 * `out` must hold at least max_cols*4 + 1 bytes to be safe. */
void ui_truncate(char* out, const char* in, int max_cols);

#endif /* UI_H */
