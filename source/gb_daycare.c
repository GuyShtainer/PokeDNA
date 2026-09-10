#include "gb_daycare.h"

#include <string.h>

#define DC_HAS_MON_BIT   0
#define DC_COMPAT_BIT    5
#define DC_HAS_EGG_BIT   6
/* bit 7 (intro-seen / active, VIEW-ONLY) deliberately has no #define -- nothing here
 * ever writes it. */

GbGame gbd_game(const GbSession* s) {
  if (!s) return GBF_G_RED;
  if (s->gen == GB_GEN1) return GBF_G_RED;
  return (s->g2w.sv.version == G2_VER_CRYSTAL) ? GBF_G_CRYSTAL : GBF_G_GS;
}

static bool present(GbGame g, GbField f) { return gbf_off(g, f) != 0; }

static bool read_bytes(const GbSession* s, GbGame g, GbField f, uint8_t* buf, uint16_t want) {
  uint32_t off = gbf_off(g, f);
  uint16_t len = gbf_len(g, f);
  if (!off || len != want) return false;
  GbSession* ncs = (GbSession*)(const void*)s;
  return gbs_read_field(ncs, off, buf, len) == GBS_OK;
}

static bool read_u8(const GbSession* s, GbGame g, GbField f, uint8_t* out) {
  return read_bytes(s, g, f, out, 1);
}

/* Fill one slot (nick/ot/rec fields) + decode it into a GbEditMon. `occupied` is
 * supplied by the caller (from the flag byte), not derived here -- record bytes can
 * legitimately be non-garbage residue on an unoccupied slot (see gb_daycare.h). */
static void read_slot(const GbSession* s, GbGame g, uint8_t gen, bool occupied,
                      GbField nick_f, GbField ot_f, GbField rec_f, GbDaycareSlot* out) {
  memset(out, 0, sizeof *out);
  out->occupied = occupied;

  uint16_t rec_len = gbf_len(g, rec_f);
  uint8_t rec[GB_MAX_REC];
  if (rec_len == 0 || rec_len > sizeof rec) return;
  if (!read_bytes(s, g, rec_f, rec, rec_len)) return;
  if (!read_bytes(s, g, nick_f, out->nick_raw, GB_NAME_BYTES)) return;
  if (!read_bytes(s, g, ot_f, out->ot_raw, GB_NAME_BYTES)) return;

  gb_name_decode(gen, out->nick, sizeof out->nick, out->nick_raw, GB_NAME_BYTES);
  gb_name_decode(gen, out->ot, sizeof out->ot, out->ot_raw, GB_NAME_BYTES);
  gb_load_parts(&out->mon, gen, false, rec, out->ot_raw, out->nick_raw, 0);
}

bool gbd_read(const GbSession* s, GbDaycare* out) {
  if (!s || !out || !s->open) return false;
  memset(out, 0, sizeof *out);

  GbGame  g   = gbd_game(s);
  uint8_t gen = s->gen;
  out->game = g;
  out->gen1 = (gen == GB_GEN1);

  uint8_t man_flag = 0;
  bool have_man_flag = read_u8(s, g, GBF_DAYCARE_FLAG, &man_flag);
  bool slot0_occupied = have_man_flag &&
                        (out->gen1 ? (man_flag != 0) : ((man_flag & (1u << DC_HAS_MON_BIT)) != 0));
  read_slot(s, g, gen, slot0_occupied, GBF_DAYCARE_NICK, GBF_DAYCARE_OT, GBF_DAYCARE_REC,
           &out->slot[0]);

  if (out->gen1) return true;   /* Gen 1: one slot, no breeding -- done */

  out->compatible = (man_flag & (1u << DC_COMPAT_BIT)) != 0;
  out->egg_ready  = (man_flag & (1u << DC_HAS_EGG_BIT)) != 0;
  out->intro_seen = (man_flag & 0x80u) != 0;
  out->has_egg    = out->egg_ready;

  out->has_slot2 = present(g, GBF_DAYCARE2_NICK);
  if (out->has_slot2) {
    uint8_t lady_flag = 0;
    bool have_lady_flag = read_u8(s, g, GBF_DAYCARE_LADY_FLAG, &lady_flag);
    bool slot1_occupied = have_lady_flag && (lady_flag & (1u << DC_HAS_MON_BIT)) != 0;
    read_slot(s, g, gen, slot1_occupied, GBF_DAYCARE2_NICK, GBF_DAYCARE2_OT,
             GBF_DAYCARE2_REC, &out->slot[1]);
  }

  read_u8(s, g, GBF_DAYCARE_STEPS, &out->steps_to_egg);

  if (out->has_egg && present(g, GBF_DAYCARE_EGG_NICK)) {
    uint16_t rec_len = gbf_len(g, GBF_DAYCARE_EGG_REC);
    uint8_t rec[GB_MAX_REC];
    bool have_ot = present(g, GBF_DAYCARE_EGG_OT) &&
                  read_bytes(s, g, GBF_DAYCARE_EGG_OT, out->egg_ot_raw, GB_NAME_BYTES);
    if (rec_len && rec_len <= sizeof rec && read_bytes(s, g, GBF_DAYCARE_EGG_REC, rec, rec_len) &&
       read_bytes(s, g, GBF_DAYCARE_EGG_NICK, out->egg_nick_raw, GB_NAME_BYTES)) {
      gb_name_decode(gen, out->egg_nick, sizeof out->egg_nick, out->egg_nick_raw, GB_NAME_BYTES);
      if (have_ot)
        gb_name_decode(gen, out->egg_ot, sizeof out->egg_ot, out->egg_ot_raw, GB_NAME_BYTES);
      /* P1a review D3: G2_LIST_EGG as the list species, not 0 -- the egg reads as an
       * egg (gb_is_egg()) rather than whatever raw byte its record happens to hold, and
       * the OT is the egg's own OT field, not its nickname reused. */
      gb_load_parts(&out->egg, gen, false, rec,
                    have_ot ? out->egg_ot_raw : out->egg_nick_raw, out->egg_nick_raw,
                    G2_LIST_EGG);
    }
  }

  return true;
}

/* ---- deposit / withdraw ---------------------------------------------------------- */

static bool slot_fields(GbGame g, uint8_t gen, int slot, GbField* nick_f, GbField* ot_f,
                        GbField* rec_f) {
  if (slot == 0) { *nick_f = GBF_DAYCARE_NICK; *ot_f = GBF_DAYCARE_OT; *rec_f = GBF_DAYCARE_REC;
    return true; }
  if (slot == 1 && gen == GB_GEN2) { *nick_f = GBF_DAYCARE2_NICK; *ot_f = GBF_DAYCARE2_OT;
    *rec_f = GBF_DAYCARE2_REC; return present(g, *nick_f); }
  return false;
}

GbsStatus gbd_deposit(GbSession* s, int slot, const GbEditMon* mon) {
  if (!s || !s->open || !mon) return GBS_ERR_ARG;
  GbGame g = gbd_game(s);
  uint8_t gen = s->gen;
  if (mon->gen != gen || mon->is_party) return GBS_ERR_ARG;

  GbField nick_f, ot_f, rec_f;
  if (!slot_fields(g, gen, slot, &nick_f, &ot_f, &rec_f)) return GBS_ERR_ARG;

  uint16_t rec_len = gbf_len(g, rec_f);
  if (rec_len == 0 || mon->rec_len != rec_len) return GBS_ERR_ARG;

  uint8_t flag_field_cur = 0;
  GbField flag_f = (slot == 0) ? GBF_DAYCARE_FLAG : GBF_DAYCARE_LADY_FLAG;
  if (!read_u8(s, g, flag_f, &flag_field_cur)) return GBS_ERR_ARG;
  bool occupied = (gen == GB_GEN1) ? (flag_field_cur != 0)
                                   : ((flag_field_cur & (1u << DC_HAS_MON_BIT)) != 0);
  if (occupied) return GBS_ERR_FULL;

  /* P1a review D2: refuse a structurally-broken record BEFORE it lands. Without this
   * gate an all-zero GbEditMon (never run through gb_set_species/gb_set_level/...) was
   * accepted and its 98 zero bytes were written straight into the slot -- gb_check is
   * the single structural gate this whole tree uses (gen1_write.c / gen2_write.c call
   * it before every commit, gb_edit.h's own header), so day-care gets it too rather
   * than trusting the caller. */
  GbIssues iss;
  if (!gb_check(mon, &iss)) return GBS_ERR_STRUCT;

  uint8_t rec[GB_MAX_REC], otname[GB_NAME_BYTES], nick[GB_NAME_BYTES];
  gb_commit_parts(mon, rec, otname, nick, NULL);

  uint32_t rec_off = gbf_off(g, rec_f), ot_off = gbf_off(g, ot_f), nick_off = gbf_off(g, nick_f);
  GbsStatus st = gbs_write_field(s, rec_off, rec, rec_len);
  if (st != GBS_OK) return st;
  st = gbs_write_field(s, ot_off, otname, GB_NAME_BYTES);
  if (st != GBS_OK) return st;
  st = gbs_write_field(s, nick_off, nick, GB_NAME_BYTES);
  if (st != GBS_OK) return st;

  uint32_t flag_off = gbf_off(g, flag_f);
  uint8_t next_flag = (gen == GB_GEN1) ? 1u
                                       : (uint8_t)(flag_field_cur | (1u << DC_HAS_MON_BIT));
  st = gbs_write_field(s, flag_off, &next_flag, 1);
  if (st != GBS_OK) return st;

  return gbs_finish(s);
}

GbsStatus gbd_withdraw(GbSession* s, int slot, GbEditMon* out) {
  if (!s || !s->open || !out) return GBS_ERR_ARG;
  GbGame g = gbd_game(s);
  uint8_t gen = s->gen;

  GbField nick_f, ot_f, rec_f;
  if (!slot_fields(g, gen, slot, &nick_f, &ot_f, &rec_f)) return GBS_ERR_SLOT;

  GbField flag_f = (slot == 0) ? GBF_DAYCARE_FLAG : GBF_DAYCARE_LADY_FLAG;
  uint8_t flag_cur = 0;
  if (!read_u8(s, g, flag_f, &flag_cur)) return GBS_ERR_SLOT;
  bool occupied = (gen == GB_GEN1) ? (flag_cur != 0)
                                   : ((flag_cur & (1u << DC_HAS_MON_BIT)) != 0);
  if (!occupied) return GBS_ERR_SLOT;

  uint16_t rec_len = gbf_len(g, rec_f);
  uint8_t rec[GB_MAX_REC], otname[GB_NAME_BYTES], nick[GB_NAME_BYTES];
  if (rec_len == 0 || rec_len > sizeof rec) return GBS_ERR_SLOT;
  if (!read_bytes(s, g, rec_f, rec, rec_len)) return GBS_ERR_SLOT;
  if (!read_bytes(s, g, ot_f, otname, GB_NAME_BYTES)) return GBS_ERR_SLOT;
  if (!read_bytes(s, g, nick_f, nick, GB_NAME_BYTES)) return GBS_ERR_SLOT;
  if (!gb_load_parts(out, gen, false, rec, otname, nick, 0)) return GBS_ERR_SLOT;

  uint8_t next_flag = (gen == GB_GEN1) ? 0u
                     : (uint8_t)(flag_cur & ~((1u << DC_HAS_MON_BIT) |
                                              (slot == 0 ? (1u << DC_COMPAT_BIT) : 0u)));
  if (next_flag == flag_cur) return GBS_OK;   /* already clear (defensive; occupied
                                               * already refused this above) */
  GbsStatus st = gbs_write_field(s, gbf_off(g, flag_f), &next_flag, 1);
  if (st != GBS_OK) return st;
  return gbs_finish(s);
}

GbsStatus gbd_withdraw_egg(GbSession* s, GbEditMon* out) {
  if (!s || !s->open || !out) return GBS_ERR_ARG;
  GbGame g = gbd_game(s);
  uint8_t gen = s->gen;
  if (gen != GB_GEN2 || !present(g, GBF_DAYCARE_EGG_NICK)) return GBS_ERR_ARG;

  uint8_t man_flag = 0;
  if (!read_u8(s, g, GBF_DAYCARE_FLAG, &man_flag)) return GBS_ERR_SLOT;
  if ((man_flag & (1u << DC_HAS_EGG_BIT)) == 0) return GBS_ERR_SLOT;

  uint16_t rec_len = gbf_len(g, GBF_DAYCARE_EGG_REC);
  uint8_t rec[GB_MAX_REC], nick[GB_NAME_BYTES], ot[GB_NAME_BYTES];
  if (rec_len == 0 || rec_len > sizeof rec) return GBS_ERR_SLOT;
  if (!read_bytes(s, g, GBF_DAYCARE_EGG_REC, rec, rec_len)) return GBS_ERR_SLOT;
  if (!read_bytes(s, g, GBF_DAYCARE_EGG_NICK, nick, GB_NAME_BYTES)) return GBS_ERR_SLOT;
  /* P1a review D3: the egg's own OT field, not the nickname reused -- falls back to the
   * nickname only if this game/session somehow lacks GBF_DAYCARE_EGG_OT (never true for
   * GS/Crystal, both define it, but a caller handed a malformed session should still
   * get a record rather than a refusal here). */
  bool have_ot = present(g, GBF_DAYCARE_EGG_OT) &&
                read_bytes(s, g, GBF_DAYCARE_EGG_OT, ot, GB_NAME_BYTES);
  if (!have_ot) memcpy(ot, nick, GB_NAME_BYTES);
  /* G2_LIST_EGG, not 0: the withdrawn record stays an egg (gb_is_egg()), not whatever
   * raw species byte the record happens to hold. */
  if (!gb_load_parts(out, gen, false, rec, ot, nick, G2_LIST_EGG)) return GBS_ERR_SLOT;

  uint8_t next_flag = (uint8_t)(man_flag & ~(1u << DC_HAS_EGG_BIT));
  GbsStatus st = gbs_write_field(s, gbf_off(g, GBF_DAYCARE_FLAG), &next_flag, 1);
  if (st != GBS_OK) return st;
  return gbs_finish(s);
}
