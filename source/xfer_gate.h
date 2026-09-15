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
 * BOXSCOPE_GB (== 2) — no cross-generation drop lands here (S2 has no lift path
 * yet; S3/S4 replace this with the real transfer). */
bool xg_drop_denied(uint8_t dst_scope, uint8_t src_scope);

#endif
