#ifndef SPRITE_ERA_H
#define SPRITE_ERA_H

#include <stdint.h>
#include <stdbool.h>

/*
 * sprite_era — the per-game, per-place "which generation's picture to draw" MODEL.
 *
 * Guy, 2026-09-05 (docs/SPRITE-ERA-DESIGN.md): "I really wanted that the sprites would
 * be exactly as in the game and from the game rom... lets make this a setting we can
 * change! The user should be able to choose in each game, which sprites to show and
 * where (pc, party, summary etc)." This file is slice E2: the PURE-C model + resolver
 * only. It owns no ROM, no pixels, no config file I/O — it decides WHICH era a screen
 * should ask for, and leaves fetching/drawing to the callers slice E3/E4 wire up
 * (rom_gbsprite, the Gen-3 ladder, pdna_origin_art's art router).
 *
 * PURE C. No tonc, no FatFs, no GBA headers, no stdio — only <stdint.h>/<string.h>/
 * <stdbool.h> — so tests/host_sprite_era_test.c compiles and runs this on the PC.
 * Following this codebase's convention for pure-C cores (gen3_places.c, pdna_places,
 * pdna_origin_art.c: zero `assert()` calls), every function here validates its
 * parameters DEFENSIVELY and returns a safe, documented default rather than aborting —
 * an abort is a hang on real hardware, a clamp is not.
 *
 * ---- THE FOUR AXES -----------------------------------------------------------------
 *
 * SeSaveKind — which SAVE is being viewed. RS/EM/FRLG are the three Gen-3 variants (the
 * map/icon ROM already tracks these separately, PkGame in gen3_trainer.h); GEN1/GEN2
 * are a raw Game Boy save (gb_edit.h's GB_GEN1/GB_GEN2).
 *
 * SePlace — WHERE the art is drawn: the in-save PC grid (small icons), the party list,
 * the summary's big portrait, the SD-backed bank (pdna_bank.c, all eras "in parallel"),
 * and a Game Boy save's own box grid.
 *
 * SeEra — WHICH era's picture. NATIVE is the sentinel "whatever era this record
 * actually is" (see se_native_era) — it is also what "no real art available" resolves
 * to, since the existing degrade ladder (compiled Gen-3 sprites, then the name chip) IS
 * the native-era behaviour that shipped before this feature existed.
 *
 * SeSetting — the 5x5 grid (kind x place) of a chosen SeEra, persisted to config.cfg.
 * Default is NATIVE everywhere, i.e. this feature is invisible until the user opens the
 * Settings "Sprites" grid (E4) and changes something.
 *
 * ---- THE RESOLVER CHAIN (se_resolve, spelled out here so the tests can hit every
 * branch by name) --------------------------------------------------------------------
 *
 *   1. WANTED     the grid cell for (kind, place). Checked against TWO gates, in this
 *                 order, before it is drawn as itself:
 *   2. NO_ICONS      place is the PC grid and the cell is GEN1 -- refused outright
 *                    (rom_gbsprite has no per-species Gen-1 icons, only the four-shade
 *                    bitplane portraits; SPRITE-ERA-DESIGN.md sec 2). Fall to NATIVE.
 *   3. NO_SPECIES    the cell is concrete (not NATIVE) and this species does not exist
 *                    there (se_species_exists). Fall to NATIVE.
 *   4. NO_ROM        the cell is concrete, the species exists, but the cell's ROM is
 *                    not registered. Fall to NATIVE.
 *   5. WANTED        none of the above fired -- WANTED is drawable as itself, return it.
 *   6. (native)   NATIVE resolves to a concrete era via se_native_era -- ONE OF SE_ERA_
 *                 GEN1/GEN2/G3_RS/G3_EM/G3_FRLG, never the NATIVE sentinel itself. That
 *                 concrete era is checked against the SAME TWO GATES as wanted was (2A.
 *                 CAUGHT THE BUG THIS COMMENT USED TO PAPER OVER: an EARLIER version of
 *                 this resolver applied the PC_GRID+GEN1 refusal and the species check
 *                 ONLY to `wanted`, so a Gen-1 import sitting in an Emerald PC box --
 *                 the DEFAULT path, no user override needed -- resolved to literal GEN1
 *                 for the PC grid, and a caller hint that force-set origin_gen=1 on a
 *                 Johto species (dex 152..251, e.g. a mis-hinted Tyranitar) would ask
 *                 for a picture Gen 1 never had. Both are checked again here, on
 *                 `native`, not just on `wanted`.) If native clears BOTH gates and its
 *                 ROM is registered, draw it (reason stays whatever step 1-5 left it
 *                 at: SE_WHY_WANTED if the cell was NATIVE to begin with and native
 *                 cleared everything, else whichever of NO_ICONS/NO_SPECIES/NO_ROM
 *                 WANTED failed for).
 *   7. COMPILED   native failed a gate, OR native's own ROM is ALSO absent. A native
 *                 Gen-3 mon or a GB import with no era-specific ROM registered both fall
 *                 here today (pdna_origin_art.h: "GB import + no such ROM -> the Gen-3
 *                 picture") -- if the build has compiled Gen-3 art (`compiled_gen3`),
 *                 draw it. There is no per-era SeEra value for "the compiled sprite
 *                 table" (it is not game-specific), so the return value is the NATIVE
 *                 sentinel and `reason` carries the news (SE_WHY_COMPILED, discarding
 *                 whatever step 1-6 left `reason` at -- once we are drawing the generic
 *                 fallback, WHY wanted/native failed no longer matters to the caller).
 *   8. CHIP       no compiled art either (the artless build) -- the caller draws the
 *                 name chip. Also the NATIVE sentinel, reason SE_WHY_CHIP.
 *
 * A resolver that returns SE_ERA_NATIVE therefore means "run the pre-existing pipeline
 * (compiled art, then chip) exactly as before this feature" -- never "draw literal
 * NATIVE pixels", because there is no such thing.
 */

/* ---- the four axes ----------------------------------------------------------------- */

typedef enum {
  SE_KIND_RS = 0,
  SE_KIND_EM,
  SE_KIND_FRLG,
  SE_KIND_GEN1,
  SE_KIND_GEN2,
  SE_KIND_N
} SeSaveKind;

typedef enum {
  SE_PLACE_PC = 0,
  SE_PLACE_PARTY,
  SE_PLACE_SUMMARY,
  SE_PLACE_BANK,
  SE_PLACE_GBGRID,
  SE_PLACE_N
} SePlace;

typedef enum {
  SE_ERA_NATIVE = 0,
  SE_ERA_GEN1,
  SE_ERA_GEN2,
  SE_ERA_G3_RS,
  SE_ERA_G3_EM,
  SE_ERA_G3_FRLG,
  SE_ERA_N
} SeEra;

/* The 5x5 (kind x place) grid of a chosen era, one byte per cell -- 25 bytes total,
 * small enough to live in config.cfg and in EWRAM without a second thought. */
typedef struct {
  uint8_t era[SE_KIND_N][SE_PLACE_N];
} SeSetting;

/* Which era ROMs are currently registered/openable. Index by SeEra; SE_ERA_NATIVE's
 * slot is IGNORED (native is a fallback sentinel, never something with its own ROM) --
 * se_era_has_rom-style checks always treat it as available. */
typedef struct {
  bool have[SE_ERA_N];
} SeRoms;

/* Why se_resolve returned what it did, so a screen can print "(no ROM)" or similar.
 * Checked in this order against both `wanted` and (separately) `native` -- see the
 * resolver chain above. */
typedef enum {
  SE_WHY_WANTED = 0,   /* drew exactly the configured cell (possibly NATIVE itself)    */
  SE_WHY_NO_ICONS,     /* PC_GRID + GEN1: no per-species Gen-1 icons exist              */
  SE_WHY_NO_SPECIES,   /* this era has no such species (se_species_exists)              */
  SE_WHY_NO_ROM,       /* this era's ROM is not registered                              */
  SE_WHY_COMPILED,     /* fell all the way to the compiled Gen-3 sprite table           */
  SE_WHY_CHIP          /* no art at all -- the caller draws the name chip               */
} SeWhy;

/* ---- the setting itself -------------------------------------------------------------- */

/* All 25 cells to SE_ERA_NATIVE -- the feature is invisible until the user changes a
 * cell in the Settings "Sprites" grid (E4). `s` may be NULL (no-op). */
void se_default(SeSetting* s);

/* What NATIVE means for one record: the era whose picture it would wear with no user
 * override at all -- exactly today's degrade ladder, expressed as a concrete SeEra.
 *
 *   kind is GEN1 or GEN2 (a raw Game Boy save)  -> that generation, trivially: every
 *       mon IN a Red/Gold save already IS that generation.
 *   kind is a Gen-3 save (RS/EM/FRLG)           -> `origin_gen` decides: 1 or 2 means a
 *       PokeDNA Game Boy import (pdna_origin_art.h's PdnaOrigin.gen -- ALREADY resolved
 *       to the max-likelihood guess for an unproven Kanto-dex import, i.e. Gen 1, by
 *       pdna_origin_of()'s which_gb_gen(); this function does not re-derive that, it
 *       only maps the already-decided gen onto an SeEra), anything else (3, or an
 *       out-of-range value, handled defensively) means a native Gen-3 mon, drawn in the
 *       SAVE'S OWN game's era.
 *
 * `origin_certain` (PdnaOrigin.gen_certain) does not change WHICH era this returns --
 * the max-likelihood default is already folded into `origin_gen` by the caller, exactly
 * as pdna_origin_of() hands it out -- it is accepted for API symmetry with se_resolve
 * and so a future caller can report the doubt without a second lookup. */
SeEra se_native_era(SeSaveKind kind, uint8_t origin_gen, uint8_t origin_certain);

/* Does this species exist as a picture in this era? NATIVE always answers true (it is
 * never itself a species restriction -- see the file header). A dex of 0 (empty slot)
 * answers false for every concrete era.
 *   GEN1               national_dex in 1..151
 *   GEN2               national_dex in 1..251
 *   G3_RS / G3_EM / G3_FRLG   national_dex in 1..386 (Deoxys and other multi-form
 *       species are keyed by their dex number only here -- which FORM to draw is the
 *       art router's business, not this existence check's). */
bool se_species_exists(SeEra era, uint16_t national_dex);

/* THE RESOLVER. See the file header for the full chain, spelled out step by step.
 * `s` and `roms` must be non-NULL; a NULL `s` or `roms` returns SE_ERA_NATIVE with
 * *reason = SE_WHY_CHIP (the safest, most degraded answer) without touching *reason
 * when `reason` itself is NULL. */
SeEra se_resolve(const SeSetting* s, SeSaveKind kind, SePlace place,
                  uint8_t origin_gen, uint8_t origin_certain, uint16_t national_dex,
                  const SeRoms* roms, bool compiled_gen3, int* reason);

/* THE ROUTER-FACING WRAPPER (E4 fix D1). se_resolve() answers a CONCRETE era at its
 * step 6 (the untouched/NATIVE-cell case, once `native` clears its own gates) -- e.g.
 * SE_ERA_G3_EM for an Emerald mon on an Emerald save, the moment that game's ROM is
 * registered. Every caller of se_resolve() that feeds an art ROUTER (pdna_origin_art's
 * era_resolver_cb) must NOT see that concrete answer when it is exactly what
 * se_native_era() would say for this same (kind, origin_gen, origin_certain): that
 * answer means "draw this record exactly as the pre-E4 pipeline always has" (the
 * compiled Gen-3 ladder / same-game ROM already open), and reporting it as a concrete
 * era instead sends EVERY untouched grid cell down the cross-game rung -- a second
 * ROM opened and cluster-walked per portrait, for a picture the old pipeline already
 * drew for free. An EXPLICIT override (e.g. an Emerald save's cell set to G3_FRLG)
 * differs from the native answer and is returned concrete, unchanged.
 *
 * `reason` (when non-NULL) is left exactly as se_resolve() set it -- this wrapper only
 * changes what the ROUTER does with the era, not the UI's account of why. */
SeEra se_resolve_for_router(const SeSetting* s, SeSaveKind kind, SePlace place,
                             uint8_t origin_gen, uint8_t origin_certain,
                             uint16_t national_dex, const SeRoms* roms,
                             bool compiled_gen3, int* reason);

/* ---- config.cfg text: "era_<kind>_<place>=<era>" ------------------------------------
 *
 * kind tokens (2 chars each, so key parsing is fixed-width): rs em fr g1 g2
 * place tokens:                                              pc party sum bank gbgrid
 * era tokens:                                                native g1 g2 rs em fr
 *
 * A key whose kind/place tokens do not parse is NOT OURS (se_config_apply returns
 * false so pdna_main.c's cfg_load can try its own keys next, the same shape as its
 * existing k_romkey loop). A recognised key with an unrecognised value sets that cell
 * to NATIVE (the safe default) and still returns true -- the key WAS ours.
 *
 * NOT EVERY CELL OF THE 5x5 GRID APPLIES -- see se_cell_applies below. The design doc's
 * "15 keys" was an early estimate; the actual dead-cell rule (a Gen-3 kind's own
 * SE_PLACE_GBGRID, and a Game Boy kind's SE_PLACE_PC) removes 5 of the 25, leaving 20
 * that can ever be written or read back.
 *
 * WIRING NOTE for E3/E4: pdna_main.c's cfg_save() writes into `char buf[PATH_MAX * 4 +
 * 128]` = 1152 bytes today (PATH_MAX is 256), via an UNCAPPED siprintf loop that trusts
 * the fixed set of existing keys to fit. Adding the sprite-era lines needs that buffer
 * grown and se_config_write called with the REMAINING capacity (`sizeof(buf) - n`), not
 * a fresh `sizeof(buf)` -- and its `truncated` out-param wired to a log_line() call, so
 * a buffer that turns out too small is a logged fact instead of a silently dropped
 * setting (cfg_save's existing philosophy: "nothing here is safety-critical... there is
 * no reason for it to be the unverified one" applies equally to truncation). */

/* Write every NON-DEFAULT, APPLICABLE (se_cell_applies) cell as one
 * "era_<kind>_<place>=<era>\n" line into `out` (capacity `cap`). Bounded: never writes
 * past `out[cap-1]`, always leaves a trailing NUL when cap >= 1. Returns the number of
 * bytes actually written (NOT counting the NUL, and never more than cap-1) -- a full
 * line that would not fit is simply not started (no half-written lines), so cap 1
 * writes nothing and cap "exactly enough" writes everything. `s` may be NULL (writes
 * nothing, returns 0). `truncated` may be NULL; when non-NULL it is always written:
 * true if at least one line had to be dropped for lack of room, false otherwise
 * (including every early-return case, where nothing was dropped because nothing was
 * attempted). */
int se_config_write(const SeSetting* s, char* out, int cap, bool* truncated);

/* Apply one already-split "key" / "value" pair (mirrors pdna_main.c's cfg_load, which
 * splits a line at '=' before dispatching). Returns false when the key is not one of
 * ours. `s` must be non-NULL when the key IS ours; a NULL `s` with one of our keys
 * still returns true (nothing to write to) since the KEY was recognised. */
bool se_config_apply(SeSetting* s, const char* key, const char* value);

/* ---- UI labels (independent of the config tokens above; may read differently) ------- */

/* Short label, <= 8 chars, never NULL; an out-of-range value returns "?". */
const char* se_era_name(SeEra e);
const char* se_kind_name(SeSaveKind k);
const char* se_place_name(SePlace p);

/* The Settings "Sprites" grid's A-button cycle: the next era AFTER `e` (wrapping) that
 * is actually offerable for this ROM set and place -- NATIVE is always offerable; a
 * concrete era is offered only when its ROM is registered (`roms`); GEN1 is never
 * offered when `place` is SE_PLACE_PC (the same refusal se_resolve applies). D8 (E4
 * review): a concrete GEN-3 era (G3_RS/G3_EM/G3_FRLG) is never offered at
 * SE_PLACE_BANK or SE_PLACE_GBGRID either -- the box GRID's per-mon overlay
 * (pdna_box.c's era_cell_draw) has no path to the cross-game Gen-3 rung at those two
 * places (its "wanted" pre-check only ever asks about a Game Boy art source), so
 * picking one there would look like a choice and never change a pixel. PC is
 * unaffected: se_store_era() gives it a separate, working whole-store mechanism.
 * Bounded to at most SE_ERA_N steps, so it always terminates -- NATIVE alone
 * guarantees a hit. */
SeEra se_era_next(SeEra e, const SeRoms* roms, SePlace place);

/* ---- small helpers for the E3/E4 wiring -------------------------------------------- */

/* Is `era`'s ROM registered? The same check se_resolve and se_era_next use internally,
 * exported so a caller can grey out a Settings row without re-deriving it. NATIVE is
 * always available (it is the fallback sentinel, never something with its own ROM). */
bool se_era_available(SeEra era, const SeRoms* roms);

/* Does the (kind, place) cell exist at all? Two dead-cell rules remove 5 of the 25
 * grid cells: a Gen-3 kind's (RS/EM/FRLG) SE_PLACE_GBGRID never applies (that place is
 * a Game Boy save's OWN box grid), and a Game Boy kind's (GEN1/GEN2) SE_PLACE_PC never
 * applies (a Game Boy save has no Gen-3-style PC box). se_config_write skips cells this
 * reports false for even if a caller somehow set one to a non-default value. An
 * out-of-range kind or place answers false. */
bool se_cell_applies(SeSaveKind kind, SePlace place);

/* Which era the icon store should build its ROM from for this (kind, place) cell: the
 * cell's own setting IF it is a concrete Gen-3 era (G3_RS/G3_EM/G3_FRLG) with a
 * registered ROM, else the SAVE'S OWN native era: G3_RS/G3_EM/G3_FRLG for a Gen-3 kind
 * (se_native_era(kind, 3, 1): a native Gen-3 mon of that game -- what the icon store's own
 * game ROM already means today), but GEN1 / GEN2 for a Game Boy kind (se_native_era
 * short-circuits on GB kinds before reading origin_gen, so the `3` is inert there). A
 * caller mapping the result onto a PkGame MUST handle the two GB values -- there is no
 * Gen-3 ROM behind them. GEN1/GEN2 CELL settings and NATIVE do not name a Gen-3 ROM and
 * fall through to that native answer (so for an EM/PC cell set to GEN2, se_resolve says
 * GEN2 per mon while the store stays EM -- E5, Gen-2 menu icons as PC-grid art, must
 * revisit this). `s` or `roms` NULL falls straight to the native answer. */
SeEra se_store_era(const SeSetting* s, SeSaveKind kind, SePlace place, const SeRoms* roms);

/* Map a PkGame value (gen3_trainer.h's PK_RS/PK_EMERALD/PK_FRLG, 0/1/2) onto its
 * SeSaveKind. Takes a raw `int`, not PkGame, so this header stays free of
 * gen3_trainer.h -- the .c file includes it privately and _Static_asserts the
 * numbering this mapping assumes. An out-of-range value defensively returns
 * SE_KIND_EM (a Gen-3 kind always exists to degrade to). */
SeSaveKind se_kind_from_game(int pkgame);

#endif /* SPRITE_ERA_H */
