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
 * shown as an empty box rather than as an error the user cannot act on.
 *
 * BACKLOG #45: the ONLY writer of m->stage, so also the only place allowed to say
 * m->staged is true -- defaults to -1 (unknown) up front and is only set to `box`
 * once the read below has actually succeeded, so a caller that checks it after any
 * early return sees "not staged", never a stale leftover claim. */
static bool gb_list_read(Gb12Mount* m, int box, int* cap, int* count) {
  uint32_t off = 0, bytes = 0;
  *cap = 0; *count = 0;
  m->staged = -1;
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
  m->staged = (int8_t)box;                           /* m->stage genuinely holds this box now */
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
 * search), so mounting stays a couple of SD reads per box.
 *
 * WARNING (BACKLOG #45): this walks EVERY box through gb_list_read(), which means
 * m->stage and m->staged end this loop holding whatever box was scanned LAST
 * (m->party_box), not whatever box a caller might expect -- and m->loaded is never
 * touched here at all. Called at mount time (before any box has paged in, so
 * harmless) and once more after a mid-session edit write-back (immediately followed
 * by `m->loaded = -1` there, which forces the grid to re-page and self-heals
 * `loaded`, but does nothing for m->staged -- it is left pointing at party_box until
 * the next gb_list_read()). Do not add a third call site without re-reading this. */
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
  m->staged = -1;                             /* BACKLOG #45: stage holds nothing yet */
  m->ui_box = -1;                             /* BACKLOG #56: no box switch yet this mount */
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
  /* BACKLOG #40(b): "GB BOX1", no space before the number -- Gen 1 always falls
   * through to this synthesized name (it has none of its own); Gen 2 only reaches it
   * when the save's own box name is empty. Before this fix Gen 1 read "GB BOX 1"
   * (a space) while a REAL Gen-2 box name of "BOX1" (Crystal's own default, above)
   * read "GB BOX1" — two spellings for what is meant to look like the same thing. */
  put_str(out, 12, &pos, "BOX");
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

/* BACKLOG #40(a): the box banner's own occupancy denominator (see pdna_box.h's
 * BoxSource.capacity) — the same gen1_list_capacity/g2_list_capacity pair
 * gb_list_read() above already reads, so this can never disagree with what the
 * grid actually pages in. 0 for an out-of-range box (draw_box_banner never asks
 * for one; defensive only). */
static int gbsrc_capacity(int box) {
  if (!g_m || box < 0 || box > g_m->party_box) return 0;
  return (g_m->kind == GB12_SAVE_RBY) ? gen1_list_capacity(box) : g2_list_capacity(box);
}

/* BACKLOG #56: BoxSource.note_box -- fired by pdna_box()'s SWITCH_BOX on every box
 * the grid puts on screen (is_bank does not gate this the way it gates
 * app_note_pc_box; that call stays PC-only, this one exists only because this
 * source sets it). Stores into ui_box, NOT current_box -- see that field's comment
 * for why the two must not be conflated. */
static void gbsrc_note_box(int box) {
  if (g_m && box >= 0 && box <= g_m->party_box) g_m->ui_box = box;
}

Gb12SaveKind pdna_gen12_active_kind(void) { return g_m ? g_m->kind : GB12_SAVE_NONE; }

BoxSource pdna_gen12_source(Gb12Mount* m) {
  BoxSource s;
  memset(&s, 0, sizeof s);
  s.last_box_is_party = true;   /* the party pseudo-box at nboxes-1 gets no banner ordinal */
  g_m = m;
  if (!m || m->kind == GB12_SAVE_NONE) return s;
  s.nboxes     = pdna_gen12_nboxes(m);
  /* BACKLOG #56: a re-entry (gb_session_core calling this again after the START
   * menu, or the bank-hand-off edge) has a ui_box the user actually left the
   * cursor on; a fresh mount's first call has ui_box == -1 and falls back to
   * current_box exactly as before this fix. */
  s.start_box  = (m->ui_box >= 0 && m->ui_box < s.nboxes) ? m->ui_box
               : (m->current_box >= 0 && m->current_box < s.nboxes) ? m->current_box : 0;
  s.is_bank    = true;                       /* see the header: this is what removes the
                                              * PARTY tab and the PC hand-off edges (codes
                                              * 1/4) -- neither applies to a raw GB save's
                                              * own box */
  s.has_start  = true;                       /* BACKLOG #48: is_bank also suppresses START
                                              * by default (see pdna_box.h's has_start
                                              * comment) -- opt back in: this box IS the
                                              * top-level screen for the visit, with no PC
                                              * to back out to first, so START must still
                                              * open the (GB-limited) nav menu. */
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
  s.note_box   = gbsrc_note_box;              /* BACKLOG #56: remember the box for re-entry */
  s.capacity   = gbsrc_capacity;
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
#include "sprite_era.h"    /* SE_KIND_GEN1/SE_KIND_GEN2 -- BACKLOG #58's app_nav_refuse */
#include "gb_session.h"
#include "gb_editor.h"
#include "gen2_save.h"     /* G1 review LOW-5: g2_unown_dv_for_letter (CREATE's Unown letter) */
#include "pdna_gbedit.h"
#include "pdna_gbsummary.h"   /* BACKLOG #41: the native VIEW/EDIT summary */
#include "pdna_gbtrainer.h"   /* BACKLOG #49 P1b: the Gen-1/2 trainer card */
#include "pdna_gbbag.h"       /* U4, BACKLOG #67: Red/Yellow's own Item bag */
#include "pdna_gbpack.h"      /* U5, BACKLOG #67: Gold/Silver/Crystal's own Pack */
#include "pdna_gbmap.h"       /* M1, BACKLOG #91: Gen 1's read-only current-map view */
#include "pdna_pick.h"        /* BACKLOG #92: pick_item / pick_item_set_gen1_2_max */
#include "pdna_layout.h"   /* PDNA_GBEDIT_* / PDNA_SIDECAR_* -- fixed strings         */
#include "gb_sidecar.h"    /* S5-B: the sidecar format + gbsc_path/gbsc_key            */
#include "gen3_to_gb.h"    /* S5-B: the Gen-3 -> Game Boy down converter               */
#include "gba_rtc.h"       /* S5-B: the sidecar entry's transfer-time RTC stamp        */
#include "rom_gbsprite.h"  /* S5-C: locates BaseStats in the user's own Gen-1 ROM      */
#include "rom_gblearn.h"   /* BACKLOG #50: level-up learnsets + min-level for CREATE   */
#include "gb_new_mon.h"    /* BACKLOG #50: gb_new_mon/gb_new_mon_g1_moves for CREATE   */
#include "pdna_pick.h"     /* BACKLOG #50 UX-parity: pick_species(), the Gen-3 picker  */
#include "rom_gbbase.h"    /* S5-C: decodes the 28-byte BaseStats row rom_gbsprite found;
                            * pk_national_no (internal index -> National Dex) comes from
                            * data_tables.h, already included at the top of this file. */
#include "pdna_origin_art.h"  /* BACKLOG #53a: pdna_origin_box_set_hint, PDNA_GEN1/GEN2 */
#include "icon_store.h"    /* E6 D4: icon_store_borrow(false), the every-exit backstop */
#include "fused_gb.h"       /* BACKLOG #62: delta-gb's cart-space ROM in gb_create_locate_rom */

/* S5-B review fix #10: PDNA_SIDECAR_DIR now lives in pdna_app.h (included above), not
 * duplicated as a local literal here. */

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

#ifdef PDNA_DELTA
/* BACKLOG #62: CREATE's own ROM lookup (gb_create_locate_rom/base1/base2/learn below)
 * reads a fused Game Boy ROM out of cartridge space instead of a FatFs FIL under
 * PDNA_DELTA (no SD there at all). One slice, set once by gb_create_locate_rom and
 * read by every later call in the same CREATE run -- mirrors how g_ed->romspath/
 * romfil persist across gb_create_base1/base2/gb_create_learn on the SD path, just
 * without a filesystem underneath. File-scope (not in the arena): a plain {pointer,
 * length} pair costs nothing worth budgeting against EWRAM either way. */
static FusedGbSlice s_gb_create_slice;
#endif

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
  /* S5-B review fix #9: computed ONCE per screen entry, not once per repaint -- the
   * loop below repaints on every key (including a round trip through
   * gb_report_page() and back via SELECT), and nothing this screen can do changes a
   * sidecar file, so the count can never go stale while it is up. */
  int sidecar_here = gb_sidecar_here_count(m);

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
    /* sidecar_here was computed ONCE above, before this loop -- see that computation's
     * own comment and gb_sidecar_here_count()'s for the "which box" reasoning. */
    if (sidecar_here >= 0) {
      char l2[40]; int pos2 = 0;
      put_uint(l2, (int)sizeof l2, &pos2, (unsigned)sidecar_here);
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
  /* S5-C Part B1: the Gen-1 BASE-STAT source, read live off the user's own ROM
   * (<save's dir>/<save's basename>.gb or .gbc) when PASTE (GB) targets a Gen-1
   * save -- gb_gen1_base_from_rom()'s own scratch, never that noinline helper's
   * stack (its OWN frame must stay < 300 B, same as gb_reconcile_on_load's, per
   * this slice's brief). romgs/romfil/romscan mirror exactly what
   * rom_gbsprite_open() asks a caller to own (rom_gbsprite.h: "No new statics
   * anywhere. The two buffers are the caller's"); romspath holds the derived ROM
   * path so no path buffer ever lives on that helper's stack either. */
  RomGbSprite romgs;
  FIL         romfil;
  uint8_t     romscan[ROM_GBSPRITE_SCRATCH_MIN];
  char        romspath[SF_PATH_MAX];
  /* S5-C review fix #6b: rom_gbsprite_open()'s table-locating scan streams the WHOLE
   * ROM once (rom_gbsprite.h: "1 MB (Gen 1) ... in ONE pass") -- re-running it on
   * every single Gen-1 paste this session is pure waste once the tables are already
   * known. romgs_ready caches "romgs already holds a valid, located Gen-1 ROM";
   * romgs_path remembers WHICH path that was (a pointer compare against g_ed->path,
   * which the mount's own contract already promises outlives the session -- see
   * pdna_gen12_show_image's own comment on `path`), so a changed save (a fresh
   * pdna_gen12_show_image() call, a different g_ed->path) re-scans rather than
   * silently reusing another ROM's tables. Explicitly cleared to false wherever a
   * new g_ed is latched (never left as whatever garbage the borrowed arena held). */
  bool        romgs_ready;
  const char* romgs_path;
  /* G1 review MEDIUM-2 (2026-09-08): CREATE's own learnset-table scan
   * (rom_gblearn_open, gb_create_learn below) is a SEPARATE full-ROM pass from
   * romgs's own -- measured, ~118,000 (Gold.gbc) / ~185,000 (Crystal.gbc)
   * read() calls before this cache and the 64-B block-read fix in
   * rom_gblearn.c's full_pointer_shape_ok. Cached the SAME way romgs_ready/
   * romgs_path are, for the identical reason and with the identical
   * limitation (keyed on g_ed->path, not on which ROM app_gb_rom_path()/the
   * beside-the-save fallback actually resolved to -- see romgs_path's own
   * comment): a second CREATE in the same session skips rom_gblearn_open's
   * scan entirely. `learn.read`/`learn.ctx` are NOT part of the cached
   * identity (a fresh FIL is opened per gb_create_learn call regardless) --
   * only `table_off`/`data_bank`/`banks`/`size`/`gen`, the part the scan
   * exists to find, are trusted across calls. Deliberately NOT invalidated by
   * gb_create_locate_rom the way romgs_ready now is (MEDIUM-1): that fix
   * exists because CREATE overwrites romgs/romspath out from under a
   * DIFFERENT consumer's (PASTE's) cache; this cache belongs to CREATE
   * itself, and invalidating it at the top of every create would defeat the
   * whole point of caching across two creates in one session. */
  RomGbLearn  learn;
  bool        learn_ready;
  const char* learn_path;
  uint32_t    learn_rom_id;   /* romgs.id_hash of the ROM the cached table was located
                               * in: the SAVE path alone is not a key -- Settings can
                               * re-register a different same-generation ROM mid-session
                               * (G1 re-verify: a stale table built one wrong moveset). */
} Gb12Edit;
static Gb12Edit* g_ed;        /* pointer only: the block itself lives in the arena */

static void s_busy(const char* line) {
  ui_clear();
  ui_panel(16, 60, 208, 48, UI_PANEL, UI_WARN);
  ui_text(28, 70, UI_WARN, PDNA_GBEDIT_BUSY_SAVING);
  ui_text(28, 88, UI_TEXT, line);
}

/* G1 review MEDIUM-2: CREATE's own busy screen, NOT s_busy() -- see
 * PDNA_GBCREATE_BUSY_TITLE's own comment (pdna_layout.h) for why "Saving - do
 * not power off" does not apply to a pure ROM read. Same panel shape. */
static void s_busy_reading(void) {
  ui_clear();
  ui_panel(16, 60, 208, 48, UI_PANEL, UI_WARN);
  ui_text(28, 70, UI_WARN, PDNA_GBCREATE_BUSY_TITLE);
  ui_text(28, 88, UI_TEXT, PDNA_GBCREATE_BUSY_LINE);
}

/* The card refused; put RAM back to what the card holds so the grid never shows an
 * edit that did not land, and re-latch the session over the restored bytes. Also the
 * documented recovery for gbs_move()'s own atomicity contract (gb_session.h): a failure
 * on the SOURCE half of a move can leave the destination half already committed in RAM,
 * and this is the only way back to a state the card actually holds. */
void gb_rollback(void) {
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
bool __attribute__((noinline)) gb_persist(const char* what_for_log);

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

/* AppSrcOps.editable (bag/menu review fix): the read-only popup's VIEW/EDIT row asks
 * this BEFORE it labels itself, so the label never promises more than gb_view_hook's
 * own `can_edit` (below) will actually allow once opened. Same two gates as gb_locate's
 * 1/2 above -- the cart (app_can_edit) and the box (gbs_box_writable, a virgin Gen-1
 * bank can never be written) -- but silent: this is a menu-drawing query, not an action,
 * so it must never pop a message box or make a sound. Requires an open edit session for
 * the same reason EDIT/MOVE/RELEASE do (no GbSession to gate against otherwise); k_gb_ops_ro
 * (the nav-menu mount, g_ed NULL) leaves this NULL. */
/* BACKLOG #95 (gbmon re-verify C1): the capture record at 0x1D/0x1E is CRYSTAL-only;
 * gb_load() leaves has_caught false by construction, so every entry point that hands a
 * record to the editor/summary must say whether this session is Crystal. The resident
 * session knows from its detected save version; the nested mount from its kind. */
static void gb_mark_caught(GbEditMon* e, uint8_t gen) {
  if (gen != GB_GEN2) return;
  bool crystal = g_ed ? gb_session_is_crystal(&g_ed->s)
                      : (g_m && g_m->kind == GB12_SAVE_CRYSTAL);
  gb_set_caught_available(e, crystal);
}

static bool gb_editable_hook(const uint8_t* rec80) {
  int box, slot;
  if (!g_ed) return false;
  if (!gb_locate_addr(rec80, &box, &slot)) return false;
  return app_can_edit() && gbs_box_writable(&g_ed->s, box) == GBS_OK;
}

/* AppSrcOps.copy_native (S5-B): capture the record in its own Game Boy shape for the
 * clipboard, not the lossy Gen-3-converted bytes the grid shows. Nothing here mutates
 * the image or requires app_can_edit(): "copying is allowed on any cart". Two sources
 * of the raw bytes, because this hook is reachable from BOTH GB entry points:
 *   - g_ed set (the picker's resident-image path): a real GbSession exists, so read
 *     through it exactly as the S2/S3 hooks do.
 *   - g_ed NULL (S5-B review fix #5, BLOCKING: the read-only nav-menu mount, which
 *     streams boxes from a FIL and never builds a GbSession at all -- COPY there was
 *     silently, permanently lossy before this fix, with no notice). This path is NOT
 *     actually missing the raw bytes: pdna_gen12_page() already staged `box`'s list
 *     blob into g_m->stage via gb_list_read() the moment the grid paged to it. This
 *     hook is only ever reached on a slot app_mon_menu already found OCCUPIED, which
 *     (gb_build_slot's own contract) can only be true because gb_list_read()
 *     genuinely succeeded for THIS box at some point -- but BACKLOG #45: gb_census()
 *     reuses the SAME m->stage buffer to scan every box (mount time, and again after
 *     a mid-session edit write-back) without ever touching m->loaded, so "the box
 *     the grid last paged" and "the box m->stage currently holds" CAN disagree by
 *     the time this hook runs. m->staged (set only by gb_list_read(), to the exact
 *     box it just wrote) is what actually answers "is m->stage fresh for `box`" --
 *     re-stage before trusting it instead of assuming gb_locate_addr()'s box
 *     (derived from m->loaded) still matches. */
/* Forward-declared: defined below, and gb_copy_native_hook() (S5-B re-verification
 * NEW-3) needs it to fill its own `has_sidecar` out-param. Does NOT require an open
 * edit session (g_ed) -- it only needs the record's own key and does its own f_stat --
 * so it works identically on both GB entry points, exactly like the copy itself now
 * does (review fix #5). */
static bool __attribute__((noinline)) gb_has_sidecar(uint8_t gen, const GbEditMon* mon);

/* `has_sidecar` (may be NULL) is set to false up front and only set true after a
 * successful load whose sidecar actually exists -- so a caller that only wants the
 * record can still pass NULL, and a caller that wants the toast-honesty flag (S5-B
 * re-verification NEW-3: app_copy()'s "lossless" claim used to fire for every GB mon,
 * including Gen-1 mons and Gen-2 mons that were never transferred down, neither of
 * which actually pastes losslessly) always gets a real answer, never a stale one from
 * a previous call. */
static bool gb_copy_native_hook(const uint8_t* rec80, GbEditMon* out, bool* has_sidecar) {
  if (has_sidecar) *has_sidecar = false;
  int box, slot;
  if (!gb_locate_addr(rec80, &box, &slot)) return false;

  bool ok;
  uint8_t gen;
  if (g_ed) {
    GbSession* s = &g_ed->s;
    if (gbs_load_list(s, box, g_ed->list) != GBS_OK) return false;
    if (slot >= gb_list_count(s->gen, g_ed->list, box)) return false;
    gen = s->gen;
    ok = gb_load(out, gen, g_ed->list, box, slot);
    if (ok) gb_mark_caught(out, gen);
  } else {
    if (!g_m || !g_m->stage) return false;
    gen = (g_m->kind == GB12_SAVE_RBY) ? GB_GEN1 : GB_GEN2;
    /* BACKLOG #45: re-stage rather than trust a possibly-stale m->stage -- see the
     * hook's own header comment. A failed re-stage means the box genuinely could
     * not be read right now; fail the copy rather than read garbage. */
    if (g_m->staged != box) {
      int rcap = 0, rcount = 0;
      if (!gb_list_read(g_m, box, &rcap, &rcount)) return false;
    }
    if (slot >= gb_list_count(gen, g_m->stage, box)) return false;
    ok = gb_load(out, gen, g_m->stage, box, slot);
    if (ok) gb_mark_caught(out, gen);
  }
  if (ok && has_sidecar) *has_sidecar = gb_has_sidecar(gen, out);
  return ok;
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
  char path[GBSC_PATH_MAX];
  if (gbsc_path(path, sizeof path, PDNA_SIDECAR_DIR, key) < 0) return false;
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
 * gbsrc_records() actually runs), so `m->loaded` cannot answer this. `m->current_box`
 * CAN: the mount already knows "the box whose LIVE copy is in main data" (its own
 * field comment) at mount time, and the grid lands on it via start_box -- so it is the
 * box the player is actually about to see, not an arbitrary stand-in. Only falls back
 * to box 0 if `current_box` is itself out of range (defensive; not expected in
 * practice). S5-B review fix #9 (corrects an earlier revision that always used box 0
 * here, calling it a "deliberate" substitute for something m->current_box already
 * tracked).
 *
 * Bounded: gb_list_capacity's own party (6) + one box (<= G2's 20, well under 30)
 * f_stat calls, at most -- and CALLED ONCE by gb_info_page() (hoisted out of its own
 * repaint loop, review fix #9), not once per repaint. */
static int gb_sidecar_here_count(const Gb12Mount* m) {
  if (!g_ed || g_ed->s.gen != GB_GEN2) return -1;

  int boxes[2]; int nb = 0;
  int box0 = (m->loaded >= 0) ? m->loaded
           : ((m->current_box >= 0 && m->current_box < m->nboxes) ? m->current_box : 0);
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
/* Steps 3-5 of the S2 edit contract, factored out so BOTH the read-only nav menu's
 * EDIT row (gb_edit_hook, below) and the new VIEW-opens-the-native-summary row
 * (gb_view_hook, BACKLOG #41) commit through the exact same path rather than two
 * copies drifting apart:
 *   3. the record  (gb_commit_checked: the bytes landed where the editor put them);
 *   4. the image   (gbs_commit_list: the engine's own structural + verify gates);
 *   5. the card    (gb_persist: sf_backup_rolling, then sf_write_verified's steps).
 * `what_for_log` is the one-word tag gb_persist's own lines use ("edit"/"view"). */
static bool gb_edit_commit(int box, int slot, const GbEditMon* e, const char* what_for_log) {
  GbSession* s = &g_ed->s;
  if (!gb_commit_checked(e, g_ed->list, box, slot)) {                        /* 3 */
    snd_error();
    msg_wait(PDNA_GBEDIT_REFUSED_TITLE, UI_WARN, PDNA_GBEDIT_NOVERIFY_L1, PDNA_GBEDIT_NOTHING_L2);
    return false;
  }
  GbsStatus st = gbs_commit_list(s, box, g_ed->list);                        /* 4 */
  if (st != GBS_OK) {
    gb_rollback();
    log_line("gen12: %s commit box %d slot %d refused: %s", what_for_log, box, slot, gbs_status_text(st));
    snd_error();
    msg_wait(PDNA_GBEDIT_REFUSED_TITLE, UI_WARN, gbs_status_text(st), PDNA_GBEDIT_UNCHANGED_L2);
    return false;
  }

  log_line("=== gb %s commit -> %s box %d slot %d ===", what_for_log, g_ed->path, box, slot);
  return gb_persist(what_for_log);                                          /* 5 */
}

/* gb_edit_hook (the AppSrcOps.edit hook: EDIT on the read-only mon menu) was retired by
 * BACKLOG #48's Gen-3-parity change (pdna_main.c: the standalone EDIT row is gone, its
 * only caller) and removed here (stack-budget p84b item 2): its address was still taken
 * by k_gb_ops's `.edit = gb_edit_hook` below even with zero call sites, which is exactly
 * the "assigned but never dispatched" shape the addrtaken-ok exemption existed to cover.
 * Deleting the assignment and the function means nothing takes its address, so no
 * exemption is needed. VIEW (gb_view_hook) already reaches the same summary screen in
 * edit mode when app_can_edit() allows it -- see pdna_main.c's Gen-3-parity comment. */

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
    /* BACKLOG #40(d): gbs_status_text(GBS_ERR_FULL) says "that box is full", which
     * reads oddly for the party -- nobody calls their party a box. Same status, a
     * destination-specific L1 instead. */
    const char* l1 = (st == GBS_ERR_FULL && gb_box_is_party(g_ed->s.gen, dst))
                    ? PDNA_GBEDIT_MOVE_PARTYFULL_L1 : gbs_status_text(st);
    msg_wait(PDNA_GBEDIT_MOVE_REFUSED_TITLE, UI_WARN, l1, hint);
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
  y = loss_row(y, loss->item_dropped || loss->secret_id, PDNA_SIDECAR_LOSS_ITEMSECRET);
  /* S5-B review fix #4: two of Gen3ToGbLoss's 17 flags had no row at all before this --
   * merged with item/secret_id above (row-count-neutral: was 2 separate rows, now 1 +
   * this 1, still 10 conditional rows total, matching the screen's own 2px-slack fit). */
  y = loss_row(y, loss->pokerus_dropped || loss->friendship_dropped, PDNA_SIDECAR_LOSS_POKERUS);
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
  char path[GBSC_PATH_MAX];
  if (gbsc_path(path, sizeof path, PDNA_SIDECAR_DIR, key) < 0) return false;

  /* S5-B review fix #10: PDNA_SIDECAR_MKDIR_TITLE was measured but never wired up --
   * f_mkdir's result was silently discarded. FR_EXIST is the expected steady state
   * (every transfer after the first); anything else means the write below cannot
   * possibly land, so say so now rather than let sf_write_verified fail later with a
   * less specific message. */
  FRESULT mkr = f_mkdir(PDNA_SIDECAR_DIR);
  if (mkr != FR_OK && mkr != FR_EXIST) {
    log_line("gen12: sidecar mkdir %s failed (%d)", PDNA_SIDECAR_DIR, (int)mkr);
    snd_error();
    msg_wait(PDNA_SIDECAR_MKDIR_TITLE, UI_WARN, PDNA_SIDECAR_NOTWRITTEN_L2, 0);
    return false;
  }

  uint32_t len = 0;
  SfStatus rst = sf_read_full(path, g_ed->sidecar, GBSC_FILE_MAX, &len);
  if (rst == SF_OK && gbsc_count(g_ed->sidecar, len) < 0) {
    /* S5-B review fix #3 (BLOCKING): the file EXISTS but fails its own CRC -- up to
     * GBSC_MAX_ENTRIES-1 OTHER mons' ORIGINAL Gen-3 records live in those bytes.
     * gbsc_init()-ing straight over them (the old behaviour here) would DESTROY every
     * one. Rename the corrupt file aside instead -- preserved, in case a future tool
     * can recover it -- and only THEN start fresh; overwriting an older .bad is
     * deliberate (this is already the second corruption of the same key, so there is
     * nothing more to preserve by keeping the first .bad too). */
    char badpath[GBSC_PATH_MAX + 4];
    int bp = 0;
    while (path[bp] && bp < GBSC_PATH_MAX - 1) { badpath[bp] = path[bp]; bp++; }
    badpath[bp++] = '.'; badpath[bp++] = 'b'; badpath[bp++] = 'a'; badpath[bp++] = 'd';
    badpath[bp] = 0;
    f_unlink(badpath);
    FRESULT rr = f_rename(path, badpath);
    log_line("gen12: sidecar %s failed its CRC, renamed to %s (%s)",
             path, badpath, rr == FR_OK ? "OK" : "FAILED");
    if (rr != FR_OK) {
      /* The corrupt bytes are still at `path` -- refuse rather than risk
       * gbsc_init()+sf_write_verified() overwriting them a moment later. */
      snd_error();
      msg_wait(PDNA_SIDECAR_CORRUPT_TITLE, UI_WARN, PDNA_SIDECAR_NOTWRITTEN_L2, 0);
      return false;
    }
    snd_deny();
    msg_wait(PDNA_SIDECAR_CORRUPT_TITLE, UI_WARN, PDNA_SIDECAR_CORRUPT_KEPT_L1, 0);
    len = (uint32_t)gbsc_init(g_ed->sidecar, key);
  } else if (rst == SF_ERR_OPEN) {
    len = (uint32_t)gbsc_init(g_ed->sidecar, key);   /* genuinely absent (or unopenable) */
  } else if (rst != SF_OK) {
    /* S5-B re-verification NEW-2 (must): SF_ERR_OPEN is the ONLY status that means
     * "no such file" -- sf_read_full() also returns SF_ERR_READ for a failed/short
     * read of a file that DOES exist (savefile.c). The old `rst != SF_OK` catch-all
     * treated a transient card fault on a VALID .pds exactly like "absent" and went on
     * to gbsc_init() + sf_write_verified() over it, destroying up to
     * GBSC_MAX_ENTRIES-1 other mons' originals for a fault that might not even recur
     * on retry. Refuse instead -- nothing is touched. */
    snd_error();
    msg_wait(PDNA_SIDECAR_READFAIL_TITLE, UI_WARN, sf_status_str(rst), PDNA_SIDECAR_NOTWRITTEN_L2);
    return false;
  }

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
    msg_wait(PDNA_SIDECAR_FULL_TITLE, UI_WARN, PDNA_SIDECAR_FULL_L1, PDNA_SIDECAR_NOTWRITTEN_L2);
    return false;
  }
  /* S5-C review #3: GBSC_FLAG_KEEP_ASKED is a whole-FILE flag ("gb_reconcile_
   * on_load already asked about every entry in this file and the user chose
   * KEEP") -- it must not silence a NEW entry this same key just gained. Without
   * this, one "keep both" on an OLDER transfer would permanently hide every LATER
   * transfer to the same OT/DVs/name (the sidecar key), reconcile-on-load included,
   * with no way for the user to ever be asked about it again. */
  uint16_t flags = gbsc_flags_get(g_ed->sidecar, len);
  if (flags & GBSC_FLAG_KEEP_ASKED) gbsc_flags_set(g_ed->sidecar, len, flags & ~GBSC_FLAG_KEEP_ASKED);

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
    msg_wait(PDNA_SIDECAR_XFER_REFUSED_TITLE, UI_WARN, gbs_status_text(ist), PDNA_GBEDIT_UNCHANGED_L2);
    return false;
  }

  log_line("=== gb paste -> %s box %d slot %d ===", g_ed->path, box, newslot);
  /* #62 review D2: "on failure gb_persist() has already rolled RAM back to what the
   * card holds" is true of the card-write (#else) half only -- the PDNA_DELTA half
   * REFUSES but KEEPS the edit for the rest of this session (there is no card to roll
   * back to; see gb_persist's own PDNA_DELTA branch). */
  bool ok = gb_persist("paste");
  if (!ok) gb_paste_sidecar_undo(path);
  return ok;
}

/* S5-C Part B1: Gen-1 targets. gen3_to_gb() itself already refuses everything a
 * missing base table has nothing to do with (egg, bad species/move, a corrupt
 * record) BEFORE it ever looks at g1base -- gen3_to_gb.c's screen() checks those in
 * that order and only then does `if (gen == GB_GEN1 && !g1base) return
 * G3GB_ERR_NEEDS_BASE;`. So gb_paste_hook below tries the conversion with no base
 * table first and only reaches for the ROM on that ONE specific refusal, rather
 * than duplicating gen3_to_gb's own gates here. */
typedef enum {
  GB1BASE_OK = 0,
  GB1BASE_NO_ROM,    /* neither <base>.gb nor <base>.gbc sits beside the .sav       */
  GB1BASE_BAD_ROM    /* opened, but not a valid Gen-1 ROM, or the dex row is bad    */
} Gb1BaseStatus;

/* Just the dex number PASTE (GB) needs to look up base stats for -- PkMon (~90 B) is
 * exactly the kind of weight this slice's brief says must not ride in gb_paste_hook's
 * own frame, so it gets its own noinline frame instead. 0 (an impossible dex) on any
 * decode failure or an Egg, which have no base stats to fetch and are about to be
 * refused by gen3_to_gb's own EGG/GLITCH checks the moment the caller retries it. */
static uint16_t __attribute__((noinline)) gb_clip_dex(void) {
  PkMon m;
  if (!pk_decode_mon(app_clip_rec(), false, &m) || m.isEgg || m.isBadEgg) return 0;
  return pk_national_no(m.species);
}

/* Derive "<save's dir><save's basename>" (no extension) from g_ed->path into
 * g_ed->romspath -- arena-resident, so the path never lives on any function's own
 * stack. Truncates (never overflows) if the source path is implausibly long. */
static void gb_rom_base_path(void) {
  const char* p = g_ed->path ? g_ed->path : "";
  int len = 0; while (p[len]) len++;
  int slash = -1, dot = -1;
  for (int i = 0; i < len; i++) { if (p[i] == '/') slash = i; if (p[i] == '.') dot = i; }
  int baselen = (dot > slash) ? dot : len;          /* strip the LAST extension only */
  if (baselen > (int)sizeof(g_ed->romspath) - 5) baselen = (int)sizeof(g_ed->romspath) - 5;
  memcpy(g_ed->romspath, p, (size_t)baselen);
  g_ed->romspath[baselen] = 0;
}

/* Locate a Gen-1 ROM's tables -- the part of gb_gen1_base_from_rom() worth caching
 * (S5-C review fix #6b): rom_gbsprite_open() streams the WHOLE ROM once through
 * romscan to find them (rom_gbsprite.h: "1 MB (Gen 1) ... in ONE pass"), so redoing
 * it on every single Gen-1 paste this session was pure waste once g_ed->romgs
 * already holds a valid result. Derives "<base>.gb" then "<base>.gbc" from
 * g_ed->path into g_ed->romspath, opens it read-only over g_ed->romfil (arena-
 * resident: a FIL is ~600 B and does not belong on any stack), and locates it into
 * g_ed->romgs/romscan. Sets g_ed->romgs_ready/romgs_path on success. Every buffer
 * this touches is a Gb12Edit field, precisely so THIS function's own frame stays
 * under the 300 B this slice's brief measures for.
 *
 * gb_art_source.c's gb_rom_path_beside() is the SAME "<base>.gb then .gbc" probe,
 * independently implemented (not refactored to share this one, which is
 * arena-resident, paste-path code this slice deliberately does not touch) for
 * origin-art's "ROM beside the save" fallback -- if the extension-derivation rule
 * ever changes, it has to change in both places. */
static Gb1BaseStatus __attribute__((noinline)) gb_gen1_locate_rom(void) {
  gb_rom_base_path();
  int baselen = 0; while (g_ed->romspath[baselen]) baselen++;

  static const char* const kExt[2] = { ".gb", ".gbc" };
  FRESULT fr = FR_NO_FILE;
  for (int e = 0; e < 2; e++) {
    int bp = baselen;
    const char* ext = kExt[e];
    for (int i = 0; ext[i] && bp < (int)sizeof(g_ed->romspath) - 1; i++)
      g_ed->romspath[bp++] = ext[i];
    g_ed->romspath[bp] = 0;
    memset(&g_ed->romfil, 0, sizeof g_ed->romfil);
    fr = f_open(&g_ed->romfil, g_ed->romspath, FA_READ);
    if (fr == FR_OK) break;
    g_ed->romspath[baselen] = 0;                    /* back to the bare base for the next try */
  }
  if (fr != FR_OK) {
    log_line("gen12: gen-1 rom: neither .gb nor .gbc beside %s", g_ed->path);
    return GB1BASE_NO_ROM;
  }
  log_line("gen12: gen-1 rom candidate %s", g_ed->romspath);

  FSIZE_t fsz = f_size(&g_ed->romfil);
  uint32_t sz = (fsz > (FSIZE_t)0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)fsz;
  int ok = rom_gbsprite_open(&g_ed->romgs, gb_read, &g_ed->romfil, sz,
                             g_ed->romscan, sizeof g_ed->romscan);
  f_close(&g_ed->romfil);
  if (!ok || g_ed->romgs.gen != GB_ROM_GEN1) {
    log_line("gen12: gen-1 rom: %s did not open as a Gen-1 ROM", g_ed->romspath);
    return GB1BASE_BAD_ROM;
  }
  g_ed->romgs_ready = true;
  g_ed->romgs_path = g_ed->path;
  return GB1BASE_OK;
}

/* Read dex's 28-byte BaseStats row out of the ROM g_ed->romgs already located --
 * re-locating it first (gb_gen1_locate_rom(), the expensive part) only when the
 * cache is cold or the save's path changed. `out` is untouched unless this returns
 * GB1BASE_OK. noinline for the same 300 B budget as every other helper here. */
static Gb1BaseStatus __attribute__((noinline))
gb_gen1_base_from_rom(uint16_t dex, GbGen1Base* out) {
  if (!g_ed || !out || dex < 1) return GB1BASE_BAD_ROM;

  if (!g_ed->romgs_ready || g_ed->romgs_path != g_ed->path) {
    g_ed->romgs_ready = false;
    Gb1BaseStatus lst = gb_gen1_locate_rom();
    if (lst != GB1BASE_OK) return lst;
  }

  /* rom_gbbase_gen1() needs the file open again for its own reads (through the SAME
   * gb_read() shim) -- reopened by the already-resolved g_ed->romspath rather than
   * held open across pastes, so a later paste never finds a FIL left in a state it
   * did not itself create. */
  memset(&g_ed->romfil, 0, sizeof g_ed->romfil);
  if (f_open(&g_ed->romfil, g_ed->romspath, FA_READ) != FR_OK) {
    log_line("gen12: gen-1 rom: %s could not be reopened", g_ed->romspath);
    g_ed->romgs_ready = false;              /* the card changed underneath us -- rescan next time */
    return GB1BASE_BAD_ROM;
  }
  RomGb1Species sp;
  bool got = rom_gbbase_gen1(&g_ed->romgs, gb_read, &g_ed->romfil, dex, &sp);
  f_close(&g_ed->romfil);
  if (!got) {
    log_line("gen12: gen-1 rom: %s has no readable row for dex %u", g_ed->romspath, dex);
    return GB1BASE_BAD_ROM;
  }
  log_line("gen12: gen-1 base stats: dex %u from %s (base_stats offset 0x%lX)",
           dex, g_ed->romspath, (unsigned long)g_ed->romgs.base_stats);
  *out = sp.base;
  return GB1BASE_OK;
}

/* The ROM was absent: name it. base_only points INTO the arena-resident romspath
 * (still holding "<dir><base>" after gb_gen1_base_from_rom's own failure above), so
 * this needs no path buffer of its own -- only the short printf-staging one, which
 * (like every message string in this tree) is safe by construction: msg_wait()
 * itself runs l1 through ui_ptext_fit(), so a long path is clipped, never overflowed. */
static void __attribute__((noinline)) gb_gen1_norom_msg(void) {
  const char* base_only = g_ed->romspath;
  for (int i = 0; g_ed->romspath[i]; i++)
    if (g_ed->romspath[i] == '/') base_only = g_ed->romspath + i + 1;
  /* S5-C review fix #6c: this used to hard-truncate the basename at 10 characters --
   * cutting off the very name the message tells the user to CREATE. The pixel-width
   * clamp is msg_wait()'s own ui_ptext_fit() (same as every other dynamic message in
   * this tree); %.48s here only bounds the STRING build against l1's own size, wide
   * enough that no real ROM filename is cut before the screen ever gets a chance to
   * ellipsize it visually. */
  char l1[64];
  siprintf(l1, "Put %.48s.gb here", base_only);
  msg_wait(PDNA_SIDECAR_GEN1_TITLE, UI_WARN, l1, PDNA_SIDECAR_GEN1_L1);
}

/* AppSrcOps.paste: convert the CLIPBOARD's Gen-3 record and append it into `rec80`'s
 * box (an empty cell -- app_mon_menu's own gate: g_clip.occupied && !g_clip.from_gb).
 * Order, each refusal leaving nothing PAST it touched:
 *   1. locate (box + the S2/S3 gates, gb_locate)
 *   2-3. gen3_to_gb() -- species/move/Egg refusals; a Gen-1 target that needs base
 *      stats retries once with the user's own ROM's table (S5-C)
 *   4. the loss screen (A = continue, B = cancel: nothing touched)
 *   5. gbs_box_writable() re-checked fresh (gb_locate's own check is against the box
 *      as it stood when the popup opened; cheap, and every other hook does the same)
 *   6-8. gb_paste_write(): the sidecar (verified, written FIRST -- design doc section 5
 *      point 3), gbs_insert(), then the card (gb_persist). */
static bool gb_paste_hook(uint8_t* rec80) {
  int box, slot;
  if (!gb_locate(rec80, &box, &slot)) return false;                          /* 1 */

  /* S5-B re-verification NEW-1 (must): the party pseudo-box IS a grid box (nboxes ==
   * party_box + 1), so app_src_paste_offered() opened PASTE (GB) on an empty PARTY
   * cell too -- gbs_box_writable() says OK for the Gen-2 party and the capacity
   * pre-check below passes (g2_list_capacity(party) == 6), so without this guard
   * gb_paste_write() would write the sidecar to the CARD and only THEN have
   * gbs_insert() refuse with GBS_ERR_ARG (it only ever inserts BOX-kind records into
   * storage boxes -- gb_session.h's own contract), triggering a rollback + best-effort
   * sidecar-undo on every single attempt. Landing a converted mon in the party needs
   * species-limit/live-stat/Mail rules gbs_insert() deliberately does not have; that is
   * gbs_move()'s job, and S5-C's. Refused here, before ANYTHING (including the loss
   * screen) runs. */
  if (gb_box_is_party(g_ed->s.gen, box)) {
    snd_deny();
    msg_wait(PDNA_SIDECAR_XFER_TITLE, UI_WARN, PDNA_SIDECAR_PARTY_L1, 0);
    return false;
  }

  GbEditMon mon;
  Gen3ToGbLoss loss;
  /* BACKLOG #95 review C11: the capture record at 0x1D/0x1E is Crystal-only real data;
   * gb_session_is_crystal() is the single source of truth this and gb_mark_caught both
   * call, so the live editor and this synthesis path cannot disagree about which target
   * saves get a synthetic Met record. */
  bool crystal = gb_session_is_crystal(&g_ed->s);
  G3GbStatus cst = gen3_to_gb(app_clip_rec(), g_ed->s.gen, crystal, NULL, &mon, &loss);   /* 2 */
  if (cst == G3GB_ERR_NEEDS_BASE) {          /* Gen 1 only -- everything else about this
                                              * mon already checked out (gen3_to_gb.c's
                                              * screen() reaches this check LAST) */
    uint16_t dex = gb_clip_dex();
    GbGen1Base g1base;
    Gb1BaseStatus bst = dex ? gb_gen1_base_from_rom(dex, &g1base) : GB1BASE_BAD_ROM;
    if (bst == GB1BASE_NO_ROM) {
      snd_deny();
      gb_gen1_norom_msg();
      return false;
    }
    if (bst != GB1BASE_OK) {
      snd_deny();
      msg_wait(PDNA_SIDECAR_XFER_TITLE, UI_WARN, PDNA_SIDECAR_GEN1_BADROM_L1, 0);
      return false;
    }
    cst = gen3_to_gb(app_clip_rec(), g_ed->s.gen, crystal, &g1base, &mon, &loss);   /* 3 */
  }
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

  /* S5-B review fix (BLOCKING #1): the grid shows 30 cells but a GB box holds at most
   * gb_list_capacity() (20 for Gen 2) -- cells 20..29 always read empty on this source,
   * so without this check a paste attempted there would write the sidecar to the card
   * FIRST and only then have gbs_insert() refuse with GBS_ERR_FULL inside
   * gb_paste_write(), leaving an orphan sidecar entry behind on EVERY such attempt
   * rather than only on a genuine race. `g_ed->list` is reloaded a moment later by
   * gb_paste_write()'s own gbs_insert() -- cheap, and every other hook in this file
   * re-derives its own gates fresh the same way. */
  GbsStatus lst = gbs_load_list(&g_ed->s, box, g_ed->list);
  if (lst != GBS_OK) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_BOXRD_TITLE, UI_WARN, gbs_status_text(lst), 0);
    return false;
  }
  int cnt = gb_list_count(g_ed->s.gen, g_ed->list, box);
  if (cnt < 0) {
    snd_deny();
    msg_wait(PDNA_SIDECAR_XFER_REFUSED_TITLE, UI_WARN, gbs_status_text(GBS_ERR_STRUCT), 0);
    return false;
  }
  if (cnt >= gb_list_capacity(g_ed->s.gen, box)) {
    snd_deny();
    msg_wait(PDNA_SIDECAR_XFER_REFUSED_TITLE, UI_WARN, gbs_status_text(GBS_ERR_FULL),
             PDNA_GBEDIT_MOVE_FULL_L2);
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
bool gb_persist(const char* what_for_log) {
#ifdef PDNA_DELTA
  /* BACKLOG #62: this build's whole "save" is its own 128 KiB flash chip (already
   * holding the fused Gen-3 save, or blank) -- there is no SD path a GB image could
   * land on at all. Refuse plainly here rather than let sf_backup_rolling/
   * sf_write_verified fail below with a generic FatFs status string that would read
   * like a card error instead of the honest truth: GB edits in this build live only
   * as long as the current session, same posture the single-fused-GB boot fallback
   * already documents (view_save's own PDNA_DELTA GB fork). */
  (void)what_for_log;
  log_line("gb %s: refused (PDNA_DELTA has no SD for a GB image)", what_for_log);
  app_log_flush();
  /* #62 review D2 (BLOCKING): the refusal above is "no card to write to", not "this
   * edit is void" -- the edit already landed in g_ed->img (every caller applies its
   * edit to img BEFORE calling gb_persist). Re-baseline pristine to the post-edit
   * image and re-census so the grid shows what was just edited instead of pre-edit
   * bytes, and so a LATER gb_rollback() (a different edit failing further down this
   * same session) rolls back to this edit's own bytes, not discarding it. Without
   * this, the session was internally inconsistent: the grid painted post-edit bytes
   * while pristine/rollback still pointed at whatever the card last held. */
  memcpy(g_ed->pristine, g_ed->img, g_ed->len);
  gb_census(g_m);
  g_m->loaded = -1;
  snd_error();
  msg_wait("GAME BOY SAVE", UI_WARN, "Edits are in-session only",
           "in the emulator build.");
  return false;
#else
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
#endif /* PDNA_DELTA */
}

/* Arena block for a session whose bytes are already resident: no FIL, and the image
 * pointer is the caller's (g_save), so only the mount + the two GB buffers are borrowed. */
#define GB12_ARENA_NEED_IMG (GB12_A4(sizeof(Gb12Mount)) + GB12_A4(sizeof(Gb12Image)) + \
                             GB12_RECS_BYTES + GB12_STAGE_BYTES + GB12_A4(sizeof(Gb12Edit)) + 4u)
_Static_assert(GB12_ARENA_NEED_IMG <= APP_ARENA_BYTES,
               "GB import (resident image) no longer fits the borrowed EWRAM arena");

/* U2b item 0: the arena TAIL past Gb12Edit, for a GB screen shell (pdna_gbscreen.c)
 * riding the resident-image mount. pdna_gen12_show_image()'s own carve-out puts
 * Gb12Edit LAST in the block (Mount, Gb12Image, recs, stage, Gb12Edit -- see that
 * function, ~pdna_gen12.c:2671-2711) and nothing else in this file claims arena
 * bytes past it for the rest of the session: `ed`'s own end IS the arena's used
 * high-water mark. GB12_ARENA_NEED_IMG (~11,952 of APP_ARENA_BYTES' 35,712) already
 * counts every byte up to and including Gb12Edit, so the slack below (~23.7 KB) is
 * exactly what a caller may still take.
 *
 * `need <= slack` is the one runtime check; g_ed itself must be non-NULL (the
 * resident-image mount with a live edit session -- pdna_gbtrainer() is reachable
 * ONLY that way, never the read-only nav-menu FIL mount, whose own arena layout
 * (GB12_ARENA_NEED, above) has no Gb12Edit at all and this function correctly
 * refuses for). No allocation, no assert, no side effect -- a NULL return is an
 * ordinary "not available right now", same posture as app_arena_acquire() itself. */
/* U2b review item 0b: gb12_arena_tail() used to be a bare pointer -- any two
 * callers in the same visit (the shell's own cache AND, say, a future second
 * consumer) could unknowingly overlap the SAME bytes. One slice at a time: a
 * caller takes the tail, uses it for the whole time it is "open" (the shell +
 * U2c's own player-pic decode share ONE slice, taken once), then releases it
 * explicitly. Cleared beside EVERY `g_ed = 0` (pdna_gen12.c's own two sites)
 * so a stale `true` from a previous session's arena block can never survive
 * into a new one that never itself called the release fn. */
static bool g_tail_lent = false;

uint8_t* gb12_arena_tail(uint32_t need) {
  if (!g_ed) return NULL;
  if (g_tail_lent) return NULL;          /* already out on loan this visit    */
  if (GB12_ARENA_NEED_IMG > (uint32_t)APP_ARENA_BYTES) return NULL;   /* belt: the
                                            * _Static_assert above already forbids this
                                            * at compile time, but a caller must never
                                            * trust an unsigned subtraction that could
                                            * wrap if that ever regressed */
  uint32_t slack = (uint32_t)APP_ARENA_BYTES - (uint32_t)GB12_ARENA_NEED_IMG;
  if (need > slack) return NULL;
  g_tail_lent = true;
  return (uint8_t*)g_ed + GB12_A4(sizeof(Gb12Edit));
}

/* U2b review item 0b: give the slice back. Safe to call even when nothing was
 * ever lent (a plain no-op) -- same "no assert, no side effect on a mismatched
 * call" posture as app_arena_release() itself. */
void gb12_arena_tail_release(void) {
  g_tail_lent = false;
}

/* S2/S3/S5-B: the resident-image edit pipeline's hooks, registered as one const struct
 * (pdna_app.h's AppSrcOps) rather than five separate setters -- const data lives in
 * ROM, so this costs nothing against the EWRAM guard. */
/* app_src_ops_set() hook: VIEW on the read-only mon menu (BACKLOG #41). Replaces
 * pdna_inspect() on the lossy Gen-3-converted copy with pdna_gbsummary() over the
 * NATIVE record, for both generations. Unlike gb_edit_hook/gb_move_hook/
 * gb_release_hook this does NOT call gb_locate() -- it uses gb_locate_addr() alone,
 * the same address-only resolve gb_copy_native_hook uses, because viewing is free:
 * no cart gate, no box-writable gate (docs/GEN3-TO-GB-SIDECAR-DESIGN.md sec. 10's
 * "copying is allowed on any cart" reasoning applies just as well to looking). Only
 * `can_edit` -- whether the summary is even ALLOWED to open in edit mode -- checks
 * both of gb_locate()'s own gates by hand.
 *
 * U/D inside the summary asks for the next/prev mon; every slot 0..count-1 of a GB
 * list IS occupied by definition (gb_list_count()'s own contract: "slots 0..count-1
 * hold Pokemon; everything from count on is free space"), so "next occupied slot"
 * is just a wrapping +-1 over count, never a skip-empty-slots search.
 *
 * Two sources of the record, same shape as gb_copy_native_hook (bag/menu review fix,
 * the nav-menu-copy-lossy finding's sibling): an open edit session (g_ed set) reads
 * and can edit through the real GbSession exactly as before; no session (g_ed NULL,
 * the read-only nav-menu mount over a bare FIL) reads the box straight from the
 * mount's staged list (g_m->stage) and NEVER offers editing -- there is no GbSession
 * to commit an edit through, so `can_edit` is unconditionally false on this path
 * (gb_edit_commit dereferences g_ed unconditionally and would crash if it ever ran
 * here; it can't, because `saved` can only go true when pdna_gbsummary was opened
 * with can_edit true).
 *
 * BACKLOG #45: g_m->stage is only guaranteed fresh for the box g_m->staged names --
 * gb_census() reuses the same buffer to scan every box without touching g_m->loaded
 * (gb_copy_native_hook's own comment has the full story), so re-stage on a mismatch
 * rather than trust gb_locate_addr()'s box (from g_m->loaded) still agrees with
 * whatever g_m->stage happens to hold. */
static bool gb_view_hook(uint8_t* rec80) {
  int box, slot;
  if (!gb_locate_addr(rec80, &box, &slot)) return false;
  /* Review fix: this used to be `int card = 0;` INSIDE the loop, so every +1/-1 step to
   * the next/prev mon silently reset the card back to 0 (INFO) -- contradicting
   * pdna_gbsummary.h's own documented contract ("scrolling to the next mon stays on the
   * same card"). Hoisted so it persists across the whole browse, exactly like
   * pdna_inspect's own `card` in/out parameter. */
  int card = 0;

  for (;;) {
    GbEditMon e;
    uint8_t gen;
    bool can_edit;
    int count;
    if (g_ed) {
      GbSession* s = &g_ed->s;
      GbsStatus lst = gbs_load_list(s, box, g_ed->list);
      if (lst != GBS_OK) { snd_deny(); msg_wait(PDNA_GBEDIT_BOXRD_TITLE, UI_WARN, gbs_status_text(lst), 0); return false; }
      count = gb_list_count(s->gen, g_ed->list, box);
      if (count <= 0 || slot >= count) {
        snd_deny(); msg_wait(PDNA_GBEDIT_EMPTYSLOT_TITLE, UI_WARN, PDNA_GBEDIT_EMPTYSLOT_L1, 0); return false;
      }
      gen = s->gen;
      if (!gb_load(&e, gen, g_ed->list, box, slot)) { snd_deny(); return false; }
      gb_mark_caught(&e, gen);   /* the LIVE editor path (VIEW/EDIT -> summary -> editor), gbmon re-verify C8 */
      can_edit = app_can_edit() && gbs_box_writable(s, box) == GBS_OK;
    } else {
      if (!g_m || !g_m->stage) return false;
      gen = (g_m->kind == GB12_SAVE_RBY) ? GB_GEN1 : GB_GEN2;
      /* BACKLOG #45: re-stage rather than trust a possibly-stale m->stage. */
      if (g_m->staged != box) {
        int rcap = 0, rcount = 0;
        if (!gb_list_read(g_m, box, &rcap, &rcount)) return false;
      }
      count = gb_list_count(gen, g_m->stage, box);
      if (count <= 0 || slot >= count) {
        snd_deny(); msg_wait(PDNA_GBEDIT_EMPTYSLOT_TITLE, UI_WARN, PDNA_GBEDIT_EMPTYSLOT_L1, 0); return false;
      }
      if (!gb_load(&e, gen, g_m->stage, box, slot)) { snd_deny(); return false; }
      gb_mark_caught(&e, gen);   /* nested import: the same live path, gbmon re-verify C8 */
      can_edit = false;
    }
    bool has_sidecar = gb_has_sidecar(gen, &e);

    bool saved = false;
    int nav = pdna_gbsummary(&e, can_edit, false,
                             gen == GB_GEN1 ? "Gen 1 record" : "Gen 2 record",
                             has_sidecar, false, &saved, &card);
    if (saved && !gb_edit_commit(box, slot, &e, "view")) return false;
    if (nav == 0) return false;

    int dir = (nav > 0) ? 1 : -1;
    slot = (slot + dir + count) % count;
  }
}

/* ============================================================================
 * BACKLOG #50 -- CREATE: build a legal Pokemon from scratch into an empty cell.
 * ============================================================================
 * species -> level -> ROM-derived facts (base stats/growth/moveset) -> gb_new_mon
 * (pure C, source/gb_new_mon.c) -> the native summary (start_editing, so the
 * player can tweak DVs/moves/name before it lands) -> gbs_insert -> gb_persist.
 * Box/bank only: gbs_insert() itself refuses the party pseudo-box ("landing a
 * converted mon in the party belongs to gbs_move()", gb_session.h) -- the exact
 * limitation gb_paste_hook's own party guard above exists for, so this checks
 * gb_box_is_party() first, in the same place, with the same message. */

/* ROM lookup, independent of gb_gen1_locate_rom()/gb_gen1_base_from_rom() above
 * (PASTE (GB)'s own Gen-1-only, beside-the-save-only pair): CREATE additionally
 * tries the REGISTERED ROM (Settings > Game ROM, app_gb_rom_path()) first, and
 * works for either generation. Duplicating ~30 lines of open/scan control flow
 * costs less than generalizing a shipped, hardware-validated feature to do
 * something it never needed to. Shares the SAME arena-resident storage
 * (g_ed->romgs/romfil/romscan/romspath) safely: a session is always ONE
 * generation, so the two families are never in use at once.
 *
 * DOES NOT USE the cache (romgs_ready) itself -- CREATE is a rare, deliberate
 * action, not a per-paste hot path, so a fresh scan every time is the simpler
 * and safer choice -- but it DOES invalidate that cache first (romgs_ready =
 * false, below), because every branch here overwrites the very fields
 * (g_ed->romgs/romspath) the cache is a claim ABOUT (G1 review MEDIUM-1: an
 * earlier version left the flag standing over data this function had already
 * replaced). On success, g_ed->romgs holds a located ROM of exactly `want_gen`
 * and g_ed->romspath names it (the FIL itself is closed again -- every later
 * read reopens it by that path, same pattern gb_gen1_base_from_rom's own
 * re-open uses). */
static bool __attribute__((noinline)) gb_create_locate_rom(uint8_t want_gen) {
  GbRomGen want = (want_gen == GB_GEN1) ? GB_ROM_GEN1 : GB_ROM_GEN2;
  /* G1 review MEDIUM-1 (2026-09-08): this function's own header comment above used
   * to say "romgs_ready/romgs_path are left untouched, not read or written here" --
   * true of the FLAG, false of what it guards: every branch below overwrites
   * g_ed->romgs/romspath directly (the SAME fields gb_gen1_base_from_rom's cache
   * trusts, line ~1590), without ever invalidating romgs_ready first. A CREATE run
   * AFTER a Gen-1 PASTE had already cached a valid romgs_ready=true would leave the
   * flag standing over data CREATE just replaced -- the NEXT PASTE would then trust
   * stale/wrong-generation base stats as if they were still its own cache, never
   * re-scanning. Invalidate up front, unconditionally: CREATE is a rare action, so
   * the cost of the NEXT PASTE doing one extra fresh scan is negligible net of a
   * silent correctness bug. */
  g_ed->romgs_ready = false;

#ifdef PDNA_DELTA
  /* BACKLOG #62: no SD, no registered-path/beside-the-save fallback here -- the only
   * ROM this build can ever have for `want_gen` is whatever tools/fuse_gb.py fused
   * into the cartridge image. Reads go through s_gb_create_slice + fused_gb_slice_read
   * for the rest of this CREATE run (gb_create_base1/base2/gb_create_learn below). */
  const uint8_t* base; uint32_t size;
  if (!fused_gb_rom(want_gen, &base, &size)) {
    log_line("gen12 create: no fused gen-%u rom", want_gen);
    return false;
  }
  s_gb_create_slice.base = base;
  s_gb_create_slice.size = size;
  int ok = rom_gbsprite_open(&g_ed->romgs, fused_gb_slice_read, &s_gb_create_slice, size,
                             g_ed->romscan, sizeof g_ed->romscan);
  if (ok && g_ed->romgs.gen == want) {
    log_line("gen12 create: fused rom (gen %u)", want_gen);
    return true;
  }
  log_line("gen12 create: fused gen-%u rom did not open as that gen", want_gen);
  return false;
#else
  const char* reg = app_gb_rom_path(want_gen);
  if (reg && reg[0]) {
    int i = 0;
    for (; reg[i] && i < (int)sizeof(g_ed->romspath) - 1; i++) g_ed->romspath[i] = reg[i];
    g_ed->romspath[i] = 0;
    memset(&g_ed->romfil, 0, sizeof g_ed->romfil);
    if (f_open(&g_ed->romfil, g_ed->romspath, FA_READ) == FR_OK) {
      FSIZE_t fsz = f_size(&g_ed->romfil);
      uint32_t sz = (fsz > (FSIZE_t)0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)fsz;
      int ok = rom_gbsprite_open(&g_ed->romgs, gb_read, &g_ed->romfil, sz,
                                 g_ed->romscan, sizeof g_ed->romscan);
      f_close(&g_ed->romfil);
      if (ok && g_ed->romgs.gen == want) {
        log_line("gen12 create: registered rom %s (gen %u)", g_ed->romspath, want_gen);
        return true;
      }
      log_line("gen12 create: registered rom %s did not open as gen %u", g_ed->romspath, want_gen);
    }
  }

  gb_rom_base_path();
  int baselen = 0; while (g_ed->romspath[baselen]) baselen++;
  static const char* const kExt[2] = { ".gb", ".gbc" };
  for (int e = 0; e < 2; e++) {
    int bp = baselen;
    const char* ext = kExt[e];
    for (int i = 0; ext[i] && bp < (int)sizeof(g_ed->romspath) - 1; i++) g_ed->romspath[bp++] = ext[i];
    g_ed->romspath[bp] = 0;
    memset(&g_ed->romfil, 0, sizeof g_ed->romfil);
    if (f_open(&g_ed->romfil, g_ed->romspath, FA_READ) != FR_OK) { g_ed->romspath[baselen] = 0; continue; }
    FSIZE_t fsz = f_size(&g_ed->romfil);
    uint32_t sz = (fsz > (FSIZE_t)0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)fsz;
    int ok = rom_gbsprite_open(&g_ed->romgs, gb_read, &g_ed->romfil, sz,
                               g_ed->romscan, sizeof g_ed->romscan);
    f_close(&g_ed->romfil);
    if (ok && g_ed->romgs.gen == want) {
      log_line("gen12 create: rom beside the save %s (gen %u)", g_ed->romspath, want_gen);
      return true;
    }
    g_ed->romspath[baselen] = 0;
  }
  log_line("gen12 create: no gen-%u rom (registered or beside %s)", want_gen, g_ed->path ? g_ed->path : "?");
  return false;
#endif /* PDNA_DELTA */
}

/* Base stats/growth for `dex` off the ROM gb_create_locate_rom already located --
 * one small noinline frame per generation rather than one shared by a union, so
 * neither frame carries the other generation's struct. Reopens g_ed->romfil by
 * the already-resolved g_ed->romspath (same re-open pattern as gb_gen1_base_
 * from_rom above), closes it again before returning either way. */
static bool __attribute__((noinline)) gb_create_base1(uint16_t dex, RomGb1Species* out) {
#ifdef PDNA_DELTA
  return rom_gbbase_gen1(&g_ed->romgs, fused_gb_slice_read, &s_gb_create_slice, dex, out);
#else
  memset(&g_ed->romfil, 0, sizeof g_ed->romfil);
  if (f_open(&g_ed->romfil, g_ed->romspath, FA_READ) != FR_OK) return false;
  bool ok = rom_gbbase_gen1(&g_ed->romgs, gb_read, &g_ed->romfil, dex, out);
  f_close(&g_ed->romfil);
  return ok;
#endif
}
static bool __attribute__((noinline)) gb_create_base2(uint16_t dex, RomGb2Species* out) {
#ifdef PDNA_DELTA
  return rom_gbbase_gen2(&g_ed->romgs, fused_gb_slice_read, &s_gb_create_slice, dex, out);
#else
  memset(&g_ed->romfil, 0, sizeof g_ed->romfil);
  if (f_open(&g_ed->romfil, g_ed->romspath, FA_READ) != FR_OK) return false;
  bool ok = rom_gbbase_gen2(&g_ed->romgs, gb_read, &g_ed->romfil, dex, out);
  f_close(&g_ed->romfil);
  return ok;
#endif
}

/* Opens the ROM's learnset table ONCE and answers BOTH questions the create
 * flow needs from it (BACKLOG #50 UX-parity, Guy 2026-09-07): the species'
 * OWN lowest legal level -- no more separate level PROMPT, matching Gen 3's
 * own create flow (pdna_main.c's app_create_mon, gen3_build_level: "5 for a
 * Bulbasaur, 36 for a Charizard", "it must evolve to there") -- and its
 * level-up moveset at exactly that level. `g1_start` is rom_gbbase_gen1's own
 * start[4] for a Gen-1 target (merged in via rom_gblearn_moves_at_seeded --
 * gb_new_mon_g1_moves does the identical thing, duplicated here rather than
 * called because that helper wants an already-open RomGbLearn* and this
 * function is also where one gets opened); NULL for Gen 2, whose own table
 * already includes the starters. `*out_level` is only written on success.
 * Returns the move count filled (0..4), or -1 on any failure -- the ONE
 * refusal path left for the create flow's ROM dependency (used to be raised
 * by the old gb_create_moves() this replaces; the level computation itself
 * never refuses, see rom_gblearn_min_level()'s own "fails open" contract, so
 * it cannot newly introduce one here).
 *
 * G1 review MEDIUM-2 (2026-09-08): the table LOCATION (g_ed->learn, arena-
 * resident -- see its own struct comment for the cache contract) is reused
 * across calls in the same session instead of re-scanning the whole ROM every
 * time (rom_gblearn_open's own scan is what drove ~118,000-185,000 read()
 * calls per create, measured, before this fix). A fresh FIL is still opened
 * every call regardless -- the located table_off/data_bank are cheap facts
 * to trust across calls, an open file handle is not. */
static int __attribute__((noinline))
gb_create_learn(uint16_t dex, const uint8_t g1_start[4], uint8_t* out_level, uint8_t out4[4]) {
#ifdef PDNA_DELTA
  /* BACKLOG #62: same cache-hit shape as the SD path below, just against the fused
   * cart-space slice gb_create_locate_rom already set instead of a FIL. */
  int ok;
  if (g_ed->learn_ready && g_ed->learn_path == g_ed->path && g_ed->learn.gen == g_ed->s.gen &&
      g_ed->learn_rom_id == g_ed->romgs.id_hash) {
    ok = 1;
  } else {
    ok = rom_gblearn_open(&g_ed->learn, g_ed->s.gen, fused_gb_slice_read, &s_gb_create_slice,
                          g_ed->romgs.size);
    if (ok) { g_ed->learn_ready = true; g_ed->learn_path = g_ed->path;
              g_ed->learn_rom_id = g_ed->romgs.id_hash; }
  }
  g_ed->learn.read = fused_gb_slice_read;
  g_ed->learn.ctx = &s_gb_create_slice;
  int kept = -1;
  if (ok) {
    uint8_t lvl = rom_gblearn_min_level(&g_ed->learn, dex);
    kept = g1_start ? rom_gblearn_moves_at_seeded(&g_ed->learn, dex, lvl, g1_start, out4)
                    : rom_gblearn_moves_at(&g_ed->learn, dex, lvl, out4);
    if (kept >= 0 && out_level) *out_level = lvl;
  }
  return kept;
#else
  memset(&g_ed->romfil, 0, sizeof g_ed->romfil);
  if (f_open(&g_ed->romfil, g_ed->romspath, FA_READ) != FR_OK) return -1;
  int ok;
  if (g_ed->learn_ready && g_ed->learn_path == g_ed->path && g_ed->learn.gen == g_ed->s.gen &&
      g_ed->learn_rom_id == g_ed->romgs.id_hash) {
    ok = 1;                                    /* cache hit: same save, gen AND ROM */
  } else {
    ok = rom_gblearn_open(&g_ed->learn, g_ed->s.gen, gb_read, &g_ed->romfil, g_ed->romgs.size);
    if (ok) { g_ed->learn_ready = true; g_ed->learn_path = g_ed->path;
              g_ed->learn_rom_id = g_ed->romgs.id_hash; }
  }
  /* Either way, `read`/`ctx` must point at THIS call's freshly (re)opened FIL --
   * a cache hit skips the scan, never the fact that the old FIL is long closed. */
  g_ed->learn.read = gb_read;
  g_ed->learn.ctx = &g_ed->romfil;
  int kept = -1;
  if (ok) {
    uint8_t lvl = rom_gblearn_min_level(&g_ed->learn, dex);
    kept = g1_start ? rom_gblearn_moves_at_seeded(&g_ed->learn, dex, lvl, g1_start, out4)
                    : rom_gblearn_moves_at(&g_ed->learn, dex, lvl, out4);
    if (kept >= 0 && out_level) *out_level = lvl;
  }
  f_close(&g_ed->romfil);
  return kept;
#endif
}

/* AppSrcOps.create. Takes NO arguments -- see pdna_app.h's own comment on why
 * app_mon_menu's (box, slot) parameters cannot supply this correctly for an
 * is_bank source (every GB session): pdna_box.c always computes `mbox = 0` for
 * one, and gbs_insert() itself makes the other (the empty CELL the cursor was
 * on) irrelevant, since it always appends at the box's own next free slot.
 *
 * The box actually on screen is Gb12Mount.ui_box (BACKLOG #56, kept current by
 * every L/R box switch via BoxSource.note_box) -- with the EXACT SAME fallback
 * chain pdna_gen12_source()'s own start_box computation already uses for the
 * identical "not updated yet this visit" gap: ui_box (a switch happened) ->
 * current_box (the save's own "live copy" box, read at mount) -> 0. Caught by
 * hand on a real emulator screenshot before this fix existed: CREATE from box
 * 13 (17/20, real room) still refused "BOX FULL", because it was silently
 * targeting box 0 (20/20) instead. */
static bool gb_create_hook(void) {
  int box = (g_m->ui_box >= 0 && g_m->ui_box <= g_m->party_box) ? g_m->ui_box
          : (g_m->current_box >= 0 && g_m->current_box <= g_m->party_box) ? g_m->current_box
          : 0;
  if (gb_box_is_party(g_ed->s.gen, box)) {
    snd_deny();
    msg_wait(PDNA_GBCREATE_TITLE, UI_WARN, PDNA_SIDECAR_PARTY_L1, 0);
    return false;
  }
  GbsStatus wr = gbs_box_writable(&g_ed->s, box);
  if (wr != GBS_OK) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_BOXWR_TITLE, UI_WARN, gbs_status_text(wr),
             wr == GBS_ERR_UNWRITABLE ? PDNA_GBEDIT_UNWRITABLE_HINT : 0);
    return false;
  }
  GbsStatus ld = gbs_load_list(&g_ed->s, box, g_ed->list);
  if (ld != GBS_OK) {
    snd_deny(); msg_wait(PDNA_GBEDIT_BOXRD_TITLE, UI_WARN, gbs_status_text(ld), 0); return false;
  }
  int count = gb_list_count(g_ed->s.gen, g_ed->list, box);
  if (count < 0 || count >= gb_list_capacity(g_ed->s.gen, box)) {
    snd_deny(); msg_wait(PDNA_GBCREATE_FULL_TITLE, UI_WARN, PDNA_GBCREATE_FULL_L1, 0); return false;
  }

  /* Species picker: pdna_pick.c's own big icon-grid pick_species(), the EXACT
   * screen app_create_mon (pdna_main.c) opens for a Gen-3 create, restricted
   * to this session's own generation (BACKLOG #50 UX-parity, Guy 2026-09-07:
   * "Pokemon creation is not from the Pokedex view -- fix that") -- see
   * pick_species_set_max_dex()'s own header comment for why a file-static
   * ceiling, not a second picker, is what changed. The CANCEL/invalid check
   * mirrors app_create_mon's own `if (sp == 0xFFFF || sp == 0) return false;`
   * exactly (0xFFFF is pdna_pick.c's private CANCEL sentinel; a 0 species can
   * never be legally picked either, but is refused the same defensive way). */
  pick_species_set_max_dex(gb_max_species(g_ed->s.gen));
  uint16_t dex = pick_species(1);
  pick_species_set_max_dex(0);
  if (dex == 0xFFFFu || dex == 0) return false;

  /* G1 review LOW-5 (2026-09-08): the Unown letter is chosen the SAME place
   * and SAME way Gen 3's own create flow does (pdna_main.c's app_create_mon:
   * "THE UNOWN LETTER IS PART OF THE ROLL, so it has to be asked for BEFORE
   * it"), not left to whatever letter gb_new_mon's own random DVs happen to
   * land on. dex 201 (Unown) can only be picked at all in a Gen-2 session --
   * pick_species_set_max_dex(151) already excludes it from Gen 1's own list,
   * so no separate generation check is needed here. -1 (B in the prompt, or a
   * form Gen 2 cannot represent -- 26/27, "!"/"?", Gen-3-only) leaves the DVs
   * exactly as gb_new_mon() rolls them: "any letter", the same fallback
   * Gen 3's own B-in-the-prompt path uses. */
  int unown_letter = -1;
  if (dex == 201) {
    int form = pick_unown_form(0);
    if (form >= 0 && form <= 25) unown_letter = form;
  }

  /* G1 review MEDIUM-2: gb_create_locate_rom + gb_create_learn together freeze the
   * screen for a real full-ROM scan (up to ~185,000 read() calls, measured, before
   * the cache below makes a second create in this session skip it) -- show honest
   * feedback before either runs, not a still screen a player might mistake for a
   * hang. Stays up through base1/base2/gb_create_learn too: nothing between here
   * and the summary/refusal draws anything else. */
  s_busy_reading();
  if (!gb_create_locate_rom(g_ed->s.gen)) {
    snd_deny();
    msg_wait(PDNA_GBCREATE_TITLE, UI_WARN, PDNA_GBCREATE_NOROM_L1, PDNA_GBCREATE_NOROM_L2);
    return false;
  }

  GbNewMonSrc src; memset(&src, 0, sizeof src);
  uint8_t g1_start_buf[4]; const uint8_t* g1_start = NULL;
  if (g_ed->s.gen == GB_GEN1) {
    RomGb1Species sp;
    if (!gb_create_base1(dex, &sp)) {
      snd_deny(); msg_wait(PDNA_GBCREATE_TITLE, UI_WARN, PDNA_SIDECAR_GEN1_BADROM_L1, 0); return false;
    }
    memcpy(src.base, sp.base.base, GB_NSTATS);
    src.type1 = sp.base.type1; src.type2 = sp.base.type2;
    src.growth = sp.growth;
    memcpy(g1_start_buf, sp.start, 4);
    g1_start = g1_start_buf;
  } else {
    RomGb2Species sp;
    if (!gb_create_base2(dex, &sp)) {
      snd_deny(); msg_wait(PDNA_GBCREATE_TITLE, UI_WARN, PDNA_SIDECAR_GEN1_BADROM_L1, 0); return false;
    }
    src.growth = sp.growth;
  }

  /* No level PROMPT any more (BACKLOG #50 UX-parity): gb_create_learn()
   * computes the species' own lowest legal level (rom_gblearn_min_level, off
   * the SAME ROM) and its moveset at that level together, one ROM open. */
  uint8_t lvl = 5;
  int kept = gb_create_learn(dex, g1_start, &lvl, src.moves);
  if (kept < 0) {
    snd_deny();
    msg_wait(PDNA_GBCREATE_TITLE, UI_WARN, PDNA_GBCREATE_NOROM_L1, PDNA_GBCREATE_NOROM_L2);
    return false;
  }

  src.species_name = pk_species_name(dex);
  src.ot_name = (g_m && g_m->player[0]) ? g_m->player : 0;
  src.ot_id = g_m ? g_m->tid : 0;

  /* G1 review BLOCKING-2 (2026-09-08): qran() is libtonc's PRNG, whose seed is a
   * fixed constant (__qran_seed = 42) unless something calls sqran() -- nothing in
   * this tree does -- so every player's Nth created mon got IDENTICAL DVs/gender/
   * shininess. app_session_seed() is the SAME counter+TID+RTC entropy
   * app_create_mon (pdna_main.c) already seeds a Gen-3 create's PID/IVs from. */
  GbEditMon party_mon;
  if (!gb_new_mon(g_ed->s.gen, dex, lvl, &src, app_session_seed(), &party_mon)) {
    snd_deny();
    msg_wait(PDNA_GBCREATE_TITLE, UI_WARN, PDNA_GBCREATE_BUILDFAIL_L1, 0);
    return false;
  }
  /* G1 review LOW-5: override the four DVs gb_new_mon() just rolled with the
   * SPECIFIC quad that decodes (g2_unown_letter) to the letter chosen above --
   * a direct set, not a search, so this does not try to also land on a
   * shiny-capable quad (g2_unown_dv_for_letter's own comment). Re-settles
   * stats since the DVs (which feed the HP DV and, in principle, the derived
   * stats) just changed out from under gb_new_mon's own settle. */
  if (unown_letter >= 0) {
    uint8_t dv4[4];
    if (g2_unown_dv_for_letter((uint8_t)unown_letter, dv4)) {
      gb_set_dv(&party_mon, GB_ATK, dv4[0]);
      gb_set_dv(&party_mon, GB_DEF, dv4[1]);
      gb_set_dv(&party_mon, GB_SPE, dv4[2]);
      gb_set_dv(&party_mon, GB_SPC, dv4[3]);
      gbe_settle_stats(&party_mon);
    }
  }

  /* Party -> box, the SAME technique gbs_move() uses for a party->box move
   * (gb_session.c: "drop the extra bytes... exactly what the game's own deposit
   * does"): the box-shaped fields are a byte-identical PREFIX of the party-
   * shaped ones in both generations (gen1_save.h's 33 of 44, gen2_save.h's own
   * "PC record: species..level" / "+ status, HP and the 5 battle stats"), and
   * gb_new_mon() already synced Gen 1's box-level byte via gb_set_level() --
   * nothing left to do but reload the same bytes at the shorter length.
   * gbs_insert() requires exactly this shape (mon->is_party == false). */
  GbEditMon box_mon;
  if (!gb_load_parts(&box_mon, g_ed->s.gen, false, party_mon.rec,
                     party_mon.otname, party_mon.nick, party_mon.list_species))
    return false;

  bool saved = false; int card = 0;
  pdna_gbsummary(&box_mon, true, true, g_ed->s.gen == GB_GEN1 ? "Gen 1 record" : "Gen 2 record",
                false /* a freshly created mon can never already have a sidecar */,
                true /* BACKLOG #50 UX-parity: the NEW chip + START-keep confirm */,
                &saved, &card);
  if (!saved) return false;

  int slot_out = 0;
  GbsStatus ist = gbs_insert(&g_ed->s, box, &box_mon, &slot_out, g_ed->list);
  if (ist != GBS_OK) {
    gb_rollback();
    log_line("gen12: create insert box %d refused: %s", box, gbs_status_text(ist));
    snd_error();
    msg_wait(PDNA_GBEDIT_REFUSED_TITLE, UI_WARN, gbs_status_text(ist), PDNA_GBEDIT_UNCHANGED_L2);
    return false;
  }
  log_line("=== gb create -> %s box %d slot %d dex %u lv %d ===", g_ed->path, box, slot_out, dex, lvl);
  return gb_persist("create");
}

/* app_src_ops_set() hook: ITEM on the read-only mon menu (BACKLOG #92). Gen 2 only --
 * only k_gb_ops_gen2 below installs this; k_gb_ops_gen1 leaves it NULL, so the row
 * never appears for a Gen-1 mount rather than appearing and refusing every press
 * (pdna_app.h's own `item` comment). Mirrors Gen 3's own quick-item action
 * (app_quick_item, pdna_main.c): the SAME pick_item() screen gb_editor.c's own
 * GBE_ITEM row already opens (pdna_gbedit.c's GBE_K_ITEM branch), restricted to ids
 * 0..255 shown as "#n" (#0 = no item, the way to remove one) via pick_item_set_gen1_2_max() -- no separate legality gate:
 * any byte is structurally legal for this field (that branch's own comment: "Held
 * item has no move-style validation to fail"). Same load/commit shape as EDIT
 * (gb_edit_hook above), just loading one field's picker instead of opening the full
 * summary screen. */
static bool gb_item_hook(uint8_t* rec80) {
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

  pick_item_set_gen1_2_max(255);
  uint16_t id = pick_item(gb_get_held_item(&e));
  pick_item_set_gen1_2_max(0);
  if (id == 0xFFFF) return false;                      /* cancel */
  /* BACKLOG #95 review C5: an Egg cannot hold an item at all (pack.asm
   * AnEggCantHoldAnItemText) -- checked here, ahead of gb_set_held_item's own
   * refusal, purely to say WHY instead of a bare deny beep. */
  if (id != 0 && gb_is_egg(&e)) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_EGG_ITEM_TITLE, UI_WARN, PDNA_GBEDIT_EGG_ITEM_L1, 0);
    return false;
  }
  /* BACKLOG #95 review C4: this tree tracks no mailbox, so a Mail item set here
   * leaves gbs_delete/gbs_move (gb_session.h, gbs_is_mail_item) refusing the
   * WHOLE PARTY the moment they see this mon -- confirm before committing. */
  if (id != 0 && gbs_is_mail_item((uint8_t)id)
      && !app_confirm(PDNA_GBEDIT_MAIL_TITLE, PDNA_GBEDIT_MAIL_L1))
    return false;                                        /* declined */
  if (!gb_set_held_item(&e, (uint8_t)id)) { snd_deny(); return false; }

  return gb_edit_commit(box, slot, &e, "item");
}

/* Split in two (BACKLOG #92) so `item` can be NULL for a Gen-1 mount and
 * gb_item_hook for a Gen-2 one -- gb_session_core picks between them off
 * g_ed->s.gen, same idea as k_gb_ops vs k_gb_ops_ro picking off g_ed itself. */
static const AppSrcOps k_gb_ops_gen1 = {
  .edit = 0, .move = gb_move_hook, .release = gb_release_hook,
  .copy_native = gb_copy_native_hook, .paste = gb_paste_hook, .view = gb_view_hook,
  .editable = gb_editable_hook, .create = gb_create_hook,
};
static const AppSrcOps k_gb_ops_gen2 = {
  .edit = 0, .move = gb_move_hook, .release = gb_release_hook,
  .copy_native = gb_copy_native_hook, .paste = gb_paste_hook, .view = gb_view_hook,
  .editable = gb_editable_hook, .create = gb_create_hook, .item = gb_item_hook,
};

/* Bag/menu review fix (the nav-menu-copy-lossy finding): the read-only nav-menu mount
 * (pdna_gen12_show over a bare FIL, no edit session -- g_ed stays NULL for its whole
 * visit) used to register NO ops table at all, because gb_session_core below only
 * called app_src_ops_set() `if (g_ed)`. That left g_src_ops NULL on this path, so
 * app_copy()'s `g_src_ops && g_src_ops->copy_native` check in pdna_main.c never fired,
 * from_gb never got set, and a later PASTE in a Gen-3 save pasted the lossy converted
 * copy with no sidecar merge -- the exact loss copy_native/gb_copy_native_hook exists
 * to prevent, just unreachable from this entry point.
 *
 * This table is the read-only twin of k_gb_ops above: every mutating hook (edit/move/
 * release/paste) is NULL, because none of them has a GbSession to write through here --
 * app_mon_menu_readonly's row list already NULL-gates each one off g_src_ops, so this
 * is enough to keep every destructive row off the menu on this path. `view` and
 * `copy_native` are the two hooks S5-B/BACKLOG#41 already made g_ed-optional (see their
 * own comments above), so they carry over unchanged; `editable` stays NULL, which
 * folds app_mon_menu_readonly's row label back to `edit && app_can_edit()` --
 * `edit` is NULL on this table, so the row is never mislabelled VIEW/EDIT here either. */
static const AppSrcOps k_gb_ops_ro = {
  .edit = 0, .move = 0, .release = 0, .copy_native = gb_copy_native_hook,
  .paste = 0, .view = gb_view_hook, .editable = 0,
};

/* BACKLOG #48 (Guy's hardware test, 2026-09-06): "the start button doesn't work, I
 * don't have the same menu" / "I can't enter settings". Root cause was NOT the box
 * screen returning a code this loop mishandled -- pdna_gen12_source() sets
 * is_bank=true (its own comment explains why: no PARTY tab, no PC hand-off edges),
 * and pdna_box.c's plain KEY_START dispatch used to be gated `!src->is_bank`
 * outright, so a GB session's START press matched no case at all and did nothing,
 * exactly as reported. Fixed at the source (BoxSource.has_start, pdna_box.h/.c,
 * set only by pdna_gen12_source): pdna_box() now genuinely returns 2 for this
 * session, which is what this helper handles.
 *
 * BACKLOG #58 (Guy, 2026-09-07): "use the same start button menu in all games... say
 * 'coming soon' or 'not supported in Gen 2 games'." This used to open the shared nav
 * menu (app_nav_menu -> pdna_main.c's nav_menu) under a MASK that dimmed every row but
 * Settings/Trainer/Back and answered one generic "GEN 3 ONLY" for all of them. The menu
 * is undimmed now (NAV_ALL_AVAILABLE, identical to a Gen-3 save's own), and every row
 * besides Settings/Trainer/Back goes through app_nav_refuse (pdna_main.c), which
 * consults source/nav_avail.h's rule table for an honest per-row COMING SOON / NOT IN
 * GEN 1 / NOT IN GEN 2 answer instead of one blanket message. */
static void gb_nav_from_start(Gb12Mount* m) {
  int kind = (m->kind == GB12_SAVE_RBY) ? SE_KIND_GEN1 : SE_KIND_GEN2;
  int nv = app_nav_menu(NAV_ALL_AVAILABLE);
  if (nv == NV_SETTINGS) {
    app_nav_settings();
    /* Settings can register/clear a Gen-1/2 ROM or flip the Sprites grid. Neither
     * needs an explicit cache-clear here: the actual invalidation happens inside
     * gb_art_source.c's gb_art_register() (E6 D6 fix -- this comment used to
     * misattribute it to app_gb_rom_path_set(), which is a plain string copy in
     * pdna_main.c and calls nothing; gb_art_register() is what app_register_gb_rom()
     * calls after a successful pick, and what gb_rom_row_action()'s "Clear ROM" row
     * calls directly, and it invalidates unconditionally on every path including
     * failure). The very next pdna_box(&s) call below re-decodes the box and
     * re-notes the cell cache from scratch (box_decode -> pdna_origin_box_note runs
     * on every entry, unconditionally) -- see pdna_origin_box_clear()'s own header
     * comment. sprite_settings's app_icon_rom_open() opens its OWN s_iconrom_fil
     * over a Gen-3 ROM candidate (a different FIL from this mount's `f`, and it
     * never touches g_pc/g_save/this session's arena), so it is harmless to run
     * mid-session. Just re-assert the hint below: nothing else in the tree writes
     * it today, but this is the cheap belt-and-braces against that ever changing
     * under Settings' many sub-screens. */
    pdna_origin_box_set_hint(m->kind == GB12_SAVE_RBY ? PDNA_GEN1 : PDNA_GEN2);
  } else if (nv == NV_TRAINER) {
    /* BACKLOG #49 P1b: the real trainer card, over the editable session when one is
     * open (g_ed != NULL -- the resident-image path, pdna_gen12_show_image, whenever
     * gbs_open succeeded on the same bytes). g_ed->s IS the session the mount itself
     * was read from (gbs_open ran over the same `img`), so pdna_gbtrainer's edits and
     * gb_persist's card write land on exactly what this box grid is showing.
     *
     * No g_ed (the plain FIL-streaming entry, pdna_gen12_show -- the "import a GB
     * save while a Gen-3 save is loaded" path): there is no bytes-resident GbSession
     * here to hand pdna_gbtrainer (a Gen-1/2 save is up to 32816 B and this path's
     * own arena budget, GB12_ARENA_NEED, has no room left to stage one -- see
     * GB12_ARENA_NEED's own comment). Falling back to the existing read-only info
     * page rather than inventing a fragile stage-buffer reuse under this slice's
     * one-hour budget; flagged for a follow-up slice, not silently worked around. */
    if (g_ed) pdna_gbtrainer(&g_ed->s, true);
    else      (void)gb_info_page(m);   /* A and B both just return to the grid */
  } else if (nv == NV_BAG && kind == SE_KIND_GEN1) {
    /* U4 (BACKLOG #67): Red/Yellow's own Item bag + PC store, same "needs a
     * live GbSession to write through" gate as NV_TRAINER above. D7 (U4
     * review): this branch used to dispatch to pdna_gbbag() for EVERY kind,
     * including Gen 2 -- nav_avail's own GB_TABLE says COMING_SOON for
     * SE_KIND_GEN2 (Gen 2's Pack is a later slice), but app_nav_menu() here
     * is called with NAV_ALL_AVAILABLE, so a Gen-2 session's NV_BAG press
     * never even reached nav_avail's rule table; it silently opened the
     * Gen-1-shaped bag screen (which then refused with its own generic
     * "This screen is Gen-1 only" message -- functionally harmless, but not
     * the DESIGNED "coming soon" text nav_avail already has for this exact
     * row). Gating on `kind` here lets a Gen-2 NV_BAG press fall through to
     * the `else if (nv != NV_BACK)` branch below, which calls
     * app_nav_refuse() and shows nav_avail's own honest message instead. */
    if (g_ed) pdna_gbbag(&g_ed->s, true);
    else      (void)gb_info_page(m);
  } else if (nv == NV_BAG && kind == SE_KIND_GEN2) {
    /* U5 (BACKLOG #67): Gold/Silver/Crystal's own Pack + PC store, mirroring
     * U4's own gate one branch up -- same "needs a live GbSession to write
     * through" rule (pdna_gbpack() itself refuses a NULL/non-Gen-2 session,
     * but g_ed's resident image is the only path that HAS one here; the
     * plain FIL-streaming entry falls back to the read-only info page, same
     * reasoning as NV_TRAINER above). */
    if (g_ed) pdna_gbpack(&g_ed->s, true);
    else      (void)gb_info_page(m);
  } else if (nv == NV_MAP && kind == SE_KIND_GEN1) {
    /* M1 (BACKLOG #91): read-only current-map view, same "needs a live
     * GbSession to read the ROM's own tile bank through" gate every other
     * real-art Gen-1/2 screen on this menu uses (Trainer/Bag/Pack above) --
     * g_ed->s is the same resident session those already read/write
     * through gb12_arena_tail(). The plain FIL-streaming mount (no g_ed)
     * falls back to the read-only info page, same as Trainer/Bag/Pack. */
    if (g_ed) pdna_gbmap_gen1(&g_ed->s);
    else      (void)gb_info_page(m);
  } else if (nv != NV_BACK) {
    app_nav_refuse(nv, kind);   /* COMING SOON or NOT IN GEN 1/2, per nav_avail.h */
  }
  /* NV_BACK: nothing to do -- the caller re-enters the grid right after this returns.
   * E6 D4: icon_store.c's backstop (the same call pdna_main.c's nav switch makes on
   * every screen exit -- see icon_store.c ~817-823) retires any Tier B icon plan left
   * over from whatever this menu visited, so a later screen never reads a stale claim
   * as a yes. It cannot release THIS session's own EWRAM arena: that arena was
   * acquired directly (app_arena_acquire in pdna_gen12_show), not through
   * icon_store_borrow(), so icon_store's s_borrow is NULL here and the call is a
   * harmless no-op that only exists to keep the two nav-menu call sites symmetric. */
  icon_store_borrow(false);
}

/* Info page -> box grid -> the "these did not convert" report. The whole session above
 * the mount, shared by both entry points: the only difference between opening a GB save
 * from a loaded Gen-3 save's nav menu and opening one straight off the file browser is
 * WHERE THE BYTES COME FROM, and that difference lives entirely in the read callback. */
static void gb_session_core(Gb12Mount* m) {
  rmbl_fire(RCUE_ROOM);
  if (!gb_info_page(m)) return;              /* B on the info page = never entered */
  /* BACKLOG #77: once per session, past the point the user could still back out --
   * open the Gen-3 ROM wallpaper rung (artless builds only, no-op if already open)
   * so the box grid below can stream the real wallpaper instead of the procedural
   * grass when a Gen-3 ROM is fused/registered. See app_gb_wallpaper_rom_open(). */
  /* Tell app_mon_menu that the ACTIVE box source is read-only, so its destructive
   * actions (PASTE / DUPLICATE / CREATE) are not offered on a source that cannot
   * accept them. This also suppresses the bank's deferred-delete bookkeeping for these
   * boxes — see pdna_main.c. Cleared unconditionally below; every exit from the box
   * screen passes through it. */
  app_src_readonly_set(pdna_gen12_why_locked, "Converted copy");
  /* Bag/menu review fix: the nav-menu path (no edit session, g_ed NULL) used to skip
   * app_src_ops_set() entirely, leaving g_src_ops NULL -- so COPY there never reached
   * copy_native and silently pasted the lossy converted bytes. Register the read-only
   * twin so VIEW and (lossless) COPY still work with no GbSession to write through. */
  /* BACKLOG #92: the live-session table also picks Gen1-vs-Gen2 now, so ITEM
   * (gb_item_hook) is only ever installed for a Gen-2 mount. */
  app_src_ops_set(!g_ed ? &k_gb_ops_ro
                        : (g_ed->s.gen == GB_GEN2 ? &k_gb_ops_gen2 : &k_gb_ops_gen1));
  BoxSource s = pdna_gen12_source(m);
  /* #77 (review, 2026-09-09): AFTER pdna_gen12_source() sets g_m, so app_save_kind()
   * reports GEN1/GEN2 when app_icon_rom_open()'s kind check runs. */
  app_gb_wallpaper_rom_open();
  /* BACKLOG #53a: this session's box grid is a RAW Game Boy save's OWN box -- unlike a
   * Gen-3 save's PC/BANK, every occupied, non-egg cell here genuinely IS m->kind's
   * generation, no signature needed. Tell the cell cache so (era_cell_mark/box_gb/
   * art_wanted all key off pdna_origin_of_hint() through cell_pack()) instead of
   * leaving it to guess from the record's bytes alone, which is what produced Guy's
   * hardware finding: every cell on a Yellow save read uncertain ('?', red box, the
   * ordinary Gen-3 OBJ icon) instead of a certain '1' with the Gen-1 picture. Cleared
   * unconditionally below so a LATER Gen-3 PC/BANK visit this same run is never left
   * thinking it is still inside a GB session. */
  pdna_origin_box_set_hint(m->kind == GB12_SAVE_RBY ? PDNA_GEN1 : PDNA_GEN2);
  /* Returns 0 on B / the SAVE tab (leave); 2 on START, now reachable (BACKLOG #48,
   * BoxSource.has_start) -- handled by gb_nav_from_start above; 5 when the cursor
   * drops off the bottom row (the PC<->Bank hand-off, which has no PC to hand off to
   * here). All non-zero codes re-enter on the top tabs, so DOWN/START never silently
   * exit the session.
   *
   * E6 D3: this used to call app_box_start_set(1) unconditionally, which reopens the
   * grid with the SAVE tab focused (pdna_box.c: st==1 && !s_holding && is_bank ->
   * s_tab_focus = 2 = SAVE) -- so every trip through the START menu left the cursor
   * sitting on the exit tab. The Gen-3 nav returns to the plain GRID after its menu,
   * so START here should match: only r==5 (the PC<->Bank hand-off edge) actually needs
   * the tabs focused, r==2 (START) does not.
   *
   * BACKLOG #56 fix: `s` is a plain struct copied once above -- s.start_box is read
   * by pdna_box() only at its own entry (pdna_box.c:2847), so re-calling pdna_box(&s)
   * with the SAME `s` used to replay whatever box the session originally opened on,
   * every single re-entry. gbsrc_note_box() (wired as s.note_box) keeps m->ui_box
   * current for every box the grid actually showed, so re-deriving `s` before each
   * re-entry picks it back up. Only the BOX is restored, not the cursor CELL within
   * it: pdna_box() always enters at cur=0 (or wherever app_box_start_take()'s 0..3
   * directional hint puts it) for the PC/Bank too -- there is no existing "resume
   * this exact cell" mechanism to mirror, so this does not invent one either. */
  for (int r; (r = pdna_box(&s)) != 0; ) {
    if (r == 2) gb_nav_from_start(m);
    else app_box_start_set(1);
    s = pdna_gen12_source(m);
  }
  pdna_origin_box_set_hint(0);
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

#ifdef PDNA_DELTA
/* BACKLOG #62. See pdna_gen12.h's own comment: same shape as pdna_gen12_show() above,
 * but mounts directly over cartridge space (fused_gb_slice_read + a persistent
 * FusedGbSlice) instead of opening a FIL. Own slice, separate from CREATE's
 * s_gb_create_slice above -- both need to persist for the length of a mount, and while
 * an import session and a CREATE run inside it never overlap in practice, using two
 * distinct statics costs nothing and removes any need to reason about that. */
static FusedGbSlice s_gb_import_slice;

int pdna_gen12_show_fused(int idx, uint8_t met_game) {
  const char* name = 0; const uint8_t* base = 0; uint32_t size = 0;
  if (!fused_gb_save(idx, &name, &base, &size)) return 0;

  uint8_t* arena = app_arena_acquire(GB12_ARENA_NEED);
  if (!arena) {
    ui_clear();
    snd_deny();
    s_msg("NOT NOW", UI_WARN, "Save the Pokemon you moved,", "then open the GB save.");
    return 0;
  }

  uint32_t abase  = GB12_A4((uint32_t)(uintptr_t)arena);
  Gb12Mount* m    = (Gb12Mount*)(uintptr_t)abase;
  uint8_t* recs   = (uint8_t*)(uintptr_t)(abase + GB12_A4(sizeof(Gb12Mount)) + GB12_A4(sizeof(FIL)));
  uint8_t* stage  = recs + GB12_RECS_BYTES;

  s_gb_import_slice.base = base;
  s_gb_import_slice.size = size;

  const char* why = 0;
  if (!pdna_gen12_mount(m, fused_gb_slice_read, &s_gb_import_slice, size, recs, stage,
                        met_game, &why)) {
    log_line("gen12: mount fused %s (%lu bytes) failed: %s", name ? name : "?",
             (unsigned long)size, why ? why : "?");
    ui_clear();
    snd_deny();
    s_msg("NOT A GB SAVE", UI_WARN, why ? why : "Unrecognised file.", 0);
    app_arena_release();
    return 0;
  }
  log_line("gen12: fused %s mounted (%s) %d mons, %d ready, %d locked, %d bad",
           name ? name : "?", pdna_gen12_kind_name(m->kind), m->nstored, m->nready,
           m->nblocked, m->nunreadable);

  gb_session_core(m);

  app_arena_release();
  return 0;
}
#endif /* PDNA_DELTA */

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
  g_tail_lent = false;      /* U2b review 0b: a fresh visit never starts on loan */
  if (pristine && path) {
    memcpy(pristine, img, len);
    GbsStatus st = gbs_open(&ed->s, img, len, ed->scratch, sizeof ed->scratch);
    if (st == GBS_OK && (ed->s.gen == GB_GEN1) == (m->kind == GB12_SAVE_RBY)) {
      ed->img = img; ed->pristine = pristine; ed->len = len; ed->path = path;
      /* S5-C review fix #6b: the arena block is uninitialised memory left over from
       * whatever last borrowed it -- romgs_ready must start false EVERY session, or
       * a stale true (garbage that happens to survive) plus a coincidentally-equal
       * romgs_path would skip the ROM scan entirely and hand back whatever RomGbSprite
       * garbage was sitting there. learn_ready (G1 review MEDIUM-2) is the identical
       * risk for the SAME reason: a stale true plus a coincidentally-equal learn_path
       * would hand a fresh session someone else's located learnset table. */
      ed->romgs_ready = false;
      ed->learn_ready = false;
      ed->learn_rom_id = 0;
      g_ed = ed;
    } else {
      log_line("gen12: edit session refused (%s, gen %d vs mount kind %d): read-only",
               gbs_status_text(st), ed->s.gen, (int)m->kind);
    }
  }

  gb_session_core(m);

  g_ed = 0;                                       /* the arena block is about to go */
  g_tail_lent = false;      /* U2b review 0b: the whole block goes away next line anyway,
                              * but a caller that checks the flag before that must see it
                              * cleared, not stale-true from this visit */
  app_arena_release();
  return GB12_ENTER_OK;
}

#endif /* PDNA_GEN12_HOST */
