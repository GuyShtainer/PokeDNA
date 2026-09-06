#ifndef PDNA_ORIGIN_ART_H
#define PDNA_ORIGIN_ART_H

#include <stdint.h>
#include <stdbool.h>

#include "gen3_mon.h"     /* PkMon */
#include "rom_sprite.h"   /* RomSprite -- gen3_ladder's third rung, see below */

/*
 * pdna_origin_art — "draw a Pokemon in the art of the generation it came from".
 *
 * Guy, 2026-08-09: "if a pokemon is from gen 1, use a gen 1 sprite, if its from gen 2,
 * use its gen 2 sprite. Its a nice intiative visual which is important when considering
 * transfaring. The bank should show all in parallel."
 *
 * So this is provenance you can SEE while deciding what to move up a generation, not
 * decoration. Two halves, deliberately separable:
 *
 *   (1) ORIGIN DETECTION  — pure, testable, no art involved. Given a decoded PkMon,
 *       which generation did this record come FROM?
 *   (2) THE ART ROUTER    — one accessor every screen calls to get "the portrait for
 *       this mon", which serves Gen-1 art, Gen-2 art or Gen-3 art and degrades in
 *       layers down to the artless build's name chip.
 *
 * PURE C. No tonc, no FatFs, no GBA headers — tests/host_originart_test.c compiles and
 * runs this file on the PC, including against Guy's real Gen-3 cartridge saves AND his
 * real Game Boy ones.
 *
 * ---- (1) WHAT ORIGIN DETECTION CAN AND CANNOT SEE ---------------------------------
 *
 * There is no "came from Gold" field in a Gen-3 record. Gen 3's origin-game nibble has
 * no value for a Game Boy game (gen12_convert.c:300-302), so the ONLY thing to read is
 * the fingerprint PokeDNA's own converter stamps on every import it makes:
 *
 *     SID 0  +  met location 0xFE ("in a trade")  +  Poke Ball  +  met game = the
 *     LOADED save's game                                   (gen12_convert.c:411-414)
 *
 * That signature alone is NOT sufficient, and saying so is the whole point of this
 * comment. A retail IN-GAME TRADE Pokemon has all four:
 *
 *     pokeruby/src/trade.c:4948   u8 metLocation = 0xFE;
 *     pokeruby/src/trade.c:851+   gIngameTrades[] otId values are 49562, 2259,
 *                                 50183 ... all below 65536, so their SECRET ID IS
 *                                 ZERO, exactly like an import;
 *     CreateMon leaves the ball at ITEM_POKE_BALL (=4, pokeruby items.h:10).
 *
 * What actually separates them is the DV LATTICE. Gen 1/2 store four 4-bit DVs and no
 * HP DV, and gen12_convert.c turns them into Gen-3 IVs by a fixed, lossy map:
 *
 *     IV = DV * 2 + GEN12_IV_LOW_BIT (0)      -> every IV is EVEN and <= 30
 *     one Special DV feeds BOTH Sp. Atk and Sp. Def -> those two IVs are EQUAL
 *     the HP DV is the other four DVs' low bits (gen12_hp_dv)
 *                                             -> the HP IV is fully DETERMINED by the
 *                                                other four IVs
 *
 * A native Gen-3 mon lands on that lattice by chance about 1 in 16384. Add the three
 * further exact invariants the converter guarantees — nature == stored EXP % 25
 * (gen12_nature), shininess == the Gen-2 DV rule (gen12_is_shiny), and EVs / contest
 * conditions / ribbons all zero, which retail in-game trades violate outright (they
 * carry MON_DATA_COOL..TOUGH + SHEEN, pokeruby/src/trade.c:4962-4968) — and the false
 * positive rate measured over every mon in Guy's five real cartridge saves is ZERO.
 * (tests/host_originart_test.c prints that count; it is the gate on this feature.)
 *
 * ---- WHICH Game Boy: PROOF vs EVIDENCE, and why the difference is the feature ------
 *
 * The converter records the source generation NOWHERE, so "Gen 1 or Gen 2" has to be
 * inferred from what survived the conversion. The rule this module is built on:
 *
 *     ONLY A SIGNAL THAT ORDINARY GEN-3 PLAY CANNOT PRODUCE MAY MAKE THE ANSWER
 *     CERTAIN. Everything else is evidence at most, and evidence never earns a label.
 *
 * That rule exists because the first version of this file broke it, and the damage was
 * measured rather than argued. It treated friendship != 70, Pokerus and a Gen-2-numbered
 * move as one-way proof of Gen 2. All three are freely mutable inside a Gen-3 game:
 * friendship rises every 128 steps you walk, Pokerus spreads through the party, and any
 * HM/TM (Rock Smash is move 249) is a Gen-2-numbered move. Over Guy's real Red.sav, all
 * 238 detectable Gen-1 imports flipped from an honest "GB?" to a CONFIDENT "GB2" — and
 * were drawn in Gen-2 art — after a single one of those three things happened to them.
 * A confident wrong label is worse than an honest unknown, so:
 *
 *   PROOF (PDNA_TELL_PROOF), Gen 3 cannot make these:
 *     * a Johto species that no Gen-1 species can evolve into. Gen 3 changes a species
 *       only by evolving it, and the ELEVEN Gen-2 ids a Kanto species reaches
 *       (gen3_legality_hooks.c's pre-evolution table: 169, 182, 186, 196, 197, 199,
 *       208, 212, 230, 233, 242) are excluded by name — six of them are trade/stone
 *       evolutions that change no EXP, so an import really can arrive there with its
 *       whole signature intact.
 *     * a FEMALE original trainer. The bit is stamped at creation from the OT's gender
 *       and nothing in Gen 3 rewrites it, and gen12_convert.c:415 only ever sets it for
 *       a Gen-2 record with Crystal caught data.
 *     * the caller telling us (pdna_origin_of_hint), which is not inference at all.
 *
 *   EVIDENCE (PDNA_TELL_WEAK), all reversible or reachable in Gen 3:
 *     friendship != 70, Pokerus, a move id in 166..251, and a Johto species that IS one
 *     of those eleven. These are reported in `tells` so a screen or a log can explain
 *     the '?', and they are allowed to choose the most likely PICTURE — but the tag,
 *     the box marker and the pad colour all stay neutral.
 *
 * SO GEN 1 IS NEVER PROVEN BY INFERENCE: a Gen-2 Kanto mon with friendship 70, no
 * Pokerus and a male OT is byte-identical to a Gen-1 one, and pretending otherwise is
 * the same mistake in the other direction. `pdna_origin_tag` answers "GB?" for it and
 * "GB1" only when a caller that KNOWS passed a hint.
 */

/* Which generation's art a record should wear. */
enum { PDNA_GEN1 = 1, PDNA_GEN2 = 2, PDNA_GEN3 = 3 };

/* verdict */
enum {
  PDNA_ORIGIN_NATIVE = 0,   /* nothing says GB -> Gen-3 art (this is the normal case) */
  PDNA_ORIGIN_GB     = 1    /* a PokeDNA GB import; `gen` is 1 or 2                   */
};

/*
 * Why this record reads as Gen 2 (bitmask in PdnaOrigin.tells). The split into PROOF
 * and WEAK is the contract: only a PROOF bit may set gen_certain.
 */
enum {
  /* --- PROOF: ordinary Gen-3 play cannot produce these --------------------------- */
  PDNA_TELL_JOHTO   = 0x01,  /* dex >= 152 and NOT reachable by evolving a Kanto mon  */
  PDNA_TELL_OTFEM   = 0x02,  /* female OT -- Crystal caught data only, never rewritten*/
  PDNA_TELL_HINT    = 0x04,  /* the caller knew (it converted the record itself)      */
  /* --- EVIDENCE: every one of these can happen inside a Gen-3 game ---------------- */
  PDNA_TELL_MOVE    = 0x08,  /* a move id in 166..251 -- or any Gen-3 TM/HM/tutor of
                              * the same number, e.g. Rock Smash (249)                */
  PDNA_TELL_POKERUS = 0x10,  /* Gen 1 has no Pokerus, but Gen 3 spreads it            */
  PDNA_TELL_FRIEND  = 0x20,  /* friendship != 70 -- 128 steps of walking does this     */
  PDNA_TELL_EVOJOHTO= 0x40   /* dex >= 152 but reachable from a Kanto species in Gen 3 */
};
#define PDNA_TELL_PROOF  (PDNA_TELL_JOHTO | PDNA_TELL_OTFEM | PDNA_TELL_HINT)
#define PDNA_TELL_WEAK   (PDNA_TELL_MOVE | PDNA_TELL_POKERUS | PDNA_TELL_FRIEND | \
                          PDNA_TELL_EVOJOHTO)

/* The FIRST signature clause that failed, i.e. why this is not a GB import. Reported
 * so the log and the host test can say something better than "no". */
enum {
  PDNA_SIG_OK = 0,
  PDNA_SIG_SPECIES,   /* internal species outside 1..251, or an egg / bad egg        */
  PDNA_SIG_SID,       /* secret id != 0                                              */
  PDNA_SIG_METLOC,    /* met location != 0xFE                                        */
  PDNA_SIG_BALL,      /* not a Poke Ball                                             */
  PDNA_SIG_LANG,      /* not English (the converter always builds English)           */
  PDNA_SIG_IV_ODD,    /* an odd IV -- cannot be DV * 2                               */
  PDNA_SIG_IV_SPLIT,  /* Sp.Atk IV != Sp.Def IV -- two Specials, not one Special DV  */
  PDNA_SIG_IV_HP,     /* HP IV is not the other four DVs' low bits                   */
  PDNA_SIG_EV,        /* a non-zero EV (Transporter erases stat exp; so do we)       */
  PDNA_SIG_CONTEST,   /* a non-zero contest condition or ribbon                      */
  PDNA_SIG_NATURE,    /* personality % 25 != experience % 25                         */
  PDNA_SIG_SHINY      /* stored shininess != the Gen-2 DV shiny rule                 */
};

typedef struct {
  uint8_t gen;          /* 1, 2 or 3 -- the era whose ART this mon should wear       */
  uint8_t verdict;      /* PDNA_ORIGIN_NATIVE / PDNA_ORIGIN_GB                       */
  uint8_t gen_certain;  /* 1 = `gen` is PROVEN; 0 = a GB import of an unproven era   */
  uint8_t tells;        /* PDNA_TELL_* bitmask -- see PDNA_TELL_PROOF / _WEAK        */
  uint8_t miss;         /* PDNA_SIG_* -- first failed clause when verdict is NATIVE  */
  uint8_t dv[4];        /* the recovered Atk/Def/Spd/Spc DVs, 0..15 (GB import only) */
} PdnaOrigin;

/* Decide the origin of one decoded record. Pure; no allocation, no I/O; safe to call
 * for all 30 slots while drawing a box. `m` may be NULL or an empty slot (species 0),
 * which reports NATIVE/Gen 3. */
void pdna_origin_of(const PkMon* m, PdnaOrigin* out);

/* Same, but the caller KNOWS the source generation because it produced the record
 * (the GB browse screen holds the mounted Gen-1/Gen-2 save). `hint_gen` 1 or 2 is
 * AUTHORITATIVE about the ERA — but never about whether there is a Pokemon here, so
 * the RECORD is still consulted: pdna_gen12.c walks every cell of a GB box including
 * the empty ones, and a slot with no species, an egg (the converter refuses those, so
 * what is drawn is a Gen-3 stand-in) or a species outside 1..251 reports NATIVE exactly
 * as pdna_origin_of would. 0 (or anything else) means "no hint". */
void pdna_origin_of_hint(const PkMon* m, uint8_t hint_gen, PdnaOrigin* out);

/* Short provenance tag for a portrait:
 *     "GB2"  proven Gen 2      "GB1"  a caller's hint said Gen 1
 *     "GB?"  a GB import whose era is not proven — the honest common case
 *     ""     a native Gen-3 mon
 * Never NULL; static storage, valid forever. */
const char* pdna_origin_tag(const PdnaOrigin* o);

/* One honest line for the summary / legality screen. Never NULL. */
const char* pdna_origin_text(const PdnaOrigin* o);

/* Single character for a 24x22 box cell: '1', '2', '?' or 0 (native -> no marker). */
char pdna_origin_mark(const PdnaOrigin* o);

/* Era colour for the cell pad / tag ink, RGB15 (no alpha bit). Native returns 0. An
 * UNPROVEN era returns the neutral Game Boy grey, never an era's colour — the picture
 * may be a best guess, the branding never is. */
uint16_t pdna_origin_color(const PdnaOrigin* o);

/* ---- (2) THE ART ROUTER ----------------------------------------------------------
 *
 * Which ROM to read Game Boy art from is a REGISTRATION question, exactly like the
 * Gen-3 ROM the map/icon/description paths already use (app_rom_path in pdna_app.h,
 * Settings > Game ROM in pdna_main.c). This module never opens a file: pdna_main.c
 * registers a source and clears it when the registration changes, the same shape as
 * boxoam_rom_icons(). That keeps this file pure C AND means a build with no GB reader
 * at all still links and still works.
 *
 * The source hands back pixels in the ONE format every PokeDNA screen already draws:
 * RGB15, 0 = transparent, 0x8000|rgb = opaque -- what ui_sprite() takes and what
 * mon_front_for_form() returns. Whatever buffer it decodes into is the source's own
 * business; the pointer must stay valid until the NEXT call, which is the same
 * contract app_item_desc() publishes.
 *
 * THE ROUTER MEMOISES THE LAST FETCH, so that contract is load-bearing in one extra
 * way: every pic() call in the program goes through this module, and the router
 * assumes nothing ELSE overwrites the source's buffer between two calls. A source that
 * shares its decode buffer with another decoder (mon_decomp, say) must therefore
 * re-register — pdna_origin_art_register() drops the memo — whenever that buffer is
 * handed to someone else. */
typedef struct PdnaGbArtSource {
  /* One species' picture in era `gen` (1 or 2). `dex` is the NATIONAL number 1..251.
   * `form` is the Unown letter 0..25 (A..Z) when dex == 201, and 0 for everything
   * else — Gen-2 Unown really is 26 different pictures and gen12_convert.c goes out
   * of its way to preserve the letter through the import (gen12_unown_letter feeding
   * solve_pid's want_letter), so a vtable that could not carry it would throw that
   * work away and draw the wrong letter.
   * `back` requests the back sprite (0 = front); a source with no back art returns
   * NULL and the router falls back to the front, exactly as the Gen-3 path does.
   * `shiny` is honoured where the era has shiny palettes (Gen 2) and ignored in Gen 1.
   * Writes the real pixel size to *out_w / *out_h (Gen 1/2 pics are not 64x64).
   * Returns NULL when this ROM cannot serve it.
   * The argument order is rom_gbsprite_pic()'s (dex, form, ...) on purpose. */
  const uint16_t* (*pic)(void* ctx, uint8_t gen, uint16_t dex, uint8_t form,
                         uint8_t back, uint8_t shiny, uint8_t* out_w, uint8_t* out_h);
  /* Is a ROM for that era registered at all? Cheap; used to skip work. */
  int (*have)(void* ctx, uint8_t gen);
  void* ctx;
} PdnaGbArtSource;

/* Register (or, with NULL, clear) the Game Boy art source. The struct is COPIED, so
 * the caller may keep it on the stack. Clearing restores the pre-feature behaviour
 * everywhere: every screen falls straight back to the Gen-3 ladder. Also drops the
 * fetch memo, so a re-registration is the way to say "my buffer moved". */
void pdna_origin_art_register(const PdnaGbArtSource* src);

/* 1 if era `gen` (1 or 2) can currently produce art. Always 0 with no source. */
int pdna_origin_art_have(uint8_t gen);

/* E3 review re-verification (2026-09-06): "is there enough STACK left right now to
 * safely take the GB rung" -- a GB fetch (gb_art_source.c's gb_art_pic_cb ->
 * gb_art_fetch -> rom_gbsprite_pic_buf -> ... -> the SD read tail) can cost up to
 * PDNA_GB_FETCH_NEED bytes on top of whatever the caller chain already used, and one
 * real caller chain (Bank -> box -> party strip -> party menu -> mon menu -> Daycare
 * -> inspect -> summary -> the portrait) is already 7,384 B deep with only ~4,112 B
 * left by the time it gets there -- not enough. pdna_origin_art_portrait() consults
 * this immediately before taking the GB branch and falls through to gen3_ladder when
 * it says no; out->era/out->era_certain are set from origin detection BEFORE that
 * check either way, so the tag/pad stay honest ("this IS a Game Boy import") even
 * when the PIXELS degrade to the Gen-3 rung.
 *
 * PURE C DEFAULT: with no hook registered, this always answers "yes, there is room"
 * (1) -- exactly today's behaviour, which is what the host build and every existing
 * host test keep getting. The GBA build registers a REAL check (gb_art_source.c,
 * reading the CPU's own SP against the linker's low-water mark) at boot; a host test
 * that wants to exercise the "no room" branch registers its own stub.
 *
 * D4 (E4 review): the ONE hook used to answer a single fixed PDNA_GB_FETCH_NEED
 * (6,144 B) for every rung -- correct for the GB fetch it was measured against, but
 * the cross-game Gen-3 rung's own subtree measures ~2,480 B, and reusing the 6,144-B
 * gate refused stack room on chains where the SMALLER, real need would have fit. The
 * hook now takes the caller's own measured need in bytes, so each rung gates on its
 * own subtree instead of borrowing another rung's number. */
int pdna_origin_art_stack_room(int need);
typedef int (*PdnaStackRoomFn)(int need);
void pdna_origin_art_set_stack_room_hook(PdnaStackRoomFn fn);

/* D4: the cross-game Gen-3 rung's own measured need, parallel to gb_art_source.h's
 * PDNA_GB_FETCH_NEED (6,144 B) -- 2,480-2,512 B measured (g3cross_pic_cb ->
 * g3x_fetch_other/g3x_decode's own subtree, two -fstack-usage runs) plus head-room.
 * NOTE: on the deepest chain that reaches it (Bank -> box -> party strip -> Day-Care ->
 * summary, HW-TEST §K6) only ~3,400 B are free at the gate, so any growth of ~340 B on
 * that chain silently (and safely) re-disables cross-game art there. Declared here (not gb_art_source.h) since
 * this rung is pdna_origin_art.c's own, not gb_art_source.c's. */
#define PDNA_G3X_FETCH_NEED 3072

/* ---- (3) THE GEN-3 ROM RUNG (Phase 1, docs/analysis-2026-08-19-rom-art/DESIGN.md
 * Sec 4.3) --------------------------------------------------------------------------
 *
 * gen3_ladder tries the compiled front/back accessors first; when BOTH answer NULL
 * (the artless build, or a species/form absent from the compiled set) it now falls
 * through to rom_sprite.c, streamed straight from the user's own retail ROM. Exactly
 * the same registration shape as pdna_origin_art_register() above: this module never
 * opens a file, pdna_main.c owns the RomSprite's lifetime (the same open ROM that
 * already serves box icons and descriptions, app_icon_rom_open() in pdna_main.c) and
 * re-registers whenever that changes. `rs` is COPIED (it is a small POD with no
 * self-referential pointers — rom_sprite.h's "reload, never copy" warning is about
 * RomItemArt's sheet cache, not RomSprite), so the caller may keep it on the stack.
 * NULL (or an `rs` that failed rom_sprite_open) clears the rung. */
void pdna_origin_art_set_romsprite(const RomSprite* rs);

/* ---- (3b) PLACE-AWARE ROUTING (E4, docs/SPRITE-ERA-DESIGN.md) ---------------------
 *
 * Slices E1/E2 built sprite_era.c's pure resolver (SeSetting/SeRoms/se_resolve); this
 * is where the ROUTER asks it. pdna_origin_art.c deliberately does NOT include
 * sprite_era.h or call se_resolve() itself -- the router only knows "ask the
 * registered resolver hook for an era", the same dependency-inversion shape
 * pdna_origin_art_register()/pdna_origin_art_set_romsprite() already use for pixels.
 * That keeps this module testable with a bare-function stub (no SeSetting/config
 * plumbing needed to exercise the router) and keeps the POLICY (which cell the user
 * picked, which ROMs are registered) entirely in pdna_main.c, which owns config.cfg.
 *
 * WHERE `place` comes from: a screen sets it ONCE on entry (pdna_box.c for the PC
 * grid / the bank / a GB save's own grid; pdna_main.c's app_party_overlay for the
 * party list; pdna_summary.c's pdna_inspect for the big portrait) — not once per mon,
 * since it does not change while that screen is open. Values are sprite_era.h's
 * SePlace, carried as a plain int so this header need not include it. Defaults to
 * SE_PLACE_SUMMARY(2)'s numeric value until the first screen sets it, matching
 * se_resolve()'s own defensive default for an out-of-range place. */
void pdna_origin_art_set_place(int place);
/* Read back the current place -- for a screen that opens ANOTHER screen (the box/
 * party screens opening a mon's summary mid-visit) to save its own place, let the
 * nested screen set its own, and restore the outer one on return rather than leaving
 * PLACE stuck at whatever the nested screen last set it to. */
int pdna_origin_art_get_place(void);

/* The resolver hook: given the CURRENT place, this record's origin (gen 1/2/3 and
 * whether that is proven), and its NATIONAL dex number, return an SeEra as a plain
 * int (sprite_era.h's SE_ERA_*, 0 = NATIVE) and, if `reason` is non-NULL, why
 * (SE_WHY_*). pdna_main.c's registered implementation is a thin wrapper around
 * se_resolve(&g_era, app_save_kind(), (SePlace)place, origin_gen, origin_certain,
 * national_dex, &app_era_roms(), compiled_gen3, reason) -- this module never sees
 * any of those inputs directly. NULL (never registered -- the host build's other
 * tests, and any screen reached before pdna_main.c's boot registration runs) means
 * "always NATIVE", i.e. today's exact behaviour: the router's NATIVE branch is
 * character-for-character what pdna_origin_art_portrait() did before this slice. */
typedef int (*PdnaEraResolverFn)(int place, uint8_t origin_gen, uint8_t origin_certain,
                                 uint16_t national_dex, int* reason);
void pdna_origin_art_set_era_resolver(PdnaEraResolverFn fn);

/* ---- (3c) THE CROSS-GAME GEN-3 RUNG (E4) -------------------------------------------
 *
 * When the resolver names a CONCRETE Gen-3 era (SE_ERA_G3_RS/G3_EM/G3_FRLG) that is
 * NOT the mon's own native game, the picture has to come from a DIFFERENT retail ROM
 * than whichever one is already open for icons/descriptions/portraits. Opening a
 * second ROM needs FatFs (a FIL, f_open/f_read), which this module may never touch
 * (hard convention 5: pure-C cores stay free of tonc/FatFs/GBA headers) — so, exactly
 * like the Game Boy rung's PdnaGbArtSource, this is a REGISTERED vtable and the real
 * file I/O lives in pdna_main.c (or a sibling module it wires up), never here.
 *
 * `game` is a raw int (PkGame's PK_RS/PK_EMERALD/PK_FRLG == 0/1/2, gen3_trainer.h) so
 * this header stays decoupled from gen3_trainer.h the same way se_kind_from_game()
 * keeps sprite_era.h decoupled from it. The callee decides for itself whether `game`
 * is the CURRENTLY open icon ROM (reuse that RomSprite directly, no new file) or a
 * different one (open app_rom_path(game) in a temporary RomCtx, decode, close) --
 * this module has no opinion either way, only the result. Returns NULL on any
 * failure (no ROM registered for `game`, a species/form that ROM cannot show, a
 * stack-room refusal, a read that failed verification) and the router falls through
 * to gen3_ladder exactly like every other rung's NULL. */
typedef struct PdnaG3CrossSource {
  const uint16_t* (*pic)(void* ctx, int game, uint16_t species, uint8_t form,
                         uint8_t back, uint8_t shiny, uint8_t* out_w, uint8_t* out_h);
  void* ctx;
} PdnaG3CrossSource;

/* Register (or, with NULL, clear) the cross-game source. Copied, like the other two
 * vtables above. */
void pdna_origin_art_set_g3cross(const PdnaG3CrossSource* src);

/* What a screen got back. `px == NULL` means "no art at all" and the caller paints
 * the artless build's name chip, exactly as it does today. */
typedef struct {
  const uint16_t* px;   /* RGB15 + 0x8000 opacity, or NULL                          */
  uint8_t w, h;         /* real pixel size: 64x64 for Gen 3, smaller for Gen 1/2    */
  uint8_t gen;          /* the era the PIXELS are from (3 when GB art was absent)   */
  uint8_t era;          /* the era the mon SHOULD wear -- always == origin.gen      */
  uint8_t era_certain;  /* 0 = `era` is a best guess; present it neutrally          */
  uint8_t egg;          /* 1 = this is the Egg picture; chip fallback is the Egg    */
  /* E4 (sprite-era, docs/SPRITE-ERA-DESIGN.md): which Gen-3 GAME the pixels came
   * from, when gen == PDNA_GEN3 and a cross-game rung served them. 0 = compiled
   * art or the currently-open icon ROM (today's behaviour, unlabelled); 1/2/3 =
   * PkGame's PK_RS/PK_EMERALD/PK_FRLG + 1 (gen3_trainer.h) -- kept a raw int here,
   * not a PkGame, so this header stays free of gen3_trainer.h. */
  uint8_t game;
} PdnaArt;

/*
 * The one accessor the screens call. Picks Gen-1 art, Gen-2 art or Gen-3 art for `m`
 * and degrades cleanly at every level:
 *
 *   GB import + a registered ROM for its era     -> that era's picture
 *   GB import + no such ROM                      -> the Gen-3 picture
 *   native Gen-3 mon                             -> the Gen-3 picture
 *   no Gen-3 art either (artless build)          -> px == NULL, caller draws the chip
 *
 * WHEN THE ERA IS NOT PROVEN it still serves Game Boy art — the record is PROVEN to be
 * an import, only WHICH Game Boy is open — and it serves the max-likelihood era, which
 * for a Kanto species is Gen 1: a Gen-1 import always lands there, and the Gen-1
 * rendering is the four DMG greys (rom_gbsprite.h:269-271), i.e. the generic Game Boy
 * look rather than a specific Game Boy Color palette. Everything that CLAIMS an era —
 * the tag, the box marker, the pad colour and out->era_certain — reports the doubt.
 * The alternative (refuse GB art unless the era is proven) was rejected because it
 * turns the feature off for its single commonest case, a freshly imported Gen-1 mon,
 * which is the one the request was about.
 *
 * `back` selects the back sprite where one exists (the summary's SELECT toggle);
 * a missing back always falls back to the front, in both eras.
 *
 * `origin` may be NULL; when non-NULL it receives the detection result so the caller
 * can print the tag WITHOUT running the detection twice.
 *
 * CALLABLE EVERY FRAME. pdna_summary.c's portrait_redraw() runs this once per frame
 * for the whole time the summary is open, and on hardware a GB fetch is an SD read and
 * an RLE decode. The router therefore memoises the LAST picture it fetched (era, dex,
 * form, side, shiny) and returns it without touching the source again, so an animating
 * portrait costs exactly ONE fetch, not one per frame. Detection itself is re-run every
 * call — it is a few dozen integer ops and re-running it is what keeps an edit in the
 * summary from showing a stale verdict.
 *
 * WITH NO SOURCE REGISTERED THIS IS EXACTLY THE OLD TWO LINES:
 *     spr = back ? mon_back_for_form(...) : 0;
 *     if (!spr) spr = mon_front_for_form(...);
 * -- same order, same fallback, same 64x64. That equality is asserted by the host
 * test, because "screens must not regress" is a requirement, not a hope.
 *
 * Returns 1 when out->px is non-NULL. */
int pdna_origin_art_portrait(const PkMon* m, int back, PdnaArt* out, PdnaOrigin* origin);

/* Where to blit art of this size inside a portrait rect: centred horizontally,
 * anchored at the BOTTOM (feet on the floor), which is what makes a 56x56 Game Boy
 * pic sit correctly in the same frame as a 64x64 Gen-3 one. Pure integer math; for
 * 64x64 art in the summary's (12,14,68,64) frame it returns (14,14) -- today's
 * literal constants. Clamped so oversized art never starts off the rect. */
void pdna_origin_art_place(const PdnaArt* a, int x, int y, int w, int h,
                           int* out_x, int* out_y);

/* Forget the memoised last fetch. The memo holds a POINTER into the source's own
 * decode buffer, and its validity rests on "nothing else overwrites that buffer
 * between two calls" (see the PdnaGbArtSource contract above). A caller that is about
 * to hand the same buffer to another decoder -- the box grid does exactly this: it
 * draws era art through mon_decomp and then the Gen-3 portrait streams through the
 * same buffer -- calls this so a later memo HIT cannot serve pixels that are no longer
 * there. Costs nothing; the next request re-fetches. */
void pdna_origin_art_invalidate(void);

/* ---- (3) THE BANK IN PARALLEL ----------------------------------------------------
 *
 * The bank (pdna_bank.c) is 16 SD-backed boxes where mons from all three eras sit side
 * by side, and Guy wants their provenance visible simultaneously. The grid is the
 * SHARED box screen (pdna_box.c drives both the in-save PC and the bank through one
 * BoxSource), so nothing here forks a second renderer: it publishes a 30-entry cache
 * the existing grid reads per cell.
 *
 * WHAT GOES IN A CELL. Two layers, and BOTH of them are "all in parallel" -- they
 * differ in what they cost and therefore in when they are there:
 *
 *   LAYER 1, THE PICTURE. Every cell whose mon is a GB import, and whose era has a
 *   registered ROM, is drawn in THAT ERA'S OWN SPRITE, scaled into the 24x22 cell and
 *   blitted to the Mode-3 bitmap with that cell's Gen-3 OBJ icon hidden. Gen-3 natives
 *   keep their OBJ icon. So a bank box holding a Red import, a Crystal import and a
 *   native shows Gen-1 art, Gen-2 art and Gen-3 art simultaneously, which is the
 *   request. It is not free -- see COST below -- and it is only available when the user
 *   has registered the Game Boy cartridge dump it reads from.
 *
 *   LAYER 2, THE LABEL. Every GB cell also carries a small era pad ('1' / '2' / '?' in
 *   the era's colour) at its top-left corner. This layer costs NOTHING at draw time
 *   (one cached byte per cell) and is therefore ALWAYS present: no GB ROM registered,
 *   the artless build, a species the ROM could not serve -- the provenance still reads
 *   off the grid. It also settles the case layer 1 cannot: a Gen-1 and a Gen-2 sprite
 *   of the same species can be near-identical in silhouette, so the picture alone is
 *   not an unambiguous answer and was never meant to be the only one.
 *
 * COST, MEASURED, not estimated. A GB pic is compressed in the SD-backed ROM and is
 * decoded on demand. Counting DISTINCT 512-byte sectors per picture over 12 species in
 * each of Guy's own Red.gb and Crystal.gbc:
 *
 *     Gen 1   2.9 sectors/pic  (worst 5)      -> a 30-cell box of Gen-1 imports ~88
 *     Gen 2   3.8 sectors/pic  (worst 4)      -> a 30-cell box of Gen-2 imports ~113
 *
 * with ONE caveat that is worth knowing before someone measures it and panics:
 * rom_gbsprite.c repairs a wrong stored bank byte lazily, by sweeping every bank until
 * one wins (rom_gbsprite.h's PicsBanks note). The FIRST picture out of a not-yet-repaired
 * bank therefore costs far more -- 134 sectors for the one that repairs Crystal's Unown
 * table -- and the repaired map is then cached in the RomGbSprite, so the warm numbers
 * above are what an actual box flip pays. The one-time cost lands on whichever flip
 * happens to be first, once per registration.
 *
 * That is paid ONCE PER FLIP -- a discrete user action that already reads a 2400-byte
 * box file off the same card -- and NEVER per frame: what stays on screen is a bitmap
 * blit, so the bob, the cursor and the carry animation all run at full speed over it.
 * The alternative (cache the decoded cells) is refused by arithmetic: 30 x 24x22 RGB15
 * is 31 KB against ~1.5 KB of EWRAM headroom, and OBJ tile VRAM is already fully
 * allocated (box_oam.h:20-24). A normal save pays ZERO of this: art_wanted() is a cache
 * lookup, so a box with no GB imports never touches the card at all -- asserted, not
 * hoped, by tests/host_originart_test.c part F.
 */
#define PDNA_ORIGIN_BOX 30

/* Recompute the 30-cell cache from an ALREADY DECODED box. This is the cheap door --
 * pdna_box.c decodes all 30 records for the grid anyway, so calling it there adds only
 * the detection itself. */
void pdna_origin_box_note(const PkMon box[PDNA_ORIGIN_BOX]);

/* Recompute the cache from 30 RAW 80-byte box records (2400 bytes), decoding them
 * internally ONE AT A TIME into a single stack PkMon -- never an array of 30, which
 * would put ~4 KB on the 32 KB IWRAM stack (hard rule 2). This is the door for
 * pdna_bank.c's page-in hook, which holds records and not PkMon. */
void pdna_origin_box_note_records(const uint8_t* recs);

/* Forget the cache (every cell reads back as native/no marker). Call when the box the
 * cache describes is no longer on screen. */
void pdna_origin_box_clear(void);

/* Per-cell readback for the grid renderer. `slot` 0..29. */
int      pdna_origin_box_gen(int slot);     /* 1/2/3; 3 (or 0 for an empty slot)     */
char     pdna_origin_box_mark(int slot);    /* '1' / '2' / '?' / 0 = draw no marker  */
uint16_t pdna_origin_box_color(int slot);   /* RGB15 pad tint, 0 = none              */
int      pdna_origin_box_count(uint8_t gen);/* how many cells are from that era      */

/* Is this cell a Game Boy import at all? Cache only, so it is free to ask for all 30
 * cells on every repaint -- which is what makes layer 2 unconditional. */
int pdna_origin_box_gb(int slot);

/* Does this box hold ANY Game Boy import? One cached bit, folded in by
 * pdna_origin_box_note, so the grid can skip its whole per-cell era pass in the case
 * that describes every box of a normal save -- 60 calls across two loops, plus the cell
 * geometry, off every full repaint.
 *
 * Measured, not assumed: skipping it did NOT move the art-free box-flip median under
 * mGBA, so the per-cell pass really was as cheap as the note above claims. It is here
 * because work that provably cannot change a pixel should not be done at repaint
 * frequency on a console reading its code out of PSRAM. */
int pdna_origin_box_any_gb(void);

/* Would this cell be served REAL era art right now -- i.e. is it a GB import AND is a
 * ROM registered for its era? Cache lookup + the source's own have() probe: NO decode,
 * NO card access. The grid asks this before it asks for pixels, so a box with no GB
 * mons (every box in a normal save) costs exactly nothing, and a hidden OBJ icon is
 * never left hidden over a cell that turned out to have no art. */
int pdna_origin_box_art_wanted(int slot);

/* Full era art for ONE cell -- a grid cell, the box screen's left data panel, or the
 * summary. Always exactly one decode. Same degrade ladder as the portrait, so a caller
 * that gets back out->gen == PDNA_GEN3 has been handed the ordinary Gen-3 picture and
 * should keep drawing the cell the ordinary way. */
int pdna_origin_box_art(int slot, const PkMon* m, PdnaArt* out);

/*
 * Scale one era picture into a CELL-SIZED image the UI can blit, in exactly the format
 * ui_sprite() takes: dw*dh RGB15 pixels, row-major, 0 = transparent and 0x8000|RGB15
 * opaque. `dst` is the CALLER'S buffer (24x22 = 1056 B is a stack frame, not a static
 * -- this module owns no pixel memory anywhere, on purpose).
 *
 * The fit preserves the aspect ratio, centres horizontally and anchors at the BOTTOM,
 * the same rule as pdna_origin_art_place -- so a 56x56 Gen-1 pic and a 64x64 Gen-3 one
 * stand on the same floor in neighbouring cells instead of floating at different
 * heights.
 *
 * Downscaling 56x56 into 22x22 is a 2.5x reduction, at which plain point sampling
 * DELETES thin features -- a tail, an ear, Pikachu's bolt -- because the one sampled
 * pixel lands on background. So each destination pixel scans its whole source block and
 * keeps the centre sample when that is opaque, otherwise the first opaque pixel in the
 * block. That biases toward preserving the SILHOUETTE, which is the entire reason a
 * Gen-1 sprite is recognisable at cell size.
 *
 * NOTE THE STRIDE. `a->px` is packed with stride a->w -- NOT the codec's fixed 56 --
 * because it is the RGB15 expansion, not the GbSprite. Reading it with a 56 stride
 * shears every sprite that is not 7x7 tiles, which is most of Gen 1.
 *
 * Returns 1, or 0 for no art / a bad size (leaving `dst` untouched).
 */
int pdna_origin_cell_render(const PdnaArt* a, uint16_t* dst, int dw, int dh);

#endif /* PDNA_ORIGIN_ART_H */
