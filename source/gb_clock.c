#include "gb_clock.h"

#include <string.h>

GbGame gbc_game(const GbSession* s) {
  if (!s) return GBF_G_RED;
  if (s->gen == GB_GEN1) return GBF_G_RED;   /* Yellow == Red/Blue layout, no clock either way */
  return (s->g2w.sv.version == G2_VER_CRYSTAL) ? GBF_G_CRYSTAL : GBF_G_GS;
}

bool gbc_field_present(GbGame game, GbField field) {
  return gbf_off(game, field) != 0;
}

/* ---- scalar get/set, same shape as gb_trainer.c's get_u/set_u ------------------- */

static bool get_u8(const GbSession* s, GbGame g, GbField f, uint8_t* out) {
  uint32_t off = gbf_off(g, f);
  uint16_t len = gbf_len(g, f);
  if (!off || len != 1) return false;
  GbSession* ncs = (GbSession*)(const void*)s;
  uint8_t b;
  if (gbs_read_field(ncs, off, &b, 1) != GBS_OK) return false;
  *out = b;
  return true;
}

static GbsStatus set_u8_unless_same(GbSession* s, GbGame g, GbField f, uint8_t v,
                                    bool* changed) {
  uint32_t off = gbf_off(g, f);
  uint16_t len = gbf_len(g, f);
  if (!off || len != 1) return GBS_ERR_ARG;
  uint8_t cur;
  if (get_u8(s, g, f, &cur) && cur == v) return GBS_OK;
  GbsStatus st = gbs_write_field(s, off, &v, 1);
  if (st == GBS_OK) *changed = true;
  return st;
}

/* ---- read ------------------------------------------------------------------------ */

bool gbc_read(const GbSession* s, GbClock* out) {
  if (!s || !out || !s->open) return false;
  memset(out, 0, sizeof *out);

  GbGame g = gbc_game(s);
  out->present = gbc_field_present(g, GBF_RTC_START_DAY);
  if (!out->present) return true;   /* Gen 1: a successful "there is no clock" read */

  uint8_t v;
  if (get_u8(s, g, GBF_RTC_START_DAY, &v))    out->start_day    = v;
  if (get_u8(s, g, GBF_RTC_START_HOUR, &v))   out->start_hour   = v;
  if (get_u8(s, g, GBF_RTC_START_MINUTE, &v)) out->start_minute = v;
  if (get_u8(s, g, GBF_RTC_START_SECOND, &v)) out->start_second = v;

  uint32_t soff = gbf_off(g, GBF_RTC_SNAPSHOT);
  uint16_t slen = gbf_len(g, GBF_RTC_SNAPSHOT);
  if (soff && slen == 4) {
    GbSession* ncs = (GbSession*)(const void*)s;
    gbs_read_field(ncs, soff, out->rtc_snapshot, 4);   /* leaves zeros on failure */
  }

  if (get_u8(s, g, GBF_RTC_DST, &v)) out->dst = (v != 0);

  uint32_t goff = gbf_off(g, GBF_GAMETIME_HOURS);
  uint16_t glen = gbf_len(g, GBF_GAMETIME_HOURS);
  if (goff && glen == 2) {
    uint8_t b[2];
    GbSession* ncs = (GbSession*)(const void*)s;
    if (gbs_read_field(ncs, goff, b, 2) == GBS_OK)
      out->gametime_hours = (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
  }
  if (get_u8(s, g, GBF_GAMETIME_MINUTES, &v)) out->gametime_minutes = v;
  if (get_u8(s, g, GBF_GAMETIME_SECONDS, &v)) out->gametime_seconds = v;
  if (get_u8(s, g, GBF_GAMETIME_FRAMES, &v))  out->gametime_frames  = v;
  if (get_u8(s, g, GBF_GAMETIME_CAP, &v))     out->gametime_capped  = (v & 1u) != 0;

  if (get_u8(s, g, GBF_CUR_DAY, &v)) out->cur_day = v;

  out->status_flags_ok = get_u8(s, g, GBF_RTC_STATUS_FLAGS, &v);
  if (out->status_flags_ok) out->status_flags = v;

  return true;
}

/* ---- write ------------------------------------------------------------------------ */

GbsStatus gbc_write(GbSession* s, const GbClock* in, bool clear_status_flags) {
  if (!s || !in || !s->open) return GBS_ERR_ARG;

  GbGame g = gbc_game(s);
  if (!gbc_field_present(g, GBF_RTC_START_DAY)) return GBS_ERR_ARG;   /* Gen 1: N/A */

  bool changed = false;
  GbsStatus st;

  st = set_u8_unless_same(s, g, GBF_RTC_START_DAY, in->start_day, &changed);
  if (st != GBS_OK) return st;
  st = set_u8_unless_same(s, g, GBF_RTC_START_HOUR, in->start_hour, &changed);
  if (st != GBS_OK) return st;
  st = set_u8_unless_same(s, g, GBF_RTC_START_MINUTE, in->start_minute, &changed);
  if (st != GBS_OK) return st;
  st = set_u8_unless_same(s, g, GBF_RTC_START_SECOND, in->start_second, &changed);
  if (st != GBS_OK) return st;

  if (gbc_field_present(g, GBF_RTC_DST)) {
    /* Compare as a BOOL, not a raw byte: the corpus stores 0x80 for "on" (not a bare 1),
     * and gbc_read() only ever exposes `(v != 0)` -- comparing the wanted bool against
     * the CURRENT bool first means an untouched dst (in->dst still matches whatever
     * gbc_read filled it with) never gets its nonstandard stored byte normalised down
     * to 0/1 by an unrelated edit elsewhere in the same gbc_write() call (same class of
     * bug set_u_capped_unless_same's own comment in gb_trainer.c documents for money/
     * coins). Only an ACTUAL toggle writes a plain 1/0. */
    uint8_t cur = 0;
    bool cur_ok = get_u8(s, g, GBF_RTC_DST, &cur);
    if (!cur_ok || (cur != 0) != in->dst) {
      st = set_u8_unless_same(s, g, GBF_RTC_DST, in->dst ? 1u : 0u, &changed);
      if (st != GBS_OK) return st;
    }
  }

  if (clear_status_flags && gbc_field_present(g, GBF_RTC_STATUS_FLAGS)) {
    uint8_t cur = 0;
    if (get_u8(s, g, GBF_RTC_STATUS_FLAGS, &cur) && cur != 0) {
      st = set_u8_unless_same(s, g, GBF_RTC_STATUS_FLAGS, 0, &changed);
      if (st != GBS_OK) return st;
    }
  }

  /* rtc_snapshot, game time, gametime_capped, cur_day: never written -- see the
   * VIEW-ONLY notes in gb_clock.h. */

  return changed ? gbs_finish(s) : GBS_OK;
}
