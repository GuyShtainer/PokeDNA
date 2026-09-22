#include "gen3_mon.h"
#include "gen3_save.h"   /* gen3_decode_char (pure) */
#include <string.h>

/* --- little-endian readers (saves are LE) --- */
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Substruct slot order by personality % 24 — [Growth, Attacks, EVs]; the Misc
 * slot is the remaining one (slots sum to 0+1+2+3 = 6). Same table as
 * gen3_save.c, validated bit-exact against real saves. */
static const uint8_t k_substruct_pos[24][3] = {
  {0,1,2},{0,1,3},{0,2,1},{0,3,1},{0,2,3},{0,3,2},
  {1,0,2},{1,0,3},{2,0,1},{3,0,1},{2,0,3},{3,0,2},
  {1,2,0},{1,3,0},{2,1,0},{3,1,0},{2,3,0},{3,2,0},
  {1,2,3},{1,3,2},{2,1,3},{3,1,2},{2,3,1},{3,2,1},
};

/* BACKLOG #183: ♂ (Gen-3 0xB5) / ♀ (0xB6) need 3 UTF-8 bytes ("\xE2\x99\x82" /
 * "\xE2\x99\x80"), not the 1 gen3_decode_char's own `char` return can carry -- that
 * function stays untouched (it has ~15 other callers throughout this tree, several
 * outside this fix's scope, all assuming its existing one-byte-in/one-char-out
 * shape; widening ITS signature would ripple into every one of them). This is the
 * ONE caller that matters for round-trip correctness: PkMon.nickname/otName feed
 * gen3_to_gb.c's BACKLOG #177 spelling-compare (gb_text_lossy), so a NIDORAN♀
 * caught in Gen 3 must decode back to a real ♀, not '?', or a lossless transfer
 * gets falsely flagged lossy. Same "tree's own representation" encode_name()
 * (gen3_edit.c) and gen1_save.c's own GB decoder already use for this exact glyph
 * pair -- not a new encoding.
 *
 * `outcap` is `out`'s real capacity (NOT `maxlen`, the Gen-3 byte count) because 3
 * output bytes for 1 input byte can outrun a fixed-size field -- PkMon.nickname[11]/
 * otName[8] (gen3_mon.h) have zero slack today, so this caller must be its own
 * upper bound, not lean on the caller's field-size intuition. A corrupt/crafted
 * save COULD have every glyph be a gender sign; degrading to gen3_decode_char's
 * plain '?' once the buffer is nearly full is a safe, bounded fallback -- never a
 * silent overflow (golden rule 2: every loop proves its own bound).
 *
 * BACKLOG #216: 0x1B (lowercase e-acute, pokeemerald charmap.txt) gets the SAME
 * treatment for the SAME reason -- it needs the 2-byte UTF-8 sequence "\xC3\xA9",
 * which is exactly the spelling source/ui.c's own pnext() already special-cases at
 * font code 127 (the ONE non-ASCII glyph PokeDNA's font can draw), so a decoded
 * name carrying it renders correctly instead of falling to gen3_decode_char's
 * plain '?'. The other 39 codes in the accented block (0x01-0x2B minus 0x1B) have
 * no glyph in this app's font at all and stay '?' -- this app's font has no
 * glyph for them, so they would render as '?' anyway -- a DISPLAY limit, not a
 * data one. BACKLOG #216b: the umlauts F1-F6 and 0xB9 '×' get the SAME
 * treatment below (decode_2byte_accent) -- they have UTF-8 spellings gb_edit.c's
 * enc_one already round-trips, so decoding them preserves them across the GB
 * bridge; ui_ascii.c's pnext() has NO glyph slot for them (unlike é at font
 * code 127), so they still render '?' on screen -- a display limit, not a data
 * one, the same as the other 39. decode_name stays the only place that owns
 * ALL of these special cases -- one thin function, not duplicated logic. */
/* BACKLOG #217: `degraded` (may be NULL) is set true when a multi-byte glyph (the
 * gender sign or e-acute) hit the `oi + N < outcap` bound above and fell through
 * to gen3_decode_char(b) instead -- which has no case for 0xB5/0xB6/0x1B, so that
 * fallthrough is ALWAYS a literal '?' standing in for a real glyph that just
 * didn't fit, never a genuine '?' byte (0xAC) typed by a player: gen3_decode_char
 * DOES have a case for 0xAC, so 0xAC never reaches this fallthrough at all --
 * `b` here is provably always one of the three multi-byte codes. Never set false
 * once true within one call: a caller passes a fresh bool (pk_decode_mon zeroes
 * the whole PkMon first), so "true" only ever means "this field's whole decoded
 * string is a fresh output, and truncation was in it." */
/* BACKLOG #216b: the umlauts (0xF1-0xF6, pokeemerald charmap.txt: Ä Ö Ü ä ö ü) and ×
 * (0xB9) get the SAME 2-byte-UTF-8-insertion treatment as 0x1B (e-acute) above, for the
 * same reason -- gen3_decode_char's `char` return cannot carry 2 output bytes. Every
 * one of these 7 spellings shares the "\xC3" lead byte, so one small table plus the
 * existing bound check covers all seven with no new control-flow shape. This is what
 * closes the GB bridge for names carrying them (gb_edit.c's enc_one already round-trips
 * these same spellings to Gen 1/2 bytes -- see gen1_save.c:99, gen2_save.c's g2_glyph). */
static bool decode_2byte_accent(uint8_t b, char out2[2]) {
  static const struct { uint8_t code, lo; } k[] = {
    { 0xF1u, 0x84u },  /* Ä */ { 0xF2u, 0x96u },  /* Ö */ { 0xF3u, 0x9Cu },  /* Ü */
    { 0xF4u, 0xA4u },  /* ä */ { 0xF5u, 0xB6u },  /* ö */ { 0xF6u, 0xBCu },  /* ü */
    { 0xB9u, 0x97u },  /* × (U+00D7), NOT the letter x */
  };
  for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
    if (k[i].code == b) { out2[0] = (char)0xC3; out2[1] = (char)k[i].lo; return true; }
  }
  return false;
}

static void decode_name(char* out, int outcap, const uint8_t* src, int maxlen, bool* degraded) {
  int k = 0, oi = 0;
  for (; k < maxlen; k++) {
    uint8_t b = src[k];
    if ((b == 0xB5u || b == 0xB6u) && oi + 3 < outcap) {
      out[oi++] = (char)0xE2; out[oi++] = (char)0x99;
      out[oi++] = (char)(b == 0xB5u ? 0x82 : 0x80);
      continue;
    }
    if (b == 0x1Bu && oi + 2 < outcap) {          /* e-acute -> pnext()'s own "\xC3\xA9" */
      out[oi++] = (char)0xC3; out[oi++] = (char)0xA9;
      continue;
    }
    char accent[2];
    if (decode_2byte_accent(b, accent) && oi + 2 < outcap) {
      out[oi++] = accent[0]; out[oi++] = accent[1];
      continue;
    }
    if ((b == 0xB5u || b == 0xB6u || b == 0x1Bu || decode_2byte_accent(b, accent)) && degraded)
      *degraded = true;
    char ch = gen3_decode_char(b);
    if (ch == 0) break;
    if (oi + 1 >= outcap) break;
    out[oi++] = ch;
  }
  out[oi] = 0;
}

/* BACKLOG #183: public wrapper so a caller outside this file (gen12_convert.c's
 * own spelling-compare, item 2(b)) can decode a raw Gen-3 name field -- nickname
 * (0x08, 10 bytes) or otName (0x14, 7 bytes) -- the exact same way pk_decode_mon
 * does, WITHOUT decrypting the whole 80-byte record first (both fields sit
 * outside the encrypted substructs, so this is safe on a record mid-build, before
 * gen3_edit_commit's checksum/encrypt pass has even run). One thin function, no
 * logic duplicated: `decode_name` stays the only place that owns the gender-sign
 * special case. */
void gen3_decode_name(char* out, int outcap, const uint8_t* src, int maxlen) {
  decode_name(out, outcap, src, maxlen, NULL);
}

uint8_t pk_nature(uint32_t personality) { return (uint8_t)(personality % 25); }

/* Deoxys forme is NOT stored per-mon in Gen 3 — it's decided by the game version
 * (RS Normal / Emerald Speed / FireRed Attack / LeafGreen Defense). The app sets the
 * display forme for the loaded save here; pk_decode_mon then tags every Deoxys with it
 * so all display paths (box/party/summary/daycare) show the right forme. 0..3 =
 * Normal/Attack/Defense/Speed. Defaults to 0 (Normal) — the host tests never set it. */
static uint8_t s_deoxys_form = 0;
void pk_set_deoxys_form(int f) { s_deoxys_form = (f >= 0 && f < 4) ? (uint8_t)f : 0; }
int  pk_get_deoxys_form(void) { return s_deoxys_form; }

/* Unown letter index 0..27 (A..Z, !, ?) — 2 bits from each PID byte, mod 28. */
uint8_t pk_unown_form(uint32_t personality) {
  uint32_t v = ((personality & 0x03000000u) >> 18) | ((personality & 0x00030000u) >> 12)
             | ((personality & 0x00000300u) >> 6)  | (personality & 0x00000003u);
  return (uint8_t)(v % 28u);
}

bool pk_is_shiny(uint32_t personality, uint16_t tid, uint16_t sid) {
  uint16_t lo = (uint16_t)(personality & 0xFFFF);
  uint16_t hi = (uint16_t)(personality >> 16);
  return (uint16_t)(tid ^ sid ^ lo ^ hi) < 8;
}

uint8_t pk_gender_from(uint32_t personality, uint8_t gender_ratio) {
  if (gender_ratio == 0xFF) return 2;          /* genderless */
  if (gender_ratio == 0xFE) return 1;          /* always female */
  if (gender_ratio == 0x00) return 0;          /* always male */
  return (gender_ratio > (uint8_t)(personality & 0xFF)) ? 1 : 0;
}

uint16_t pk_calc_hp(uint8_t base, uint8_t iv, uint8_t ev, uint8_t level) {
  uint32_t v = (((uint32_t)(2 * base + iv + ev / 4) * level) / 100) + level + 10;
  return (uint16_t)v;
}

uint16_t pk_calc_stat(uint8_t base, uint8_t iv, uint8_t ev, uint8_t level, int nature_mod) {
  uint32_t v = (((uint32_t)(2 * base + iv + ev / 4) * level) / 100) + 5;
  if (nature_mod > 0)      v = v * 110 / 100;
  else if (nature_mod < 0) v = v * 90 / 100;
  return (uint16_t)v;
}

bool pk_decode_mon(const uint8_t* mon, bool is_party, PkMon* out) {
  memset(out, 0, sizeof(*out));
  out->isParty = is_party;
  out->raw = mon;

  uint8_t flags = mon[0x13];                     /* BoxPokemon flags byte */
  out->isBadEgg = (flags & 0x01) != 0;

  /* Language is a PLAINTEXT byte at 0x12 (struct BoxPokemon: nickname[10] @0x08,
   * then u8 language, then the flags byte at 0x13 we just read). It is not part of
   * the encrypted substructs, so it needs no key. Eggs carry LANGUAGE_JAPANESE
   * regardless of cart language (daycare.c CreateEgg / SetInitialEggData), which is
   * what makes it a legality signal — see gen3_legality2.c. */
  out->language = mon[0x12];

  uint32_t pers = rd32(mon + 0x00);
  uint32_t otid = rd32(mon + 0x04);
  out->personality = pers;
  out->otId = otid;
  uint32_t key = pers ^ otid;

  uint8_t dec[48];
  for (int w = 0; w < 12; w++) {
    uint32_t word = rd32(mon + 0x20 + (uint32_t)w * 4) ^ key;
    dec[w * 4 + 0] = (uint8_t)word;
    dec[w * 4 + 1] = (uint8_t)(word >> 8);
    dec[w * 4 + 2] = (uint8_t)(word >> 16);
    dec[w * 4 + 3] = (uint8_t)(word >> 24);
  }

  uint16_t sum = 0;
  for (int h = 0; h < 24; h++) sum = (uint16_t)(sum + rd16(dec + h * 2));
  bool checksum_ok = (sum == rd16(mon + 0x1C));

  const uint8_t* pos = k_substruct_pos[pers % 24];
  int gslot = pos[0], aslot = pos[1], eslot = pos[2];
  int mslot = 6 - (gslot + aslot + eslot);
  const uint8_t* g = dec + (uint32_t)gslot * 12;   /* Growth  */
  const uint8_t* a = dec + (uint32_t)aslot * 12;   /* Attacks */
  const uint8_t* e = dec + (uint32_t)eslot * 12;   /* EVs     */
  const uint8_t* m = dec + (uint32_t)mslot * 12;   /* Misc    */

  uint16_t sp = rd16(g + 0);
  uint32_t ivword = rd32(m + 0x04);
  bool egg = ((flags & 0x04) != 0) || ((ivword >> 30) & 1);
  out->isEgg = egg;

  /* Empty slot: a zeroed record (pers/otid 0 → checksum_ok, species 0). */
  if (checksum_ok && sp == 0 && !egg) return false;

  /* Corrupt record → mark bad egg; fields below may be garbage but harmless. */
  if (!checksum_ok) out->isBadEgg = true;

  out->species     = sp;
  out->form        = (sp == 201) ? pk_unown_form(pers)        /* Unown letter (A..?) */
                   : (sp == 410) ? s_deoxys_form : 0;         /* Deoxys (internal 410; nat 386) forme, set from the game version */
  out->heldItem    = rd16(g + 0x02);
  out->experience  = rd32(g + 0x04);
  out->ppBonuses   = g[0x08];
  out->friendship  = g[0x09];
  for (int i = 0; i < 4; i++) { out->moves[i] = rd16(a + i * 2); out->pp[i] = a[0x08 + i]; }
  for (int i = 0; i < 6; i++) out->evs[i] = e[i];
  out->evSum = (uint16_t)(e[0] + e[1] + e[2] + e[3] + e[4] + e[5]);
  for (int i = 0; i < 6; i++) out->contest[i] = e[0x06 + i];

  out->pokerus     = m[0x00];
  out->metLocation = m[0x01];
  uint16_t origins = rd16(m + 0x02);
  out->metLevel = (uint8_t)(origins & 0x7F);
  out->metGame  = (uint8_t)((origins >> 7) & 0x0F);
  out->pokeball = (uint8_t)((origins >> 11) & 0x0F);
  out->otGender = (uint8_t)((origins >> 15) & 0x01);
  out->ivs[PK_HP]  = (uint8_t)(ivword & 0x1F);
  out->ivs[PK_ATK] = (uint8_t)((ivword >> 5) & 0x1F);
  out->ivs[PK_DEF] = (uint8_t)((ivword >> 10) & 0x1F);
  out->ivs[PK_SPE] = (uint8_t)((ivword >> 15) & 0x1F);
  out->ivs[PK_SPA] = (uint8_t)((ivword >> 20) & 0x1F);
  out->ivs[PK_SPD] = (uint8_t)((ivword >> 25) & 0x1F);
  out->abilityNum  = (uint8_t)((ivword >> 31) & 1);
  out->ribbons     = rd32(m + 0x08);

  out->nature  = pk_nature(pers);
  out->isShiny = pk_is_shiny(pers, (uint16_t)(otid & 0xFFFF), (uint16_t)(otid >> 16));

  /* BACKLOG #217: out->nameFlags starts 0 (pk_decode_mon's own memset above). */
  bool nickDegraded = false, otDegraded = false;
  decode_name(out->nickname, (int)sizeof out->nickname, mon + 0x08, 10, &nickDegraded);
  decode_name(out->otName,   (int)sizeof out->otName,   mon + 0x14, 7,  &otDegraded);
  if (nickDegraded) out->nameFlags |= PK_NAME_NICK_DEGRADED;
  if (otDegraded)   out->nameFlags |= PK_NAME_OT_DEGRADED;

  if (is_party) {
    out->level = mon[0x54];
    out->stats[PK_HP]  = rd16(mon + 0x58);
    out->stats[PK_ATK] = rd16(mon + 0x5A);
    out->stats[PK_DEF] = rd16(mon + 0x5C);
    out->stats[PK_SPE] = rd16(mon + 0x5E);
    out->stats[PK_SPA] = rd16(mon + 0x60);
    out->stats[PK_SPD] = rd16(mon + 0x62);
  }
  /* box records: level + stats are computed later (needs base stats / growth rate). */
  return true;
}

static int read_party_at(const uint8_t* sb1, uint16_t count_off, uint16_t party_off,
                         PkMon out[6], int* goodness) {
  uint8_t count = sb1[count_off];
  int good = 0, kept = 0;
  if (count == 0 || count > 6) { if (goodness) *goodness = -1; return 0; }
  for (int i = 0; i < count; i++) {
    PkMon m;
    if (!pk_decode_mon(sb1 + party_off + (uint32_t)i * 100, true, &m)) continue;
    out[kept++] = m;
    if (!m.isBadEgg && m.species >= 1 && m.species <= 411 && m.level >= 1 && m.level <= 100)
      good++;
  }
  if (goodness) *goodness = good;
  return kept;
}

int pk_read_party(const uint8_t* sb1, bool frlg, PkMon out[6]) {
  return read_party_at(sb1, frlg ? 0x0034 : 0x0234, frlg ? 0x0038 : 0x0238, out, NULL);
}

int pk_read_party_auto(const uint8_t* sb1, PkMon out[6], bool* is_frlg) {
  PkMon rse[6], frlg[6];
  int g_rse = -1, g_frlg = -1;
  int n_rse  = read_party_at(sb1, 0x0234, 0x0238, rse, &g_rse);   /* R/S/E */
  int n_frlg = read_party_at(sb1, 0x0034, 0x0038, frlg, &g_frlg); /* FireRed/LeafGreen */
  if (g_frlg > g_rse) {
    memcpy(out, frlg, sizeof(frlg));
    if (is_frlg) *is_frlg = true;
    return n_frlg;
  }
  memcpy(out, rse, sizeof(rse));
  if (is_frlg) *is_frlg = false;
  return n_rse;
}
