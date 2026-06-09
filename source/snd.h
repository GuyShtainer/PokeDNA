#ifndef SND_H
#define SND_H

#include <stdbool.h>

/* Tiny UI sound effects via the GBA PSG square/noise channels (no samples, no
 * EWRAM, no maxmod). Each call fires a short note with an envelope that decays to
 * silence on its own, so nothing needs to be stopped. Safe to call every frame.
 *
 * NOTE: do NOT call these while an EZ-Flash SD transfer is in flight — sound regs
 * are fine, but keep the IRQ/OS-mode discipline; UI sounds only fire from the
 * normal menu loops, never mid-write. */

void snd_init(void);          /* enable the sound hardware (call once at boot)   */
void snd_set_enabled(bool on);/* master mute toggle (settings)                   */
bool snd_is_enabled(void);

void snd_move(void);          /* cursor moved — very short, quiet tick           */
void snd_ok(void);            /* A / confirm — bright blip                       */
void snd_back(void);          /* B / cancel — soft lower blip                    */
void snd_tab(void);           /* L/R page or tab change — mid blip               */
void snd_deny(void);          /* invalid / blocked action — low buzz             */
void snd_save(void);          /* save committed OK — rising two-note jingle      */
void snd_error(void);         /* save failed / hard error — descending buzz      */
void snd_edit(void);          /* a value was changed/applied — soft confirm      */
void snd_boot(void);          /* boot ready — gentle two-note welcome            */

/* Advance any multi-note jingle by one frame. Call once per frame from the main
 * VBlank tick (vsync). No-op when nothing is playing. */
void snd_vblank(void);

#endif /* SND_H */
