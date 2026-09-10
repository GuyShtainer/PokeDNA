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
  if (get_u8(s, g, GBF_RTC_START_DAY, &v))    out->rtc_offset_day    = v;
  if (get_u8(s, g, GBF_RTC_START_HOUR, &v))   out->rtc_offset_hour   = v;
  if (get_u8(s, g, GBF_RTC_START_MINUTE, &v)) out->rtc_offset_minute = v;
  if (get_u8(s, g, GBF_RTC_START_SECOND, &v)) out->rtc_offset_second = v;

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

  if (get_u8(s, g, GBF_CUR_DAY, &v)) out->day_count = v;

  out->status_flags_ok = get_u8(s, g, GBF_RTC_STATUS_FLAGS, &v);
  if (out->status_flags_ok) out->status_flags = v;

  return true;
}

/* ---- shift ------------------------------------------------------------------------ */

/* Adds `delta` to `cur` modulo `mod` (mod > 0), returning the wrapped result and
 * leaving the whole (possibly negative or >=1) quotient in *carry -- the same
 * borrow/carry shape FixTime's own addition chain uses, generalised to signed deltas
 * so a shift can move the clock backward too. */
static uint8_t wrap_add(int32_t cur, int32_t delta, int32_t mod, int32_t* carry) {
  int32_t v = cur + delta;
  int32_t c = 0;
  while (v < 0)    { v += mod; c--; }
  while (v >= mod) { v -= mod; c++; }
  *carry = c;
  return (uint8_t)v;
}

GbsStatus gbc_shift(GbSession* s, int32_t d_days, int32_t d_hours, int32_t d_minutes,
                    int32_t d_seconds) {
  if (!s || !s->open) return GBS_ERR_ARG;
  GbGame g = gbc_game(s);
  if (!gbc_field_present(g, GBF_RTC_START_DAY)) return GBS_ERR_ARG;   /* Gen 1: N/A */

  uint8_t cur_day, cur_hour, cur_min, cur_sec;
  if (!get_u8(s, g, GBF_RTC_START_DAY, &cur_day))    return GBS_ERR_ARG;
  if (!get_u8(s, g, GBF_RTC_START_HOUR, &cur_hour))  return GBS_ERR_ARG;
  if (!get_u8(s, g, GBF_RTC_START_MINUTE, &cur_min)) return GBS_ERR_ARG;
  if (!get_u8(s, g, GBF_RTC_START_SECOND, &cur_sec)) return GBS_ERR_ARG;

  int32_t carry;
  uint8_t new_sec = wrap_add(cur_sec, d_seconds, 60, &carry);
  d_minutes += carry;
  uint8_t new_min = wrap_add(cur_min, d_minutes, 60, &carry);
  d_hours += carry;
  uint8_t new_hour = wrap_add(cur_hour, d_hours, 24, &carry);
  d_days += carry;
  uint8_t new_day = wrap_add(cur_day, d_days, 256, &carry);   /* the byte's own width */

  bool changed = false;
  GbsStatus st;
  st = set_u8_unless_same(s, g, GBF_RTC_START_DAY, new_day, &changed);
  if (st != GBS_OK) return st;
  st = set_u8_unless_same(s, g, GBF_RTC_START_HOUR, new_hour, &changed);
  if (st != GBS_OK) return st;
  st = set_u8_unless_same(s, g, GBF_RTC_START_MINUTE, new_min, &changed);
  if (st != GBS_OK) return st;
  st = set_u8_unless_same(s, g, GBF_RTC_START_SECOND, new_sec, &changed);
  if (st != GBS_OK) return st;

  return changed ? gbs_finish(s) : GBS_OK;
}

/* ---- reset / clear ----------------------------------------------------------------- */

#define RTC_RESET_BIT 0x80u   /* pokecrystal constants/ram_constants.asm: RTC_RESET, bit 7 */

GbsStatus gbc_request_time_reset(GbSession* s) {
  if (!s || !s->open) return GBS_ERR_ARG;
  GbGame g = gbc_game(s);
  if (!gbc_field_present(g, GBF_RTC_START_DAY)) return GBS_ERR_ARG;   /* Gen 1: N/A */
  if (!gbc_field_present(g, GBF_RTC_STATUS_FLAGS)) return GBS_ERR_ARG;

  uint8_t cur = 0;
  if (get_u8(s, g, GBF_RTC_STATUS_FLAGS, &cur) && cur == RTC_RESET_BIT) return GBS_OK;

  bool changed = false;
  GbsStatus st = set_u8_unless_same(s, g, GBF_RTC_STATUS_FLAGS, RTC_RESET_BIT, &changed);
  if (st != GBS_OK) return st;
  return changed ? gbs_finish(s) : GBS_OK;
}

GbsStatus gbc_clear_status_flags(GbSession* s) {
  if (!s || !s->open) return GBS_ERR_ARG;
  GbGame g = gbc_game(s);
  if (!gbc_field_present(g, GBF_RTC_START_DAY)) return GBS_ERR_ARG;   /* Gen 1: N/A */
  if (!gbc_field_present(g, GBF_RTC_STATUS_FLAGS)) return GBS_OK;     /* nothing to clear */

  uint8_t cur = 0;
  if (!get_u8(s, g, GBF_RTC_STATUS_FLAGS, &cur) || cur == 0) return GBS_OK;

  bool changed = false;
  GbsStatus st = set_u8_unless_same(s, g, GBF_RTC_STATUS_FLAGS, 0, &changed);
  if (st != GBS_OK) return st;
  return changed ? gbs_finish(s) : GBS_OK;
}
