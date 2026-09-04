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
  s.last_box_is_party = true;   /* the party pseudo-box at nboxes-1 gets no banner ordinal */
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
#include "gb_session.h"
#include "gb_editor.h"
#include "pdna_gbedit.h"
#include "pdna_layout.h"   /* PDNA_GBEDIT_* / PDNA_SIDECAR_* -- fixed strings         */
#include "gb_sidecar.h"    /* S5-B: the sidecar format + gbsc_path/gbsc_key            */
#include "gen3_to_gb.h"    /* S5-B: the Gen-3 -> Game Boy down converter               */
#include "gba_rtc.h"       /* S5-B: the sidecar entry's transfer-time RTC stamp        */

/* Same value as pdna_main.c's PDNA_DIR "/sidecar" (hard rule 9: one folder per tool).
 * Not shared through a header because nothing in this tree centralises PokeDNA's own
 * path constants today (pdna_main.c's LOG_PATH is likewise a local #define) -- if
 * PDNA_DIR ever changes, this one must change with it. */
#define PDNA_GEN12_SIDECAR_DIR "/PokeDNA/sidecar"

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

/* The SAME shim against a RESIDENT image instead of a file. The GB session opened
 * from the file browser has already read the whole save into g_save (pdna_main.c) --
 * a GB battery save is 32 KiB and g_save is 128 KiB of EWRAM that a GB session never
 * otherwise uses -- so every parser read is a memcpy and no FatFs handle is held at
 * all. This is also the shape the WRITE path needs: the engines edit a resident image
 * and sf_write_verified() persists it once, so the user's file is never open for
 * writing while an edit is half-applied (docs/GEN12-EDIT-DESIGN.md section 3.2).
 *
 * The bounds test is written as two subtractions rather than `off + len > c->len` so
 * that a caller asking for a range that wraps 32 bits is refused instead of accepted. */
static bool gb_img_read(void* ctx, uint32_t off, void* buf, uint32_t len) {
  const Gb12Image* c = (const Gb12Image*)ctx;
  if (!c || !c->img || !buf) return false;
  if (off > c->len || len > c->len - off) return false;
  memcpy(buf, c->img + off, len);
  return true;
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

/* S5-B Part E (defined further down, after the S2 edit session it needs -- g_ed,
 * GbSession, gb_has_sidecar): "N Pokemon here came from Gen 3", counted for
 * gb_info_page() below. Forward-declared here so the info page can call it without
 * moving the whole S2 section above the info/report screens it has never depended on
 * until now. */
static int gb_sidecar_here_count(const Gb12Mount* m);

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
    /* S5-B Part E: max_lines dropped 4 -> 3 here. The fixed paragraph below measures
     * exactly 3 wrapped lines at 232px (tests/host_textfit_test.c pins it), so this
     * was never really a 4-line budget -- it is a 3-line paragraph plus ONE more row
     * deliberately reserved for the sidecar count line below, so the total ink still
     * provably stays within the same y=105..147 span the screen always had. */
    int nlines = ui_ptext_wrap(4, 105, 232, 9, 3, UI_DIM,
                  "Copy a Pokemon here, then paste it into your Gen 3 boxes or the Bank. "
                  "This Game Boy save is only ever read.");
    /* Computed lazily (this page only, not on every repaint elsewhere) -- see
     * gb_sidecar_here_count()'s own comment for why box 0 stands in for "the current
     * box" here and the f_stat bound this stays within. */
    int here = gb_sidecar_here_count(m);
    if (here >= 0) {
      char l2[40]; int pos2 = 0;
      put_uint(l2, (int)sizeof l2, &pos2, (unsigned)here);
      put_str(l2, (int)sizeof l2, &pos2, PDNA_SIDECAR_INFO_SUFFIX);
      ui_ptext_fit(4, 105 + nlines * 9, 232, UI_TEXT, l2);
    }

    ui_hline(0, 147, UI_SCR_W, UI_BORDER);
    ui_text(4, 150, UI_DIM, "A browse  SEL list  B back");

    u16 k = s_wait(KEY_A | KEY_B | KEY_SELECT);
    if (k & KEY_SELECT) { gb_report_page(m); continue; }
    if (k & KEY_B) return false;
    if (k & KEY_A) return true;
  }
}

/* ---- entry ---------------------------------------------------------------- */


/* ---- S2: editing the resident image (docs/GEN12-EDIT-DESIGN.md 3.2-3.3) ------
 *
 * Everything an edit needs, placed in the arena behind the mount's buffers — no new
 * statics (the EWRAM guard is a few hundred bytes). ONE module pointer to it, NULL for
 * a read-only session (the nav-menu entry, which opens a FIL over a file while a Gen-3
 * save owns g_save, and the delta build).
 *
 * The session's scratch is its OWN 1152 B and not the mount's staging buffer: the
 * G2Writer keeps the pointer for the whole session and streams its verify through it,
 * and the mount pages boxes through `stage` on every grid refresh — never at the same
 * instant today, but "never at the same instant" is not a contract anyone checks. */
typedef struct {
  GbSession  s;
  uint8_t    list[GBS_LIST_BYTES];
  uint8_t    list2[GBS_LIST_BYTES];   /* S3: gbs_move's SECOND staging buffer (destination) */
  uint8_t    scratch[GBS_SCRATCH_BYTES];
  uint8_t*   img;             /* the resident save, edited in place            */
  uint8_t*   pristine;        /* byte-exact copy: rollback after a failed write */
  uint32_t   len;
  const char* path;           /* where an edit is persisted                    */
  /* S5-B (Part D): the one .pds file gb_paste_write()/gb_paste_sidecar_undo() are
   * reading/rewriting RIGHT NOW -- one at a time, never a cache, so it does not grow
   * with GBSC_MAX_ENTRIES the way a real cache would. Arena-resident like every other
   * Gb12Edit field, not a local: 1042 B is real weight neither hook's own frame should
   * carry (see gb_paste_write's own noinline comment). */
  uint8_t    sidecar[GBSC_FILE_MAX];
} Gb12Edit;
static Gb12Edit* g_ed;        /* pointer only: the block itself lives in the arena */

static void s_busy(const char* line) {
  ui_clear();
  ui_panel(16, 60, 208, 48, UI_PANEL, UI_WARN);
  ui_text(28, 70, UI_WARN, PDNA_GBEDIT_BUSY_SAVING);
  ui_text(28, 88, UI_TEXT, line);
}

/* The card refused; put RAM back to what the card holds so the grid never shows an
 * edit that did not land, and re-latch the session over the restored bytes. Also the
 * documented recovery for gbs_move()'s own atomicity contract (gb_session.h): a failure
 * on the SOURCE half of a move can leave the destination half already committed in RAM,
 * and this is the only way back to a state the card actually holds. */
static void gb_rollback(void) {
  memcpy(g_ed->img, g_ed->pristine, g_ed->len);
  gbs_open(&g_ed->s, g_ed->img, g_ed->len, g_ed->scratch, sizeof g_ed->scratch);
  if (g_m) g_m->loaded = -1;
}

/* noinline is the whole point of the split: with one call site GCC inlines it at -O2 and
 * bak[272] would be back in every hook's frame for the entire editor/picker run (measured
 * 472 B inlined vs 120 + 360 B split for the S2 editor alone). savefile.c:122 uses the
 * same attribute for the same reason. Shared by all three hooks now: `what_for_log` is
 * the one-word tag ("edit"/"move"/"release") each hook's own detailed log line already
 * named, so this function's own lines stay generic. */
static bool __attribute__((noinline)) gb_persist(const char* what_for_log);

/* The address-only half of gb_locate: `rec80`'s ADDRESS inside the paged box (exactly
 * like pdna_gen12_why_locked) resolves to a (box, slot) in the GB session's own
 * numbering -- storage boxes 0..n-1 and the party at n, which is GEN1_PARTY_BOX /
 * G2_BOX_PARTY (both == their box count). NO CART GATE and NO edit-session requirement
 * here on purpose: gb_copy_native_hook below calls this directly because "copying is
 * allowed on any cart" (docs/GEN3-TO-GB-SIDECAR-DESIGN.md section 10) -- only the two
 * hooks that actually mutate the image (via gb_locate, just below) need the gates. */
static bool gb_locate_addr(const uint8_t* rec80, int* box, int* slot) {
  if (!g_m || !g_m->recs || !rec80 || !box || !slot) return false;
  const uint8_t* base = g_m->recs + 0x0004;
  if (rec80 < base || rec80 >= base + (uint32_t)GB12_SLOTS * 80) return false;
  uint32_t d = (uint32_t)(rec80 - base);
  if (d % 80u) return false;
  *slot = (int)(d / 80u);
  *box  = g_m->loaded;
  if (*box < 0) return false;
  return true;
}

/* Shared by gb_edit_hook / gb_move_hook / gb_release_hook: gb_locate_addr() above, plus
 * the two gates every one of the three needs before it may touch the image at all:
 *   1. the cart (app_can_edit: Omega only, hard rule 4);
 *   2. the box (gbs_box_writable: a Gen-1 virgin bank would DESTROY the edit).
 * Also requires an open edit session (g_ed) -- none of these three exist without one.
 * Returns false (nothing touched, the user already told why for 1/2) or true with
 * the out-params box and slot filled in. */
static bool gb_locate(uint8_t* rec80, int* box, int* slot) {
  if (!g_ed) return false;
  if (!gb_locate_addr(rec80, box, slot)) return false;

  if (!app_can_edit()) {                                                    /* 1 */
    snd_deny();
    msg_wait(PDNA_GBEDIT_READONLY_TITLE, UI_WARN, PDNA_GBEDIT_NEEDS_OMEGA, 0);
    return false;
  }
  GbsStatus st = gbs_box_writable(&g_ed->s, *box);                          /* 2 */
  if (st != GBS_OK) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_BOXWR_TITLE, UI_WARN, gbs_status_text(st),
             st == GBS_ERR_UNWRITABLE ? PDNA_GBEDIT_UNWRITABLE_HINT : 0);
    return false;
  }
  return true;
}

/* AppSrcOps.copy_native (S5-B): capture the record in its own Game Boy shape for the
 * clipboard, not the lossy Gen-3-converted bytes the grid shows. Deliberately requires
 * an open edit session (g_ed) even though gb_locate_addr() itself does not -- reading
 * the RAW record needs a GbSession (gbs_load_list) and a staging buffer, both of which
 * only exist while g_ed is set (the picker's resident-image path); the read-only
 * nav-menu mount streams boxes from a FIL and never builds either. Nothing here mutates
 * the image or requires app_can_edit(): "copying is allowed on any cart". */
static bool gb_copy_native_hook(const uint8_t* rec80, GbEditMon* out) {
  int box, slot;
  if (!g_ed) return false;
  if (!gb_locate_addr(rec80, &box, &slot)) return false;
  GbSession* s = &g_ed->s;
  if (gbs_load_list(s, box, g_ed->list) != GBS_OK) return false;
  if (slot >= gb_list_count(s->gen, g_ed->list, box)) return false;
  return gb_load(out, s->gen, g_ed->list, box, slot);
}

/* S5-B Part E: does `mon` already have a sidecar entry on the card? Only Gen 2 ever
 * can (Gen 1 has no gen3_to_gb() target yet -- S5-C -- so no Gen-1 record was ever
 * transferred down). noinline for the same reason gb_paste_write is: FILINFO
 * (lib/fatfs/ffconf.h: FF_USE_LFN=1, so fname[FF_LFN_BUF+1]=256 bytes alone) is well
 * over the ~200 B a hook's own frame should carry -- one f_stat is cheap, but only if
 * its 280-ish-byte argument lives in a frame that is not also live for the whole
 * editor/picker/confirm run gb_edit_hook drives. */
static bool __attribute__((noinline)) gb_has_sidecar(uint8_t gen, const GbEditMon* mon) {
  if (gen != GB_GEN2) return false;
  uint8_t dv4[4] = {
    gb_get_dv(mon, GB_ATK), gb_get_dv(mon, GB_DEF),
    gb_get_dv(mon, GB_SPE), gb_get_dv(mon, GB_SPC)
  };
  uint64_t key = gbsc_key(mon->gen, gb_get_otid(mon), dv4, mon->otname);
  char path[48];
  if (gbsc_path(path, sizeof path, PDNA_GEN12_SIDECAR_DIR, key) < 0) return false;
  FILINFO fi;
  return f_stat(path, &fi) == FR_OK;
}

/* S5-B Part E, forward-declared above gb_info_page(): "N Pokemon here came from Gen 3".
 * Walks the PARTY plus ONE box (never the whole save -- every box x its capacity would
 * be dozens of f_stat calls) and counts occupied slots whose CURRENT key already has a
 * sidecar file. -1 (line omitted by the caller) when there is nothing to walk with:
 * Gen 1 has no gen3_to_gb() target yet (S5-C) so it can never have a sidecar entry, and
 * no open edit session (g_ed NULL, the read-only nav-menu mount) means no GbSession to
 * load a slot through at all.
 *
 * "the current box": gb_info_page() runs BEFORE the box grid ever pages one in
 * (m->loaded is -1 there -- see pdna_gen12_mount's own comment, set only once
 * gbsrc_records() actually runs), so there IS no current box yet at the point this is
 * called. Box 0 (the grid's own default landing box) stands in for it -- a deliberate,
 * documented call, not what the S5-B brief's "current box" literally describes.
 *
 * Bounded: gb_list_capacity's own party (6) + one box (<= G2's 20, well under 30)
 * f_stat calls, at most. */
static int gb_sidecar_here_count(const Gb12Mount* m) {
  if (!g_ed || g_ed->s.gen != GB_GEN2) return -1;

  int boxes[2]; int nb = 0;
  int box0 = (m->loaded >= 0) ? m->loaded : 0;
  boxes[nb++] = box0;
  if (m->party_box != box0) boxes[nb++] = m->party_box;

  int n = 0;
  for (int bi = 0; bi < nb; bi++) {
    int box = boxes[bi];
    if (gbs_load_list(&g_ed->s, box, g_ed->list) != GBS_OK) continue;
    int cnt = gb_list_count(g_ed->s.gen, g_ed->list, box);
    for (int slot = 0; slot < cnt; slot++) {
      GbEditMon e;
      if (!gb_load(&e, g_ed->s.gen, g_ed->list, box, slot)) continue;
      if (gb_has_sidecar(g_ed->s.gen, &e)) n++;
    }
  }
  return n;
}

/* app_src_ops_set() hook: EDIT on the read-only mon menu.
 *
 * Order of gates past gb_locate's two, each refusing with the card untouched:
 *   3. the record (gb_commit_checked: the bytes landed where the editor put them);
 *   4. the image (gbs_commit_list: the engine's own structural + verify gates);
 *   5. the card (gb_persist: sf_backup_rolling, then sf_write_verified's four steps).
 * Returns true only after step 5 -- but that return value is not what re-pages the
 * grid: both app_mon_menu call sites (pdna_box.c) discard it and re-fetch
 * src->records(box) unconditionally. The re-page happens because gb_persist's
 * success path sets g_m->loaded = -1, which forces the next records() to reload. */
static bool gb_edit_hook(uint8_t* rec80) {
  int box, slot;
  if (!gb_locate(rec80, &box, &slot)) return false;
  GbSession* s = &g_ed->s;

  GbsStatus st = gbs_load_list(s, box, g_ed->list);
  if (st != GBS_OK) { snd_deny(); msg_wait(PDNA_GBEDIT_BOXRD_TITLE, UI_WARN, gbs_status_text(st), 0); return false; }
  if (slot >= gb_list_count(s->gen, g_ed->list, box)) {
    snd_deny(); msg_wait(PDNA_GBEDIT_EMPTYSLOT_TITLE, UI_WARN, PDNA_GBEDIT_EMPTYSLOT_L1, 0); return false;
  }

  GbEditMon e;
  if (!gb_load(&e, s->gen, g_ed->list, box, slot)) { snd_deny(); return false; }
  bool has_sidecar = gb_has_sidecar(s->gen, &e);
  if (!pdna_gbedit(&e, s->gen == GB_GEN1 ? "Gen 1 record" : "Gen 2 record", has_sidecar))
    return false;

  if (!gb_commit_checked(&e, g_ed->list, box, slot)) {                       /* 3 */
    snd_error();
    msg_wait(PDNA_GBEDIT_REFUSED_TITLE, UI_WARN, PDNA_GBEDIT_NOVERIFY_L1, PDNA_GBEDIT_NOTHING_L2);
    return false;
  }
  st = gbs_commit_list(s, box, g_ed->list);                                  /* 4 */
  if (st != GBS_OK) {
    gb_rollback();
    log_line("gen12: edit commit box %d slot %d refused: %s", box, slot, gbs_status_text(st));
    snd_error();
    msg_wait(PDNA_GBEDIT_REFUSED_TITLE, UI_WARN, gbs_status_text(st), PDNA_GBEDIT_UNCHANGED_L2);
    return false;
  }

  log_line("=== gb edit commit -> %s box %d slot %d ===", g_ed->path, box, slot);
  return gb_persist("edit");                                                 /* 5 */
}

/* MOVE TO's destination picker: every box the session knows, party last (S3 design,
 * docs/GEN12-EDIT-DESIGN.md section 6). `exclude` (the box the mon already lives in) is
 * drawn UI_DIM and is never reachable by UP/DOWN or pickable by A -- moving a box to
 * itself is not a move gbs_move() even accepts (GBS_ERR_ARG). Returns the picked box,
 * or -1 on B. */
/* The largest either generation's box list can be: G2_NUM_BOXES (14) storage boxes +
 * the party pseudo-box = 15 (Gen 1 is 12 + 1 = 13). Sized to that rather than to
 * m->party_box + 1 at call time so the array is a fixed frame slot, not a VLA. */
#define GB12_PICKBOX_MAX 15

static int gb_pick_box(const Gb12Mount* m, int exclude) {
  int n = m->party_box + 1;
  if (n <= 1) return -1;
  if (n > GB12_PICKBOX_MAX) n = GB12_PICKBOX_MAX;   /* defensive; never true today */

  /* Every box gbs_box_writable refuses (a Gen-1 virgin/uninitialised bank) is exactly as
   * unreachable as `exclude` -- moving a Pokemon there would only bounce back off
   * gbs_move's own gbs_commit_list gate, so it is dimmed and skipped here instead of
   * offered and then refused a screen later. Computed ONCE, not per repaint: a box's
   * writability cannot change while this picker is up (nothing else touches the image). */
  bool skip[GB12_PICKBOX_MAX];
  int selectable = 0;
  for (int b = 0; b < n; b++) {
    skip[b] = (b == exclude) || (gbs_box_writable(&g_ed->s, b) != GBS_OK);
    if (!skip[b]) selectable++;
  }
  if (!selectable) {
    msg_wait(PDNA_GBEDIT_PICKBOX_NONE_TITLE, UI_WARN, PDNA_GBEDIT_PICKBOX_NONE_L1, 0);
    return -1;
  }

  int sel = 0;
  while (skip[sel]) sel++;                          /* selectable > 0, so this halts */
  int top = 0;
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + PDNA_GBEDIT_PICKBOX_ROWS) top = sel - PDNA_GBEDIT_PICKBOX_ROWS + 1;

    ui_clear();
    ui_text(4, 3, UI_TITLE, PDNA_GBEDIT_PICKBOX_TITLE);
    ui_hline(0, 13, UI_SCR_W, UI_BORDER);
    int shown = n - top;
    if (shown > PDNA_GBEDIT_PICKBOX_ROWS) shown = PDNA_GBEDIT_PICKBOX_ROWS;
    for (int i = 0; i < shown; i++) {
      int b = top + i;
      char nm[12];
      pdna_gen12_box_name(m, b, nm);
      int y = PDNA_GBEDIT_PICKBOX_Y0 + i * PDNA_GBEDIT_PICKBOX_ROW_H;
      bool sh = (b == sel);
      if (sh) ui_panel(2, y - 1, UI_SCR_W - 4, PDNA_GBEDIT_PICKBOX_ROW_H, UI_SEL, UI_TITLE);
      ui_text(4, y, skip[b] ? UI_DIM : (sh ? UI_SELTEXT : UI_TEXT), nm);
    }
    ui_hline(0, 147, UI_SCR_W, UI_BORDER);
    ui_text(4, 150, UI_DIM, PDNA_GBEDIT_PICKBOX_FOOT);

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return -1;
    if (k & KEY_UP)   { do { sel = (sel > 0) ? sel - 1 : n - 1; } while (skip[sel]); }
    if (k & KEY_DOWN) { do { sel = (sel + 1) % n; } while (skip[sel]); }
    if ((k & KEY_A) && !skip[sel]) return sel;
  }
}

/* app_src_ops_set() hook: MOVE TO on the read-only mon menu (S3). Picks a destination,
 * then gbs_move() -- see its header for the full refusal list and the atomicity
 * contract this function has to honour: a non-OK return can mean the DESTINATION half
 * already committed (the source delete is what failed), so every non-OK here rolls the
 * whole image back, not just on the ones that look like they need it. */
static bool gb_move_hook(uint8_t* rec80) {
  int box, slot;
  if (!gb_locate(rec80, &box, &slot)) return false;

  int dst = gb_pick_box(g_m, box);
  if (dst < 0) return false;                       /* B on the picker: nothing touched */

  int to_slot = -1;
  GbsStatus st = gbs_move(&g_ed->s, box, slot, dst, &to_slot, g_ed->list, g_ed->list2);
  if (st != GBS_OK) {
    /* A refusal BEFORE either commit (ARG/SLOT/FULL/PARTY_FLOOR/MAIL/NEEDS_BASE/BOX)
     * leaves the image untouched, and gb_rollback() over an untouched image is a no-op
     * memcpy -- calling it unconditionally is simpler than tracking which refusals are
     * "safe" and is documented as the deliberate choice in gb_session.h's own header. */
    gb_rollback();
    log_line("gen12: move box %d slot %d -> box %d refused: %s",
             box, slot, dst, gbs_status_text(st));
    snd_error();
    const char* hint = 0;
    switch (st) {
      case GBS_ERR_NEEDS_BASE:  hint = PDNA_GBEDIT_MOVE_NEEDSBASE_L2; break;
      case GBS_ERR_PARTY_FLOOR: hint = PDNA_GBEDIT_MOVE_FLOOR_L2;     break;
      case GBS_ERR_MAIL:        hint = PDNA_GBEDIT_MOVE_MAIL_L2;      break;
      case GBS_ERR_FULL:        hint = PDNA_GBEDIT_MOVE_FULL_L2;      break;
      /* gb_pick_box already dims and skips every box gbs_box_writable refuses, so this
       * should be unreachable in practice -- kept as a real case, not folded into
       * `default`, because "unreachable in practice" is not a proof and a refusal this
       * specific deserves its own hint rather than silently falling back to none. */
      case GBS_ERR_UNWRITABLE:  hint = PDNA_GBEDIT_UNWRITABLE_HINT;   break;
      default: break;
    }
    msg_wait(PDNA_GBEDIT_REFUSED_TITLE, UI_WARN, gbs_status_text(st), hint);
    return false;
  }

  log_line("=== gb move -> %s box %d slot %d -> box %d slot %d ===",
           g_ed->path, box, slot, dst, to_slot);
  return gb_persist("move");
}

/* app_src_ops_set() hook: RELEASE on the read-only mon menu (S3). Confirms with the
 * mon's own GB nickname (decoded fresh via gb_load -- NEVER the converted Gen-3 record
 * the grid shows, which the design forbids editing off of), then gbs_delete(). */
/* Split out of gb_release_hook so its own frame never has to hold a GbEditMon
 * (~90 B, gb_edit.h's own estimate) alongside the two name buffers app_confirm needs
 * -- same "noinline sheds the big locals" reasoning as gb_persist's split for
 * bak[SF_PATH_MAX]. Decodes the mon's OWN GB nickname (never the converted Gen-3
 * record the grid shows -- the design forbids editing off of that). */
static bool __attribute__((noinline))
gb_release_confirm(uint8_t gen, const uint8_t* list, int box, int slot) {
  char name[GB_TEXT_MAX], l1[64];
  GbEditMon e;
  if (gb_load(&e, gen, list, box, slot)) gb_get_nickname(&e, name, sizeof name);
  else name[0] = 0;
  /* app_confirm wraps l1 to 2 lines inside a 184 px proportional panel. ui_truncate here
   * is a SAFETY CAP, not a claim that nothing is ever cut: gb_get_nickname's own escape
   * for a byte no charset spells is "{XX}", four columns for one glyph, so a nickname
   * built entirely of such escapes can run to 40 columns for 10 real glyphs (GB_NICK_
   * GLYPHS) -- 12 columns genuinely truncates a name like that to 11 characters plus a
   * trailing '~' (ui_truncate's own marker). That is the intended behaviour of a confirm
   * prompt that must not overflow its panel, not a guarantee against truncation. The
   * ASCII fallback (PDNA_GBEDIT_RELEASE_FALLBACK) is exactly 12 characters and never
   * needs the cut. */
  ui_truncate(l1, name[0] ? name : PDNA_GBEDIT_RELEASE_FALLBACK, 12);
  return app_confirm(PDNA_GBEDIT_RELEASE_TITLE, l1);
}

static bool gb_release_hook(uint8_t* rec80) {
  int box, slot;
  if (!gb_locate(rec80, &box, &slot)) return false;
  GbSession* s = &g_ed->s;

  GbsStatus st = gbs_load_list(s, box, g_ed->list);
  if (st != GBS_OK) { snd_deny(); msg_wait(PDNA_GBEDIT_BOXRD_TITLE, UI_WARN, gbs_status_text(st), 0); return false; }
  if (slot >= gb_list_count(s->gen, g_ed->list, box)) {
    snd_deny(); msg_wait(PDNA_GBEDIT_EMPTYSLOT_TITLE, UI_WARN, PDNA_GBEDIT_EMPTYSLOT_L1, 0); return false;
  }

  if (!gb_release_confirm(s->gen, g_ed->list, box, slot)) return false;

  st = gbs_delete(s, box, slot, g_ed->list);
  if (st != GBS_OK) {
    gb_rollback();
    log_line("gen12: release box %d slot %d refused: %s", box, slot, gbs_status_text(st));
    snd_error();
    const char* hint = (st == GBS_ERR_PARTY_FLOOR) ? PDNA_GBEDIT_MOVE_FLOOR_L2
                      : (st == GBS_ERR_MAIL)        ? PDNA_GBEDIT_MOVE_MAIL_L2
                      : PDNA_GBEDIT_UNCHANGED_L2;
    msg_wait(PDNA_GBEDIT_REFUSED_TITLE, UI_WARN, gbs_status_text(st), hint);
    return false;
  }

  log_line("=== gb release -> %s box %d slot %d ===", g_ed->path, box, slot);
  return gb_persist("release");
}

/* ============================================================================
 * S5-B Part D: AppSrcOps.paste -- PASTE (GB) on an empty cell, the DOWN direction
 * (docs/GEN3-TO-GB-SIDECAR-DESIGN.md sections 5 + 10)
 * ========================================================================== */

/* The transfer-down confirm screen: one short line per Gen3ToGbLoss flag actually set,
 * grouped to the ten categories the design doc lists (several raw flags share a line --
 * nature+ability both vanish for the same reason: neither exists on a Game Boy record).
 * noinline for the same reason gb_release_confirm is split off gb_release_hook. */
static int loss_row(int y, bool cond, const char* text) {
  if (!cond) return y;
  ui_ptext_fit(4, y, UI_SCR_W - 8, UI_TEXT, text);
  return y + PDNA_SIDECAR_LOSS_ROW_H;
}
static bool __attribute__((noinline)) gb_paste_loss_screen(const Gen3ToGbLoss* loss) {
  ui_clear();
  ui_text(4, 3, UI_TITLE, PDNA_SIDECAR_LOSS_TITLE);
  ui_hline(0, 13, UI_SCR_W, UI_BORDER);

  int y = PDNA_SIDECAR_LOSS_ROW_Y0;
  y = loss_row(y, loss->nature || loss->ability,      PDNA_SIDECAR_LOSS_NATURE);
  y = loss_row(y, loss->ribbons || loss->contest,     PDNA_SIDECAR_LOSS_RIBBONS);
  y = loss_row(y, loss->met_data || loss->ball,       PDNA_SIDECAR_LOSS_METDATA);
  y = loss_row(y, loss->ivs_halved,                   PDNA_SIDECAR_LOSS_IVS);
  y = loss_row(y, loss->evs_scaled,                   PDNA_SIDECAR_LOSS_EVS);
  y = loss_row(y, loss->item_dropped,                 PDNA_SIDECAR_LOSS_ITEM);
  y = loss_row(y, loss->secret_id,                    PDNA_SIDECAR_LOSS_SECRETID);
  y = loss_row(y, loss->shiny_lost,                   PDNA_SIDECAR_LOSS_SHINY);
  y = loss_row(y, loss->gender_lost,                  PDNA_SIDECAR_LOSS_GENDER);
  y = loss_row(y, loss->nick_lossy || loss->ot_lossy, PDNA_SIDECAR_LOSS_NAME);

  y += PDNA_SIDECAR_LOSS_ROW_H / 2;
  ui_ptext_fit(4, y, UI_SCR_W - 8, UI_DIM, PDNA_SIDECAR_LOSS_KEPT_L1); y += PDNA_SIDECAR_LOSS_ROW_H;
  ui_ptext_fit(4, y, UI_SCR_W - 8, UI_DIM, PDNA_SIDECAR_LOSS_KEPT_L2); y += PDNA_SIDECAR_LOSS_ROW_H;
  ui_ptext_fit(4, y, UI_SCR_W - 8, UI_DIM, PDNA_SIDECAR_LOSS_STAYS);   y += PDNA_SIDECAR_LOSS_ROW_H;
  y += PDNA_SIDECAR_LOSS_ROW_H / 2;
  ui_text(4, y, UI_TEXT, PDNA_SIDECAR_LOSS_A_TRANSFER); y += PDNA_SIDECAR_LOSS_ROW_H;
  ui_text(4, y, UI_DIM,  PDNA_SIDECAR_LOSS_B_CANCEL);

  u16 k = s_wait(KEY_A | KEY_B);
  return (k & KEY_A) != 0;
}

/* Best-effort cleanup after gbs_insert()/gb_persist() refused a paste whose sidecar
 * entry already landed on the card: remove that entry (it is always the LAST one --
 * gbsc_add() appends, and nothing else touches this file between gb_paste_write()'s own
 * write and this call running). An orphan entry left behind is harmless (design doc
 * section 5: "an unclaimed entry nobody will ever match"), so a failure here only logs,
 * it never blocks -- the caller has already decided what to tell the user about the
 * paste itself. */
static void gb_paste_sidecar_undo(const char* path) {
  uint32_t len = 0;
  if (sf_read_full(path, g_ed->sidecar, GBSC_FILE_MAX, &len) != SF_OK) {
    log_line("gen12: sidecar cleanup: could not re-read %s", path);
    return;
  }
  int n = gbsc_count(g_ed->sidecar, len);
  if (n <= 0) { log_line("gen12: sidecar cleanup: %s is empty", path); return; }

  if (gbsc_remove(g_ed->sidecar, &len, n - 1) != 0) {
    log_line("gen12: sidecar cleanup: remove failed in %s", path);
    return;
  }
  rmbl_pause();
  SfStatus wst;
  if (gbsc_count(g_ed->sidecar, len) == 0) wst = (f_unlink(path) == FR_OK) ? SF_OK : SF_ERR_WRITE;
  else                                     wst = sf_write_verified(path, g_ed->sidecar, len);
  rmbl_resume();
  if (wst != SF_OK) log_line("gen12: sidecar cleanup: rewrite failed for %s", path);
}

/* Steps 6-8 of gb_paste_hook (below): the sidecar, then gbs_insert(), then the card.
 * noinline: GbscEntry (~120 B) + the 48-byte path together are exactly the kind of
 * "sidecar I/O" weight the S5-B brief calls out as needing its own frame, same
 * reasoning as gb_persist's bak[SF_PATH_MAX] split. */
static bool __attribute__((noinline)) gb_paste_write(const GbEditMon* mon, int box) {
  uint8_t dv4[4] = {
    gb_get_dv(mon, GB_ATK), gb_get_dv(mon, GB_DEF),
    gb_get_dv(mon, GB_SPE), gb_get_dv(mon, GB_SPC)
  };
  uint64_t key = gbsc_key(mon->gen, gb_get_otid(mon), dv4, mon->otname);
  char path[48];
  if (gbsc_path(path, sizeof path, PDNA_GEN12_SIDECAR_DIR, key) < 0) return false;

  f_mkdir(PDNA_GEN12_SIDECAR_DIR);            /* FR_EXIST is fine (hard rule 9) */

  uint32_t len = 0;
  SfStatus rst = sf_read_full(path, g_ed->sidecar, GBSC_FILE_MAX, &len);
  if (rst != SF_OK || gbsc_count(g_ed->sidecar, len) < 0)
    len = (uint32_t)gbsc_init(g_ed->sidecar, key);   /* absent or corrupt: start fresh */

  GbaRtcTime t;
  uint32_t epoch = 0;
  if (gba_rtc_get(&t))                              /* 0 when absent -- gb_sidecar.h's own note */
    epoch = ((uint32_t)(t.year - 2000u) << 26) | ((uint32_t)t.month << 22) |
            ((uint32_t)t.day << 17) | ((uint32_t)t.hour << 12) |
            ((uint32_t)t.minute << 6) | (uint32_t)t.second;

  GbscEntry e;
  gbsc_entry_from(&e, mon, app_clip_rec(), epoch);
  int idx = gbsc_add(g_ed->sidecar, &len, GBSC_FILE_MAX, &e);
  if (idx < 0) {
    snd_deny();
    msg_wait(PDNA_SIDECAR_FULL_TITLE, UI_WARN, PDNA_SIDECAR_FULL_L1, 0);
    return false;
  }

  rmbl_pause();
  SfStatus wst = sf_write_verified(path, g_ed->sidecar, len);
  rmbl_resume();
  log_line("gen12: sidecar %s: %s", path, wst == SF_OK ? "OK" : sf_status_str(wst));
  if (wst != SF_OK) {
    snd_error();
    msg_wait(PDNA_SIDECAR_NOTWRITTEN_TITLE, UI_WARN, sf_status_str(wst), PDNA_SIDECAR_NOTWRITTEN_L2);
    return false;
  }

  int newslot = -1;
  GbsStatus ist = gbs_insert(&g_ed->s, box, mon, &newslot, g_ed->list);
  if (ist != GBS_OK) {
    gb_rollback();
    log_line("gen12: paste insert box %d refused: %s", box, gbs_status_text(ist));
    gb_paste_sidecar_undo(path);
    snd_error();
    msg_wait(PDNA_GBEDIT_REFUSED_TITLE, UI_WARN, gbs_status_text(ist), PDNA_GBEDIT_UNCHANGED_L2);
    return false;
  }

  log_line("=== gb paste -> %s box %d slot %d ===", g_ed->path, box, newslot);
  bool ok = gb_persist("paste");           /* on failure gb_persist() has already rolled
                                             * RAM back to what the card holds */
  if (!ok) gb_paste_sidecar_undo(path);
  return ok;
}

/* AppSrcOps.paste: convert the CLIPBOARD's Gen-3 record and append it into `rec80`'s
 * box (an empty cell -- app_mon_menu's own gate: g_clip.occupied && !g_clip.from_gb).
 * Order, each refusal leaving nothing PAST it touched:
 *   1. locate (box + the S2/S3 gates, gb_locate)
 *   2. Gen 1 refused outright -- no base-stat table in this tree yet (S5-C)
 *   3. gen3_to_gb() -- species/move/Egg refusals
 *   4. the loss screen (A = continue, B = cancel: nothing touched)
 *   5. gbs_box_writable() re-checked fresh (gb_locate's own check is against the box
 *      as it stood when the popup opened; cheap, and every other hook does the same)
 *   6-8. gb_paste_write(): the sidecar (verified, written FIRST -- design doc section 5
 *      point 3), gbs_insert(), then the card (gb_persist). */
static bool gb_paste_hook(uint8_t* rec80) {
  int box, slot;
  if (!gb_locate(rec80, &box, &slot)) return false;                          /* 1 */

  if (g_ed->s.gen == GB_GEN1) {                                              /* 2 */
    snd_deny();
    msg_wait(PDNA_SIDECAR_GEN1_TITLE, UI_WARN, PDNA_SIDECAR_GEN1_L1, 0);
    return false;
  }

  GbEditMon mon;
  Gen3ToGbLoss loss;
  G3GbStatus cst = gen3_to_gb(app_clip_rec(), GB_GEN2, NULL, &mon, &loss);   /* 3 */
  if (cst != G3GB_OK) {
    snd_deny();
    msg_wait(PDNA_SIDECAR_XFER_TITLE, UI_WARN, g3gb_status_text(cst), 0);
    return false;
  }

  if (!gb_paste_loss_screen(&loss)) return false;                           /* 4 */

  GbsStatus wst = gbs_box_writable(&g_ed->s, box);                          /* 5 */
  if (wst != GBS_OK) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_BOXWR_TITLE, UI_WARN, gbs_status_text(wst),
             wst == GBS_ERR_UNWRITABLE ? PDNA_GBEDIT_UNWRITABLE_HINT : 0);
    return false;
  }

  return gb_paste_write(&mon, box);                                         /* 6-8 */
}

/* The card, for all FOUR hooks (edit/move/release, and S5-B's paste). Same two safe
 * points as the Gen-3 path (pdna_main.c app_commit): the motor is off the bus for both
 * transfers, and the log is flushed after the verdict. Split out of the hooks so `bak`
 * (SF_PATH_MAX, 272 B) is not live in any of their frames while the editor/picker/
 * confirm sub-screens run. `what_for_log` is the one-word tag ("edit"/"move"/"release"/
 * "paste") the calling hook already logged its own detailed line under, just here to
 * keep THIS function's lines identifiable too. */
static bool gb_persist(const char* what_for_log) {
  char bak[SF_PATH_MAX]; bak[0] = 0;
  s_busy(PDNA_GBEDIT_BUSY_BACKUP);
  rmbl_pause();
  SfStatus bst = sf_backup_rolling(g_ed->path, bak, sizeof bak);
  rmbl_resume();
  if (bst != SF_OK) {
    log_line("gb %s: backup failed (%s)", what_for_log, sf_status_str(bst));
    app_log_flush();
    gb_rollback();
    snd_error();
    msg_wait(PDNA_GBEDIT_BACKUPFAIL_TITLE, UI_WARN, sf_status_str(bst), PDNA_GBEDIT_DISCARDED_L2);
    return false;
  }
  s_busy(PDNA_GBEDIT_BUSY_WRITING);
  rmbl_pause();
  SfStatus wst = sf_write_verified(g_ed->path, g_ed->img, g_ed->len);
  rmbl_resume();
  log_line("gb %s: write %s (backup %s)", what_for_log, wst == SF_OK ? "OK" : sf_status_str(wst), bak);
  app_log_flush();
  if (wst == SF_ERR_RENAME) {
    /* The bytes were written AND read back byte-for-byte -- it is the final swap the
     * card did not keep, a different piece of news from "the write failed". Ask the
     * card which file the user is actually holding rather than guessing, same as the
     * Gen-3 path (pdna_main.c app_commit). */
    const char* nm = strrchr(g_ed->path, '/');
    nm = nm ? nm + 1 : g_ed->path;
    char l1[64];
    SfWhere w = sf_where_are_the_bytes(g_ed->path, g_ed->img, g_ed->len);
    log_line("gb %s: rename unconfirmed, bytes are at %d (%s)", what_for_log, (int)w, nm);
    app_log_flush();
    if (w == SF_WHERE_TARGET) {                     /* it IS on the card; only unconfirmed */
      siprintf(l1, "%.30s looks correct", nm);
      msg_wait(PDNA_GBEDIT_UNCONFIRMED_TITLE, UI_WARN, l1, PDNA_GBEDIT_UNCONFIRMED_L2);
      /* fall through: the card really holds the new image, so this is a success */
    } else {
      gb_rollback();
      snd_error();
      switch (w) {
        case SF_WHERE_TMP_ONLY:                     /* the loud one: no .sav on the card */
          siprintf(l1, "Edit is in %.28s.tmp", nm);
          msg_wait(PDNA_GBEDIT_TMPONLY_TITLE, UI_WARN, l1, PDNA_GBEDIT_TMPONLY_L2);
          break;
        case SF_WHERE_TMP_AND_OLD:                  /* old save intact; edit not applied */
          siprintf(l1, "Edit is in %.28s.tmp", nm);
          msg_wait(PDNA_GBEDIT_TMPANDOLD_TITLE, UI_WARN, l1, PDNA_GBEDIT_TMPANDOLD_L2);
          break;
        default:                                    /* neither name matches: use the backup */
          msg_wait(PDNA_GBEDIT_SAVELOST_TITLE, UI_WARN, PDNA_GBEDIT_SAVELOST_L1,
                   bak[0] ? PDNA_GBEDIT_SAVELOST_BAK : PDNA_GBEDIT_SAVELOST_NOBAK);
          break;
      }
      return false;
    }
  } else if (wst != SF_OK) {
    gb_rollback();
    snd_error();
    msg_wait(PDNA_GBEDIT_WRITEFAIL_TITLE, UI_WARN, sf_status_str(wst), PDNA_GBEDIT_DISCARDED_L2);
    return false;
  }
  memcpy(g_ed->pristine, g_ed->img, g_ed->len);   /* the card now holds this image */
  gb_census(g_m);                                 /* nready/nblocked/the exit report may have changed */
  g_m->loaded = -1;                               /* the grid re-pages from the new bytes */
  snd_save();
  return true;
}

/* Arena block for a session whose bytes are already resident: no FIL, and the image
 * pointer is the caller's (g_save), so only the mount + the two GB buffers are borrowed. */
#define GB12_ARENA_NEED_IMG (GB12_A4(sizeof(Gb12Mount)) + GB12_A4(sizeof(Gb12Image)) + \
                             GB12_RECS_BYTES + GB12_STAGE_BYTES + GB12_A4(sizeof(Gb12Edit)) + 4u)
_Static_assert(GB12_ARENA_NEED_IMG <= APP_ARENA_BYTES,
               "GB import (resident image) no longer fits the borrowed EWRAM arena");

/* S2/S3/S5-B: the resident-image edit pipeline's hooks, registered as one const struct
 * (pdna_app.h's AppSrcOps) rather than five separate setters -- const data lives in
 * ROM, so this costs nothing against the EWRAM guard. */
static const AppSrcOps k_gb_ops = {
  gb_edit_hook, gb_move_hook, gb_release_hook, gb_copy_native_hook, gb_paste_hook
};

/* Info page -> box grid -> the "these did not convert" report. The whole session above
 * the mount, shared by both entry points: the only difference between opening a GB save
 * from a loaded Gen-3 save's nav menu and opening one straight off the file browser is
 * WHERE THE BYTES COME FROM, and that difference lives entirely in the read callback. */
static void gb_session_core(Gb12Mount* m) {
  rmbl_fire(RCUE_ROOM);
  if (!gb_info_page(m)) return;              /* B on the info page = never entered */
  /* Tell app_mon_menu that the ACTIVE box source is read-only, so its destructive
   * actions (PASTE / DUPLICATE / CREATE) are not offered on a source that cannot
   * accept them. This also suppresses the bank's deferred-delete bookkeeping for these
   * boxes — see pdna_main.c. Cleared unconditionally below; every exit from the box
   * screen passes through it. */
  app_src_readonly_set(pdna_gen12_why_locked, "Converted copy");
  if (g_ed) app_src_ops_set(&k_gb_ops);      /* S2/S3: EDIT / MOVE TO / RELEASE */
  BoxSource s = pdna_gen12_source(m);
  /* Returns 0 on B / the SAVE tab, 5 when the cursor drops off the bottom row (the
   * PC<->Bank hand-off, which has no PC to hand off to here) — re-enter on the top
   * tabs so DOWN puts the user back in the grid instead of silently exiting. */
  while (pdna_box(&s) != 0) app_box_start_set(1);
  app_src_readonly_clear();
  pdna_gen12_source(0);                      /* unmount: no dangling arena pointers */
  if (m->nblocked || m->nunreadable) gb_report_page(m);
}

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

  gb_session_core(m);

  f_close(f);
  app_arena_release();
  return 0;
}

/* Same session over bytes the caller has already read. `img` must stay put and stay
 * unchanged for the whole call (the mount pages boxes out of it on demand); `len` is
 * the FILE length, RTC tail included, which is what pdna_gen12_size_is_gb() accepted.
 *
 * Returns GB12_ENTER_* so the file browser can tell "that was not a GB save after all"
 * (fall through to the Gen-3 error the caller was about to print) from "it mounted and
 * the user has now backed out of it" — a size test alone cannot distinguish a Gen-1/2
 * save from any other 32 KiB file, and printing "not a valid Gen-3 .sav" over a file we
 * never even tried to parse as Gen-3 would be a lie in the other direction. */
int pdna_gen12_show_image(const char* path, uint8_t* img, uint32_t len,
                          uint8_t* pristine, uint8_t met_game) {
  if (!img || !pdna_gen12_size_is_gb(len)) return GB12_ENTER_NOT_GB;
  if (pristine && len > GB12_PRISTINE_OFF) pristine = 0;   /* cannot both fit: read-only */

  uint8_t* arena = app_arena_acquire(GB12_ARENA_NEED_IMG);
  if (!arena) {
    ui_clear();
    snd_deny();
    s_msg("NOT NOW", UI_WARN, "Save the Pokemon you moved,", "then open the GB save.");
    return GB12_ENTER_BUSY;
  }

  uint32_t base   = GB12_A4((uint32_t)(uintptr_t)arena);
  Gb12Mount* m    = (Gb12Mount*)(uintptr_t)base;
  Gb12Image* ic   = (Gb12Image*)(uintptr_t)(base + GB12_A4(sizeof(Gb12Mount)));
  uint8_t* recs   = (uint8_t*)(uintptr_t)(base + GB12_A4(sizeof(Gb12Mount)) + GB12_A4(sizeof(Gb12Image)));
  uint8_t* stage  = recs + GB12_RECS_BYTES;
  Gb12Edit* ed    = (Gb12Edit*)(uintptr_t)(stage + GB12_STAGE_BYTES);

  ic->img = img;
  ic->len = len;

  const char* why = 0;
  if (!pdna_gen12_mount(m, gb_img_read, ic, len, recs, stage, met_game, &why)) {
    log_line("gen12: %s is %lu bytes but did not mount: %s",
             path ? path : "(image)", (unsigned long)len, why ? why : "?");
    app_arena_release();
    return GB12_ENTER_NOT_GB;                /* the caller says what it is not */
  }
  log_line("gen12: %s mounted from RAM (%s) %d mons, %d ready, %d locked, %d bad",
           path ? path : "(image)", pdna_gen12_kind_name(m->kind), m->nstored,
           m->nready, m->nblocked, m->nunreadable);

  /* S2: the editing session over the same bytes. gbs_open runs its own identification
   * (Gen 2 first, then Gen 1); the mount just succeeded on the same image, so a refusal
   * here is news worth logging — and the session simply stays read-only. */
  g_ed = 0;
  if (pristine && path) {
    memcpy(pristine, img, len);
    GbsStatus st = gbs_open(&ed->s, img, len, ed->scratch, sizeof ed->scratch);
    if (st == GBS_OK && (ed->s.gen == GB_GEN1) == (m->kind == GB12_SAVE_RBY)) {
      ed->img = img; ed->pristine = pristine; ed->len = len; ed->path = path;
      g_ed = ed;
    } else {
      log_line("gen12: edit session refused (%s, gen %d vs mount kind %d): read-only",
               gbs_status_text(st), ed->s.gen, (int)m->kind);
    }
  }

  gb_session_core(m);

  g_ed = 0;                                       /* the arena block is about to go */
  app_arena_release();
  return GB12_ENTER_OK;
}

#endif /* PDNA_GEN12_HOST */
