/*
 * pdna_gen12 — a Game Boy save mounted as a read-only BoxSource.
 *
 * See pdna_gen12.h for the contract, the read-only argument and the memory story.
 * The short version: the parsers (gen1_save.c / gen2_save.c) and the converter
 * (gen12_convert.c) already exist and are host-tested; this file is the PAGING and
 * PRESENTATION layer that turns them into 30 x 80-byte Gen-3 box records the shared
 * box screen can draw, plus the GBA glue that opens the file and runs the screen.
 *
 * Everything above the PDNA_GEN12_HOST guard is pure C so tests/host_gen12_test.c
 * can drive the real BoxSource over a synthetic image.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "pdna_gen12.h"
#include "gen3_box.h"      /* G3_BOX_WALLPAPER_COUNT, G3_IN_BOX */
#include "gen3_edit.h"     /* gen3_build_mon, em_set_egg, em_set_nickname */
#include "gen3_mon.h"
#include "data_tables.h"   /* pk_species_name (report list) */

/* ================================================================= pure core */

/* The single mounted save. The BoxSource contract's hooks take no `self`
 * (source/pdna_box.h:18), so the mount has to be reachable from module scope —
 * exactly like pdna_bank.c's g_bankbuf/g_meta. One GB save at a time. */
static Gb12Mount* g_m;

bool pdna_gen12_size_is_gb(uint32_t size) {
  /* The battery image is exactly 32 KiB. Emulators of MBC3+RTC carts (Gold, Silver,
   * Crystal) append a 44- or 48-byte clock footer — 32-bit vs 64-bit timestamp,
   * bgb.bircd.org/rtcsave.html — and both parsers ignore anything past 0x8000, so
   * accept the whole window rather than three magic numbers. Nothing else in the
   * PokeDNA world is 32 KiB: a Gen-3 save is 131072. */
  return size >= G2_SAVE_SIZE && size <= G2_SAVE_SIZE + G2_MAX_RTC_TAIL;
}

const char* pdna_gen12_kind_name(Gb12SaveKind k) {
  switch (k) {
    case GB12_SAVE_RBY:     return "Red/Blue/Yellow";
    case GB12_SAVE_GS:      return "Gold/Silver";
    case GB12_SAVE_CRYSTAL: return "Crystal";
    default:                return "not a GB save";
  }
}

const char* pdna_gen12_honest_line(void) {
  /* The one sentence that must never be optimised away. There was no official
   * Gen 1/2 -> Gen 3 transfer (gen12_convert.h), so calling these "your Pokemon,
   * moved" would be a lie the save file itself would later contradict. */
  return "Converted copy - not a native Gen 3 Pokemon.";
}

/* Short enough for the 88 px action-menu panel AND the report list; the long-form
 * wording lives in gen12_reason_text() and on the info screen. Pinned in
 * tests/host_textfit_test.c. */
const char* pdna_gen12_block_reason(uint8_t r) {
  switch ((Gb12Result)r) {
    case GB12_OK:            return 0;
    case GB12_ERR_EMPTY:     return 0;
    case GB12_ERR_EGG:       return "Egg: can't move";
    case GB12_ERR_HELD_ITEM: return "Holding an item";
    case GB12_ERR_SPECIES:   return "Glitch Pokemon";
    case GB12_ERR_MOVE:      return "Bad move data";
    case GB12_ERR_LEVEL:     return "Bad level data";
    case GB12_ERR_PID:       return "No ID could fit";
    default:                 return "Can't convert";
  }
}

int pdna_gen12_nboxes(const Gb12Mount* m) { return m ? m->party_box + 1 : 0; }

uint8_t pdna_gen12_slot_reason(const Gb12Mount* m, int slot) {
  if (!m || slot < 0 || slot >= GB12_SLOTS) return (uint8_t)GB12_ERR_EMPTY;
  return m->reason[slot];
}

/* ---- tiny pure helpers (no libc beyond string.h) --------------------------- */

static uint16_t rd16be_(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

/* Append the decimal form of `v` (0..999) to `dst` at *pos, bounded by cap. */
static void put_uint(char* dst, int cap, int* pos, unsigned v) {
  char t[6]; int n = 0;
  if (v == 0) t[n++] = '0';
  while (v && n < 5) { t[n++] = (char)('0' + (v % 10u)); v /= 10u; }
  while (n > 0 && *pos < cap - 1) dst[(*pos)++] = t[--n];
  dst[*pos] = 0;
}

static void put_str(char* dst, int cap, int* pos, const char* s) {
  while (s && *s && *pos < cap - 1) dst[(*pos)++] = *s++;
  dst[*pos] = 0;
}

/* Copy `src` into `dst` (cap bytes incl. NUL) without splitting a UTF-8 sequence.
 * Decoded GB names are UTF-8 (the gender signs are three bytes each), so a blind
 * strncpy into the 12-byte banner buffer could leave a half codepoint that the
 * font decoder then renders as garbage. */
static void copy_utf8(char* dst, int cap, const char* src) {
  int n = 0;
  if (cap <= 0) return;
  while (src[n] && n < cap - 1) n++;
  while (n > 0 && ((unsigned char)src[n] & 0xC0u) == 0x80u) n--;   /* back off a split */
  memcpy(dst, src, (size_t)n);
  dst[n] = 0;
}

/* NUL-terminated working copy of a name field. gen3_edit.c's encode_name walks its
 * input until the terminator, so a name that arrived from a corrupt GB save without
 * one would read off the end of the struct (the same reason gen12_convert.c does
 * this before every conversion). */
static void copy_z(char* dst, const char* src, int cap) {
  int i = 0;
  for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
  dst[i] = 0;
}

/* ---- mounting ------------------------------------------------------------- */

/* Read the handful of Gen-2 header fields the UI needs, without a resident image.
 * (g2_read_header() wants the whole 32 KiB; this is the same four reads it does.) */
static bool g2_header_ranged(Gb12Mount* m) {
  uint8_t buf[16];
  if (!g2_offsets(m->g2.version, &m->g2o)) return false;
  if (!m->rd(m->ctx, m->g2o.tid, buf, 2)) return false;
  m->tid = rd16be_(buf);
  if (!m->rd(m->ctx, m->g2o.player_name, buf, 11)) return false;
  g2_decode_text(buf, 7, m->player, (int)sizeof m->player);
  if (!m->rd(m->ctx, m->g2o.current_box_no, buf, 1)) return false;
  {
    int cur = buf[0] & 0x0F;                 /* low 4 bits = the open box (gen2_save.c:539) */
    m->current_box = (cur < G2_NUM_BOXES) ? cur : 0;
  }
  if (!m->rd(m->ctx, m->g2o.box_names, m->g2names, sizeof m->g2names)) return false;
  m->g2_gender = -1;
  if (m->g2o.player_gender && m->rd(m->ctx, m->g2o.player_gender, buf, 1))
    m->g2_gender = buf[0] & 1;
  return true;
}

/* Read box `box`'s list blob into m->stage. Fills *cap (slots the box can hold) and
 * *count (slots actually used). false = the box could not be read at all, which is
 * shown as an empty box rather than as an error the user cannot act on. */
static bool gb_list_read(Gb12Mount* m, int box, int* cap, int* count) {
  uint32_t off = 0, bytes = 0;
  *cap = 0; *count = 0;
  if (box < 0 || box > m->party_box) return false;

  if (m->kind == GB12_SAVE_RBY) {
    off   = gen1_list_offset(&m->g1, box);
    bytes = gen1_list_bytes(box);
    *cap  = gen1_list_capacity(box);
  } else {
    off   = g2_list_offset(&m->g2, box, m->current_box);
    bytes = (uint32_t)g2_list_size(box);
    *cap  = g2_list_capacity(box);
  }
  if (!off || bytes == 0 || bytes > GB12_STAGE_BYTES || *cap <= 0) { *cap = 0; return false; }
  if (!m->rd(m->ctx, off, m->stage, bytes)) { *cap = 0; return false; }

  int n = (m->kind == GB12_SAVE_RBY) ? gen1_list_count(m->stage, box)
                                     : g2_list_count(m->stage, box);
  if (n < 0) n = 0;                                  /* malformed list -> "no mons here" */
  if (n > *cap) n = *cap;
  *count = n;
  return true;
}

/* Decode slot `slot` out of the staged list into the generation-neutral struct the
 * converter takes. false = there is no decodable Pokemon there. `slot_salt` is
 * positional and STABLE (box * 20 + slot, the party being box == party_box, which is
 * the 20 * nboxes + slot the header asks for) — it is what keeps two byte-identical
 * GB twins from converting to the same 80 bytes, and it must never depend on
 * anything that changes between page-ins. */
static bool gb_slot_mon(const Gb12Mount* m, int box, int slot, Gb12Mon* out) {
  uint32_t salt = (uint32_t)box * 20u + (uint32_t)slot;
  memset(out, 0, sizeof *out);
  if (m->kind == GB12_SAVE_RBY) {
    Gen1Mon g1;
    if (!gen1_decode(m->stage, box, slot, &g1)) return false;
    gen12_from_gen1(&g1, salt, out);
  } else {
    G2Mon g2;
    if (!g2_list_mon(m->stage, box, slot, &g2)) return false;
    gen12_from_gen2(&g2, salt, out);
  }
  return true;
}

/* How a record is PRESENTED in the grid. A refusal is never a hole: the user has to
 * be able to find the Pokemon they are looking for and be told why it is stuck. */
enum {
  GB_SHOW_FULL,        /* converts cleanly                                      */
  GB_SHOW_RELAXED,     /* egg / item holder: shown truthfully, copy refused      */
  GB_SHOW_PLACEHOLDER, /* record damaged but the species is real: shown as itself */
  GB_SHOW_NONE         /* no Pokemon can be shown (glitch species / empty slot)  */
};

/* Decide the presentation WITHOUT running a PID search, so the whole-save census at
 * mount stays cheap. */
static int gb_presentation(const Gb12Mon* in, Gb12Result r) {
  if (r == GB12_OK) return GB_SHOW_FULL;
  if (r == GB12_ERR_EMPTY) return GB_SHOW_NONE;
  if (r == GB12_ERR_EGG || r == GB12_ERR_HELD_ITEM) {
    /* An egg and an item holder are both real, identifiable Pokemon; only the
     * TRANSFER is refused. Convert a copy with just that property dropped so the
     * grid shows the actual species/level/nickname, and veto the copy instead. */
    Gb12Mon relaxed = *in;
    relaxed.is_egg = false;
    relaxed.held_item = 0;
    if (gen12_can_convert(&relaxed) == GB12_OK) return GB_SHOW_RELAXED;
    return (in->species_dex >= 1 && in->species_dex <= 251) ? GB_SHOW_PLACEHOLDER : GB_SHOW_NONE;
  }
  /* Damaged move list / level / (unreachable) PID failure: the species is still a
   * real Pokemon, so show a minimal record of that species rather than a hole. */
  if (in->species_dex >= 1 && in->species_dex <= 251) return GB_SHOW_PLACEHOLDER;
  return GB_SHOW_NONE;                       /* MissingNo and friends: nothing to draw */
}

/* Deterministic personality for a placeholder record. Not the converter's identity
 * hash (that one is private to gen12_convert.c and only defined for records that
 * convert); this only has to be STABLE for a given slot, because the clipboard and
 * the bank match mons by their first 8 bytes. FNV-1a over an explicit byte list —
 * never over struct memory, whose padding is uninitialised. */
static uint32_t placeholder_pid(int box, int slot, const Gb12Mon* in) {
  uint32_t h = 2166136261u;
  const uint8_t seq[8] = {
    (uint8_t)box, (uint8_t)slot,
    (uint8_t)in->species_dex, (uint8_t)(in->species_dex >> 8),
    (uint8_t)in->ot_id, (uint8_t)(in->ot_id >> 8),
    in->level, in->gen
  };
  for (int i = 0; i < 8; i++) { h ^= seq[i]; h *= 16777619u; }
  return h ? h : 1u;                         /* 0 would read as an empty slot */
}

/* Build the 80-byte record shown for one slot, and its refusal reason.
 * `rec` is zeroed on entry by the caller; leaving it zeroed means "empty cell". */
static void gb_build_slot(Gb12Mount* m, int box, int slot, uint8_t* rec, uint8_t* reason) {
  Gb12Mon in;
  *reason = (uint8_t)GB12_ERR_EMPTY;
  if (!gb_slot_mon(m, box, slot, &in)) {
    /* The list says a Pokemon is here but nothing decodable is: a glitch species
     * byte. Nothing can be drawn, so the census/report carries it instead. */
    *reason = (uint8_t)GB12_ERR_SPECIES;
    return;
  }

  Gb12Result r = gen12_can_convert(&in);
  *reason = (uint8_t)r;
  int how = gb_presentation(&in, r);

  if (how == GB_SHOW_FULL) {
    if (gen12_convert(&in, &m->tgt, rec, 0) == GB12_OK) return;
    memset(rec, 0, 80);                      /* documented-unreachable; fail visible, not wrong */
    *reason = (uint8_t)GB12_ERR_PID;
    how = (in.species_dex >= 1 && in.species_dex <= 251) ? GB_SHOW_PLACEHOLDER : GB_SHOW_NONE;
  }

  if (how == GB_SHOW_RELAXED) {
    Gb12Mon relaxed = in;
    bool was_egg = in.is_egg;
    relaxed.is_egg = false;
    relaxed.held_item = 0;
    if (gen12_convert(&relaxed, &m->tgt, rec, 0) == GB12_OK) {
      if (was_egg) {                         /* draw it as the Egg it really is */
        EditMon e;
        gen3_edit_load(rec, false, &e);
        em_set_egg(&e, true);
        gen3_edit_commit(&e, rec);
      }
      return;
    }
    memset(rec, 0, 80);
    how = (in.species_dex >= 1 && in.species_dex <= 251) ? GB_SHOW_PLACEHOLDER : GB_SHOW_NONE;
  }

  if (how == GB_SHOW_PLACEHOLDER) {
    /* A stand-in of the right species and (clamped) level carrying the GB nickname,
     * so the cell reads as the Pokemon the player remembers. It is NOT the real mon
     * — its IVs/moves are gen3_build_mon's defaults — which is exactly why the copy
     * veto below is unconditional for every non-OK reason. */
    char otname[sizeof in.ot_name], nick[sizeof in.nickname];
    uint8_t lv = in.level;
    if (lv < 1) lv = 1;
    if (lv > 100) lv = 100;
    copy_z(otname, in.ot_name, (int)sizeof otname);
    copy_z(nick, in.nickname, (int)sizeof nick);
    uint8_t metgame = (m->tgt.met_game >= 1 && m->tgt.met_game <= 15) ? m->tgt.met_game : 3;
    gen3_build_mon(in.species_dex, lv, placeholder_pid(box, slot, &in),
                   (uint32_t)in.ot_id, otname, metgame, rec);
    if (nick[0]) {
      EditMon e;
      gen3_edit_load(rec, false, &e);
      em_set_nickname(&e, nick);
      gen3_edit_commit(&e, rec);
    }
    return;
  }
  memset(rec, 0, 80);                        /* GB_SHOW_NONE */
}

/* Whole-save census: how many Pokemon are here, how many will convert, and WHERE the
 * ones that will not are. Cheap by construction (gen12_can_convert only — no PID
 * search), so mounting stays a couple of SD reads per box. */
static void gb_census(Gb12Mount* m) {
  m->nstored = m->nready = m->nblocked = m->nunreadable = m->nreport = 0;
  for (int b = 0; b <= m->party_box; b++) {
    int cap = 0, count = 0;
    if (!gb_list_read(m, b, &cap, &count)) continue;
    for (int s = 0; s < count && s < cap; s++) {
      Gb12Mon in;
      Gb12Result r;
      uint16_t dex = 0;
      int how;
      m->nstored++;
      if (!gb_slot_mon(m, b, s, &in)) {
        /* The species list claims a Pokemon here but nothing decodes: a glitch
         * species byte. `in` is not filled, so do NOT ask gb_presentation. */
        r = GB12_ERR_SPECIES;
        how = GB_SHOW_NONE;
      } else {
        r = gen12_can_convert(&in);
        dex = in.species_dex;
        how = gb_presentation(&in, r);
      }

      if (r == GB12_OK) { m->nready++; continue; }
      if (how == GB_SHOW_NONE) m->nunreadable++;
      else                     m->nblocked++;
      if (m->nreport < GB12_REPORT_MAX) {
        Gb12Report* rp = &m->report[m->nreport++];
        rp->box = (uint8_t)b;
        rp->slot = (uint8_t)s;
        rp->reason = (uint8_t)r;
        rp->species = dex;
      }
    }
  }
  m->loaded = -1;                            /* the staging buffer no longer matches a box */
}

bool pdna_gen12_mount(Gb12Mount* m, Gb12ReadFn rd, void* ctx, uint32_t len,
                      uint8_t* recs, uint8_t* stage, uint8_t met_game,
                      const char** why) {
  const char* dummy = 0;
  bool specific = false;                     /* a better reason than the generic one? */
  if (!why) why = &dummy;
  *why = "Not a Game Boy save file.";
  if (!m || !rd || !recs || !stage) return false;

  memset(m, 0, sizeof *m);
  m->rd = rd; m->ctx = ctx; m->len = len;
  m->recs = recs; m->stage = stage;
  m->loaded = -1;
  m->g2_gender = -1;
  m->tgt.met_game = met_game;

  if (!pdna_gen12_size_is_gb(len)) { *why = "Wrong size for a GB save."; return false; }

  /* Gen 2 FIRST. Its verdict is a 16-bit sum over ~3 KB checked against a stored
   * word, so a Gen-1 image passing it by accident is a ~1/65536 event; Gen 1's
   * single 8-bit sum would accept a Gen-2 image about once in 256. Trying the
   * stronger test first is what keeps the misidentification rate at the low number
   * instead of the high one. */
  if (g2_detect_ranged(rd, ctx, len, stage, GB12_STAGE_BYTES, &m->g2)) {
    m->kind = (m->g2.version == G2_VER_CRYSTAL) ? GB12_SAVE_CRYSTAL : GB12_SAVE_GS;
    m->nboxes = G2_NUM_BOXES;
    m->party_box = G2_NUM_BOXES;             /* == G2_BOX_PARTY, the parser's own index */
    if (!g2_header_ranged(m)) { *why = "Could not read the GB header."; m->kind = GB12_SAVE_NONE; return false; }
    /* An ambiguous detect means two versions' checksums both matched (1-in-65536).
     * Break the tie structurally rather than guessing: a real box list has a sane
     * count and a 0xFF terminator where the chosen layout says it does. */
    if (m->g2.ambiguous) {
      int cap = 0, count = 0;
      if (!gb_list_read(m, m->current_box, &cap, &count) ||
          !g2_list_plausible(m->stage, m->current_box)) {
        m->kind = GB12_SAVE_NONE;
      }
    }
    if (m->kind != GB12_SAVE_NONE) { gb_census(m); return true; }
  } else if (m->g2.version == G2_VER_JP_GS || m->g2.version == G2_VER_JP_CRYSTAL) {
    /* Detected, deliberately not parsed: JP saves use 9 boxes of 30 and 6-byte name
     * fields, so reading them with the western layout would produce confident
     * nonsense. Say so instead. */
    *why = "Japanese saves not supported yet.";
    return false;
  } else if (m->g2.version != G2_VER_NONE && !m->g2.primary_ok) {
    /* Fall through to Gen 1 — but remember the reason in case that fails too. */
    *why = "GB save checksum failed (corrupt?).";
    specific = true;
  }

  {
    Gen1Save g1;
    Gen1Status st = gen1_open_ranged(rd, ctx, len, &g1);
    if (st == GEN1_OK) {
      m->kind = GB12_SAVE_RBY;
      m->g1 = g1;
      m->nboxes = GEN1_NUM_BOXES;
      m->party_box = GEN1_NUM_BOXES;         /* == GEN1_PARTY_BOX */
      m->current_box = (g1.current_box >= 0 && g1.current_box < GEN1_NUM_BOXES) ? g1.current_box : 0;
      copy_utf8(m->player, (int)sizeof m->player, g1.player_name);
      m->tid = g1.trainer_id;
      gb_census(m);
      return true;
    }
    if (st == GEN1_ERR_CHECKSUM && !specific) *why = "GB save checksum failed (corrupt?).";
  }
  (void)dummy;

  m->kind = GB12_SAVE_NONE;
  return false;
}

/* ---- paging --------------------------------------------------------------- */

uint8_t* pdna_gen12_page(Gb12Mount* m, int box) {
  if (!m || !m->recs || box < 0 || box > m->party_box) return 0;
  uint8_t* recs = m->recs + 0x0004;          /* pc-mini layout: 4-byte header, then records */
  if (m->loaded == box) return recs;

  /* Zero the WHOLE buffer first, every time. Two reasons: slots past the GB box's
   * capacity (a GB box holds 20, the Gen-3 grid shows 30) must read as empty, and
   * the page-in has to be a pure function of the box — never of whatever the buffer
   * happened to hold before, which is the property the determinism test pins. */
  memset(m->recs, 0, GB12_RECS_BYTES);
  for (int s = 0; s < GB12_SLOTS; s++) m->reason[s] = (uint8_t)GB12_ERR_EMPTY;
  m->loaded = box;                           /* set even if the read fails: an unreadable
                                              * box shows as empty, not as stale contents */
  int cap = 0, count = 0;
  if (!gb_list_read(m, box, &cap, &count)) { m->capacity = 0; return recs; }
  if (cap > GB12_SLOTS) cap = GB12_SLOTS;
  m->capacity = (uint8_t)cap;
  for (int s = 0; s < count && s < cap; s++)
    gb_build_slot(m, box, s, recs + (uint32_t)s * 80, &m->reason[s]);
  return recs;
}

void pdna_gen12_box_name(const Gb12Mount* m, int box, char out[12]) {
  int pos = 0;
  out[0] = 0;
  if (!m || box < 0 || box > m->party_box) { put_str(out, 12, &pos, "GB BOX"); return; }
  put_str(out, 12, &pos, "GB ");
  if (box == m->party_box) { put_str(out, 12, &pos, "PARTY"); return; }
  if (m->kind != GB12_SAVE_RBY) {
    /* Gen 2 stores real box names; showing the player's own is worth more than a
     * number. The "GB " prefix stays so the banner never reads like a Gen-3 box. */
    char nm[G2_NAME_BYTES];
    if (g2_box_name(m->g2names, box, nm, (int)sizeof nm) && nm[0]) {
      char cut[12];
      copy_utf8(cut, 12 - pos, nm);
      put_str(out, 12, &pos, cut);
      return;
    }
  }
  put_str(out, 12, &pos, "BOX ");
  put_uint(out, 12, &pos, (unsigned)(box + 1));
}

/* Why the record at `rec80` may not be copied, or NULL if it may.
 *
 * Identified by ADDRESS: `rec80` is the pointer pdna_box handed to app_mon_menu,
 * i.e. records + slot * 80 inside our own paged buffer, so the slot index is exact
 * arithmetic rather than a guess from the record's contents (two GB mons can share
 * every visible field). Anything outside the buffer is not ours -> no veto. */
const char* pdna_gen12_why_locked(const uint8_t* rec80) {
  if (!g_m || !g_m->recs || !rec80) return 0;
  const uint8_t* base = g_m->recs + 0x0004;
  if (rec80 < base || rec80 >= base + (uint32_t)GB12_SLOTS * 80) return 0;
  uint32_t d = (uint32_t)(rec80 - base);
  if (d % 80u) return 0;
  return pdna_gen12_block_reason(g_m->reason[d / 80u]);
}

/* ---- BoxSource hooks (module-singleton state, per the contract) ------------ */

static uint8_t* gbsrc_records(int box) {
  uint8_t* r = pdna_gen12_page(g_m, box);
  return r ? r : (g_m ? g_m->recs + 0x0004 : 0);
}
static void gbsrc_get_name(int box, char out[12]) { pdna_gen12_box_name(g_m, box, out); }
static void gbsrc_set_name(int box, const char* s) { (void)box; (void)s; }   /* read-only */
static int  gbsrc_get_wp(int box) { (void)box; return GB12_WALLPAPER; }
static void gbsrc_set_wp(int box, int wp) { (void)box; (void)wp; }           /* read-only */
static bool gbsrc_can_edit(void) { return false; }
/* MUST return false. pdna_box's cross-scope drop writes the record into the
 * destination, calls commit(), and reverts on failure (pdna_box.c:714-718): a
 * commit() that returned true would tell the box screen a Pokemon had been written
 * into a Game Boy save. It also has nothing to write — this file never opens a file
 * for writing. */
static bool gbsrc_commit(void) { return false; }
static void gbsrc_mark_dirty(void) { }

BoxSource pdna_gen12_source(Gb12Mount* m) {
  BoxSource s;
  memset(&s, 0, sizeof s);
  g_m = m;
  if (!m || m->kind == GB12_SAVE_NONE) return s;
  s.nboxes     = pdna_gen12_nboxes(m);
  s.start_box  = (m->current_box >= 0 && m->current_box < s.nboxes) ? m->current_box : 0;
  s.is_bank    = true;                       /* see the header: this is what removes the
                                              * PARTY tab / START / PC hand-off paths */
  s.wp_count   = G3_BOX_WALLPAPER_COUNT;
  s.records    = gbsrc_records;
  s.menu_block = m->recs;                    /* records at +0x0004; menu box index = 0 */
  s.get_name   = gbsrc_get_name;
  s.set_name   = gbsrc_set_name;
  s.get_wp     = gbsrc_get_wp;
  s.set_wp     = gbsrc_set_wp;
  s.can_edit   = gbsrc_can_edit;
  s.commit     = gbsrc_commit;
  s.mark_dirty = gbsrc_mark_dirty;
  s.note_add   = 0;                           /* nothing lands here; nothing to register */
  return s;
}

/* ============================================================== GBA glue ==== */
#ifndef PDNA_GEN12_HOST

#include <tonc.h>
#include <stdio.h>

#include "ff.h"
#include "savefile.h"      /* SF_PATH_MAX */
#include "log.h"
#include "ui.h"
#include "snd.h"
#include "rmbl.h"
#include "pdna_app.h"

#define GB12_A4(n)  (((uint32_t)(n) + 3u) & ~3u)
/* One arena block holds everything: the mount, the FatFs handle we keep open for the
 * whole session, the 2404-byte box buffer and the list staging buffer. +4 so the
 * base can be rounded up to a 4-byte boundary (FIL has 32-bit members and g_pc is a
 * u8 array whose alignment the compiler is not obliged to guarantee). */
#define GB12_ARENA_NEED (GB12_A4(sizeof(Gb12Mount)) + GB12_A4(sizeof(FIL)) + \
                         GB12_RECS_BYTES + GB12_STAGE_BYTES + 4u)
/* Measured 2026-08: 468 (mount) + 600 (FIL) + 2404 + 1152 + 4 = 4628 of 35712 (the
 * FIL grew 592 -> 600 when FF_USE_FASTSEEK went to 1; see lib/fatfs/ffconf.h). The
 * assert is here because app_arena_acquire returns NULL rather than failing loudly if
 * the request ever outgrew the donor, and "the GB screen quietly refuses to open" is
 * a bad way to learn that a struct grew. */
_Static_assert(GB12_ARENA_NEED <= APP_ARENA_BYTES,
               "GB import no longer fits the borrowed EWRAM arena");

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) snd_move();
  else if (k & KEY_A) snd_ok();
  else if (k & KEY_B) snd_back();
  return k;
}

/* Panel is 216 px with text at x=20 -> body lines must stay <= 25 columns. */
static void s_msg(const char* title, u16 ink, const char* l1, const char* l2) {
  ui_panel(12, 50, 216, l2 ? 60 : 50, UI_PANEL, UI_BORDER);
  ui_text(20, 58, ink, title);
  ui_hline(16, 70, 208, UI_BORDER);
  if (l1) ui_ptext_fit(20, 76, 200, UI_TEXT, l1);
  if (l2) ui_ptext_fit(20, 86, 200, UI_TEXT, l2);
  ui_text(20, l2 ? 96 : 86, UI_DIM, "A ok");
  s_wait(KEY_A | KEY_B);
}

/* The one seek+read shim both parsers and the pager go through. The FIL stays open
 * for the whole session (in the arena), so a box switch costs one seek + one read.
 * READ-ONLY: the file is opened FA_READ and no write call exists in this file. */
static bool gb_read(void* ctx, uint32_t off, void* buf, uint32_t len) {
  FIL* f = (FIL*)ctx;
  UINT br = 0;
  if (!f || !buf) return false;
  if (f_lseek(f, (FSIZE_t)off) != FR_OK) return false;
  if (f_read(f, buf, (UINT)len, &br) != FR_OK) return false;
  return br == len;
}

/* ---- info + report screens ------------------------------------------------ */

static void gb_report_page(const Gb12Mount* m) {
  for (;;) {
    ui_clear();
    ui_text(4, 3, UI_TITLE, "NOT TRANSFERABLE");
    ui_hline(0, 13, UI_SCR_W, UI_BORDER);
    if (m->nreport == 0) {
      ui_ptext_fit(4, 20, 232, UI_OK, "Every Pokemon here can be copied.");
    } else {
      ui_ptext_fit(4, 18, 232, UI_DIM, "Where they are, and why they stay:");
      int rows = m->nreport;
      if (rows > 11) rows = 11;
      for (int i = 0; i < rows; i++) {
        const Gb12Report* r = &m->report[i];
        char where[24]; int pos = 0;
        if (r->box == (uint8_t)m->party_box) put_str(where, (int)sizeof where, &pos, "PARTY ");
        else { put_str(where, (int)sizeof where, &pos, "BOX "); put_uint(where, (int)sizeof where, &pos, (unsigned)(r->box + 1)); put_str(where, (int)sizeof where, &pos, " "); }
        put_str(where, (int)sizeof where, &pos, "#");
        put_uint(where, (int)sizeof where, &pos, (unsigned)(r->slot + 1));
        int y = 30 + i * 10;
        ui_ptext_fit(4, y, 60, UI_TEXT, where);
        ui_ptext_fit(66, y, 74, UI_DIM,
                     (r->species >= 1 && r->species <= 251) ? pk_species_name(r->species) : "?");
        ui_ptext_fit(142, y, 94, UI_WARN, pdna_gen12_block_reason(r->reason));
      }
      if (m->nreport > 11)
        ui_ptext_fit(4, 30 + 11 * 10, 232, UI_DIM, "More than fit; the rest are in the boxes.");
    }
    ui_hline(0, 147, UI_SCR_W, UI_BORDER);
    ui_text(4, 150, UI_DIM, "B back");
    u16 k = s_wait(KEY_A | KEY_B);
    if (k & (KEY_A | KEY_B)) return;
  }
}

/* The page the user lands on. Everything honest about this feature is said here
 * BEFORE any Pokemon is shown, and again on the way out if anything was blocked. */
static bool gb_info_page(const Gb12Mount* m) {
  for (;;) {
    char l[64]; int pos;

    ui_clear();
    ui_text(4, 3, UI_TITLE, "GAME BOY SAVE");
    ui_hline(0, 13, UI_SCR_W, UI_BORDER);

    pos = 0;
    put_str(l, (int)sizeof l, &pos, pdna_gen12_kind_name(m->kind));
    if (m->player[0]) { put_str(l, (int)sizeof l, &pos, "  -  "); put_str(l, (int)sizeof l, &pos, m->player); }
    put_str(l, (int)sizeof l, &pos, "  ID ");
    put_uint(l, (int)sizeof l, &pos, (unsigned)m->tid);
    ui_ptext_fit(4, 18, 232, UI_TEXT, l);

    ui_ptext_wrap(4, 30, 232, 9, 2, UI_WARN, pdna_gen12_honest_line());

    ui_hline(0, 50, UI_SCR_W, UI_BORDER);
    pos = 0; put_str(l, (int)sizeof l, &pos, "Pokemon in this save: ");
    put_uint(l, (int)sizeof l, &pos, (unsigned)m->nstored);
    ui_ptext_fit(4, 55, 232, UI_TEXT, l);
    pos = 0; put_str(l, (int)sizeof l, &pos, "Ready to copy: ");
    put_uint(l, (int)sizeof l, &pos, (unsigned)m->nready);
    ui_ptext_fit(4, 66, 232, UI_OK, l);
    pos = 0; put_str(l, (int)sizeof l, &pos, "Shown but locked: ");
    put_uint(l, (int)sizeof l, &pos, (unsigned)m->nblocked);
    ui_ptext_fit(4, 77, 232, m->nblocked ? UI_WARN : UI_DIM, l);
    pos = 0; put_str(l, (int)sizeof l, &pos, "Slots we cannot read: ");
    put_uint(l, (int)sizeof l, &pos, (unsigned)m->nunreadable);
    ui_ptext_fit(4, 88, 232, m->nunreadable ? UI_WARN : UI_DIM, l);

    ui_hline(0, 100, UI_SCR_W, UI_BORDER);
    ui_ptext_wrap(4, 105, 232, 9, 4, UI_DIM,
                  "Copy a Pokemon here, then paste it into your Gen 3 boxes or the Bank. "
                  "This Game Boy save is only ever read.");

    ui_hline(0, 147, UI_SCR_W, UI_BORDER);
    ui_text(4, 150, UI_DIM, "A browse  SEL list  B back");

    u16 k = s_wait(KEY_A | KEY_B | KEY_SELECT);
    if (k & KEY_SELECT) { gb_report_page(m); continue; }
    if (k & KEY_B) return false;
    if (k & KEY_A) return true;
  }
}

/* ---- entry ---------------------------------------------------------------- */

int pdna_gen12_show(const char* path, uint8_t met_game) {
  if (!path || !path[0]) return 0;

  /* The arena is the map screen's donor (pdna_app.h): it hands back g_pc, which is
   * only safe when the PC holds nothing unsaved. NULL means exactly that — say so
   * and stop, never "helpfully" commit on the user's behalf. */
  uint8_t* arena = app_arena_acquire(GB12_ARENA_NEED);
  if (!arena) {
    ui_clear();
    snd_deny();
    s_msg("NOT NOW", UI_WARN, "Save the Pokemon you moved,", "then open the GB save.");
    return 0;
  }

  uint32_t base   = GB12_A4((uint32_t)(uintptr_t)arena);
  Gb12Mount* m    = (Gb12Mount*)(uintptr_t)base;
  FIL*  f         = (FIL*)(uintptr_t)(base + GB12_A4(sizeof(Gb12Mount)));
  uint8_t* recs   = (uint8_t*)(uintptr_t)(base + GB12_A4(sizeof(Gb12Mount)) + GB12_A4(sizeof(FIL)));
  uint8_t* stage  = recs + GB12_RECS_BYTES;

  memset(f, 0, sizeof *f);
  if (f_open(f, path, FA_READ) != FR_OK) {
    ui_clear();
    snd_error();
    s_msg("CANNOT OPEN", UI_WARN, "The file could not be read.", 0);
    app_arena_release();
    return 0;
  }
  /* FSIZE_t is 64-bit with exFAT enabled (lib/fatfs/ffconf.h FF_FS_EXFAT 1). Saturate
   * rather than truncate: a 4 GiB + 32 KiB file must not wrap to "exactly 32 KiB" and
   * be handed to a parser as a GB save. */
  FSIZE_t fsz = f_size(f);
  uint32_t len = (fsz > (FSIZE_t)0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)fsz;

  const char* why = 0;
  if (!pdna_gen12_mount(m, gb_read, f, len, recs, stage, met_game, &why)) {
    log_line("gen12: mount %s failed (%lu bytes): %s", path, (unsigned long)len, why ? why : "?");
    ui_clear();
    snd_deny();
    s_msg("NOT A GB SAVE", UI_WARN, why ? why : "Unrecognised file.", 0);
    f_close(f);
    app_arena_release();
    return 0;
  }
  log_line("gen12: %s mounted (%s) %d mons, %d ready, %d locked, %d bad",
           path, pdna_gen12_kind_name(m->kind), m->nstored, m->nready,
           m->nblocked, m->nunreadable);

  rmbl_fire(RCUE_ROOM);
  if (gb_info_page(m)) {
    /* Tell app_mon_menu that the ACTIVE box source is read-only, so its destructive
     * actions (PASTE / RELEASE / DUPLICATE / CREATE / MOVE) are not offered on a
     * source that cannot accept them. This also suppresses the bank's deferred-delete
     * bookkeeping for these boxes — see pdna_main.c. Cleared unconditionally below;
     * every exit from the box screen passes through it. */
    app_src_readonly_set(pdna_gen12_why_locked, "Converted copy");
    BoxSource s = pdna_gen12_source(m);
    /* Returns 0 on B / the SAVE tab, 5 when the cursor drops off the bottom row (the
     * PC<->Bank hand-off, which has no PC to hand off to here) — re-enter on the top
     * tabs so DOWN puts the user back in the grid instead of silently exiting. */
    while (pdna_box(&s) != 0) app_box_start_set(1);
    app_src_readonly_clear();
    pdna_gen12_source(0);                    /* unmount: no dangling arena pointers */
    if (m->nblocked || m->nunreadable) gb_report_page(m);
  }

  f_close(f);
  app_arena_release();
  return 0;
}

#endif /* PDNA_GEN12_HOST */
