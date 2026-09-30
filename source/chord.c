/* SPDX-License-Identifier: GPL-3.0-or-later
 * chord.c -- see chord.h. Pure C. */
#include "chord.h"

void chord_reset(Chord* c) {
  if (!c) return;
  c->down = 0; c->used = 0;
}

void chord_swallow(Chord* c) {
  if (c && c->down) c->used = 1;
}

int chord_frame(Chord* c, uint16_t curr, uint16_t hit, bool live, uint16_t* fresh) {
  uint16_t f;
  int ev = CHORD_NONE;
  if (!c || !fresh) return CHORD_NONE;
  f = (uint16_t)(hit & (uint16_t)~CHORD_KEY_SELECT);          /* SELECT never acts on the press itself */
  if (hit & CHORD_KEY_SELECT) { c->down = 1; c->used = 0; }
  if (c->down && (hit & (CHORD_KEY_L | CHORD_KEY_R))) {        /* L/R pressed while SELECT is held (or with it) */
    c->used = 1;
    if (live) {
      ev = (hit & CHORD_KEY_L) ? CHORD_UNDO : CHORD_REDO;      /* both on one frame: undo wins, a redo is never guessed */
      f = (uint16_t)(f & (uint16_t)~(CHORD_KEY_L | CHORD_KEY_R));
    }
  }
  if (c->down && !(curr & CHORD_KEY_SELECT)) {                 /* released (or the release was missed) */
    if (!c->used) f = (uint16_t)(f | CHORD_KEY_SELECT);        /* a clean tap */
    c->down = 0; c->used = 0;
  }
  *fresh = f;
  return ev;
}
