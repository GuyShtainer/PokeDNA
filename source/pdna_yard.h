#ifndef PDNA_YARD_H
#define PDNA_YARD_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_mon.h"      /* PkMon */
#include "gen3_trainer.h"  /* PkGame */

/* pdna_yard — BACKLOG #114: the Day-Care yard scene, extracted verbatim out of
 * pdna_main.c's static ~5711-6089 region (dc_tri_roof .. dc_rescan) so the Gen-1/2
 * screen (pdna_gbdaycare.c, a later commit) can draw the same yard art + random
 * visitors Gen 3's pdna_daycare() already draws, instead of the two bare text rows
 * it used before. pdna_main.c keeps its own pdna_daycare()/dc_menu/dc_deposit/
 * dc_withdraw (SaveBlock1-specific; out of scope here) and now #includes this
 * header for dc_scene/dc_pointer/dc_icon_over_bg/dc_rescan/dc_roll_decos, whose
 * BODIES are unchanged from their old static selves — see the extraction commit's
 * PNG-cmp proof.
 *
 * Two file-scope globals dc_seed()/dc_rescan() used to read directly (g_vinfo,
 * g_sb1, g_game — all pdna_main.c-owned) are NOT exported raw: pdna_app.h's own
 * stated convention is "pdna_main.c owns g_sb1/g_frlg; these let [other files]
 * not need SaveBlock1 offset math or exposing g_sb1 itself" (golden rule 6,
 * smallest scope). dc_rescan takes sb1/game as explicit parameters instead
 * (pdna_daycare() already has both in scope at its two call sites); dc_seed calls
 * the new narrow accessor app_tid_public() (pdna_app.h) instead of reading
 * g_vinfo.tid_public directly. This is the ONLY behavioural difference from the
 * old static functions — same values in, same values out, just handed across the
 * new file boundary instead of read off a global. */

/* ---- shared compiled-yard-art probe (verbatim from pdna_main.c) --------------
 * Both pdna_main.c's own pdna_daycare() (three #ifdef HAVE_DAYCARE_BG branches
 * that stayed behind) and this file's dc_scene/dc_icon_over_bg must agree on
 * whether compiled yard art exists — hence the probe lives here, included by
 * both, rather than duplicated. See the comment on the probe itself for why this
 * is a build FLAG (PDNA_NO_DAYCARE_BG), not just a file-existence check: the
 * artless target filters daycare_bg_data.h out of the build without moving it
 * off disk. pdna_yard.c conditionally #includes daycare_bg_data.h itself, right
 * after this header, once HAVE_DAYCARE_BG is known — pdna_main.c never needs the
 * data file itself, only the macro. */
#if !defined(PDNA_NO_DAYCARE_BG) && defined(__has_include) && __has_include("daycare_bg_data.h")
#  define HAVE_DAYCARE_BG 1
#endif

/* Up to 5 invented yard-visitor mons per visit (2..5, dc_roll_decos' own range) —
 * the bound the drawing loops and the deco arrays share. */
#define PDNA_YARD_MAXDECO 5

/* ---- pure math core (host-testable, no tonc/GBA headers) --------------------
 * Rolls this visit's 2..5 yard-visitor species in [1, max_dex] from an explicit
 * RNG seed (same LCG dc_roll_decos always used: rng = rng*1103515245+12345).
 * Returns the final RNG state, OR'd with 1 (the area-pick stream seed the caller
 * feeds into dc_region_pick/dc_take_slot) — identical postcondition to the old
 * `s_dc_visit_rng = rng | 1u` assignment. `out_sp` must hold >= PDNA_YARD_MAXDECO
 * entries; `*out_n` is written 2..PDNA_YARD_MAXDECO.
 * BACKLOG #114: `max_dex` replaces the old hard-coded 251 so Gen 1 (no Gen-2 mon
 * has ever been born there) can cap the roll at 151 while Gen 2/Gen 3 keep 251 —
 * tests/host_yard_test.c exercises this directly. */
static inline uint32_t pdna_yard_roll_core(uint32_t seed, uint16_t max_dex,
                                            uint16_t out_sp[PDNA_YARD_MAXDECO],
                                            int* out_n) {
  uint32_t rng = seed;
  rng = rng * 1103515245u + 12345u;
  *out_n = 2 + (int)((rng >> 16) % 4);                    /* 2..5 */
  for (int i = 0; i < *out_n; i++) {
    rng = rng * 1103515245u + 12345u;
    out_sp[i] = (uint16_t)(1 + (rng >> 9) % max_dex);      /* internal species 1..max_dex */
  }
  return rng | 1u;
}

/* Roll THIS visit's yard visitors into the file-scope deco state below (replaces
 * the old dc_roll_decos(void), which hard-coded max_dex = 251). Gen 3 callers
 * (pdna_main.c's pdna_daycare) pass 251; the Gen-1/2 screen (a later commit)
 * passes 151 on a Gen-1 save, 251 on Gen 2 (BACKLOG #114: "gen 1 should have
 * gen 1 only and gen 2 should have both gen 1 and gen 2" — Guy's own wording;
 * Gen 1/2 share one national-dex-ordered species table 1..251, so a straight
 * modulo cap is exact, no species table needed). */
void pdna_yard_roll(uint16_t max_dex);

/* Turn OFF this visit's yard visitors (app_yard_visitors_ok() said no) without
 * rolling any: zeroes the pending-roll count and s_ndeco, and reseeds the area-
 * pick stream fresh anyway (dc_rescan still needs SOME stream to place the two
 * real boarders). Exactly what pdna_daycare()'s own "visitors off" branch did
 * inline before s_ndeco_roll/s_dc_visit_rng became private to this file. */
void dc_visitors_off(void);

/* Current visitor state, filled by dc_roll_decos()+dc_rescan() and read by the
 * drawing loops (pdna_main.c's pdna_daycare, and later pdna_gbdaycare.c). Never
 * exceeds PDNA_YARD_MAXDECO entries. File-scope in pdna_yard.c; declared extern
 * here per the SAME narrow-exposure convention as the rest of this header (these
 * are the yard's OWN state, unlike g_sb1/g_game/g_vinfo which stay behind
 * pdna_main.c's boundary) — accessor functions would be pure overhead for four
 * fields every caller already reads by index every frame. */
extern int      s_ndeco;
extern int      s_deco_x[PDNA_YARD_MAXDECO];
extern int      s_deco_y[PDNA_YARD_MAXDECO];
extern uint16_t s_deco_sp[PDNA_YARD_MAXDECO];

/* Paint the yard background (compiled art DMA, or the procedural sky/grass/fence
 * scene) into the scene region starting at screen y=12. Overdraws the whole
 * scene area; callers panel/overlay on top. */
void dc_scene(void);

/* Small downward selection arrow centred at (cx, y). */
void dc_pointer(int cx, int y);

/* Composite a 32x32 icon over the TRUE, unpainted yard background at screen
 * (x, y) — see the .c file's own header comment for why the source must never be
 * vid_mem itself. `num` is the blend weight out of 8 (8 = opaque real boarder,
 * lower = hazed invented visitor). `tick` picks the transport (false = load-time
 * DMA, true = idle-bob CPU copy — fd205bb's fix; never DMA on a per-vblank tick). */
void dc_icon_over_bg(int x, int y, const uint16_t* icon, int num, bool tick);

/* (Re)scan a Gen-3 daycare: fill recs[]/dc[]/phys[]/dcx[]/dcy[] for the up to 2
 * boarders (placed into the six type-grouped yard areas, DC_SPOT) and read the
 * shared egg/step state. `sb1`/`game` are pdna_main.c's own g_sb1/g_game, passed
 * explicitly (see this header's top comment). Returns the boarder count. */
int dc_rescan(uint8_t* sb1, PkGame game, uint32_t base, uint32_t stride,
              uint8_t* recs[2], PkMon dc[2], int phys[2],
              int dcx[2], int dcy[2], bool* egg, int* to_check);

#endif /* PDNA_YARD_H */
