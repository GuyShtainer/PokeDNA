#ifndef GEN3_LEGALITY2_H
#define GEN3_LEGALITY2_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_mon.h"

/* Legality V2 — the check CATALOGUE (docs/research-legality-v2.md §3).
 *
 * V1 (gen3_legality.h) is a flat "warning / illegal" list. V2 keeps the same cheap
 * fixed-row shape but adds the three things the research doc asked for:
 *
 *   1. a THREE-state verdict per row. SUSPECT exists precisely so PokeDNA never calls
 *      a legitimate Pokemon illegal (OVERNIGHT-DECISIONS.md §2): "no known legit path,
 *      but exemptions exist" is a different statement from "impossible on retail", and
 *      an unsure checker must say unsure.
 *   2. CATEGORIES, so the screen can group rows and show a per-category glyph.
 *   3. HOOKS for the three table-driven check families that ship as separate modules
 *      (encounters, move sources, PID/IV RNG). They are optional: this core compiles,
 *      links and runs correctly with none of them present.
 *
 * This module is PURE C (hard convention 5): <stdint.h>/<string.h>/<stdbool.h> plus
 * data_tables.h. No tonc, no FatFs, no GBA headers — tests/host_legality2_test.c
 * compiles and runs it on the PC.
 *
 * Every check here is table-free: it needs only a decoded PkMon and the tables that
 * already ship (base stats / growth rates / abilities / items). ROM cost: code only,
 * no new data. EWRAM cost: zero — the report is returned by value on the caller's
 * stack (~1.4 KiB, same class as V1's PkLegality).
 */

/* Severity. Ordered: a later value outranks an earlier one. */
enum {
  PK2_INFO    = 0,  /* worth showing, not evidence of tampering (e.g. event flag set) */
  PK2_SUSPECT = 1,  /* no known legitimate path — but an exemption may exist          */
  PK2_INVALID = 2   /* impossible on retail hardware                                  */
};

/* Categories = the UI's section headers. */
enum {
  PK2_CAT_STRUCT = 0,   /* "STRUCTURE" — species/level/EXP/EVs/IVs/item/ability */
  PK2_CAT_MOVES,        /* "MOVES"     — ids, duplicates, PP (+ the moves hook)  */
  PK2_CAT_PID,          /* "PID/RNG"   — the PIDIV hook + PID-derived checks     */
  PK2_CAT_MET,          /* "MET"       — met level/ball/origin (+ encounter hook)*/
  PK2_CAT_EGG,          /* "EGG"       — egg-state coherence                     */
  PK2_CAT_FLAGS,        /* "FLAGS"     — language, names, ribbons, Pokerus       */
  PK2_NCAT
};

/* Overall grade for the banner. */
enum { PK2_LEGAL = 0, PK2_QUESTIONABLE = 1, PK2_ILLEGAL = 2 };

/* 39 chars + NUL: the row must fit 240 px in the 5x7 proportional font. 32 rows is
 * double V1's 24 and still leaves headroom for every hook to speak. */
#define PK2_MAX_ROWS  32
#define PK2_TEXT_LEN  40

typedef struct {
  uint8_t cat;                 /* PK2_CAT_*  */
  uint8_t sev;                 /* PK2_INFO / PK2_SUSPECT / PK2_INVALID */
  char    text[PK2_TEXT_LEN];
} Pk2Row;

/* Which optional check families were NOT linked into this build (bitmask). The UI
 * should say so rather than imply those checks passed. */
#define PK2_HOOK_MOVES      0x01
#define PK2_HOOK_ENCOUNTER  0x02
#define PK2_HOOK_PIDIV      0x04

typedef struct {
  uint8_t grade;               /* PK2_LEGAL / PK2_QUESTIONABLE / PK2_ILLEGAL */
  uint8_t n;                   /* rows used                                   */
  uint8_t n_suspect, n_invalid;
  uint8_t cat_n[PK2_NCAT];     /* rows per category (0 = category came out clean) */
  uint8_t cat_worst[PK2_NCAT]; /* worst severity per category                     */
  uint8_t hooks_absent;        /* PK2_HOOK_* bits for families not linked in       */
  uint8_t truncated;           /* rows dropped because the table filled up         */
  /* PIDIV result, filled by the PIDIV hook when it is present and ran.
   * method: 0 = none found, else 1/2/4 = Method 1/2/4, 5 = reversed-order (Unown). */
  uint8_t  pidiv_method;
  uint8_t  pidiv_ran;
  uint32_t pidiv_seed;
  Pk2Row   row[PK2_MAX_ROWS];
} Pk2Report;

/* Facts the core has already computed, handed to every hook so it does not redo the
 * work (and so all modules agree on, e.g., what "hatched" means). */
typedef struct {
  uint8_t  growth;             /* growth-rate id (pk_species_growth)               */
  uint8_t  level;              /* EFFECTIVE level: party plaintext, else from EXP  */
  uint16_t tid, sid;
  uint8_t  language;
  bool     species_ok;         /* a real Gen-3 species (not 0, not an unused slot) */
  bool     is_egg;             /* the record is still an egg                       */
  bool     is_hatched;         /* metLevel == 0 and not an egg => came from an egg */
  bool     is_gc;              /* origin 15 = Colosseum/XD (different rules)       */
  bool     fateful;            /* ribbons bit 31 (event / obedience flag)          */
  bool     pidiv_exempt;       /* egg, hatched or GC: no wild PID/IV correlation
                                * exists to test (doc §2.3, §5 exemptions)         */
} Pk2Facts;

/* Run the catalogue. `flags` is a bitmask; 0 = the fast set used by the box-wide
 * sweep (OVERNIGHT-DECISIONS.md §3: no PIDIV in the sweep — 30 x a 65,536-iteration
 * LCRNG search is a visible stall). */
#define PK2_RUN_PIDIV  0x01

void pk_check_legality2_ex(const PkMon* m, Pk2Report* out, uint8_t flags);
void pk_check_legality2(const PkMon* m, Pk2Report* out);   /* == _ex(..., 0) */

/* Append a row (bounds-checked, updates the counters/grade). Public because the
 * hooks below report through it. Text is truncated to PK2_TEXT_LEN-1 chars. */
void pk2_add(Pk2Report* R, uint8_t cat, uint8_t sev, const char* text);

/* Display helpers (pure, no allocation) — UI and tests share them. */
const char* pk2_grade_name(uint8_t grade);   /* "LEGAL" / "QUESTIONABLE" / "ILLEGAL" */
const char* pk2_cat_name(uint8_t cat);       /* "STRUCTURE" / "MOVES" / ...          */
const char* pk2_sev_name(uint8_t sev);       /* "info" / "suspect" / "INVALID"       */

/* ---- HOOKS: the table-driven check families -------------------------------
 * Each is defined WEAK in gen3_legality2.c as a no-op that records its own absence
 * in R->hooks_absent. A module that provides the real check simply defines the same
 * symbol strongly (no registration call, no RAM, no init order) and the linker
 * prefers it — the established pattern in source/art_fallbacks.c.
 *
 * Contract for an implementor:
 *   - report findings with pk2_add(R, <your category>, <sev>, "text");
 *   - do NOT clear or re-grade the report; pk_check_legality2_ex finalises it;
 *   - `f` is already filled: honour f->is_egg / f->is_gc / f->pidiv_exempt instead of
 *     re-deriving them, so exemptions stay consistent across modules;
 *   - stay pure C and allocate nothing (this runs on a 32 KiB IWRAM stack);
 *   - a check with no known-clean calibration ships as PK2_SUSPECT, never
 *     PK2_INVALID (OVERNIGHT-DECISIONS.md §2).
 *
 * pk2_hook_pidiv additionally owns R->pidiv_method / R->pidiv_seed and must set
 * R->pidiv_ran = 1. It is called ONLY when PK2_RUN_PIDIV is passed. */
void pk2_hook_moves(const PkMon* m, const Pk2Facts* f, Pk2Report* R);
void pk2_hook_encounter(const PkMon* m, const Pk2Facts* f, Pk2Report* R);
void pk2_hook_pidiv(const PkMon* m, const Pk2Facts* f, Pk2Report* R);

#endif /* GEN3_LEGALITY2_H */
