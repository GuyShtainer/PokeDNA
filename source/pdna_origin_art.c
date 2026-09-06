/*
 * pdna_origin_art.c — origin detection + the art router. See pdna_origin_art.h for
 * the whole rationale, the decomp citations and the honest limits.
 *
 * PURE C: <stdint.h>/<string.h> plus PokeDNA's own pure cores. No tonc, no FatFs, no
 * GBA headers, so tests/host_originart_test.c runs this exact code on the PC.
 *
 * Deliberate reuse: the detector calls gen12_convert.c's OWN gen12_hp_dv /
 * gen12_is_shiny / gen12_nature / gen12_iv_from_dv rather than re-deriving them. If
 * the converter's rules ever change, the detector changes with them — the one thing a
 * re-implementation here could not guarantee.
 */
#include <string.h>

#include "pdna_origin_art.h"
#include "gen12_convert.h"   /* gen12_hp_dv, gen12_is_shiny, gen12_nature, GEN12_IV_LOW_BIT */
#include "mon_front.h"       /* mon_front_for_form, mon_front_egg (weak in art_fallbacks.c) */
#include "mon_back.h"        /* mon_back_for_form                                            */
#include "rom_sprite.h"      /* gen3_ladder's third rung -- see pdna_origin_art_set_romsprite */
#include "artbuf.h"          /* mon_decomp -- the shared 8 KiB decode buffer                 */
#include "data_tables.h"     /* pk_national_no -- the era resolver hook wants NATIONAL dex   */
#include "gb_art_source.h"   /* PDNA_GB_FETCH_NEED -- D4: this rung's own measured need      */

/* ---- constants the signature is written against ---------------------------------
 * Every one of these is the value gen12_convert.c actually writes, or a decomp fact,
 * never a wiki number. */
#define SIG_METLOC     0xFE   /* METLOC_IN_GAME_TRADE; gen12_convert.c:411, and
                               * pokeruby/src/trade.c:4948 for the retail collision  */
#define SIG_BALL       4      /* ITEM_POKE_BALL, pokeruby/include/constants/items.h:10 */
#define SIG_LANG       2      /* English; gen3_edit.c:143 (gen3_build_mon)            */
#define GEN1_FRIENDSHIP 70    /* gen3_edit.c:147 -- what a Gen-1 import always keeps  */
#define GEN1_MAX_MOVE  165    /* STRUGGLE, pokered/constants/move_constants.asm:173-4 */
#define GEN2_MAX_MOVE  251    /* BEAT_UP,  pokecrystal/.../move_constants.asm:259-60  */
#define GEN2_MAX_DEX   251
#define GEN1_MAX_DEX   151
#define MEW_DEX        151
#define UNOWN_DEX      201    /* the one species whose picture depends on a form      */
#define UNOWN_LETTERS  26     /* Gen 2 has A..Z only; Gen 3 adds ! and ?              */

/* The ribbon word's bit 31 is modernFatefulEncounter, and gen12_convert.c:429 SETS IT
 * for an imported Mew so the game will obey it (put_fateful, citing
 * reference/pokeemerald/pokemon.h:169-175). Every other ribbon bit must still be zero.
 * Without this exemption the detector rejected every Mew the converter ever made —
 * three of them in Guy's own Red.sav, measured. */
#define RIBBON_FATEFUL 0x80000000u

/* Era colours, RGB15 (r | g<<5 | b<<10), chosen to read as the hardware they came
 * from and to stay legible over every box wallpaper:
 *   Gen 1 = the DMG's olive-green LCD;  Gen 2 = a Game Boy Color berry/violet;
 *   unproven = a neutral Game Boy grey that is neither of them, so an unproven era is
 *   never BRANDED as one. */
#define COL_GEN1  ((uint16_t)(11u | (21u << 5) | (9u  << 10)))   /* 0x25CB */
#define COL_GEN2  ((uint16_t)(21u | (10u << 5) | (25u << 10)))   /* 0x6555 */
#define COL_GBQ   ((uint16_t)(17u | (17u << 5) | (17u << 10)))   /* 0x4631 */

/* ---- (1) ORIGIN DETECTION -------------------------------------------------------- */

/* The ELEVEN Gen-2 species a GEN-1 species can still become inside a Gen-3 game — the
 * Kanto-preevolution rows of source/gen3_legality_hooks.c's s_preevo table:
 *   169 Crobat<-Golbat   182 Bellossom<-Gloom     186 Politoed<-Poliwhirl
 *   196 Espeon<-Eevee    197 Umbreon<-Eevee       199 Slowking<-Slowpoke
 *   208 Steelix<-Onix    212 Scizor<-Scyther      230 Kingdra<-Seadra
 *   233 Porygon2<-Porygon                         242 Blissey<-Chansey
 * Bellossom needs only a Sun Stone and the five trade evolutions change no EXP at all,
 * so a Gen-1 IMPORT reaches those ids with its whole signature intact — a Johto number
 * on one of them is therefore evidence, not proof. (The four friendship evolutions need
 * a level-up, which moves the EXP and breaks the nature clause 24 times in 25; they are
 * listed anyway because "usually rejected" is not a reason to claim certainty.) */
static const uint8_t KANTO_EVO_JOHTO[11] = {
  169, 182, 186, 196, 197, 199, 208, 212, 230, 233, 242
};
static int johto_from_kanto(uint16_t dex) {
  for (unsigned i = 0; i < sizeof KANTO_EVO_JOHTO / sizeof KANTO_EVO_JOHTO[0]; i++)
    if (KANTO_EVO_JOHTO[i] == dex) return 1;
  return 0;
}

/* Recover the four Game Boy DVs from Gen-3 IVs. Only meaningful once the even-IV
 * clause has passed; the caller checks that first. */
static void dvs_from_ivs(const PkMon* m, uint8_t dv[4]) {
  dv[0] = (uint8_t)(m->ivs[PK_ATK] / 2u);   /* Atk */
  dv[1] = (uint8_t)(m->ivs[PK_DEF] / 2u);   /* Def */
  dv[2] = (uint8_t)(m->ivs[PK_SPE] / 2u);   /* Spd -- Gen 1/2 call this Speed        */
  dv[3] = (uint8_t)(m->ivs[PK_SPA] / 2u);   /* Spc -- ONE Special DV, see below      */
}

/* A record that could hold a Game Boy Pokemon at all: a real, non-egg species in the
 * range the converter emits. Split out because pdna_origin_of_hint needs exactly this
 * much of the signature — a hint is authority about the ERA, never about whether the
 * slot holds a Pokemon. */
static int species_could_be_gb(const PkMon* m) {
  if (!m || m->species == 0 || m->species > GEN2_MAX_DEX) return 0;
  if (m->isEgg || m->isBadEgg) return 0;
  return 1;
}

/* Every clause of the import fingerprint, in the order that fails fastest and reports
 * best. Returns PDNA_SIG_OK when all of them hold. */
static uint8_t signature_miss(const PkMon* m, uint8_t dv[4]) {
  /* Species. The converter refuses eggs (GB12_ERR_EGG) and only ever emits national
   * 1..251, where the Gen-3 INTERNAL index and the National Dex number are the same
   * number (gen12_convert.c:304-307). So an internal id above 251 cannot be one. */
  if (!species_could_be_gb(m)) return PDNA_SIG_SPECIES;

  /* The four stamped fields (gen12_convert.c:336-338, 411-414). Individually weak --
   * a retail in-game trade has all four -- but free to test and they cut the
   * population down to something the IV lattice can then settle. */
  if ((m->otId >> 16) != 0u)          return PDNA_SIG_SID;
  if (m->metLocation != SIG_METLOC)   return PDNA_SIG_METLOC;
  if (m->pokeball    != SIG_BALL)     return PDNA_SIG_BALL;
  if (m->language    != SIG_LANG)     return PDNA_SIG_LANG;

  /* THE DV LATTICE — this is the clause that actually does the work.
   *
   * IV = DV * 2 + GEN12_IV_LOW_BIT, so with the low bit defined as 0 every IV a GB
   * import can hold is EVEN and at most 30. */
  for (int i = 0; i < PK_NSTATS; i++)
    if ((m->ivs[i] & 1u) != (unsigned)GEN12_IV_LOW_BIT || m->ivs[i] > 30u)
      return PDNA_SIG_IV_ODD;

  /* Gen 1/2 have ONE Special DV where Gen 3 has two stats, and gen12_convert.c:373-374
   * feeds it to both. Two different Specials therefore cannot have come from a GB
   * record. (1 chance in 32 for a native mon that got this far.) */
  if (m->ivs[PK_SPA] != m->ivs[PK_SPD]) return PDNA_SIG_IV_SPLIT;

  dvs_from_ivs(m, dv);

  /* Gen 1/2 store no HP DV at all: it IS the other four DVs' low bits (gen12_hp_dv).
   * So the HP IV is not free — it is a function of the other four, 1 chance in 16. */
  if (m->ivs[PK_HP] != gen12_iv_from_dv(gen12_hp_dv(dv[0], dv[1], dv[2], dv[3])))
    return PDNA_SIG_IV_HP;

  /* "Stat experience is completely erased" — the converter leaves every EV, contest
   * condition and ribbon at zero (gen12_convert.c:381-385). A retail in-game trade
   * does NOT: it sets COOL/BEAUTY/CUTE/SMART/TOUGH and SHEEN from its table
   * (pokeruby/src/trade.c:4962-4968), which is a second independent way this clause
   * catches the one native population that shares the stamped fields.
   * The single exception is the fateful bit the converter itself sets on a Mew. */
  for (int i = 0; i < PK_NSTATS; i++) if (m->evs[i]) return PDNA_SIG_EV;
  for (int i = 0; i < 6; i++)         if (m->contest[i]) return PDNA_SIG_CONTEST;
  {
    uint32_t ribbons = m->ribbons;
    if (m->species == MEW_DEX) ribbons &= ~RIBBON_FATEFUL;
    if (ribbons) return PDNA_SIG_CONTEST;
  }

  /* Transporter's nature rule, applied to the EXP that actually shipped, so
   * "nature == stored EXP % 25" is an exact invariant of every record the converter
   * makes (gen12_convert.c:325-329 + pid_matches at :190). 1 chance in 25. */
  if (m->nature != gen12_nature(m->experience)) return PDNA_SIG_NATURE;

  /* Shininess is a HARD constraint in every solve_pid() call (gen12_convert.c:342-350
   * never relaxes want_shiny), so the Gen-3 shiny flag must agree with the Gen-2 DV
   * rule exactly — in both directions. */
  if ((m->isShiny ? 1 : 0) != (gen12_is_shiny(dv[0], dv[1], dv[2], dv[3]) ? 1 : 0))
    return PDNA_SIG_SHINY;

  return PDNA_SIG_OK;
}

/* Which GB generation, given that the record IS an import.
 *
 * The split between PROOF and EVIDENCE is the whole contract (pdna_origin_art.h):
 * *certain is set ONLY by a signal ordinary Gen-3 play cannot produce. The weak tells
 * are still collected — they explain the '?' to a log or a screen, and they are allowed
 * to pick the most likely PICTURE — but they never grant a label. */
static uint8_t which_gb_gen(const PkMon* m, uint8_t* tells, uint8_t* certain) {
  uint8_t t = 0;

  /* --- PROOF ---------------------------------------------------------------------
   * A species Gen 1 never had. Gen 3 changes a species only by evolving it, so a Johto
   * id that no Kanto species evolves into cannot have grown here. */
  if (m->species > GEN1_MAX_DEX)
    t |= johto_from_kanto(m->species) ? PDNA_TELL_EVOJOHTO : PDNA_TELL_JOHTO;
  /* Origins bit 15, written once at creation and never rewritten by Gen 3;
   * gen12_convert.c:415 only sets it for a Gen-2 record with Crystal caught data. */
  if (m->otGender) t |= PDNA_TELL_OTFEM;

  /* --- EVIDENCE ONLY -------------------------------------------------------------
   * Every one of these can appear on a genuine GEN-1 import after ordinary Gen-3 play:
   * a TM/HM teaches a Gen-2-numbered move (Rock Smash is 249), Pokerus spreads through
   * the party without the carrier ever battling, and friendship moves every 128 steps.
   * The first version of this file called them proof; over Guy's Red.sav that mislabeled
   * all 238 detectable Gen-1 imports as a confident "GB2". */
  for (int i = 0; i < 4; i++)
    if (m->moves[i] > GEN1_MAX_MOVE && m->moves[i] <= GEN2_MAX_MOVE) t |= PDNA_TELL_MOVE;
  if (m->pokerus)                       t |= PDNA_TELL_POKERUS;
  if (m->friendship != GEN1_FRIENDSHIP) t |= PDNA_TELL_FRIEND;

  *tells = t;
  if (t & PDNA_TELL_PROOF) { *certain = 1; return PDNA_GEN2; }

  /* Unproven. The PICTURE still has to be one of them: a Johto species simply has no
   * Gen-1 art to draw, and everything else gets Gen 1 — the era a Gen-1 import always
   * lands on, rendered in the DMG greys, which is the neutral Game Boy look. */
  *certain = 0;
  return (t & PDNA_TELL_EVOJOHTO) ? (uint8_t)PDNA_GEN2 : (uint8_t)PDNA_GEN1;
}

void pdna_origin_of(const PkMon* m, PdnaOrigin* out) {
  if (!out) return;
  memset(out, 0, sizeof *out);
  out->gen = PDNA_GEN3;
  out->gen_certain = 1;
  if (!m) return;

  uint8_t dv[4] = { 0, 0, 0, 0 };
  uint8_t miss = signature_miss(m, dv);
  if (miss != PDNA_SIG_OK) { out->miss = miss; return; }

  out->verdict = PDNA_ORIGIN_GB;
  memcpy(out->dv, dv, sizeof dv);
  out->gen = which_gb_gen(m, &out->tells, &out->gen_certain);
}

void pdna_origin_of_hint(const PkMon* m, uint8_t hint_gen, PdnaOrigin* out) {
  if (!out) return;
  /* Start from the record, always. The hint is authority about the ERA, never about
   * whether this cell holds a Pokemon at all — pdna_gen12.c fills a 30-slot grid from
   * a 20-slot GB box, so most of the cells it hands us are EMPTY, and it draws eggs
   * and damaged records as Gen-3 stand-ins (gb_build_slot's GB_SHOW_RELAXED /
   * GB_SHOW_PLACEHOLDER). Claiming "Gen 2 import" for those is a lie the caller never
   * asked us to tell. */
  pdna_origin_of(m, out);
  if (hint_gen != PDNA_GEN1 && hint_gen != PDNA_GEN2) return;
  if (!species_could_be_gb(m)) return;

  out->gen = hint_gen;
  out->verdict = PDNA_ORIGIN_GB;
  out->gen_certain = 1;
  out->tells = PDNA_TELL_HINT;
  out->miss = PDNA_SIG_OK;
  /* pdna_origin_of already recovered the DVs when the full signature held; a hinted
   * record that misses a later clause (a placeholder) simply has none to report. */
}

const char* pdna_origin_tag(const PdnaOrigin* o) {
  if (!o || o->verdict != PDNA_ORIGIN_GB) return "";
  if (!o->gen_certain) return "GB?";
  return (o->gen == PDNA_GEN2) ? "GB2" : "GB1";
}

const char* pdna_origin_text(const PdnaOrigin* o) {
  if (!o || o->verdict != PDNA_ORIGIN_GB) return "Gen 3 native";
  if (!o->gen_certain) return "GB import: Gen 1 or 2";
  return (o->gen == PDNA_GEN2) ? "Gen 2 import (converted)"
                               : "Gen 1 import (converted)";
}

char pdna_origin_mark(const PdnaOrigin* o) {
  if (!o || o->verdict != PDNA_ORIGIN_GB) return 0;
  if (!o->gen_certain) return '?';
  return (o->gen == PDNA_GEN2) ? '2' : '1';
}

uint16_t pdna_origin_color(const PdnaOrigin* o) {
  if (!o || o->verdict != PDNA_ORIGIN_GB) return 0;
  if (!o->gen_certain) return COL_GBQ;
  return (o->gen == PDNA_GEN2) ? COL_GEN2 : COL_GEN1;
}

/* ---- (2) THE ART ROUTER ---------------------------------------------------------- */

/* ~16 bytes of plain .bss. Nothing here lands in EWRAM: the hardware build has ~1.5 KB
 * of EWRAM free and a post-link guard that rejects an overflow, so this module adds no
 * EWRAM_BSS at all — the pixel buffers belong to whoever registers the source. */
static PdnaGbArtSource s_gb;
static int s_gb_on = 0;

/* The LAST picture fetched from the source, so a repeat request costs nothing.
 * NOT a pixel cache — the pixels stay in the source's own buffer, whose published
 * contract is "valid until the NEXT pic() call". Every pic() call in the program goes
 * through fetch_pic() below, so a memo hit proves the source's buffer still holds
 * exactly these pixels. 12 more bytes of plain .bss, no EWRAM. */
static const uint16_t* s_memo_px;
static uint16_t s_memo_dex;
static uint8_t  s_memo_on, s_memo_gen, s_memo_form, s_memo_back, s_memo_shiny,
                s_memo_w, s_memo_h;
/* E5: an icon() fetch and a pic() fetch for the SAME (gen,dex) are two DIFFERENT
 * pictures (16x16 vs up to 56x56) that can legitimately alternate on the same
 * mon within one screen visit (a box grid cell asks for the icon, its hover
 * panel or the summary asks for the portrait) -- so the memo key must say which
 * kind of picture it is holding, or a box-cell HIT would hand a 16x16 buffer to
 * a caller expecting up to 56x56 (and vice-versa), reading past what was
 * actually decoded. 1 more byte of plain .bss, no EWRAM. */
static uint8_t  s_memo_icon;
/* E3 review BLOCKING 2: the memo's key alone is not enough -- mon_decomp is shared
 * with every OTHER decoder in the app (artbuf.h's artbuf_claim() list), so a memo
 * hit on (gen,dex,form,back,shiny) can no longer prove the buffer still holds THESE
 * pixels once something else has decoded into it in between. Stamping the shared
 * epoch alongside the memo and rejecting a hit whose epoch has moved on turns that
 * into an ordinary, correct miss -- 4 more bytes of plain .bss, no EWRAM. */
static uint32_t s_memo_epoch;

static void memo_clear(void) { s_memo_on = 0; s_memo_px = 0; }

void pdna_origin_art_register(const PdnaGbArtSource* src) {
  memo_clear();   /* a new source means a new buffer; the old pointer is meaningless */
  if (src && src->pic) { s_gb = *src; s_gb_on = 1; }
  else { memset(&s_gb, 0, sizeof s_gb); s_gb_on = 0; }
}

void pdna_origin_art_invalidate(void) { memo_clear(); }

int pdna_origin_art_have(uint8_t gen) {
  if (!s_gb_on) return 0;
  if (gen != PDNA_GEN1 && gen != PDNA_GEN2) return 0;
  return s_gb.have ? (s_gb.have(s_gb.ctx, gen) ? 1 : 0) : 1;
}

/* E3 review re-verification: the stack-headroom gate. Plain .bss, one function
 * pointer, no EWRAM. NULL (never registered -- the host build, and any GBA build
 * that boots before gb_art_source.c's registration runs) means "always room": this
 * function must never ITSELF be the reason art that used to show stops showing. */
static PdnaStackRoomFn s_stack_room_fn = 0;

void pdna_origin_art_set_stack_room_hook(PdnaStackRoomFn fn) { s_stack_room_fn = fn; }

int pdna_origin_art_stack_room(int need) { return s_stack_room_fn ? s_stack_room_fn(need) : 1; }

/* ---- (3b) PLACE-AWARE ROUTING (E4) ------------------------------------------------
 * Plain .bss, no EWRAM: a screen-scoped int and a function pointer. See the header
 * for why this module never includes sprite_era.h or calls se_resolve() itself. */
#define PDNA_PLACE_DEFAULT 2   /* sprite_era.h's SE_PLACE_SUMMARY, mirrored as a
                                * literal so this file need not include that header
                                * just for one default value -- se_resolve() itself
                                * defends the same out-of-range case the same way. */
static int s_place = PDNA_PLACE_DEFAULT;
static PdnaEraResolverFn s_era_resolver = 0;

void pdna_origin_art_set_place(int place) { s_place = place; }
int pdna_origin_art_get_place(void) { return s_place; }
void pdna_origin_art_set_era_resolver(PdnaEraResolverFn fn) { s_era_resolver = fn; }

/* sprite_era.h's SeEra values, mirrored as literals for the same reason as
 * PDNA_PLACE_DEFAULT above -- the resolver hook is typed `int` on purpose so this
 * file has nothing to keep in sync but these six numbers, which sprite_era.h's own
 * enum ordering is _Static_assert-pinned against by sprite_era.c and every host test
 * that includes both headers. */
enum {
  ERA_NATIVE = 0, ERA_GEN1, ERA_GEN2, ERA_G3_RS, ERA_G3_EM, ERA_G3_FRLG
};

/* ---- (3c) THE CROSS-GAME GEN-3 RUNG (E4) ------------------------------------------ */
static PdnaG3CrossSource s_g3x;
static int               s_g3x_on = 0;

void pdna_origin_art_set_g3cross(const PdnaG3CrossSource* src) {
  if (src && src->pic) { s_g3x = *src; s_g3x_on = 1; }
  else { memset(&s_g3x, 0, sizeof s_g3x); s_g3x_on = 0; }
}

/* One picture, memoised on the WHOLE request (including `want_back`, so the
 * back-then-front fallback is remembered as one answer and a summary showing the back
 * sprite does not re-run the failed back fetch every frame).
 *
 * E5: `icon` selects the 16x16 party/PC menu-icon rung (source->icon(), gen==GEN2
 * only, no form/back/shiny -- Gen-2 menu icons are one picture per species
 * regardless of any of those) instead of the ordinary up-to-56x56 pic() rung.
 * Folded into the SAME memo/epoch machinery as the portrait fetch (a box cell's
 * icon request and the next mon's portrait request still take turns through one
 * shared last-fetch slot) but keyed additionally on `icon`, so the two kinds of
 * picture can never satisfy each other's cache hit -- see s_memo_icon's comment. */
static const uint16_t* fetch_pic_ex(uint8_t gen, uint16_t dex, uint8_t form,
                                    uint8_t want_back, uint8_t shiny, uint8_t icon,
                                    uint8_t* out_w, uint8_t* out_h) {
  if (s_memo_on && s_memo_epoch == artbuf_epoch && s_memo_gen == gen &&
      s_memo_dex == dex && s_memo_form == form && s_memo_icon == icon &&
      s_memo_back == want_back && s_memo_shiny == shiny) {
    *out_w = s_memo_w; *out_h = s_memo_h;
    return s_memo_px;
  }
  /* Drop the memo BEFORE calling: a fetch that fails may still have written into the
   * source's buffer, so the old pointer stops being trustworthy the moment we ask. */
  memo_clear();
  uint8_t w = 0, h = 0;
  const uint16_t* px = 0;
  if (icon) {
    if (s_gb.icon) px = s_gb.icon(s_gb.ctx, gen, dex, &w, &h);
  } else {
    if (want_back) px = s_gb.pic(s_gb.ctx, gen, dex, form, 1, shiny, &w, &h);
    if (!px)       px = s_gb.pic(s_gb.ctx, gen, dex, form, 0, shiny, &w, &h);
  }
  if (!px || !w || !h) return 0;
  s_memo_on = 1; s_memo_gen = gen; s_memo_dex = dex; s_memo_form = form;
  s_memo_back = want_back; s_memo_shiny = shiny; s_memo_icon = icon;
  s_memo_px = px; s_memo_w = w; s_memo_h = h;
  s_memo_epoch = artbuf_epoch;    /* the source's pic()/icon() call above may itself
                                   * have bumped this (gb_art_source.c does, before its
                                   * own decode) -- capture it AFTER the call, not before */
  *out_w = w; *out_h = h;
  return px;
}

/* The ordinary portrait rung -- unchanged call shape for every pre-E5 caller. */
static const uint16_t* fetch_pic(uint8_t gen, uint16_t dex, uint8_t form,
                                 uint8_t want_back, uint8_t shiny,
                                 uint8_t* out_w, uint8_t* out_h) {
  return fetch_pic_ex(gen, dex, form, want_back, shiny, 0, out_w, out_h);
}

/* ---- (3) THE GEN-3 ROM RUNG -------------------------------------------------------
 * See the contract in pdna_origin_art.h. `rs` is COPIED (a small POD, no pointers back
 * into it), exactly like PdnaGbArtSource above. No EWRAM here either: mon_decomp is
 * artbuf.c's, not this module's. */
static RomSprite s_romsprite;
static int       s_romsprite_on = 0;

void pdna_origin_art_set_romsprite(const RomSprite* rs) {
  if (rs && rs->ok) { s_romsprite = *rs; s_romsprite_on = 1; }
  else { memset(&s_romsprite, 0, sizeof s_romsprite); s_romsprite_on = 0; }
}

/* Decode straight into mon_decomp and expand it to RGB15 IN PLACE
 * (rom_sprite_to_rgb15) -- after which it is exactly the pointer the compiled path
 * used to return, so gen3_ladder's caller needs no change. Returns NULL (mon_decomp's
 * contents undefined) on any failure: no ROM registered, wrong-game ROM, a species/
 * form this cart cannot show, or a read that failed verification -- every one of
 * those must degrade to "no art" here exactly like an absent compiled sprite does.
 *
 * DELIBERATELY NOT MEMOISED, unlike fetch_pic() above. mon_decomp is shared with
 * item icons and type badges (app_item_icon/app_type_badge, pdna_main.c), and
 * pdna_summary.c's draw_left draws its type badges AFTER fetching (and blitting) the
 * portrait but BEFORE the summary's animation setup fetches it a SECOND time for the
 * same mon (portrait_sprite(), pdna_summary.c:587) -- see the "Fetch the portrait
 * ONCE per repaint" comment at pdna_summary.c:748-763, which already documents this
 * exact hazard: "If a future caller ... starts decoding inside this loop, it must
 * re-fetch here." A memo keyed on (species, form, back, shiny) would have handed
 * that second fetch a STALE pointer -- same key, but mon_decomp's tail already
 * overwritten by the type-badge decode in between. Always redecoding costs one
 * extra 8 KB LZ77 pass per repaint (the SAME cost the compiled mon_front_for_form/
 * mon_back_for_form already pay today, since neither of them memoises either), and
 * buys correctness instead of a plausible-looking corrupted portrait. */
static const uint16_t* rom_portrait(const PkMon* m, int back) {
  if (!s_romsprite_on || !m) return 0;
  artbuf_claim();          /* E3 review BLOCKING 2: about to overwrite mon_decomp */
  RomSpritePic pic;
  RomSpriteSide side = back ? ROM_SPRITE_BACK : ROM_SPRITE_FRONT;
  if (!rom_sprite_pic(&s_romsprite, side, m->species, m->form,
                      (uint8_t*)mon_decomp, MON_DECOMP_BYTES, &pic))
    return 0;
  uint16_t pal[16];
  if (!rom_sprite_pal(&s_romsprite, m->species, m->form, m->isShiny ? 1 : 0, pal)) return 0;
  if (!rom_sprite_to_rgb15(mon_decomp, MON_DECOMP_BYTES, pic.frame, pal)) return 0;
  return mon_decomp;
}

/* The Gen-3 ladder, character for character what pdna_summary.c's draw_left did before
 * this module existed, PLUS the ROM rung (Phase 1): compiled art wins when present,
 * a registered ROM lights up an otherwise-empty artless summary, and the procedural
 * fallback (art_fallbacks.c's weak NULLs) still has the last word when neither has an
 * answer. Kept in one place so "nothing registered => no visible change" is a
 * property of the code and not of two files agreeing. */
static void gen3_ladder(const PkMon* m, int back, PdnaArt* out) {
  const uint16_t* spr = back ? mon_back_for_form(m->species, m->isShiny, m->form) : 0;
  if (!spr) spr = mon_front_for_form(m->species, m->isShiny, m->form);
  if (!spr) spr = rom_portrait(m, back);
  out->px = spr;
  out->w = MON_FRONT_W; out->h = MON_FRONT_H;
  out->gen = PDNA_GEN3;
}

int pdna_origin_art_portrait(const PkMon* m, int back, PdnaArt* out, PdnaOrigin* origin) {
  if (!out) return 0;
  memset(out, 0, sizeof *out);
  out->w = MON_FRONT_W; out->h = MON_FRONT_H;
  out->gen = out->era = PDNA_GEN3;
  out->era_certain = 1;

  PdnaOrigin local;
  PdnaOrigin* o = origin ? origin : &local;
  pdna_origin_of(m, o);
  if (!m) return 0;

  /* An Egg reads as an Egg in every era: Gen 1 has no egg at all and the converter
   * refuses Gen-2 eggs outright (GB12_ERR_EGG), so there is no "Gen-2 egg sprite" this
   * could honestly serve. Keep the Gen-3 Egg and say so through out->egg. */
  if (m->isEgg && !m->isBadEgg) {
    out->egg = 1;
    out->px = mon_front_egg();
    return out->px != 0;
  }

  out->era = o->gen;
  out->era_certain = o->gen_certain;

  /* E4: ask the place-aware resolver hook what era THIS screen's setting wants for
   * this record. No resolver registered (the host build's other tests, or a screen
   * reached before pdna_main.c's boot registration runs) => ERA_NATIVE => every line
   * below behaves EXACTLY as it did before this slice existed -- the branch is a
   * structural no-op in that case, not a re-derivation of the old logic. */
  int era = ERA_NATIVE;
  if (s_era_resolver) {
    uint16_t nat_dex = pk_national_no(m->species);
    era = s_era_resolver(s_place, o->gen, o->gen_certain, nat_dex, 0);
  }

  if (era == ERA_GEN1 || era == ERA_GEN2) {
    /* The resolver already ran se_resolve()'s own gates (species-exists, ROM
     * registered, the PC-grid Gen-1-icon refusal) -- the only thing left for THIS
     * module to check is the stack-room gate, which is a live hardware fact
     * se_resolve() has no way to know about (sprite_era.c is pure and stack-agnostic
     * by design). Same have()/stack-room pair the NATIVE branch below always used. */
    uint8_t want_gen = (era == ERA_GEN2) ? (uint8_t)PDNA_GEN2 : (uint8_t)PDNA_GEN1;
    if (s_gb_on && s_gb.pic && m->species >= 1 && m->species <= GEN2_MAX_DEX &&
        pdna_origin_art_have(want_gen) && pdna_origin_art_stack_room(PDNA_GB_FETCH_NEED)) {
      uint8_t form = 0;
      if (m->species == UNOWN_DEX && m->form < UNOWN_LETTERS) form = m->form;
      uint8_t w = 0, h = 0;
      const uint16_t* px = fetch_pic(want_gen, m->species, form,
                                     back ? 1u : 0u, m->isShiny ? 1u : 0u, &w, &h);
      if (px && w && h) { out->px = px; out->w = w; out->h = h; out->gen = want_gen; return 1; }
    }
    /* Refused or could not serve it -- fall through to gen3_ladder below, same as
     * every other rung's failure. */
  } else if (era == ERA_G3_RS || era == ERA_G3_EM || era == ERA_G3_FRLG) {
    /* D4: this rung's own need, not the GB fetch's 6,144 B -- the cross-game
     * subtree measures ~2,480 B (PDNA_G3X_FETCH_NEED's own comment), so a chain
     * that could not clear the GB gate may still safely clear this smaller one. */
    if (s_g3x_on && s_g3x.pic && pdna_origin_art_stack_room(PDNA_G3X_FETCH_NEED)) {
      int game = (era == ERA_G3_RS) ? 0 : (era == ERA_G3_EM) ? 1 : 2;  /* PkGame */
      uint8_t w = 0, h = 0;
      const uint16_t* px = s_g3x.pic(s_g3x.ctx, game, m->species, m->form,
                                     back ? 1u : 0u, m->isShiny ? 1u : 0u, &w, &h);
      if (px && w && h) {
        out->px = px; out->w = w; out->h = h; out->gen = PDNA_GEN3;
        out->game = (uint8_t)(game + 1);
        return 1;
      }
    }
    /* No cross-game source, or it could not serve this species/form/game -- fall
     * through to gen3_ladder, exactly like every other rung's failure. */
  } else if (o->verdict == PDNA_ORIGIN_GB && s_gb_on && s_gb.pic &&
      m->species >= 1 && m->species <= GEN2_MAX_DEX &&
      pdna_origin_art_have(o->gen) && pdna_origin_art_stack_room(PDNA_GB_FETCH_NEED)) {
    /* era == ERA_NATIVE: the pre-E4 behaviour, byte for byte. Internal id ==
     * national dex for 1..251 (gen12_convert.c:304-307), so no map. The Unown LETTER
     * is a real part of the picture and the converter went out of its way to
     * preserve it (solve_pid's want_letter), so it has to reach the source: dropping
     * it here would draw a Gen-2 Unown A for every letter. Gen-3-only forms (! and ?,
     * 26/27) have no Gen-2 picture, so they fall back to A. */
    uint8_t form = 0;
    if (m->species == UNOWN_DEX && m->form < UNOWN_LETTERS) form = m->form;
    uint8_t w = 0, h = 0;
    const uint16_t* px = fetch_pic(o->gen, m->species, form,
                                   back ? 1u : 0u, m->isShiny ? 1u : 0u, &w, &h);
    if (px && w && h) {
      out->px = px; out->w = w; out->h = h; out->gen = o->gen;
      return 1;
    }
    /* The era's ROM is registered but could not serve this species: fall through to
     * Gen-3 art rather than showing nothing. out->era still says where it came from,
     * so the tag keeps telling the truth even though the pixels do not. */
  }

  gen3_ladder(m, back, out);
  return out->px != 0;
}

void pdna_origin_art_place(const PdnaArt* a, int x, int y, int w, int h,
                           int* out_x, int* out_y) {
  int aw = a ? (int)a->w : 0, ah = a ? (int)a->h : 0;
  int ox = x + (w - aw) / 2;      /* centred horizontally */
  int oy = y + (h - ah);          /* feet on the floor    */
  if (ox < x) ox = x;
  if (oy < y) oy = y;
  if (out_x) *out_x = ox;
  if (out_y) *out_y = oy;
}

/* ---- (3) THE BANK IN PARALLEL ---------------------------------------------------- */

/* 30 bytes + 1 int of plain .bss. Packed so the whole cache is one cache line's worth
 * of state and can be rebuilt from scratch on every box flip.
 *   bits 0-1 : gen (0 = empty slot, 1/2/3)
 *   bit  2   : gen_certain
 *   bit  3   : this cell is a GB import
 *   bit  4   : E5 -- the era RESOLVER says this cell draws in GEN2 at the
 *              CURRENT place (pdna_origin_box_art will therefore try the
 *              16x16 menu-icon rung for it). Independent of bit 3: set for an
 *              ordinary GB import whose native/overridden era is GEN2 (bit 3
 *              usually set too), AND for a NATIVE Gen-3 mon whose cell the
 *              user overrode to GEN2 in Settings (bit 3 stays clear -- a
 *              costume is not provenance, so this bit must never feed the
 *              '1'/'2' import marker). */
#define CELL_GEN   0x03u
#define CELL_CERT  0x04u
#define CELL_GB    0x08u
#define CELL_ERA_GB2 0x10u

static uint8_t s_cell[PDNA_ORIGIN_BOX];
static int     s_cells_valid = 0;
/* Folded in as the cache is filled: does ANY of the 30 cells carry CELL_GB? The grid's
 * whole era pass is a no-op when this is 0, and that is the case for every box of every
 * save with no GB imports -- i.e. the common one. See pdna_origin_box_any_gb(). */
static int     s_cells_any_gb = 0;

static uint8_t cell_pack(const PkMon* m) {
  if (!m || m->species == 0) return 0;
  PdnaOrigin o;
  pdna_origin_of(m, &o);
  uint8_t v = (uint8_t)(o.gen & CELL_GEN);
  if (o.gen_certain) v |= CELL_CERT;
  if (o.verdict == PDNA_ORIGIN_GB) v |= CELL_GB;
  /* E5: ask the SAME resolver hook pdna_origin_box_art() will ask, at the SAME
   * place -- box_decode() (pdna_box.c) is the only caller of pdna_origin_box_note/
   * _records, and pdna_box() has already called pdna_origin_art_set_place() before
   * ever reaching it, so s_place is already correct for whichever grid (PC/BANK/
   * GBGRID) is on screen. No resolver registered => era stays ERA_NATIVE => this
   * bit is never set => zero change from pre-E5 behaviour, same convention as
   * every other era_resolver_cb call site in this file. */
  if (s_era_resolver) {
    uint16_t nat_dex = pk_national_no(m->species);
    if (s_era_resolver(s_place, o.gen, o.gen_certain, nat_dex, 0) == ERA_GEN2)
      v |= CELL_ERA_GB2;
  }
  return v;
}

/* The one place s_cells_any_gb is derived, so it cannot drift from s_cell[]. */
static void cells_finish(void) {
  uint8_t any = 0;
  for (int i = 0; i < PDNA_ORIGIN_BOX; i++) any |= s_cell[i];
  s_cells_any_gb = (any & (CELL_GB | CELL_ERA_GB2)) ? 1 : 0;
  s_cells_valid = 1;
}

void pdna_origin_box_note(const PkMon box[PDNA_ORIGIN_BOX]) {
  if (!box) { pdna_origin_box_clear(); return; }
  for (int i = 0; i < PDNA_ORIGIN_BOX; i++) s_cell[i] = cell_pack(&box[i]);
  cells_finish();
}

void pdna_origin_box_note_records(const uint8_t* recs) {
  if (!recs) { pdna_origin_box_clear(); return; }
  for (int i = 0; i < PDNA_ORIGIN_BOX; i++) {
    PkMon m;                                   /* ONE PkMon on the stack, never 30 */
    if (!pk_decode_mon(recs + (unsigned)i * 80u, false, &m)) { s_cell[i] = 0; continue; }
    s_cell[i] = cell_pack(&m);
  }
  cells_finish();
}

void pdna_origin_box_clear(void) {
  memset(s_cell, 0, sizeof s_cell);
  s_cells_valid = 0;
  s_cells_any_gb = 0;
}

static uint8_t cell_at(int slot) {
  if (!s_cells_valid || slot < 0 || slot >= PDNA_ORIGIN_BOX) return 0;
  return s_cell[slot];
}

int pdna_origin_box_gen(int slot) { return (int)(cell_at(slot) & CELL_GEN); }

char pdna_origin_box_mark(int slot) {
  uint8_t v = cell_at(slot);
  if (!(v & CELL_GB)) return 0;
  if (!(v & CELL_CERT)) return '?';
  return ((v & CELL_GEN) == PDNA_GEN2) ? '2' : '1';
}

uint16_t pdna_origin_box_color(int slot) {
  uint8_t v = cell_at(slot);
  if (!(v & CELL_GB)) return 0;
  if (!(v & CELL_CERT)) return COL_GBQ;
  return ((v & CELL_GEN) == PDNA_GEN2) ? COL_GEN2 : COL_GEN1;
}

int pdna_origin_box_count(uint8_t gen) {
  int n = 0;
  for (int i = 0; i < PDNA_ORIGIN_BOX; i++) {
    uint8_t v = cell_at(i);
    if ((v & CELL_GEN) == (gen & CELL_GEN) && (v & CELL_GEN) != 0) n++;
  }
  return n;
}

/* E5: also true for a cell whose resolver-derived era is GEN2 even when it is
 * NOT a GB import (a native Gen-3 mon wearing a Settings-overridden Gen-2
 * icon) -- this is the gate era_cell_draw() checks before doing ANY work for a
 * slot, so a native-GEN2 cell must not be skipped here the way an ordinary
 * native cell (CELL_GB clear, CELL_ERA_GB2 clear) still is. */
int pdna_origin_box_gb(int slot) { return (cell_at(slot) & (CELL_GB | CELL_ERA_GB2)) ? 1 : 0; }

int pdna_origin_box_any_gb(void) { return s_cells_valid ? s_cells_any_gb : 0; }

/* The cheap door the grid opens 30 times per repaint. Everything here is either the
 * packed cache byte or the source's own have() probe -- deliberately NO decode and no
 * card access, because this is what decides whether a cell's OBJ icon gets hidden. Ask
 * it first and a box with no GB imports (every box in a normal save) costs one pass
 * over 30 bytes. */
int pdna_origin_box_art_wanted(int slot) {
  uint8_t v = cell_at(slot);
  /* E5: a GEN2-resolved cell (import or native-overridden alike) wants the
   * icon rung whenever a Gen-2 ROM is registered, regardless of CELL_GB --
   * checked FIRST so a native-GEN2 cell (CELL_GB clear) is not short-circuited
   * by the plain-GB check below. */
  if (v & CELL_ERA_GB2) return pdna_origin_art_have(PDNA_GEN2);
  if (!(v & CELL_GB)) return 0;
  return pdna_origin_art_have((uint8_t)(v & CELL_GEN));
}

/*
 * E5 (docs/SPRITE-ERA-DESIGN.md sec 2/4): in the box GRID specifically -- not the
 * hover panel, not the summary, not the party list, all of which keep asking
 * pdna_origin_art_portrait() for the up-to-56x56 picture -- a cell whose
 * RESOLVED era is GEN2 wears the REAL 16x16 Gen-2 menu icon instead, matching
 * what the retail Gold/Silver/Crystal PC list actually shows. This applies
 * uniformly to a GB-import cell resolved to GEN2 (the ordinary case: a Crystal
 * import sitting in an Emerald PC box) AND to a NATIVE Gen-3 mon whose PC/BANK/
 * GBGRID cell the user set to GEN2 in Settings -- both ask the SAME question
 * ("what era does THIS cell, at the CURRENT place, resolve to") and get the
 * SAME answer, which is the whole point: the icon is a property of the CELL's
 * chosen era, not of whether the mon happened to arrive via an import.
 *
 * GEN1-resolved cells are UNCHANGED (fall through to the portrait below): Gen 1
 * has no per-species menu icons at all (rom_gbicon.h; se_resolve's own
 * SE_WHY_NO_ICONS already refuses GEN1 at the PC grid for the same reason), so
 * there is nothing here for GEN1 to serve.
 */
int pdna_origin_box_art(int slot, const PkMon* m, PdnaArt* out) {
  (void)slot;   /* the cache decides the MARKER; the art is always recomputed from the
                 * record, so a stale cache can never put the wrong picture on screen */
  if (out) memset(out, 0, sizeof *out);
  if (!m || !out) return 0;

  PdnaOrigin o;
  pdna_origin_of(m, &o);
  int era = ERA_NATIVE;
  uint16_t nat_dex = pk_national_no(m->species);
  if (s_era_resolver) era = s_era_resolver(s_place, o.gen, o.gen_certain, nat_dex, 0);

  if (era == ERA_GEN2 && s_gb_on && s_gb.icon &&
      m->species >= 1 && m->species <= GEN2_MAX_DEX &&
      pdna_origin_art_have(PDNA_GEN2) &&
      pdna_origin_art_stack_room(PDNA_GB_ICON_NEED)) {
    uint8_t w = 0, h = 0;
    const uint16_t* px = fetch_pic_ex(PDNA_GEN2, nat_dex, 0, 0, 0, 1, &w, &h);
    if (px && w && h) {
      out->px = px; out->w = w; out->h = h; out->gen = PDNA_GEN2;
      out->era = o.gen; out->era_certain = o.gen_certain;
      return 1;
    }
    /* The Gen-2 ROM could not serve this species' icon (should not happen --
     * se_species_exists(GEN2,dex) already gated era==GEN2 on dex<=251 -- but
     * degrade to the ordinary portrait rather than showing nothing, same
     * posture as every other rung's failure). */
  }
  return pdna_origin_art_portrait(m, 0, out, 0);
}

int pdna_origin_cell_render(const PdnaArt* a, uint16_t* dst, int dw, int dh) {
  if (!a || !a->px || !dst) return 0;
  if (dw <= 0 || dh <= 0 || a->w == 0 || a->h == 0) return 0;

  int sw = (int)a->w, sh = (int)a->h;

  /* E5: a source no LARGER than the cell in either axis (the 16x16 Gen-2 menu
   * icon against a 24x22 cell, at every size PDNA_ORIGIN_BOX cells come in
   * today) is blitted 1:1, CENTRED on both axes -- not scaled up, and not
   * anchored to the floor the way a downscaled sprite is below. A retail menu
   * icon must not be stretched (it would look soft/blurred against the crisp
   * Gen-3 OBJ icons beside it) and it is small enough that floor-anchoring
   * would look wrong too (a 16x16 icon sitting at the very bottom of a 22-tall
   * cell reads as "floating low", not "standing" -- true centring is what the
   * real game's own box list does). Every larger-than-cell source (the
   * existing 56x56/64x64 portrait path) is UNCHANGED below, byte for byte. */
  if (sw <= dw && sh <= dh) {
    for (int i = 0; i < dw * dh; i++) dst[i] = 0;
    int x0 = (dw - sw) / 2, y0 = (dh - sh) / 2;
    for (int y = 0; y < sh; y++) {
      const uint16_t* srow = a->px + (unsigned)y * (unsigned)sw;
      uint16_t* drow = dst + (unsigned)(y0 + y) * (unsigned)dw + (unsigned)x0;
      for (int x = 0; x < sw; x++) drow[x] = srow[x] & 0x8000u ? srow[x] : 0u;
    }
    return 1;
  }

  /* Largest box fit that keeps the aspect ratio. Rounded, then clamped: the rounding
   * is what stops a 56x56 pic landing on 21x22 and leaning. */
  int ow = dw, oh = (sh * dw + sw / 2) / sw;
  if (oh > dh) { oh = dh; ow = (sw * dh + sh / 2) / sh; }
  if (ow < 1) ow = 1;
  if (oh < 1) oh = 1;
  if (ow > dw) ow = dw;
  if (oh > dh) oh = dh;

  for (int i = 0; i < dw * dh; i++) dst[i] = 0;      /* transparent everywhere first */

  int x0 = (dw - ow) / 2;      /* centred horizontally */
  int y0 = dh - oh;            /* feet on the floor -- same rule as _place()          */

  for (int y = 0; y < oh; y++) {
    /* The source block this destination row covers. Half-open, and always non-empty
     * even when the scale is an exact integer. */
    int sy0 = (y * sh) / oh, sy1 = ((y + 1) * sh) / oh;
    if (sy1 <= sy0) sy1 = sy0 + 1;
    if (sy1 > sh) sy1 = sh;
    uint16_t* drow = dst + (unsigned)(y0 + y) * (unsigned)dw + (unsigned)x0;
    for (int x = 0; x < ow; x++) {
      int sx0 = (x * sw) / ow, sx1 = ((x + 1) * sw) / ow;
      if (sx1 <= sx0) sx1 = sx0 + 1;
      if (sx1 > sw) sx1 = sw;
      /* The primary sample is PLAIN NEAREST NEIGHBOUR — the block's top-left, which is
       * the same point a point sampler picks. That matters: it makes this scaler a
       * strict SUPERSET of point sampling rather than a different one. Shifting the
       * sample to the block's centre instead was tried and looked worse on Guy's own
       * Red.gb (it re-picks every interior pixel too, so the whole sprite changes and
       * the outlines thicken); the only pixels that should differ from a point sampler
       * are the ones it would have thrown away.
       *
       * STRIDE = a->w. See the header: a->px is the packed RGB15 expansion, not the
       * codec's fixed-56 GbSprite, and a 56 stride here shears every Gen-1 pic that is
       * not 7x7 tiles. */
      uint16_t px = a->px[(unsigned)sy0 * (unsigned)sw + (unsigned)sx0];
      if (!(px & 0x8000)) {
        /* That sample landed on background. Before writing a hole, look at the rest of
         * the block this destination pixel covers: a feature thinner than the reduction
         * — a tail, an ear, Pikachu's bolt — lives entirely between sample points and is
         * otherwise deleted outright. Taking the first opaque pixel in the block costs
         * at most a 3x3 scan and only ever ADDS ink a point sampler missed. */
        for (int yy = sy0; yy < sy1 && !(px & 0x8000); yy++)
          for (int xx = sx0; xx < sx1; xx++) {
            uint16_t q = a->px[(unsigned)yy * (unsigned)sw + (unsigned)xx];
            if (q & 0x8000) { px = q; break; }
          }
      }
      drow[x] = (px & 0x8000) ? px : 0u;
    }
  }
  return 1;
}
