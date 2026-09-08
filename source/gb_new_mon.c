/* gb_new_mon.c -- BACKLOG #50: a legal Gen-1/2 record from scratch. See
 * gb_new_mon.h for the design. Pure C: no tonc, no FatFs, no GBA headers. */
#include <string.h>

#include "gb_new_mon.h"

int gb_new_mon_g1_moves(const RomGb1Species* base, RomGbLearn* rl, uint16_t dex,
                        uint8_t level, uint8_t out4[4]) {
  if (!base || !rl || !out4) return -1;
  if (!rl->ok || rl->gen != GB_GEN1) return -1;
  /* rom_gblearn_moves_at_seeded seeds the SAME 4-slot dedup-aware FIFO the
   * table's own within-table walk uses with the starters, so a table entry
   * that relists a starting move (Nidoqueen/Nidoking/Kabutops all do, in
   * Guy's own Red.gb) is skipped rather than duplicated -- see rom_gblearn.h. */
  return rom_gblearn_moves_at_seeded(rl, dex, level, base->start, out4);
}

/* A plain linear congruential generator (glibc's own constants) -- see
 * gb_new_mon.h for why this needs to be reproducible, not unpredictable.
 * Advances `*seed` and returns its NEW high nibble (bits 27..24): a raw LCG's
 * LOW bits have a short period and visibly correlate between draws, which the
 * high bits do not, so four consecutive draws feeding Atk/Def/Spe/Spc do not
 * quietly end up related to each other. */
static uint8_t lcg_nibble(uint32_t* seed) {
  *seed = *seed * 1103515245u + 12345u;
  return (uint8_t)((*seed >> 24) & 0x0Fu);
}

/* ASCII-only uppercase into a caller buffer, NUL-terminated, truncated at
 * `cap` - 1 if `s` is longer. Every species name this tree's own tables
 * produce is plain ASCII, so this is not a full Unicode case-fold -- it does
 * not need to be. */
static void uppercase_ascii(char* dst, int cap, const char* s) {
  int i = 0;
  if (!dst || cap <= 0) return;
  if (s) for (; s[i] && i < cap - 1; i++)
    dst[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 'a' + 'A') : s[i];
  dst[i] = 0;
}

bool gb_new_mon(uint8_t gen, uint16_t dex, uint8_t level, const GbNewMonSrc* src,
               uint32_t seed, GbEditMon* out) {
  GbGen1Base g1b;
  uint32_t s;
  char nick[GB_TEXT_MAX];

  if (!src || !out) return false;
  if (gen != GB_GEN1 && gen != GB_GEN2) return false;
  if (level < 1 || level > 100) return false;
  if (gb_index_from_dex(gen, dex) == 0) return false;          /* no such species */
  if (src->growth != gb_growth_rate(dex)) return false;        /* the cross-check gate */

  if (!gb_load_parts(out, gen, true, NULL, NULL, NULL, 0)) return false;  /* blank party rec */

  memset(&g1b, 0, sizeof g1b);
  g1b.base[GB_HP]  = src->base[GB_HP];
  g1b.base[GB_ATK] = src->base[GB_ATK];
  g1b.base[GB_DEF] = src->base[GB_DEF];
  g1b.base[GB_SPE] = src->base[GB_SPE];
  g1b.base[GB_SPC] = src->base[GB_SPC];
  g1b.type1 = src->type1;
  g1b.type2 = src->type2;
  if (!gb_set_species(out, dex, (gen == GB_GEN1) ? &g1b : NULL)) return false;
  if (!gb_set_level(out, level)) return false;

  s = seed;
  if (!gb_set_dv(out, GB_ATK, lcg_nibble(&s))) return false;
  if (!gb_set_dv(out, GB_DEF, lcg_nibble(&s))) return false;
  if (!gb_set_dv(out, GB_SPE, lcg_nibble(&s))) return false;
  if (!gb_set_dv(out, GB_SPC, lcg_nibble(&s))) return false;

  for (int i = 0; i < 4; i++) {
    uint8_t mv = src->moves[i];
    if (!mv) break;                        /* moves[] is left-packed, see gb_new_mon.h */
    if (!gb_set_move(out, i, mv)) return false;
    (void)gb_set_ppup(out, i, 0);           /* already the fresh default; explicit anyway */
  }

  gb_set_otid(out, src->ot_id);
  if (src->ot_name && src->ot_name[0])
    (void)gb_set_otname_lossy(out, src->ot_name);   /* never refuse over a foreign glyph */

  uppercase_ascii(nick, (int)sizeof nick, src->species_name);
  if (nick[0] && !gb_set_nickname(out, nick))
    (void)gb_set_nickname_lossy(out, nick);   /* belt and suspenders; see gb_new_mon.h */

  if (gen == GB_GEN2) {
    (void)gb_set_held_item(out, 0);
    (void)gb_set_friendship(out, 70);
    (void)gb_set_caught(out, 0, 0, 0, 0);
    (void)gb_set_egg(out, false);
  }

  return gbe_settle_stats(out);
}
