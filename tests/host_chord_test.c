/* Host test for BACKLOG #234 slice 3 step 1: the SELECT chord helper (source/chord.c), design D7.
 * Every screen's SELECT / L / R handling goes through chord_frame(); this pins the REAL state machine.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_chord_test.c source/chord.c -o /tmp/hchord && /tmp/hchord
 *
 * The mutation run (python3 tests/host_chord_test.py-style, see the lane report) recompiles against copies
 * of chord.c with one rule removed; each mutant must fail at least one check below. */
#include <stdio.h>
#include <stdint.h>

#include "chord.h"

static int fails = 0, checks = 0;
static void check(const char* what, int cond) {
  checks++;
  if (!cond) { fails++; printf("  FAIL %s\n", what); }
}

#define S CHORD_KEY_SELECT
#define L CHORD_KEY_L
#define R CHORD_KEY_R

/* One frame; returns the event, *out = filtered fresh. */
static int fr(Chord* c, uint16_t curr, uint16_t hit, int live, uint16_t* out) {
  return chord_frame(c, curr, hit, live != 0, out);
}

int main(void) {
  Chord c; uint16_t f; int ev;

  /* (a) a clean tap: nothing on the press, SELECT on the release frame only */
  chord_reset(&c);
  ev = fr(&c, S, S, 1, &f);   check("tap: press frame emits no SELECT", !(f & S) && ev == CHORD_NONE);
  ev = fr(&c, S, 0, 1, &f);   check("tap: held frame emits nothing", f == 0 && ev == CHORD_NONE);
  ev = fr(&c, 0, 0, 1, &f);   check("tap: release frame emits SELECT", (f & S) && ev == CHORD_NONE);
  ev = fr(&c, 0, 0, 1, &f);   check("tap: fires once", f == 0);

  /* (b) SELECT held, then L press: undo on the L press, plain L suppressed, NO tap on release */
  chord_reset(&c);
  (void)fr(&c, S, S, 1, &f);
  ev = fr(&c, S | L, L, 1, &f); check("SEL+L: undo fires on the L press", ev == CHORD_UNDO);
  check("SEL+L: the L is removed from fresh", !(f & L));
  ev = fr(&c, S, 0, 1, &f);     check("SEL+L: L release is silent", ev == CHORD_NONE && f == 0);
  ev = fr(&c, 0, 0, 1, &f);     check("SEL+L: SELECT release emits NO tap (the on-release action must not also fire)", !(f & S));

  /* (c) SELECT+R: redo */
  chord_reset(&c);
  (void)fr(&c, S, S, 1, &f);
  ev = fr(&c, S | R, R, 1, &f); check("SEL+R: redo fires on the R press", ev == CHORD_REDO && !(f & R));
  ev = fr(&c, 0, 0, 1, &f);     check("SEL+R: no tap on release", !(f & S));

  /* (d) both pressed on the SAME frame (a real emulator chord) */
  chord_reset(&c);
  ev = fr(&c, S | L, S | L, 1, &f); check("same-frame SEL+L: undo", ev == CHORD_UNDO && !(f & (S | L)));
  ev = fr(&c, 0, 0, 1, &f);         check("same-frame SEL+L: no tap after", !(f & S));

  /* (e) plain L / R with SELECT not held: passes through untouched */
  chord_reset(&c);
  ev = fr(&c, L, L, 1, &f);   check("plain L passes through", (f & L) && ev == CHORD_NONE);
  ev = fr(&c, R, R, 1, &f);   check("plain R passes through", (f & R) && ev == CHORD_NONE);

  /* (f) L held BEFORE SELECT, then SELECT tapped: L was not pressed while SELECT was held -> still a tap */
  chord_reset(&c);
  (void)fr(&c, L, L, 1, &f);
  (void)fr(&c, L | S, S, 1, &f);
  (void)fr(&c, L, 0, 1, &f);
  check("L held before SELECT: the release is still a tap", (f & S) != 0);

  /* (g) not live: SEL+L is NOT an undo, L passes through as a plain L, and the hold is still 'used' */
  chord_reset(&c);
  (void)fr(&c, S, S, 0, &f);
  ev = fr(&c, S | L, L, 0, &f); check("not live: no undo event", ev == CHORD_NONE);
  check("not live: L passes through as a plain box/card switch", (f & L) != 0);
  (void)fr(&c, 0, 0, 0, &f);    check("not live: no tap after a SEL+L", !(f & S));

  /* (h) swallow (another gesture consumed the hold) */
  chord_reset(&c);
  (void)fr(&c, S, S, 1, &f);
  chord_swallow(&c);
  (void)fr(&c, 0, 0, 1, &f);    check("swallowed hold: no tap", !(f & S));
  chord_swallow(&c);            /* not held: harmless */
  (void)fr(&c, S, S, 1, &f);
  (void)fr(&c, 0, 0, 1, &f);    check("swallow does not leak into the next hold", (f & S) != 0);

  /* (i) a missed release (curr shows SELECT up, no earlier frame saw it): treated as the release */
  chord_reset(&c);
  (void)fr(&c, S, S, 1, &f);
  (void)fr(&c, 0, 0, 1, &f);
  check("release detected from curr alone", (f & S) != 0);

  /* (j) other keys pass through unfiltered; press+release inside one frame is a tap; NULL args are inert */
  chord_reset(&c);
  ev = fr(&c, 0x0001, 0x0001, 1, &f); check("A passes through", f == 0x0001 && ev == CHORD_NONE);
  ev = fr(&c, 0, S, 1, &f);           check("press+release in one frame is a tap", (f & S) != 0);
  check("NULL chord is inert", chord_frame(0, 0, 0, true, &f) == CHORD_NONE);
  check("NULL out is inert", chord_frame(&c, 0, 0, true, 0) == CHORD_NONE);

  printf("host_chord_test: %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
