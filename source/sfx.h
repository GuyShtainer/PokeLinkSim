#ifndef SFX_H
#define SFX_H

/* Self-contained PSG (square-wave) sound effects — no maxmod, no sample assets.
 * Gated on config_get_sfx_enabled(). Notes are driven from the main loop:
 * sfx_play() starts a short jingle, sfx_tick() advances it one frame (call once
 * per vsync while it should play). All on PSG channel 1, so it never needs an IRQ
 * and never runs during an SD transfer (the caller only ticks it around writes). */

enum { SFX_BLIP = 0, SFX_TRADE, SFX_SUCCESS, SFX_CANCEL };

void sfx_init(void);     /* enable the PSG; call once at boot      */
void sfx_play(int id);   /* start a jingle (no-op if sound off)    */
void sfx_tick(void);     /* advance one frame; call per vsync      */
void sfx_silence(void);  /* stop immediately                      */

#endif /* SFX_H */
