#include <string.h>

#include "gb_hof.h"
#include "gb_fields.h"
#include "gb_edit.h"        /* GB_GEN1/GB_GEN2, gb_dex_from_index, gb_name_decode      */
#include "gen1_save.h"      /* GEN1_OFF_HOF, GEN1_HOF_TEAM_BYTES, GEN1_HOF_NUM_TEAMS   */
#include "gen2_save.h"      /* G2_VER_CRYSTAL, g2_dv_shiny                             */

/* gb_hof.c — see gb_hof.h for the full design comment (BACKLOG #89). */

/* --- big-endian read, local copy (same convention as gen1_write.c/gen2_save.c: a
 * shared little-endian helper would be too easy to grab by mistake on this tree). --- */
static uint16_t rd16be(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

static GbGame hof_game(const GbSession* s) {
  /* Yellow deliberately maps to GBF_G_RED here (gbt_game/gbb_game's own convention,
   * gb_bag.c:131): every HOF offset this table defines is numerically identical on
   * Red/Blue/Yellow, so which of the two is picked never changes a single byte read
   * or written. */
  if (s->gen == GB_GEN1) return GBF_G_RED;
  return (s->g2w.sv.version == G2_VER_CRYSTAL) ? GBF_G_CRYSTAL : GBF_G_GS;
}

static int hof_capacity(const GbSession* s) {
  return (s->gen == GB_GEN1) ? GBH_G1_CAPACITY : GBH_G2_CAPACITY;
}

static uint32_t hof_team_bytes(const GbSession* s) {
  return (s->gen == GB_GEN1) ? GEN1_HOF_TEAM_BYTES : 98u;
}

/* True when a team slot at file offset `team_off` looks occupied: its first mon's raw
 * species byte is neither the Gen-1 $FF terminator nor $00 (the "never touched" state
 * on both gens -- see gb_hof.h's own note on why Gen 1 checks both). `mon0_off` is
 * team_off itself on Gen 1 (species is byte 0 of the team) and team_off+1 on Gen 2
 * (byte 0 is the win-count). */
static bool hof_slot_present(const GbSession* s, uint32_t team_off) {
  uint32_t mon0_off = (s->gen == GB_GEN1) ? team_off : team_off + 1u;
  uint8_t species;
  if (gbs_read_field((GbSession*)(const void*)s, mon0_off, &species, 1) != GBS_OK) return false;
  /* $FF is used as the "fewer than 6 party members" end-of-team marker on BOTH gens
   * (confirmed against Guy's real Crystal.sav corpus: team 4's 5th mon slot holds
   * species=$FF/otid=0/dv=0/level=0, an unmistakable "this team had 4 members"
   * marker -- not a real Pokemon numbered 255 -- so Gen 2 needs the same $FF check
   * Gen 1's own decomp explicitly documents, even though nothing in the pinned Gen-2
   * decomp excerpts spelled it out as plainly as pokered's AnimateHallOfFame does). */
  return species != 0x00u && species != 0xFFu;
}

int gbh_count(const GbSession* s) {
  if (!s || !s->open) return 0;
  GbGame g = hof_game(s);
  uint32_t off = gbf_off(g, GBF_HOF_COUNT);
  if (!off) return 0;
  uint8_t v = 0;
  if (gbs_read_field((GbSession*)(const void*)s, off, &v, 1) != GBS_OK) return 0;
  return (int)v;
}

int gbh_team_count_present(const GbSession* s) {
  if (!s || !s->open) return 0;
  GbGame g = hof_game(s);
  uint32_t base = gbf_off(g, GBF_HOF_TEAMS);
  if (!base) return 0;
  uint32_t stride = hof_team_bytes(s);
  int cap = hof_capacity(s);
  int n = 0;
  for (int i = 0; i < cap; i++) {
    if (!hof_slot_present(s, base + (uint32_t)i * stride)) break;
    n++;
  }
  /* D2: pokered never initialises sHallOfFame (no Gen-1 EraseHallOfFame), so virgin
   * SRAM can hold power-on noise that happens to look like occupied slots (neither
   * $00 nor $FF) while the real win-count byte still reads 0. Trust the count byte
   * as the ceiling on what's genuinely present, same as the retail League PC does. */
  { int c = gbh_count(s); if (n > c) n = (c < cap) ? c : cap; }
  return n;
}

/* storage_index for UI index `i` (0 = newest). Gen 1 stores oldest-first (newest at
 * present-1); Gen 2 stores newest-first (AddHallOfFameEntry inserts at 0) already. */
static int hof_storage_index(const GbSession* s, int i, int present) {
  if (s->gen == GB_GEN1) return present - 1 - i;
  return i;
}

static void hof_decode_mon_g1(const GbSession* s, uint32_t mon_off, GbHofMon* out) {
  uint8_t raw_species = 0, level = 0, nick[GEN1_NAME_BYTES];
  gbs_read_field((GbSession*)(const void*)s, mon_off, &raw_species, 1);
  if (raw_species == 0x00u || raw_species == 0xFFu) { out->present = false; return; }
  out->present = true;
  out->dex = gb_dex_from_index(GB_GEN1, raw_species);
  gbs_read_field((GbSession*)(const void*)s, mon_off + 1u, &level, 1);
  out->level = level;
  memset(nick, 0, sizeof nick);
  gbs_read_field((GbSession*)(const void*)s, mon_off + 2u, nick, GEN1_NAME_BYTES);
  gb_name_decode(GB_GEN1, out->nick, GBH_NICK_CAP, nick, GEN1_NAME_BYTES);
  /* No OT id / DVs / shiny on Gen 1 -- already zeroed by the caller's memset. */
}

static void hof_decode_mon_g2(const GbSession* s, uint32_t mon_off, GbHofMon* out) {
  uint8_t rec[16];
  memset(rec, 0, sizeof rec);
  gbs_read_field((GbSession*)(const void*)s, mon_off, rec, sizeof rec);
  /* $FF terminates a short (<6-member) team on Gen 2 too -- see hof_slot_present's
   * comment for the corpus evidence (Crystal.sav team 4). */
  if (rec[0] == 0x00u || rec[0] == 0xFFu) { out->present = false; return; }
  out->present = true;
  out->dex = gb_dex_from_index(GB_GEN2, rec[0]);
  out->otid = rd16be(rec + 1);
  out->dv[0] = (uint8_t)(rec[3] >> 4);   /* Attack  */
  out->dv[1] = (uint8_t)(rec[3] & 0x0F); /* Defense */
  out->dv[2] = (uint8_t)(rec[4] >> 4);   /* Speed   */
  out->dv[3] = (uint8_t)(rec[4] & 0x0F); /* Special */
  out->shiny = g2_dv_shiny(out->dv);
  out->level = rec[5];
  gb_name_decode(GB_GEN2, out->nick, GBH_NICK_CAP, rec + 6, 10);
}

bool gbh_team(const GbSession* s, int i, GbHofTeam* out) {
  if (!out) return false;
  memset(out, 0, sizeof *out);
  if (!s || !s->open || i < 0) return false;

  int present = gbh_team_count_present(s);
  if (i >= present) return false;

  GbGame g = hof_game(s);
  uint32_t base = gbf_off(g, GBF_HOF_TEAMS);
  if (!base) return false;
  uint32_t stride = hof_team_bytes(s);
  int slot = hof_storage_index(s, i, present);
  if (slot < 0) return false;
  uint32_t team_off = base + (uint32_t)slot * stride;

  if (s->gen == GB_GEN2) {
    uint8_t wc = 0;
    gbs_read_field((GbSession*)(const void*)s, team_off, &wc, 1);
    out->win_count = wc;
  }

  int n = 0;
  for (int m = 0; m < GBH_NUM_MONS; m++) {
    GbHofMon* mon = &out->mon[m];
    uint32_t mon_off;
    if (s->gen == GB_GEN1) {
      mon_off = team_off + (uint32_t)m * 16u;   /* GEN1_HOF_TEAM_BYTES / GBH_NUM_MONS */
      hof_decode_mon_g1(s, mon_off, mon);
    } else {
      mon_off = team_off + 1u + (uint32_t)m * 16u;
      hof_decode_mon_g2(s, mon_off, mon);
    }
    if (!mon->present) break;   /* the retail terminator: stop, do not keep scanning */
    n++;
  }
  out->n = n;
  return true;
}

GbsStatus gbh_clear(GbSession* s) {
  if (!s || !s->open) return GBS_ERR_ARG;
  GbGame g = hof_game(s);
  uint32_t base = gbf_off(g, GBF_HOF_TEAMS);
  uint32_t count_off = gbf_off(g, GBF_HOF_COUNT);
  if (!base || !count_off) return GBS_ERR_ARG;

  uint32_t stride = hof_team_bytes(s);
  int cap = hof_capacity(s);
  /* One team's worth of zero bytes, reused for every slot -- 98 B max (Gen 2), well
   * under GBS_SCRATCH_BYTES; a .rodata constant, never a stack array (hard rule 2). */
  static const uint8_t k_zero[98] = {0};

  for (int i = 0; i < cap; i++) {
    uint32_t off = base + (uint32_t)i * stride;
    GbsStatus st = (s->gen == GB_GEN1)
                     ? gbs_write_outside_sum(s, off, k_zero, stride)
                     : gbs_write_field(s, off, k_zero, stride);
    if (st != GBS_OK) return st;   /* caller restores the WHOLE image -- see gb_hof.h */
  }

  uint8_t zero = 0;
  GbsStatus st = gbs_write_field(s, count_off, &zero, 1);
  if (st != GBS_OK) return st;

  return gbs_finish(s);   /* Gen 1: no-op (gen1 writes finalize per-call). Gen 2: one
                           * whole-batch checksum/mirror refresh for every write above. */
}

GbsStatus gbh_set_count(GbSession* s, int n) {
  if (!s || !s->open) return GBS_ERR_ARG;
  if (n < 0) n = 0;
  /* Gen 1's real League PC decodes every team slot up to the stored count without
   * an independent bounds check (AnimateHallOfFame walks mon 0 of each of `count`
   * slots before ever looking at the $FF terminator) -- a count past the teams this
   * cart actually has drawn from BaseStats out of bounds. Clamp to the teams present
   * (never past GBH_G1_CAPACITY) so the count this writes can never exceed what's
   * really stored. Gen 2's own HOF viewer re-derives its own count from the slots,
   * so 200 (the byte's natural ceiling) stays safe there. */
  int cap = (s->gen == GB_GEN2) ? 200 : ((gbh_team_count_present(s) < GBH_G1_CAPACITY) ? gbh_team_count_present(s) : 255);
  if (n > cap) n = cap;

  GbGame g = hof_game(s);
  uint32_t off = gbf_off(g, GBF_HOF_COUNT);
  if (!off) return GBS_ERR_ARG;

  uint8_t v = (uint8_t)n;
  GbsStatus st = gbs_write_field(s, off, &v, 1);
  if (st != GBS_OK) return st;
  return gbs_finish(s);
}
