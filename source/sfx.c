/*
 * Tiny PSG square-wave jingle player for the trade/mix flourishes. Uses GBA
 * sound channel 1 only (no Direct Sound, no maxmod, no sample data). A jingle is
 * a list of {rate, frames}; sfx_tick() steps it. Note rate = 2048 - 131072/Hz.
 *
 * Honors the OS-mode rule trivially: the caller only ticks this from the normal
 * (ROM-mapped, IRQs-on) UI loop around an SD write, never during the transfer.
 */
#include "sfx.h"
#include <tonc.h>
#include "app_config.h"

typedef struct { unsigned short rate; unsigned char frames; } Note;

/* {0,0} terminates. Frames are at ~60 fps. */
static const Note J_BLIP[]    = { {1881, 4}, {0, 0} };                              /* G5 */
static const Note J_TRADE[]   = { {1797,6}, {1849,6}, {1881,6}, {1923,12}, {0,0} };/* C5 E5 G5 C6 */
static const Note J_SUCCESS[] = { {1923,5}, {1881,5}, {1923,5}, {1990,16}, {0,0} };/* up flourish */
static const Note J_CANCEL[]  = { {1750,6}, {1600,10}, {0, 0} };                    /* down */

static const Note* s_seq = 0;
static int s_idx = 0, s_frames = 0, s_on = 0;

static void note_on(unsigned short rate) {
  REG_SND1CNT  = 0xC080;                     /* duty 50%, env vol 12, no decay   */
  REG_SND1FREQ = (rate & 0x07FF) | 0x8000;   /* set frequency + (re)trigger      */
}
static void note_off(void) { REG_SND1CNT = 0x0000; REG_SND1FREQ = 0x8000; }

void sfx_init(void) {
  REG_SNDSTAT   = 0x0080;   /* master sound enable                 */
  REG_SNDDSCNT  = 0x0002;   /* DMG-to-output volume ratio = 100%   */
  REG_SNDDMGCNT = 0x3377;   /* channels 1+2 on L+R, volume 7/7     */
  REG_SND1SWEEP = 0x0000;   /* no sweep                            */
  s_seq = 0; s_on = 0;
}

void sfx_silence(void) { s_on = 0; s_seq = 0; note_off(); }

void sfx_play(int id) {
  if (!config_get_sfx_enabled()) return;
  switch (id) {
    case SFX_TRADE:   s_seq = J_TRADE;   break;
    case SFX_SUCCESS: s_seq = J_SUCCESS; break;
    case SFX_CANCEL:  s_seq = J_CANCEL;  break;
    default:          s_seq = J_BLIP;    break;
  }
  s_idx = 0; s_frames = 0; s_on = 1;
}

void sfx_tick(void) {
  if (!s_on || !s_seq) return;
  if (s_frames > 0) { s_frames--; return; }
  if (s_seq[s_idx].frames == 0) { sfx_silence(); return; }   /* end of jingle */
  note_on(s_seq[s_idx].rate);
  s_frames = s_seq[s_idx].frames;
  s_idx++;
}
