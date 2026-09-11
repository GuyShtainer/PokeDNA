#include "gen3_to_gb.h"
#include "gen3_mon.h"      /* PkMon, pk_decode_mon, pk_resolve, PK_* stat order */
#include "gen3_box.h"      /* pk_resolve — fills level/gender for a BOX record */
#include "data_tables.h"   /* pk_national_no, pk_species_ability, growth/exp tables */
#include "evolutions.h"    /* pk_evo_have_data/pk_evo_floor — BACKLOG #104 R1 */
#include <string.h>

/* ---- status text ----------------------------------------------------------- */

const char* g3gb_status_text(G3GbStatus st) {
  switch (st) {
    case G3GB_OK:             return "ok";
    case G3GB_ERR_ARG:        return "bad argument";
    case G3GB_ERR_EGG:        return "an Egg cannot become a Game Boy record";
    case G3GB_ERR_SPECIES:    return "this species has no Game Boy form";
    case G3GB_ERR_MOVE:       return "a move this generation does not have";
    case G3GB_ERR_NEEDS_BASE: return "Gen 1 needs base stats/types (GbGen1Base)";
    case G3GB_ERR_GLITCH:     return "the Gen-3 record could not be decoded";
  }
  return "?";
}

/* Markings (four owner-set marks: circle/square/triangle/heart) are a single PLAINTEXT
 * byte the Growth/Attacks/EVs/Misc substruct machinery never touches — struct BoxPokemon
 * lays out personality(4)@0x00, otId(4)@0x04, nickname(10)@0x08, language(1)@0x12,
 * flags(1)@0x13, otName(7)@0x14, markings(1)@0x1B, checksum(2)@0x1C (gen3_mon.c reads
 * language at mon[0x12] and the checksum at mon[0x1C] directly, so mon[0x1B] — the one
 * byte in between — is markings by elimination, independent of any wiki). 0 = no marks
 * set, which is the "default" a caller need not be told is being lost. */
static uint8_t rec_markings(const uint8_t* rec80) { return rec80[0x1B]; }

/* GEN12_IV_LOW_BIT (gen12_convert.h) fixes the up-converter's IV = DV*2 + 0, i.e.
 * DV = IV/2 exactly, with no rounding ambiguity — this is that inverse, spelled out
 * once so both this file and the merge in gb_sidecar.c use the identical rule. */
static uint8_t iv_to_dv(uint8_t iv) { return (uint8_t)(iv / 2u); }

/* ---- helpers, each one stage of the build, each checked -----------------------
 *
 * Every setter below returns bool and IS checked (rule: trust nothing across a
 * boundary), folding into G3GB_ERR_GLITCH on a false it should not have been able to
 * produce given the screening in screen() below. That fallback is not theoretical:
 * an earlier revision of set_moves() called gb_set_ppup() for every slot including
 * empty ones, and gb_set_ppup() REFUSES a non-zero PP-Up count on an empty move slot
 * by design (gb_edit.c — "an empty slot has no maximum ... clear the slot with
 * gb_set_move(e, i, 0) instead"). That was a bug in THIS file's call order, not a
 * corrupt Gen-3 record: it refused every single Pokemon with fewer than four moves
 * (181 of 654 real corpus mons) as G3GB_ERR_GLITCH. Fixed below by never touching a
 * slot's PP Ups or PP at all when its move is empty — the checked-return convention
 * is what caught it (every refusal showed up as a counted GLITCH rather than a
 * silently wrong record), and it stays, but the earlier comment's explanation for
 * *why* a setter might refuse here was wrong and has been corrected to this one. */

static G3GbStatus screen(const uint8_t* rec80, uint8_t gen, const GbGen1Base* g1base,
                         PkMon* m, uint16_t* dex_out) {
  if (!pk_decode_mon(rec80, false, m)) return G3GB_ERR_GLITCH;
  if (m->isEgg || m->isBadEgg) return G3GB_ERR_EGG;
  pk_resolve(m);   /* fills m->level (box mons decode with level 0) and m->gender */

  /* A checksum-valid record can still hold a level byte outside 1..100 for a party
   * mon (gen3_mon.c reads mon[0x54] straight into m->level with no clamp — box levels
   * come back clamped by pk_level_from_exp, so this can only bite a party record). */
  if (m->level < 1 || m->level > 100) return G3GB_ERR_GLITCH;

  uint16_t dex = pk_national_no(m->species);
  if (dex < 1 || dex > gb_max_species(gen)) return G3GB_ERR_SPECIES;

  for (int i = 0; i < 4; i++)
    if (m->moves[i] != 0 && m->moves[i] > gb_max_move(gen)) return G3GB_ERR_MOVE;

  if (gen == GB_GEN1 && !g1base) return G3GB_ERR_NEEDS_BASE;

  *dex_out = dex;
  return G3GB_OK;
}

/* Species FIRST: gb_set_level needs the growth rate gb_set_species just installed,
 * and (Gen 1) the type/catch-rate bytes have to exist before anything else runs. */
static G3GbStatus set_identity_and_level(GbEditMon* e, const PkMon* m, uint16_t dex,
                                         const GbGen1Base* g1base, Gen3ToGbLoss* loss) {
  if (!gb_set_species(e, dex, g1base)) return G3GB_ERR_SPECIES;

  gb_set_otid(e, (uint16_t)(m->otId & 0xFFFFu));
  loss->secret_id = (m->otId >> 16) != 0;

  if (!gb_set_level(e, m->level)) return G3GB_ERR_GLITCH;
  return G3GB_OK;
}

/* DVs. Gen 3's stat order is HP,Atk,Def,Spe,SpA,SpD; the Game Boy's is HP,Atk,Def,
 * Spe,Spc — Spc is fed from SpA (gen12_convert.h documents the same choice on the way
 * up), so SpD's information is what ivs_halved additionally reports losing.
 *
 * Stat exp. GB_SPC takes the LARGER of SpA/SpD's investment so a Pokemon trained
 * either way keeps at least that much of its special stat exp visibly; the sidecar
 * keeps the true split for the round trip up (design doc section 4, "stat exp: KEPT
 * AS SIDECAR EVs"). ev*257 never exceeds 65535 (255*257 == 65535), so every one of
 * these is in gb_set_statexp's domain by construction. */
static G3GbStatus set_dvs_and_statexp(GbEditMon* e, const PkMon* m, Gen3ToGbLoss* loss) {
  uint8_t iv_atk = m->ivs[PK_ATK], iv_def = m->ivs[PK_DEF];
  uint8_t iv_spe = m->ivs[PK_SPE], iv_spa = m->ivs[PK_SPA], iv_spd = m->ivs[PK_SPD];
  if (!gb_set_dv(e, GB_ATK, iv_to_dv(iv_atk))) return G3GB_ERR_GLITCH;
  if (!gb_set_dv(e, GB_DEF, iv_to_dv(iv_def))) return G3GB_ERR_GLITCH;
  if (!gb_set_dv(e, GB_SPE, iv_to_dv(iv_spe))) return G3GB_ERR_GLITCH;
  if (!gb_set_dv(e, GB_SPC, iv_to_dv(iv_spa))) return G3GB_ERR_GLITCH;
  loss->ivs_halved = ((iv_atk | iv_def | iv_spe | iv_spa) & 1u) != 0 ||
                     (iv_to_dv(iv_spa) != iv_to_dv(iv_spd));

  uint8_t ev_hp = m->evs[PK_HP], ev_atk = m->evs[PK_ATK], ev_def = m->evs[PK_DEF];
  uint8_t ev_spe = m->evs[PK_SPE], ev_spa = m->evs[PK_SPA], ev_spd = m->evs[PK_SPD];
  uint8_t ev_spc = (ev_spa > ev_spd) ? ev_spa : ev_spd;
  if (!gb_set_statexp(e, GB_HP,  (uint16_t)(ev_hp  * 257u))) return G3GB_ERR_GLITCH;
  if (!gb_set_statexp(e, GB_ATK, (uint16_t)(ev_atk * 257u))) return G3GB_ERR_GLITCH;
  if (!gb_set_statexp(e, GB_DEF, (uint16_t)(ev_def * 257u))) return G3GB_ERR_GLITCH;
  if (!gb_set_statexp(e, GB_SPE, (uint16_t)(ev_spe * 257u))) return G3GB_ERR_GLITCH;
  if (!gb_set_statexp(e, GB_SPC, (uint16_t)(ev_spc * 257u))) return G3GB_ERR_GLITCH;
  loss->evs_scaled = (ev_hp | ev_atk | ev_def | ev_spe | ev_spa | ev_spd) != 0;
  return G3GB_OK;
}

/* Moves: base PP first (gb_set_move resets it), then PP Ups, then current PP clamped
 * to whatever maximum those Ups now allow — and NEITHER of the last two for an empty
 * slot: gb_set_ppup() refuses a non-zero PP-Up count with no move to buy it for, and
 * an empty slot's PP byte has to stay 0 (see the note above this file's helpers). */
static G3GbStatus set_moves(GbEditMon* e, uint8_t gen, const PkMon* m) {
  for (int i = 0; i < 4; i++) {
    uint16_t mv = m->moves[i];
    uint8_t mv8 = (mv > 255u) ? 0 : (uint8_t)mv;   /* screened <= gb_max_move above */
    if (!gb_set_move(e, i, mv8)) return G3GB_ERR_GLITCH;
    if (mv8 == 0) continue;

    uint8_t ppups = (uint8_t)((m->ppBonuses >> (i * 2)) & 0x3u);
    if (!gb_set_ppup(e, i, ppups)) return G3GB_ERR_GLITCH;
    uint8_t cap = gb_max_pp(gen, mv8, ppups);
    uint8_t pp = m->pp[i];
    if (pp > cap) pp = cap;
    if (!gb_set_pp(e, i, pp)) return G3GB_ERR_GLITCH;
  }
  return G3GB_OK;
}

/* Names. gb_text_lossy tells us BEFORE we touch anything whether the exact setter
 * will accept the text; the _lossy variant is only reached when it would refuse. */
static G3GbStatus set_names(GbEditMon* e, uint8_t gen, const PkMon* m, Gen3ToGbLoss* loss) {
  char bad[GB_GLYPH_MAX];
  if (gb_text_lossy(gen, m->nickname, GB_NICK_GLYPHS, bad) == 0) {
    if (!gb_set_nickname(e, m->nickname)) return G3GB_ERR_GLITCH;
  } else {
    if (!gb_set_nickname_lossy(e, m->nickname)) return G3GB_ERR_GLITCH;
    loss->nick_lossy = true;
    memcpy(loss->nick_first_bad, bad, GB_GLYPH_MAX);
  }
  if (gb_text_lossy(gen, m->otName, GB_OT_GLYPHS, bad) == 0) {
    if (!gb_set_otname(e, m->otName)) return G3GB_ERR_GLITCH;
  } else {
    if (!gb_set_otname_lossy(e, m->otName)) return G3GB_ERR_GLITCH;
    loss->ot_lossy = true;
    memcpy(loss->ot_first_bad, bad, GB_GLYPH_MAX);
  }
  return G3GB_OK;
}

/* Gen-2-only record fields; Gen 1 has none of these, so it only records what got
 * dropped. Pokerus is a single byte, high nibble strain / low nibble days remaining,
 * in BOTH generations (gen2_save.c:490 rec[0x1C]; gen3_mon.c:139 m[0x00] of the Misc
 * substruct use the identical nibble split) — verified layout, not a guess, so it is
 * wired through rather than reported as pokerus_dropped. Crystal's capture record
 * (time/level/loc/ot_gender) is only READ by Crystal; gen2_save.c:493-497 shows it is
 * just bytes 0x1D/0x1E, so writing it on a G/S target is harmless dead data rather
 * than a corruption. RTC-less time slot (0 = "not recorded") and location 0 (no
 * Gen-3 -> Gen-2 location map exists) — only the level and OT gender carry over. */
static G3GbStatus set_gen2_only_fields(GbEditMon* e, uint8_t gen, bool caught_available,
                                       const PkMon* m, Gen3ToGbLoss* loss) {
  if (gen != GB_GEN2) {
    loss->friendship_dropped = true;   /* Gen 1 has no friendship byte at all */
    loss->pokerus_dropped    = (m->pokerus != 0);
    return G3GB_OK;
  }
  if (!gb_set_friendship(e, m->friendship)) return G3GB_ERR_GLITCH;
  if (!gb_set_pokerus(e, m->pokerus)) return G3GB_ERR_GLITCH;
  uint8_t caught_level = (m->metLevel > 63) ? 63 : m->metLevel;
  /* BACKLOG #95 review C11: on a Gold/Silver target (caught_available false) this write
   * is refused by gb_set_caught's own has_caught gate (e->has_caught was set to
   * caught_available below, in gen3_to_gb) -- that is expected, not a conversion
   * failure, so the refusal is discarded rather than propagated. loss->met_data (set in
   * set_remaining_loss_flags) already reports the met-data loss this represents. */
  if (caught_available) {
    if (!gb_set_caught(e, 0, caught_level, 0, m->otGender)) return G3GB_ERR_GLITCH;
  } else {
    (void)gb_set_caught(e, 0, caught_level, 0, m->otGender);
  }
  return G3GB_OK;
}

/* Everything else the Game Boy record cannot carry, none of it ever refused on:
 * nature/ability/ribbons/contest/met data/ball/markings/held item/shiny-or-gender
 * drift, each flagged only when it is non-default so the UI lists real losses. */
static void set_remaining_loss_flags(const GbEditMon* e, const uint8_t* rec80,
                                     const PkMon* m, Gen3ToGbLoss* loss) {
  loss->item_dropped = (m->heldItem != 0);   /* no Gen-3 -> Gen-2 item map in this tree */

  uint16_t ab0 = pk_species_ability(m->species, 0), ab1 = pk_species_ability(m->species, 1);
  loss->ability  = (ab0 != ab1);
  loss->nature   = (pk_nature_boost(m->nature) != -1) || (pk_nature_hinder(m->nature) != -1);
  loss->ribbons  = (m->ribbons != 0);
  loss->contest  = (m->contest[0] | m->contest[1] | m->contest[2] |
                    m->contest[3] | m->contest[4] | m->contest[5]) != 0;
  loss->met_data = (m->metLocation != 0) || (m->metLevel != 0) || (m->metGame != 0);
  loss->ball     = (m->pokeball != 1);       /* 1 == the plain Poke Ball */
  loss->markings = (rec_markings(rec80) != 0);

  GbDvEffects eff;
  gb_dv_effects_of(e, &eff);
  loss->shiny_lost  = m->isShiny && !eff.shiny;
  loss->gender_lost = (m->gender != (uint8_t)eff.gender);
}

/* Every field is built into a LOCAL record and only copied to `out` on the final
 * G3GB_OK — never into the caller's `out` directly — so "on refusal `out` is untouched"
 * holds even for a defensive mid-function refusal, not only for the up-front screening. */
G3GbStatus gen3_to_gb(const uint8_t* rec80, uint8_t gen, bool caught_available,
                     const GbGen1Base* g1base, GbEditMon* out, Gen3ToGbLoss* loss) {
  Gen3ToGbLoss local_loss;
  if (!loss) loss = &local_loss;
  memset(loss, 0, sizeof *loss);

  if (!rec80 || !out || (gen != GB_GEN1 && gen != GB_GEN2)) return G3GB_ERR_ARG;

  PkMon m;
  uint16_t dex;
  G3GbStatus st = screen(rec80, gen, g1base, &m, &dex);
  if (st != G3GB_OK) return st;

  GbEditMon e;
  uint8_t rec[GB_MAX_REC];
  uint8_t nm[GB_NAME_BYTES];
  memset(rec, 0, sizeof rec);
  memset(nm, 0x50, sizeof nm);
  if (!gb_load_parts(&e, gen, false, rec, nm, nm, 0)) return G3GB_ERR_ARG;
  /* BACKLOG #95 review C1, then C11 (gbmon lane): this is a FRESHLY SYNTHESIZED
   * record, not a real save's own bytes -- gb_set_caught's has_caught gate (gb_edit.h)
   * exists to stop the LIVE EDITOR writing into a real Gold/Silver save's Unused1/
   * Unused2 bytes, and it was never meant to block this module's own capture-record
   * write outright (C1's fix). But C1's fix went too far the other way: it opened the
   * gate unconditionally, so EVERY Gen-2 target got a synthetic capture record even
   * when the target save is Gold/Silver, where those bytes are read as arbitrary
   * Unused1/Unused2, not a capture record -- the caller now says which is true. */
  if (gen == GB_GEN2) gb_set_caught_available(&e, caught_available);

  st = set_identity_and_level(&e, &m, dex, g1base, loss);
  if (st != G3GB_OK) return st;

  st = set_dvs_and_statexp(&e, &m, loss);
  if (st != G3GB_OK) return st;

  st = set_moves(&e, gen, &m);
  if (st != G3GB_OK) return st;

  st = set_names(&e, gen, &m, loss);
  if (st != G3GB_OK) return st;

  st = set_gen2_only_fields(&e, gen, caught_available, &m, loss);
  if (st != G3GB_OK) return st;

  set_remaining_loss_flags(&e, rec80, &m, loss);

  *out = e;
  return G3GB_OK;
}

/* ---- BACKLOG #104 R1: the MAKE LEGAL correction --------------------------------
 * See gen3_to_gb.h for the contract. */
bool gen3_to_gb_evo_needs_fix(const GbEditMon* out, uint8_t* from_level, uint8_t* to_level) {
  if (!out) return false;
  if (!pk_evo_have_data()) return false;         /* "no data" never manufactures a fix */
  uint16_t dex = gb_get_species_dex(out);
  if (dex == 0) return false;
  /* pk_evo_FLOOR, not pk_evo_min_level: 20 of the 100 offer-capable species are catchable
   * wild BELOW their own evolution level (Sootopolis' Super Rod gives a L5 Gyarados),
   * pk_evo_floor is what this tool's OWN checker judges against (gen3_legality_hooks.c
   * pk2_evo_floor) and what the create flow builds at (gen3_edit.c gen3_build_level).
   * Offering to 'fix' a mon the checker calls legal is the over-correction gen3_edit.c
   * names as the one direction this project forbids (r1 review D1: 4 of 5 offers on
   * Guy's own saves were false). */
  int min_lvl = pk_evo_floor(dex);
  if (min_lvl == PK_EVO_NO_DATA || min_lvl <= 1) return false;
  uint8_t cur = gb_get_level(out);
  if (cur >= (uint8_t)min_lvl) return false;
  if (from_level) *from_level = cur;
  if (to_level)   *to_level   = (uint8_t)min_lvl;
  return true;
}
