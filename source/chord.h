/* SPDX-License-Identifier: GPL-3.0-or-later
 * chord.h -- the ONE SELECT chord helper (BACKLOG #234 slice 3, design D7). PURE C: only <stdint.h>/<stdbool.h>,
 * so tests/host_chord_test.c pins the real state machine on the PC.
 *
 * The rule (never open-coded at a screen): SELECT does NOT act on PRESS. It acts on RELEASE, and only if no L/R
 * was pressed while it was held. SELECT + L / SELECT + R fire undo / redo on the L/R PRESS instead. A screen
 * feeds each frame's key state through chord_frame() and then uses the FILTERED `fresh` mask exactly as it used
 * the raw one: KEY_SELECT appears in it only as a tap (on release), and KEY_L/KEY_R are removed from it while the
 * chord owns them, so plain L/R keep switching boxes/cards when SELECT is not held.
 *
 * Bit values are the GBA's KEYINPUT bits (tonc KEY_*), restated here so the core needs no tonc header. */
#ifndef CHORD_H
#define CHORD_H

#include <stdbool.h>
#include <stdint.h>

#define CHORD_KEY_SELECT 0x0004u
#define CHORD_KEY_L      0x0200u
#define CHORD_KEY_R      0x0100u

enum { CHORD_NONE = 0, CHORD_UNDO = 1, CHORD_REDO = 2 };

typedef struct Chord {
  uint8_t down;   /* SELECT is held (a press was seen and it has not been released)  */
  uint8_t used;   /* an L/R (or a swallow) happened while it was held: no tap on release */
} Chord;

void chord_reset(Chord* c);
/* Mark the current SELECT hold as used (another gesture consumed it): no tap fires on its release. */
void chord_swallow(Chord* c);
/* One frame. `curr` = keys held now, `hit` = keys pressed this frame (both KEYINPUT bit masks). `live` = undo/redo
 * may fire on this screen; when false SELECT+L/R still mark the hold used (no tap) but L/R pass through as plain
 * L/R. `*fresh` is REWRITTEN to the filtered fresh-press mask. Returns CHORD_NONE / CHORD_UNDO / CHORD_REDO. */
int  chord_frame(Chord* c, uint16_t curr, uint16_t hit, bool live, uint16_t* fresh);

#endif /* CHORD_H */
