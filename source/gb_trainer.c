#include "gb_trainer.h"

#include <string.h>

/* ---- caps ------------------------------------------------------------------
 * Declared up top: set_u_capped_unless_same() (below) needs clamp_u() before any
 * of the get/set/encode helpers that follow it. */

static uint32_t clamp_u(uint32_t v, uint32_t cap) { return v > cap ? cap : v; }

#define GBT_MONEY_CAP 999999u
#define GBT_COINS_CAP 9999u

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

/* P1a review D1 (extended to every scalar field, not just money/coins): compares
 * against the field's CURRENT value first and skips the write entirely when it
 * already matches, setting `*changed` only on a write that actually happened.
 * gbt_write() uses this to know whether it may skip gbs_finish() altogether --
 * see that function's own comment for why "nothing changed" must mean "not even
 * the stored checksums move" on a Gen-2 save whose backup was already stale
 * before gbt_write ever ran (Gold.sav's own corpus state, docs/GEN12-PARITY-
 * DESIGN.md Appendix A: "backup ... stored AEF9 calc C03D BAD"). */
static GbsStatus set_u_unless_same(GbSession* s, GbGame g, GbField f, uint32_t new_v,
                                   bool* changed) {
  uint32_t cur;
  if (get_u(s, g, f, &cur) && cur == new_v) return GBS_OK;
  GbsStatus st = set_u(s, g, f, new_v);
  if (st == GBS_OK) *changed = true;
  return st;
}

/* P1a re-verify D3: set_u_unless_same() above is wrong for a CAPPED field when the
 * caller passes an already-clamped `new_v` -- it then compares the field's RAW
 * stored value against the CLAMPED target, which never matches when the stored
 * value is itself over the cap (a hex-edited save, or simply a byte the game never
 * enforces its own visible limit on: measured on Gold.sav, editing only a badge
 * rewrote money 0xABCDEF -> 0x0F423F, coins 0xFFFF -> 0x270F, mom's money
 * 0xABCDEF -> 0x0F423F). This is the function every capped field must go through
 * instead: compare the RAW `want` against the RAW stored value FIRST, and only
 * clamp on the way OUT, once a real change is already known to be happening. An
 * untouched over-cap field (want == cur, both uncapped) is therefore left exactly
 * as found, never silently rewritten down to the cap by an unrelated edit. */
static GbsStatus set_u_capped_unless_same(GbSession* s, GbGame g, GbField f,
                                          uint32_t want, uint32_t cap, bool* changed) {
  uint32_t cur;
  if (get_u(s, g, f, &cur) && cur == want) return GBS_OK;
  GbsStatus st = set_u(s, g, f, clamp_u(want, cap));
  if (st == GBS_OK) *changed = true;
  return st;
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

/* P1a review D1 (blocking): a no-op read -> write must not touch a single byte of
 * the name field. gb_name_encode always emits a FULL GB_NAME_BYTES record, 0x50-fill
 * and all -- it has no way to know what the game itself left after the terminator
 * (a previous, longer name's leftover glyphs; the corpus shows 8 stray bytes on
 * Red, 7 on Yellow, 3 on Gold/Crystal). Calling set_name() unconditionally on every
 * write therefore replaces that residue with fresh 0x50 fill even when the DECODED
 * text did not change -- which also means a corpus name that happens to decode to
 * more than max_glyphs glyphs (only possible from that same stray-byte residue,
 * never from a name the game itself wrote) made gbt_write() refuse EVERY edit,
 * including ones that never touched the name (D5). Comparing the DECODED text
 * (not the raw bytes) against `text` before ever calling set_name fixes both: an
 * unchanged name is a true no-op, and a corpus record whose residue count_glyphs
 * would reject is never even offered to it. */
static GbsStatus set_name_if_changed(GbSession* s, GbGame g, GbField f, uint8_t gen,
                                     const char* text, int max_glyphs, bool* changed) {
  uint32_t off = gbf_off(g, f);
  uint16_t len = gbf_len(g, f);
  if (off && len == GB_NAME_BYTES) {
    uint8_t cur[GB_NAME_BYTES];
    if (gbs_read_field(s, off, cur, len) == GBS_OK) {
      char cur_text[GB_TEXT_MAX];
      gb_name_decode(gen, cur_text, sizeof cur_text, cur, len);
      if (strcmp(cur_text, text) == 0) return GBS_OK;   /* unchanged: never rewrite */
    }
  }
  GbsStatus st = set_name(s, g, f, gen, text, max_glyphs);
  if (st == GBS_OK) *changed = true;
  return st;
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

  /* P1a review D4: an unreadable money/coins field (Gen-1 BCD with a non-decimal
   * nibble -- get_u/decode_u's own "refuse, do not guess") must not fail the WHOLE
   * card, contradicting this header's own documented contract ("every other field
   * defaults to zero ... rather than causing a hard failure"). money_ok/coins_ok
   * false means "P1b: show '?'  here, gbt_write: never touch this row". */
  if (gbt_field_present(g, GBF_MONEY)) {
    out->money_ok = get_u(s, g, GBF_MONEY, &v);
    if (out->money_ok) out->money = v;
  } else if (gbt_field_present(g, GBF_MONEY_BIN)) {
    out->money_ok = get_u(s, g, GBF_MONEY_BIN, &v);
    if (out->money_ok) out->money = v;
  }

  if (gbt_field_present(g, GBF_COINS)) {
    out->coins_ok = get_u(s, g, GBF_COINS, &v);
    if (out->coins_ok) out->coins = (uint16_t)v;
  } else if (gbt_field_present(g, GBF_COINS_BIN)) {
    out->coins_ok = get_u(s, g, GBF_COINS_BIN, &v);
    if (out->coins_ok) out->coins = (uint16_t)v;
  }

  if (gbt_field_present(g, GBF_MOMS_MONEY)) {
    out->has_mom = true;
    if (get_u(s, g, GBF_MOMS_MONEY, &v)) out->moms_money = v;
    if (get_u(s, g, GBF_MOM_SAVING_FLAG, &v)) {
      out->mom_saving_bits = (uint8_t)(v & 0x07u);
      out->mom_active      = (v & 0x80u) != 0;
    }
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
    /* P1a review D6: Gen 2's own maxed-out flag, GBF_GAMETIME_CAP bit 0 -- NOT
     * always false the way this struct used to claim. */
    if (get_u(s, g, GBF_GAMETIME_CAP, &v))     out->playtime.maxed   = (v & 1u) != 0;
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

  /* BACKLOG #96 D10 / #126b: STATUSFLAGS_POKEDEX_F is bit 0 of GBF_STATUS_FLAGS
   * (constants/ram_constants.asm) -- Gen 2 only, ABSENT (and so left at its
   * memset(0)/false default) on Gen 1, same posture as has_gender above.
   * Gen 2 fails OPEN, not closed: default true (the field is present on
   * every real Gen-2 save), and clear it ONLY on a SUCCESSFUL read whose
   * bit 0 is 0 -- a failed get_u() on a field that IS present (a transient
   * read glitch, not "this game lacks it") must never silently hide a real
   * save's #DEX row the way the old unconditional-false default did. */
  if (gen == GB_GEN2) out->has_pokedex = true;
  if (gbt_field_present(g, GBF_STATUS_FLAGS) && get_u(s, g, GBF_STATUS_FLAGS, &v))
    out->has_pokedex = (v & 1u) != 0;

  return true;
}

/* ---- write ------------------------------------------------------------------ */

GbsStatus gbt_write(GbSession* s, const GbTrainer* in) {
  if (!s || !in || !s->open) return GBS_ERR_ARG;

  GbGame  g   = gbt_game(s);
  uint8_t gen = s->gen;
  GbsStatus st;
  /* P1a review D1: true only once a field write ACTUALLY landed. gbs_finish() is
   * skipped entirely when this stays false -- a Gen-2 g2w_finish() unconditionally
   * recomputes and re-stores BOTH checksums, and Gold.sav's own corpus state
   * proves that is not a no-op even when every field write above was itself
   * skipped: its real backup is already out of sync with its primary in a region
   * this slice never touches (Appendix A: "253 bytes differ (stale)"), so a
   * checksum refresh over CURRENT bytes produces a value that differs from what
   * is already stored -- a pure read/write round trip must never move that byte
   * either. */
  bool changed = false;

  st = set_name_if_changed(s, g, GBF_PLAYER_NAME, gen, in->name, GB_OT_GLYPHS, &changed);
  if (st != GBS_OK) return st;

  st = set_u_unless_same(s, g, GBF_TRAINER_ID, in->trainer_id, &changed);
  if (st != GBS_OK) return st;

  /* P1a review D3 (re-verified, previous fix was wrong -- see
   * set_u_capped_unless_same's own comment): compare against the field's CURRENT
   * RAW value BEFORE clamping, via set_u_capped_unless_same, not set_u_unless_same
   * with a pre-clamped argument. An untouched field (in->money already equal to
   * what is on disk, even if that stored value is itself over the cap: a
   * hand-edited save, or the design's own §1.1 caveat that the byte holds 0..255
   * with only the game enforcing the visible limit) must never be silently
   * clamped down by an unrelated edit elsewhere on the same gbt_write() call. D4:
   * money_ok/coins_ok false means the field was not readable at all -- leave it
   * untouched, never write the zero gbt_read() defaulted it to. */
  if (in->money_ok && gbt_field_present(g, GBF_MONEY)) {
    st = set_u_capped_unless_same(s, g, GBF_MONEY, in->money, GBT_MONEY_CAP, &changed);
    if (st != GBS_OK) return st;
  } else if (in->money_ok && gbt_field_present(g, GBF_MONEY_BIN)) {
    st = set_u_capped_unless_same(s, g, GBF_MONEY_BIN, in->money, GBT_MONEY_CAP, &changed);
    if (st != GBS_OK) return st;
  }

  if (in->coins_ok && gbt_field_present(g, GBF_COINS)) {
    st = set_u_capped_unless_same(s, g, GBF_COINS, in->coins, GBT_COINS_CAP, &changed);
    if (st != GBS_OK) return st;
  } else if (in->coins_ok && gbt_field_present(g, GBF_COINS_BIN)) {
    st = set_u_capped_unless_same(s, g, GBF_COINS_BIN, in->coins, GBT_COINS_CAP, &changed);
    if (st != GBS_OK) return st;
  }

  if (gbt_field_present(g, GBF_MOMS_MONEY)) {
    st = set_u_capped_unless_same(s, g, GBF_MOMS_MONEY, in->moms_money, GBT_MONEY_CAP,
                                  &changed);
    if (st != GBS_OK) return st;
    if (gbt_field_present(g, GBF_MOM_SAVING_FLAG)) {
      /* Read-modify-write bits 0-2 and bit 7 ONLY (P1a review D7: this is not a
       * single bool -- pokegold/pokecrystal define MOM_SAVING_SOME/HALF/ALL_MONEY_F
       * at bits 0/1/2 and MOM_ACTIVE_F at bit 7). Bits 3-6 are undocumented; the
       * corpus shows this byte holding other set bits (Gold.sav: 0x81) that predate
       * this field table, so they are preserved rather than guessed at. The review
       * that asked for this confirmed it is load-bearing: a whole-byte write here
       * would have cleared MOM_ACTIVE_F -- the player's bank account going inactive
       * -- on every commit that touched anything else on the trainer card. */
      uint32_t cur = 0;
      if (!get_u(s, g, GBF_MOM_SAVING_FLAG, &cur)) return GBS_ERR_ARG;
      uint32_t next = (cur & ~(uint32_t)0x87u) | (in->mom_saving_bits & 0x07u) |
                     (in->mom_active ? 0x80u : 0u);
      if (next != cur) {
        st = set_u(s, g, GBF_MOM_SAVING_FLAG, next);
        if (st != GBS_OK) return st;
        changed = true;
      }
    }
  }

  if (gbt_field_present(g, GBF_BADGES)) {
    st = set_u_unless_same(s, g, GBF_BADGES, in->badges, &changed);
    if (st != GBS_OK) return st;
  }
  if (gbt_field_present(g, GBF_BADGES_JOHTO)) {
    st = set_u_unless_same(s, g, GBF_BADGES_JOHTO, in->badges_johto, &changed);
    if (st != GBS_OK) return st;
  }
  if (gbt_field_present(g, GBF_BADGES_KANTO)) {
    st = set_u_unless_same(s, g, GBF_BADGES_KANTO, in->badges_kanto, &changed);
    if (st != GBS_OK) return st;
  }

  if (gbt_field_present(g, GBF_PLAYTIME_HOURS)) {
    /* P1a review D10 (refined alongside re-verify D3): retail writes 0xFF to
     * wPlayTimeMaxed, not a bare 1 (pokered/engine/play_time.asm:34-35). Forcing
     * maxed=true must happen only when the HOURS WRITE ITSELF is what is being
     * clamped (a caller passing more than the byte holds) -- not whenever the
     * field's raw stored value already happens to read back over the cap (an
     * inconsistent/hex-edited state this call was never asked to touch), or a
     * pure no-op read->write would flip the maxed byte purely because of what
     * was already on disk -- the same class of bug D3 fixed for money/coins/
     * mom's money. `hours_unchanged` is computed the same way
     * set_u_capped_unless_same() judges it internally. */
    uint32_t cur_hours;
    bool hours_unchanged = get_u(s, g, GBF_PLAYTIME_HOURS, &cur_hours) &&
                           cur_hours == in->playtime.hours;
    bool maxed = in->playtime.maxed || (!hours_unchanged && in->playtime.hours > 255u);
    st = set_u_capped_unless_same(s, g, GBF_PLAYTIME_HOURS, in->playtime.hours, 255u,
                                  &changed);
    if (st != GBS_OK) return st;
    /* P1a re-verify D12: preserve whatever NONZERO byte is already stored -- the game
     * only tests nonzero (pokered/engine/play_time.asm `and a; ret nz`), so normalising
     * a stored 0x01 to 0xFF would rewrite a row this call was never asked to touch. */
    uint32_t cur_maxed = 0;
    bool had_maxed = get_u(s, g, GBF_PLAYTIME_MAXED, &cur_maxed) && cur_maxed != 0;
    uint32_t maxed_byte = maxed ? (had_maxed ? cur_maxed : 0xFFu) : 0u;
    st = set_u_unless_same(s, g, GBF_PLAYTIME_MAXED, maxed_byte, &changed);
    if (st != GBS_OK) return st;
    st = set_u_unless_same(s, g, GBF_PLAYTIME_MINUTES, in->playtime.minutes, &changed);
    if (st != GBS_OK) return st;
    st = set_u_unless_same(s, g, GBF_PLAYTIME_SECONDS, in->playtime.seconds, &changed);
    if (st != GBS_OK) return st;
    st = set_u_unless_same(s, g, GBF_PLAYTIME_FRAMES, in->playtime.frames, &changed);
    if (st != GBS_OK) return st;
  } else if (gbt_field_present(g, GBF_GAMETIME_HOURS)) {
    /* P1a review D6: Gen 2's own maxed flag, GBF_GAMETIME_CAP bit 0 -- read-modify-
     * write, same discipline as GBF_MOM_SAVING_FLAG below: this byte carries only
     * one documented bit (GAME_TIME_CAPPED = 0) but nothing rules out the games
     * using the rest for scratch, so only bit 0 is ever touched. Clamped to 999
     * (this struct's own on-screen budget, not a value the design doc cites). */
    uint32_t cur_ghours;
    bool ghours_unchanged = get_u(s, g, GBF_GAMETIME_HOURS, &cur_ghours) &&
                            cur_ghours == in->playtime.hours;
    bool maxed = in->playtime.maxed || (!ghours_unchanged && in->playtime.hours > 999u);
    st = set_u_capped_unless_same(s, g, GBF_GAMETIME_HOURS, in->playtime.hours, 999u,
                                  &changed);
    if (st != GBS_OK) return st;
    if (gbt_field_present(g, GBF_GAMETIME_CAP)) {
      uint32_t cur = 0;
      if (!get_u(s, g, GBF_GAMETIME_CAP, &cur)) return GBS_ERR_ARG;
      uint32_t next = (cur & ~1u) | (maxed ? 1u : 0u);
      if (next != cur) {
        st = set_u(s, g, GBF_GAMETIME_CAP, next);
        if (st != GBS_OK) return st;
        changed = true;
      }
    }
    st = set_u_unless_same(s, g, GBF_GAMETIME_MINUTES, in->playtime.minutes, &changed);
    if (st != GBS_OK) return st;
    st = set_u_unless_same(s, g, GBF_GAMETIME_SECONDS, in->playtime.seconds, &changed);
    if (st != GBS_OK) return st;
    st = set_u_unless_same(s, g, GBF_GAMETIME_FRAMES, in->playtime.frames, &changed);
    if (st != GBS_OK) return st;
  }

  /* rival name, mother's name, gender, dex counts: never written -- see the
   * scope note in gb_trainer.h. */

  return changed ? gbs_finish(s) : GBS_OK;
}
