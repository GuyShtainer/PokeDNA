#ifndef GB_EDITOR_H
#define GB_EDITOR_H

#include <stdint.h>
#include <stdbool.h>
#include "gb_edit.h"

/*
 * gb_editor — the FIELD MODEL of the Game Boy mon editor screen, kept apart from the
 * screen so tests/host_gbeditor_test.c can drive every row against Guy's real saves.
 *
 * Pure C (no tonc, no FatFs, no GBA headers). It owns three things the screen would
 * otherwise have to know: WHICH rows a record has (Gen 2 has an item and friendship,
 * Gen 1 does not; a box record has no stats), WHAT each row says, and what the d-pad
 * does to it. The screen is left with drawing, the pickers and the keyboard.
 *
 * Every mutation goes through gb_edit's setters, so the refusals documented there
 * (an empty move slot has no PP, the HP DV is derived, a name unchanged is not
 * written) hold here without a second copy of the rule. Nothing here touches the
 * species: a Gen-1 species change needs base data this tree does not carry
 * (gb_edit.h, GbGen1Base), so that row is a later slice for both generations.
 */

/* Rows, in screen order. Not every record shows every row — gbe_fields() lists the
 * visible ones; the ids are stable so the screen can special-case a kind. */
enum {
  GBE_NICK = 0, GBE_OT, GBE_OTID, GBE_LEVEL,
  GBE_ITEM, GBE_FRIEND,                          /* Gen 2 only                          */
  GBE_CURHP,                                     /* current HP; PARTY records only (#231) */
  GBE_MV0, GBE_MV1, GBE_MV2, GBE_MV3,
  GBE_PPU0, GBE_PPU1, GBE_PPU2, GBE_PPU3,        /* PP Ups per move (0..3)              */
  GBE_PP0, GBE_PP1, GBE_PP2, GBE_PP3,            /* current PP                          */
  GBE_DVA, GBE_DVD, GBE_DVS, GBE_DVC,            /* Atk / Def / Spe / Spc DVs           */
  GBE_DVH,                                       /* HP DV: DERIVED, shown, not editable */
  GBE_GENDER,                                    /* Gen 2 only, gender-having species only;
                                                   * DERIVED from the Atk DV -- see gbe_adjust */
  GBE_SHINY,                                      /* Gen 2 only, every species (shininess has
                                                   * no gender_ratio gate); DERIVED from all
                                                   * four DVs -- see gbe_adjust's DV search,
                                                   * the same "toggle -> nearest DV combo"
                                                   * shape gbe_flip_gender already uses for
                                                   * GENDER (BACKLOG #95 parity: mirrors Gen
                                                   * 3's F_SHINY quick toggle, pdna_edit.c) */
  GBE_EGG,                                        /* Gen 2 only: the species-LIST byte, not a
                                                   * record field -- see gb_set_egg (BACKLOG
                                                   * #95 parity: Gen 3 has no discrete egg
                                                   * toggle of its own; this one exists because
                                                   * gb_set_egg already did, unlike Gen 3 where
                                                   * eggs are the CREATE flow's own species
                                                   * choice, not a later toggle) */
  GBE_METTIME, GBE_METLEVEL, GBE_METLOC, GBE_METOTGENDER,  /* Gen 2 only: the capture record
                                                   * gb_set_caught() packs into 0x1D/0x1E --
                                                   * BACKLOG #95 parity with Gen 3's own
                                                   * F_METGAME/F_METREGION/F_METLOC/F_METLEVEL
                                                   * rows (pdna_edit.c). No location NAME
                                                   * table for Gen 2 exists in this tree yet
                                                   * (same "#n" gap GBE_ITEM already has) --
                                                   * METLOC shows the raw id. */
  GBE_SE0, GBE_SE1, GBE_SE2, GBE_SE3, GBE_SE4,   /* stat exp HP/Atk/Def/Spe/Spc        */
  GBE_NUM
};

/* How the screen should react to A on a row. */
enum {
  GBE_K_NUM = 0,   /* d-pad adjusts; A jumps to an extreme (see gbe_press)             */
  GBE_K_TEXT,      /* A opens the keyboard; commit through gbe_set_text                 */
  GBE_K_MOVE,      /* A opens the move picker; commit through gbe_set_move              */
  GBE_K_ITEM,      /* A opens the (restricted, "#n") item picker; d-pad still numeric-
                     * steps the raw byte, same dual-input shape as GBE_K_MOVE (UX-parity
                     * audit, Guy 2026-09-07) -- see pdna_gbedit.c's own GBE_K_ITEM branch */
  GBE_K_SHOW       /* read-only row (the derived HP DV)                                 */
};

#define GBE_VALUE_MAX 48    /* gbe_value()'s buffer: a GB_TEXT_MAX name is the longest */

/* The rows this record shows, in order. Returns the count (<= GBE_NUM). */
int gbe_fields(const GbEditMon* e, uint8_t out[GBE_NUM]);

/* True iff this record shows GBE_GENDER: Gen 2 only, a resolvable species (dex != 0),
 * and a gender_ratio that is neither all-male (0x00) nor all-female (0xFE) nor
 * genderless (0xFF) -- those three have no Attack-DV threshold to flip, so there is
 * nothing a Gender row could ever change. Gen 1 never shows it: the game itself has no
 * gender concept, so there is nothing here to edit (see gbe_header's own comment on
 * what a VC transfer would derive). Exposed because pdna_gbsummary.c's cards hard-code
 * their own rows rather than iterating gbe_fields() and need the identical gate. */
bool gbe_has_gender_row(const GbEditMon* e);

const char* gbe_label(int f);
int         gbe_kind(int f);

/* Same as gbe_label except a Gen-2 EGG's Friendship row reads "Egg cycles" -- that
 * byte is the hatch counter while the list byte is 0xFD. */
const char* gbe_label_of(const GbEditMon* e, int f);

/* The row's value as text. Moves use pk_move_name (Gen-1/2 move ids ARE the Gen-3 ids
 * for 1..251); a Gen-2 held item has no name table in this tree and is shown as "#n". */
void gbe_value(const GbEditMon* e, int f, char* out, int cap);

/* d-pad LEFT/RIGHT (dir -1/+1); `big` = the shoulder buttons. Returns true iff the
 * record changed. Values clamp at their ends rather than wrapping, so holding a
 * direction is safe. Setter refusals (an empty move slot's PP) simply return false. */
bool gbe_adjust(GbEditMon* e, int f, int dir, bool big);

/* A on a GBE_K_NUM row: jump between extremes (current HP full <-> 0, DV 0 <-> 15, PP -> max, PP Ups
 * 0 <-> 3, stat exp 0 <-> 65535, level 100 <-> 1, friendship 0 <-> 255). Returns true
 * iff the record changed. */
bool gbe_press(GbEditMon* e, int f);

/* The keyboard's result for a GBE_K_TEXT row. Returns false — having written nothing —
 * when the generation cannot store `s` exactly; `first_bad` then names the glyph so the
 * screen can say which one. An unchanged name changes zero bytes (gb_edit.h, NAMES). */
bool gbe_set_text(GbEditMon* e, int f, const char* s, char first_bad[GB_GLYPH_MAX]);

/* The move picker's result for a GBE_K_MOVE row. Refuses an id this generation does
 * not have (the Gen-3 picker lists 354), and a duplicate of a move in another slot. */
bool gbe_set_move(GbEditMon* e, int f, uint16_t move);

/* One line for the header: species, level, and what the DVs make of it
 * ("PIKACHU  Lv25  F  SHINY" — gender/shiny/Unown letter are DV-derived in Gen 2 and
 * are what a Virtual Console transfer reads out of a Gen-1 record). */
void gbe_header(const GbEditMon* e, char* out, int cap);

/* Party records store stats the game recomputes only on level-up / withdrawal. After a
 * stat-affecting edit this says what the screen must tell the user before writing:
 * NULL = nothing to say (box record, or nothing stale); else a sentence. Gen 2 can be
 * recalculated here (gb_recalc_stats, base stats identical to Gen 3's); Gen 1 cannot
 * (no base-stat table in this tree), and the sentence says so. */
const char* gbe_stale_note(const GbEditMon* e);

/* -1 == the last gbe_adjust/gbe_press(GBE_SHINY) did not have to move gender to turn
 * shininess ON; 0/1 == it did, and this is the gender (male/female) the Pokemon ended
 * up as (some gender ratios have no shiny combination for one of the two sexes, so
 * turning shiny ON can force the other one). The underlying flag is
 * GbEditMon.shiny_gender_forced (gb_edit.h) — this reads the record's CURRENT
 * gender rather than remembering which way the search went. Meant for an immediate
 * message right after the toggle (pdna_gbedit.c), not the write confirm screen: see
 * pdna_layout.h's PDNA_GBEDIT_SHINY_FORCED_* comment for why (BACKLOG #95 review C2). */
int gbe_shiny_forced_gender(const GbEditMon* e);

/* Recompute party stats where this tree can (Gen 2). Returns true iff the record is
 * now not stale — also true when there was nothing to do. */
bool gbe_settle_stats(GbEditMon* e);

#endif /* GB_EDITOR_H */
