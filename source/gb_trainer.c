#include "gb_trainer.h"

#include <string.h>

/* ---- game identification --------------------------------------------------- */

GbGame gbt_game(const GbSession* s) {
  if (!s) return GBF_G_RED;
  if (s->gen == GB_GEN1) return GBF_G_RED;   /* Yellow == Red/Blue layout, §1.10 */
  return (s->g2w.sv.version == G2_VER_CRYSTAL) ? GBF_G_CRYSTAL : GBF_G_GS;
}

bool gbt_field_present(GbGame game, GbField field) {
  return gbf_off(game, field) != 0;
}

/* ---- kind decode/encode ----------------------------------------------------
 * One generic path for every scalar field kind this slice touches. GBFK_TEXT is
 * handled separately (get_name/set_name) since it needs the GB-charset codec, not
 * an integer decode. */

static bool decode_u(GbFieldKind k, const uint8_t* b, uint16_t n, uint32_t* out) {
  switch (k) {
    case GBFK_BCD24BE: {
      uint32_t v = 0;
      for (uint16_t i = 0; i < n; i++) {
        uint8_t hi = (uint8_t)(b[i] >> 4), lo = (uint8_t)(b[i] & 0x0Fu);
        if (hi > 9 || lo > 9) return false;  /* not valid BCD -- refuse, do not guess */
        v = v * 100u + (uint32_t)hi * 10u + lo;
      }
      *out = v;
      return true;
    }
    case GBFK_U8:
    case GBFK_BITFIELD:
      *out = b[0];
      return true;
    case GBFK_U16BE:
      *out = ((uint32_t)b[0] << 8) | b[1];
      return true;
    case GBFK_U24BE:
      *out = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
      return true;
    default:
      return false;
  }
}

static void encode_u(GbFieldKind k, uint8_t* b, uint16_t n, uint32_t v) {
  switch (k) {
    case GBFK_BCD24BE:
      for (int i = (int)n - 1; i >= 0; i--) {
        uint32_t d = v % 100u;
        v /= 100u;
        b[i] = (uint8_t)(((d / 10u) << 4) | (d % 10u));
      }
      return;
    case GBFK_U16BE:
      b[0] = (uint8_t)(v >> 8);
      b[1] = (uint8_t)v;
      return;
    case GBFK_U24BE:
      b[0] = (uint8_t)(v >> 16);
      b[1] = (uint8_t)(v >> 8);
      b[2] = (uint8_t)v;
      return;
    case GBFK_U8:
    case GBFK_BITFIELD:
    default:
      b[0] = (uint8_t)v;
      return;
  }
}

static bool get_u(const GbSession* s, GbGame g, GbField f, uint32_t* out) {
  uint32_t off = gbf_off(g, f);
  uint16_t len = gbf_len(g, f);
  if (!off || !len || len > 4) return false;
  uint8_t b[4] = { 0, 0, 0, 0 };
  /* gbs_read_field is documented read-only ("nothing to corrupt") -- the const
   * this function takes is honest even though the shared struct type is not
   * itself marked const. */
  GbSession* ncs = (GbSession*)(const void*)s;
  if (gbs_read_field(ncs, off, b, len) != GBS_OK) return false;
  return decode_u(gbf_kind(g, f), b, len, out);
}

static GbsStatus set_u(GbSession* s, GbGame g, GbField f, uint32_t v) {
  uint32_t off = gbf_off(g, f);
  uint16_t len = gbf_len(g, f);
  if (!off || !len || len > 4) return GBS_ERR_ARG;
  uint8_t b[4] = { 0, 0, 0, 0 };
  encode_u(gbf_kind(g, f), b, len, v);
  return gbs_write_field(s, off, b, len);
}

/* ---- names ------------------------------------------------------------- */

static bool get_name(const GbSession* s, GbGame g, GbField f, uint8_t gen,
                      uint8_t raw[GB_NAME_BYTES], char* text, int text_cap) {
  uint32_t off = gbf_off(g, f);
  uint16_t len = gbf_len(g, f);
  if (!off || !len || len != GB_NAME_BYTES) return false;
  GbSession* ncs = (GbSession*)(const void*)s;
  if (gbs_read_field(ncs, off, raw, len) != GBS_OK) return false;
  gb_name_decode(gen, text, text_cap, raw, len);
  return true;
}

/* How many glyphs `s` spells, by the same per-glyph encoder set_name() commits
 * with -- gb_text_lossy only judges the first `max_glyphs` glyphs for CHARSET
 * loss, it does not itself refuse a string that has MORE glyphs than that (it
 * would just truncate); this is the separate "too long" check the brief asks
 * for ("refuse names over 7 glyphs"). */
static int count_glyphs(uint8_t gen, const char* s) {
  int n = 0;
  uint8_t out;
  while (*s) {
    int c = gb_char_encode(gen, s, &out);
    if (c <= 0) break;
    s += c;
    n++;
  }
  return n;
}

static GbsStatus set_name(GbSession* s, GbGame g, GbField f, uint8_t gen,
                          const char* text, int max_glyphs) {
  if (!text) return GBS_ERR_ARG;
  uint32_t off = gbf_off(g, f);
  uint16_t len = gbf_len(g, f);
  if (!off || !len || len != GB_NAME_BYTES) return GBS_ERR_ARG;
  if (count_glyphs(gen, text) > max_glyphs) return GBS_ERR_ARG;
  char bad[GB_GLYPH_MAX];
  if (gb_text_lossy(gen, text, max_glyphs, bad) > 0) return GBS_ERR_ARG;
  uint8_t enc[GB_NAME_BYTES];
  gb_name_encode(gen, enc, (int)len, max_glyphs, text);
  return gbs_write_field(s, off, enc, len);
}

/* ---- Pokedex popcounts (view-only) --------------------------------------- */

static bool get_dex_count(const GbSession* s, GbGame g, GbField f, uint16_t* out) {
  uint32_t off = gbf_off(g, f);
  uint16_t len = gbf_len(g, f);
  if (!off || !len || len > 32) return false;
  uint8_t buf[32];
  GbSession* ncs = (GbSession*)(const void*)s;
  if (gbs_read_field(ncs, off, buf, len) != GBS_OK) return false;
  int count = 0;
  for (uint16_t i = 0; i < len; i++) {
    uint8_t v = buf[i];
    while (v) { count += (int)(v & 1u); v >>= 1; }
  }
  *out = (uint16_t)count;
  return true;
}

/* ---- caps ----------------------------------------------------------------- */

static uint32_t clamp_u(uint32_t v, uint32_t cap) { return v > cap ? cap : v; }

#define GBT_MONEY_CAP 999999u
#define GBT_COINS_CAP 9999u

/* ---- read ------------------------------------------------------------------ */

bool gbt_read(const GbSession* s, GbTrainer* out) {
  if (!s || !out || !s->open) return false;
  memset(out, 0, sizeof *out);

  GbGame  g   = gbt_game(s);
  uint8_t gen = s->gen;

  if (!get_name(s, g, GBF_PLAYER_NAME, gen, out->name_raw, out->name, sizeof out->name))
    return false;

  uint32_t v;
  if (!get_u(s, g, GBF_TRAINER_ID, &v)) return false;
  out->trainer_id = (uint16_t)v;

  if (gbt_field_present(g, GBF_MONEY)) {
    if (!get_u(s, g, GBF_MONEY, &v)) return false;
    out->money = v;
  } else if (gbt_field_present(g, GBF_MONEY_BIN)) {
    if (!get_u(s, g, GBF_MONEY_BIN, &v)) return false;
    out->money = v;
  }

  if (gbt_field_present(g, GBF_COINS)) {
    if (!get_u(s, g, GBF_COINS, &v)) return false;
    out->coins = (uint16_t)v;
  } else if (gbt_field_present(g, GBF_COINS_BIN)) {
    if (!get_u(s, g, GBF_COINS_BIN, &v)) return false;
    out->coins = (uint16_t)v;
  }

  if (gbt_field_present(g, GBF_MOMS_MONEY)) {
    out->has_mom = true;
    if (get_u(s, g, GBF_MOMS_MONEY, &v)) out->moms_money = v;
    if (get_u(s, g, GBF_MOM_SAVING_FLAG, &v)) out->mom_saving = (v != 0);
  }

  if (gbt_field_present(g, GBF_BADGES) && get_u(s, g, GBF_BADGES, &v))
    out->badges = (uint8_t)v;
  if (gbt_field_present(g, GBF_BADGES_JOHTO) && get_u(s, g, GBF_BADGES_JOHTO, &v))
    out->badges_johto = (uint8_t)v;
  if (gbt_field_present(g, GBF_BADGES_KANTO) && get_u(s, g, GBF_BADGES_KANTO, &v))
    out->badges_kanto = (uint8_t)v;

  get_name(s, g, GBF_RIVAL_NAME, gen, out->rival_name_raw, out->rival_name,
           sizeof out->rival_name);   /* view-only; a false return leaves it blank */

  if (gbt_field_present(g, GBF_MOTHERS_NAME))
    out->has_mother = get_name(s, g, GBF_MOTHERS_NAME, gen, out->mothers_name_raw,
                               out->mothers_name, sizeof out->mothers_name);

  if (gbt_field_present(g, GBF_PLAYTIME_HOURS)) {
    if (get_u(s, g, GBF_PLAYTIME_HOURS, &v))   out->playtime.hours   = (uint16_t)v;
    if (get_u(s, g, GBF_PLAYTIME_MAXED, &v))   out->playtime.maxed   = (v != 0);
    if (get_u(s, g, GBF_PLAYTIME_MINUTES, &v)) out->playtime.minutes = (uint8_t)v;
    if (get_u(s, g, GBF_PLAYTIME_SECONDS, &v)) out->playtime.seconds = (uint8_t)v;
    if (get_u(s, g, GBF_PLAYTIME_FRAMES, &v))  out->playtime.frames  = (uint8_t)v;
  } else if (gbt_field_present(g, GBF_GAMETIME_HOURS)) {
    if (get_u(s, g, GBF_GAMETIME_HOURS, &v))   out->playtime.hours   = (uint16_t)v;
    if (get_u(s, g, GBF_GAMETIME_MINUTES, &v)) out->playtime.minutes = (uint8_t)v;
    if (get_u(s, g, GBF_GAMETIME_SECONDS, &v)) out->playtime.seconds = (uint8_t)v;
    if (get_u(s, g, GBF_GAMETIME_FRAMES, &v))  out->playtime.frames  = (uint8_t)v;
  }

  if (gbt_field_present(g, GBF_GENDER) && get_u(s, g, GBF_GENDER, &v)) {
    out->has_gender = true;
    out->gender = (uint8_t)(v & 1u);
  }

  get_dex_count(s, g, GBF_DEX_OWNED, &out->dex_owned);
  get_dex_count(s, g, GBF_DEX_SEEN,  &out->dex_seen);

  return true;
}

/* ---- write ------------------------------------------------------------------ */

GbsStatus gbt_write(GbSession* s, const GbTrainer* in) {
  if (!s || !in || !s->open) return GBS_ERR_ARG;

  GbGame  g   = gbt_game(s);
  uint8_t gen = s->gen;
  GbsStatus st;

  st = set_name(s, g, GBF_PLAYER_NAME, gen, in->name, GB_OT_GLYPHS);
  if (st != GBS_OK) return st;

  st = set_u(s, g, GBF_TRAINER_ID, in->trainer_id);
  if (st != GBS_OK) return st;

  uint32_t money = clamp_u(in->money, GBT_MONEY_CAP);
  if (gbt_field_present(g, GBF_MONEY)) {
    st = set_u(s, g, GBF_MONEY, money);
    if (st != GBS_OK) return st;
  } else if (gbt_field_present(g, GBF_MONEY_BIN)) {
    st = set_u(s, g, GBF_MONEY_BIN, money);
    if (st != GBS_OK) return st;
  }

  uint32_t coins = clamp_u(in->coins, GBT_COINS_CAP);
  if (gbt_field_present(g, GBF_COINS)) {
    st = set_u(s, g, GBF_COINS, coins);
    if (st != GBS_OK) return st;
  } else if (gbt_field_present(g, GBF_COINS_BIN)) {
    st = set_u(s, g, GBF_COINS_BIN, coins);
    if (st != GBS_OK) return st;
  }

  if (gbt_field_present(g, GBF_MOMS_MONEY)) {
    uint32_t mm = clamp_u(in->moms_money, GBT_MONEY_CAP);
    st = set_u(s, g, GBF_MOMS_MONEY, mm);
    if (st != GBS_OK) return st;
    if (gbt_field_present(g, GBF_MOM_SAVING_FLAG)) {
      st = set_u(s, g, GBF_MOM_SAVING_FLAG, in->mom_saving ? 1u : 0u);
      if (st != GBS_OK) return st;
    }
  }

  if (gbt_field_present(g, GBF_BADGES)) {
    st = set_u(s, g, GBF_BADGES, in->badges);
    if (st != GBS_OK) return st;
  }
  if (gbt_field_present(g, GBF_BADGES_JOHTO)) {
    st = set_u(s, g, GBF_BADGES_JOHTO, in->badges_johto);
    if (st != GBS_OK) return st;
  }
  if (gbt_field_present(g, GBF_BADGES_KANTO)) {
    st = set_u(s, g, GBF_BADGES_KANTO, in->badges_kanto);
    if (st != GBS_OK) return st;
  }

  if (gbt_field_present(g, GBF_PLAYTIME_HOURS)) {
    uint32_t hrs = in->playtime.hours > 255u ? 255u : in->playtime.hours;
    st = set_u(s, g, GBF_PLAYTIME_HOURS, hrs);
    if (st != GBS_OK) return st;
    st = set_u(s, g, GBF_PLAYTIME_MAXED, in->playtime.maxed ? 1u : 0u);
    if (st != GBS_OK) return st;
    st = set_u(s, g, GBF_PLAYTIME_MINUTES, in->playtime.minutes);
    if (st != GBS_OK) return st;
    st = set_u(s, g, GBF_PLAYTIME_SECONDS, in->playtime.seconds);
    if (st != GBS_OK) return st;
    st = set_u(s, g, GBF_PLAYTIME_FRAMES, in->playtime.frames);
    if (st != GBS_OK) return st;
  } else if (gbt_field_present(g, GBF_GAMETIME_HOURS)) {
    st = set_u(s, g, GBF_GAMETIME_HOURS, in->playtime.hours);
    if (st != GBS_OK) return st;
    st = set_u(s, g, GBF_GAMETIME_MINUTES, in->playtime.minutes);
    if (st != GBS_OK) return st;
    st = set_u(s, g, GBF_GAMETIME_SECONDS, in->playtime.seconds);
    if (st != GBS_OK) return st;
    st = set_u(s, g, GBF_GAMETIME_FRAMES, in->playtime.frames);
    if (st != GBS_OK) return st;
  }

  /* rival name, mother's name, gender, dex counts: never written -- see the
   * scope note in gb_trainer.h. */

  return gbs_finish(s);
}
