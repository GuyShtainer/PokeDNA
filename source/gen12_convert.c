#include "gen12_convert.h"
#include "gen3_edit.h"     /* EditMon, gen3_build_mon, the em_* setters */
#include "gen3_mon.h"      /* PK_* stat order, pk_gender_from, pk_unown_form, pk_is_shiny */
#include "data_tables.h"   /* growth rates, exp tables, base PP, gender ratios, abilities */
#include <string.h>

/* Gen 1/2 -> Gen 3 conversion. See gen12_convert.h for the honesty posture and the
 * determinism requirement; this file is the mechanism.
 *
 * Sources for the rules, once, here: Bulbapedia "Poke Transporter" (nature = EXP % 25,
 * EVs erased, Poke Ball, SID 0, eggs and item-holders refused, gender from the Attack
 * IV), Bulbapedia "Individual values" (the Gen-2 shiny DV pattern, the derived HP DV,
 * the Unown letter), and docs/research-gen12.md sections 4 and 5 for how each of those
 * lands in a Gen-3 record. Where we deviate from Transporter the comment says so. */

/* ---- derived properties (pure DV / EXP math) ---------------------------------- */

/* Gen 1/2 store four DVs and DERIVE the HP one from their low bits, so a mon whose
 * Atk/Def/Spd/Spc DVs are all odd has HP DV 15. */
uint8_t gen12_hp_dv(uint8_t atk, uint8_t def, uint8_t spd, uint8_t spc) {
  return (uint8_t)(((atk & 1u) << 3) | ((def & 1u) << 2) | ((spd & 1u) << 1) | (spc & 1u));
}

/* Gen 2 has no shiny flag: a mon is shiny iff Def = Spd = Spc = 10 and the Atk DV has
 * bit 1 set (2, 3, 6, 7, 10, 11, 14, 15). Gen-1 mons have DVs too, so the same test
 * applies to them -- which is exactly what a Virtual Console RBY transfer does. */
bool gen12_is_shiny(uint8_t atk, uint8_t def, uint8_t spd, uint8_t spc) {
  return def == 10 && spd == 10 && spc == 10 && (atk & 2u) != 0;
}

/* Gen 2: female iff the Atk DV is at or below the species' threshold (7M:1F -> 0..1,
 * 3:1 -> 0..3, 1:1 -> 0..7, 1:3 -> 0..11). Gen 3 states the same four ratios out of
 * 256 (31, 63, 127, 191) and the DV form is just those divided by 16, so
 * dv * 16 <= ratio reproduces the Gen-2 thresholds exactly, for any ratio byte.
 *
 * Species with a fixed gender answer from the ratio alone, exactly as pk_gender_from
 * does for Gen 3 -- Nidoran-f is female whatever its Atk DV says, and reporting it as
 * "genderless" here would put the wrong symbol on the box grid. */
uint8_t gen12_gender(uint8_t dv_atk, uint8_t gen3_gender_ratio) {
  if (gen3_gender_ratio == 0x00) return 0;   /* always male       */
  if (gen3_gender_ratio == 0xFE) return 1;   /* always female     */
  if (gen3_gender_ratio == 0xFF) return 2;   /* genderless        */
  return ((uint16_t)dv_atk * 16u <= gen3_gender_ratio) ? 1u : 0u;
}

/* Whether the PID has to be searched for a gender at all: for the three fixed ratios
 * Gen 3 never consults the personality value, so constraining it would only shrink the
 * search space for nothing. */
static bool gender_is_pid_derived(uint8_t ratio) {
  return ratio != 0x00 && ratio != 0xFE && ratio != 0xFF;
}

/* The middle two bits (1-2) of each DV nibble, in Atk, Def, Spd, Spc order, form an
 * 8-bit value; the letter is that / 10, so 0..25 = A..Z. Gen 2 has no ! or ? forms,
 * which is why this never returns 26 or 27 (Gen 3's pk_unown_form does). */
uint8_t gen12_unown_letter(uint8_t atk, uint8_t def, uint8_t spd, uint8_t spc) {
  uint8_t v = (uint8_t)((((atk >> 1) & 3u) << 6) | (((def >> 1) & 3u) << 4)
                      | (((spd >> 1) & 3u) << 2) |  ((spc >> 1) & 3u));
  return (uint8_t)(v / 10u);
}

uint8_t gen12_nature(uint32_t exp) { return (uint8_t)(exp % 25u); }

uint8_t gen12_iv_from_dv(uint8_t dv) {
  if (dv > 15) dv = 15;
  return (uint8_t)(dv * 2u + GEN12_IV_LOW_BIT);
}

/* ---- reason codes -------------------------------------------------------------- */

const char* gen12_reason_text(Gb12Result r) {
  switch (r) {
    case GB12_OK:            return "Can be converted";
    case GB12_ERR_EMPTY:     return "Empty slot";
    case GB12_ERR_SPECIES:   return "Not a real Pokemon (glitch species)";
    case GB12_ERR_EGG:       return "Eggs cannot be transferred";
    case GB12_ERR_HELD_ITEM: return "Holding an item - take it off first";
    case GB12_ERR_MOVE:      return "Move list is not valid";
    case GB12_ERR_LEVEL:     return "Level is out of range";
    case GB12_ERR_PID:       return "Could not build a matching Pokemon";
    default:                 return "Cannot be converted";
  }
}

Gb12Result gen12_can_convert(const Gb12Mon* in) {
  if (!in) return GB12_ERR_EMPTY;
  if (in->species_dex == 0 && in->level == 0 && in->exp == 0 && in->moves[0] == 0)
    return GB12_ERR_EMPTY;
  /* Refused for being an egg before anything else: an egg's own species is valid, so
   * any other verdict here would be a worse explanation. */
  if (in->is_egg) return GB12_ERR_EGG;
  /* Gen 1's internal-index map sends MissingNo and friends to dex 0; 252+ cannot come
   * from a sane GB record at all. Both mean "do not invent a Pokemon for this". */
  if (in->species_dex == 0 || in->species_dex > 251) return GB12_ERR_SPECIES;
  /* Transporter leaves item-holders behind rather than dropping the item silently, and
   * most Gen-2 items (Berserk Gene, mail, the old berries) have no Gen-3 counterpart to
   * carry anyway. docs/research-gen12.md section 4 floats the softer "import without
   * the item, say so on screen"; if that is ever wanted, this is the only line to
   * change and GB12_ERR_HELD_ITEM already names the case. */
  if (in->gen >= 2 && in->held_item != 0) return GB12_ERR_HELD_ITEM;
  if (in->level == 0 || in->level > 100) return GB12_ERR_LEVEL;
  /* Every Gen-1 move (1..165) and Gen-2 move (166..251) exists in Gen 3 under the same
   * id -- verified against data_tables.c: all 251 have a name and a non-zero base PP,
   * and 252 is FAKE OUT, the first Gen-3-only move. So the only bad move id is one
   * outside that range, which means a corrupt record. */
  if (in->moves[0] == 0) return GB12_ERR_MOVE;
  for (int i = 0; i < 4; i++) if (in->moves[i] > 251) return GB12_ERR_MOVE;
  return GB12_OK;
}

/* ---- the identity hash --------------------------------------------------------- */

/* FNV-1a over an EXPLICIT byte sequence, never over the struct's memory: struct
 * padding is uninitialised and would make the "deterministic" PID depend on whatever
 * was on the caller's stack. The fields fed in are the ones that identify the mon.
 *
 * Deliberately NOT fed: friendship, Pokerus and current PP, which the GB game changes
 * as you play, and the held item, which refuses the mon anyway. EXP is fed because it
 * already decides the nature. */
static uint32_t fnv1a(uint32_t h, const void* data, uint32_t n) {
  const uint8_t* p = (const uint8_t*)data;
  for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}
static uint32_t fnv1a_u8(uint32_t h, uint8_t v)   { return fnv1a(h, &v, 1); }
static uint32_t fnv1a_u32(uint32_t h, uint32_t v) {
  uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
  return fnv1a(h, b, 4);   /* fixed little-endian order, so the hash is host-independent */
}

/* strlen that cannot leave the field. These strings are decoded straight out of a GB
 * save that may be corrupt, so a missing terminator has to be survivable rather than a
 * read off the end of the struct. */
static uint32_t bounded_len(const char* s, uint32_t cap) {
  uint32_t n = 0;
  while (n < cap && s[n]) n++;
  return n;
}

/* Copy one of those fields into a local that IS terminated. `cap` is the destination
 * size, so at most cap-1 bytes survive and dst[cap-1] is always free for the NUL.
 * identity_seed hashes the same cap-1 bound, so the name that is hashed is the name
 * that gets stored. */
static void copy_z(char* dst, const char* src, uint32_t cap) {
  uint32_t n = bounded_len(src, cap - 1);
  memcpy(dst, src, n);
  dst[n] = 0;
}

static uint32_t identity_seed(const Gb12Mon* in) {
  uint32_t h = 2166136261u;
  h = fnv1a_u8 (h, in->gen);
  h = fnv1a_u32(h, in->species_dex);
  h = fnv1a_u32(h, in->ot_id);
  h = fnv1a_u32(h, in->exp);
  h = fnv1a_u8 (h, (uint8_t)((in->dv_atk << 4) | (in->dv_def & 0x0Fu)));
  h = fnv1a_u8 (h, (uint8_t)((in->dv_spd << 4) | (in->dv_spc & 0x0Fu)));
  for (int i = 0; i < 4; i++) h = fnv1a_u32(h, in->moves[i]);
  h = fnv1a(h, in->ot_name,  bounded_len(in->ot_name,  sizeof in->ot_name  - 1));
  h = fnv1a_u8(h, 0);                      /* length delimiter: "AB"+"C" != "A"+"BC" */
  h = fnv1a(h, in->nickname, bounded_len(in->nickname, sizeof in->nickname - 1));
  h = fnv1a_u8(h, 0);
  h = fnv1a_u32(h, in->slot_salt);
  return h;
}

/* ---- deterministic PID construction -------------------------------------------- */

/* Gen 3 derives the nature, the gender, shininess and the Unown letter from the same
 * 32 bits, so satisfying all four at once means searching for a personality value --
 * and the search must be a pure function of the record, or the mon would get a new
 * identity every time its box is paged in.
 *
 * The shiny branch mirrors em_reroll (gen3_edit.c:295): shininess is
 * (tid ^ sid ^ pid_lo ^ pid_hi) < 8, so rather than testing PIDs at random we FORCE
 * pid_hi = tid ^ sid ^ pid_lo ^ k for k = 0..7. Every candidate is then shiny by
 * construction, and the loop walks the entire shiny space for this otId (65536 * 8
 * PIDs) exactly once. Nature keeps ~1/25 of it, gender at worst 31/256, the Unown
 * letter 1/28 -- hundreds of solutions survive even for a shiny Unown, and the first
 * hit arrives in a few thousand iterations.
 *
 * We do not call em_reroll itself, for two reasons worth stating: (1) it cannot take
 * an Unown-letter constraint, and its partner em_set_unown_form applies the letter
 * afterwards with a random walk that would need ~5.7M tries to re-hit shiny + nature +
 * letter -- past its 4M cap, i.e. shiny Unown would quietly fail; (2) its shiny branch
 * always starts at lo = 0, so two shiny mons sharing species, nature, gender and TID
 * get the SAME PID. That is harmless when a user rerolls one mon in the editor, but
 * here the PID is an identity key, so the walk is seeded from the record instead. */
static bool pid_matches(uint32_t pid, uint16_t tid, uint16_t sid, int want_nature,
                        int want_shiny, int want_gender, uint8_t ratio, int want_letter) {
  if (want_nature >= 0 && (int)(pid % 25u) != want_nature) return false;
  if (want_letter >= 0 && (int)pk_unown_form(pid) != want_letter) return false;
  if (want_gender >= 0 && (int)pk_gender_from(pid, ratio) != want_gender) return false;
  if (want_shiny  >= 0 && (int)pk_is_shiny(pid, tid, sid) != want_shiny) return false;
  return true;
}

static bool solve_pid(uint32_t seed, uint16_t tid, uint16_t sid, int want_nature,
                      int want_shiny, int want_gender, uint8_t ratio, int want_letter,
                      uint32_t* out_pid) {
  if (want_shiny == 1) {
    for (uint32_t i = 0; i < 0x10000u; i++) {
      /* 0x9E35 is odd, so (seed + i * 0x9E35) mod 2^16 visits all 65536 low halves
       * exactly once, in an order that differs even for adjacent seeds. */
      uint16_t lo = (uint16_t)(seed + i * 0x9E35u);
      for (uint32_t j = 0; j < 8u; j++) {
        uint32_t k = ((seed >> 16) + j) & 7u;
        uint16_t hi = (uint16_t)(tid ^ sid ^ lo ^ (uint16_t)k);
        uint32_t pid = ((uint32_t)hi << 16) | lo;
        if (pid == 0) continue;            /* gen3_build_mon substitutes its own PID for 0 */
        if (pid_matches(pid, tid, sid, want_nature, want_shiny, want_gender, ratio, want_letter)) {
          *out_pid = pid;
          return true;
        }
      }
    }
    return false;
  }

  /* Not shiny: no closed form to exploit, so walk em_reroll's LCG from our seed. The
   * constraints keep >=1/700 of PIDs (nature 1/25 * letter 1/28, or nature * gender),
   * so this converges in low thousands; the cap only exists so a caller can never
   * hang. */
  uint32_t pid = seed;
  for (uint32_t i = 0; i < 400000u; i++) {
    pid = pid * 1103515245u + 12345u;
    if (pid == 0) continue;
    if (pid_matches(pid, tid, sid, want_nature, want_shiny, want_gender, ratio, want_letter)) {
      *out_pid = pid;
      return true;
    }
  }
  return false;
}

/* ---- fields gen3_edit.h has no setter for -------------------------------------- */

/* Five values the conversion must carry have no em_* setter, so they are written into
 * the EditMon's substruct array directly. EditMon.sub[] is public and documented as
 * the four substructs in canonical Growth/Attacks/EVs/Misc order (gen3_edit.h:22), and
 * the record is still produced by gen3_edit_commit, so encryption, the per-mon
 * checksum and the personality-derived substruct order are still not our problem.
 * Offsets are from reference/pokeemerald/pokemon.h (PokemonSubstruct0 / 3) and match
 * how gen3_mon.c:124-146 reads them back. */
static void put_exp(EditMon* e, uint32_t exp) {           /* Growth 0x04 */
  e->sub[0][4] = (uint8_t)exp;         e->sub[0][5] = (uint8_t)(exp >> 8);
  e->sub[0][6] = (uint8_t)(exp >> 16); e->sub[0][7] = (uint8_t)(exp >> 24);
}
static void put_pp_bonuses(EditMon* e, uint8_t bonuses) { /* Growth 0x08, 2 bits per move */
  e->sub[0][8] = bonuses;
}
static void put_pokerus(EditMon* e, uint8_t pokerus) {    /* Misc 0x00 */
  e->sub[3][0] = pokerus;
}
static void put_ot_gender(EditMon* e, bool female) {      /* Misc origins bit 15 */
  if (female) e->sub[3][3] |= 0x80u;
  else        e->sub[3][3] &= (uint8_t)~0x80u;
}
/* Misc ribbon word bit 31. In R/S this bit does nothing; in FireRed/LeafGreen and
 * Emerald it "controls Mew & Deoxys obedience and whether they can be traded"
 * (reference/pokeemerald/pokemon.h:169-175, modernFatefulEncounter). Set for an
 * imported Mew only -- without it the game refuses to obey a Mew it did not hand out,
 * and a Mew that ignores its trainer would look like our bug rather than Gen 3's rule.
 * Deliberately not set for anything else: it is also the flag later generations read
 * as "fateful encounter", so handing it out freely would be a second, quieter lie. */
static void put_fateful(EditMon* e) {
  e->sub[3][11] |= 0x80u;
}

/* ---- the conversion ------------------------------------------------------------ */

/* Case-insensitive ASCII compare, for "is this nickname just the species name?". */
static bool same_name(const char* a, const char* b) {
  for (;; a++, b++) {
    char ca = *a, cb = *b;
    if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
    if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
    if (ca != cb) return false;
    if (!ca) return true;
  }
}

/* BACKLOG #183: spelling-compare, same shape as b177's gb_name_changed
 * (bank_down_convert.c) but for THIS hop (GB name -> Gen-3 bytes) -- decode what
 * actually landed in `out80` and diff it against what the caller meant to write.
 * Name fields (nickname @0x08, otName @0x14) are plaintext regardless of the
 * encrypt pass gen3_edit_commit already ran, so gen3_decode_name can read out80
 * directly. Catches every glyph gen3_encode_char still cannot store (the
 * brackets, per gen3_save.c's own comment -- Gen 3's real charmap has no code
 * point for either) as a loss row instead of a silent space. The intended
 * nickname is the species name whenever gen12_convert's own em_set_nickname call
 * was skipped -- comparing against the ORIGINAL `nick` there would
 * false-positive every unnamed import.
 *
 * BACKLOG #216 CLOSURE: `written` is decoded straight from `out80` (the Gen-3
 * bytes THIS conversion just produced) through gen3_decode_name -- the SAME
 * table encode_name/em_set_nickname used to build those bytes -- so the
 * comparison already runs at the Gen-3 byte level, not a mismatched
 * GB-string-vs-Gen-3-string compare. That is what catches a length overflow too:
 * a Gen-1 nickname built entirely of <PK>/<MN> ligature bytes (gen1_decode_name
 * expands each to 2 ASCII chars) can exceed the Gen-3 field's 10/7-glyph cap, and
 * the resulting truncation makes `written` shorter than `otname`/`nick`, which
 * strcmp catches (tests/host_gen3_codec_lossy_test.c pins this). What stays
 * OUT OF REACH here, structurally: a loss already baked into `otname`/`nick`
 * before this function runs -- gen1_save.c/gen2_save.c own that decode and this
 * module "never sees a raw GB save" (this file's own top-of-file comment); see
 * gen12_convert.h's Gb12Notes.otname_lossy/nick_lossy comment for the full
 * before/after. */
static void note_spelling_loss(Gb12Notes* notes, const uint8_t out80[80],
                               const char* otname, const char* nick, uint16_t species) {
  /* Worst case is 10 Gen-3 bytes that are ALL gender signs -- 3 UTF-8 bytes each
   * (30) plus the NUL. 32 covers both the 10-byte nickname and the 7-byte otName
   * with room to spare; gen3_decode_name's own `outcap` bound (not `maxlen`)
   * means a too-small buffer here would read as a false loss, not a real one, so
   * this must never shrink without re-deriving the worst case. */
  char written[32];
  gen3_decode_name(written, (int)sizeof written, out80 + 0x14, 7);
  notes->otname_lossy = strcmp(written, otname) != 0;

  const char* intended_nick = (nick[0] && !same_name(nick, pk_species_name(species)))
                               ? nick : pk_species_name(species);
  gen3_decode_name(written, (int)sizeof written, out80 + 0x08, 10);
  notes->nick_lossy = strcmp(written, intended_nick) != 0;
}

Gb12Result gen12_convert(const Gb12Mon* in, const Gb12Target* tgt,
                         uint8_t out[80], Gb12Notes* notes) {
  Gb12Notes local;
  if (!notes) notes = &local;
  memset(notes, 0, sizeof(*notes));

  Gb12Result why = gen12_can_convert(in);
  if (why != GB12_OK) return why;
  if (!out) return GB12_ERR_EMPTY;

  /* Work from NUL-terminated copies. gen3_edit.c's encode_name walks its input until
   * the terminator (consuming three bytes per gender sign), so a name that arrived
   * from a corrupt GB save without one would take it off the end of the struct. */
  char otname[sizeof in->ot_name], nick[sizeof in->nickname];
  copy_z(otname, in->ot_name,  sizeof otname);
  copy_z(nick,   in->nickname, sizeof nick);

  /* Gen 3's origin field is 4 bits wide; anything else would silently truncate to a
   * different game (or to 0, which no record uses). */
  uint8_t metgame = (tgt && tgt->met_game >= 1 && tgt->met_game <= 15) ? tgt->met_game : 3;

  /* Species: for ids 1..251 the Gen-3 INTERNAL index and the National Dex number are
   * the same number (verified against data_tables.c's national table: identity across
   * 1..251, and internal 252 is the first that is not), and a Gen-2 record already
   * stores the dex number. So there is no species map to apply on this side. */
  const uint16_t species = in->species_dex;
  const uint8_t  growth  = pk_species_growth(species);

  /* EXP carries over raw, so the mon keeps its progress towards the next level. None
   * of species 1..251 changed EXP group between generations (histogram over
   * data_tables.c: 119 Medium Fast, 64 Medium Slow, 23 Fast, 45 Slow, and zero in the
   * two groups Gen 3 added), and the four legacy formulas are unchanged, so the level
   * Gen 3 computes from this EXP is the level the GB game showed. A glitched Gen-1
   * record can hold more EXP than level 100 needs; cap it and say so. */
  uint32_t exp = in->exp;
  uint32_t exp_max = pk_exp_for_level(growth, 100);
  if (exp > exp_max) { exp = exp_max; notes->exp_clamped = true; }
  uint8_t level = pk_level_from_exp(growth, exp);

  const uint8_t ratio = pk_species_gender_ratio(species);
  const uint8_t hp_dv = gen12_hp_dv(in->dv_atk, in->dv_def, in->dv_spd, in->dv_spc);

  /* The PID has to encode all of these at once. Nature is Transporter's rule, applied
   * to the EXP we actually store so that "nature == stored EXP % 25" holds for every
   * record this produces. */
  const int  want_nature = (int)gen12_nature(exp);
  const int  want_shiny  = gen12_is_shiny(in->dv_atk, in->dv_def, in->dv_spd, in->dv_spc) ? 1 : 0;
  int        want_gender = gender_is_pid_derived(ratio)
                         ? (int)gen12_gender(in->dv_atk, ratio) : -1;
  int        want_letter = (species == 201)             /* Unown */
                         ? (int)gen12_unown_letter(in->dv_atk, in->dv_def, in->dv_spd, in->dv_spc)
                         : -1;

  /* SID 0 is half the import fingerprint (gen12_convert.h) and it is also what
   * Transporter does: "The Pokemon's Secret ID number is set to 00000". */
  const uint16_t tid = in->ot_id, sid = 0;
  const uint32_t otid = (uint32_t)tid;

  uint32_t seed = identity_seed(in), pid = 0;
  if (!solve_pid(seed, tid, sid, want_nature, want_shiny, want_gender, ratio, want_letter, &pid)) {
    /* Unreachable for real gender ratios -- the shiny space alone offers ~500k
     * candidates and the loosest constraint set keeps hundreds. Relax in the order
     * docs/research-gen12.md section 5.9 gives (gender first, then the letter) and
     * flag it, because a silently wrong gender would be a lie on the summary screen. */
    if (solve_pid(seed, tid, sid, want_nature, want_shiny, -1, ratio, want_letter, &pid)) {
      notes->gender_relaxed = true;
    } else if (solve_pid(seed, tid, sid, want_nature, want_shiny, -1, ratio, -1, &pid)) {
      notes->gender_relaxed = true;
      notes->letter_relaxed = true;
    } else {
      return GB12_ERR_PID;
    }
  }

  /* Build a real record rather than assembling bytes: gen3_build_mon gives a valid,
   * present, non-egg box mon with English language, friendship 70, exp for the level
   * and the nickname set to the species name. */
  gen3_build_mon(species, level, pid, otid, otname, metgame, out);

  EditMon e;
  gen3_edit_load(out, false, &e);

  put_exp(&e, exp);

  /* IVs are DV * 2 (see GEN12_IV_LOW_BIT). Gen 1/2 have one Special DV where Gen 3 has
   * two stats, so it feeds both Sp. Atk and Sp. Def -- the same way the old games used
   * one Special stat for both halves of a special attack. */
  em_set_iv(&e, PK_HP,  gen12_iv_from_dv(hp_dv));
  em_set_iv(&e, PK_ATK, gen12_iv_from_dv(in->dv_atk));
  em_set_iv(&e, PK_DEF, gen12_iv_from_dv(in->dv_def));
  em_set_iv(&e, PK_SPE, gen12_iv_from_dv(in->dv_spd));
  em_set_iv(&e, PK_SPA, gen12_iv_from_dv(in->dv_spc));
  em_set_iv(&e, PK_SPD, gen12_iv_from_dv(in->dv_spc));

  /* EVs stay 0, as Transporter does: "Stat experience is completely erased". Gen-1/2
   * stat exp counts to 65535 per stat with no budget, so there is no honest mapping
   * into Gen 3's 255-per-stat / 510-total system, and a made-up one would hand the mon
   * stats it never earned. gen3_build_mon already leaves them (and the contest
   * conditions and ribbons) zeroed. */

  /* Moves keep their ids -- Gen 3 kept every Gen-1/2 move at the same number. PP is
   * rebuilt from Gen 3's own base PP, plus the PP Ups the GB record carried in bits
   * 6-7 of each PP byte. Current PP is set full rather than carried: box storage is
   * where PP goes to be forgotten, and a mon that arrives with 2/35 PP just looks
   * broken. The bonus formula is the games' own (base + base * 20 * ups / 100, i.e.
   * Hyper Beam 5 -> 8 at 3 Ups, Petal Dance 20 -> 32). */
  uint8_t pp_bonuses = 0;
  for (int i = 0; i < 4; i++) {
    em_set_move(&e, i, in->moves[i]);          /* also resets PP and clears this slot's Ups */
    if (in->moves[i]) {
      uint8_t ups  = (in->pp_ups[i] > 3) ? 3 : in->pp_ups[i];
      uint8_t base = pk_move_pp(in->moves[i]);
      pp_bonuses = (uint8_t)(pp_bonuses | (uint8_t)(ups << (i * 2)));
      em_set_pp(&e, i, (uint8_t)(base + (base * 20u * ups) / 100u));
    }
  }
  put_pp_bonuses(&e, pp_bonuses);              /* after the loop: em_set_move clears these */

  /* Gen 2 keeps a real friendship value; Gen 1 has none (only Yellow's Pikachu), so
   * those keep gen3_build_mon's 70. Pokerus uses the same strain/days nibbles in both. */
  if (in->gen >= 2) {
    em_set_friendship(&e, in->friendship);
    put_pokerus(&e, in->pokerus);
  }

  /* The import signature, written in one place even where gen3_build_mon already
   * agrees -- this is what makes an import self-identifying instead of disguised, so
   * it must not drift with another function's defaults. */
  em_set_metloc(&e, 0xFE);      /* METLOC_IN_GAME_TRADE; pk_location_name renders "TRADE" */
  em_set_ball(&e, 4);           /* Poke Ball -- Transporter puts every transfer in one    */
  em_set_metlevel(&e, level);
  em_set_metgame(&e, metgame);
  put_ot_gender(&e, in->gen >= 2 && in->has_caught_data && in->ot_gender != 0);

  /* Ability slot: pre-Gen-4 the games pick it from the PID's low bit, and only for a
   * species that has a second ability -- retail CreateBoxMon leaves the field 0
   * otherwise, so we do too rather than storing a slot that does not exist. */
  if (pk_species_ability(species, 1) != 0) em_set_ability(&e, (uint8_t)(pid & 1u));

  /* Gen 1/2 have no "is nicknamed" bit and neither does Gen 3: an unnamed mon simply
   * carries its species name in the nickname field. Rewriting the matching case with
   * the Gen-3 spelling keeps that true (and fixes the two Nidoran, whose name is a
   * gender sign in both generations but a different byte in each). */
  if (nick[0] && !same_name(nick, pk_species_name(species)))
    em_set_nickname(&e, nick);

  if (species == 151) put_fateful(&e);   /* Mew only; see put_fateful */

  gen3_edit_commit(&e, out);
  note_spelling_loss(notes, out, otname, nick, species);
  return GB12_OK;
}

/* ---- adapters from gen1_save.c / gen2_save.c ----------------------------------- */

/* Straight field copies (see gen12_convert.h). Both readers already decode their names
 * to the same UTF-8 spelling data_tables.c uses, so the strings pass through untouched
 * and the "is this just the species name?" test above works for both generations. */

void gen12_from_gen1(const Gen1Mon* m, uint32_t slot_salt, Gb12Mon* out) {
  memset(out, 0, sizeof *out);
  if (!m) return;
  out->gen = 1;
  out->species_dex = m->dex;          /* already mapped from the internal index; 0 = MissingNo */
  out->exp = m->exp;
  out->level = m->level;
  out->dv_atk = m->dv[G1_ATK];
  out->dv_def = m->dv[G1_DEF];
  out->dv_spd = m->dv[G1_SPE];        /* G1_SPE is the GB "Speed" DV */
  out->dv_spc = m->dv[G1_SPC];
  for (int i = 0; i < 4; i++) {
    out->moves[i]  = m->moves[i];
    out->pp_ups[i] = m->ppups[i];
  }
  out->ot_id = m->otId;
  copy_z(out->ot_name,  m->otName,   sizeof out->ot_name);
  copy_z(out->nickname, m->nickname, sizeof out->nickname);
  out->slot_salt = slot_salt;
  /* Left at 0 on purpose: held_item (m->catch_rate is not one), friendship (Gen 1 has
   * none, so the conversion uses 70), pokerus, is_egg (Gen 1 has no eggs at all) and
   * the Crystal caught data. */
}

void gen12_from_gen2(const G2Mon* m, uint32_t slot_salt, Gb12Mon* out) {
  memset(out, 0, sizeof *out);
  if (!m) return;
  out->gen = 2;
  out->species_dex = m->species;      /* Gen 2 stores the National Dex number directly */
  out->exp = m->exp;
  out->level = m->level;
  out->dv_atk = m->dv[0];
  out->dv_def = m->dv[1];
  out->dv_spd = m->dv[2];
  out->dv_spc = m->dv[3];
  for (int i = 0; i < 4; i++) {
    out->moves[i]  = m->moves[i];
    out->pp_ups[i] = m->pp_up[i];
  }
  out->ot_id      = m->otid;
  out->held_item  = m->held_item;     /* non-zero refuses the mon, as Transporter does */
  out->friendship = m->friendship;
  out->pokerus    = m->pokerus;
  out->is_egg     = m->is_egg;
  out->has_caught_data = m->caught_valid;
  out->ot_gender  = m->ot_gender;
  copy_z(out->ot_name,  m->otname,   sizeof out->ot_name);
  copy_z(out->nickname, m->nickname, sizeof out->nickname);
  out->slot_salt = slot_salt;
  /* m->hp_dv, m->is_shiny and m->unown_letter are NOT copied: this module derives all
   * three from the same four DVs, so leaving them out keeps one implementation of each
   * rule and lets the host test assert the two agree instead of trusting either. */
}
