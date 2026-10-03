#ifndef PDNA_SUMMARY_H
#define PDNA_SUMMARY_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_mon.h"   /* PkMon, for the exported chrome below */

/* Inline VIEW + EDIT of one Pokémon record (the 6 summary cards). Starts in VIEW:
 * A enters edit mode (can_edit only), U/D request the prev/next mon, L/R flip card,
 * B leaves. The "save changes?" prompt appears only when leaving or changing mon
 * with unsaved edits — on save it writes the edited 100/80-byte record to out_rec
 * and sets *saved. Returns 0 (exit), +1 (next mon) or -1 (prev mon); the caller
 * loads that mon and calls again. `saved` may be NULL.
 *
 * `card` carries the current summary card (0..5) in and out, so scrolling U/D to
 * the next mon stays on the same card (real-PC behaviour) instead of resetting to
 * card 0. Pass a caller-owned int that persists across the scroll loop; may be NULL
 * (treated as card 0, not written back). */
int pdna_inspect(uint8_t* rec, bool is_party, bool can_edit, uint8_t* out_rec,
                   bool* saved, int* card);

/* #234 s2: while ON (the caller sets it around a PC / party pdna_inspect and clears it after), leaving a mon with
 * edits does NOT raise "Save changes?" -- the caller stages the edit as one journal step and the exit save confirms
 * once. An edit that changes the mon's PID / OT id (a re-key candidate, a barrier) is still prompted: it is written
 * at once, so the prompt names what is about to happen. Default OFF (Bank, bases and every other caller keep it). */
void pdna_summary_quiet_save(bool on);
bool pdna_summary_quiet(void);   /* is the quiet-save posture set right now? (#234 s4: the GB native-cell edit asks it) */

/* CREATE mode: the same six cards over a brand-new record that is NOT in a save slot
 * yet (one that gen3_build_mon just made), so making a Pokémon looks like inspecting
 * one instead of dropping into a flat field list. Differences from pdna_inspect:
 *   - U/D never scroll to a prev/next mon — there isn't one.
 *   - START, or B, raises "Keep this Pokémon?"; A keeps, B discards.
 *   - the keep path fills out_rec and sets *saved EVEN IF nothing was edited, because
 *     the record itself is the new thing (gen3_edit_commit is lossless, so a no-edit
 *     keep reproduces gen3_build_mon's bytes exactly).
 * A green NEW chip replaces the VIEW/EDIT banner so it is obvious the mon is not
 * saved yet. Always returns 0. Editing is always allowed (the caller is Omega-gated). */
int pdna_inspect_create(uint8_t* rec, uint8_t* out_rec, bool* saved, int* card);

/* ---- shared chrome, exported for pdna_gbsummary.c (BACKLOG #41 slice E1: the Game
 * Boy summary restyled to this same Gen-3 CARD design, docs/SPRITE-ERA-DESIGN.md sec 3).
 * Every one of these is a straight export of pdna_summary.c's own static — same body,
 * same pixels, so the Gen-3 summary's own rendering is provably unchanged. */

/* The whole-screen blue gradient backdrop (card_paint_needed's caller repaints it
 * every time the card side needs a repaint at all). */
void pdna_summary_bg(void);
/* BACKLOG #203: the summary's blue portrait "screen" fill, (12,14)-(79,77). */
void pdna_summary_portrait_screen(void);

/* The shared left info column: framed sprite (drawn via the origin-art router, so a
 * Game Boy import shows in ITS OWN generation's art the same way it does inside a
 * Gen-3 save), dex no., name, level + gender, species, type badges, egg/shiny/Pokerus
 * tag. `back` selects the front/back sprite exactly like pdna_summary.c's own g_back —
 * pass false for a screen with no front/back toggle of its own. Unconditional: always
 * repaints (no I/O-skip memo) — the caller decides WHEN to call this, the same way
 * pdna_summary.c's own draw_left_conditional wraps the identical static draw_left(). */
void pdna_summary_draw_left(const PkMon* p, bool back);

/* Same, for a caller that KNOWS the source generation (1 or 2) because it
 * mounted the GB save itself — the provenance chip reads GB1/GB2 (certain)
 * instead of draw_left's own best guess ("GB?"). See pdna_summary.c's own
 * header comment on this function for the full rationale. */
void pdna_summary_draw_left_hint(const PkMon* p, bool back, uint8_t hint_gen, int t1o, int t2o);
/* #388: t1o/t2o are Gen-3-numbered type ids taken from the Game Boy RECORD itself (Gen 1 stores its own type bytes);
 * pass -1/-1 to draw the species-table types (Gen 2: its species table is its type table). */

/* The card-index dots (n dots, dot `active` lit). */
void pdna_summary_draw_dots(int x, int y, int n, int active);

/* The selection outline's save-under mechanism. Backed by pdna_summary.c's OWN
 * s_self_px buffer (588 B, EWRAM_BSS) — safe to share because the Gen-3 summary and
 * the Game Boy summary are never both on screen at once, and each screen drops the
 * outline state on its own entry (see pdna_gbsummary.c). Reusing this buffer instead
 * of declaring a second one is what keeps the Game Boy summary's EWRAM cost at zero
 * (docs/SPRITE-ERA-DESIGN.md sec 0: "EWRAM 524 B free. No new statics."). */
void pdna_summary_sel_frame_set(int sx, int sy, int sw);
void pdna_summary_sel_frame_hide(void);
void pdna_summary_sel_frame_drop(void);

#endif /* PDNA_SUMMARY_H */
