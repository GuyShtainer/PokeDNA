#ifndef PDNA_XFER_GATE_H
#define PDNA_XFER_GATE_H

/* BACKLOG #120 S2: the six one-line predicates that gate every Gen-3 write path
 * a Bank visit from a Game Boy session exposes. Pure C — no tonc, no sys.h
 * u8/u16, no pdna_box.h (the scope values travel as plain uint8_t) — so
 * tests/host_xfergate_test.c dual-compiles on the host. Each predicate is
 * commented with the call site it serves; the sites themselves live in
 * pdna_box.c / pdna_gen12.c / pdna_main.c (see docs/briefs/s2-bank-gb-session-brief.md). */

#include <stdint.h>
#include <stdbool.h>

/* pdna_main.c app_gen3_pc_live(): a live Gen-3 PC in g_pc requires both a parsed
 * save AND the arena not lent out (a GB session borrows g_pc as its mount arena). */
bool xg_pc_live(bool vinfo_valid, bool arena_held);

/* pdna_main.c :4385 TO GAME row: only offered from a Bank cell, only with a live
 * Gen-3 PC to receive it, only with something actually in hand. */
bool xg_togame_row(bool is_bank, bool pc_live, bool have_pc);

/* pdna_main.c :4382/:4392 PASTE rows (Bank only — :4208's RO_PASTE is untouched):
 * only with something clipped, only with a live Gen-3 PC to paste into. */
bool xg_paste_row(bool clip_occupied, bool pc_live);

/* pdna_main.c empty-cell CREATE row (app_mon_menu): CREATE builds a Gen-3 record off
 * g_vinfo (otId/trainer name) and, on a Bank cell, commits straight to the SD card
 * (banksrc_commit -> box_save) -- with no live Gen-3 save (a GB session's Bank
 * visit) that write would persist a checksummed record built off a zeroed g_vinfo,
 * a new write surface a later Gen-3 session's TO GAME could inject into the real
 * save. Always offered off a Bank cell (!is_bank -- PC/party's own CREATE is
 * unaffected); on a Bank cell, only with a live Gen-3 PC (BACKLOG #120 S2 F1). */
bool xg_create_row(bool is_bank, bool pc_live);

/* pdna_main.c app_inject_to_game()/_deferred()/app_commit_pc() first statement:
 * refuse writing into g_pc while it is the GB session's arena, or with no parsed
 * Gen-3 save to receive the write. */
bool xg_inject_refuse(bool arena_held, bool vinfo_valid);

/* pdna_gen12.c gb_session_core() exit block: a held carry that originated from a
 * GB-scope source must not survive back into a different session. */
bool xg_clear_carry_on_gb_exit(bool carry_is_gb);

/* pdna_box.c drop_held(): deny a drop when either side of the transfer is
 * BOXSCOPE_GB (== 2) — no cross-generation drop lands here, EXCEPT (BACKLOG #150
 * S150-4 decision 9) a Bank destination receiving a GB-scope carry when both sides
 * have an xfer vtable (`have_xfer`, since a bare predicate cannot see the vtable
 * itself) -- the UP edge this lane adds. Every other GB-involving pair (including
 * GB<-BANK, which is DOWN, not this lane) stays denied. */
bool xg_drop_denied(uint8_t dst_scope, uint8_t src_scope, bool have_xfer);

/* BACKLOG #150 S150-3: the escape-route gate. A native "GBC1" Bank cell
 * (source/bank_cell.h, bc_is_native) is, once S150-4 lands, the mon's ONLY copy on
 * the card -- it may only ever move WITHIN the Bank (a Bank-scope destination).
 * True when `rec80` is native AND the drop's destination is anything other than
 * the Bank (PC, GB, or any later scope) -- called before every 80-byte write a
 * held/carried record could reach: pdna_box.c's drop_held (the twelve sites named
 * in the brief), the party-place call, and the homeless B-cancel. */
bool xg_native_escape_denied(const uint8_t rec80[80], uint8_t dst_scope);

/* pdna_box.c drop_chunk(): the same rule as the line-1926 cross-generation refusal
 * it sits beside (`dst_scope == BOXSCOPE_GB || chunk_scope == BOXSCOPE_GB`), spelled
 * out as its own predicate -- a verbatim factoring of that existing expression, not
 * a new rule. Takes both scopes (not parameterless, docs/BANK-CROSSGEN-DESIGN.md
 * SS11.12 notwithstanding): a constant with no parameters could not be truth-tabled
 * and would not preserve the line it replaces (S150-3 decision 1). */
bool xg_chunk_crossgen_denied(uint8_t dst_scope, uint8_t chunk_scope);

/* BACKLOG #150 S150-7 decision D2 -- which DOWN arm a native "GBC1" Bank cell takes
 * off pdna_box.c's bank_down_dispatch(). Pure predicate: `cell_gen` = bc_kind(cell80)
 * (0 when not native), `dst_scope` = BOXSCOPE_* as a plain uint8_t, `dst_gen` = the
 * destination GB session's generation (GB_GEN1/GB_GEN2; 0 when the destination is not
 * a GB save at all). This header never includes bank_cell.h/gb_edit.h -- the caller
 * has already done the one read (bc_kind) this predicate needs.
 *
 *   cell_gen==0                        -> XG_DOWN_ARM_NONE   (not a native DOWN at all)
 *   dst_scope==BOXSCOPE_BANK           -> XG_DOWN_ARM_NONE   (native cells only ever
 *                                          leave the Bank through THIS dispatcher, and
 *                                          a Bank->Bank drop is drop_held's own
 *                                          within-scope move, never this arm)
 *   dst_scope==BOXSCOPE_PC             -> XG_DOWN_ARM_GEN3   (S150-8)
 *   dst_scope==BOXSCOPE_GB, same gen   -> XG_DOWN_ARM_EXACT  (S150-7, this lane)
 *   dst_scope==BOXSCOPE_GB, other gen  -> XG_DOWN_ARM_GB_BRIDGE (S150-8)
 *   dst_scope==BOXSCOPE_GB, dst_gen 0
 *     or out of range                  -> XG_DOWN_ARM_NONE   (no session, nothing can land) */
enum { XG_DOWN_ARM_NONE = 0,   /* not a native DOWN at all -- drop_held's own paths handle it */
       XG_DOWN_ARM_EXACT,      /* native cell -> SAME-generation GB save   (S150-7)           */
       XG_DOWN_ARM_GB_BRIDGE,  /* native cell -> OTHER-generation GB save  (S150-8)           */
       XG_DOWN_ARM_GEN3 };     /* native cell -> Gen-3 PC / party          (S150-8)           */
uint8_t xg_bank_down_arm(uint8_t cell_gen, uint8_t dst_scope, uint8_t dst_gen);

/* BACKLOG #150 S150-12 decision 9: a native cell marked BC_FLAG_COPY (b5) never lost
 * its GB original, so a DOWN of it must skip the Gen-3 ledger write entirely -- see
 * bank_cell.h's own BC_FLAG_COPY comment. bc_is_native() gates it first (a non-native
 * cell's byte 11 means something unrelated). */
bool xg_cell_is_copy(const uint8_t rec80[80]);

/* BACKLOG #150 S150-12 decision 13: the read-only mount's exit offer ("WAITING FOR
 * THE PC / N Pokemon. Put them in now?") fires only when `queued` copies are pending
 * AND a live Gen-3 PC (`pc_live`, xg_pc_live()) exists to receive them. */
bool xg_pc_offer(uint8_t queued, bool pc_live);

/* BACKLOG #239: the ONE switch for "may a second save be mounted while a Gen-3 save
 * stays resident" (source/nav_avail.c's nav_avail(NV_GB, ...) is the sole caller --
 * it is what makes the GB-IMPORT row itself say COMING_SOON / "Open the Bank instead."
 * for a Gen-3 save kind, exactly the same honesty the GB_TABLE already gives that row
 * for a raw Game Boy save). Deliberately a plain function, not a #define or a second
 * rule table (golden rule 8: one leash on the preprocessor) -- flipping this one `return`
 * is the whole re-enable, once hardware has validated the Bank-only flow and BACKLOG #239's
 * later pass is ready to delete the ledger/promote/reconcile machinery this gate currently
 * leaves standing but unreachable. Always false today: no direct save-to-save or
 * cart-to-save transfer is offered; deposit-to-Bank and take-from-Bank (both already
 * Bank-mediated, see BACKLOG #239's own survivor list) are untouched by this gate. */
bool xfer_direct_allowed(void);

#endif
