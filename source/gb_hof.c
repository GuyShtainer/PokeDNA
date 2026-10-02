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

/* One team's worth of zero bytes, reused for every zero-fill in this file -- 98 B
 * max (Gen 2), well under GBS_SCRATCH_BYTES; a .rodata constant, never a stack
 * array (hard rule 2). D5 (b194 review, golden rule 6): ONE file-static instead of
 * three separate function-local copies (gbh_clear/gbh_append_team/gbh_delete_team
 * each declared their own identical `static const uint8_t k_zero[98] = {0};`). */
static const uint8_t k_zero[98] = {0};

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
 * (byte 0 is the win-count).
 *
 * b89 re-verify A2-LEAK: the species-byte test ALONE is far too weak once
 * hof_raw_slots() (R1) is gbh_set_count()'s own write ceiling -- on virgin Gen-1
 * SRAM (pokered never runs an Erase on sHallOfFame) power-on noise across the
 * whole 16-byte record can easily avoid landing on exactly $00 or $FF in byte 0
 * while every other byte is garbage, so the species test alone reports up to
 * GBH_G1_CAPACITY (50) "present" slots on a blob that is really empty (the count
 * byte reads 0) -- gbh_set_count() would then happily write a count that sends the
 * real League PC to decode 50 noise records (the exact D1 hazard R1 exists to
 * avoid re-introducing from a different angle).
 *
 * The decomp gives an exact, reference discriminator for GENUINE Gen-1 records:
 * AnimateHallOfFame zero-fills the whole 96-byte team buffer before writing into
 * it (pokered/engine/movie/hall_of_fame.asm:17-19) and HoFRecordMonInfo then
 * writes only species + level + an 11-byte name (NAME_LENGTH) = 13 of each 16-byte
 * record (hall_of_fame.asm:267-280) -- so on every REAL record, the trailing 3
 * pad bytes (offsets +13..+15) are left exactly as the zero-fill wrote them, and
 * the recorded level (offset +1) is never 0 (a Pokemon is always level 1-100,
 * hacked saves included -- checking rec[1]==0 rather than a 1..100 range
 * deliberately does not reject a hacked Lv255 team, which is still a real
 * record). Measured against Guy's own corpus: 9/9 real Red records and Yellow's
 * own record pass this test; 0/50 SPLAT-noise slots (test K's own 0x42 fixture)
 * pass it. Gen 2 is deliberately left alone: its 98-byte record has no reserved
 * pad bytes to check, and LoadHOFTeam bails on each record's own win-count byte
 * rather than a species/pad heuristic (R1's own design note). */
static bool hof_slot_present(const GbSession* s, uint32_t team_off) {
  uint32_t mon0_off = (s->gen == GB_GEN1) ? team_off : team_off + 1u;
  uint8_t rec[16];
  if (gbs_read_field((GbSession*)(const void*)s, mon0_off, rec, sizeof rec) != GBS_OK)
    return false;
  /* $FF is used as the "fewer than 6 party members" end-of-team marker on BOTH gens
   * (confirmed against Guy's real Crystal.sav corpus: team 4's 5th mon slot holds
   * species=$FF/otid=0/dv=0/level=0, an unmistakable "this team had 4 members"
   * marker -- not a real Pokemon numbered 255 -- so Gen 2 needs the same $FF check
   * Gen 1's own decomp explicitly documents, even though nothing in the pinned Gen-2
   * decomp excerpts spelled it out as plainly as pokered's AnimateHallOfFame does). */
  if (rec[0] == 0x00u || rec[0] == 0xFFu) return false;
  if (s->gen == GB_GEN1) {
    if (rec[13] || rec[14] || rec[15]) return false;   /* pad bytes: zero-filled, never noise, on a real record */
    if (rec[1] == 0u) return false;                    /* level 0 does not exist on a real Pokemon */
  }
  return true;
}

/* The UNCLAMPED blob scan: how many team slots actually look occupied, with no
 * reference to the count byte at all. A CEILING on what may be written to the
 * count must not be derived from the count itself, or SET COUNT becomes a one-way
 * ratchet (b89 re-verify R1): D2 clamps gbh_team_count_present()'s RESULT to
 * gbh_count() on Gen 1, so if gbh_set_count()'s own cap were gbh_team_count_present()
 * (as the first fix pass had it), every set_count() below the current count would
 * permanently lower the cap for every set_count() after it -- 9 real teams, set to 3,
 * then trying to set back to 9 clamps to 3 forever. This function is the real,
 * count-independent answer to "how many teams does the blob actually hold". */
static int hof_raw_slots(const GbSession* s) {
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
  return n;
}

int gbh_slots_in_blob(const GbSession* s) {
  return (s && s->open) ? hof_raw_slots(s) : 0;
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
  int n = hof_raw_slots(s);
  /* D2 is a GEN-1-ONLY defence (b89 re-verify R1): pokered never initialises
   * sHallOfFame (no Gen-1 EraseHallOfFame), so virgin Gen-1 SRAM can hold power-on
   * noise that happens to look like occupied slots (neither $00 nor $FF) while the
   * real win-count byte still reads 0 -- the real League PC bounds its own decode
   * on that count, so clamping here matches retail. Gen 2 must NOT be clamped the
   * same way: pokecrystal's LoadHOFTeam (engine/events/halloffame.asm:408-431)
   * bails on each RECORD's OWN win-count byte, not on wHallOfFameCount (which only
   * gates the PC menu row, pokecenter_pc.asm:104-109) -- so a Gen-2 cart can show
   * (and this screen must show) more teams than the count byte says. */
  if (s->gen == GB_GEN1) {
    int c = gbh_count(s);
    int cap = hof_capacity(s);
    if (n > c) n = (c < cap) ? c : cap;
  }
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
   * so 200 (the byte's natural ceiling) stays safe there.
   *
   * b89 re-verify R1: the ceiling MUST be hof_raw_slots(), not
   * gbh_team_count_present() -- that function clamps to gbh_count() on Gen 1 (D2),
   * so using it here made the cap ratchet down with every set_count() below the
   * real slot count and never recover (9 real teams -> set 3 -> set 9 stuck at 3). */
  int raw = hof_raw_slots(s);
  int cap = (s->gen == GB_GEN2) ? 200 : ((raw < GBH_G1_CAPACITY) ? raw : 255);
  if (n > cap) n = cap;

  GbGame g = hof_game(s);
  uint32_t off = gbf_off(g, GBF_HOF_COUNT);
  if (!off) return GBS_ERR_ARG;

  uint8_t v = (uint8_t)n;
  GbsStatus st = gbs_write_field(s, off, &v, 1);
  if (st != GBS_OK) return st;
  return gbs_finish(s);
}

/* --- BACKLOG #194: edit / append / delete ---------------------------------------- */

static bool hof_mon_valid(const GbSession* s, const GbHofMon* mon) {
  if (!mon) return false;
  if (gb_index_from_dex(s->gen, mon->dex) == 0) return false;   /* unmapped species */
  if (mon->level < 1 || mon->level > 100) return false;
  return true;
}

/* Writes ONE mon record at `mon_off` -- Gen 1: species+level+nickname (13 of the 16
 * bytes; the 3 reserved pad bytes at +13..+15 are never touched). Gen 2: the whole
 * 16-byte record (species/OT id/DVs/level/nickname; no per-mon pad on Gen 2). Does
 * NOT call gbs_finish -- callers batch one or many of these and finish once, per
 * gbs_write_field's own "call gbs_finish once after the LAST write" contract. */
static GbsStatus hof_write_mon(GbSession* s, uint32_t mon_off, const GbHofMon* mon) {
  if (s->gen == GB_GEN1) {
    uint8_t rec[13];
    rec[0] = gb_index_from_dex(GB_GEN1, mon->dex);
    rec[1] = mon->level;
    gb_name_encode(GB_GEN1, rec + 2, GEN1_NAME_BYTES, GB_NICK_GLYPHS, mon->nick);
    return gbs_write_outside_sum(s, mon_off, rec, sizeof rec);
  }
  uint8_t rec[16];
  rec[0] = gb_index_from_dex(GB_GEN2, mon->dex);
  rec[1] = (uint8_t)(mon->otid >> 8);
  rec[2] = (uint8_t)(mon->otid & 0xFFu);
  rec[3] = (uint8_t)(((mon->dv[0] & 0x0Fu) << 4) | (mon->dv[1] & 0x0Fu));
  rec[4] = (uint8_t)(((mon->dv[2] & 0x0Fu) << 4) | (mon->dv[3] & 0x0Fu));
  rec[5] = mon->level;
  /* D1 (b194 review, HIGH): the real Gen-2 HOF nickname field is 10 bytes with NO
   * reserved terminator (pokecrystal HOF_MON_LENGTH = 1+2+2+1+(MON_NAME_LENGTH-1)
   * = 1+2+2+1+9... i.e. 10 raw bytes, all glyphs, never a trailing 0x50) -- but
   * gb_name_encode(dst, cap, ...) ALWAYS reserves its own last byte for a
   * terminator (gb_edit.c's own "Terminator + padding" contract: `for (; n < cap;
   * n++) dst[n] = NAME_TERM;`), so calling it with cap=10 directly kept only 9
   * real glyphs + one 0x50 -- a LEVEL-ONLY edit (nickname untouched by the
   * caller, but re-encoded from the decoded string every write) silently
   * truncated a 10-glyph nickname by one character every time (TYPHLOSION ->
   * TYPHLOSIO, caught live in this lane's own g2_06 shot). Encode into an
   * 11-byte scratch (cap=11, matching GB_NICK_GLYPHS=10 max glyphs -- room for
   * all 10 real glyphs plus gb_name_encode's own terminator byte), then copy
   * only the first 10 bytes into the real field -- dropping the terminator byte
   * gb_name_encode wrote at index 10, which the real 10-byte field has no room
   * for and gb_name_decode's own 10-byte read (gb_hof.c's hof_decode_mon_g2)
   * never expected in the first place. */
  uint8_t nb[11];
  gb_name_encode(GB_GEN2, nb, sizeof nb, GB_NICK_GLYPHS, mon->nick);
  memcpy(rec + 6, nb, 10);
  return gbs_write_field(s, mon_off, rec, sizeof rec);
}

void gbh_roll_dv(uint32_t seed, uint8_t dv[4]) {
  /* Same LCG (glibc's own constants) and same "advance then take the NEW high
   * nibble" shape as gb_new_mon.c's lcg_nibble -- see that file's comment for why
   * the high nibble, not the low one. Deliberately re-derived here rather than
   * calling across the module boundary (gb_hof.h's own doc comment). */
  uint32_t s = seed;
  if (!dv) return;
  for (int i = 0; i < 4; i++) {
    s = s * 1103515245u + 12345u;
    dv[i] = (uint8_t)((s >> 24) & 0x0Fu);
  }
}

GbsStatus gbh_set_mon(GbSession* s, int team_idx, int mon_idx, const GbHofMon* mon) {
  if (!s || !s->open || !mon) return GBS_ERR_ARG;
  if (mon_idx < 0 || mon_idx >= GBH_NUM_MONS) return GBS_ERR_ARG;
  if (!hof_mon_valid(s, mon)) return GBS_ERR_ARG;

  int present = gbh_team_count_present(s);
  if (team_idx < 0 || team_idx >= present) return GBS_ERR_ARG;
  GbHofTeam t;
  if (!gbh_team(s, team_idx, &t)) return GBS_ERR_ARG;
  if (mon_idx >= t.n) return GBS_ERR_ARG;   /* only an EXISTING mon slot is editable */

  GbGame g = hof_game(s);
  uint32_t base = gbf_off(g, GBF_HOF_TEAMS);
  if (!base) return GBS_ERR_ARG;
  uint32_t stride = hof_team_bytes(s);
  int slot = hof_storage_index(s, team_idx, present);
  if (slot < 0) return GBS_ERR_ARG;
  uint32_t team_off = base + (uint32_t)slot * stride;
  uint32_t mon_off = (s->gen == GB_GEN1) ? team_off + (uint32_t)mon_idx * 16u
                                          : team_off + 1u + (uint32_t)mon_idx * 16u;

  GbsStatus st = hof_write_mon(s, mon_off, mon);
  if (st != GBS_OK) return st;
  return gbs_finish(s);
}

/* Shift the WHOLE table down by one slot (dst=i, src=i-1, i from cap-1 downto 1),
 * discarding whatever was in the bottom slot (cap-1) before the call, and leaving
 * slot 0 UNTOUCHED (the caller overwrites it next) -- "make room at the front" for
 * Gen 2's own AddHallOfFameEntry, which always inserts the newest team at slot 0. */
static GbsStatus hof_shift_down(GbSession* s, uint32_t base, uint32_t stride, int cap) {
  uint8_t buf[98];
  for (int i = cap - 1; i >= 1; i--) {
    uint32_t src = base + (uint32_t)(i - 1) * stride;
    uint32_t dst = base + (uint32_t)i * stride;
    if (gbs_read_field(s, src, buf, stride) != GBS_OK) return GBS_ERR_ARG;
    GbsStatus st = (s->gen == GB_GEN1) ? gbs_write_outside_sum(s, dst, buf, stride)
                                        : gbs_write_field(s, dst, buf, stride);
    if (st != GBS_OK) return st;
  }
  return GBS_OK;
}

/* Close the gap at `from`: shift every slot ABOVE `from` DOWN by one (dst=i,
 * src=i+1, i ascending from `from` to cap-2), leaving slot cap-1's OLD content
 * duplicated into cap-2's old spot and the table's own logical length one shorter
 * -- the opposite direction from hof_shift_down above. Two callers, two different
 * meanings of "the gap": gbh_delete_team closes a gap in the MIDDLE (a team just
 * removed) and leaves the vacated top slot for the caller to zero; Gen 1's own
 * gbh_append_team, once its 50 slots are already full, closes the gap at `from=0`
 * (SaveHallOfFameTeams discards slot 0, the OLDEST team, to make room at the far
 * end, slot 49) and the caller overwrites slot cap-1 with the new team right
 * after -- unlike hof_shift_down, which makes room at the FRONT for Gen 2's own
 * insert-at-0 rule, this makes room at the BACK, which is where Gen 1 always
 * appends. (b194 mutation run caught this: reusing hof_shift_down verbatim for
 * Gen 1's overflow case left slot 0 untouched and DUPLICATED the oldest team into
 * slot 1 instead of evicting it -- test O/append_gen1_boundary's own "GT001 must
 * survive, GT000 must not" check failed until this function was added.) */
static GbsStatus hof_shift_close_gap(GbSession* s, uint32_t base, uint32_t stride,
                                     int cap, int from) {
  uint8_t buf[98];
  for (int i = from; i < cap - 1; i++) {
    uint32_t src = base + (uint32_t)(i + 1) * stride;
    uint32_t dst = base + (uint32_t)i * stride;
    if (gbs_read_field(s, src, buf, stride) != GBS_OK) return GBS_ERR_ARG;
    GbsStatus st = (s->gen == GB_GEN1) ? gbs_write_outside_sum(s, dst, buf, stride)
                                        : gbs_write_field(s, dst, buf, stride);
    if (st != GBS_OK) return st;
  }
  return GBS_OK;
}

GbsStatus gbh_append_team(GbSession* s, const GbHofTeam* team) {
  if (!s || !s->open || !team) return GBS_ERR_ARG;
  if (team->n < 1 || team->n > GBH_NUM_MONS) return GBS_ERR_ARG;
  for (int m = 0; m < team->n; m++)
    if (!hof_mon_valid(s, &team->mon[m])) return GBS_ERR_ARG;   /* whole call refuses first */

  GbGame g = hof_game(s);
  uint32_t base = gbf_off(g, GBF_HOF_TEAMS);
  uint32_t count_off = gbf_off(g, GBF_HOF_COUNT);
  if (!base || !count_off) return GBS_ERR_ARG;
  uint32_t stride = hof_team_bytes(s);
  int cap = hof_capacity(s);
  int old_count = gbh_count(s);
  /* D2 (b194 review, MEDIUM): Gen 1 saturates at 255 (AnimateHallOfFame's own
   * "inc a / jr z, skip" guard, gb_hof.h's header). Gen 2 is DIFFERENT --
   * pokecrystal's halloffame.asm (HOF_MASTER_COUNT, lines 19-23) does NOT
   * increment past 200 at all; the count is simply HELD once it reaches 200,
   * not incremented-then-clamped. Since the new record's own win-count byte is
   * this SAME `new_count` value, the old "saturate at 255" formula would have
   * written a win byte the real game's own counter can never reach (a Gen-2
   * cart's count field is capped at 200 everywhere else in this file, D1/
   * gbh_set_count's own clamp) -- held at 200, not merely clamped after the
   * increment. */
  int new_count = (s->gen == GB_GEN1) ? ((old_count < 255) ? old_count + 1 : 255)
                                       : ((old_count < 200) ? old_count + 1 : old_count);

  int slot;
  if (s->gen == GB_GEN1) {
    if (old_count < cap) {
      slot = old_count;                          /* not yet full: next empty slot */
    } else {
      /* Full: discard slot 0 (the oldest), close the gap, append at the far end --
       * NOT hof_shift_down (that makes room at the FRONT, Gen 2's own rule). */
      GbsStatus sst = hof_shift_close_gap(s, base, stride, cap, 0);
      if (sst != GBS_OK) return sst;
      slot = cap - 1;                              /* full: always the last slot */
    }
  } else {
    GbsStatus sst = hof_shift_down(s, base, stride, cap);   /* Gen 2 always shifts */
    if (sst != GBS_OK) return sst;
    slot = 0;
  }
  uint32_t team_off = base + (uint32_t)slot * stride;

  /* Zero the whole team block first: a fresh record's unused mon slots (and, Gen 1,
   * the pad bytes) must read back as the retail "empty" pattern -- the file-static
   * k_zero (above), GBH_NUM_MONS*16(+1 win-count byte on Gen 2) always <= 98. */
  GbsStatus zst = (s->gen == GB_GEN1) ? gbs_write_outside_sum(s, team_off, k_zero, stride)
                                       : gbs_write_field(s, team_off, k_zero, stride);
  if (zst != GBS_OK) return zst;

  if (s->gen == GB_GEN2) {
    uint8_t wc = (uint8_t)new_count;
    GbsStatus wst = gbs_write_field(s, team_off, &wc, 1);
    if (wst != GBS_OK) return wst;
  }

  for (int m = 0; m < team->n; m++) {
    uint32_t mon_off = (s->gen == GB_GEN1) ? team_off + (uint32_t)m * 16u
                                            : team_off + 1u + (uint32_t)m * 16u;
    GbsStatus mst = hof_write_mon(s, mon_off, &team->mon[m]);
    if (mst != GBS_OK) return mst;
  }

  /* #369: a team of fewer than 6 ends with the retail $FF terminator in the NEXT mon's
   * species byte (retail's HoF display / LoadHOFTeam stop on it; without it the real game
   * pages through blank mons -- Crystal `No.000 ?????`, Red `LEVEL/ 0`). The corpus Crystal.sav
   * carries it on its 4-5 mon teams. A full 6-mon team has no next slot: unchanged. */
  if (team->n < GBH_NUM_MONS) {
    uint32_t term_off = (s->gen == GB_GEN1) ? team_off + (uint32_t)team->n * 16u
                                             : team_off + 1u + (uint32_t)team->n * 16u;
    const uint8_t ff = 0xFFu;
    GbsStatus tst = (s->gen == GB_GEN1) ? gbs_write_outside_sum(s, term_off, &ff, 1)
                                         : gbs_write_field(s, term_off, &ff, 1);
    if (tst != GBS_OK) return tst;
  }

  uint8_t cv = (uint8_t)new_count;
  GbsStatus cst = gbs_write_field(s, count_off, &cv, 1);
  if (cst != GBS_OK) return cst;

  return gbs_finish(s);
}

GbsStatus gbh_delete_team(GbSession* s, int team_idx) {
  if (!s || !s->open) return GBS_ERR_ARG;
  int present = gbh_team_count_present(s);
  if (team_idx < 0 || team_idx >= present) return GBS_ERR_ARG;

  GbGame g = hof_game(s);
  uint32_t base = gbf_off(g, GBF_HOF_TEAMS);
  uint32_t count_off = gbf_off(g, GBF_HOF_COUNT);
  if (!base || !count_off) return GBS_ERR_ARG;
  uint32_t stride = hof_team_bytes(s);
  int cap = hof_capacity(s);
  int slot = hof_storage_index(s, team_idx, present);
  if (slot < 0) return GBS_ERR_ARG;

  /* Close the gap at `slot` (hof_shift_close_gap -- the SAME primitive Gen 1's own
   * gbh_append_team uses once its table is full), then zero the vacated top slot. */
  GbsStatus gst = hof_shift_close_gap(s, base, stride, cap, slot);
  if (gst != GBS_OK) return gst;
  uint32_t last_off = base + (uint32_t)(cap - 1) * stride;
  GbsStatus zst = (s->gen == GB_GEN1) ? gbs_write_outside_sum(s, last_off, k_zero, stride)
                                       : gbs_write_field(s, last_off, k_zero, stride);
  if (zst != GBS_OK) return zst;

  int old_count = gbh_count(s);
  int new_count = (old_count > 0) ? old_count - 1 : 0;
  uint8_t cv = (uint8_t)new_count;
  GbsStatus cst = gbs_write_field(s, count_off, &cv, 1);
  if (cst != GBS_OK) return cst;

  return gbs_finish(s);
}
