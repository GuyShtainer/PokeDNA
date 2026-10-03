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
#include "evolutions.h"    /* pk_evo_floor/pk_evo_min_level -- BACKLOG #104 R1 D3 */
#include "gb12_render.h"  /* the display ladder: GB_SHOW_*, gb12_presentation, gb12_render_rec */
#include "bank_cell.h"    /* bc_unpack -- gb_native_summary_open (BACKLOG #150 S150-2 step 5) */
#include "pdna_layout.h"  /* PDNA_SIDECAR_LOSS_ITEMSECRET -- loss_item_text below, host-testable */

/* F3/F5 (xfer-items fix pass): loss_item_text lives here, ABOVE the PDNA_GEN12_HOST
 * guard, on purpose -- it is pure C (pk_item_name + strcpy/strcat, no tonc, no
 * siprintf/newlib) despite building a string gb_paste_loss_screen (below the guard)
 * draws, so tests/host_gen12_test.c can call it directly instead of re-deriving its
 * logic. F5 also rewrote it off siprintf: every format string here has exactly one
 * %s, so strcpy/strcat needs no formatting machinery, and gb_paste_loss_screen's own
 * stack (tools/stack_budget.py) drops the ~800 B _svfiprintf_r/newlib pulled onto a
 * draw path by putting the first siprintf on that chain. cap>=48 always (see the
 * comment on the switch below for the measured 39-byte worst case); a caller that
 * cannot afford 48 gets an empty string, never a truncated write. */
void loss_item_text(const Gen3ToGbLoss* loss, char* out, int cap) {
  const char* name = (loss->g3_held_item != 0) ? pk_item_name(loss->g3_held_item) : "item";
  const bool sid = loss->secret_id;
  if (cap < 48) { out[0] = 0; return; }
  switch (loss->item_outcome) {
    case G3GB_ITEM_HELD:
      if (sid) { strcpy(out, name); strcat(out, " travels + Secret ID"); }
      else     { strcpy(out, "Item: "); strcat(out, name); strcat(out, " travels"); }
      return;
    case G3GB_ITEM_BAG:
      if (sid) { strcpy(out, name); strcat(out, " -> bag + Secret ID"); }
      else     { strcpy(out, "Item: "); strcat(out, name); strcat(out, " -> bag"); }
      return;
    case G3GB_ITEM_PC:
      if (sid) { strcpy(out, name); strcat(out, " -> item PC + Secret ID"); }
      else     { strcpy(out, "Item: "); strcat(out, name); strcat(out, " -> item PC"); }
      return;
    case G3GB_ITEM_STAYS:
      if (sid) { strcpy(out, name); strcat(out, " stays + Secret ID"); }
      else     { strcpy(out, "Item: "); strcat(out, name); strcat(out, " stays behind"); }
      return;
    case G3GB_ITEM_NONE:
    default:
      /* BACKLOG #376: a Game Boy SOURCE (the Gen-2 -> Gen-1 bridge) reaches here with item_dropped and no
       * Gen-3 outcome; it has no Secret ID, so the generic "Held item and Secret ID" row was wrong. Name the
       * item that stays behind in the ledger original (g3_held_item is its Gen-3 counterpart when one exists). */
      if (!sid && loss->item_dropped) {
        if (loss->g3_held_item != 0) { strcpy(out, "Item: "); strcat(out, name); strcat(out, " stays behind"); }
        else strcpy(out, PDNA_SIDECAR_LOSS_ITEMBEHIND);
        return;
      }
      strcpy(out, PDNA_SIDECAR_LOSS_ITEMSECRET);
      return;
  }
}

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

/* The display ladder (GB_SHOW_*, gb12_presentation, gb12_render_rec) moved to the
 * pure-C source/gb12_render.{c,h} (BACKLOG #150 S150-2) so the native Bank cell's
 * own render path (pdna_box.c) can share it byte-for-byte. See gb12_render.h. */

/* Build the 80-byte record shown for one slot, and its refusal reason. `rec` is part
 * of pdna_gen12_page's whole-buffer memset (pdna_gen12.c:~445), so gb12_render_rec's
 * own memset at entry is redundant-but-harmless here — see decision 2. */
static void gb_build_slot(Gb12Mount* m, int box, int slot, uint8_t* rec, uint8_t* reason) {
  Gb12Mon in;
  *reason = (uint8_t)GB12_ERR_EMPTY;
  if (!gb_slot_mon(m, box, slot, &in)) {
    /* The list says a Pokemon is here but nothing decodable is: a glitch species
     * byte. Nothing can be drawn, so the census/report carries it instead. */
    *reason = (uint8_t)GB12_ERR_SPECIES;
    return;
  }
  gb12_render_rec(&in, &m->tgt, (uint32_t)box * 20u + (uint32_t)slot, rec, reason);
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
         * species byte. `in` is not filled, so do NOT ask gb12_presentation. */
        r = GB12_ERR_SPECIES;
        how = GB_SHOW_NONE;
      } else {
        r = gen12_can_convert(&in);
        dex = in.species_dex;
        how = gb12_presentation(&in, r);
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
  if (box == m->party_box) { put_str(out, 12, &pos, "GB PARTY"); return; }
  if (m->kind != GB12_SAVE_RBY) {
    /* Gen 2 stores real box names; echo the player's own name verbatim (no "GB " prefix)
     * for Gen-3 parity (BACKLOG #61). Gen 1's synthesized names keep the prefix since
     * there is no stored name to echo. */
    char nm[G2_NAME_BYTES];
    if (g2_box_name(m->g2names, box, nm, (int)sizeof nm) && nm[0]) {
      char cut[12];
      copy_utf8(cut, 12, nm);
      put_str(out, 12, &pos, cut);
      return;
    }
  }
  /* BACKLOG #40(b): "GB BOX1", no space before the number -- Gen 1 always falls
   * through to this synthesized name (it has none of its own); Gen 2 only reaches it
   * when the save's own box name is empty. Before this fix Gen 1 read "GB BOX 1"
   * (a space) while a REAL Gen-2 box name of "BOX1" (Crystal's own default, above)
   * read "GB BOX1" — two spellings for what is meant to look like the same thing. */
  put_str(out, 12, &pos, "GB BOX");
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

/* F1b: get_name() above is a DISPLAY formatter -- it prefixes "GB " (and, for Gen 1
 * or an empty Gen-2 name, synthesizes "BOXn") -- so it must never be used to seed the
 * rename editor: an unedited confirm would write the decoration itself into the save
 * ("GB BOX1" instead of "BOX1"). get_raw_name() reads the session's own stored bytes
 * via gbbn_read() (below the guard, host-side no-op here) -- the same shim/_impl split
 * gbsrc_set_name/gbsrc_can_rename use, for the same PDNA_GEN12_HOST reason. */
#ifndef PDNA_GEN12_HOST
static void gbsrc_get_raw_name_impl(int box, char out[12]);
#endif
static void gbsrc_get_raw_name(int box, char out[12]) {
#ifndef PDNA_GEN12_HOST
  gbsrc_get_raw_name_impl(box, out);
#else
  out[0] = 0;
#endif
}

/* F1 (BACKLOG #94): the real rename/can_rename bodies need g_ed, app_can_edit,
 * gbbn_rename/gbbn_supported and gb_persist, all of which live below the
 * PDNA_GEN12_HOST guard (this fill function does not) -- tests/host_gen12_test.c
 * compiles everything ABOVE the guard with -DPDNA_GEN12_HOST and no GBA glue at all.
 * These two thin shims keep BoxSource's fill pure-C-compilable; the real work is in
 * the _impl pair defined below the guard, in the GBA-only half of this file. */
#ifndef PDNA_GEN12_HOST
static void gbsrc_set_name_impl(int box, const char* s);
static bool gbsrc_can_rename_impl(void);
static bool gbsrc_box_names_supported_impl(void);
#endif
static void gbsrc_set_name(int box, const char* s) {
#ifndef PDNA_GEN12_HOST
  gbsrc_set_name_impl(box, s);
#else
  (void)box; (void)s;
#endif
}
static bool gbsrc_can_rename(void) {
#ifndef PDNA_GEN12_HOST
  return gbsrc_can_rename_impl();
#else
  return false;
#endif
}
/* BACKLOG #244 (b199-fixes2): the "has a box-name table at all" question, isolated
 * from can_rename()'s "AND is it writable right now" -- see the BoxSource.h comment
 * on box_names_supported. Host default (no live GbSession) is false, the same safe
 * default gbsrc_can_rename's own host shim uses. */
static bool gbsrc_box_names_supported(void) {
#ifndef PDNA_GEN12_HOST
  return gbsrc_box_names_supported_impl();
#else
  return false;
#endif
}
static int  gbsrc_get_wp(int box) { (void)box; return GB12_WALLPAPER; }
static void gbsrc_set_wp(int box, int wp) { (void)box; (void)wp; }           /* read-only */
static bool gbsrc_can_edit(void) { return false; }
/* BACKLOG #93: forward-declared _impl bodies (defined below gb_export_hook, whose
 * GBA-only helpers gbpk_sanitize/gb_pk_pack/box_oam.h/pdna_progress.h they reuse) --
 * same "thin wrapper here, real body later, host build gets a safe default" pattern
 * gbsrc_get_raw_name_impl/gbsrc_can_rename_impl already use just above, for the same
 * reason: tests/host_gen12_test.c compiles this whole file with -DPDNA_GEN12_HOST and
 * has no tonc/box_oam/pdna_progress to link against. */
#ifndef PDNA_GEN12_HOST
static bool gbsrc_can_boxops_impl(int box);
static bool gbsrc_export_all_impl(int box);
static bool gbsrc_release_all_impl(int box);
static bool gbsrc_can_enter_move_impl(int box);   /* BACKLOG #187/#192, F1 */
static bool gb_can_lift_hook_impl(int box, int slot);
/* BACKLOG #150 S150-4 step 3: BoxXferOps.lift_up/release_up real bodies (defined
 * further below, beside gb_release_hook/gb_lift_up_hook's own header comments) --
 * forward-declared here so k_gb_xfer (this same guarded block) can name them before
 * pdna_gen12_source() (which wires s.xfer) appears in file order. */
static int gb_lift_up_hook(int box, int slot, uint8_t* out80);   /* review D5: bool -> XG_LIFT_* tri-state */
static bool gb_release_up_hook(int box, int slot, const uint8_t cell80[80]);
/* BACKLOG #150 S150-12 decision 4: the read-only mount's copy-flavoured lift --
 * gb_lift_pack()'s `copy` switch, thin hook defined beside gb_lift_up_hook below. */
static int gb_lift_copy_hook(int box, int slot, uint8_t* out80);   /* review D5: bool -> XG_LIFT_* tri-state */
/* BACKLOG #150 S150-7 step 4: BoxXferOps.accept_down's real body (defined further
 * below, beside gb_paste_hook -- D8's own ordering comment), forward-declared for the
 * same reason as lift_up/release_up above. */
static bool gb_accept_down_hook(int dst_box, const uint8_t cell80[80]);
/* BACKLOG #187/#191a, F2: BoxXferOps.move_within's real body (defined further below,
 * beside gb_move_hook -- both share gb_move_core), forward-declared for the same
 * reason as lift_up/release_up/accept_down above. */
static bool gb_move_within_hook(int box, int slot, int dst_box);
/* The real xfer vtable -- replaces the earlier S2 marker-only table (every function
 * pointer NULL) now that UP has a real lift/release, DOWN has a real EXACT-arm
 * landing, and (BACKLOG #187/#191a, F2) a same-scope GB drop has a real move_within
 * instead of pdna_box.c's own deny-beep stub. preview_down stays NULL -- not this
 * lane's job. `.gen` is
 * unread today (kept 0, same as the S2 marker it replaces) -- BACKLOG #150 S150-7's
 * arm selector reads the session's generation through app_gb_session_gen() instead
 * (below), a computed value off the EXISTING g_m pointer, specifically so this table
 * can stay `static const` (in ROM) rather than becoming a 24-byte EWRAM static just to
 * hold one field that changes per session (D-Q8: no new EWRAM statics). Declared
 * `static const` at file scope like every other BoxXferOps/AppSrcOps table in this file
 * (k_gb_ops_gen1/gen2/ro). */
static const BoxXferOps k_gb_xfer = {
  .gen = 0, .lift_up = gb_lift_up_hook, .preview_down = 0, .accept_down = gb_accept_down_hook,
  .release_up = gb_release_up_hook,
  .move_within = gb_move_within_hook,   /* BACKLOG #187/#191a, F2: within-save GB drop */
};

/* BACKLOG #150 S150-12 decision 2: the read-only nav-menu mount's own xfer table --
 * copy-flavoured lift, and STRUCTURALLY no way to delete: .release_up/.accept_down/
 * .move_within are all NULL, not just refused at runtime by a guard that could be
 * edited away later. drop_held (pdna_box.c) already treats a NULL release_up as "this
 * peer cannot be asked to delete" (decision 6); bank_down_exact already NULL-checks
 * accept_down before dereferencing it (source/pdna_box.c:1143) -- both existing
 * checks, not new ones this lane adds. `.gen = 0`, same "unread today" reasoning as
 * k_gb_xfer above. `static const` -> ROM, not a new EWRAM static. */
static const BoxXferOps k_gb_xfer_ro = {
  .gen = 0, .lift_up = gb_lift_copy_hook, .preview_down = 0, .accept_down = 0,
  .release_up = 0, .move_within = 0,
};

/* BACKLOG #150 S150-7 D-Q7 plumbing fix: xg_bank_down_arm()'s `dst_gen` argument, for
 * pdna_box.c's bank_down_dispatch -- it cannot read the session's generation off
 * `k_gb_xfer.gen` (kept 0, see above) or off `src->xfer` (never populated -- see
 * bank_down_exact's own comment), so this is a small computed getter over the
 * EXISTING `g_m` pointer (set by pdna_gen12_source(), already reachable from every GB
 * screen) instead of a new stored field. 0 outside a GB session (no g_m), matching
 * D2's "no session -> NONE" row. */
uint8_t app_gb_session_gen(void) {
  return g_m ? ((g_m->kind == GB12_SAVE_RBY) ? GB_GEN1 : GB_GEN2) : 0;
}
#endif
static bool gbsrc_can_boxops(int box) {
#ifndef PDNA_GEN12_HOST
  return gbsrc_can_boxops_impl(box);
#else
  (void)box; return false;
#endif
}
/* BoxSource.can_enter_move (BACKLOG #187/#192, F1) thin wrapper -- real body
 * (gbsrc_can_enter_move_impl) defined below, beside gbsrc_can_boxops_impl, same
 * PDNA_GEN12_HOST reason as every other _impl pair on this source (app_can_edit()
 * is GBA-only, not linked into tests/host_gen12_test.c's -DPDNA_GEN12_HOST build). */
static bool gbsrc_can_enter_move(int box) {
#ifndef PDNA_GEN12_HOST
  return gbsrc_can_enter_move_impl(box);
#else
  (void)box; return false;
#endif
}
static bool gbsrc_export_all(int box) {
#ifndef PDNA_GEN12_HOST
  return gbsrc_export_all_impl(box);
#else
  (void)box; return false;
#endif
}
static bool gbsrc_release_all(int box) {
#ifndef PDNA_GEN12_HOST
  return gbsrc_release_all_impl(box);
#else
  (void)box; return false;
#endif
}
/* BACKLOG #150 S150-5: BoxSource.can_lift -- same "thin wrapper here, real body
 * later" pattern as the three _impl pairs just above, for the same reason (the real
 * body needs gbs_can_delete/gbs_load_list, which cost nothing extra to guard the
 * same way here). Host build gets a safe "never liftable" default. */
static bool gb_can_lift_hook(int box, int slot) {
#ifndef PDNA_GEN12_HOST
  return gb_can_lift_hook_impl(box, slot);
#else
  (void)box; (void)slot; return false;
#endif
}
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
  s.scope      = BOXSCOPE_GB;    /* BACKLOG #171b: can_lift/xfer are wired below --
                                  * start_carry() (pdna_box.c) reads src->xfer, not the
                                  * file-static s_xfer_peer a Bank visit installs */
  s.bank_edge  = true;            /* BACKLOG #120 S2: the single-carry UP-past-the-tabs edge
                                   * now opens the Bank instead of staying dead */
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
  s.can_rename = gbsrc_can_rename;   /* BACKLOG #94: rename gated separately from can_edit
                                      * (gbsrc_can_edit() is hardwired false -- see its own
                                      * comment; the box banner's own commit()-return path
                                      * never actually inspects the boolean either way, see
                                      * the _impl pair's header note below the guard) */
  s.get_raw_name = gbsrc_get_raw_name;  /* F1b: seed the rename editor with the raw stored
                                         * name, not get_name()'s "GB "-decorated display
                                         * string (BACKLOG #94 DO-NOT-SHIP fix) */
  s.get_wp     = gbsrc_get_wp;
  s.set_wp     = gbsrc_set_wp;
  s.can_edit   = gbsrc_can_edit;
  s.can_lift   = gb_can_lift_hook;   /* BACKLOG #150 S150-5: the GB grid's own grab-time refusal */
#ifndef PDNA_GEN12_HOST
  /* BACKLOG #150 S150-12 decision 2: g_ed selects MOVE (k_gb_xfer) vs. the read-only
   * mount's COPY-only table (k_gb_xfer_ro, no release_up/accept_down/move_within) --
   * same selector gb_session_ops_install already uses for k_gb_ops_ro below.
   * pdna_gen12_resident() (pdna_gen12.h), not a bare `g_ed` read: this function runs
   * before g_ed's own declaration appears in this file (g_ed lives inside the
   * PDNA_GEN12_HOST-guarded block further down), and the accessor is the existing,
   * already-declared way every OTHER file asks this question. */
  s.xfer       = pdna_gen12_resident() ? &k_gb_xfer : &k_gb_xfer_ro;   /* BACKLOG #171b: start_carry reads src->xfer, not s_xfer_peer */
#endif
  s.commit     = gbsrc_commit;
  s.mark_dirty = gbsrc_mark_dirty;
  s.note_add   = 0;                           /* nothing lands here; nothing to register */
  s.note_box   = gbsrc_note_box;              /* BACKLOG #56: remember the box for re-entry */
  s.capacity   = gbsrc_capacity;
  /* BACKLOG #93: the box-menu open gate (can_boxops) is narrower than can_lift/
   * can_edit -- see pdna_box.h's own comment on the field -- plus EXPORT ALL/RELEASE
   * ALL bodies so the read-only .pk3 pair in pdna_box.c never runs on a Game Boy box
   * (which has no 80-byte Gen-3 records to hand box_decode_to). */
  s.can_boxops  = gbsrc_can_boxops;
  s.export_all  = gbsrc_export_all;
  s.release_all = gbsrc_release_all;
  s.can_enter_move = gbsrc_can_enter_move;   /* BACKLOG #187/#192, F1: box-level SELECT gate */
  s.box_names_supported = gbsrc_box_names_supported;   /* BACKLOG #244: "has a table", not "is writable" */
  return s;
}

/* ============================================================== GBA glue ==== */
#ifndef PDNA_GEN12_HOST

#include <tonc.h>
#include <stdio.h>

#include "ff.h"
#include "savefile.h"      /* SF_PATH_MAX */
#include "log.h"
#include "xfer_io.h"       /* BACKLOG #150 S150-6: xr_path_for_key -- the one reader */
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
#include "pdna_summary.h"     /* pdna_summary_quiet_save (#234 s4 D2) */
#include "pdna_gbtrainer.h"   /* BACKLOG #49 P1b: the Gen-1/2 trainer card */
#include "pdna_gbbag.h"       /* U4, BACKLOG #67: Red/Yellow's own Item bag */
#include "pdna_gbpack.h"      /* U5, BACKLOG #67: Gold/Silver/Crystal's own Pack */
#include "pdna_gbfly.h"       /* BACKLOG #90: Fly destinations, both generations */
#include "pdna_gbclock.h"     /* BACKLOG #86/#108: Gen-2's own Clock fix screen */
#include "pdna_gbflags.h"     /* BACKLOG #88: the Flags & counters screen */
#include "gb_boxnames.h"      /* BACKLOG #94: gbbn_rename/gbbn_supported -- the box banner's rename */
#include "pdna_gbdaycare.h"   /* BACKLOG #85: the Gen-1/2 Day-Care screen */
#include "gb_daycare.h"       /* BACKLOG #93: gbd_read/gbd_deposit for gb_daycare_hook's push */
#include "gb_pk.h"            /* BACKLOG #93: gb_pk_pack/gb_pk_ext for gb_export_hook's .pk1/.pk2 */
#include "pdna_pk.h"          /* BACKLOG #93: PDNA_BANK_DIR, shared with the Gen-3 .pk3 exporter */
#include "box_oam.h"          /* BACKLOG #93: boxoam_suspend/resume around gbsrc_export_all/release_all's UI */
#include "pdna_progress.h"    /* BACKLOG #93: pdna_progress_frame -- gbsrc_export_all's own progress screen */
#include "pdna_gbdex.h"       /* BACKLOG #87: the Gen-1/2 Pokedex screen */
#include "pdna_gbmap.h"       /* M1, BACKLOG #91: Gen 1's read-only current-map view */
#include "pdna_gbhof.h"       /* BACKLOG #89: the Hall of Fame, both generations */
#include "pdna_gbmap2.h"      /* M1-G2, BACKLOG #91: Gen 2's read-only current-map view */
#include "gb_jkey.h"          /* #234 s4: gb_journal_key / gb_step_name */
#include "jrn_app.h"          /* #234 s4: JaHist (the History screen's rows) */
#include "pdna_bank.h"        /* BACKLOG #120 S2: the Bank, reachable from a GB session now */
#include "xfer_gate.h"        /* BACKLOG #120 S2: xg_clear_carry_on_gb_exit */
#include "pdna_pick.h"        /* BACKLOG #92: pick_item / pick_item_set_gen1_2_held */
#include "pdna_layout.h"   /* PDNA_GBEDIT_* / PDNA_SIDECAR_* -- fixed strings         */
#include "gb_sidecar.h"    /* S5-B: the sidecar format + gbsc_path/gbsc_key            */
#include "gen3_to_gb.h"    /* S5-B: the Gen-3 -> Game Boy down converter               */
#include "gb_moves_legal.h" /* BACKLOG #150 S150-10: per-slot move predicate/fill/pack */
#include "gba_rtc.h"       /* S5-B: the sidecar entry's transfer-time RTC stamp        */
#include "gb_item_names.h" /* BACKLOG #150 S150-8: gb2_item_name for gb_down_loss_screen */
#include "xfer_rec.h"      /* BACKLOG #150 S150-8: xr_key_g3/xr_game_item_mask/xr_time_capsule_block */
#include "bank_restore.h"  /* BACKLOG #150 S150-9 decision 10: bank_restore_from_entry_gb            */
#include "item_map_g2g3.h" /* BACKLOG #150 S150-8: item_g2_to_g3 (bdc_convert_*_core's own use) */
#include "item_map_g1g2.h" /* BACKLOG #249: item_g2_to_g1 (g3gb_item_ladder's own use)  */
#include "gb_bag.h"        /* BACKLOG #249: gbb_read/gbb_pocket_cap/gbb_insert_and_write */
#include "rom_gbsprite.h"  /* S5-C: locates BaseStats in the user's own Gen-1 ROM      */
#include "rom_gblearn.h"   /* BACKLOG #50: level-up learnsets + min-level for CREATE   */
#include "gb_origin.h"     /* BACKLOG #266: static-encounter level floor for CREATE   */
#include "gb_new_mon.h"    /* BACKLOG #50: gb_new_mon/gb_new_mon_g1_moves for CREATE   */
#include "gb1_base_tbl.h"  /* BACKLOG #276: generated Gen-1 base table (ROM-free scratch) */
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

/* BACKLOG #64: a read-only STREAMED GbSession riding the SAME arena block as the
 * plain FIL-streaming mount above -- no resident image (a full Gen-1/2 image plus a
 * session does not fit this arena at all; see docs/briefs/64-gb-import-path-brief.md's
 * "THE BUDGET DOES NOT HOLD" section). `scratch` is g2w's own streaming scratch /
 * gen1_commit's snapshot buffer -- GBS_SCRATCH_BYTES (1152), the same size as this
 * mount's own GB12_STAGE_BYTES staging buffer, but a SEPARATE block: the mount's
 * `stage` buffer is still live and read through while this session's own g2w streams
 * (g2_detect_ranged during gbs_open_streamed's Gen-2 probe re-reads the file fresh,
 * it does not reuse the mount's already-parsed `stage`), so the two buffers cannot be
 * folded into one without risking one pipeline overwriting the other mid-read. */
typedef struct { GbSession s; uint8_t scratch[GBS_SCRATCH_BYTES]; } Gb12View;
#define GB12_ARENA_NEED_RO (GB12_A4(sizeof(Gb12Mount)) + GB12_A4(sizeof(FIL)) + \
                            GB12_RECS_BYTES + GB12_STAGE_BYTES + \
                            GB12_A4(sizeof(Gb12View)) + 4u)
_Static_assert(GB12_ARENA_NEED_RO <= APP_ARENA_BYTES,
               "GB import (read-only session) no longer fits the borrowed EWRAM arena");

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
      if (rows > 11) rows = 10;                  /* overflow: 10 rows + the "More than fit" line = the 11-line budget, clear of the rule at y=147 (#342) */
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
        ui_ptext_fit(4, 30 + 10 * 10 + 2, 232, UI_DIM, "More than fit; the rest are in the boxes.");
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
  /* BACKLOG #172: when gb_origin_for_save()'s .og persist fails (mkdir or the
   * verified write itself), the ANSWERED byte still has to hold for the rest of
   * THIS mount -- re-reading the file would just fail again and re-prompt on
   * every lift, and a second prompt could pick a DIFFERENT answer and mis-route
   * S150-7's exact DOWN mid-mount. Explicitly cleared to false wherever a new
   * g_ed is latched, same discipline as romgs_ready/learn_ready above (never
   * left as whatever garbage the borrowed arena held). */
  bool        origin_write_failed;
  uint8_t     origin_cached;
} Gb12Edit;
static Gb12Edit* g_ed;        /* pointer only: the block itself lives in the arena */

/* Review fix F2(a): true while a resident, writable Game Boy edit session is open;
 * false for streamed/view-only sessions. */
bool pdna_gen12_resident(void) { return g_ed != 0; }


/* BACKLOG #64 review Finding 6 (CRITICAL fix): gb12_arena_tail()'s ONLY tail before
 * this was the resident-image mount's own (g_ed's end); a streamed read-only session
 * (g_ed == NULL) had no tail of its own at all, so map/flags/bag/dex/pack's own
 * gb12_arena_tail() callers all refused with a false "Not enough memory right now"
 * panel on EVERY read-only nav row that needs one -- a regression this lane
 * introduced (those rows showed the working gb_info_page on main). g_ro_tail is the
 * streamed-session equivalent of `g_ed`'s own implicit tail pointer: set by
 * pdna_gen12_show()/pdna_gen12_show_fused() right after their own gbs_open_streamed()
 * succeeds, cleared beside every place this file already clears g_ed to 0. Pointer
 * only (8 B .bss -- both start zero, so they land beside g_ed in .bss, not .data;
 * confirmed via arm-none-eabi-size: __iheap_start moved 8 B, __eheap_start did
 * not, matching g_ed's own "pointer only" comment) -- the bytes themselves live
 * in the SAME arena block gb12_arena_tail() has always pointed into. */
static uint8_t* g_ro_tail;
static uint32_t g_ro_tail_slack;

/* BACKLOG #150 S150-12 decision 5: the read-only mount's own path/name, so
 * gb_origin_for_save()'s per-save-remembered .og file can be keyed the same way for
 * both entry points (g_ed->path when resident, g_ro_path here when only a streamed
 * view exists) -- same "clear before release" bracket as g_ro_tail above, set right
 * beside it in pdna_gen12_show()/pdna_gen12_show_fused(). 4 B plain .bss. */
static const char* g_ro_path;

/* BACKLOG #150 S150-12 decisions 8/12: the RAM-only "how many copies are waiting for
 * the PC" counter and the Bank box the last one landed in -- 2 B plain .bss, cleared
 * by gb_ro_exit_offer() on every exit path and again at the top of
 * pdna_gen12_show()/_fused() (a stale count from an entry that failed before its own
 * gb_session_core() call must never leak into the NEXT mount). Session-RAM only: a
 * power-off between a copy and the mount's exit loses the OFFER, never the cells
 * (BC_FLAG_QUEUED_PC/BC_FLAG_COPY are already on the card) -- OPEN QUESTION 7. */
static uint8_t g_pcq_count;
static uint8_t g_pcq_box;

/* BACKLOG #150 S150-12 decision 12: see pdna_app.h's own comment on the declaration.
 * Saturating (never wraps past 255 -- the offer's worst-case wording is measured for
 * exactly that ceiling, decision 16). Defined here (beside g_pcq_count/g_pcq_box),
 * not next to app_gb_session_gen() as first planned: app_gb_session_gen() sits
 * before these two statics' own declaration in this file, and C does not let a
 * function read a file-static that has not been declared yet by that point. */
void app_pc_queue_note(int bank_box) {
  if (g_pcq_count < 255) g_pcq_count++;
  g_pcq_box = (uint8_t)bank_box;
}

/* F1 (BACKLOG #94): gbsrc_set_name/gbsrc_can_rename's real bodies (declared as thin
 * shims above the PDNA_GEN12_HOST guard, since g_ed, app_can_edit, gbbn_rename,
 * gbbn_supported and gb_persist all live down here in the GBA-glue half of this file).
 *
 * gbsrc_can_rename_impl mirrors gb_editable_hook's own "cart AND box" shape (below)
 * but asks gbbn_supported() instead of the generic gbs_box_writable() -- Gen 1 has
 * no box-name table at all (gbbn_supported refuses it outright), so a Gen-1 banner's
 * A press must snd_deny(), never call through a NULL-shaped path. No g_ed (the plain
 * FIL-streaming entry) also refuses: there is no live GbSession to write through,
 * same reasoning pdna_gbtrainer's own g_ed gate documents. */
static bool gbsrc_can_rename_impl(void) {
  return g_ed && app_can_edit() && gbbn_supported(&g_ed->s);
}

/* BACKLOG #244 (b199-fixes2): deliberately DROPS the app_can_edit() conjunct
 * gbsrc_can_rename_impl() above has -- this answers "does this session's game have
 * a box-name table at all", not "is it writable right now". No g_ed (the plain
 * FIL-streaming entry, no live GbSession) also answers false: there is nothing to
 * ask gbbn_supported() about, the same reasoning gbsrc_can_rename_impl's own
 * no-g_ed refusal documents -- not a writability judgement. */
static bool gbsrc_box_names_supported_impl(void) {
  return g_ed && gbbn_supported(&g_ed->s);
}

/* F1b: the RAW seed for the rename editor -- gbbn_read() decodes the session's own
 * stored bytes with gb_name_decode (the same sequence-safe decoder gbbn_rename()
 * itself compares against for its "unchanged is untouched" guard), never through
 * pdna_gen12_box_name()'s "GB "-prefixed display formatter. out[0] left 0 (empty
 * seed, same as a blank Gen-2 box name) when there is no live session or the read
 * refuses (a Gen-1 save, an out-of-range box, or the party pseudo-box -- gbbn_read
 * itself already zeroes `out` on every false return). */
static void gbsrc_get_raw_name_impl(int box, char out[12]) {
  out[0] = 0;
  if (g_ed) (void)gbbn_read(&g_ed->s, box, out, 12);
}

/* Writes through gbbn_rename() (the verified field write + checksum fix-up gb_boxnames.c
 * itself does), then persists via gb_persist("boxname") -- the same commit path every
 * other Gen-1/2 write in this file uses. Two notes tie this to pdna_box.c's own call
 * sites (box_options_menu's rename case and the direct-A-on-banner path, both
 * `src->set_name(box, buf); src->commit();`):
 *
 *   (a) src->commit() (gbsrc_commit(), just above the guard) is HARDWIRED to return
 *       false -- neither call site inspects that return value for the rename path, so
 *       a rename that already persisted here via gb_persist() never gets a false
 *       "commit failed" surfaced to the player. Traced both sites; nothing to fix.
 *   (b) a Gen-1 save: gbbn_supported() is false, so gbsrc_can_rename_impl() above
 *       already refused before src->set_name() is ever called -- pdna_box.c's A-on-
 *       banner path takes the `else snd_deny()` branch instead. This function's own
 *       `if (!g_ed || !s) return;` guard is pure defense-in-depth (no crash if it were
 *       ever reached anyway), not the actual gate.
 *   (c) an Everdrive session: app_can_edit() is false (hard rule 4, write is
 *       Omega-only), so the SAME can_rename_impl() check above refuses first.
 *
 * gbsrc_get_name() (above the guard) reads m->g2names, a raw-byte snapshot
 * g2_header_ranged() cached ONCE at mount time -- without a refresh here the box
 * banner would keep showing the pre-rename name until the next full remount even
 * though the save itself is already correct. g_ed->s and g_m read through the SAME
 * bytes (the resident-image path opens gbs_open over the mount's own `img` -- see
 * gb_nav_from_start's own NV_TRAINER comment), so re-running the exact read
 * g2_header_ranged() did is a cheap, correct refresh, not a second decode of stale
 * data. */
static void gbsrc_set_name_impl(int box, const char* s) {
  if (!g_ed || !s) return;
  /* The party pseudo-box (BACKLOG #56's extra "GB PARTY" row, g_m->party_box) is not
   * a real box -- it has no slot in the save's box-name table, so gbbn_rename()
   * below would refuse it as an out-of-range box index (GBS_ERR_ARG) with only a
   * silent snd_error() to show for it. can_rename() itself has no box parameter (it
   * gates the whole session, not a specific box -- see its own header note), so this
   * is the one place that CAN tell the party row apart from a real box; name it. */
  if (g_m && box == g_m->party_box) {
    msg_wait(PDNA_GBEDIT_REFUSED_TITLE, UI_WARN,
             "The party row isn't a box --", "there's nothing to rename.");
    return;
  }
  /* Defense-in-depth, not a reachable path from pdna_box.c's own two call sites
   * today: both seed osk_input() with cap=9 (GB_BOXNAME_BYTES), which already caps
   * what the player can type at 8 glyphs. Named here so a future caller that skips
   * that cap (or a longer paste-style input path) gets an honest reason instead of
   * gbbn_rename()'s generic snd_error() for the SAME over-length refusal. */
  if ((int)strlen(s) > GB_BOXNAME_GLYPHS) {
    msg_wait(PDNA_GBEDIT_REFUSED_TITLE, UI_WARN,
             "Box names are up to 8", "characters.");
    return;
  }
  if (gbbn_rename(&g_ed->s, box, s) != GBS_OK) { snd_error(); return; }
  if (g_m) (void)g_m->rd(g_m->ctx, g_m->g2o.box_names, g_m->g2names, sizeof g_m->g2names);
  gb_hold_commit("boxname");
}

static void s_busy(const char* line) {
  ui_clear();
  ui_panel(PDNA_BUSY_PANEL_X, PDNA_BUSY_PANEL_Y, PDNA_BUSY_PANEL_W, PDNA_BUSY_PANEL_H, UI_PANEL, UI_WARN);
  ui_text(PDNA_BUSY_TEXT_X, 70, UI_WARN, PDNA_GBEDIT_BUSY_SAVING);
  ui_text(PDNA_BUSY_TEXT_X, 88, UI_TEXT, line);
}

/* G1 review MEDIUM-2: CREATE's own busy screen, NOT s_busy() -- see
 * PDNA_GBCREATE_BUSY_TITLE's own comment (pdna_layout.h) for why "Saving - do
 * not power off" does not apply to a pure ROM read. Same panel shape. */
static void s_busy_reading(void) {
  ui_clear();
  ui_panel(PDNA_BUSY_PANEL_X, PDNA_BUSY_PANEL_Y, PDNA_BUSY_PANEL_W, PDNA_BUSY_PANEL_H, UI_PANEL, UI_WARN);
  ui_text(PDNA_BUSY_TEXT_X, 70, UI_WARN, PDNA_GBCREATE_BUSY_TITLE);
  ui_text(PDNA_BUSY_TEXT_X, 88, UI_TEXT, PDNA_GBCREATE_BUSY_LINE);
}

/* The card refused; put RAM back to what the card holds so the grid never shows an
 * edit that did not land, and re-latch the session over the restored bytes. Also the
 * documented recovery for gbs_move()'s own atomicity contract (gb_session.h): a failure
 * on the SOURCE half of a move can leave the destination half already committed in RAM,
 * and this is the only way back to a state the card actually holds. */
void gb_rollback(void) {
  if (!g_ed) return;   /* BACKLOG #64: streamed sessions have no resident image to roll back */
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
    msg_wait(PDNA_GBEDIT_READONLY_TITLE, UI_WARN, app_gb_readonly_why(), 0);
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

/* The reason `box`/`slot` cannot be lifted, or NULL when it can -- the shared core
 * both gb_can_lift_hook_impl (BoxSource.can_lift, called by pdna_box.c with the
 * REAL box/slot it already tracks) and gb_lift_why_hook (AppSrcOps.lift_why, below
 * -- called by app_mon_menu_readonly, which only ever has a record ADDRESS) end up
 * running. One rule table, not two: whoever needs the bool (gb_can_lift_hook_impl)
 * just checks this for NULL. */
/* BACKLOG #166 review F1: the panel this draws in (app_mon_menu_readonly's RO_MOVE
 * gate) is 88 px wide (PDNA_MONMENU_PROSE_W), not msg_wait's 184 -- gbs_status_text()'s
 * own wording is sized for the LATTER (gb_move_hook's own late-refusal dialog, kept
 * unchanged) and overflows here ("the party needs one Pokemon" alone is 176 px, over
 * twice this budget) -- ui_ptext_fit would truncate it into an illegible fragment.
 * Buckets the GbsStatus into one of five short, fixed strings (pdna_layout.h)
 * instead of quoting gbs_status_text() verbatim; PARTY_FLOOR and MAIL keep their own
 * bucket (the two refusals worth a specific word), everything else structural
 * (UNWRITABLE/BOX/STRUCT) collapses to "the box itself", and anything left over
 * (ENGINE/VERIFY/FULL/NEEDS_BASE/SLOT/ARG/NOT_GB -- none of them reachable from
 * gbs_can_delete's own documented return set, but a bucket, not a silent NULL, if
 * that set ever grows) collapses to a generic "refused". */
static const char* gb_lift_why_status(GbsStatus st) {
  switch (st) {
    case GBS_ERR_PARTY_FLOOR: return PDNA_GB_LIFT_WHY_FLOOR;
    case GBS_ERR_MAIL:        return PDNA_GB_LIFT_WHY_MAIL;
    case GBS_ERR_UNWRITABLE:
    case GBS_ERR_BOX:
    case GBS_ERR_STRUCT:      return PDNA_GB_LIFT_WHY_BOX;
    default:                  return PDNA_GB_LIFT_WHY_OTHER;
  }
}

static const char* gb_lift_why_bs(int box, int slot) {
  /* BACKLOG #150 S150-12 decision 4: the read-only nav-menu mount (no open edit
   * session) is no longer a flat refusal -- a COPY lift is allowed there, gated
   * the same way gb_lift_copy_hook's own guard is (g_m and g_ro_path both set,
   * decision 5/OPEN QUESTION 8: g_ro_path is set unconditionally after a successful
   * pdna_gen12_mount, not only when the streamed view session also opened, since
   * the copy itself only ever needs g_m->stage). !app_can_edit() on that mount
   * (Q-C, §11.8: an EverDrive, or a hack ROM) is its own distinct reason. */
  if (!g_ed) {
    if (!g_m || !g_ro_path) return PDNA_GB_LIFT_WHY_VIEW;
    return app_can_edit() ? NULL
         : (app_rom_hack_active() ? PDNA_ROMHACK_NOTE : PDNA_GB_LIFT_WHY_OMEGA);
  }
  if (!app_can_edit())                                /* cart/ROM-hack read-only */
    return app_rom_hack_active() ? PDNA_ROMHACK_NOTE : PDNA_GB_LIFT_WHY_OMEGA;
  GbsStatus wst = gbs_box_writable(&g_ed->s, box);
  if (wst != GBS_OK) return gb_lift_why_status(wst);  /* e.g. a virgin Gen-1 bank */
  GbsStatus dst = gbs_can_delete(&g_ed->s, box, slot, g_ed->list, 0);
  if (dst != GBS_OK) return gb_lift_why_status(dst);  /* party floor / Mail / ... */
  return 0;
}

/* BoxSource.can_lift real body (BACKLOG #150 S150-5): the GB grid's own grab-time
 * refusal. Same two gates gb_editable_hook takes (the cart and the box), PLUS
 * gbs_can_delete()'s whole refusal table (gb_session.c) -- a Gen-1 one-mon party or a
 * Mail-holding Gen-2 party is not liftable AT ALL, rather than letting the grab
 * succeed and the drop fail late (S150-5's whole acceptance). No open edit session
 * (g_ed NULL: the read-only nav-menu mount) means no GbSession to check against --
 * refuse. GBA-only (needs gbs_can_delete's own frame); gb_can_lift_hook() above is
 * the thin wrapper the host build actually links. BACKLOG #166: delegates to
 * gb_lift_why_bs (immediately above) rather than re-deriving the same verdict a
 * second way, so this bool and AppSrcOps.lift_why's reason text can never diverge. */
static bool gb_can_lift_hook_impl(int box, int slot) {
  return gb_lift_why_bs(box, slot) == NULL;
}

/* BACKLOG #372a: the reason the grid's MOVE-mode grab of (box, slot) was refused, or NULL. Same table
 * BoxSource.can_lift and AppSrcOps.lift_why use (gb_lift_why_bs), so the toast and the refusal can never
 * disagree. Read-only. GBA-only (the host build never links pdna_box.c). */
const char* gb_lift_why_note(int box, int slot) { return gb_lift_why_bs(box, slot); }

/* AppSrcOps.lift_why real body (BACKLOG #166): same question as gb_can_lift_hook_impl
 * above, but for a caller that only has the record's own ADDRESS -- app_mon_menu's
 * `box` parameter is zeroed for any is_bank source (pdna_box.c's `mbox = src->is_bank
 * ? 0 : box`, and a GB session always sets is_bank true), so app_mon_menu_readonly
 * cannot pass a real box index through; it derives one from `rec80` here via
 * gb_locate_addr(), exactly like gb_move_hook/gb_release_hook/every other AppSrcOps
 * hook in this file already does. A record this mount cannot locate returns NULL
 * (no reason to show) rather than guessing -- the same silent-defensive shape
 * gb_move_hook's own `if (!gb_locate(...)) return false;` already uses; a genuinely
 * unlocatable record cannot reach a real MOVE press either, so there is nothing to
 * warn about NOW that would not also fail silently there, unchanged from today. */
static const char* gb_lift_why_hook(const uint8_t* rec80) {
  int box, slot;
  if (!gb_locate_addr(rec80, &box, &slot)) return 0;
  /* Same cart gate gb_lift_why_bs applies (called next) -- restated in THIS
   * function's own body so host_gb_write_gate_test.py's per-hook scan (every
   * mutating AppSrcOps hook must reach app_can_edit( in its own text) can see it
   * here too, not just one call away. Redundant, not wrong: gb_lift_why_bs checks
   * the identical predicate immediately below. */
  if (!app_can_edit())
    return app_rom_hack_active() ? PDNA_ROMHACK_NOTE : PDNA_GB_LIFT_WHY_OMEGA;
  return gb_lift_why_bs(box, slot);
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
/* BACKLOG #199: the box/slot-driven half of gb_copy_native_hook below, factored out
 * so gb_lift_pack (the Bank-UP lift, now called by ORIGIN COORDINATES from
 * drop_held_up rather than by a rec80 address resolved through the currently-paged
 * display buffer) can load the SAME way without a rec80 to resolve one from. Neither
 * caller needs gb_locate_addr's own bounds check here -- both already have a
 * trustworthy (box, slot): this function's own gbs_load_list()/gb_list_count() (or
 * the RO mount's gb_list_read()/gb_list_count()) calls fail closed on an out-of-range
 * box/slot exactly as they always have. */
static bool gb_copy_native_by_coord(int box, int slot, GbEditMon* out, bool* has_sidecar) {
  if (has_sidecar) *has_sidecar = false;
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

static bool gb_copy_native_hook(const uint8_t* rec80, GbEditMon* out, bool* has_sidecar) {
  if (has_sidecar) *has_sidecar = false;
  int box, slot;
  if (!gb_locate_addr(rec80, &box, &slot)) return false;
  return gb_copy_native_by_coord(box, slot, out, has_sidecar);
}

/* S5-B Part E: does `mon` already have a sidecar entry on the card? Gen-1 targets
 * shipped (S5-C Part B1, pdna_gen12.c gen3_to_gb() Gen-1 branch) and gb_paste_write
 * writes a ledger entry for both gens (pdna_gen12.c gbsc_entry_from call), so a Gen-1
 * mon can carry a sidecar just like a Gen-2 one -- the old `if (gen != GB_GEN2) return
 * false;` short-circuit was stale (BACKLOG #150 S150-4 mismatch 6) and made every
 * caller wrong for Gen 1: gb_copy_native_hook's "lossless" toast, the info-page sidecar
 * count, gb_dup_confirm's once-per-visit warning and the summary view's has_sidecar
 * flag all now answer correctly for Gen-1 mons too (an intended shipped-behaviour
 * change, not a bug fix to a call site). noinline for the same reason gb_paste_write
 * is: FILINFO (lib/fatfs/ffconf.h: FF_USE_LFN=1, so fname[FF_LFN_BUF+1]=256 bytes
 * alone) is well over the ~200 B a hook's own frame should carry -- one f_stat is
 * cheap, but only if its 280-ish-byte argument lives in a frame that is not also live
 * for the whole editor/picker/confirm run gb_edit_hook drives. */
static bool __attribute__((noinline)) gb_has_sidecar(uint8_t gen, const GbEditMon* mon) {
  uint8_t dv4[4] = {
    gb_get_dv(mon, GB_ATK), gb_get_dv(mon, GB_DEF),
    gb_get_dv(mon, GB_SPE), gb_get_dv(mon, GB_SPC)
  };
  uint64_t key = gbsc_key(mon->gen, gb_get_otid(mon), dv4, mon->otname);
  char path[GBSC_PATH_MAX];
  /* BACKLOG #150 S150-6, site 1: xr_path_for_key() already does the f_stat (it has
   * to, to decide xfer vs sidecar) and returns true iff a file exists at the path
   * it wrote -- the separate f_stat this function used to do afterward is now
   * always redundant, so it is gone. */
  return xr_path_for_key(path, key);
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
  return gb_hold_commit(what_for_log);                                           /* 5 (#234 s4: held until exit while the journal records) */
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

/* BACKLOG #187/#193, F3/F4 follow-on: `exclude_party` -- gbs_insert() (DUPLICATE's
 * and CREATE's full-box retry, both below) refuses the party pseudo-box outright
 * ("PARTY IS REFUSED HERE, ON PURPOSE", gb_session.c's own comment on gbs_insert) --
 * offering it as a pickable row would only earn a generic "bad argument" a screen
 * later. gb_move_hook's own call passes false: gbs_move() DOES support a party
 * destination (species-limit/live-stat/Mail rules), so hiding it there would be a
 * real feature loss, not a UX fix. */
static int gb_pick_box(const Gb12Mount* m, int exclude, const char* title, bool exclude_party) {
  int n = m->party_box + 1;
  if (n <= 1) return -1;
  if (n > GB12_PICKBOX_MAX) n = GB12_PICKBOX_MAX;   /* defensive; never true today */

  /* Every box gbs_box_writable refuses (a Gen-1 virgin/uninitialised bank) is exactly as
   * unreachable as `exclude` -- moving a Pokemon there would only bounce back off
   * gbs_move's own gbs_commit_list gate, so it is dimmed and skipped here instead of
   * offered and then refused a screen later. Computed ONCE, not per repaint: a box's
   * writability cannot change while this picker is up (nothing else touches the image).
   *
   * BACKLOG #187/#193, F3: also dim+skip a box that is genuinely FULL -- picking one
   * used to be offered and only THEN refused a screen later by gbs_move/gbs_insert's
   * own GBS_ERR_FULL; this picker now shows that up front instead (both the count and
   * the skip), so "refuse only when NO box has room" (DUPLICATE's new full-box path,
   * gb_dup_hook below) can just call this same picker and trust it never offers a
   * full one. cnt[]/cap[] feed the row labels below. g_ed->list2 as scratch: same
   * "safe here, nothing past this point touches list2 for real until the caller's
   * OWN gbs_move/gbs_insert call after this function returns" reasoning
   * gb_first_free_box's own comment documents for the identical pattern. */
  bool skip[GB12_PICKBOX_MAX];
  int  cnt[GB12_PICKBOX_MAX], cap[GB12_PICKBOX_MAX];
  int selectable = 0;
  for (int b = 0; b < n; b++) {
    cap[b] = gb_list_capacity(g_ed->s.gen, b);
    cnt[b] = -1;
    bool writable = gbs_box_writable(&g_ed->s, b) == GBS_OK;
    if (writable && gbs_load_list(&g_ed->s, b, g_ed->list2) == GBS_OK)
      cnt[b] = gb_list_count(g_ed->s.gen, g_ed->list2, b);
    bool full = cap[b] > 0 && cnt[b] >= 0 && cnt[b] >= cap[b];
    bool party_excluded = exclude_party && gb_box_is_party(g_ed->s.gen, b);
    skip[b] = (b == exclude) || !writable || full || party_excluded;
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
    ui_text(4, 3, UI_TITLE, title);
    ui_hline(0, 13, UI_SCR_W, UI_BORDER);
    int shown = n - top;
    if (shown > PDNA_GBEDIT_PICKBOX_ROWS) shown = PDNA_GBEDIT_PICKBOX_ROWS;
    for (int i = 0; i < shown; i++) {
      int b = top + i;
      char nm[12], row[24];
      pdna_gen12_box_name(m, b, nm);
      /* BACKLOG #187/#193, F3: "NAME  n/cap" -- cnt[b] < 0 means the count could not
       * be read (an otherwise-writable box whose list came back malformed); shown as
       * "?/cap" rather than a wrong number. */
      if (cnt[b] >= 0) siprintf(row, "%s  %d/%d", nm, cnt[b], cap[b] > 0 ? cap[b] : 0);
      else             siprintf(row, "%s  ?/%d", nm, cap[b] > 0 ? cap[b] : 0);
      int y = PDNA_GBEDIT_PICKBOX_Y0 + i * PDNA_GBEDIT_PICKBOX_ROW_H;
      bool sh = (b == sel);
      if (sh) ui_panel(2, y - 1, UI_SCR_W - 4, PDNA_GBEDIT_PICKBOX_ROW_H, UI_SEL, UI_TITLE);
      ui_text(4, y, skip[b] ? UI_DIM : (sh ? UI_SELTEXT : UI_TEXT), row);
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

/* BACKLOG #150 S150-4 decision 4/D-Q1: pre-select the origin prompt's default row
 * from a REGISTERED Game Boy ROM of this generation's own header title -- never an
 * auto-skip (D-Q1: "a save can come from a different cart than the ROM beside it").
 * Reuses g_ed's own romspath/romfil/romgs/romscan scratch (gb_create_locate_rom's
 * own fields, same registered-ROM-open shape at :2994-3010) -- no new statics.
 * Returns one of BC_ORIGIN_RED/BLUE/YELLOW/GOLD/SILVER, or BC_ORIGIN_UNKNOWN when no
 * registered ROM of this generation opens or its title matches none of the known
 * names (D-Q2: Yellow has no detector of its own -- it only ever wins this match by
 * its own distinct title substring, never inferred). */
static uint8_t gb_pick_origin_default(uint8_t gen) {
  const char* reg = app_gb_rom_path(gen);
  if (!reg || !reg[0]) return BC_ORIGIN_UNKNOWN;
  int i = 0;
  for (; reg[i] && i < (int)sizeof(g_ed->romspath) - 1; i++) g_ed->romspath[i] = reg[i];
  g_ed->romspath[i] = 0;
  memset(&g_ed->romfil, 0, sizeof g_ed->romfil);
  if (f_open(&g_ed->romfil, g_ed->romspath, FA_READ) != FR_OK) return BC_ORIGIN_UNKNOWN;
  FSIZE_t fsz = f_size(&g_ed->romfil);
  uint32_t sz = (fsz > (FSIZE_t)0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)fsz;
  /* BACKLOG #201 F4: `gen` (GB_GEN1/GB_GEN2, this function's own caller already
   * picked one) is the SAME numbering as GB_ROM_GEN1/GB_ROM_GEN2 -- passing it as
   * the hint runs only that generation's three scan jobs on a cold (uncached)
   * registered ROM, instead of all six. */
  int ok = rom_gbsprite_open(&g_ed->romgs, gb_read, &g_ed->romfil, sz,
                             g_ed->romscan, sizeof g_ed->romscan,
                             (gen == GB_GEN1) ? GB_ROM_GEN1 : GB_ROM_GEN2);
  f_close(&g_ed->romfil);
  if (!ok || g_ed->romgs.gen != gen) return BC_ORIGIN_UNKNOWN;
  const char* t = g_ed->romgs.title;
  if (gen == GB_GEN1) {
    if (strstr(t, "YELLOW")) return BC_ORIGIN_YELLOW;
    if (strstr(t, "BLUE"))   return BC_ORIGIN_BLUE;
    if (strstr(t, "RED"))    return BC_ORIGIN_RED;
  } else {
    if (strstr(t, "SILVER")) return BC_ORIGIN_SILVER;
    if (strstr(t, "GOLD"))   return BC_ORIGIN_GOLD;
  }
  return BC_ORIGIN_UNKNOWN;
}

/* BACKLOG #150 S150-4 decision 4: the one-time origin prompt. Modelled verbatim on
 * gb_pick_box() above (same ui_clear/ui_text/ui_panel/ui_hline shape, same
 * s_wait(KEY_UP|KEY_DOWN|KEY_A|KEY_B) loop, same PDNA_GBEDIT_PICKBOX_* row metrics --
 * Gen-1/2 UX parity: reuse the session's own picker idiom, do not invent a screen).
 * Exactly the three games of the detected generation (Gen 2 non-Crystal is two
 * rows) -- EXCEPT `yellow_possible == false` (BACKLOG #191b: gen1_detect_yellow()
 * came back 0, i.e. proved not-Yellow), which drops to the two Red/Blue rows only,
 * same shape as Gen 2's two rows. gen1_detect_yellow() never actually returns 0
 * today (see gen1_save.h), so this parameter has no live caller yet -- it exists so
 * the call site is correct the day a 0-producing rule is found, rather than a second
 * change needed then. Returns the picked BC_ORIGIN_* value, or -1 on B (cancel --
 * the LIFT fails, nothing held, nothing written). */
static int __attribute__((noinline)) gb_pick_origin(uint8_t gen, bool crystal, bool yellow_possible) {
  if (crystal) return BC_ORIGIN_CRYSTAL;   /* D-Q7: Crystal is not ambiguous -- no prompt at all */

  const char* names[3];
  uint8_t     vals[3];
  int n;
  if (gen == GB_GEN1) {
    n = 2;
    names[0] = PDNA_XFER_GAME_RED;    vals[0] = BC_ORIGIN_RED;
    names[1] = PDNA_XFER_GAME_BLUE;   vals[1] = BC_ORIGIN_BLUE;
    if (yellow_possible) { names[n] = PDNA_XFER_GAME_YELLOW; vals[n] = BC_ORIGIN_YELLOW; n++; }
  } else {
    n = 2;
    names[0] = PDNA_XFER_GAME_GOLD;   vals[0] = BC_ORIGIN_GOLD;
    names[1] = PDNA_XFER_GAME_SILVER; vals[1] = BC_ORIGIN_SILVER;
  }

  uint8_t def = gb_pick_origin_default(gen);
  int sel = 0;
  for (int i = 0; i < n; i++) if (vals[i] == def) { sel = i; break; }

  for (;;) {
    ui_clear();
    ui_text(4, 3, UI_TITLE, PDNA_XFER_ORIGIN_TITLE);
    ui_hline(0, 13, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < n; i++) {
      int y = PDNA_GBEDIT_PICKBOX_Y0 + i * PDNA_GBEDIT_PICKBOX_ROW_H;
      bool sh = (i == sel);
      if (sh) ui_panel(2, y - 1, UI_SCR_W - 4, PDNA_GBEDIT_PICKBOX_ROW_H, UI_SEL, UI_TITLE);
      ui_text(4, y, sh ? UI_SELTEXT : UI_TEXT, names[i]);
    }
    ui_hline(0, 147, UI_SCR_W, UI_BORDER);
    ui_text(4, 150, UI_DIM, PDNA_XFER_ORIGIN_FOOT);

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return -1;
    if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    if (k & KEY_DOWN) sel = (sel + 1) % n;
    if (k & KEY_A)    return (int)vals[sel];
  }
}

/* BACKLOG #150 S150-4 decision 4 / BACKLOG #172: the FNV-1a-64 step both the path
 * key and the save fingerprint below need (the SAME constants gbsc_key() uses,
 * gb_sidecar.c) -- factored out so #172's fingerprint is not a second hash
 * implementation living next to this one. */
static uint64_t gb_fnv64(const uint8_t* buf, int len) {
  uint64_t h = 14695981039346656037ULL;
  for (int i = 0; i < len; i++) { h ^= (uint64_t)buf[i]; h *= 1099511628211ULL; }
  return h;
}

/* FNV-1a-64 over the save's own on-card path -- "this save" is identified by its
 * path, so it selects WHICH .og file to look at; BACKLOG #172's fingerprint below
 * is what proves the save actually sitting at that path is still the one that
 * answered it. */
static uint64_t gb_origin_key(const char* path) {
  int n = 0;
  while (path[n]) n++;
  return gb_fnv64((const uint8_t*)path, n);
}

/* BACKLOG #172: a 4-byte fingerprint of THIS save's identity (player name + public
 * trainer id), independent of its on-card path. g_m (the live mount, set by
 * pdna_gen12_mount before any edit session or lift can run) already carries both
 * as G2Header-shaped fields -- Gb12Mount.tid/.player, "public trainer id, both
 * generations" per pdna_gen12.h -- populated once at mount time for the box-header
 * UI, so this reads an existing cache rather than adding a second save parser.
 * Truncated to the low 32 bits of the same FNV-1a-64 gb_origin_key() uses: a path
 * key collision AND a fingerprint collision both landing on the same wrong file is
 * astronomically unlikely, and a false MISMATCH only costs one re-prompt, never a
 * mis-route. g_m == NULL (should not happen: no edit session opens without a
 * mount) folds to a fixed sentinel input rather than dereferencing a null
 * pointer -- the resulting fingerprint simply mismatches every stored file, so
 * the failure mode is "always re-ask", not a crash. */
static uint32_t gb_origin_fingerprint(void) {
  uint8_t buf[2 + G2_NAME_BYTES];
  int n = 0;
  uint16_t tid = g_m ? g_m->tid : 0;
  buf[n++] = (uint8_t)(tid & 0xFFu);
  buf[n++] = (uint8_t)((tid >> 8) & 0xFFu);
  if (g_m) {
    for (int i = 0; i < G2_NAME_BYTES && g_m->player[i]; i++) buf[n++] = (uint8_t)g_m->player[i];
  }
  uint64_t h = gb_fnv64(buf, n);
  return (uint32_t)(h & 0xFFFFFFFFu);
}

/* BACKLOG #150 S150-4 decision 4 / BACKLOG #172: read -> prompt -> write -> stamp,
 * remembered ONCE PER SAVE under /PokeDNA/xfer/<16hex>.og (xr_path_for_name -- the
 * resolver lives in the ledger folder with everything else, no new path rule).
 * noinline: the FatFs path[GBSC_PATH_MAX] local is the same "keep it out of the
 * caller's frame" reason gb_paste_write's own helper is noinline.
 *
 * File format (BACKLOG #172): 5 bytes -- byte 0 the BC_ORIGIN_* answer, bytes 1-4
 * the LE gb_origin_fingerprint() of the save that answered it. The OLD 1-byte
 * format (no fingerprint) and any other stray length both fail the `len >= 5`
 * gate below and fall into the same re-prompt-and-rewrite path as a genuine
 * fingerprint mismatch -- "old file" and "different save at this path" are the
 * same bug from this function's point of view (#172), and both get the same fix:
 * ask again, then persist the current save's own fingerprint.
 *
 * Returns the BC_ORIGIN_* byte, or -1 if the user cancelled the prompt (the
 * caller must fail the lift). A PERSIST failure (mkdir or the verified write) does
 * NOT fail the lift and does NOT re-prompt again this mount: the answered byte is
 * cached in g_ed (origin_write_failed/origin_cached) and returned directly by
 * every later call in the same mount, logged once here. A READ failure of an
 * existing file re-prompts+rewrites, same as a fingerprint mismatch. */
static int __attribute__((noinline)) gb_origin_for_save(uint8_t gen, bool crystal) {
  if (crystal) return BC_ORIGIN_CRYSTAL;

  /* BACKLOG #191b: Gen-1 origin bypass, the same shape as the Crystal bypass above --
   * BEFORE any sidecar/.og read, ask the save's own bytes whether it is Yellow
   * (gen1_detect_yellow_window(), gen1_save.h). g_m->rd/g_m->ctx is the mount's
   * streamed reader (Gb12Mount has no whole-image buffer to hand gen1_detect_yellow()
   * itself -- see gen1_save.h's own note on the two entry points), so this reads only
   * the 128-byte window the detector needs, not the full 32 KiB save. A read failure
   * (should not happen mid-mount) folds to -1 (ambiguous), same as the detector's own
   * "cannot cover the window" case -- never a crash, never a false "not Yellow". */
  bool yellow_possible = true;
  if (gen == GB_GEN1) {
    uint8_t win[GEN1_YELLOW_WIN_LEN];
    bool got = g_m && g_m->rd && g_m->rd(g_m->ctx, GEN1_YELLOW_WIN_OFF, win, GEN1_YELLOW_WIN_LEN);
    int yd = got ? gen1_detect_yellow_window(win, GEN1_YELLOW_WIN_LEN) : -1;
    if (yd == 1) return BC_ORIGIN_YELLOW;   /* proven Yellow: no prompt, no sidecar read/write */
    yellow_possible = (yd != 0);            /* dead today (see gb_pick_origin's own note) */
  }

  /* BACKLOG #172: an earlier lift this mount already answered and could not
   * persist it -- honour that in-RAM answer rather than re-prompting (which
   * could pick a DIFFERENT origin and mis-route S150-7's exact DOWN mid-mount). */
  if (g_ed && g_ed->origin_write_failed) return g_ed->origin_cached;

  uint32_t fp = gb_origin_fingerprint();

  /* REVIEW FIX (HIGH, found post-merge): decision 5 was claimed in 217c148's
   * commit message but never actually written here -- this line read
   * `g_ed->path` unconditionally, a NULL deref on every RO-mount lift that
   * reaches this point (every Red/Blue/Gold/Silver save; only Crystal and a
   * proven-Yellow save return earlier, above). On hardware that reads the
   * pointer field at Gb12Edit's own path offset off address 0 (BIOS-protected,
   * open bus) and strlen-walks garbage -- mGBA happened to survive it, which is
   * why the shot chain's frame 06 caption ("no NULL deref") was false. Refuse
   * up front when neither a resident session nor a mounted RO path exists
   * (should not happen -- both hooks' own guards keep this function
   * unreachable otherwise -- but self-sufficient, same posture gb_lift_up_hook/
   * gb_lift_copy_hook already take on their own guards), then read the path
   * from whichever of the two is actually live. */
  if (!g_ed && !g_ro_path) {
    log_line("gen12: origin: no session and no mount path");
    return -1;
  }

  char name[24];
  char hex[17];
  gbsc_key_hex(gb_origin_key(g_ed ? g_ed->path : g_ro_path), hex);
  siprintf(name, "%s.og", hex);
  char path[GBSC_PATH_MAX];
  bool existed = xr_path_for_name(path, name);

  if (existed) {
    uint8_t buf[5]; uint32_t len = 0;
    if (sf_read_full(path, buf, sizeof buf, &len) == SF_OK && len >= 5) {
      bool ok = (gen == GB_GEN1)
              ? (buf[0] == BC_ORIGIN_RED || buf[0] == BC_ORIGIN_BLUE || buf[0] == BC_ORIGIN_YELLOW)
              : (buf[0] == BC_ORIGIN_GOLD || buf[0] == BC_ORIGIN_SILVER || buf[0] == BC_ORIGIN_CRYSTAL);
      uint32_t stored_fp = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8) |
                            ((uint32_t)buf[3] << 16) | ((uint32_t)buf[4] << 24);
      if (ok && stored_fp == fp) return buf[0];   /* same save, answered before -- no re-prompt */
    }
    /* absent, short (old 1-byte format or otherwise truncated), corrupt, a value
     * that doesn't belong to this gen, or a fingerprint mismatch (a DIFFERENT
     * save landed at this path, BACKLOG #172) -> re-prompt below and rewrite */
  }

  int picked = gb_pick_origin(gen, false, yellow_possible);
  if (picked < 0) return -1;                          /* B cancels the lift */

  FRESULT mkr = f_mkdir(PDNA_XFER_DIR);
  if (mkr != FR_OK && mkr != FR_EXIST) {
    log_line("gen12: origin mkdir %s failed (%d) -- kept the answer for this mount", PDNA_XFER_DIR, (int)mkr);
    if (g_ed) { g_ed->origin_write_failed = true; g_ed->origin_cached = (uint8_t)picked; }
    /* REVIEW FIX (decision 5's second half, missing from 217c148): the RO mount
     * has no g_ed to cache the answer in, and BACKLOG #172's own "two cells from
     * one save can never disagree" rule means this function must not silently
     * re-derive (and possibly re-prompt to a DIFFERENT answer) on the NEXT grab
     * this same mount -- refuse the lift instead. Without this, an RO lift whose
     * card cannot take a 5-byte .og re-prompts on EVERY grab. */
    else { log_line("gen12: origin persist failed on the read-only mount -- lift refused"); return -1; }
  } else {
    uint8_t b[5];
    b[0] = (uint8_t)picked;
    b[1] = (uint8_t)(fp & 0xFFu);
    b[2] = (uint8_t)((fp >> 8) & 0xFFu);
    b[3] = (uint8_t)((fp >> 16) & 0xFFu);
    b[4] = (uint8_t)((fp >> 24) & 0xFFu);
    if (sf_write_verified(path, b, sizeof b) != SF_OK) {
      log_line("gen12: origin write %s failed -- kept the answer for this mount", path);
      if (g_ed) { g_ed->origin_write_failed = true; g_ed->origin_cached = (uint8_t)picked; }
      else { log_line("gen12: origin persist failed on the read-only mount -- lift refused"); return -1; }
    }
  }
  return picked;
}

/* app_src_ops_set() hook: MOVE TO on the read-only mon menu (S3). Picks a destination,
 * then gbs_move() -- see its header for the full refusal list and the atomicity
 * contract this function has to honour: a non-OK return can mean the DESTINATION half
 * already committed (the source delete is what failed), so every non-OK here rolls the
 * whole image back, not just on the ones that look like they need it. */
/* BACKLOG #187/#191a, F2: the shared move-and-persist core gb_move_hook (the menu's
 * MOVE TO BOX row, picker-driven) and gb_move_within_hook (drag-and-drop, the
 * destination box already chosen by the drop cell -- see pdna_box.c's drop_held)
 * both need: gbs_move() into `dst`, the SAME refusal wording/hint table, and the
 * SAME gb_persist("move") reload/repaint on success (which invalidates g_m->loaded
 * so the display mount re-pages from the just-committed image -- pdna_gen12_page's
 * own `if (m->loaded == box) return recs;` early-out is why a fresh page is needed
 * at all). One body, two thin callers, so a defect fixed here is fixed for both
 * entry points -- not a second, drifting copy of the refusal table. */
static bool gb_move_core(int box, int slot, int dst) {
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
  return gb_hold_commit("move");
}

static bool gb_move_hook(uint8_t* rec80) {
  int box, slot;
  if (!gb_locate(rec80, &box, &slot)) return false;

  int dst = gb_pick_box(g_m, box, PDNA_GBEDIT_PICKBOX_TITLE, false);
  if (dst < 0) return false;                       /* B on the picker: nothing touched */

  return gb_move_core(box, slot, dst);
}

/* BoxXferOps.move_within (BACKLOG #187/#191a, F2): a same-generation GB drop across
 * boxes -- pdna_box.h's own contract comment on the field. `box`/`slot` are the
 * ORIGIN the drag started from (s_orig_box/s_orig_slot in pdna_box.c, already
 * resolved by the caller); `dst_box` is the box the cursor dropped on -- NOT a
 * destination slot, because gbs_move() (same primitive gb_move_hook's own picker
 * uses) always lands at the box's own next free slot, exactly like a count-prefixed
 * list has to. The caller (drop_held) has ALREADY refused an occupied destination
 * cell and a same-box drop before calling this -- this only ever runs for a real
 * cross-box, destination-empty drop. */
static bool gb_move_within_hook(int box, int slot, int dst_box) {
  if (!g_ed) return false;
  return gb_move_core(box, slot, dst_box);
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
  return gb_hold_commit("release");
}

/* BACKLOG #280 (Guy's #270 ruling, the G3_HOME half): the Gen-3-TARGET restore. The GB->Bank
 * lift is a pass-through now (gb_lift_pack), so a Game Boy mon whose HOME is Gen-3 (ledger
 * entry XR_KIND_G3_HOME, written when a Gen-3 mon was sent DOWN into a Game Boy save --
 * gb_bank_down_g3 -> gb_paste_write) reaches the Bank as its native "GBC1" cell. When THAT
 * cell is dropped on a Gen-3 target (a PC box or the party, both through
 * bank_down_convert_gen3*), this is the edge that hands the EXACT original Gen-3 bytes back.
 * It is the relocated core of the old lift-time gb_lift_restore_g3home: the SAME
 * gbsc_merge_up_sel walls (probe, then the TAKE/KEEP merge screen) -- but it runs in a Gen-3
 * session, where g_ed is NULL, so it owns a GBSC_FILE_MAX frame (gbpc_restore_up's shape,
 * pdna_box.c) instead of borrowing g_ed->sidecar. noinline: never inlined into a
 * wrapper on drop_held's stack chain.
 *
 * Lifecycle (R3): the entry is CONSUMED, not marked RESTORED (gb_release_g3home's old
 * semantics, design 2.5), and only after the restored bytes are durable: this function
 * writes nothing to the card. It records the entry in the session's ONE pending-transfer
 * slot (app_xfer_pending_set, the very slot the GEN3 arm uses); app_xfer_promote consumes
 * the entry after the verified PC save, app_xfer_pending_undo leaves it untouched on a
 * declined save. The same slot is what stops a second restore of the same entry before
 * the flush: with a transfer pending, the next Gen-3-target drop meets the SAVE NOW? wall
 * (below), and once saved the entry is gone.
 *
 * Returns 1 = `out80` holds the plain Gen-3 record to land (pending set); 0 = convert
 * normally (no entry / a COPY cell / not a Gen-3 PC session / an entry that is not plain);
 * -2 = refused or declined, ALREADY said on screen (any failure shows its own dialog). */
int __attribute__((noinline))
gb_g3home_restore_up(const uint8_t cell80[80], uint8_t out80[80]) {
  if (!cell80 || !out80) return -2;
  if (!bc_is_native(cell80) || xg_cell_is_copy(cell80)) return 0;   /* a COPY's GB original still lives: restoring = clone */
  if (!app_can_edit() || !app_gen3_pc_live()) return 0;

  GbEditMon mon; BcMeta meta;
  if (!bc_unpack(cell80, &mon, &meta)) return 0;
  uint8_t dv4[4] = {
    gb_get_dv(&mon, GB_ATK), gb_get_dv(&mon, GB_DEF),
    gb_get_dv(&mon, GB_SPE), gb_get_dv(&mon, GB_SPC)
  };
  uint64_t key = gbsc_key(mon.gen, gb_get_otid(&mon), dv4, mon.otname);

  uint8_t buf[GBSC_FILE_MAX];
  uint32_t len = 0;
  GbscEntry e;
  int found = -1;
  for (int attempt = 0; attempt < 2; attempt++) {   /* bounded: one SAVE NOW? re-read at most */
    SfStatus rst = xr_open(key, buf, sizeof buf, &len, NULL);
    if (rst == SF_ERR_OPEN) return 0;               /* no ledger entry -- an ordinary native cell */
    if (rst != SF_OK || gbsc_count(buf, len) < 0) {
      log_line("gen12: g3home restore: ledger open/validate failed (%s)", sf_status_str(rst));
      boxoam_suspend(); snd_error();
      msg_wait(PDNA_XFERREC_TITLE, UI_WARN, PDNA_XFERREC_L1, PDNA_XFERREC_L2);
      boxoam_resume();
      return -2;
    }
    found = xr_resolve_home(buf, len, &mon, XR_KIND_G3_HOME, gb_get_species_dex(&mon));
    if (found < 0) return 0;                        /* no G3_HOME entry for this mon */
    if (!gbsc_get(buf, len, found, &e) || e.state != XR_STATE_NONE) return 0;   /* only a plain entry restores */
    if (!app_xfer_pending() || attempt == 1) break;
    /* An unpromoted transfer is pending: the same SAVE NOW? wall gb_bank_down_gen3's
     * 16(g) shows -- and the only thing keeping one entry from restoring twice pre-flush. */
    log_line("gen12: g3home restore: a transfer is pending -- offering SAVE NOW?");
    boxoam_suspend();
    char l1[64];
    siprintf(l1, "%s %s", PDNA_XFER_SAVENOW_L1, PDNA_XFER_SAVENOW_L2);
    bool yes = app_confirm(PDNA_XFER_SAVENOW_TITLE, l1);
    bool ok  = yes && app_xfer_save_now();
    boxoam_resume();
    if (!ok) { snd_deny(); return -2; }
  }
  if (app_xfer_pending() || app_bank_defer_full()) { snd_deny(); return -2; }   /* still pending / no deferred-delete room */

  GbscMergeReport rep;
  uint8_t probe80[80];
  if (!gbsc_merge_up_sel(&e, &mon, 0, probe80, &rep)) {
    log_line("gen12: g3home restore: gbsc_merge_up_sel probe failed");
    boxoam_suspend(); snd_error();
    msg_wait(PDNA_XFERREC_TITLE, UI_WARN, PDNA_XFERREC_L1, PDNA_XFERREC_L2);
    boxoam_resume();
    return -2;
  }
  XrMergeReport xrep;
  xr_report_from_gbsc(&rep, &xrep);
  uint8_t accept = 0;
  boxoam_suspend();
  bool confirmed = app_xfer_merge_screen(&xrep, XR_MERGE_UP, &accept);
  boxoam_resume();
  if (!confirmed) return -2;                        /* B: nothing spent, nothing written */

  if (!gbsc_merge_up_sel(&e, &mon, accept, out80, NULL) || bc_is_native(out80)) {
    log_line("gen12: g3home restore: commit failed or produced a native cell");
    boxoam_suspend(); snd_error();
    msg_wait(PDNA_XFERREC_TITLE, UI_WARN, PDNA_XFERREC_L1, PDNA_XFERREC_L2);
    boxoam_resume();
    return -2;
  }
  app_xfer_pending_set(key, (int16_t)found);        /* consumed by app_xfer_promote after the verified save */
  app_xfer_pending_mark_g3home();                   /* #284: a failed save keeps this key (undo never removes G3_HOME) */
  log_line("gen12: g3home restore: original Gen-3 record ready, entry %d pending consume", found);
  return 1;
}

/* BACKLOG #150 S150-12 decision 4: gb_lift_up_hook's (BACKLOG #150 S150-4 decision
 * 3/step 3) shared body, factored so a COPY lift (the read-only mount) and a MOVE
 * lift (the resident edit session) run the exact same pack sequence instead of two
 * drifting copies (golden rule: one function per job). `copy` selects only the two
 * places that must differ: the flags byte (COPY marks BC_FLAG_QUEUED_PC|BC_FLAG_COPY
 * on top of the ordinary egg/item/party bits; MOVE marks none of those three) and,
 * inside the shared origin-prompt call, which source answers "is this Crystal" (a
 * MOVE lift asks the open GbSession, exactly as it always has; a COPY lift -- no
 * GbSession exists on the read-only mount -- asks the mount's own Gb12Mount.kind,
 * decision 4's own text). The two per-hook GUARDS (self-sufficient refusal, before
 * either can run: MOVE needs g_ed set, COPY needs g_ed absent AND a mounted
 * read-only path) live in the two thin callers below, not here.
 *
 * Order: copy the native record -> (#280: no ledger step -- the lift is a pass-through)
 * -> the one-time-per-save origin prompt
 * (decision 5) -> pdna_bank_next_serial() (0 -> fail the lift, the serial is
 * persisted before the cell that consumes it) -> flags derived from the SAME getters
 * bank_plant.c's own test fixture uses (gb_is_egg / gb_get_held_item), never a new
 * rule -> bc_pack(), which owns the party->box truncation itself (do not truncate
 * here). epoch reuses gb_paste_write's own RTC source (gba_rtc_get), not a new clock
 * call -- 0 when the RTC is absent, exactly as gb_paste_write already tolerates.
 *
 * BACKLOG #199 (lane b199): `box`/`slot` replace the old `rec80` parameter -- the
 * call moved from grab time (start_carry) to the one drop that actually needs the
 * Bank's price (drop_held_up), so there is no live rec80 address into the display
 * buffer to resolve any more, only the carry's own origin coordinates. Loads through
 * gb_copy_native_by_coord() (the box/slot half of gb_copy_native_hook, factored out
 * for this), which is the SAME load gb_release_up_hook's own re-verify already does
 * by coordinates -- unlike the old rec80 form, this is robust to an L/R re-page of
 * the display mount between the grab and the drop. */
static int gb_lift_pack(int box, int slot, uint8_t* out80, bool copy) {
  if (!out80) { log_line("gen12: %s lift refused: no destination buffer", copy ? "copy" : "xferup"); return XG_LIFT_FAILED; }

  GbEditMon mon;
  if (!gb_copy_native_by_coord(box, slot, &mon, NULL)) {
    log_line("gen12: %s lift refused: gb_copy_native_by_coord could not read box %d slot %d", copy ? "copy" : "xferup", box, slot);
    return XG_LIFT_FAILED;
  }

  /* #280 (Guy's #270 ruling): the lift is a PASS-THROUGH -- no ledger read, no restore, no wall,
   * no merge screen, COPY or MOVE alike. A mon whose ledger entry says it has a home elsewhere
   * banks as the native record it is; the restore happens only at a different-generation TARGET
   * drop (gb_g3home_restore_up for a Gen-3 target, gb_bridge_restore_up for the other Game Boy
   * generation). */

  /* decision 4/D-Q7: the one-time-per-save origin prompt. g_ed set (MOVE) asks the
   * open GbSession, exactly as before; g_ed NULL (COPY, decision 4) asks the mount's
   * own Gb12Mount.kind -- `mon.gen` (just loaded, above) is the right `gen` argument
   * either way, since it is the SAME session/mount's own generation. review D5: -1 = B
   * cancelled on the full-screen picker itself (gb_pick_origin's own KEY_B branch) --
   * the player was already looking straight at it, so its own disappearance IS the
   * "B = no" the house style elsewhere draws explicitly; CANCELLED, not a failure,
   * before any serial is spent. */
  bool crystal = g_ed ? gb_session_is_crystal(&g_ed->s) : (g_m && g_m->kind == GB12_SAVE_CRYSTAL);
  int origin = gb_origin_for_save(mon.gen, crystal);
  if (origin < 0) {
    log_line("gen12: %s lift cancelled: origin prompt cancelled", copy ? "copy" : "xferup");
    return XG_LIFT_CANCELLED;
  }
  uint8_t origin_game = (uint8_t)origin;

  uint32_t serial = pdna_bank_next_serial();
  if (!serial) {                              /* meta write failed -> refuse the lift */
    log_line("gen12: %s lift refused: bank.meta write failed (pdna_bank_next_serial)", copy ? "copy" : "xferup");
    return XG_LIFT_CARD;
  }

  uint8_t flags = 0;
  if (mon.is_party)          flags |= BC_FLAG_FROM_PARTY;
  if (gb_is_egg(&mon))       flags |= BC_FLAG_EGG;
  if (gb_get_held_item(&mon) != 0) flags |= BC_FLAG_HOLDS_ITEM;
  if (copy)                  flags |= (BC_FLAG_QUEUED_PC | BC_FLAG_COPY);   /* S150-12 decision 3 */

  GbaRtcTime t;
  uint32_t epoch = 0;
  if (gba_rtc_get(&t))
    epoch = ((uint32_t)(t.year - 2000u) << 26) | ((uint32_t)t.month << 22) |
            ((uint32_t)t.day << 17) | ((uint32_t)t.hour << 12) |
            ((uint32_t)t.minute << 6) | (uint32_t)t.second;

  if (bc_pack(&mon, flags, origin_game, epoch, serial, out80) != 0) {
    log_line("gen12: %s lift refused: bc_pack failed", copy ? "copy" : "xferup");
    return XG_LIFT_FAILED;
  }
  return XG_LIFT_OK;
}

/* BoxXferOps.lift_up (MOVE, the resident write session -- BACKLOG #150 S150-4
 * decision 3/step 3). BACKLOG #199: called from drop_held_up (pdna_box.c) at the
 * Bank-UP drop, by the carry's own ORIGIN coordinates -- no rec80 any more (see this
 * field's own contract comment, pdna_box.h). Self-sufficient guard, unchanged from
 * before the S150-12 refactor (BACKLOG #171b review F3): refuse up front rather than
 * trust the caller's own can_lift/can_enter_move gates to forever stay in lock-step
 * with this one. */
static int gb_lift_up_hook(int box, int slot, uint8_t* out80) {
  if (!g_ed) { log_line("gen12: xferup lift refused: no editable session"); return XG_LIFT_FAILED; }
  /* hard rule 4 / tests/host_gb_write_gate_test.py: this hook itself writes
   * (pdna_bank_next_serial() persists bank.meta), so it carries its own
   * app_can_edit() gate rather than relying only on the caller's can_lift check --
   * the SD side of the write is this function's own responsibility, not begin_
   * select's/the NORMAL-mode menu's, which merely decide whether to LOOK at a cell. */
  if (!app_can_edit()) { log_line("gen12: xferup lift refused: cart is not writable"); return XG_LIFT_FAILED; }
  return gb_lift_pack(box, slot, out80, false);
}

/* BoxXferOps.lift_up (COPY, the read-only nav-menu mount -- BACKLOG #150 S150-12
 * decision 4). Mirror-image guard of gb_lift_up_hook above: refuses a write session
 * outright (a write session never copies -- it MOVEs, through gb_lift_up_hook), and
 * refuses without a mounted read-only path (g_m/g_ro_path, decision 5). BACKLOG #199:
 * box/slot, not rec80, same reason as gb_lift_up_hook above. app_can_edit( is
 * repeated here (not only reachable through gb_lift_pack) for the same
 * tests/host_gb_write_gate_test.py reason gb_lift_up_hook's own copy is: the checker
 * scans each NAMED_WRITE_HOOKS function's OWN body text, never the shared callee it
 * delegates to. */
static int gb_lift_copy_hook(int box, int slot, uint8_t* out80) {
  if (g_ed) { log_line("gen12: copy lift refused: a write session never copies"); return XG_LIFT_FAILED; }
  if (!g_m || !g_ro_path) { log_line("gen12: copy lift refused: no read-only mount path"); return XG_LIFT_FAILED; }
  if (!app_can_edit()) { log_line("gen12: copy lift refused: cart is not writable"); return XG_LIFT_FAILED; }
  return gb_lift_pack(box, slot, out80, true);
}

/* BoxXferOps.release_up (BACKLOG #150 S150-4 decision 7): RE-VERIFIES before it
 * deletes. Between the lift and the drop the user walked to another screen; deleting
 * (box, slot) blind could delete a bystander -- so this reloads the slot and refuses
 * unless its bytes still match `cell80` (the exact 80 bytes the Bank now holds).
 * Same 8-byte-re-verify discipline pdna_bank_clear_slots uses, but a FULL match here
 * (rec[0..meta.rec_len), otname, nick) since bc_unpack() hands back a whole
 * GbEditMon to compare against, not just an 8-byte identity span. Any mismatch or
 * refusal -> gb_rollback() + log, return false -- the caller (drop_held) shows the
 * duplicate message; the Bank already has the mon either way.
 * #280: cell80 is always a native cell (a plain Gen-3 cell here is refused). */
static bool gb_release_up_hook(int box, int slot, const uint8_t cell80[80]) {
  if (!g_ed) return false;
  if (!app_can_edit()) return false;
  if (gbs_box_writable(&g_ed->s, box) != GBS_OK) return false;

  GbSession* s = &g_ed->s;
  if (gbs_load_list(s, box, g_ed->list) != GBS_OK) { gb_rollback(); return false; }
  if (slot >= gb_list_count(s->gen, g_ed->list, box)) { gb_rollback(); return false; }

  GbEditMon have;
  if (!gb_load(&have, s->gen, g_ed->list, box, slot)) { gb_rollback(); return false; }

  /* #280: the G3_HOME arm that used to sit here (a plain Gen-3 cell from the lift-time restore,
   * consumed by gb_release_g3home) is gone -- the lift is a pass-through, so the Bank cell is
   * always this slot's own native record. */
  if (!bc_is_native(cell80)) { gb_rollback(); return false; }

  GbEditMon want; BcMeta meta;
  if (!bc_unpack(cell80, &want, &meta)) { gb_rollback(); return false; }

  (void)meta;
  if (have.gen != want.gen || have.rec_len != want.rec_len ||
      memcmp(have.rec, want.rec, want.rec_len) != 0 ||
      memcmp(have.otname, want.otname, GB_NAME_BYTES) != 0 ||
      memcmp(have.nick, want.nick, GB_NAME_BYTES) != 0) {
    /* #280: no ledger-identity second chance any more (gb_release_restored_verify + the
     * RESTORED mark died with the lift-time restore) -- the cell is always the slot's own
     * bytes, so a mismatch is a genuine bystander. */
    gb_rollback();
    log_line("gen12: xferup box %d slot %d: bystander mismatch, refusing delete", box, slot);
    return false;
  }

  GbsStatus st = gbs_delete(s, box, slot, g_ed->list);
  if (st != GBS_OK) {
    gb_rollback();
    log_line("gen12: xferup box %d slot %d refused: %s", box, slot, gbs_status_text(st));
    return false;
  }

  return gb_persist("xferup");
}

/* BACKLOG #93: DUPLICATE on the read-only mon menu. Once-per-visit warning when the
 * source mon already carries a Gen-3 sidecar claim (decision 2) -- same idiom as
 * pdna_gbtrainer.c's s_id_warned/gbtr_id_edit_ok. */
static bool s_dup_warned;

/* Split out of gb_dup_hook so the confirm dialog's own call frame (app_confirm's text
 * layout locals) is never simultaneously live with gbs_insert()'s -- same "noinline
 * sheds a frame that need not overlap with a later one" discipline gb_release_confirm's
 * own header explains. Loads the record fresh via gb_load into *out (NEVER the
 * converted Gen-3 copy the grid shows -- the design forbids editing off of that); the
 * caller hands *out to gbs_insert() completely unmodified -- decision 2: record bytes
 * stay byte-identical, DVs included, or this makes a different Pokemon. */
static bool __attribute__((noinline))
gb_dup_confirm(uint8_t gen, const uint8_t* list, int box, int slot, GbEditMon* out) {
  if (!gb_load(out, gen, list, box, slot)) return false;
  if (!gb_has_sidecar(gen, out)) return true;      /* no Gen-3 original to double-claim */
  if (s_dup_warned) return true;                   /* once per visit */
  s_dup_warned = true;
  return app_confirm(PDNA_GBEDIT_DUP_SIDECAR_TITLE, PDNA_GBEDIT_DUP_SIDECAR_L1);
}

/* app_src_ops_set() hook: DUPLICATE on the read-only mon menu (BACKLOG #93). Mirrors
 * Gen 3's own app_duplicate (append-then-commit, pdna_main.c) for a source whose real
 * record cannot travel through the Gen-3 clipboard/commit carry (decision 1's header):
 * gbs_insert() appends at the box's own next free slot and enforces capacity itself
 * (GBS_ERR_FULL). No identity-edit warning of the Gen-3 kind -- id_edit_ok has no twin
 * here (decision 2) -- the sidecar note above stands in for it. The success message
 * names the landing slot (decision 1). */
static bool gb_dup_hook(uint8_t* rec80) {
  int box, slot;
  if (!gb_locate(rec80, &box, &slot)) return false;
  GbSession* s = &g_ed->s;

  /* D5 (review-opus, BACKLOG #93): refuse the party pseudo-box in plain English
   * before gbs_insert() ever refuses it with GBS_ERR_ARG (a party-shaped record
   * handed to a box-only destination -- gbs_insert()'s own header) -- that raw
   * status text reads as "bad argument", not a sentence about what actually
   * happened. Same structural-refusal idiom TO DAY-CARE already uses for the
   * same box. */
  if (gb_box_is_party(s->gen, box)) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_DAYCARE_PARTY_TITLE, UI_WARN, PDNA_GBEDIT_DUP_PARTY_L1, 0);
    return false;
  }

  GbsStatus st = gbs_load_list(s, box, g_ed->list);
  if (st != GBS_OK) { snd_deny(); msg_wait(PDNA_GBEDIT_BOXRD_TITLE, UI_WARN, gbs_status_text(st), 0); return false; }
  if (slot >= gb_list_count(s->gen, g_ed->list, box)) {
    snd_deny(); msg_wait(PDNA_GBEDIT_EMPTYSLOT_TITLE, UI_WARN, PDNA_GBEDIT_EMPTYSLOT_L1, 0); return false;
  }

  GbEditMon e;
  if (!gb_dup_confirm(s->gen, g_ed->list, box, slot, &e)) return false;

  int slot_out = -1;
  int dst = box;
  GbsStatus ist = gbs_insert(s, dst, &e, &slot_out, g_ed->list);
  /* BACKLOG #187/#193, F3: a full SOURCE box used to be a flat refusal with no
   * alternative -- Guy's own words ("cant duplicate pokemon ... always errors with a
   * full box") describe exactly this UX gap on a corpus where boxes 1-7 really are
   * 20/20 full. Offer gb_pick_box (same picker MOVE TO BOX uses, now showing n/cap
   * and dimming full boxes too, F3's other half) for a destination WITH room instead
   * of refusing outright; B on the picker, or every other box also being full
   * (gb_pick_box's own "NO DESTINATION" message), still refuses -- nothing is
   * touched (gb_rollback below is a no-op over an untouched image either way). */
  if (ist == GBS_ERR_FULL) {
    dst = gb_pick_box(g_m, box, PDNA_GBEDIT_PICKBOX_DUP_TITLE, true);
    if (dst < 0) { gb_rollback(); return false; }
    ist = gbs_insert(s, dst, &e, &slot_out, g_ed->list);
  }
  if (ist != GBS_OK) {
    gb_rollback();
    log_line("gen12: dup box %d slot %d -> box %d refused: %s", box, slot, dst, gbs_status_text(ist));
    snd_error();
    msg_wait(PDNA_GBEDIT_REFUSED_TITLE, UI_WARN, gbs_status_text(ist), PDNA_GBEDIT_UNCHANGED_L2);
    return false;
  }

  log_line("=== gb dup -> %s box %d slot %d -> box %d slot %d ===", g_ed->path, box, slot, dst, slot_out);
  bool ok = gb_hold_commit("dup");
  if (ok) {
    char l1[32];
    siprintf(l1, "Landed in slot %d.", slot_out + 1);
    msg_wait(PDNA_GBEDIT_DUP_TITLE, UI_OK, l1, 0);
  }
  return ok;
}

/* app_src_ops_set() hook: TO DAY-CARE on the read-only mon menu (BACKLOG #93) -- the
 * PUSH direction from an occupied box cell (the existing Day-Care screen's gbdc_deposit,
 * pdna_gbdaycare.c, is a PULL -- a box picker reached FROM inside the Day-Care page;
 * this is Gen 3's own A_DAYCARE shape reached FROM the mon menu instead). Mirrors
 * app_to_daycare/gbdc_deposit's own steps: find the first free Day-Care slot (Gen 1 has
 * only slot 0 -- GbDaycare.gen1), deposit, delete the source, one persist. The full/
 * confirm/success strings are Gen 3's OWN literals (app_to_daycare, pdna_main.c),
 * reused verbatim. */
static bool gb_daycare_hook(uint8_t* rec80) {
  int box, slot;
  if (!gb_locate(rec80, &box, &slot)) return false;
  GbSession* s = &g_ed->s;

  /* The party is exposed as one more box in this session's own numbering (gb_session's
   * header comment) -- Gen 3's twin excludes it structurally (`!is_bank` around A_DAYCARE);
   * this menu has no such split, so refuse it here with a message, never a bare buzz. */
  if (gb_box_is_party(s->gen, box)) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_DAYCARE_PARTY_TITLE, UI_WARN, PDNA_GBEDIT_DAYCARE_PARTY_L1, 0);
    return false;
  }

  GbsStatus st = gbs_load_list(s, box, g_ed->list);
  if (st != GBS_OK) { snd_deny(); msg_wait(PDNA_GBEDIT_BOXRD_TITLE, UI_WARN, gbs_status_text(st), 0); return false; }
  if (slot >= gb_list_count(s->gen, g_ed->list, box)) {
    snd_deny(); msg_wait(PDNA_GBEDIT_EMPTYSLOT_TITLE, UI_WARN, PDNA_GBEDIT_EMPTYSLOT_L1, 0); return false;
  }

  GbEditMon mon;
  if (!gb_load(&mon, s->gen, g_ed->list, box, slot)) { snd_deny(); return false; }

  /* Egg-ness lives in the list byte, outside the 32-byte record (decision 5's own
   * reasoning, reused here) -- gbd_deposit() has no equivalent structural gate of its
   * own (it only ever sees a box-shaped GbEditMon), so this hook checks first. */
  if (gb_is_egg(&mon)) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_DAYCARE_EGG_TITLE, UI_WARN, PDNA_GBEDIT_DAYCARE_EGG_L1, 0);
    return false;
  }

  GbDaycare dc;
  if (!gbd_read(s, &dc)) { snd_deny(); return false; }
  int dcslot = -1;
  if (!dc.slot[0].occupied) dcslot = 0;
  else if (!dc.gen1 && !dc.slot[1].occupied) dcslot = 1;
  if (dcslot < 0) {
    snd_deny();
    msg_wait("DAY-CARE FULL", UI_WARN, "Take a Pokemon out first.", 0);   /* Gen 3's own words */
    return false;
  }

  if (!app_confirm("Send to Day-Care?", "Moves this Pokemon there.")) return false;  /* Gen 3's own words */

  GbsStatus dst = gbd_deposit(s, dcslot, &mon);          /* lands in RAM, calls gbs_finish() itself */
  if (dst != GBS_OK) {
    gb_rollback();
    log_line("gen12: daycare-put box %d slot %d refused: %s", box, slot, gbs_status_text(dst));
    snd_error();
    msg_wait(PDNA_GBEDIT_REFUSED_TITLE, UI_WARN, gbs_status_text(dst), PDNA_GBEDIT_UNCHANGED_L2);
    return false;
  }

  /* Remove the source AFTER the deposit landed, exactly like gbdc_deposit's own two-
   * commit shape: any failure here rolls the WHOLE image back, not just this half. */
  GbsStatus del = gbs_delete(s, box, slot, g_ed->list);
  if (del != GBS_OK) {
    gb_rollback();
    log_line("gen12: daycare-put box %d slot %d delete refused: %s", box, slot, gbs_status_text(del));
    snd_error();
    msg_wait(PDNA_GBEDIT_REFUSED_TITLE, UI_WARN, gbs_status_text(del), PDNA_GBEDIT_UNCHANGED_L2);
    return false;
  }

  log_line("=== gb daycare-put -> %s box %d slot %d -> daycare slot %d ===",
           g_ed->path, box, slot, dcslot);
  if (!gb_hold_commit("daycare-put")) return false;   /* gb_persist already reported any refusal */
  snd_ok();
  msg_wait("LEFT AT DAY CARE", UI_OK, gb_hold_live() ? "Moved from the box." : "Moved from the box. Saved.", 0);   /* gbdc_deposit's own words */
  pdna_gbdaycare(&g_ed->s, box, app_can_edit());
  return true;
}

/* keep only [A-Za-z0-9] from `in`; collapse runs of other chars to one '_'. Mirrors
 * pdna_pk.c's own (file-static, unexported) `sanitize` byte-for-byte -- small enough
 * that duplicating it here is cheaper than exporting a private helper across files. */
static void gbpk_sanitize(char* out, const char* in, int cap) {
  int o = 0;
  for (int i = 0; in[i] && o < cap - 1; i++) {
    char c = in[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[o++] = c;
    else if (o > 0 && out[o - 1] != '_') out[o++] = '_';
  }
  while (o > 0 && out[o - 1] == '_') o--;
  out[o] = 0;
  if (o == 0) { out[0] = 'M'; out[1] = 'O'; out[2] = 'N'; out[3] = 0; }
}

/* Decision 7's borrowed buffer, split into its two pieces: path first (SF_PATH_MAX,
 * 272 B), then the packed .pk1/.pk2 payload (at most 56 B, GB_PK1_BYTES's own ceiling)
 * right after it. `g_ed->list2` is S3's destination staging buffer (gbs_move's own),
 * never concurrent with an export -- no new statics, per hard rule 2 (EWRAM is ~1,156
 * B free). */
_Static_assert(GBS_LIST_BYTES >= SF_PATH_MAX + 56,
               "g_ed->list2 too small for gb_export_hook's path+payload split");

/* Shared by gb_export_hook and gbsrc_export_all: `PDNA_BANK_DIR/<sanitized-nick-or-
 * species>_<key16>.pk1|.pk2` (decision 6). GB records have no `personality` for
 * .pk3's own %08lX, so gbsc_key/gbsc_key_hex (the sidecar's own fingerprint) stands
 * in. Writes into `path` (caller-owned, >= SF_PATH_MAX); returns false on truncation.
 *
 * D7 (review-opus, BACKLOG #93): gbsc_key() is a pure function of copied fields
 * (gen/otid16/dv4/otname) -- decision 2 keeps a DUPLICATE byte-identical to its
 * source, DVs included, so a mon and its own copy collide on this SAME filename.
 * f_stat() checks before returning; on a hit, `_2`, `_3`, ... is appended before the
 * extension until a free name is found, bounded to 99 tries (golden rule 2: every
 * loop needs a provable upper bound) -- past that, the last-tried (still-colliding)
 * path stands and the write becomes an overwrite rather than looping forever or
 * growing the path unboundedly. */
static bool gb_pk_build_path(char* path, int cap, const GbEditMon* e) {
  char nick[GB_TEXT_MAX];
  gb_get_nickname(e, nick, sizeof nick);
  const char* label = nick[0] ? nick : "MON";
  uint16_t dex = gb_get_species_dex(e);
  if (!nick[0] && dex) label = pk_species_name(dex);
  char sbase[16];
  gbpk_sanitize(sbase, label, sizeof sbase);

  uint8_t dv4[4] = {
    gb_get_dv(e, GB_ATK), gb_get_dv(e, GB_DEF), gb_get_dv(e, GB_SPE), gb_get_dv(e, GB_SPC)
  };
  uint64_t key = gbsc_key(e->gen, gb_get_otid(e), dv4, e->otname);
  char keyhex[17];
  gbsc_key_hex(key, keyhex);
  const char* ext = gb_pk_ext(e->gen);

  int nprint = sniprintf(path, cap, PDNA_BANK_DIR "/%s_%s%s", sbase, keyhex, ext);
  if (nprint < 0 || nprint >= cap) return false;

  FILINFO fi;
  if (f_stat(path, &fi) != FR_OK) return true;      /* no collision, the common case */
  for (int n = 2; n <= 99; n++) {
    int np2 = sniprintf(path, cap, PDNA_BANK_DIR "/%s_%s_%d%s", sbase, keyhex, n, ext);
    if (np2 < 0 || np2 >= cap) return false;
    if (f_stat(path, &fi) != FR_OK) return true;
  }
  return true;   /* suffix range exhausted; the last-tried path stands (an overwrite) */
}

/* app_src_ops_set() hook: EXPORT .pk on the read-only mon menu (BACKLOG #93). Writes a
 * .pk1/.pk2 file (source/gb_pk.c) to the same bank folder /PokeDNA/bank/ the Gen-3
 * .pk3 exporter uses. gb_locate_addr(), NOT gb_locate(): this is a read of the record
 * plus an SD write of a NEW file, never a box mutation, so a virgin Gen-1 bank (which
 * gb_locate's gbs_box_writable gate would refuse) stays exportable -- only the CART
 * gate (app_can_edit(), hard rule 4: Omega-only) applies. */
static bool gb_export_hook(uint8_t* rec80) {
  int box, slot;
  if (!g_ed) return false;
  if (!gb_locate_addr(rec80, &box, &slot)) return false;

  if (!app_can_edit()) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_READONLY_TITLE, UI_WARN, app_gb_readonly_why(), 0);
    return false;
  }

  GbSession* s = &g_ed->s;
  GbsStatus st = gbs_load_list(s, box, g_ed->list);
  if (st != GBS_OK) { snd_deny(); msg_wait(PDNA_GBEDIT_BOXRD_TITLE, UI_WARN, gbs_status_text(st), 0); return false; }
  if (slot >= gb_list_count(s->gen, g_ed->list, box)) {
    snd_deny(); msg_wait(PDNA_GBEDIT_EMPTYSLOT_TITLE, UI_WARN, PDNA_GBEDIT_EMPTYSLOT_L1, 0); return false;
  }

  GbEditMon e;
  if (!gb_load(&e, s->gen, g_ed->list, box, slot)) { snd_deny(); return false; }

  if (gb_is_egg(&e)) {
    snd_deny();
    msg_wait("EGGS", UI_WARN, "Eggs can't be exported.", 0);
    return false;
  }

  /* Assemble the payload in RAM first (rom-load-lab's own f_write-from-ROM bug: the
   * pointer handed to sf_write_verified must never resolve into ROM -- both halves of
   * this split live in g_ed->list2, an EWRAM buffer, never a `const` table). */
  char* path = (char*)g_ed->list2;
  uint8_t* payload = g_ed->list2 + SF_PATH_MAX;
  int wrote = gb_pk_pack(&e, payload, 56);
  if (wrote < 0) {
    snd_error();
    msg_wait("EXPORT FAILED", UI_WARN, "Could not build the file.", 0);
    return false;
  }

  f_mkdir(PDNA_DIR);       /* ignore FR_EXIST */
  f_mkdir(PDNA_BANK_DIR);

  if (!gb_pk_build_path(path, SF_PATH_MAX, &e)) {
    snd_error();
    msg_wait("EXPORT FAILED", UI_WARN, "Path too long.", 0);
    return false;
  }

  rmbl_pause();
  SfStatus wst = sf_write_verified(path, payload, wrote);
  rmbl_resume();

  if (wst == SF_OK) {
    char p2[40]; ui_truncate(p2, path, 29);
    snd_save();
    msg_wait("EXPORTED", UI_OK, p2, "Open it from START > Bank.");
    return true;
  }
  snd_error();
  msg_wait("EXPORT FAILED", UI_WARN, sf_status_str(wst), 0);
  return false;
}

/* BACKLOG #271: EXPORT .pk1/.pk2 for a NATIVE Bank cell (a banked Gen-1/2 record) -- the
 * menu row Gen-3 records already had. Same file format, folder, name rule and collision
 * suffixing as gb_export_hook (shared gb_pk_build_path), but the record comes out of the
 * cell (bc_unpack) instead of a mounted session, so it needs no g_ed and no GB session:
 * it runs from a Gen-3 session's Bank visit too. Only the CART gate applies (Omega-only
 * writes, hard rule 4). The path + payload live in one caller-frame buffer (272 + 56 B).
 * The FRAME is bigger than pdna_pk_export's (488 B vs 320 B); what is equal is the deepest
 * SUBTREE below this menu (2,760 B here vs 2,904 B there), so the chain's STACK budget is unchanged. Nothing in the Bank is modified. */
bool gb_export_native(const uint8_t cell80[80]) {
  GbEditMon e; BcMeta mt;
  if (!cell80 || !bc_unpack(cell80, &e, &mt)) { snd_deny(); return false; }
  if (!app_can_edit()) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_READONLY_TITLE, UI_WARN, app_gb_readonly_why(), 0);
    return false;
  }
  if (gb_is_egg(&e)) { snd_deny(); msg_wait("EGGS", UI_WARN, "Eggs can't be exported.", 0); return false; }
  uint8_t payload[56];
  int wrote = gb_pk_pack(&e, payload, (int)sizeof payload);
  if (wrote < 0) { snd_error(); msg_wait("EXPORT FAILED", UI_WARN, "Could not build the file.", 0); return false; }
  char path[SF_PATH_MAX];
  f_mkdir(PDNA_DIR);       /* ignore FR_EXIST */
  f_mkdir(PDNA_BANK_DIR);
  if (!gb_pk_build_path(path, (int)sizeof path, &e)) {
    snd_error(); msg_wait("EXPORT FAILED", UI_WARN, "Path too long.", 0); return false;
  }
  rmbl_pause();
  SfStatus wst = sf_write_verified(path, payload, wrote);
  rmbl_resume();
  if (wst == SF_OK) {
    char p2[40]; ui_truncate(p2, path, 29);
    snd_save();
    msg_wait("EXPORTED", UI_OK, p2, "Open it from START > Bank.");
    return true;
  }
  snd_error();
  msg_wait("EXPORT FAILED", UI_WARN, sf_status_str(wst), 0);
  return false;
}

/* BoxSource.can_boxops (BACKLOG #93, step 5): the narrow box-menu open gate this
 * step exists to add -- see pdna_box.h's own comment on the field and the finding
 * that shapes it (gbsrc_can_edit() is hardwired false and can_lift is S3's transfer
 * field; neither means "the box options menu may open"). Silent: this is a menu-
 * drawing query, never an action (same posture gb_editable_hook already documents
 * for the same reason). The party pseudo-box refuses here too -- EXPORT ALL/RELEASE
 * ALL on "the box that is really your party" is not a shape this step defines. */
static bool gbsrc_can_boxops_impl(int box) {
  if (!g_ed) return false;
  if (gb_box_is_party(g_ed->s.gen, box)) return false;
  return app_can_edit() && gbs_box_writable(&g_ed->s, box) == GBS_OK;
}

/* BoxSource.can_enter_move (BACKLOG #187/#192, F1): the box-level "may SELECT enter
 * MOVE mode HERE at all" question -- deliberately NOT gbsrc_can_boxops_impl (that one
 * refuses the party pseudo-box outright, and MOVE mode must still work there, e.g.
 * lifting a party mon into another box) and deliberately NOT gbs_can_delete (that is
 * per-SLOT -- party-floor, Mail-holder, the exact refusal a specific mon earns, which
 * stays exactly where the brief puts it: the actual lift on A, via can_lift). This is
 * the box-wide half only: is the session writable, and is this box's own list one the
 * engine will accept a write into. Thin wrapper (gbsrc_can_enter_move) lives up by
 * gbsrc_can_boxops, same PDNA_GEN12_HOST pattern. */
static bool gbsrc_can_enter_move_impl(int box) {
  /* BACKLOG #150 S150-12 (DRIFT finding, folded from BACKLOG #187/#192 F1): the
   * read-only nav-menu mount admits SELECT->MOVE too, box-level only -- no per-slot
   * gbs_can_delete check (a copy deletes nothing, decision 4's own reasoning), same
   * `g_m && g_ro_path` guard gb_lift_why_bs/gb_lift_copy_hook already use. Without
   * this, frame 05 of the --s150-12 shot chain (SELECT enters MOVE on the RO mount)
   * never fires: this gate sits IN FRONT of can_lift at the SELECT dispatch site
   * (source/pdna_box.c) and refused the read-only mount outright, independent of
   * gb_lift_why_bs's own fix above. */
  if (!g_ed) return g_m != NULL && g_ro_path != NULL && app_can_edit();
  return app_can_edit() && gbs_box_writable(&g_ed->s, box) == GBS_OK;
}

/* BoxSource.export_all (BACKLOG #93): one .pk1/.pk2 per occupied slot of `box`,
 * mirroring export_box_all's own shape (pdna_box.c) -- the empty-box early-out, the
 * progress screen per slot, the EXPORTED n/total result panel. No gb_persist: this
 * writes new files under /PokeDNA/bank/, never a byte of the save itself. The
 * progress screen's portrait argument is NULL (Gen-3's `mon_front_for_form` needs an
 * INTERNAL Gen-3 species index, gen1/2's own dex numbering is NOT that index, and
 * mapping one to the other is out of this step's scope) -- title/counter/note still
 * draw, just without a sprite. */
static bool gbsrc_export_all_impl(int box) {
  if (!g_ed || gb_box_is_party(g_ed->s.gen, box)) return false;
  /* b160: gb_export_hook's own single-mon export checks app_can_edit() directly
   * (hard rule 4 -- ALL writes, not just save edits, are Omega-only; a .pk file
   * under /PokeDNA/bank/ is still an SD write) rather than trusting
   * can_boxops()'s menu-open gate alone. This loop reaches the same
   * sf_write_verified() call per slot but never re-checked the cart itself --
   * the same "trust the menu gate" gap item 1 of this slice fixed at the nav
   * dispatch sites. Re-check here too. */
  if (!app_can_edit()) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_READONLY_TITLE, UI_WARN, app_gb_readonly_why(), 0);
    return false;
  }
  GbSession* s = &g_ed->s;
  GbsStatus lst = gbs_load_list(s, box, g_ed->list);
  if (lst != GBS_OK) { snd_deny(); msg_wait(PDNA_GBEDIT_BOXRD_TITLE, UI_WARN, gbs_status_text(lst), 0); return false; }

  int total = gb_list_count(s->gen, g_ed->list, box);
  if (total <= 0) {
    snd_deny();
    ui_clear();
    ui_panel(PDNA_NOTICE_PANEL_X, 60, PDNA_NOTICE_PANEL_W, 44, UI_PANEL, UI_BORDER);
    ui_text(PDNA_NOTICE_TEXT_X, 70, UI_WARN, "BOX IS EMPTY");
    ui_text(PDNA_NOTICE_TEXT_X, 86, UI_DIM, "Nothing to export. Press A");
    u16 kk; do { s_vsync(); kk = key_hit(KEY_A); } while (!kk);
    return false;
  }

  f_mkdir(PDNA_DIR);
  f_mkdir(PDNA_BANK_DIR);

  boxoam_suspend();
  /* D6 (review-opus, BACKLOG #93): an Egg is a STRUCTURAL refusal (decision 5 --
   * the file format cannot carry it, same as gb_export_hook's single-mon refusal),
   * never a write failure -- counting it into `failed` made a box with one Egg and
   * nineteen ordinary exports show "EXPORTED 19 / 20" in UI_WARN (orange), reading
   * as "one broke" when nothing did. Eggs get their own bucket and never turn the
   * panel orange by themselves. */
  int failed = 0, eggs = 0;
  for (int slot = 0; slot < total; slot++) {
    pdna_progress_frame("EXPORT TO .pk", 0, slot, total, "Writing...");
    GbEditMon e;
    bool loaded = gb_load(&e, s->gen, g_ed->list, box, slot);
    if (loaded && gb_is_egg(&e)) {
      eggs++;
    } else {
      bool ok = loaded;
      if (ok) {
        char* path = (char*)g_ed->list2;
        uint8_t* payload = g_ed->list2 + SF_PATH_MAX;
        int wrote = gb_pk_pack(&e, payload, 56);
        ok = wrote >= 0 && gb_pk_build_path(path, SF_PATH_MAX, &e);
        if (ok) { rmbl_pause(); ok = sf_write_verified(path, payload, wrote) == SF_OK; rmbl_resume(); }
      }
      if (!ok) failed++;
    }
    for (int v = 0; v < 3; v++) s_vsync();
  }
  if (failed) snd_error(); else snd_save();
  int exportable = total - eggs;
  ui_clear();
  if (eggs) {
    /* Taller panel, a fourth row for the egg count -- see the header comment: the
     * EXPORTED count is now out of `exportable`, not `total`, so a box that is
     * entirely Eggs and otherwise-successful exports still reads UI_OK green. */
    ui_panel(20, 54, 200, 72, UI_PANEL, failed ? UI_WARN : UI_OK);
    char l[40]; siprintf(l, "EXPORTED %d / %d", exportable - failed, exportable);
    ui_text(30, 64, failed ? UI_WARN : UI_OK, l);
    char eggline[32];
    siprintf(eggline, "%d Egg%s skipped", eggs, eggs == 1 ? "" : "s");
    ui_text(30, 80, UI_DIM, eggline);
    ui_text(30, 96, UI_DIM, "Saved to /PokeDNA/bank/");
    ui_text(30, 110, UI_DIM, "Press A");
  } else {
    ui_panel(20, 54, 200, 58, UI_PANEL, failed ? UI_WARN : UI_OK);
    char l[40]; siprintf(l, "EXPORTED %d / %d", exportable - failed, exportable);
    ui_text(30, 64, failed ? UI_WARN : UI_OK, l);
    ui_text(30, 82, UI_DIM, "Saved to /PokeDNA/bank/");
    ui_text(30, 96, UI_DIM, "Press A");
  }
  u16 kk; do { s_vsync(); kk = key_hit(KEY_A); } while (!kk);
  boxoam_resume();
  return false;   /* writes no save bytes -- box_options_menu's caller re-decodes for nothing either way */
}

/* BoxSource.release_all (BACKLOG #93): confirm with the count (the twin's own string,
 * pdna_box.c's release_box_all), then delete every slot top-down so a mid-failure
 * never shifts the indices of slots not yet visited, roll back + ONE persist -- the
 * two-step "every deletion RAM-only until the single persist" shape decision 5's own
 * paragraph documents: sf_write_verified's .tmp/byte-compare/rename means the live
 * .sav can never be caught half-written. Refuses the party pseudo-box (can_boxops
 * already keeps this unreachable from the menu; kept here too as the structural
 * refusal the step's own text names). */
static bool gbsrc_release_all_impl(int box) {
  if (!g_ed || gb_box_is_party(g_ed->s.gen, box)) return false;
  GbSession* s = &g_ed->s;
  GbsStatus lst = gbs_load_list(s, box, g_ed->list);
  if (lst != GBS_OK) { snd_deny(); msg_wait(PDNA_GBEDIT_BOXRD_TITLE, UI_WARN, gbs_status_text(lst), 0); return false; }

  int total = gb_list_count(s->gen, g_ed->list, box);
  if (total <= 0) {
    snd_deny();
    ui_clear();
    ui_panel(PDNA_NOTICE_PANEL_X, 60, PDNA_NOTICE_PANEL_W, 44, UI_PANEL, UI_BORDER);
    ui_text(PDNA_NOTICE_TEXT_X, 70, UI_WARN, "BOX IS EMPTY");
    ui_text(PDNA_NOTICE_TEXT_X, 86, UI_DIM, "Nothing to release. Press A");
    u16 kk; do { s_vsync(); kk = key_hit(KEY_A); } while (!kk);
    return false;
  }

  char q[40]; siprintf(q, "Release all %d Pokemon?", total);   /* release_box_all's own string */
  if (!app_confirm(q, "Deleted permanently!")) { snd_back(); return false; }

  boxoam_suspend();
  bool ok = true;
  for (int slot = total - 1; slot >= 0 && ok; slot--) {
    GbsStatus dst = gbs_delete(s, box, slot, g_ed->list);
    ok = (dst == GBS_OK);
  }
  if (!ok) {
    /* A mid-way gbs_delete() refusal: gb_rollback() genuinely reverts every delete
     * this loop already applied (g_ed->img <- g_ed->pristine), so "Nothing changed"
     * is accurate here -- this hook's OWN panel is the first and only word on it. */
    gb_rollback();
    log_line("gen12: release-all box %d refused partway", box);
    snd_error();
    ui_clear();
    ui_panel(20, 54, 200, 56, UI_PANEL, UI_WARN);
    ui_text(30, 64, UI_WARN, "RELEASE FAILED");
    ui_text(30, 82, UI_DIM, "Nothing changed.");
    ui_text(30, 96, UI_DIM, "Press A");
    u16 kk; do { s_vsync(); kk = key_hit(KEY_A); } while (!kk);
    boxoam_resume();
    return false;
  }

  /* gb_persist() ALWAYS shows its own message on refusal (the PDNA_DELTA wall or a
   * hardware backup/write-fail dialog, each with its own accurate wording -- the
   * PDNA_DELTA one explicitly does NOT roll RAM back, unlike the mid-way case just
   * above) -- drawing a SECOND "RELEASE FAILED" panel on top of it would be a false
   * "nothing changed" on the one path where something genuinely did. Only success
   * gets a panel of this hook's own. */
  bool persisted = gb_hold_commit("release-all");   /* ONE persist for the whole box */
  if (persisted) {
    if (!gb_hold_live()) snd_save();
    ui_clear();
    ui_panel(20, 54, 200, 56, UI_PANEL, UI_OK);
    ui_text(30, 64, UI_OK, "RELEASED");
    char l[40]; siprintf(l, "Freed %d slots.", total);
    ui_text(30, 82, UI_DIM, l);
    ui_text(30, 96, UI_DIM, "Press A");
    u16 kk; do { s_vsync(); kk = key_hit(KEY_A); } while (!kk);
  }
  boxoam_resume();
  return persisted;
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

/* BACKLOG #177: gb_paste_loss_screen's own fit has exactly 2 px of slack
 * (host_textfit_test.c's "loss screen worst-case height" check, pdna_layout.h:1126-1133
 * says so too) -- there is no room for an 11th conditional row, so ot_lossy/nick_lossy
 * stay ONE row exactly as they already were; only the TEXT gets more specific when only
 * one of the two actually fired (the common case for this backlog item -- the source
 * record's OT name or nickname, never usually both, holds the byte a decoder couldn't
 * spell). Both fired, or the pre-existing gen3_to_gb-side loss also set the other flag,
 * falls back to the original combined wording. */
static const char* loss_name_text(const Gen3ToGbLoss* loss) {
  if (loss->ot_lossy && !loss->nick_lossy)   return PDNA_SIDECAR_LOSS_OTNAME;
  if (loss->nick_lossy && !loss->ot_lossy)   return PDNA_SIDECAR_LOSS_NICKNAME;
  return PDNA_SIDECAR_LOSS_NAME;
}

/* #365: the DOWN confirm's twin of loss_name_text() (that one reads Gen3ToGbLoss; this screen
 * holds a Gb12Notes) -- the same three strings, so the wording matches the GB-bound screen. */
static const char* down_name_text(const Gb12Notes* n) {
  if (n->otname_lossy && !n->nick_lossy) return PDNA_SIDECAR_LOSS_OTNAME;
  if (n->nick_lossy && !n->otname_lossy) return PDNA_SIDECAR_LOSS_NICKNAME;
  return PDNA_SIDECAR_LOSS_NAME;
}

/* BACKLOG #248/#249: the item row's text, five possible shapes (case A HELD / B BAG /
 * C PC / D-E STAYS / no item at all). loss_item_text() itself now lives at the top of
 * this file, ABOVE the PDNA_GEN12_HOST guard (F3/F5, xfer-items fix pass) -- see the
 * comment there for why. */

/* BACKLOG #247: exp_floored shares the EVS row's slot (same reasoning as the item row
 * above -- zero spare row budget). "EVs rescaled" alone when only evs_scaled fired,
 * the new EXP text alone when only exp_floored fired, a combined line when both did. */
static const char* loss_evs_text(const Gen3ToGbLoss* loss) {
  if (loss->evs_scaled && loss->exp_floored) return PDNA_SIDECAR_LOSS_EVS_EXP;
  if (loss->exp_floored)                     return PDNA_SIDECAR_LOSS_EXP_FLOORED;
  return PDNA_SIDECAR_LOSS_EVS;
}
/* BACKLOG #150 S150-12 decision 10 (folds BACKLOG #180): the shared PASTE/bridge
 * screen's footer, by which caller. PASTE keeps the original three lines verbatim
 * (there really is a ledger entry the PASTE route can restore from); BRIDGE gets
 * the honest "the slot empties" wording instead of PASTE's stale "it stays in the
 * Bank" claim (the bridge arm DOES consume the source Bank slot); COPY prints the
 * NOBACK rows instead -- a copy cell has no ledger entry at all, so there is
 * nothing to "keep". A local enum, not a new file-static override: three callers,
 * one screen. */
typedef enum { LOSS_FOOT_PASTE = 0, LOSS_FOOT_BRIDGE, LOSS_FOOT_COPY } GbLossFooter;

static bool __attribute__((noinline)) gb_paste_loss_screen(const Gen3ToGbLoss* loss, GbLossFooter footer) {
  ui_clear();
  ui_text(4, 3, UI_TITLE, PDNA_SIDECAR_LOSS_TITLE);
  ui_hline(0, 13, UI_SCR_W, UI_BORDER);

  int y = PDNA_SIDECAR_LOSS_ROW_Y0;
  y = loss_row(y, loss->nature || loss->ability,      PDNA_SIDECAR_LOSS_NATURE);
  y = loss_row(y, loss->ribbons || loss->contest,     PDNA_SIDECAR_LOSS_RIBBONS);
  y = loss_row(y, loss->met_data || loss->ball,       PDNA_SIDECAR_LOSS_METDATA);
  y = loss_row(y, loss->ivs_halved,                   PDNA_SIDECAR_LOSS_IVS);
  y = loss_row(y, loss->evs_scaled || loss->exp_floored, loss_evs_text(loss));
  {
    char item_row[48];
    loss_item_text(loss, item_row, (int)sizeof item_row);
    y = loss_row(y, g3gb_loss_needs_item_row(loss), item_row);
  }
  /* S5-B review fix #4: two of Gen3ToGbLoss's 17 flags had no row at all before this --
   * merged with item/secret_id above (row-count-neutral: was 2 separate rows, now 1 +
   * this 1, still 10 conditional rows total, matching the screen's own 2px-slack fit). */
  y = loss_row(y, loss->pokerus_dropped || loss->friendship_dropped, PDNA_SIDECAR_LOSS_POKERUS);
  y = loss_row(y, loss->shiny_lost,                   PDNA_SIDECAR_LOSS_SHINY);
  y = loss_row(y, loss->gender_lost,                  PDNA_SIDECAR_LOSS_GENDER);
  y = loss_row(y, loss->nick_lossy || loss->ot_lossy, loss_name_text(loss));

  y += PDNA_SIDECAR_LOSS_ROW_H / 2;
  if (footer == LOSS_FOOT_COPY) {
    ui_ptext_fit(4, y, UI_SCR_W - 8, UI_DIM, PDNA_XFER_COPY_NOBACK_L1); y += PDNA_SIDECAR_LOSS_ROW_H;
    ui_ptext_fit(4, y, UI_SCR_W - 8, UI_DIM, PDNA_XFER_COPY_NOBACK_L2); y += PDNA_SIDECAR_LOSS_ROW_H;
  } else {
    ui_ptext_fit(4, y, UI_SCR_W - 8, UI_DIM, PDNA_SIDECAR_LOSS_KEPT_L1); y += PDNA_SIDECAR_LOSS_ROW_H;
    ui_ptext_fit(4, y, UI_SCR_W - 8, UI_DIM, PDNA_SIDECAR_LOSS_KEPT_L2); y += PDNA_SIDECAR_LOSS_ROW_H;
    ui_ptext_fit(4, y, UI_SCR_W - 8, UI_DIM,
                 footer == LOSS_FOOT_BRIDGE ? PDNA_XFER_BRIDGE_STAYS : PDNA_SIDECAR_LOSS_STAYS);
    y += PDNA_SIDECAR_LOSS_ROW_H;
  }
  y += PDNA_SIDECAR_LOSS_ROW_H / 2;
  ui_text(4, y, UI_TEXT, PDNA_SIDECAR_LOSS_A_TRANSFER); y += PDNA_SIDECAR_LOSS_ROW_H;
  ui_text(4, y, UI_DIM,  PDNA_SIDECAR_LOSS_B_CANCEL);

  u16 k = s_wait(KEY_A | KEY_B);
  return (k & KEY_A) != 0;
}

/* BACKLOG #104 R1 / #150 S150-10 decision 7: KEEP AS IS / MAKE LEGAL, shown when
 * gen3_to_gb_evo_needs_fix() found a level correction to offer AND/OR one or more move
 * slots were out of range for the destination generation (G-H8) -- most transfers never
 * see this screen at all. A SEPARATE screen from gb_paste_loss_screen (see the
 * PDNA_SIDECAR_LEGAL_* comment in pdna_layout.h for why), reusing the exact same
 * primitives/hint convention. B here means "cancel the whole transfer, nothing
 * written" -- gb_paste_hook has already let the earlier loss screen's own B do that
 * once; this is a second, independent chance to back out, not a redefinition of what B
 * means. */
typedef enum { GB_XFER_CANCEL = 0, GB_XFER_KEEP, GB_XFER_MAKE_LEGAL } GbXferChoice;

/* `from4`/`bad4`/`fill4`/`nbad` are the move-swap preview (decision 7); NULL/0 for the
 * plain level-only screen (the two shipped callers, unchanged below). `to_lvl == 0`
 * means "no level correction offered" -- the WHY row and its gap are skipped, matching
 * decision 8's `fix ? to : 0`. `nbad > 0`: one row per bad slot (index order,
 * PDNA_XFER_SWAP_FMT), KEEP AS IS greyed (PDNA_SIDECAR_LEGAL_KEEP_OFF, not in the wait
 * mask -- A does nothing, no beep, the row already says why) and the FIX row reads
 * PDNA_SIDECAR_LEGAL_FIX_MOVES instead of the level "%u -> %u" when there is no level
 * correction alongside it. Worst case (to_lvl != 0 AND all 4 slots bad): WHY(1) + 4 swap
 * rows + KEEP(1) + FIX(1) + BACK(1) + B(1) = 9 rows, 2 half-gaps -- pinned in
 * host_textfit_test.c. */
static GbXferChoice __attribute__((noinline))
gb_paste_legal_screen_ex(uint16_t dex, uint8_t from_lvl, uint8_t to_lvl,
                          const uint16_t from4[4], const uint8_t bad4[4],
                          const uint8_t fill4[4], int nbad) {
  ui_clear();
  ui_text(4, 3, UI_TITLE, PDNA_SIDECAR_LEGAL_TITLE);
  ui_hline(0, 13, UI_SCR_W, UI_BORDER);

  int y = PDNA_SIDECAR_LOSS_ROW_Y0;
  char l2[64];

  /* D3: the WHY row, first, only when a level correction is actually offered. to_lvl
   * is always pk_evo_floor(dex) when present (that is what gen3_to_gb_evo_needs_fix()
   * corrects to) -- "evolves at" is only true when that floor equals the true
   * evolution floor (pk_evo_min_level); when it is instead the lower wild-caught
   * floor, say "legal from" so the claim stays honest. */
  if (to_lvl != 0) {
    int floor = pk_evo_floor(dex);
    int min_lvl = pk_evo_min_level(dex);
    bool is_true_evo_lvl = (floor != PK_EVO_NO_DATA && min_lvl != PK_EVO_NO_DATA &&
                             floor == min_lvl);
    siprintf(l2, is_true_evo_lvl ? PDNA_SIDECAR_LEGAL_WHY_FMT
                                  : PDNA_SIDECAR_LEGAL_WHY_FLOOR_FMT,
             pk_species_name(dex), (unsigned)to_lvl, (unsigned)from_lvl);
    ui_ptext_fit(4, y, UI_SCR_W - 8, UI_TEXT, l2);
    y += PDNA_SIDECAR_LOSS_ROW_H;
  }

  /* Decision 7: one swap row per bad slot, index order -- "ROCK TOMB -> WHIRLPOOL" or
   * "-> (no move)" when nothing eligible was found for that slot. */
  if (nbad > 0 && from4 && bad4 && fill4) {
    for (int i = 0; i < 4; i++) {
      if (!bad4[i]) continue;
      siprintf(l2, PDNA_XFER_SWAP_FMT, pk_move_name(from4[i]),
               fill4[i] ? pk_move_name(fill4[i]) : PDNA_XFER_SWAP_NONE);
      ui_ptext_fit(4, y, UI_SCR_W - 8, UI_TEXT, l2);
      y += PDNA_SIDECAR_LOSS_ROW_H;
    }
  }
  y += PDNA_SIDECAR_LOSS_ROW_H / 2;   /* prose above, the two choices below (r1 re-verify nit) */

  if (nbad > 0) ui_ptext_fit(4, y, UI_SCR_W - 8, UI_DIM, PDNA_SIDECAR_LEGAL_KEEP_OFF);
  else          ui_ptext_fit(4, y, UI_SCR_W - 8, UI_TEXT, PDNA_SIDECAR_LEGAL_KEEP_ROW);
  y += PDNA_SIDECAR_LOSS_ROW_H;

  if (to_lvl != 0) siprintf(l2, PDNA_SIDECAR_LEGAL_FIX_FMT, (unsigned)from_lvl, (unsigned)to_lvl);
  ui_ptext_fit(4, y, UI_SCR_W - 8, UI_TEXT, to_lvl != 0 ? l2 : PDNA_SIDECAR_LEGAL_FIX_MOVES);
  y += PDNA_SIDECAR_LOSS_ROW_H;

  y += PDNA_SIDECAR_LOSS_ROW_H / 2;
  /* Proportional fit, not fixed sys8 ui_text: "Either way it comes back unchanged."
   * is a full sentence (36 chars), far past what an 8px/glyph fixed font leaves room
   * for at x=4 -- the same reason every prose row on this screen goes through
   * ui_ptext_fit rather than ui_text (reserved for the short fixed hints). */
  ui_ptext_fit(4, y, UI_SCR_W - 8, UI_DIM, PDNA_SIDECAR_LEGAL_BACK);
  y += PDNA_SIDECAR_LOSS_ROW_H;
  ui_text(4, y, UI_DIM, PDNA_SIDECAR_LOSS_B_CANCEL);

  /* Decision 7: A is simply not in the mask on a greyed KEEP AS IS row (nbad > 0) --
   * no beep, the row already says why; KEEP is reachable only when nbad == 0. */
  u16 mask = (nbad > 0) ? (KEY_SELECT | KEY_B) : (KEY_A | KEY_SELECT | KEY_B);
  u16 k = s_wait(mask);
  if (k & KEY_B) return GB_XFER_CANCEL;
  return (k & KEY_SELECT) ? GB_XFER_MAKE_LEGAL : GB_XFER_KEEP;
}

/* The shipped 3-argument level-only modal -- unchanged shape for its two existing
 * callers (bank-down's evolution-fix arm and the native<->Gen-3 bridge's, both
 * untouched by this lane). */
static GbXferChoice
gb_paste_legal_screen(uint16_t dex, uint8_t from_lvl, uint8_t to_lvl) {
  return gb_paste_legal_screen_ex(dex, from_lvl, to_lvl, NULL, NULL, NULL, 0);
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

/* The sidecar, then gbs_insert(), then the card -- shared by every Gen-3 -> Game Boy
 * landing (BACKLOG #246: gb_bank_down_g3 below is its only caller now; the menu-driven
 * gb_paste_hook this comment used to describe steps 6-8 of is deleted, along with the
 * PASTE row -- see this file's own delivery report). noinline: GbscEntry (~120 B) +
 * the 48-byte path together are exactly the kind of "sidecar I/O" weight the S5-B
 * brief calls out as needing its own frame, same reasoning as gb_persist's
 * bak[SF_PATH_MAX] split. `orig80` is the TRUE Gen-3 original -- the clipboard
 * record before #246, the Bank cell since -- gbsc_entry_from()'s own original80.
 * `item_outcome`/`g1_item` (BACKLOG #249): NONE/HELD/STAYS never touch the bag here
 * (HELD is already inside `mon` by the time this runs, gen3_to_gb.c's own job);
 * only BAG/PC insert `g1_item` into the resident image, see below. */
static bool __attribute__((noinline))
gb_paste_write(const GbEditMon* mon, int box, const uint8_t orig80[80],
               G3GbItemOutcome item_outcome, uint8_t g1_item) {
  uint8_t dv4[4] = {
    gb_get_dv(mon, GB_ATK), gb_get_dv(mon, GB_DEF),
    gb_get_dv(mon, GB_SPE), gb_get_dv(mon, GB_SPC)
  };
  uint64_t key = gbsc_key(mon->gen, gb_get_otid(mon), dv4, mon->otname);
  char path[GBSC_PATH_MAX];
  /* BACKLOG #150 S150-6, site 2: xr_path_for_key's bool is ignored here -- this key
   * is the one gb_paste_write() is about to WRITE, existing or fresh; either way the
   * path it resolves to (an in-place write of an existing record, or a brand-new
   * file always created under xfer -- decision 4/D-Q7) is exactly where this write
   * belongs. */
  (void)xr_path_for_key(path, key);

  /* BACKLOG #150 S150-6, site 3: the folder this write's mkdir must ensure is now
   * xfer, not sidecar (S5-B review fix #10's own reasoning about f_mkdir's result
   * still applies verbatim). */
  FRESULT mkr = f_mkdir(PDNA_XFER_DIR);
  if (mkr != FR_OK && mkr != FR_EXIST) {
    log_line("gen12: sidecar mkdir %s failed (%d)", PDNA_XFER_DIR, (int)mkr);
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

  /* BACKLOG #246 review F4 fix: refuse the creation of a SECOND ambiguous entry.
   * Every entry already in this file shares the fingerprint (gen/otid16/dv4/
   * otname -- one file per key), so xr_resolve_home's species_written tiebreak is
   * the ONLY thing that can tell two entries apart; if an existing live entry ALSO
   * has species_written == this mon's current dex, the new entry about to be
   * added would be indistinguishable from it and gb_release_g3home's consume
   * would bind to whichever one xr_resolve_home happens to return first --
   * silently swapping the two originals on a later restore. */
  uint16_t paste_nowdex = gb_get_species_dex(mon);
  int paste_collide = xr_resolve_home(g_ed->sidecar, len, mon, XR_KIND_G3_HOME, paste_nowdex);
  if (paste_collide >= 0) {
    GbscEntry cand;
    if (gbsc_get(g_ed->sidecar, len, paste_collide, &cand) && cand.species_written == paste_nowdex) {
      snd_deny();
      msg_wait(PDNA_SIDECAR_AMBIG_TITLE, UI_WARN, PDNA_SIDECAR_AMBIG_L1, PDNA_SIDECAR_AMBIG_L2);
      return false;
    }
  }

  GbscEntry e;
  gbsc_entry_from(&e, mon, orig80, epoch);
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
  app_xv_cache_invalidate();   /* BACKLOG #213: this write's f_mkdir CREATES /PokeDNA/xfer */

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

  /* BACKLOG #249 cases B/C: the item lands in the SAME resident image gbs_insert()
   * just wrote the mon into, BEFORE gb_persist()'s one whole-image verified write
   * below -- one transaction, same rollback machinery as the gbs_insert refusal
   * just above. item_outcome is only ever BAG/PC here for a Gen-1 target whose
   * caller (gb_bank_down_g3) already confirmed room a moment ago; gbb_insert_and_write
   * re-checks for real (never trusts a stale decision) and refuses cleanly if the
   * bag changed out from under it between that check and this write. */
  if (item_outcome == G3GB_ITEM_BAG || item_outcome == G3GB_ITEM_PC) {
    GbBagPocket pocket = (item_outcome == G3GB_ITEM_BAG) ? GBB_POCKET_ITEMS : GBB_POCKET_PC;
    GbBagOpStatus bst = gbb_insert_and_write(&g_ed->s, pocket, g1_item, 1);
    if (bst != GBB_OK) {
      gb_rollback();
      log_line("gen12: paste item insert (id 0x%02X, pocket %d) refused: %d",
               g1_item, (int)pocket, (int)bst);
      gb_paste_sidecar_undo(path);
      snd_error();
      msg_wait(PDNA_SIDECAR_XFER_REFUSED_TITLE, UI_WARN, "Item could not be placed",
               PDNA_GBEDIT_UNCHANGED_L2);
      return false;
    }
  }

  log_line("=== gb paste -> %s box %d slot %d ===", g_ed->path, box, newslot);
  /* #62 review D2: "on failure gb_persist() has already rolled RAM back to what the
   * card holds" is true of the card-write (#else) half only -- the PDNA_DELTA half
   * REFUSES but KEEPS the edit for the rest of this session (there is no card to roll
   * back to; see gb_persist's own PDNA_DELTA branch). */
  bool ok = gb_persist("bank-down");   /* #310: the plain Gen-3 Bank->GB drop reads "Transfer down" (gb_step_name), like xferup/xferdown */
#ifdef PDNA_DELTA
  /* BACKLOG #292 (delta vehicle ONLY): the wall above is "no card", not "this paste is void" -- the mon is
   * in the resident image and stays there for the session, so the drop LANDED as far as this build's image
   * is concerned. Reporting false left a live carry + a stale grid and a second A duplicated the mon.
   * The sidecar the paste wrote is kept (it describes the mon the image now holds). */
  ok = true;
#endif
  if (!ok) gb_paste_sidecar_undo(path);
  return ok;
}

/* BACKLOG #150 S150-8 decision 7: the DOWN-converting edge's own loss screen -- a
 * NEW screen (not gb_paste_loss_screen, which is Gen3ToGbLoss-shaped and lists the
 * wrong things for this direction), built from the SAME primitives (loss_row,
 * PDNA_SIDECAR_LOSS_TITLE/ROW_H/A_TRANSFER/B_CANCEL, the same s_wait(KEY_A|KEY_B)
 * return convention). `g2_item`/`item_travels` name the held-item row exactly as
 * S11.18 Q8 words it. */
/* BACKLOG #150 S150-12 decision 10: `is_copy` adds one extra dim row before
 * A_TRANSFER -- a copy cell's DOWN never reaches the ledger (decision 9), so this
 * is the honest replacement for "there is a transfer record" that every other DOWN
 * implicitly promises. Row budget (re-counted at #365 against the code below): worst case is
 * Item + exp + PID + name-loss + the 2 fixed rows + copy's 2 = 8 content rows of
 * PDNA_SIDECAR_LOSS_ROW_H (9 px) from PDNA_SIDECAR_LOSS_ROW_Y0 (16) -> y=88, then the 4 px
 * half-gap -> A_TRANSFER at 92, B_CANCEL at 101, block ends 110 px, inside UI_SCR_H (160).
 * (to_party shows 1 row where copy shows 2, so the worst case is the copy cell.) */
static bool __attribute__((noinline))
gb_down_loss_screen(const Gb12Notes* n, uint8_t g2_item, bool item_travels, bool is_copy,
                    bool to_party) {
  /* BACKLOG #207 (from the s150-12 review): this dialog had no boxoam_suspend/resume
   * bracket, so the PC box's live OBJ icons sat over the dialog text (visible in the
   * --s150-12 chain's frames 16/20). Bracketed exactly like the same idiom elsewhere
   * in this file (release_box_all, above) and in pdna_box.c (:1089-1091 / :1253-1264):
   * suspend before the FIRST draw, resume after the last key read -- the ONE return
   * below is covered either way. */
  boxoam_suspend();
  ui_clear();
  ui_text(4, 3, UI_TITLE, PDNA_SIDECAR_LOSS_TITLE);
  ui_hline(0, 13, UI_SCR_W, UI_BORDER);

  int y = PDNA_SIDECAR_LOSS_ROW_Y0;
  if (g2_item != 0) {
    char row[40];
    siprintf(row, "Item: %s %s", gb2_item_name(g2_item), item_travels ? "travels" : "waits here");
    y = loss_row(y, true, row);
  }
  y = loss_row(y, n->exp_clamped, "EXP clamped to level 100");
  y = loss_row(y, n->gender_relaxed || n->letter_relaxed, "PID search relaxed");
  /* #365: a nickname / OT name whose spelling changed crossing the bridge (a `[AB]` nickname
   * lands as ` AB `, a long OT truncates) -- one row, the text names which of the two. */
  y = loss_row(y, n->nick_lossy || n->otname_lossy, down_name_text(n));
  y = loss_row(y, true, "IVs come from DVs, nature from EXP");
  y = loss_row(y, true, "Met: this game, traded");
  /* BACKLOG #174 (S150-8c) D7: to_party and is_copy are mutually exclusive -- a COPY
   * cell's DOWN never writes a ledger entry (decision 9), and D2 admits only Bank-origin
   * native cells to the party site, so the row budget (see the header: 8 content
   * rows worst case, block ends at 110 px, inside UI_SCR_H 160) is unchanged by adding this ONE row in the same slot copy's two
   * would otherwise occupy alone. */
  if (to_party) {
    y = loss_row(y, true, PDNA_XFER_PARTYLAND_L1);
  } else if (is_copy) {
    y = loss_row(y, true, PDNA_XFER_COPY_NOBACK_L1);
    y = loss_row(y, true, PDNA_XFER_COPY_NOBACK_L2);
  }

  y += PDNA_SIDECAR_LOSS_ROW_H / 2;
  ui_text(4, y, UI_TEXT, PDNA_SIDECAR_LOSS_A_TRANSFER); y += PDNA_SIDECAR_LOSS_ROW_H;
  ui_text(4, y, UI_DIM,  PDNA_SIDECAR_LOSS_B_CANCEL);

  u16 k = s_wait(KEY_A | KEY_B);
  boxoam_resume();
  return (k & KEY_A) != 0;
}

/* Same cleanup gb_paste_sidecar_undo() performs, but against a CALLER-supplied
 * scratch buffer (decision 8's own deviation note: gb_paste_write's model always
 * has a resident GB session, g_ed->sidecar, to reuse; the native->Gen-3-PC arm can
 * run with NO Game Boy session mounted at all -- app_gen3_pc_live()'s own gate
 * requires !app_arena_held(), which is exactly the state a mounted GB session
 * would never be in while donating its arena tail). */
static void xfer_down_undo(const char* path, uint8_t* scratch) {
  uint32_t len = 0;
  if (sf_read_full(path, scratch, GBSC_FILE_MAX, &len) != SF_OK) {
    log_line("gen12: xfer_down cleanup: could not re-read %s", path);
    return;
  }
  int n = gbsc_count(scratch, len);
  if (n <= 0) { log_line("gen12: xfer_down cleanup: %s is empty", path); return; }
  if (gbsc_remove(scratch, &len, n - 1) != 0) {
    log_line("gen12: xfer_down cleanup: remove failed in %s", path);
    return;
  }
  rmbl_pause();
  SfStatus wst;
  if (gbsc_count(scratch, len) == 0) wst = (f_unlink(path) == FR_OK) ? SF_OK : SF_ERR_WRITE;
  else                                wst = sf_write_verified(path, scratch, len);
  rmbl_resume();
  if (wst != SF_OK) log_line("gen12: xfer_down cleanup: rewrite failed for %s", path);
  else              app_xv_cache_invalidate();   /* BACKLOG #213: a real ledger write */
}

/* BACKLOG #150 S150-8 decision 8: steps (4) of S11.3 for a NATIVE-home transfer,
 * modelled line-for-line on gb_paste_write() above. Differences: `original80` is
 * the NATIVE CELL, not a Gen-3 record; the four S150-6 ledger fields are set
 * explicitly and XR_STATE_CLAIMED is NEVER written here (G-F1, decision 9 promotes
 * later); `nick_written` is overridden with the Gen-3 nickname bytes verbatim
 * (D-8b-link); the lookup is REPLACE-not-append keyed on the cell's own first 8
 * bytes (S11.6), with an evict-oldest retry when the file is full. `scratch` is a
 * caller-owned GBSC_FILE_MAX buffer (see xfer_down_undo's own comment on why this
 * cannot always be g_ed->sidecar). Returns the entry index (>=0) and fills
 * *path_out (>= GBSC_PATH_MAX) on success; <0 on any refusal, with the message
 * already shown. */
static int __attribute__((noinline))
xfer_down_write(uint64_t key, const uint8_t cell80[80], const GbEditMon* written,
                uint8_t direction, const uint8_t nick_g3[10], uint8_t* scratch,
                char path_out[GBSC_PATH_MAX], uint32_t* len_out) {
  (void)xr_path_for_key(path_out, key);

  FRESULT mkr = f_mkdir(PDNA_XFER_DIR);
  if (mkr != FR_OK && mkr != FR_EXIST) {
    log_line("gen12: xfer_down mkdir %s failed (%d)", PDNA_XFER_DIR, (int)mkr);
    snd_error();
    msg_wait(PDNA_SIDECAR_MKDIR_TITLE, UI_WARN, PDNA_SIDECAR_NOTWRITTEN_L2, 0);
    return -1;
  }

  uint32_t len = 0;
  SfStatus rst = sf_read_full(path_out, scratch, GBSC_FILE_MAX, &len);
  if (rst == SF_OK && gbsc_count(scratch, len) < 0) {
    char badpath[GBSC_PATH_MAX + 4];
    int bp = 0;
    while (path_out[bp] && bp < GBSC_PATH_MAX - 1) { badpath[bp] = path_out[bp]; bp++; }
    badpath[bp++] = '.'; badpath[bp++] = 'b'; badpath[bp++] = 'a'; badpath[bp++] = 'd';
    badpath[bp] = 0;
    f_unlink(badpath);
    FRESULT rr = f_rename(path_out, badpath);
    log_line("gen12: xfer_down %s failed its CRC, renamed to %s (%s)",
             path_out, badpath, rr == FR_OK ? "OK" : "FAILED");
    if (rr != FR_OK) {
      snd_error();
      msg_wait(PDNA_SIDECAR_CORRUPT_TITLE, UI_WARN, PDNA_SIDECAR_NOTWRITTEN_L2, 0);
      return -1;
    }
    snd_deny();
    msg_wait(PDNA_SIDECAR_CORRUPT_TITLE, UI_WARN, PDNA_SIDECAR_CORRUPT_KEPT_L1, 0);
    len = (uint32_t)gbsc_init(scratch, key);
  } else if (rst == SF_ERR_OPEN) {
    len = (uint32_t)gbsc_init(scratch, key);
  } else if (rst != SF_OK) {
    snd_error();
    msg_wait(PDNA_SIDECAR_READFAIL_TITLE, UI_WARN, sf_status_str(rst), PDNA_SIDECAR_NOTWRITTEN_L2);
    return -1;
  }

  GbaRtcTime t;
  uint32_t epoch = 0;
  if (gba_rtc_get(&t))
    epoch = ((uint32_t)(t.year - 2000u) << 26) | ((uint32_t)t.month << 22) |
            ((uint32_t)t.day << 17) | ((uint32_t)t.hour << 12) |
            ((uint32_t)t.minute << 6) | (uint32_t)t.second;

  /* BACKLOG #150 S150-9 decision 11: the entry build itself now lives in
   * source/xfer_rec.c's xr_entry_for_down() -- pure gbsc_entry_from() + the five
   * field stores + the nick_written override this comment used to explain -- so the
   * flagship host round-trip test exercises the exact artefact this DOWN edge
   * writes, not a synthetic copy. */
  GbscEntry e;
  xr_entry_for_down(&e, written, cell80, epoch, direction, nick_g3);

  int old = gbsc_find_by_key(scratch, len, cell80);
  if (old >= 0) gbsc_remove(scratch, &len, old);

  int idx = gbsc_add(scratch, &len, GBSC_FILE_MAX, &e);
  if (idx < 0 && gbsc_evict_oldest(scratch, &len) == 0)
    idx = gbsc_add(scratch, &len, GBSC_FILE_MAX, &e);
  if (idx < 0) {
    snd_deny();
    msg_wait(PDNA_XFER_TOOMANY_TITLE, UI_WARN, PDNA_XFER_TOOMANY_L1, PDNA_XFER_TOOMANY_L2);
    return -1;
  }

  rmbl_pause();
  SfStatus wst = sf_write_verified(path_out, scratch, len);
  rmbl_resume();
  log_line("gen12: xfer_down %s: %s", path_out, wst == SF_OK ? "OK" : sf_status_str(wst));
  if (wst != SF_OK) {
    snd_error();
    msg_wait(PDNA_SIDECAR_NOTWRITTEN_TITLE, UI_WARN, sf_status_str(wst), PDNA_SIDECAR_NOTWRITTEN_L2);
    return -1;
  }
  app_xv_cache_invalidate();   /* BACKLOG #213: a real ledger write */
  if (len_out) *len_out = len;
  return idx;
}

/* BACKLOG #150 S150-8 decision 15: promote the entry xfer_down_write() just wrote
 * (at index `idx`, still resident in `scratch`/`len`) to XR_STATE_CLAIMED
 * immediately -- the bridge arm's write is NOT deferred like the Gen-3 arm's (no
 * static is involved; gb_persist() below has already, or is about to, verify the
 * destination on disk in the SAME gesture). Best-effort: a failure here leaves the
 * entry PENDING, which is fail-safe (never removable, S11.8), so it only logs. */
static void xfer_down_claim_now(uint8_t* scratch, uint32_t len, int idx, const char* path) {
  GbscEntry e;
  if (!gbsc_get(scratch, len, idx, &e)) { log_line("gen12: xfer_down claim: bad index"); return; }
  e.state = XR_STATE_CLAIMED;
  if (gbsc_remove(scratch, &len, idx) != 0) { log_line("gen12: xfer_down claim: remove failed"); return; }
  int nidx = gbsc_add(scratch, &len, GBSC_FILE_MAX, &e);
  if (nidx < 0) { log_line("gen12: xfer_down claim: re-add failed"); return; }
  rmbl_pause();
  SfStatus wst = sf_write_verified(path, scratch, len);
  rmbl_resume();
  if (wst != SF_OK) log_line("gen12: xfer_down claim: rewrite failed for %s", path);
  else              app_xv_cache_invalidate();   /* BACKLOG #213: a real ledger write */
}

/* BACKLOG #150 S150-8 decision 3/5/6/8/9/11/12/16, arm 2: a native cell converts
 * into a real Gen-3 record for the Gen-3 PC box `dst_box`/`dst_cell` addresses.
 * Deliberately touches no `src->*` member (bank_down_convert.h's own comment on
 * this signature explains why -- the stack-budget walker's GATED-4 gate). `dstrec`
 * is the 80 bytes already at the destination (read-only, the caller's own already-
 * loaded box); on BANK_DOWN_CONVERTED `out80` holds the finished record and the
 * CALLER places it (pdna_box.c's drop_held). The ledger write is DEFERRED-promoted
 * (decision 9): the PC write itself stays the caller's own `mark_dirty()`, never
 * committed here, and the entry stays XR_STATE_PENDING until flush_on_exit's own
 * app_xfer_promote() sees the PC verified on disk. */
BankDownResult gb_bank_down_gen3(BoxSource* src, int dst_box, int dst_cell,
                                 const uint8_t cell80[80], const uint8_t dstrec[80],
                                 uint8_t out80[80]) {
  (void)src; (void)dst_cell;
  if (!app_can_edit()) { snd_deny(); return BANK_DOWN_REFUSED; }             /* 16(a) */
  if (!app_gen3_pc_live()) { snd_deny(); return BANK_DOWN_REFUSED; }         /* 16(b), G-F2 */

  /* BACKLOG #150 S150-12 decision 9: a COPY cell (its GB original still exists,
   * BC_FLAG_COPY) never reaches xfer_down_write/app_xfer_pending_set below -- an
   * entry keyed by this cell's gbsc_key would collide with the entry the STILL-LIVING
   * original would derive (SS11.7 G-L3, the clone-claims-the-original hole). */
  const bool copy = xg_cell_is_copy(cell80);

  GbEditMon written;
  Gb12Notes notes;
  uint16_t g3item = 0;
  uint8_t met_game = app_met_game();
  Gb12Result cr = bdc_convert_gen3_core(cell80, met_game, out80, &written, &notes, &g3item);
  if (cr != GB12_OK) {                                                      /* 16(c)/(d): egg/damaged/other */
    snd_deny();
    /* BACKLOG #168a review D6: this dialog is drawn with the PC box's OBJ icons
     * still live, same class of bug as #207's loss screen (gb_down_loss_screen,
     * above) -- bracket it the same way. */
    boxoam_suspend();
    msg_wait(PDNA_SIDECAR_XFER_TITLE, UI_WARN, gen12_reason_text(cr), 0);
    boxoam_resume();
    return BANK_DOWN_REFUSED;
  }

  if (app_bank_defer_full()) { snd_deny(); return BANK_DOWN_REFUSED; }      /* 16(f) */
  /* S150-12 decision 9: a copy has no ledger entry to promote, so N copies may land
   * in one session without a SAVE-FIRST wall -- the S150-8d constraint this pre-
   * flight guards is about ledger entries, never about cells. */
  if (!copy && app_xfer_pending()) {                                        /* 16(g), decision 9 */
    /* BACKLOG #175 (S150-8d) D13: offer SAVE NOW? instead of a flat refusal -- the
     * shipped SAVE FIRST msg_wait is gone (its own strings stay live for the two
     * restore-side SAVE FIRST refusals, D18, decision 18); this is the ONLY site
     * that used them for the pending-transfer wall. BACKLOG #168a review D6's own
     * bracket (the PC box's OBJ icons must not sit under the dialog) still applies. */
    boxoam_suspend();
    char l1[64];
    siprintf(l1, "%s %s", PDNA_XFER_SAVENOW_L1, PDNA_XFER_SAVENOW_L2);
    bool yes = app_confirm(PDNA_XFER_SAVENOW_TITLE, l1);      /* A = save now, B = no */
    bool ok  = yes && app_xfer_save_now();                    /* D14 */
    boxoam_resume();
    if (!ok) { snd_deny(); return BANK_DOWN_REFUSED; }        /* the helper/confirm already said why */
  }

  bool occ = (dstrec[0] | dstrec[1] | dstrec[2] | dstrec[3]) != 0 || bc_is_native(dstrec);
  if (occ) { snd_deny(); return BANK_DOWN_REFUSED; }                        /* 16(h) */

  bool travels = (g3item != 0);
  const bool to_party = (dst_box < 0);   /* BACKLOG #174 D7: gb_bank_down_gen3's own dst_box<0
                                          * test, so bank_down_convert_gen3_party's exported
                                          * signature stays unchanged (D1) */
  if (!gb_down_loss_screen(&notes, notes.item_g2, travels, copy, to_party)) return BANK_DOWN_REFUSED;

  /* decision 7's MAKE LEGAL correction: the mon standing below pk_evo_floor(dex). */
  PkMon pk;
  if (pk_decode_mon(out80, false, &pk)) {
    pk_resolve(&pk);
    uint16_t dex = pk_national_no(pk.species);
    if (dex && pk_evo_have_data() && pk.level < (uint8_t)pk_evo_floor(dex)) {
      uint8_t from_lvl = pk.level, to_lvl = (uint8_t)pk_evo_floor(dex);
      GbXferChoice ch = gb_paste_legal_screen(dex, from_lvl, to_lvl);
      if (ch == GB_XFER_CANCEL) return BANK_DOWN_REFUSED;
      if (ch == GB_XFER_MAKE_LEGAL) {
        EditMon em; gen3_edit_load(out80, false, &em);
        em_set_level(&em, to_lvl);
        gen3_edit_commit(&em, out80);
        (void)gb_set_level(&written, to_lvl);   /* the ledger's written_level = the level ACTUALLY written abroad (G-H9/R1) */
      }
    }
  }
  if (g3item != 0) {
    EditMon em; gen3_edit_load(out80, false, &em);
    em_set_item(&em, g3item);
    gen3_edit_commit(&em, out80);
  }

  /* decision 8: the ledger write, BEFORE the PC write (verified first). No resident
   * GB session is guaranteed here (see xfer_down_undo's own comment) -- this arm's
   * own noinline frame carries the GBSC_FILE_MAX scratch (1042 B), never a new
   * static of any kind. */
  uint8_t scratch[GBSC_FILE_MAX];
  char path[GBSC_PATH_MAX];
  /* F3 (review): the Gen-3 record's own 10 raw nickname bytes (gen3_mon.c's own
   * decode_name(out->nickname, mon + 0x08, 10) call site names the offset) -- the
   * nickname AS WRITTEN ABROAD, not the GB bytes gbsc_entry_from() would otherwise
   * copy from `written` (the cell's own unpacked GB record). */
  if (!copy) {
    int idx = xfer_down_write(xr_key_g3(out80), cell80, &written, XR_DIR_ABROAD_G3,
                              out80 + 0x08, scratch, path, NULL);
    if (idx < 0) return BANK_DOWN_REFUSED;

    app_xfer_pending_set(xr_key_g3(out80), (int16_t)idx);                   /* decision 9 */
    log_line("gen12: down->gen3 box %d slot %d: pending, %s", dst_box, dst_cell, path);
  } else {
    log_line("gen12: down->gen3 box %d slot %d: copy cell, no ledger entry", dst_box, dst_cell);
  }
  return BANK_DOWN_CONVERTED;
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

/* Just the dex number a Gen-3 -> Game Boy landing needs to look up base stats for --
 * PkMon (~90 B) is exactly the kind of weight this slice's brief says must not ride
 * in the caller's own frame, so it gets its own noinline frame instead. 0 (an
 * impossible dex) on any decode failure or an Egg, which have no base stats to fetch
 * and are about to be refused by gen3_to_gb's own EGG/GLITCH checks the moment the
 * caller retries it. BACKLOG #246: takes `rec80` explicitly -- this used to read
 * app_clip_rec() (the clipboard) when its only caller was the now-deleted
 * gb_paste_hook; its ONLY caller now (gb_bank_down_g3, below) hands in the Bank
 * cell instead, so there is nothing clipboard-shaped left to default to. */
static uint16_t __attribute__((noinline)) gb_rec_dex(const uint8_t rec80[80]) {
  PkMon m;
  if (!pk_decode_mon(rec80, false, &m) || m.isEgg || m.isBadEgg) return 0;
  return pk_national_no(m.species);
}

/* BACKLOG #150 S150-10 decision 8, step 2: bad4/from4 for a Gen-3 -> Game Boy
 * landing, off the SAME decode gb_rec_dex() above does (a second, independent
 * decode -- this function's own frame, same reasoning as gb_rec_dex's). -1 on a
 * decode failure or an Egg (mirrors gb_rec_dex's own refusal set exactly): the
 * caller then passes bad4 = NULL into gen3_to_gb_fixed, so it refuses precisely the
 * way gen3_to_gb() always has. Otherwise returns g3gb_moves_ok()'s bad count (0..4)
 * and fills from4 with the raw Gen-3 move ids (for the modal's "X -> Y" rows --
 * pk_move_name() takes them directly, no re-decode needed later). BACKLOG #246:
 * takes `rec80` explicitly, same reasoning as gb_rec_dex above. */
static int __attribute__((noinline))
gb_rec_moves(const uint8_t rec80[80], uint8_t gen, uint16_t from4[4], uint8_t bad4[4]) {
  PkMon m;
  if (!pk_decode_mon(rec80, false, &m) || m.isEgg || m.isBadEgg) return -1;
  for (int i = 0; i < 4; i++) from4[i] = m.moves[i];
  return g3gb_moves_ok(m.moves, gen, bad4);
}

/* Defined below, after gb_create_locate_rom/gb_create_base1/gb_create_learn (the
 * learnset block this needs, BACKLOG #150 S150-10 decision 5) -- forward-declared here
 * because gb_paste_hook, which calls it, sits earlier in this file than that block. */
static int __attribute__((noinline)) gb_paste_fill_moves(uint16_t dex, uint8_t level, GbEditMon* mon,
                                const uint8_t bad4[4], uint8_t fill4[4], bool* no_rom);

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

static bool __attribute__((noinline)) gb_create_rom_available(void) {
  const char* reg = app_gb_rom_path(g_ed->s.gen);
  if (reg && reg[0]) return true;
#ifndef PDNA_DELTA
  gb_rom_base_path();
  int bl = 0; while (g_ed->romspath[bl]) bl++;
  static const char* const kExt[2] = { ".gb", ".gbc" };
  for (int e = 0; e < 2; e++) {
    int bp = bl;
    for (int i = 0; kExt[e][i] && bp < (int)sizeof(g_ed->romspath) - 1; i++) g_ed->romspath[bp++] = kExt[e][i];
    g_ed->romspath[bp] = 0;
    memset(&g_ed->romfil, 0, sizeof g_ed->romfil);
    if (f_open(&g_ed->romfil, g_ed->romspath, FA_READ) == FR_OK) { f_close(&g_ed->romfil); g_ed->romspath[bl] = 0; return true; }
    g_ed->romspath[bl] = 0;
  }
#endif
  return false;
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
#ifdef PDNA_DELTA
  /* BACKLOG #112: no SD under PDNA_DELTA -- the beside-the-save f_open() below can
   * never succeed (there is no card to open a file on), so PASTE (GB)'s Gen-1 base-
   * stats lookup ALWAYS refused ("NO GEN-1 ROM") on this build, before the loss
   * screen ever rendered. Fall back to the already-fused ROM's own table instead of
   * a second SD open -- same fused_gb_slice_read GbReadFn + FusedGbSlice shape
   * gb_create_locate_rom (~:2303-2314, CREATE's own precedent for exactly this) and
   * gb_art_source.c's rom_gbsprite_open_loc() callers already use; s_gb_create_slice
   * is reused rather than a new static (the fused ROM's bytes are the same table
   * regardless of which caller asks: fused_gb_rom() answers off fused_gb_set_active_
   * save()'s current pick, not anything CREATE-specific). SD path (below, #else) is
   * untouched -- this guard changes nothing there. */
  const uint8_t* base; uint32_t size;
  if (!fused_gb_rom(GB_GEN1, &base, &size)) {
    log_line("gen12: gen-1 rom: no fused gen-1 rom");
    return GB1BASE_NO_ROM;
  }
  s_gb_create_slice.base = base;
  s_gb_create_slice.size = size;
  /* BACKLOG #201 F4: this whole function is Gen-1-only (gb_gen1_locate_rom) -- the
   * hint is known before the call, not just after it. */
  int ok = rom_gbsprite_open(&g_ed->romgs, fused_gb_slice_read, &s_gb_create_slice, size,
                             g_ed->romscan, sizeof g_ed->romscan, GB_ROM_GEN1);
  if (!ok || g_ed->romgs.gen != GB_ROM_GEN1) {
    log_line("gen12: gen-1 rom: fused rom did not open as a Gen-1 rom");
    return GB1BASE_BAD_ROM;
  }
  g_ed->romgs_ready = true;
  g_ed->romgs_path = g_ed->path;
  return GB1BASE_OK;
#else
  /* BACKLOG #269: try the REGISTERED Gen-1 ROM (Settings > Game ROM) FIRST, the same
   * order gb_create_locate_rom uses. This probe used to look ONLY beside the save, so a
   * cart session that registered yellow.gb (and read its art fine) still got "Put
   * yellow.gb here" the moment a Gen-3 -> Gen-1 transfer needed base stats. */
  {
    const char* reg = app_gb_rom_path(GB_GEN1);
    if (reg && reg[0]) {
      int ri = 0;
      for (; reg[ri] && ri < (int)sizeof(g_ed->romspath) - 1; ri++) g_ed->romspath[ri] = reg[ri];
      g_ed->romspath[ri] = 0;
      memset(&g_ed->romfil, 0, sizeof g_ed->romfil);
      if (f_open(&g_ed->romfil, g_ed->romspath, FA_READ) == FR_OK) {
        FSIZE_t rsz = f_size(&g_ed->romfil);
        uint32_t rs = (rsz > (FSIZE_t)0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)rsz;
        int rok = rom_gbsprite_open(&g_ed->romgs, gb_read, &g_ed->romfil, rs,
                                    g_ed->romscan, sizeof g_ed->romscan, GB_ROM_GEN1);
        f_close(&g_ed->romfil);
        if (rok && g_ed->romgs.gen == GB_ROM_GEN1) {
          log_line("gen12: gen-1 rom: registered %s", g_ed->romspath);
          g_ed->romgs_ready = true;
          g_ed->romgs_path = g_ed->path;
          return GB1BASE_OK;
        }
        log_line("gen12: gen-1 rom: registered %s did not open as Gen-1", g_ed->romspath);
      }
    }
  }
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
  /* BACKLOG #201 F4: same Gen-1-only function as the fused-path call above. */
  int ok = rom_gbsprite_open(&g_ed->romgs, gb_read, &g_ed->romfil, sz,
                             g_ed->romscan, sizeof g_ed->romscan, GB_ROM_GEN1);
  f_close(&g_ed->romfil);
  if (!ok || g_ed->romgs.gen != GB_ROM_GEN1) {
    log_line("gen12: gen-1 rom: %s did not open as a Gen-1 ROM", g_ed->romspath);
    return GB1BASE_BAD_ROM;
  }
  g_ed->romgs_ready = true;
  g_ed->romgs_path = g_ed->path;
  return GB1BASE_OK;
#endif /* PDNA_DELTA */
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
   * did not itself create. BACKLOG #112: under PDNA_DELTA there is no FIL to reopen
   * (gb_gen1_locate_rom's own PDNA_DELTA branch above never opened one) -- read
   * straight back out of s_gb_create_slice through the same fused_gb_slice_read
   * shim it used to locate the ROM. */
#ifdef PDNA_DELTA
  RomGb1Species sp;
  bool got = rom_gbbase_gen1(&g_ed->romgs, fused_gb_slice_read, &s_gb_create_slice, dex, &sp);
#else
  memset(&g_ed->romfil, 0, sizeof g_ed->romfil);
  if (f_open(&g_ed->romfil, g_ed->romspath, FA_READ) != FR_OK) {
    log_line("gen12: gen-1 rom: %s could not be reopened", g_ed->romspath);
    g_ed->romgs_ready = false;              /* the card changed underneath us -- rescan next time */
    return GB1BASE_BAD_ROM;
  }
  RomGb1Species sp;
  bool got = rom_gbbase_gen1(&g_ed->romgs, gb_read, &g_ed->romfil, dex, &sp);
  f_close(&g_ed->romfil);
#endif /* PDNA_DELTA */
  if (!got) {
    log_line("gen12: gen-1 rom: %s has no readable row for dex %u", g_ed->romspath, dex);
    return GB1BASE_BAD_ROM;
  }
  log_line("gen12: gen-1 base stats: dex %u from %s (base_stats offset 0x%lX)",
           dex, g_ed->romspath, (unsigned long)g_ed->romgs.base_stats);
  *out = sp.base;
  return GB1BASE_OK;
}

/* BACKLOG #367: the Gen-1 BaseStats row for a Day-Care mon about to land in the PARTY
 * (pdna_gbdaycare.c's gbdc_land) -- the same ROM lookup the Bank TO-GAME landing uses,
 * exposed without its UI so the caller keeps its own honest refusal. NO_ROM = no ROM beside
 * the .sav (caller names it with gb_gen12_norom_msg, like the Bank twin); BAD = bad ROM or an
 * impossible species; `out` untouched unless OK. */
Gb12BaseSt gb12_gen1_base_for(const GbEditMon* mon, GbGen1Base* out) {
  if (!mon || !out) return GB12_BASE_BAD;
  uint16_t dex = gb_get_species_dex(mon);
  if (!dex) return GB12_BASE_BAD;
  Gb1BaseStatus bst = gb_gen1_base_from_rom(dex, out);
  if (bst == GB1BASE_OK) return GB12_BASE_OK;
  return (bst == GB1BASE_NO_ROM) ? GB12_BASE_NO_ROM : GB12_BASE_BAD;
}

/* The ROM was absent: name it. base_only points INTO the arena-resident romspath
 * (still holding "<dir><base>" after the failed locate above -- gb_gen1_base_from_rom's
 * own probe, or, since BACKLOG #150 S150-10 decision 10, gb_create_locate_rom's), so
 * this needs no path buffer of its own -- only the short printf-staging one, which
 * (like every message string in this tree) is safe by construction: msg_wait()
 * itself runs l1 through ui_ptext_fit(), so a long path is clipped, never overflowed.
 *
 * Decision 10: generalised by generation (was gb_gen1_norom_msg(void), Gen-1-only) --
 * ".gb"/GEN-1 title for GB_GEN1, ".gbc"/NEW GEN-2 title for GB_GEN2; L1's wording never
 * named an extension, so it is reused unchanged for both. */
void __attribute__((noinline)) gb_gen12_norom_msg(uint8_t gen) {
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
  siprintf(l1, "Put %.48s.%s here", base_only, gen == GB_GEN1 ? "gb" : "gbc");
  msg_wait(gen == GB_GEN1 ? PDNA_SIDECAR_GEN1_TITLE : PDNA_SIDECAR_GEN2_TITLE,
           UI_WARN, l1, PDNA_SIDECAR_GEN1_L1);
}

/* BACKLOG #212 review D9 / #390: the zero-move refusal. For GEN 1 the base-stats gate has already proved a ROM is
 * present by the time it runs (gb_gen12_norom_msg above would be a false "the ROM is missing" claim), so the true
 * reason is that no eligible move was found in that ROM's learnset for this species/level. GEN 2 has no such gate:
 * its callers check gb_paste_fill_moves' no_rom flag first and name the missing ROM instead. */
static void __attribute__((noinline)) gb_gen12_nomoves_msg(uint8_t gen) {
  msg_wait(gen == GB_GEN1 ? PDNA_SIDECAR_NOMOVES_TITLE1 : PDNA_SIDECAR_NOMOVES_TITLE2,
           UI_WARN, PDNA_SIDECAR_NOMOVES_L1, 0);
}

/* BACKLOG #150 S150-8 decision 14/15, arm 1: a native cell bridges into the
 * CURRENTLY MOUNTED Game Boy session's OTHER generation, under the time-capsule
 * rules. `dst_box` is the box the cursor is on; gbs_insert() assigns the slot
 * itself (no dst_cell -- a GB box is not slot-addressable the way a Gen-3 grid
 * is, exactly as gb_paste_hook's own gb_locate()-free insert already works).
 * Never returns BANK_DOWN_CONVERTED: on success the destination is already
 * written and the ledger entry promoted to XR_STATE_CLAIMED (no deferred commit
 * on this arm -- decision 15). */
BankDownResult gb_bank_down_bridge(int dst_box, const uint8_t cell80[80]) {
  if (!app_can_edit() || !g_ed) { snd_deny(); return BANK_DOWN_REFUSED; }    /* 16(a)/(b) */
  /* BACKLOG #150 S150-12 decision 9: same derivation as gb_bank_down_gen3 -- a COPY
   * cell's GB original still exists, so this arm must not write a ledger entry for
   * it either. */
  const bool copy = xg_cell_is_copy(cell80);
  if (gb_box_is_party(g_ed->s.gen, dst_box)) {
    snd_deny();
    msg_wait(PDNA_SIDECAR_XFER_TITLE, UI_WARN, PDNA_SIDECAR_PARTY_L1, 0);
    return BANK_DOWN_REFUSED;
  }

  uint8_t dst_gen = g_ed->s.gen;   /* the MOUNTED session IS the destination (gbs_insert requires mon->gen == s->gen); the cell is the OTHER generation -- review F1 of the merged tree */

  int tc; uint16_t tc_bad; Gb12Result g12; G3GbStatus g3gb;
  GbEditMon mon; Gen3ToGbLoss loss; Gb12Notes notes;
  /* BACKLOG #212: bad4/nbad are bdc_convert_gb_core's own per-slot verdict (S150-10's
   * g3gb_moves_ok, applied to dst_gen's own bound); from4 is the cell's raw move ids,
   * for the swap-row modal below (mirrors gb_paste_hook's own from4/bad4, decision 8
   * step 2). */
  uint16_t from4[4]; uint8_t bad4[4]; int nbad;
  bool crystal = gb_session_is_crystal(&g_ed->s);
  bdc_convert_gb_core(cell80, dst_gen, crystal, NULL, &tc, &tc_bad, &g12, &g3gb, &mon, &loss,
                      &notes, from4, bad4, &nbad);
  if (tc == 1) {                                                            /* decision 14 */
    snd_deny();
    char l1[64]; siprintf(l1, PDNA_XFER_TC_SPECIES_FMT, pk_species_name(tc_bad));
    msg_wait(PDNA_XFER_TC_TITLE, UI_WARN, l1, 0);
    return BANK_DOWN_REFUSED;
  }
  /* BACKLOG #212: `tc == 2` (the move-bound whole-cell refusal) can no longer fire --
   * bdc_convert_gb_core now passes NULL moves into xr_time_capsule_block (species-floor
   * only) and clips the move bound itself via bad4/nbad instead (handled below, after
   * the loss screen, the same place gb_paste_hook applies its own fill). */
  if (g12 != GB12_OK) {                                                     /* 16(c)/(d) */
    snd_deny();
    msg_wait(PDNA_SIDECAR_XFER_TITLE, UI_WARN, gen12_reason_text(g12), 0);
    return BANK_DOWN_REFUSED;
  }
  if (g3gb == G3GB_ERR_NEEDS_BASE) {                                        /* S5-C's own retry, verbatim shape */
    /* Re-derive the intermediate species dex the same way gb_rec_dex() would, off
     * the cell's own view -- bdc_convert_gb_core() already decoded it once
     * internally; re-unpack here rather than widening that function's signature
     * just to smuggle one uint16_t out on the ONE refusal path that needs it. */
    GbEditMon srcmon; BcMeta srcmeta;
    uint16_t dex = 0;
    if (bc_unpack(cell80, &srcmon, &srcmeta)) {
      Gb12Mon view;
      if (bc_view(&srcmon, &srcmeta, bc_ident32(cell80), &view)) dex = view.species_dex;
    }
    GbGen1Base g1base;
    Gb1BaseStatus bst = dex ? gb_gen1_base_from_rom(dex, &g1base) : GB1BASE_BAD_ROM;
    if (bst == GB1BASE_NO_ROM) { snd_deny(); boxoam_suspend(); gb_gen12_norom_msg(GB_GEN1); boxoam_resume(); return BANK_DOWN_REFUSED; }
    if (bst != GB1BASE_OK) {
      snd_deny();
      msg_wait(PDNA_SIDECAR_XFER_TITLE, UI_WARN, PDNA_SIDECAR_GEN1_BADROM_L1, 0);
      return BANK_DOWN_REFUSED;
    }
    bdc_convert_gb_core(cell80, dst_gen, crystal, &g1base, &tc, &tc_bad, &g12, &g3gb, &mon, &loss,
                        &notes, from4, bad4, &nbad);
  }
  if (g3gb != G3GB_OK) {
    snd_deny();
    msg_wait(PDNA_SIDECAR_XFER_TITLE, UI_WARN, g3gb_status_text(g3gb), 0);
    return BANK_DOWN_REFUSED;
  }

  loss.item_dropped |= notes.item_dropped;   /* S150-8 decision 15: the Gen-2 item stays behind */
  if (notes.item_dropped && notes.item_g2 != 0 && loss.g3_held_item == 0)
    loss.g3_held_item = item_g2_to_g3(notes.item_g2);   /* #376: name it on the row (0 = no Gen-3 counterpart) */
  /* BACKLOG #177: gb_name_changed() (bank_down_convert.c, review F1) already compared
   * the SOURCE record's name spelling against the WRITTEN record's; fold the verdict
   * into Gen3ToGbLoss's own ot_lossy/nick_lossy so the pre-existing PDNA_SIDECAR_LOSS_NAME
   * row (gb_paste_loss_screen below) fires for this loss too, exactly the way it
   * already fires for gen3_to_gb's own (different) name loss. */
  loss.ot_lossy   |= notes.otname_lossy;
  loss.nick_lossy |= notes.nick_lossy;
  /* decision 10: COPY prints the NOBACK rows instead of KEPT/STAYS -- there is no
   * ledger entry to keep. BACKLOG #212 decision 6 (mirrored from S150-10): "say so"
   * lives on the swap-row modal below, not a new row here -- no change to this call. */
  boxoam_suspend();
  if (!gb_paste_loss_screen(&loss, copy ? LOSS_FOOT_COPY : LOSS_FOOT_BRIDGE)) { boxoam_resume(); return BANK_DOWN_REFUSED; }  /* decision 15: the shipped screen */
  boxoam_resume();

  uint8_t fix_from = 0, fix_to = 0;
  bool fix = gen3_to_gb_evo_needs_fix(&mon, &fix_from, &fix_to);            /* R1 block, verbatim */

  /* BACKLOG #212: the SAME per-slot fill BACKLOG #150 S150-10 gives PASTE (decision 8
   * step 7) -- fetched at the level that will be WRITTEN (`to` when the evolution fix
   * above will also apply, else the mon's current level), off dst_gen's own ROM
   * (gb_paste_fill_moves' CREATE resolution, decision 5). `mon` is mutated in place
   * BEFORE the modal, same reasoning as gb_paste_hook: once a slot is bad, MAKE LEGAL
   * is the only accepting choice, and CANCEL discards `mon` entirely (nothing is
   * written on any `return BANK_DOWN_REFUSED` below -- the box write further down is
   * the only writer). */
  uint8_t fill4[4] = { 0, 0, 0, 0 };
  int nfill = 0;
  if (nbad > 0) {
    uint8_t wlvl = fix ? fix_to : gb_get_level(&mon);
    /* BACKLOG #210/#212: gb_paste_fill_moves() below is the SAME cold, uncached,
     * ~185,000-read ROM scan CREATE's own s_busy_reading() masks -- see #210's fix
     * on gb_paste_hook's own call to it, applied here too since the bridge now shares
     * the same function. */
    boxoam_suspend();
    s_busy_reading();
    bool fill_no_rom = false;
    nfill = gb_paste_fill_moves(gb_get_species_dex(&mon), wlvl, &mon, bad4, fill4, &fill_no_rom);
    log_line("gen12: bridge moves: gen %u, %d bad slot(s), %d filled",
             (unsigned)dst_gen, nbad, nfill);
    /* Decision 8.7's real predicate (review D1): "the record would be WRITTEN with
     * no moves at all". `nbad == 4` misses the 1-3-bad case where every non-empty
     * slot was out of range and the fill ran dry -- that lands a Struggle-forever
     * record just as illegally. */
    int nleft = 0;
    for (int i = 0; i < 4; i++) if (gb_get_move(&mon, i)) nleft++;
    if (nleft == 0) {
      snd_deny();
      /* D9 + #390: the Gen-1 base-stats gate above already proved a ROM, so "no learnset" is true there. Gen 2 has no
       * such gate: its fill comes up empty when the ROM is simply missing, and that is what the player must be told. */
      if (GB_GEN2 == dst_gen && fill_no_rom) gb_gen12_norom_msg(GB_GEN2);
      else gb_gen12_nomoves_msg(dst_gen);
      boxoam_resume();
      return BANK_DOWN_REFUSED;
    }
    boxoam_resume();
  }

  if (fix || nbad > 0) {
    boxoam_suspend();
    GbXferChoice ch = gb_paste_legal_screen_ex(gb_get_species_dex(&mon), fix_from,
                                                fix ? fix_to : 0, from4, bad4, fill4, nbad);
    boxoam_resume();
    if (ch == GB_XFER_CANCEL) return BANK_DOWN_REFUSED;
    if (ch == GB_XFER_MAKE_LEGAL && fix) gb_set_level(&mon, fix_to);
  }

  /* BACKLOG #289 (K1): the loss-screen resume above un-suspended the glove, and suspend/resume
   * do not nest -- every dialog from here on (the box refusals, xfer_down_write's ledger
   * refusals, gb_persist's PDNA_DELTA wall / backup / write panels) drew UNDER the live OBJ.
   * One fresh suspend for the whole tail; EVERY return below resumes exactly once. */
  boxoam_suspend();
  GbsStatus wst = gbs_box_writable(&g_ed->s, dst_box);
  if (wst != GBS_OK) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_BOXWR_TITLE, UI_WARN, gbs_status_text(wst),
             wst == GBS_ERR_UNWRITABLE ? PDNA_GBEDIT_UNWRITABLE_HINT : 0);
    boxoam_resume();
    return BANK_DOWN_REFUSED;
  }
  GbsStatus lst = gbs_load_list(&g_ed->s, dst_box, g_ed->list);
  if (lst != GBS_OK) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_BOXRD_TITLE, UI_WARN, gbs_status_text(lst), 0);
    boxoam_resume();
    return BANK_DOWN_REFUSED;
  }
  int cnt = gb_list_count(g_ed->s.gen, g_ed->list, dst_box);
  if (cnt < 0 || cnt >= gb_list_capacity(g_ed->s.gen, dst_box)) {
    snd_deny();
    msg_wait(PDNA_SIDECAR_XFER_REFUSED_TITLE, UI_WARN, gbs_status_text(GBS_ERR_FULL),
             PDNA_GBEDIT_MOVE_FULL_L2);
    boxoam_resume();
    return BANK_DOWN_REFUSED;
  }

  uint8_t dv4[4] = {
    gb_get_dv(&mon, GB_ATK), gb_get_dv(&mon, GB_DEF),
    gb_get_dv(&mon, GB_SPE), gb_get_dv(&mon, GB_SPC)
  };
  uint64_t key = gbsc_key(mon.gen, gb_get_otid(&mon), dv4, mon.otname);
  /* decision 9: a copy writes NO ledger entry -- idx stays -1 and path/wlen stay
   * unused, so every guard below (undo/claim) must key on `copy`, never on `idx`
   * alone (idx == -1 is ALSO gbsc_insert's own "the ledger is full" failure shape
   * for a non-copy cell, which must still refuse the whole drop). */
  char path[GBSC_PATH_MAX];
  uint32_t wlen = 0;
  int idx = -1;
  if (!copy) {
    idx = xfer_down_write(key, cell80, &mon, XR_DIR_ABROAD_GB, NULL, g_ed->sidecar, path, &wlen);
    if (idx < 0) { boxoam_resume(); return BANK_DOWN_REFUSED; }
  }

  int newslot = -1;
  GbsStatus ist = gbs_insert(&g_ed->s, dst_box, &mon, &newslot, g_ed->list);
  if (ist != GBS_OK) {
    gb_rollback();
    log_line("gen12: down->bridge insert box %d refused: %s", dst_box, gbs_status_text(ist));
    if (!copy) xfer_down_undo(path, g_ed->sidecar);
    snd_error();
    msg_wait(PDNA_SIDECAR_XFER_REFUSED_TITLE, UI_WARN, gbs_status_text(ist), PDNA_GBEDIT_UNCHANGED_L2);
    boxoam_resume();
    return BANK_DOWN_REFUSED;
  }

  log_line("=== gen12 down->bridge -> %s box %d slot %d ===", g_ed->path, dst_box, newslot);
  bool ok = gb_persist("xferdown");
#ifdef PDNA_DELTA
  ok = true;   /* BACKLOG #292 (delta vehicle ONLY): the in-session edit stands -- see gb_paste_write's twin note */
#endif
  if (!ok) {
    if (!copy) xfer_down_undo(path, g_ed->sidecar);
    boxoam_resume();
    return BANK_DOWN_REFUSED;
  }

  if (!copy) xfer_down_claim_now(g_ed->sidecar, wlen, idx, path);           /* decision 15: CLAIMED now */
  else       log_line("gen12: down->bridge box %d slot %d: copy cell, no ledger entry", dst_box, newslot);
  boxoam_resume();
  return BANK_DOWN_LANDED;
}

/* BACKLOG #150 S150-7 decision D10: the first STORAGE box (never the party) with room,
 * for the 10(c) deposit. -1 when every box is full -- the caller refuses the WHOLE drop
 * before anything moves rather than offering a second picker. g_ed->list2 as scratch:
 * safe here because this only ever runs from gb_accept_down_party_deposit(), strictly
 * BEFORE that function's own gbs_move() call touches list2 for real. */
static int gb_first_free_box(void) {
  int nb = gbs_nboxes(&g_ed->s);
  for (int b = 0; b < nb; b++) {
    if (gbs_box_writable(&g_ed->s, b) != GBS_OK) continue;
    if (gbs_load_list(&g_ed->s, b, g_ed->list2) != GBS_OK) continue;
    int c = gb_list_count(g_ed->s.gen, g_ed->list2, b);
    if (c >= 0 && c < gb_list_capacity(g_ed->s.gen, b)) return b;
  }
  return -1;
}

/* BACKLOG #150 S150-7 decision D9: the party-full offer's own picker, a direct sibling
 * of gb_pick_box() above -- same PDNA_GBEDIT_PICKBOX_* layout constants, same
 * ui_panel/ui_hline chrome, same s_wait(KEY_UP|KEY_DOWN|KEY_A|KEY_B) loop. Rows are the
 * party members' own nicknames (gb_get_nickname, drawn with ui_ptext -- PokeDNA's own
 * proportional font, never ui_text+truncate). Returns the chosen slot, or -1 on B. */
#define GB12_PICKPARTY_MAX 6   /* retail's own party cap, both generations */

static int gb_pick_party_slot(const Gb12Mount* m) {
  int pb = m->party_box;
  if (gbs_load_list(&g_ed->s, pb, g_ed->list2) != GBS_OK) return -1;
  int n = gb_list_count(g_ed->s.gen, g_ed->list2, pb);
  if (n <= 0 || n > GB12_PICKPARTY_MAX) return -1;

  int sel = 0;
  for (;;) {
    ui_clear();
    ui_text(4, 3, UI_TITLE, PDNA_GBEDIT_PICKPARTY_TITLE);
    ui_hline(0, 13, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < n; i++) {
      GbEditMon e;
      char nm[GB_TEXT_MAX];
      if (gb_load(&e, g_ed->s.gen, g_ed->list2, pb, i)) gb_get_nickname(&e, nm, sizeof nm);
      else nm[0] = 0;
      int y = PDNA_GBEDIT_PICKBOX_Y0 + i * PDNA_GBEDIT_PICKBOX_ROW_H;
      bool sh = (i == sel);
      if (sh) ui_panel(2, y - 1, UI_SCR_W - 4, PDNA_GBEDIT_PICKBOX_ROW_H, UI_SEL, UI_TITLE);
      ui_ptext(4, y, sh ? UI_SELTEXT : UI_TEXT, nm[0] ? nm : "?");
    }
    ui_hline(0, 147, UI_SCR_W, UI_BORDER);
    ui_text(4, 150, UI_DIM, PDNA_GBEDIT_PICKPARTY_FOOT);

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return -1;
    if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    if (k & KEY_DOWN) sel = (sel + 1) % n;
    if (k & KEY_A) return sel;
  }
}

/* BACKLOG #150 S150-7 SS11.20 item 10(c): the party is full -- offer to send one party
 * member to a box first rather than refusing the drop outright. Confirm decline, B on
 * the picker, or no free box all refuse the WHOLE drop with nothing touched; the actual
 * deposit is gbs_move(), RAM-only (D5 -- the caller's ONE gb_persist runs afterwards). */
static bool gb_accept_down_party_deposit(const Gb12Mount* m) {
  char l1[64];
  siprintf(l1, "%s %s", PDNA_XFER_PARTYFULL_L1, PDNA_XFER_PARTYFULL_L2);
  if (!app_confirm(PDNA_XFER_PARTYFULL_TITLE, l1)) return false;

  int chosen = gb_pick_party_slot(m);
  if (chosen < 0) return false;

  int dep = gb_first_free_box();
  if (dep < 0) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_MOVE_REFUSED_TITLE, UI_WARN, PDNA_XFER_DOWN_NOROOM_L1, 0);
    return false;
  }

  int pb = gbs_party_box(&g_ed->s);
  int to_slot = -1;
  GbsStatus st = gbs_move(&g_ed->s, pb, chosen, dep, &to_slot, g_ed->list, g_ed->list2);
  if (st != GBS_OK) {
    gb_rollback();
    log_line("gen12: bank-down party deposit box %d slot %d -> box %d refused: %s",
             pb, chosen, dep, gbs_status_text(st));
    snd_error();
    const char* hint = 0;
    switch (st) {
      case GBS_ERR_NEEDS_BASE:  hint = PDNA_GBEDIT_MOVE_NEEDSBASE_L2; break;
      case GBS_ERR_PARTY_FLOOR: hint = PDNA_GBEDIT_MOVE_FLOOR_L2;     break;
      case GBS_ERR_MAIL:        hint = PDNA_GBEDIT_MOVE_MAIL_L2;      break;
      case GBS_ERR_FULL:        hint = PDNA_GBEDIT_MOVE_FULL_L2;      break;
      case GBS_ERR_UNWRITABLE:  hint = PDNA_GBEDIT_UNWRITABLE_HINT;   break;
      default: break;
    }
    msg_wait(PDNA_GBEDIT_MOVE_REFUSED_TITLE, UI_WARN, gbs_status_text(st), hint);
    return false;
  }
  return true;
}

/* BACKLOG #150 S150-7 decisions D3/D8/D11: land a NATIVE "GBC1" Bank cell in THIS Game
 * Boy save -- the EXACT arm bank_down_dispatch() (pdna_box.c) calls through
 * BoxXferOps.accept_down. Mirrors gb_paste_hook's own pre-flight ordering (above): every
 * refusal leaves nothing past it touched, and after the ONE confirm line nothing may
 * refuse for a reason the pre-flight list could already have seen.
 * PRECONDITION, guaranteed by bank_down_dispatch: bc_is_native(cell80) and the cell's
 * gen equals this session's gen (xg_bank_down_arm's EXACT row) -- both re-checked here
 * anyway (belt and braces; a bare `bl` through the vtable cannot itself enforce them). */
static bool __attribute__((noinline)) gb_accept_down_hook(int dst_box, const uint8_t cell80[80]) {
  /* D8: gb_locate()'s own gates 1/2, run directly -- a native cell has no rec80 address
   * inside the mount's paged box buffer for gb_locate_addr() to resolve. */
  if (!g_ed) return false;
  if (!app_can_edit()) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_READONLY_TITLE, UI_WARN, app_gb_readonly_why(), 0);
    return false;
  }
  GbsStatus wr = gbs_box_writable(&g_ed->s, dst_box);
  if (wr != GBS_OK) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_BOXWR_TITLE, UI_WARN, gbs_status_text(wr),
             wr == GBS_ERR_UNWRITABLE ? PDNA_GBEDIT_UNWRITABLE_HINT : 0);
    return false;
  }

  GbEditMon mon; BcMeta meta;
  if (!bc_unpack(cell80, &mon, &meta)) { snd_deny(); return false; }
  if (mon.gen != g_ed->s.gen) { snd_deny(); return false; }   /* xg_bank_down_arm's own precondition */

  const bool to_party = (dst_box == gbs_party_box(&g_ed->s));
  GbGen1Base g1base;
  bool have_g1base = false;
  uint16_t dex = 0;
  if (to_party && g_ed->s.gen == GB_GEN1) {
    dex = gb_get_species_dex(&mon);
    Gb1BaseStatus bst = dex ? gb_gen1_base_from_rom(dex, &g1base) : GB1BASE_BAD_ROM;
    if (bst == GB1BASE_NO_ROM) { snd_deny(); gb_gen12_norom_msg(GB_GEN1); return false; }
    if (bst != GB1BASE_OK) {
      snd_deny();
      msg_wait(PDNA_GBEDIT_MOVE_REFUSED_TITLE, UI_WARN, PDNA_GBEDIT_MOVE_NEEDSBASE_L2, 0);
      return false;
    }
    have_g1base = true;
  }

  /* Capacity, and the 10(c) party-full offer -- strictly before any byte moves. */
  if (gbs_load_list(&g_ed->s, dst_box, g_ed->list) != GBS_OK) { snd_deny(); return false; }
  int count = gb_list_count(g_ed->s.gen, g_ed->list, dst_box);
  int cap = gb_list_capacity(g_ed->s.gen, dst_box);
  if (count < 0) { snd_deny(); return false; }
  if (count >= cap) {
    if (!to_party) {
      snd_deny();
      msg_wait(PDNA_GBEDIT_MOVE_REFUSED_TITLE, UI_WARN, PDNA_GBEDIT_MOVE_FULL_L2, 0);
      return false;
    }
    if (!g_m || !gb_accept_down_party_deposit(g_m)) return false;
    /* REVIEW F1: the 10(c) deposit above already committed the moved party member into
     * a box in the RESIDENT image (gbs_move -> gbs_commit_list) -- an un-consented
     * change to the user's data if this function returns false past this point without
     * undoing it (the next gb_persist() anywhere, even a plain box rename, would write
     * it to the card). gb_rollback() over an image this reload call left untouched is
     * the documented no-op (gb_session.h), so it is safe here regardless of which
     * branch below actually needed it. */
    if (gbs_load_list(&g_ed->s, dst_box, g_ed->list) != GBS_OK) {
      gb_rollback(); snd_deny(); return false;
    }
  }

  /* The ONE confirm line (D-Q2/D-Q3), REVIEW F6: the party landing now names the mon
   * on its own first line and keeps the per-generation stats note as a second line --
   * UX parity with the box-destination confirm (which has always named the mon) and
   * with the Gen-3 twin's own two-line confirm shape. app_confirm() takes a single
   * string and wraps it itself (ui_ptext_wrap, max 2 lines) at word boundaries, so the
   * name is padded with spaces to fill the panel's own 184px width before the stats
   * note is appended -- the padding-width space is always the wrap point (any word
   * from the stats note would overflow before a shorter one), landing the name alone
   * on line 1 without a second, separately-addressed line parameter. */
  char nm[GB_TEXT_MAX];
  gb_get_nickname(&mon, nm, sizeof nm);
  char cl1[PDNA_XFER_DOWN_CL1_SZ];  /* name (<=20 cols, up to 33 B with UTF-8 chars like ♀) +
                                      * padding spaces (until pwidth >= PDNA_XFER_DOWN_PAD_PX
                                      * force-break) + the longest per-gen stat line.
                                      * Worst case: 11 × ♀ (GB_NAME_BYTES = 11) = 33 B /
                                      * 66 px + 38 spaces (to reach 180px) + 30 B
                                      * "Lv 100 from EXP (box said 100)" (Gen-1 exp) = 101 B.
                                      * room = 128 - 101 - 1 = 26 bytes guaranteed minimum.
                                      * Verified: host_textfit_test.c #178. */
  ui_truncate(cl1, nm[0] ? nm : PDNA_GBEDIT_RELEASE_FALLBACK, 20);
  if (to_party) {
    char stat_line[48];
    if (g_ed->s.gen == GB_GEN1) {
      uint8_t exp_level = gb_level_from_exp(dex, gb_get_exp(&mon));
      uint8_t box_level = mon.rec[G1R_BOXLEVEL];
      if (exp_level && exp_level != box_level)
        siprintf(stat_line, "Lv %u from EXP (box said %u)", (unsigned)exp_level, (unsigned)box_level);
      else
        siprintf(stat_line, "%s", PDNA_XFER_DOWN_PARTYFOOT_G1);
    } else {
      siprintf(stat_line, "%s", PDNA_XFER_DOWN_PARTYFOOT_G2);
    }
    int n = 0;
    while (cl1[n]) n++;                                     /* end of the name */
    while (n < (int)sizeof cl1 - 2 && ui_ptext_w(cl1) < PDNA_XFER_DOWN_PAD_PX) { cl1[n++] = ' '; cl1[n] = 0; }
    int room = (int)sizeof cl1 - n - 1;
    if (room > 0) {
      int k = 0;
      while (stat_line[k] && k < room) { cl1[n + k] = stat_line[k]; k++; }
      cl1[n + k] = 0;
    }
  }
  /* REVIEW F1: same reasoning as the reload refusal above -- a DECLINED confirm must
   * not leave the 10(c) deposit's own party->box move sitting committed in the
   * resident image. gb_rollback() is a no-op when the party-full branch never ran
   * (nothing to undo), so this is unconditional, not gated on `to_party`. */
  if (!app_confirm(PDNA_XFER_DOWN_CONFIRM_TITLE, cl1)) { gb_rollback(); return false; }

  int slot = -1;
  GbsStatus ist = to_party
    ? gbs_insert_party(&g_ed->s, &mon, have_g1base ? &g1base : NULL, &slot, g_ed->list)
    : gbs_insert(&g_ed->s, dst_box, &mon, &slot, g_ed->list);
  if (ist != GBS_OK) {
    gb_rollback();
    log_line("gen12: bank-down -> box %d refused: %s", dst_box, gbs_status_text(ist));
    snd_error();
    const char* hint = 0;
    switch (ist) {
      case GBS_ERR_NEEDS_BASE:  hint = PDNA_GBEDIT_MOVE_NEEDSBASE_L2; break;
      case GBS_ERR_PARTY_FLOOR: hint = PDNA_GBEDIT_MOVE_FLOOR_L2;     break;
      case GBS_ERR_MAIL:        hint = PDNA_GBEDIT_MOVE_MAIL_L2;      break;
      case GBS_ERR_FULL:        hint = PDNA_GBEDIT_MOVE_FULL_L2;      break;
      case GBS_ERR_UNWRITABLE:  hint = PDNA_GBEDIT_UNWRITABLE_HINT;   break;
      default: break;
    }
    const char* l1 = (ist == GBS_ERR_FULL && to_party)
                    ? PDNA_GBEDIT_MOVE_PARTYFULL_L1 : gbs_status_text(ist);
    msg_wait(PDNA_GBEDIT_MOVE_REFUSED_TITLE, UI_WARN, l1, hint);
    return false;
  }

  log_line("=== gb bank-down -> %s box %d slot %d ===", g_ed->path, dst_box, slot);
#ifdef PDNA_DELTA
  /* BACKLOG #292 (delta vehicle ONLY): gb_persist's emulator branch KEEPS the edit in-session and returns false
   * only to say "no card". bank_down_exact / bank_down_g3_run read false as "refused, still holding" and left a
   * stale grid + a live carry whose second A duplicated the mon. The edit landed in the resident image, so report
   * it landed: the caller consumes the Bank slot and repaints from the image. The shipped build below is
   * unchanged (every non-DELTA failure already rolled the image back). */
  (void)gb_persist("bank-down");
  return true;
#else
  return gb_persist("bank-down");     /* the ONE card write; it reports its own refusals */
#endif
}

/* #286/y12: the TO GAME menu action's destination -- the first STORAGE box of the mounted resident Game Boy save that
 * can take one more mon, scanning from the box on screen (resumes at the box on screen, like the Gen-3 twin's
 * g_pc_last_box start; a drop lands where the cursor is, the menu has no cursor there). *out_unwritable (optional)
 * counts the non-party boxes skipped as UNWRITABLE, so a caller can tell "full" from "change box in-game". The party pseudo-box is never
 * chosen (a menu action must not silently fill the party). "Has room" means exactly what accept_down's own
 * pre-flight tests (so the pick can never send a mon somewhere the hook would refuse for a reason THIS function could
 * have seen): gbs_box_writable() == GBS_OK AND gb_list_count() trusted (>= 0) AND count < gb_list_capacity(). Writes
 * the 0-based slot the mon will occupy into *out_slot (a Game Boy list always appends at its own count). Read-only:
 * it stages lists into the session's scratch (g_ed->list, exactly as accept_down does) and writes nothing. Returns -1
 * on no session / no room. */
int gb_togame_pick_box(int* out_slot, int* out_unwritable) {
  if (!g_ed || !out_slot) return -1;
  const int party = gbs_party_box(&g_ed->s);
  const int n = gbs_nboxes(&g_ed->s);
  const int start = (g_m && g_m->ui_box >= 0 && g_m->ui_box < n) ? g_m->ui_box : 0;
  for (int i = 0; i < n; i++) {
    const int b = (start + i) % n;
    if (b == party) continue;
    if (gbs_box_writable(&g_ed->s, b) != GBS_OK) { if (out_unwritable) (*out_unwritable)++; continue; }
    if (gbs_load_list(&g_ed->s, b, g_ed->list) != GBS_OK) continue;
    const int count = gb_list_count(g_ed->s.gen, g_ed->list, b);
    if (count >= 0 && count < gb_list_capacity(g_ed->s.gen, b)) { *out_slot = count; return b; }
  }
  return -1;
}

/* BACKLOG #280 (Guy's #270 ruling, the bridge half): the marking step of gb_bridge_restore_up --
 * flip the NATIVE_HOME entry to RESTORED, STRICTLY AFTER the restored cell has landed and
 * persisted (the old release-side marking, gb_release_up_hook, relocated with the restore).
 * gbsc_set_state flips only the state bits in place, so bank_keep survives (#215(c)). The entry is
 * re-resolved from the card by the same key + tiebreak as the lookup (gb_persist may have reused
 * the sidecar buffer). Best-effort like every sibling: the card already has the right bytes, so a
 * bookkeeping failure only logs -- and a surviving unmarked entry is a residual duplicate risk,
 * never a loss. */
static void __attribute__((noinline))
gb_bridge_mark_restored(const GbEditMon* now) {
  uint8_t dv4[4] = {
    gb_get_dv(now, GB_ATK), gb_get_dv(now, GB_DEF), gb_get_dv(now, GB_SPE), gb_get_dv(now, GB_SPC)
  };
  uint64_t key = gbsc_key(now->gen, gb_get_otid(now), dv4, now->otname);
  uint32_t len = 0;
  char path[GBSC_PATH_MAX];
  if (xr_open(key, g_ed->sidecar, GBSC_FILE_MAX, &len, path) != SF_OK || gbsc_count(g_ed->sidecar, len) < 0) {
    log_line("gen12: bridge restore: mark: ledger re-open failed"); return;
  }
  int idx = xr_resolve_home(g_ed->sidecar, len, now, XR_KIND_NATIVE_HOME, gb_get_species_dex(now));
  if (idx < 0 || gbsc_set_state(g_ed->sidecar, len, idx, XR_STATE_RESTORED) != 0) {
    log_line("gen12: bridge restore: mark: entry vanished / set_state failed"); return;
  }
  rmbl_pause();
  SfStatus wst = sf_write_verified(path, g_ed->sidecar, len);
  rmbl_resume();
  if (wst != SF_OK) log_line("gen12: bridge restore: mark: rewrite failed for %s", path);
  else              app_xv_cache_invalidate();   /* BACKLOG #213: a real ledger write */
}

/* BACKLOG #280 (Guy's #270 ruling, the bridge half): the GB-BRIDGE-TARGET restore. The lift is a
 * pass-through now, so a Gen-2 mon that was bridged down from a Gen-1 original (ledger entry
 * XR_KIND_NATIVE_HOME / XR_DIR_ABROAD_GB, written by gb_bank_down_bridge) reaches the Bank as its
 * own native cell. Dropped on a Game Boy PC of the OTHER generation -- the entry's HOME generation
 * -- this hands back the original native cell instead of a fresh lossy conversion. It is the
 * relocated core of the old lift-time gb_lift_restore native arm: the same probe
 * (xr_merge_down_gb_sel), the same merge screen, the same bank_restore_from_entry_gb, the same
 * ALREADY-RESTORED / PENDING behaviour as gbpc_restore_up -- and the restored cell then lands
 * through gb_accept_down_hook, exactly like the EXACT drop. The entry keeps its RESTORED lifecycle
 * (gb_bridge_mark_restored, after the landing). Called only from bank_down_convert_gb.
 *
 * OAM: brackets its own screens (and the landing) like gb_bank_down_bridge -- its caller must not.
 * Returns 1 = restored and landed (persisted; the caller consumes the Bank cell), 0 = convert
 * normally (no entry / a COPY cell / an entry of another kind, direction or home generation / an
 * entry already RESTORED -- the DUPLICATE, never restored twice), -2 = refused or declined,
 * already said on screen. */
int __attribute__((noinline))
gb_bridge_restore_up(int dst_box, const uint8_t cell80[80]) {
  if (!g_ed || !cell80) return 0;
  if (!app_can_edit()) return 0;
  if (!bc_is_native(cell80) || xg_cell_is_copy(cell80)) return 0;   /* a COPY's original still lives: restoring = clone */

  GbEditMon mon; BcMeta meta;
  if (!bc_unpack(cell80, &mon, &meta)) return 0;
  uint8_t dv4[4] = {
    gb_get_dv(&mon, GB_ATK), gb_get_dv(&mon, GB_DEF), gb_get_dv(&mon, GB_SPE), gb_get_dv(&mon, GB_SPC)
  };
  uint64_t key = gbsc_key(mon.gen, gb_get_otid(&mon), dv4, mon.otname);

  uint32_t len = 0;
  SfStatus rst = xr_open(key, g_ed->sidecar, GBSC_FILE_MAX, &len, NULL);
  if (rst == SF_ERR_OPEN) return 0;                 /* no ledger entry -- an ordinary native cell */
  if (rst != SF_OK || gbsc_count(g_ed->sidecar, len) < 0) {
    log_line("gen12: bridge restore: ledger open/validate failed (%s)", sf_status_str(rst));
    boxoam_suspend(); snd_error();
    msg_wait(PDNA_XFERREC_TITLE, UI_WARN, PDNA_XFERREC_L1, PDNA_XFERREC_L2);
    boxoam_resume();
    return -2;
  }
  int found = xr_resolve_home(g_ed->sidecar, len, &mon, XR_KIND_NATIVE_HOME, gb_get_species_dex(&mon));
  if (found < 0) return 0;                          /* no NATIVE_HOME entry for this mon */
  GbscEntry e;
  if (!gbsc_get(g_ed->sidecar, len, found, &e) || e.direction != XR_DIR_ABROAD_GB) return 0;
  GbEditMon home; BcMeta hmeta;
  if (!bc_is_native(e.original80) || !bc_unpack(e.original80, &home, &hmeta) ||
      home.gen != g_ed->s.gen) return 0;            /* only an original of THIS save's generation comes home */

  if (e.state == XR_STATE_RESTORED) {               /* the duplicate of an original that already came home */
    log_line("gen12: bridge restore: entry already RESTORED -- this drop converts instead");
    return 0;
  }
  if (e.state == XR_STATE_PENDING) {                /* the honest SAVE FIRST wall (relocated from the lift) */
    log_line("gen12: bridge restore: entry still PENDING -- refusing (SAVE FIRST wall)");
    boxoam_suspend(); snd_deny();
    msg_wait(PDNA_XFER_SAVEFIRST_TITLE, UI_WARN, PDNA_XFER_SAVEFIRST_L1, PDNA_XFER_SAVEFIRST_L2);
    boxoam_resume();
    return -2;
  }

  GbEditMon probe; XrMergeReport rep;
  if (!xr_merge_down_gb_sel(&e, &mon, 0, &probe, &rep)) {
    log_line("gen12: bridge restore: xr_merge_down_gb_sel probe failed");
    boxoam_suspend(); snd_error();
    msg_wait(PDNA_XFERREC_TITLE, UI_WARN, PDNA_XFERREC_L1, PDNA_XFERREC_L2);
    boxoam_resume();
    return -2;
  }
  uint8_t accept = 0;
  boxoam_suspend();
  bool confirmed = app_xfer_merge_screen(&rep, XR_MERGE_DOWN, &accept);
  boxoam_resume();
  if (!confirmed) return -2;                        /* B: nothing spent, nothing written */

  uint32_t serial = pdna_bank_next_serial();
  uint8_t restored[80];
  if (serial == 0 || bank_restore_from_entry_gb(&e, &mon, accept, serial, restored, NULL) != 1) {
    log_line("gen12: bridge restore: serial/rebuild failed");
    boxoam_suspend(); snd_error();
    msg_wait(PDNA_XFERREC_TITLE, UI_WARN, PDNA_XFERREC_L1, PDNA_XFERREC_L2);
    boxoam_resume();
    return -2;
  }

  boxoam_suspend();
  bool landed = gb_accept_down_hook(dst_box, restored);   /* asks its own confirm; persists */
  boxoam_resume();
  if (!landed) return -2;                           /* the hook already said why; still holding */
  gb_bridge_mark_restored(&mon);                    /* AFTER the landed check */
  return 1;
}

/* BACKLOG #246 (#104 Phase 1): the missing arm -- a PLAIN Gen-3 Bank cell (never
 * native "GBC1") landing in THIS Game Boy save for the FIRST time. Called from
 * pdna_box.c's drop_held, its own new cross-scope branch for the pair xg_drop_denied
 * now allows (GB<-BANK) -- the caller already knows the destination box (a Bank
 * drop, unlike the old menu-driven PASTE, is never addressed via gb_locate()) and
 * hands the held Bank cell in directly as `cell80`, so every place this body used
 * to read app_clip_rec() (the clipboard) now reads `cell80` instead. Body and order
 * are gb_paste_hook's own (deleted in this same commit, along with its PASTE row --
 * two live routes into a Game Boy save must never drift apart), with two deliberate
 * departures from that shape, matching this call site's siblings
 * (gb_bank_down_bridge/gb_bank_down_gen3, above -- the SAME caller, drop_held's
 * cross-scope dispatch): (1) `dst_box` arrives as a parameter, no gb_locate(); (2)
 * BACKLOG #246 review D2 fix: this function opens NO boxoam_suspend()/resume() of
 * its own around its screens -- its ONE caller, drop_held_down_g3 (pdna_box.c),
 * already brackets this entire call, and boxoam_suspend/resume are bare
 * REG_DISPCNT toggles with no depth counter (source/box_oam.c), so a second,
 * inner pair does not "help" the box-grid OBJ sprites stay off -- it turns them
 * back ON the moment its own resume() runs, for the REST of this function's own
 * body, bleeding the carry glove and the carried mon through every screen/dialog
 * drawn after that point. (An earlier revision of this comment claimed the
 * opposite -- that this function needed its own bracket because a Bank-drop's OBJ
 * sprites are live the whole time; true of the SYMPTOM, wrong about the fix: the
 * caller's own bracket already covers "the whole time", and nesting a second one
 * is what broke it.) The party refusal (S5-B re-verification NEW-1's own reasoning:
 * gbs_insert() only ever inserts BOX-kind records into storage boxes, never the
 * party -- landing there needs gbs_move()'s species-limit/live-stat/Mail rules this
 * function does not have) is KEPT, first, unchanged.
 * Returns BANK_DOWN_LANDED on a verified card write, BANK_DOWN_REFUSED otherwise
 * (a message has already been shown on every refusal path; nothing is written on
 * any BANK_DOWN_REFUSED return -- gb_paste_write() is the only writer, at the very
 * end). Never returns BANK_DOWN_CONVERTED -- this arm lands directly in the
 * destination, exactly like gb_bank_down_bridge, never through the Gen-3-PC
 * fall-through gb_bank_down_gen3 uses. */
BankDownResult gb_bank_down_g3(int dst_box, const uint8_t cell80[80]) {
  if (!app_can_edit() || !g_ed) { snd_deny(); return BANK_DOWN_REFUSED; }

  /* S5-B re-verification NEW-1 (must), kept verbatim from gb_paste_hook: the party
   * pseudo-box IS a grid box (nboxes == party_box + 1) -- without this guard
   * gb_paste_write() would write the sidecar to the CARD and only THEN have
   * gbs_insert() refuse with GBS_ERR_ARG, orphaning a sidecar entry on every single
   * attempt rather than only on a genuine race. Refused here, before ANYTHING
   * (including the loss screen) runs. */
  if (gb_box_is_party(g_ed->s.gen, dst_box)) {
    snd_deny();
    msg_wait(PDNA_SIDECAR_XFER_TITLE, UI_WARN, PDNA_SIDECAR_PARTY_L1, 0);
    return BANK_DOWN_REFUSED;
  }

  /* BACKLOG #246 review D6: hoisted from just before gb_paste_write() (inherited
   * from gb_paste_hook, where the destination box was picked in a MENU after the
   * loss/legal screens already ran) -- a Bank drop knows dst_box at entry, so
   * writable/list/capacity are checked HERE, before the loss screen, not after. The
   * pre-fix order let the user confirm "A = transfer" on the loss screen and only
   * THEN be told the box was unwritable/unreadable/full -- a confirmed choice that
   * silently turned out to mean nothing. */
  GbsStatus wst = gbs_box_writable(&g_ed->s, dst_box);                      /* 5 */
  if (wst != GBS_OK) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_BOXWR_TITLE, UI_WARN, gbs_status_text(wst),
             wst == GBS_ERR_UNWRITABLE ? PDNA_GBEDIT_UNWRITABLE_HINT : 0);
    return BANK_DOWN_REFUSED;
  }

  /* S5-B review fix (BLOCKING #1), kept verbatim: the grid shows 30 cells but a GB
   * box holds at most gb_list_capacity() (20 for Gen 2) -- cells 20..29 always read
   * empty on this source, so without this check a drop attempted there would write
   * the sidecar to the card FIRST and only then have gbs_insert() refuse with
   * GBS_ERR_FULL inside gb_paste_write(), leaving an orphan sidecar entry behind on
   * EVERY such attempt rather than only on a genuine race. `g_ed->list` is reloaded
   * a moment later by gb_paste_write()'s own gbs_insert() -- cheap, and every other
   * hook in this file re-derives its own gates fresh the same way. */
  GbsStatus lst = gbs_load_list(&g_ed->s, dst_box, g_ed->list);
  if (lst != GBS_OK) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_BOXRD_TITLE, UI_WARN, gbs_status_text(lst), 0);
    return BANK_DOWN_REFUSED;
  }
  int cnt = gb_list_count(g_ed->s.gen, g_ed->list, dst_box);
  if (cnt < 0) {
    snd_deny();
    msg_wait(PDNA_SIDECAR_XFER_REFUSED_TITLE, UI_WARN, gbs_status_text(GBS_ERR_STRUCT), 0);
    return BANK_DOWN_REFUSED;
  }
  if (cnt >= gb_list_capacity(g_ed->s.gen, dst_box)) {
    snd_deny();
    msg_wait(PDNA_SIDECAR_XFER_REFUSED_TITLE, UI_WARN, gbs_status_text(GBS_ERR_FULL),
             PDNA_GBEDIT_MOVE_FULL_L2);
    return BANK_DOWN_REFUSED;
  }

  GbEditMon mon;
  Gen3ToGbLoss loss;
  /* BACKLOG #95 review C11: the capture record at 0x1D/0x1E is Crystal-only real data;
   * gb_session_is_crystal() is the single source of truth this and gb_mark_caught both
   * call, so the live editor and this synthesis path cannot disagree about which target
   * saves get a synthetic Met record. */
  bool crystal = gb_session_is_crystal(&g_ed->s);

  /* BACKLOG #150 S150-10 decision 8, step 2: bad move slots are found BEFORE the
   * conversion runs, so gen3_to_gb_fixed can empty them instead of the whole record
   * being refused with G3GB_ERR_MOVE (G-H8). nbad < 0 (decode failure/Egg) passes
   * bad4 = NULL through, so gen3_to_gb_fixed refuses exactly the way gen3_to_gb()
   * always has -- and indeed IS, decision 1. */
  uint16_t from4[4] = { 0, 0, 0, 0 };
  uint8_t  bad4[4]  = { 0, 0, 0, 0 };
  int nbad = gb_rec_moves(cell80, g_ed->s.gen, from4, bad4);
  const uint8_t* bad4p = (nbad > 0) ? bad4 : NULL;

  G3GbStatus cst = gen3_to_gb_fixed(cell80, g_ed->s.gen, crystal, NULL,
                                     bad4p, &mon, &loss);                   /* 2 */
  if (cst == G3GB_ERR_NEEDS_BASE) {          /* Gen 1 only -- everything else about this
                                              * mon already checked out (gen3_to_gb.c's
                                              * screen() reaches this check LAST) */
    uint16_t dex = gb_rec_dex(cell80);
    GbGen1Base g1base;
    Gb1BaseStatus bst = dex ? gb_gen1_base_from_rom(dex, &g1base) : GB1BASE_BAD_ROM;
    if (bst == GB1BASE_NO_ROM) {
      snd_deny();
      gb_gen12_norom_msg(GB_GEN1);
      return BANK_DOWN_REFUSED;
    }
    if (bst != GB1BASE_OK) {
      snd_deny();
      msg_wait(PDNA_SIDECAR_XFER_TITLE, UI_WARN, PDNA_SIDECAR_GEN1_BADROM_L1, 0);
      return BANK_DOWN_REFUSED;
    }
    cst = gen3_to_gb_fixed(cell80, g_ed->s.gen, crystal, &g1base,
                            bad4p, &mon, &loss);                            /* 3 */
  }
  if (cst != G3GB_OK) {
    snd_deny();
    msg_wait(PDNA_SIDECAR_XFER_TITLE, UI_WARN, g3gb_status_text(cst), 0);
    return BANK_DOWN_REFUSED;
  }

  /* BACKLOG #249 cases B/C/D/E: gen3_to_gb_fixed() (a pure module with no session)
   * can only ever leave loss.item_outcome at NONE/HELD/STAYS -- for a Gen-1 target
   * with an item that STAYS, this is the ONE place that knows both the destination
   * save is OPEN right now (g_ed->s) and which item wants a home, so the ladder is
   * decided here, once, off a live bag read -- gb_paste_write() re-checks for real
   * at commit time (a decision can go stale between here and the A=transfer press),
   * this is only what the loss screen below shows the user. */
  uint8_t g1_item = 0;
  if (g_ed->s.gen == GB_GEN1 && loss.item_outcome == G3GB_ITEM_STAYS && loss.g3_held_item != 0) {
    GbBag bag;
    if (gbb_read(&g_ed->s, &bag)) {
      /* F2 fix: g3gb_item_ladder()'s own room test is `items_count < items_cap`, which
       * knows nothing about stack depth -- gbb_insert() MERGES, so a pocket at its
       * entry cap can still accept a matching stack below 99, and a pocket with free
       * slots refuses a stack already at 99. gbb_has_room_for() is the real predicate;
       * feed it in as an EFFECTIVE count/cap pair (0 of cap = room, cap of cap = full)
       * so g3gb_item_ladder's own signature and every test against it stay untouched. */
      uint8_t want = g3gb_item_to_gb1(loss.g3_held_item);
      int icap = gbb_pocket_cap(GBF_G_RED, GBB_POCKET_ITEMS);
      int pcap = gbb_pocket_cap(GBF_G_RED, GBB_POCKET_PC);
      loss.item_outcome = g3gb_item_ladder(
          GB_GEN1, loss.g3_held_item,
          gbb_has_room_for(GBF_G_RED, &bag, GBB_POCKET_ITEMS, want) ? 0 : icap, icap,
          gbb_has_room_for(GBF_G_RED, &bag, GBB_POCKET_PC,    want) ? 0 : pcap, pcap,
          &g1_item);
    }
  }

  /* LOSS_FOOT_BRIDGE, not LOSS_FOOT_PASTE: the KEPT rows ("Kept in /PokeDNA/xfer;
   * restored when it comes back.") stay -- the sidecar entry IS written, same as
   * PASTE always did -- but the footer's third line must say the Bank slot EMPTIES,
   * not PDNA_SIDECAR_LOSS_STAYS's stale "the copy in your Gen-3 save stays" (that
   * was true of the OLD clipboard-copy PASTE route; dropping a Bank cell is a MOVE,
   * consumed on landing, docs/104-ROUNDTRIP-DESIGN.md section 2.1's "the cell is
   * consumed" -- the caller, drop_held's new branch, deletes it right after this
   * call returns LANDED). PDNA_XFER_BRIDGE_STAYS's own wording ("The Bank slot is
   * emptied when it lands.") is generic, not bridge-specific, and applies verbatim.
   * BACKLOG #246 review D2: NO boxoam_suspend()/resume() bracket here -- the caller
   * (drop_held_down_g3, pdna_box.c) already brackets this ENTIRE call, and
   * boxoam_suspend/resume are bare REG_DISPCNT toggles with no depth counter
   * (source/box_oam.c) -- an inner resume() here would turn OBJ back ON while the
   * outer bracket is still logically "suspended", bleeding the carry glove and the
   * carried mon through every dialog/screen this function draws AFTER this point
   * (the review's own repro: the first inner resume left OBJ on for the rest of the
   * call, including later refusal dialogs). */
  bool loss_ok = gb_paste_loss_screen(&loss, LOSS_FOOT_BRIDGE);             /* 4 */
  if (!loss_ok) return BANK_DOWN_REFUSED;

  /* BACKLOG #104 R1 (docs/104-ROUNDTRIP-DESIGN.md section 3c/4): KEEP AS IS vs
   * MAKE LEGAL, additive between the loss screen and the box-writable check -- most
   * transfers never trigger gen3_to_gb_evo_needs_fix() and this whole block is a
   * no-op. MAKE LEGAL's one correction is the level; gb_set_level() (gb_edit.h,
   * already shipped) also recomputes EXP under the target generation's own growth
   * rate, so level and EXP stay consistent. `mon` is corrected HERE, before
   * gb_paste_write() runs, so its own gbsc_entry_from() call (unchanged) captures
   * the CORRECTED level as written_level while original80 (`cell80`, also
   * unchanged) stays the true, uncorrected Gen-3 original -- see gb_sidecar.c's
   * merge_species_and_level() for why that distinction matters. */
  uint8_t fix_from = 0, fix_to = 0;
  bool fix = gen3_to_gb_evo_needs_fix(&mon, &fix_from, &fix_to);

  /* BACKLOG #150 S150-10 decision 8, step 7: nbad > 0 -- fill the bad slots from the
   * destination ROM's learnset AT THE LEVEL THAT WILL BE WRITTEN (decision 5): `to`
   * when the evolution fix above will be applied (MAKE LEGAL is the only accepting
   * choice once a slot is bad, decision 7), else the mon's current level. Writes the
   * fills into `mon` HERE, before the modal, because a MAKE LEGAL choice adds nothing
   * further for moves and a CANCEL discards `mon` entirely (nothing is written on any
   * BANK_DOWN_REFUSED return below -- gb_paste_write is the only writer, at the very
   * end). */
  uint8_t fill4[4] = { 0, 0, 0, 0 };
  int nfill = 0;
  if (nbad > 0) {
    uint8_t wlvl = fix ? fix_to : gb_get_level(&mon);
    /* BACKLOG #210: gb_paste_fill_moves() below calls gb_create_locate_rom() (decision
     * 5's own choice, deliberately bypassing romgs_ready -- see that function's MEDIUM-1
     * comment: CREATE's resolution is ALWAYS a cold, ~185,000-read full-ROM scan, never
     * cached). CREATE masks that exact scan with s_busy_reading() (see gb_create_hook's
     * own MEDIUM-2 comment) before it ever calls gb_create_locate_rom.
     * BACKLOG #246 review D2: no boxoam_suspend/resume bracket -- see the loss-screen
     * comment above (drop_held_down_g3's own outer bracket already covers this). */
    s_busy_reading();
    bool fill_no_rom = false;
    nfill = gb_paste_fill_moves(gb_get_species_dex(&mon), wlvl, &mon, bad4, fill4, &fill_no_rom);
    /* "packed": did any KEPT (non-bad) slot's move end up at a different index than
     * it started at? g3gb_moves_fill's own contract writes a fill AT its bad slot's
     * own index and never touches a kept slot's value -- so a kept slot's move can
     * only have moved if g3gb_moves_pack() (called last, inside the fill) shifted it
     * to close a hole. Cheap and correct without a new return value from that
     * shipped, already-tested function. */
    bool packed = false;
    for (int i = 0; i < 4; i++)
      if (!bad4[i] && from4[i] != 0 && gb_get_move(&mon, i) != (uint8_t)from4[i]) packed = true;
    log_line("gen12: bank-down->g3 moves: gen %u, %d bad slot(s), %d filled, packed=%d",
             (unsigned)g_ed->s.gen, nbad, nfill, (int)packed);
    /* Decision 8.7's real predicate (review D1): "the record would be WRITTEN with
     * no moves at all". `nbad == 4` misses the 1-3-bad case where every non-empty
     * slot was out of range and the fill ran dry -- that lands a Struggle-forever
     * record just as illegally. Refused here, nothing written; the modal below is
     * never shown. */
    int nleft = 0;
    for (int i = 0; i < 4; i++) if (gb_get_move(&mon, i)) nleft++;
    if (nleft == 0) {
      snd_deny();
      /* #390: same wording rule as the bridge above -- a Gen-2 session with no ROM must say so. */
      if (g_ed->s.gen == GB_GEN2 && fill_no_rom) gb_gen12_norom_msg(GB_GEN2);
      else gb_gen12_nomoves_msg(g_ed->s.gen);
      return BANK_DOWN_REFUSED;
    }
  }

  /* BACKLOG #246 review D2: no boxoam_suspend/resume bracket here either -- same
   * reasoning as the two sites above. */
  if (fix || nbad > 0) {
    GbXferChoice ch = gb_paste_legal_screen_ex(gb_get_species_dex(&mon), fix_from,
                                                fix ? fix_to : 0, from4, bad4, fill4, nbad);
    if (ch == GB_XFER_CANCEL) return BANK_DOWN_REFUSED;
    if (ch == GB_XFER_MAKE_LEGAL && fix) gb_set_level(&mon, fix_to);
  }

  bool wok = gb_paste_write(&mon, dst_box, cell80, loss.item_outcome, g1_item); /* 6-8 */
  return wok ? BANK_DOWN_LANDED : BANK_DOWN_REFUSED;
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
  if (strcmp(what_for_log, "exit") != 0 && app_gb_hold_live())         /* #234 s4: the edit is kept in-session: it is a step */
    (void)app_gb_stage(g_ed->pristine, g_ed->img, gb_step_name(what_for_log), true);
  memcpy(g_ed->pristine, g_ed->img, g_ed->len);
  if (g_m) { gb_census(g_m); g_m->loaded = -1; }   /* the exit save runs after the grid is gone: g_m is NULL */
  snd_error();
  msg_wait("GAME BOY SAVE", UI_WARN, "Edits are in-session only",
           "in the emulator build.");
  return false;
#else
  /* b160: choke-point backstop for hard rule 4 (writes are Omega-only). Every
   * gb_persist() caller is SUPPOSED to already be behind a can_edit gate the
   * nav dispatch passed to its screen (gb_nav_from_start, all now
   * `ed && app_can_edit()` per this slice's own fix) or, for the direct edit/
   * move/release hooks, gb_locate()'s own app_can_edit() check above -- but
   * this function is the ONE place every GB write path converges on before
   * touching the card, so it re-checks here rather than trusting every caller
   * (present and future) got its own gate right. Mirrors the PDNA_DELTA
   * refusal shape above: log, sound, message, return false -- nothing touched. */
  if (app_partial_refuse()) {   /* D10/9.2: a TORN step left PART of a step in the image -- never written; the edit rolls back to the (partial) baseline */
    gb_rollback();
    return false;
  }
  if (!app_can_edit()) {
    log_line("gen12: persist refused: %s", app_readonly_why());
    app_log_flush();
    gb_rollback();            /* pdna_gen12.h:263 contract: EVERY non-DELTA refusal
                               * leaves the resident image at the card's bytes and
                               * re-latches the session. Callers discard our return
                               * value and rely on exactly this. */
    snd_error();
    msg_wait(PDNA_GBEDIT_READONLY_TITLE, UI_WARN, app_gb_readonly_why(), 0);
    return false;
  }
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
  /* #234 s4: the card holds the image now. An IMMEDIATE persist is a crossed step (a transfer, a bank hand-off: it lived in
   * another file, so undo must not walk back over it); the exit write ("exit") only flushes steps already recorded. The step
   * is recorded AFTER the verified write on purpose: a failed write rolls back to the baseline and records nothing, so the
   * journal never holds a step the image does not. Everything above this line -- the backup, the verified write, its
   * failure triage -- is unchanged and in its original order. */
  if (strcmp(what_for_log, "exit") != 0 && app_gb_hold_live())
    (void)app_gb_stage(g_ed->pristine, g_ed->img, gb_step_name(what_for_log), true);
  app_gb_saved();                                 /* the image is on the card: not dirty, the journal's cursor is the saved point */
  memcpy(g_ed->pristine, g_ed->img, g_ed->len);   /* the card now holds this image */
  if (g_m) { gb_census(g_m); g_m->loaded = -1; } /* nready/nblocked may have changed; the grid re-pages. g_m is NULL on the exit save */
  snd_save();
  return true;
#endif /* PDNA_DELTA */
}

/* ---- #234 slice 4: the resident Game Boy session on the journal (design D8) -------------------------------------------
 * `pristine` is now the STAGED baseline (the last state the session accepted and the journal recorded), not the card:
 * a held commit re-baselines it, so gb_rollback still undoes exactly ONE failed edit and never throws earlier staged
 * edits away. The card image is only ever the file on the SD (re-read on a discard). */
bool gb_hold_live(void) { return g_ed && app_gb_hold_live(); }

uint64_t pdna_gen12_journal_key(void) { return g_ed ? gb_journal_key(&g_ed->s) : 0u; }

/* The image was patched behind the session's back (undo / redo / re-apply): the patched bytes are the new baseline; the
 * session re-latches over them (gbs_open re-identifies, exactly what gb_rollback does) and the grid re-pages. */
void gb_relatch(void) {
  if (!g_ed) return;
  memcpy(g_ed->pristine, g_ed->img, g_ed->len);
  if (gbs_open(&g_ed->s, g_ed->img, g_ed->len, g_ed->scratch, sizeof g_ed->scratch) != GBS_OK)
    log_line("gen12: relatch: the patched image no longer opens as a session");
  if (g_m) { gb_census(g_m); g_m->loaded = -1; }
}

/* THE QUIET COMMIT. The edit is already in g_ed->img; with the journal recording it becomes ONE recorded step, the
 * baseline moves up to it and NOTHING is written (the exit confirm writes once). With the journal off it is exactly
 * gb_persist(): the caller's own confirm stays and the write is immediate, as before slice 4. */
bool gb_hold_commit(const char* what_for_log) {
  if (!g_ed) return false;
  if (!app_can_edit() || !app_gb_hold_live()) return gb_persist(what_for_log);
  if (!app_gb_stage(g_ed->pristine, g_ed->img, gb_step_name(what_for_log), false)) return gb_persist(what_for_log);   /* not recorded: no net under a hold -> write now */
  memcpy(g_ed->pristine, g_ed->img, g_ed->len);
  gb_census(g_m);
  g_m->loaded = -1;
  log_line("gb %s: staged (held until exit)", what_for_log);
  snd_ok();
  return true;
}

/* B at the exit confirm: put the card's image back (re-read the file), re-baseline + re-latch, and tell the journal its
 * thrown-away steps were discarded. A failed re-read leaves the staged image in RAM (never written; the session is
 * ending and the next open re-reads the file). The emulator build has no card: its edits stay in-session, as always. */
void gb_discard_staged(void) {
#ifdef PDNA_DELTA
  log_line("gb exit: discard in the emulator build leaves the in-session image (no card to re-read)");
  app_gb_dirty_clear();
#else
  uint32_t got = 0;
  rmbl_pause();
  SfStatus st = sf_read_full(g_ed->path, g_ed->img, g_ed->len, &got);
  rmbl_resume();
  if (st != SF_OK || got != g_ed->len) {
    log_line("gb exit: discard re-read failed (%s, %lu of %lu B) - RAM only, never written", sf_status_str(st),
             (unsigned long)got, (unsigned long)g_ed->len);
    app_gb_dirty_clear();
    return;
  }
  gb_relatch();
  app_gb_discarded();
#endif
}

/* The session's ONE exit confirm (Gen 3's flush_on_exit): the pending records reach the journal first (a cut between that
 * and the write = the load-time offer), then A writes the whole staged image once, B discards it. */
static void __attribute__((noinline)) gb_flush_on_exit(void) {
  if (!g_ed) return;
  app_gb_rest();
  if (!app_gb_dirty()) return;
  if (app_confirm("Save changes?", "Save the staged changes?")) {
    (void)gb_persist("exit");                 /* reports its own refusals; success marks the journal saved */
    app_gb_rest();                            /* the next segment + any identity redirect */
  } else {
    gb_discard_staged();
  }
}

/* The History screen over a resident GB session. Its rows borrow the GB arena tail (the session already holds the arena; the
 * file browser's g_entries the Gen-3 screen borrows is not this session's to take). A read-only mount / a journal that is
 * off gets the same plain "History is off for this save" page the Gen-3 screen shows (the screen itself says so). */
#define GB_HIST_ROWS 48
void pdna_gen12_history(void) {
  JaHist* rows = g_ed ? (JaHist*)(void*)gb12_arena_tail(sizeof(JaHist) * GB_HIST_ROWS) : 0;
  if (!rows) {
    msg_wait("HISTORY", UI_WARN, g_ed ? "Not enough memory right now." : "History is off for this save.", g_ed ? 0 : "(read-only mount.)");
    return;
  }
  pdna_history_screen_rows(rows, GB_HIST_ROWS);
  gb12_arena_tail_release();
}

/* The session opened: bind the recorder to the resident image, open its journal (+ the load-time offer, + the ring's first
 * fill). Never fatal: any failure leaves the journal off and the UI silent about it. */
static void __attribute__((noinline)) gb_journal_session_open(void) {
  uint64_t key = gb_journal_key(&g_ed->s);
  if (!key) { log_line("journal(gb): no identity key, journal off"); return; }
  (void)app_gb_journal_open(g_ed->img, key, gb_journal_key_legacy(&g_ed->s));   /* #308: an old-keyed journal is continued */
}

/* Arena block for a session whose bytes are already resident: no FIL, and the image
 * pointer is the caller's (g_save), so only the mount + the two GB buffers are borrowed. */
#define GB12_ARENA_NEED_IMG (GB12_A4(sizeof(Gb12Mount)) + GB12_A4(sizeof(Gb12Image)) + \
                             GB12_RECS_BYTES + GB12_STAGE_BYTES + GB12_A4(sizeof(Gb12Edit)) + 4u)
_Static_assert(GB12_ARENA_NEED_IMG <= APP_ARENA_BYTES,
               "GB import (resident image) no longer fits the borrowed EWRAM arena");
/* BACKLOG #208 fixes review D3: pins the arena-tail slack every gb12_arena_tail()
 * caller's own comment cites as a literal (pdna_gen12.h's GB12_ARENA_TAIL_SLACK)
 * to the SAME arithmetic gb12_arena_tail() itself runs at call time -- a future
 * change to GB12_ARENA_NEED_IMG or APP_ARENA_BYTES that silently drifts the real
 * slack away from the number every caller's comment assumes now fails the build
 * instead of only being caught (or not) by eyeballing bytes on a review. */
_Static_assert((uint32_t)APP_ARENA_BYTES - (uint32_t)GB12_ARENA_NEED_IMG == GB12_ARENA_TAIL_SLACK,
               "gb12_arena_tail()'s real slack drifted from GB12_ARENA_TAIL_SLACK -- "
               "update the constant (and every caller's cited figure) together");

/* U2b item 0: the arena TAIL past Gb12Edit, for a GB screen shell (pdna_gbscreen.c)
 * riding the resident-image mount. pdna_gen12_show_image()'s own carve-out puts
 * Gb12Edit LAST in the block (Mount, Gb12Image, recs, stage, Gb12Edit -- see that
 * function, ~pdna_gen12.c:2671-2711) and nothing else in this file claims arena
 * bytes past it for the rest of the session: `ed`'s own end IS the arena's used
 * high-water mark. GB12_ARENA_NEED_IMG (11,968 of APP_ARENA_BYTES' 35,712) already
 * counts every byte up to and including Gb12Edit, so the slack below (23,744 B) is
 * exactly what a caller may still take.
 *
 * `need <= slack` is the one runtime check. BACKLOG #64 review Finding 6 (CRITICAL
 * fix): this used to read "g_ed itself must be non-NULL ... never the read-only
 * nav-menu FIL mount ... this function correctly refuses for" -- that was WRONG
 * the moment this lane gave the read-only nav-menu FIL mount (and the fused delta
 * entry) their own streamed GbSession: pdna_gbflags/pdna_gbdex/pdna_gbpack's own
 * screens call THIS function regardless of which kind of session they were handed
 * (gs, not g_ed -- see gb_nav_from_start), so a blanket "g_ed == NULL always
 * refuses" answer here would false-panel every one of those screens on the
 * streamed path instead of rendering them. The real rule now: g_ed non-NULL uses
 * the resident-image tail (below, unchanged); g_ed NULL uses g_ro_tail instead
 * (set only by a successful gbs_open_streamed(), see that global's own comment) --
 * either way a NULL return is an ordinary "not available right now", same posture
 * as app_arena_acquire() itself, never a silent wrong-tail hand-out. */
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
  if (g_tail_lent) return NULL;          /* already out on loan this visit    */
  /* BACKLOG #64 review Finding 6: the streamed (read-only, g_ed == NULL) tail --
   * see g_ro_tail's own comment above. Checked BEFORE the resident-image math below
   * so a streamed session never falls through to dereference a NULL g_ed. */
  if (!g_ed) {
    if (!g_ro_tail || need > g_ro_tail_slack) return NULL;
    g_tail_lent = true;
    return g_ro_tail;
  }
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

/* BACKLOG #150 S150-14 (Guy's §10 Round-4 Q2, docs/BANK-CROSSGEN-DESIGN.md:456-458,
 * verbatim: "editing a native Bank cell -> MUST-HAVE NOW: a slice that opens the
 * Gen-1/2 editor (the GB session's own summary/edit screens) on a native Bank cell,
 * writing the native bytes back through bc_pack; the Gen-3 editor stays denied on a
 * native cell"). This was S150-2 step 5's read-only-only wrapper (SCOPE DEVIATION,
 * forward-pulled from S150-13's own row, docs/BANK-CROSSGEN-DESIGN.md:1345); it
 * still deliberately stays a SECOND, PARALLEL entry point, NOT the refactor of
 * gb_view_hook (below) that S150-13's own design (§11.9) eventually wants (a shared
 * gb_summary_show(mon, gen), called from both this and gb_view_hook) -- §11.9's own
 * argument for denying the GEN-3 editor on a native cell (it would write an 80-byte
 * Gen-3 record and destroy the "GBC1" tag) is untouched by Guy's Q2 and still stands;
 * this function only ever writes back through bc_pack, never through the Gen-3
 * editor's commit path. `has_sidecar` stays false: the ledger's key space
 * (xr_key_g3, bytes 0..7) is not gbsc_key's, so gb_has_sidecar would warn about the
 * wrong file (decision 11) -- the correct orphan warning for an edit is
 * app_xfer_pid_guard's own dialog, fired by the caller at commit time.
 *
 * `can_edit` is computed HERE, not passed straight through: `allow_edit` alone is
 * not enough to actually enter edit mode (decision 10) -- `out80` must be non-NULL
 * (nowhere to pack a result) and `app_can_edit()` must be true (the CART gate; there
 * is no GB save mounted here for `gb_locate()` to check instead).
 *
 * TRAP (decision 3): `pdna_gbsummary` edits `e` IN PLACE and does NOT restore it on a
 * discard (pdna_gbedit.h:22-24 says so outright) -- so `bc_unpack` is re-run at the
 * TOP of EVERY loop iteration, never once before the loop. A single `bc_unpack`
 * outside the loop was safe only while can_edit was always false (S150-2); with
 * editing on, a discarded edit followed by a U/D re-open would otherwise show the
 * mutated (discarded) record as if it had been kept. `saved` is also checked BEFORE
 * the `nav == 0` test, since pdna_gbsummary.c can set `*saved` on a U/D exit too
 * (:617-625), not only on B. */
/* BACKLOG #150 S150-15 decision 7: the body of gb_native_summary_open, unchanged
 * shape, with a caller-supplied note line instead of the hard-coded
 * "Gen 1/2 record" pair -- `note` NULL keeps that exact default (gb_native_summary_
 * open's own three callers pass NULL implicitly by construction below). Read-only
 * for the S150-15 caller is STRUCTURAL, not a policy flag threaded through: pass
 * `can_edit=false, out80=NULL` and `pdna_gbsummary` can never set `*saved` (its own
 * contract), so the `if (saved)` arm's whole re-pack body never runs. */
static bool native_summary_run(const uint8_t rec80[80], bool can_edit,
                               uint8_t out80[80], const char* note) {
  int card = 0;                                            /* sticky across re-opens, gb_view_hook's own hoist */
  for (;;) {
    GbEditMon e; BcMeta meta;
    if (!bc_unpack(rec80, &e, &meta)) return false;
    bool saved = false;
    gbedit_set_crystal_origin(meta.origin_game == BC_ORIGIN_CRYSTAL);   /* #362: held-item list follows the record's origin */
    int nav = pdna_gbsummary(&e, can_edit, /*start_editing*/false,
                        note ? note : (meta.gen == GB_GEN1 ? "Gen 1 record" : "Gen 2 record"),
                        /*has_sidecar*/false, /*create*/false, &saved, &card);
    gbedit_set_crystal_origin(false);
    if (saved) {
      /* decision 4: keep bank_serial/origin_game/rtc_epoch; ident32 recomputes for
       * free inside bc_pack; re-derive only the two flag bits a GbEditMon can carry
       * (b3 egg, b4 held-item) -- b0/b1/b2 are Bank/ledger state bank_cell.h says a
       * GbEditMon has no home for, and must survive verbatim. */
      /* S150-12 decision 3: BC_FLAG_COPY (b5) is permanent while the cell lives, so it
       * must survive an edit's re-pack too -- an unmarked copy would silently start
       * writing ledger entries on its next DOWN (G-L3 reached through the editor). */
      uint8_t nf = (uint8_t)(meta.flags & (BC_FLAG_FROM_PARTY | BC_FLAG_HAS_XFER_REC |
                                            BC_FLAG_QUEUED_PC | BC_FLAG_COPY));
      if (gb_is_egg(&e))        nf |= BC_FLAG_EGG;          /* gb_edit.h -- Gen 2 list byte 0xFD   */
      if (gb_get_held_item(&e)) nf |= BC_FLAG_HOLDS_ITEM;   /* gb_edit.h -- Gen 2 only, 0 on Gen 1 */
      if (bc_pack(&e, nf, meta.origin_game, meta.rtc_epoch, meta.bank_serial, out80) != 0) return false;
      return true;
    }
    if (nav == 0) return false;
    /* U/D: a single native Bank cell has no prev/next mon to load -- re-open the SAME
     * cell (Review F5, S150-2), re-unpacking fresh from `rec80` at the top of the
     * next iteration so a preceding discard (nav != 0, saved == false) can never
     * leak the mutated `e` back into view (the trap this loop shape exists to avoid). */
  }
}

bool gb_native_summary_open(const uint8_t rec80[80], bool allow_edit, uint8_t out80[80]) {
  const bool can_edit = allow_edit && out80 && app_can_edit();
  return native_summary_run(rec80, can_edit, out80, NULL);
}

/* BACKLOG #150 S150-15 decision 7: the GB ORIGINAL row's own entry point -- a
 * converted mon's ledger `original80`, READ-ONLY (can_edit is a literal `false`,
 * out80 is NULL, so pdna_gbsummary can never set `*saved`), with a caller-composed
 * note line (decision 8's origin-game + transfer-date string) in place of the
 * hard-coded "Gen 1/2 record" pair. `has_sidecar` stays `false`, parity with the
 * native VIEW row (open question 5). */
bool gb_original_summary_open(const uint8_t original80[80], const char* note) {
  return native_summary_run(original80, /*can_edit*/false, /*out80*/NULL, note);
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
    pdna_summary_quiet_save(true);            /* #234 s4: a resident-session edit is held (one step; the exit confirm writes) */
    int nav = pdna_gbsummary(&e, can_edit, false,
                             gen == GB_GEN1 ? "Gen 1 record" : "Gen 2 record",
                             has_sidecar, false, &saved, &card);
    pdna_summary_quiet_save(false);
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
  /* BACKLOG #201 F4: `want` (computed above from `want_gen`) is known before the
   * call -- runs only that generation's three scan jobs on a cold scan. */
  int ok = rom_gbsprite_open(&g_ed->romgs, fused_gb_slice_read, &s_gb_create_slice, size,
                             g_ed->romscan, sizeof g_ed->romscan, want);
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
      /* BACKLOG #201 F4: `want` known before the call. */
      int ok = rom_gbsprite_open(&g_ed->romgs, gb_read, &g_ed->romfil, sz,
                                 g_ed->romscan, sizeof g_ed->romscan, want);
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
    /* BACKLOG #201 F4: `want` known before the call. */
    int ok = rom_gbsprite_open(&g_ed->romgs, gb_read, &g_ed->romfil, sz,
                               g_ed->romscan, sizeof g_ed->romscan, want);
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
 * to trust across calls, an open file handle is not.
 *
 * BACKLOG #150 S150-10 decision 5: `at_level` (added, least-diff shape) -- 0 means
 * "use rom_gblearn_min_level(dex) and report it via *out_level", CREATE's own
 * behaviour, unchanged for its one caller below (which still passes 0); any other
 * value fetches the learnset AT THAT LEVEL instead (gb_paste_fill_moves' own use:
 * the level the transfer will actually WRITE, decision 5's "written level" answer to
 * §11.12) and `*out_level` is left untouched (the caller already knows the level it
 * asked for -- there is nothing new to report back). */
static int __attribute__((noinline))
gb_create_learn(uint16_t dex, const uint8_t g1_start[4], uint8_t at_level,
                 uint8_t* out_level, uint8_t out4[4]) {
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
    uint8_t lvl = at_level ? at_level
                             : gb_origin_level_floor(g_ed->s.gen, dex, rom_gblearn_min_level(&g_ed->learn, dex));
    kept = g1_start ? rom_gblearn_moves_at_seeded(&g_ed->learn, dex, lvl, g1_start, out4)
                    : rom_gblearn_moves_at(&g_ed->learn, dex, lvl, out4);
    if (kept >= 0 && out_level && !at_level) *out_level = lvl;
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
    uint8_t lvl = at_level ? at_level
                             : gb_origin_level_floor(g_ed->s.gen, dex, rom_gblearn_min_level(&g_ed->learn, dex));
    kept = g1_start ? rom_gblearn_moves_at_seeded(&g_ed->learn, dex, lvl, g1_start, out4)
                    : rom_gblearn_moves_at(&g_ed->learn, dex, lvl, out4);
    if (kept >= 0 && out_level && !at_level) *out_level = lvl;
  }
  f_close(&g_ed->romfil);
  return kept;
#endif
}

/* BACKLOG #150 S150-10 decision 5: the fill for gb_paste_hook's bad move slots, off
 * CREATE's own ROM resolution (gb_create_locate_rom: registered first, then beside
 * the save; the fused slice under PDNA_DELTA) -- NOT gb_gen1_locate_rom's
 * beside-the-save-only probe (open question 2: a user with a DIFFERENT Red.gb
 * registered gets base stats from one ROM and moves from another; left as-is,
 * both are the user's own ROMs of the same game).
 *
 * `learn4` stays {0,0,0,0} (the legitimate "no ROM" input, Guy's "never block") when
 * no ROM resolves, when the Gen-1 base-stats lookup this needs for the starter seed
 * fails, or when gb_create_learn() itself returns < 0 -- g3gb_moves_fill() still
 * runs and empties every bad slot, decision 3's documented behaviour for an
 * all-zero learnset. `mon` is mutated in place: up to `nbad` slots get gb_set_move
 * (fresh base PP, 0 PP-Ups) then the whole record is left-packed (decision 4) --
 * both inside g3gb_moves_fill(), this function is a pure ROM-resolution wrapper
 * around it. Returns g3gb_moves_fill()'s own fill count (0..nbad). */
static int __attribute__((noinline)) gb_paste_fill_moves(uint16_t dex, uint8_t level, GbEditMon* mon,
                                const uint8_t bad4[4], uint8_t fill4[4], bool* no_rom) {
  uint8_t learn4[4] = { 0, 0, 0, 0 };
  bool have_rom = gb_create_locate_rom(g_ed->s.gen);
  if (have_rom && g_ed->s.gen == GB_GEN1) {
    RomGb1Species sp;
    if (gb_create_base1(dex, &sp)) {
      uint8_t g1_start[4];
      memcpy(g1_start, sp.start, 4);
      have_rom = gb_create_learn(dex, g1_start, level, NULL, learn4) >= 0;
    } else {
      have_rom = false;
    }
  } else if (have_rom) {
    have_rom = gb_create_learn(dex, NULL, level, NULL, learn4) >= 0;
  }
  if (no_rom) *no_rom = !have_rom;   /* BACKLOG #390: the caller's refusal wording depends on WHY the fill came up empty */
  if (!have_rom) {
    int n = 0; for (int i = 0; i < 4; i++) n += bad4[i] != 0;
    log_line("gen12: paste moves: no gen-%u rom, %d slot(s) emptied", (unsigned)g_ed->s.gen, n);
  }
  return g3gb_moves_fill(mon, bad4, learn4, fill4);
}

/* BACKLOG #265: CREATE's origin choice -- LEGIT COPY reads the registered ROM for the
 * species' real level and moves; FROM SCRATCH builds a level-1 Growl mon (see
 * gb_create_src_scratch). Same idiom as gb_paste_legal_screen_ex above: a
 * title, prose, an "A = ..." row, a "SELECT = ..." row, "B = cancel". */
typedef enum { GB_CREATE_CANCEL = 0, GB_CREATE_LEGIT, GB_CREATE_SCRATCH } GbCreateMode;

static GbCreateMode __attribute__((noinline)) gb_create_origin_screen(uint16_t dex) {
  ui_clear();
  ui_text(4, 3, UI_TITLE, PDNA_GBCREATE_ORIGIN_TITLE);
  ui_hline(0, 13, UI_SCR_W, UI_BORDER);
  int y = PDNA_SIDECAR_LOSS_ROW_Y0;
  ui_ptext_fit(4, y, UI_SCR_W - 8, UI_TEXT, pk_species_name(dex));
  y += PDNA_SIDECAR_LOSS_ROW_H + PDNA_SIDECAR_LOSS_ROW_H / 2;
  ui_ptext_fit(4, y, UI_SCR_W - 8, UI_TEXT, PDNA_GBCREATE_ORIGIN_A);
  y += PDNA_SIDECAR_LOSS_ROW_H;
  ui_ptext_fit(4, y, UI_SCR_W - 8, UI_DIM, PDNA_GBCREATE_ORIGIN_A_WHY);
  y += PDNA_SIDECAR_LOSS_ROW_H + PDNA_SIDECAR_LOSS_ROW_H / 2;
  ui_ptext_fit(4, y, UI_SCR_W - 8, UI_TEXT, PDNA_GBCREATE_ORIGIN_SEL);
  y += PDNA_SIDECAR_LOSS_ROW_H;
  ui_ptext_fit(4, y, UI_SCR_W - 8, UI_DIM, PDNA_GBCREATE_ORIGIN_SEL_WHY);
  y += PDNA_SIDECAR_LOSS_ROW_H + PDNA_SIDECAR_LOSS_ROW_H / 2;
  ui_text(4, y, UI_DIM, PDNA_GBCREATE_ORIGIN_B);
  u16 k = s_wait(KEY_A | KEY_SELECT | KEY_B);
  if (k & KEY_B) return GB_CREATE_CANCEL;
  return (k & KEY_SELECT) ? GB_CREATE_SCRATCH : GB_CREATE_LEGIT;
}

/* LEGIT COPY: locate the ROM, read the species' base row, then its learnset AND its
 * lowest legal level (gb_create_learn -> gb_origin_level_floor, BACKLOG #266) in one
 * open. Fills src's ROM facts + moves and *lvl; false (after its own message) on any
 * failure. This is the body CREATE ran unconditionally before #265. */
static bool __attribute__((noinline)) gb_create_src_legit(uint16_t dex, GbNewMonSrc* src, uint8_t* lvl) {
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

  uint8_t g1_start_buf[4]; const uint8_t* g1_start = NULL;
  if (g_ed->s.gen == GB_GEN1) {
    RomGb1Species sp;
    if (!gb_create_base1(dex, &sp)) {
      snd_deny(); msg_wait(PDNA_GBCREATE_TITLE, UI_WARN, PDNA_SIDECAR_GEN1_BADROM_L1, 0); return false;
    }
    memcpy(src->base, sp.base.base, GB_NSTATS);
    src->type1 = sp.base.type1; src->type2 = sp.base.type2;
    src->growth = sp.growth;
    memcpy(g1_start_buf, sp.start, 4);
    g1_start = g1_start_buf;
  } else {
    RomGb2Species sp;
    if (!gb_create_base2(dex, &sp)) {
      snd_deny(); msg_wait(PDNA_GBCREATE_TITLE, UI_WARN, PDNA_SIDECAR_GEN1_BADROM_L1, 0); return false;
    }
    src->growth = sp.growth;
  }

  /* No level PROMPT any more (BACKLOG #50 UX-parity): gb_create_learn()
   * computes the species' own lowest legal level (rom_gblearn_min_level, off
   * the SAME ROM) and its moveset at that level together, one ROM open. */
  *lvl = 5;
  int kept = gb_create_learn(dex, g1_start, 0, lvl, src->moves);
  if (kept < 0) {
    snd_deny();
    msg_wait(PDNA_GBCREATE_TITLE, UI_WARN, PDNA_GBCREATE_NOROM_L1, PDNA_GBCREATE_NOROM_L2);
    return false;
  }

  return true;
}

/* FROM SCRATCH (Guy 2026-09-29): level 1, Growl, no learnset lookup, no level floor.
 * Gen 2 needs nothing from the card at all (base stats/growth are the in-tree Gen-3
 * tables, gb_edit.h). Gen 1 has no in-tree Game Freak data, but BACKLOG #276's
 * generated, git-ignored table (gb1_base_tbl.h, tools/gen_gb1_base.py) carries all 151
 * base rows: when it is linked the arm is ROM-free and touches no file at all -- it
 * works with NO ROM registered. Only a build without the generated table (or a dex it
 * lacks) falls back to the one 28-byte base-row read from the registered ROM, and
 * refuses after its own message when there is none. */
static bool __attribute__((noinline)) gb_create_src_scratch(uint16_t dex, GbNewMonSrc* src) {
  src->moves[0] = 45;   /* Growl: the same move id in Gen 1 and Gen 2 */
  if (g_ed->s.gen == GB_GEN1) {
    if (gb1_base_table_fill(dex, src)) return true;   /* zero card access */
    s_busy_reading();
    RomGb1Species sp;
    if (!gb_create_locate_rom(GB_GEN1)) {
      snd_deny();
      msg_wait(PDNA_GBCREATE_TITLE, UI_WARN, PDNA_GBCREATE_NOROM_L1, PDNA_GBCREATE_NOROM_L2);
      return false;
    }
    if (!gb_create_base1(dex, &sp)) {
      snd_deny(); msg_wait(PDNA_GBCREATE_TITLE, UI_WARN, PDNA_SIDECAR_GEN1_BADROM_L1, 0); return false;
    }
    memcpy(src->base, sp.base.base, GB_NSTATS);
    src->type1 = sp.base.type1; src->type2 = sp.base.type2;
    src->growth = sp.growth;
  } else {
    src->growth = gb_growth_rate(dex);   /* cross-checked inside gb_new_mon */
  }
  return true;
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
  /* b160 F2: the CREATE row reaches here with NO cart gate anywhere on its path
   * (app_src_empty_action_offered -> app_mon_menu_readonly's RO_CREATE row ->
   * g_src_ops->create; none check app_can_edit). Same gate gb_locate() applies to
   * every other mutating hook -- refuse BEFORE the picker, not at gb_persist(). */
  if (!app_can_edit()) {
    snd_deny();
    msg_wait(PDNA_GBEDIT_READONLY_TITLE, UI_WARN, app_gb_readonly_why(), 0);
    return false;
  }
  int box = (g_m->ui_box >= 0 && g_m->ui_box <= g_m->party_box) ? g_m->ui_box
          : (g_m->current_box >= 0 && g_m->current_box <= g_m->party_box) ? g_m->current_box
          : 0;
  const int original_box = box;   /* review fix 3: named in the post-create message
                                   * only when F4's picker actually redirected here */
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
  int cap   = gb_list_capacity(g_ed->s.gen, box);
  /* BACKLOG #187, F4: split the old `count < 0 || count >= cap` fold in two --
   * Step 1 (this backlog's own repro matrix, run against Guy's real Yellow.sav on
   * the delta vehicle) found `box` here already correct on every box tried (12, 11,
   * a has-room box) -- the ui_box/current_box fallback chain 2026-09-07's own fix
   * put in place holds. Every CREATE refusal reachable today is a genuinely full
   * box (Yellow's own boxes 1-7, 20/20 each); an unreadable list is a DIFFERENT,
   * separately-worded problem (a corrupt box, not a full one) that was silently
   * wearing the same "BOX FULL" words before this split. */
  if (count < 0) {
    snd_deny(); msg_wait(PDNA_GBCREATE_BADLIST_TITLE, UI_WARN, PDNA_GBCREATE_BADLIST_L1, 0);
    return false;
  }
  if (count >= cap) {
    /* Name the box and its count (brief's own wording: "Box 1 is full (20/20) --
     * pick another box"), then offer the SAME destination picker DUPLICATE/MOVE TO
     * BOX use (gb_pick_box, F3's n/cap+dim-full picker) instead of a flat refusal.
     * `box` is reassigned to the pick -- everything below (species/level/insert)
     * runs against the NEW destination; gbs_insert() reloads its own list for
     * whatever box it is handed, so no stale state carries over from the full one. */
    char nm[12], l1[32];
    pdna_gen12_box_name(g_m, box, nm);
    siprintf(l1, "%s is full (%d/%d).", nm, count, cap);
    snd_deny();
    msg_wait(PDNA_GBCREATE_FULL_TITLE, UI_WARN, l1, PDNA_GBCREATE_FULL_PICKHINT_L2);
    int dst = gb_pick_box(g_m, box, PDNA_GBEDIT_PICKBOX_CREATE_TITLE, true);
    if (dst < 0) return false;   /* B on the picker, or gb_pick_box's own "no room anywhere" */
    box = dst;
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

  /* BACKLOG #265: the LEGIT COPY / FROM SCRATCH choice comes BEFORE any card access
   * (the legit arm's reads freeze the screen for a real ROM scan -- s_busy_reading()
   * inside gb_create_src_legit -- so the player must know why, and must be able to
   * decline). With no ROM registered there is nothing to copy from: build from scratch
   * without asking (Gen 1 uses the generated gb1_base table when linked, else the
   * ROM read for its base row -- and says so, below). */
  GbNewMonSrc src; memset(&src, 0, sizeof src);
  uint8_t lvl = 1;
  bool legit = false;
  if (gb_create_rom_available()) {
    GbCreateMode mode = gb_create_origin_screen(dex);
    if (mode == GB_CREATE_CANCEL) return false;
    legit = (mode == GB_CREATE_LEGIT);
  }
  if (!(legit ? gb_create_src_legit(dex, &src, &lvl) : gb_create_src_scratch(dex, &src))) return false;

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
  gb_set_caught_available(&box_mon, gb_session_is_crystal(&g_ed->s));   /* #356: gb_load_parts leaves has_caught false (gb_mark_caught's rule) */

  bool saved = false; int card = 0;
  pdna_summary_quiet_save(true);            /* #234 s4: a resident-session edit is held */
  pdna_gbsummary(&box_mon, true, true, g_ed->s.gen == GB_GEN1 ? "Gen 1 record" : "Gen 2 record",
                false /* a freshly created mon can never already have a sidecar */,
                true /* BACKLOG #50 UX-parity: the NEW chip + START-keep confirm */,
                &saved, &card);
  pdna_summary_quiet_save(false);
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
  bool ok = gb_hold_commit("create");
  /* Review fix 3 (LOW), BACKLOG #187: F4's picker can redirect `box` away from the
   * one the grid is showing -- say so, or the new mon looks like it never landed. A
   * plain in-place create (the common case) already shows it right where the
   * player is looking; nothing new to say there. */
  if (ok && box != original_box) {
    char nm[12], l1[32];
    pdna_gen12_box_name(g_m, box, nm);
    siprintf(l1, PDNA_GBCREATE_REDIRECTED_FMT, nm, slot_out + 1);
    msg_wait(PDNA_GBCREATE_REDIRECTED_TITLE, UI_OK, l1, 0);
  }
  return ok;
}

/* app_src_ops_set() hook: ITEM on the read-only mon menu (BACKLOG #92). Gen 2 only --
 * only k_gb_ops_gen2 below installs this; k_gb_ops_gen1 leaves it NULL, so the row
 * never appears for a Gen-1 mount rather than appearing and refusing every press
 * (pdna_app.h's own `item` comment). Mirrors Gen 3's own quick-item action
 * (app_quick_item, pdna_main.c): the SAME pick_item() screen gb_editor.c's own
 * GBE_ITEM row already opens (pdna_gbedit.c's GBE_K_ITEM branch), restricted to the
 * real Gen-2 item ids with REAL names (#340a; the "NO ITEM" row, id 0, removes one, via
 * pick_item_set_gen1_2_held()) -- no separate legality gate:
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

  GbGame ig = gb_session_is_crystal(s) ? GBF_G_CRYSTAL : GBF_G_GS;   /* #340a: Gold vs Crystal pocket map */
  pick_item_set_gen1_2_held(GBIN_GEN2, ig, gbb_max_item_id(ig));
  uint16_t id = pick_item(gb_get_held_item(&e));
  pick_item_set_gen1_2(0, GBF_G_RED, 0);
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
 * g_ed->s.gen, same idea as k_gb_ops vs k_gb_ops_ro picking off g_ed itself.
 * BACKLOG #246: `.paste = 0` on both (was gb_paste_hook, deleted this same commit)
 * -- the menu-driven clipboard PASTE route into a Game Boy save is retired; the
 * ONLY route now is the Bank-drop arm (gb_bank_down_g3, above), reached through
 * drop_held's cross-scope dispatch, never through g_src_ops->paste. app_mon_menu's
 * own `paste = g_src_ops && g_src_ops->paste && ...` gate (pdna_main.c) already
 * hides the PASTE row the moment this is NULL -- no separate row deletion needed. */
static const AppSrcOps k_gb_ops_gen1 = {
  .edit = 0, .move = gb_move_hook, .release = gb_release_hook,
  .copy_native = gb_copy_native_hook, .paste = 0, .view = gb_view_hook,
  .editable = gb_editable_hook, .create = gb_create_hook,
  .dup = gb_dup_hook, .daycare = gb_daycare_hook, .export_one = gb_export_hook,   /* BACKLOG #93 */
  .lift_why = gb_lift_why_hook,                                                  /* BACKLOG #166 */
};
static const AppSrcOps k_gb_ops_gen2 = {
  .edit = 0, .move = gb_move_hook, .release = gb_release_hook,
  .copy_native = gb_copy_native_hook, .paste = 0, .view = gb_view_hook,
  .editable = gb_editable_hook, .create = gb_create_hook, .item = gb_item_hook,
  .dup = gb_dup_hook, .daycare = gb_daycare_hook, .export_one = gb_export_hook,   /* BACKLOG #93 */
  .lift_why = gb_lift_why_hook,                                                  /* BACKLOG #166 */
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
/* BACKLOG #120 S2: the three GB-session gates (readonly / ops / hint), extracted so a
 * Bank visit can re-install all three when it hands the grid back, exactly like
 * gb_session_core's own initial install below. ORDER IS LOAD-BEARING: readonly_set
 * (pdna_main.c) ALSO nulls g_src_ops, exactly like readonly_clear -- so ops_set must
 * run AFTER readonly_set, never before, or the ops table it just installed is wiped
 * out again. hint goes last (it depends on neither of the other two). */
static void gb_session_ops_install(Gb12Mount* m) {
  app_src_readonly_set(pdna_gen12_why_locked, "Converted copy");
  app_src_ops_set(!g_ed ? &k_gb_ops_ro
                        : (g_ed->s.gen == GB_GEN2 ? &k_gb_ops_gen2 : &k_gb_ops_gen1));
  pdna_origin_box_set_hint(m->kind == GB12_SAVE_RBY ? PDNA_GEN1 : PDNA_GEN2);
}

/* BACKLOG #120 S2: the Bank, opened from a Game Boy session's box grid (bank_edge's UP
 * hop, r == 4). Mirrors pdna_main.c's own r==4 hop (:9089-9092) verbatim. BROKEN #2
 * (docs/briefs/s2-bank-gb-session-brief.md): pdna_bank_show() ends with
 * pdna_origin_box_clear(), so the GB grid's era-cache hint is gone the moment this
 * function returns -- gb_session_ops_install's hint re-install below is what actually
 * fixes that (box_decode's pdna_origin_box_note refills the cache on re-entry); the
 * cache itself does NOT survive the visit.
 *
 * STACK BUDGET (orchestrator ruling, BACKLOG #120 S2 review): nesting the Bank visit
 * inside gb_session_core's own frame adds gb_session_core (208 B) + this function
 * (8 B) under main -> pdna_gen12_show_image -> gb_session_core -> gb_bank_visit ->
 * pdna_bank_show -> pdna_box -> ... -> gb_art_fetch, tools/stack_budget.py's new
 * deepest whole-program chain (was 13,000/12,992, now 11,264/11,256 on main 719f483 -- margins 3,808/4,352, both still positive;
 * 1,776/2,320, both still positive). That chain is RUNTIME-UNREACHABLE from here,
 * twice over: (a) pdna_box.c's pcp_open_party_strip refuses immediately when
 * `src->is_bank` (pdna_box.c:2964, `if (src->is_bank) { snd_deny(); return; }`) --
 * pdna_gen12_source()/the Bank's own BoxSource both set is_bank true, so the PARTY
 * strip that walks into app_mon_menu/pdna_daycare/gb_art_fetch never opens from a
 * Bank visit; (b) even setting (a) aside, pcp_open_party_strip's own tripwire
 * (PDNA_PARTY_STRIP_NEED, pdna_box.c ~2966) refuses again when the room is short.
 * The walker is context-free -- it cannot see the is_bank DATA deny, only the STATIC
 * call edge -- so its whole-program figure is conservative by construction; the real
 * gate is `STACK ok` with a positive margin (confirmed: `python3 tools/stack_budget.py
 * --root pdna_bank_show` finds the same chain only 10,624/10,616 B deep, margin
 * 4,456/5,000, because rooting there drops main+pdna_gen12_show_image+gb_session_core+
 * this function's own frames). The structural fix -- routing the party-strip opener
 * through a BoxSource capability the Bank leaves NULL so the walker itself can scope
 * it out -- is BACKLOG #134, not this slice. */
static void gb_bank_visit(Gb12Mount* m, bool from_hop) {
  app_src_readonly_clear();                  /* also nulls g_src_ops, same as readonly_set */
  pdna_origin_box_set_hint(0);
  rmbl_fire(RCUE_ROOM);
  if (from_hop) app_box_start_set(2);   /* the UP hop arrives from below, like Gen 3's r == 4;
                                         * the START-menu entry must land where the Gen-3 twin does */                      /* bank opens at the bottom row (unless carrying) */
  int br = pdna_bank_show();
  if (br == 5) app_box_start_set(1);         /* bank dropped off the bottom -> PC tabs */
  gb_session_ops_install(m);                 /* re-install readonly -> ops -> hint, in order */
}

static void gb_nav_from_start(Gb12Mount* m, GbSession* ro) {
  int kind = (m->kind == GB12_SAVE_RBY) ? SE_KIND_GEN1 : SE_KIND_GEN2;
  /* BACKLOG #64: `gs` is whichever session this visit actually has -- the resident,
   * EDITABLE one (g_ed->s) when the resident-image path latched one, else the
   * read-only STREAMED one this nav-menu path built (`ro`, NULL if its own
   * gbs_open_streamed refused). `ed` is true only for the resident-image path: every
   * write primitive below is gated `ed && ...`, so a streamed session (gs == ro)
   * always renders through the SAME screens with editing compiled out, never a
   * write reaching gbs_write_field/gbs_commit_list with gs->img == NULL (that would
   * already refuse via gbs_can_write(), but the UI should never even offer it). */
  GbSession* gs = g_ed ? &g_ed->s : ro;
  bool ed = (g_ed != 0);
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
     * one-hour budget; flagged for a follow-up slice, not silently worked around.
     *
     * app_can_edit() here, not a bare `ed` (b160 fix, same pattern as b153's
     * NV_FLY change below): pdna_gbtrainer() trusts its caller for the write
     * gate (no internal app_can_edit() call of its own), so a resident session
     * on an EverDrive / bad-ROM-check / hack cart must not reach it with edits
     * enabled. */
    if (gs) pdna_gbtrainer(gs, ed && app_can_edit());
    else    (void)gb_info_page(m);   /* A and B both just return to the grid */
  } else if (nv == NV_DEX) {
    /* BACKLOG #87: the shared Pokedex screen (pdna_pick.c's pdna_dex_screen, reused
     * UNCHANGED under item 1's species cap), over source/gb_dex.h's owned/seen core
     * -- same "needs a live GbSession to write through" gate as NV_TRAINER/NV_BAG/
     * NV_CLOCK/NV_DAYCARE above. Offered on BOTH kinds (nav_avail's own GB_TABLE
     * says NAV_OK for both) -- pdna_gbdex() itself branches internally on s->gen to
     * add the Gen-2-only Unown-forms chooser, so this call site does not need to.
     *
     * app_can_edit() here, not a bare `true`: this is a NEW call site (same
     * reasoning NV_DAYCARE's own comment gives for why it does not just copy the
     * NV_TRAINER/NV_BAG/NV_CLOCK sibling literal). */
    if (gs) pdna_gbdex(gs, ed && app_can_edit());
    else    (void)gb_info_page(m);
  } else if (nv == NV_BANK) {
    /* BACKLOG #120 S2: the Bank is reachable from the START menu too, not only the
     * bank_edge UP hop -- no g_ed needed (unlike every real-art screen above, the
     * Bank never touches the GB session's own bytes; it is the Gen-3 PC storage,
     * gated entirely on the readonly/ops gates around the visit). */
    gb_bank_visit(m, false);
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
     * app_nav_refuse() and shows nav_avail's own honest message instead.
     *
     * app_can_edit() here, not a bare `ed` (b160 fix, same pattern as b153's
     * NV_FLY change below): pdna_gbbag() trusts its caller for the write gate
     * (no internal app_can_edit() call of its own). */
    if (gs) pdna_gbbag(gs, ed && app_can_edit());
    else    (void)gb_info_page(m);
  } else if (nv == NV_BAG && kind == SE_KIND_GEN2) {
    /* U5 (BACKLOG #67): Gold/Silver/Crystal's own Pack + PC store, mirroring
     * U4's own gate one branch up -- same "needs a live GbSession to write
     * through" rule (pdna_gbpack() itself refuses a NULL/non-Gen-2 session,
     * but g_ed's resident image is the only path that HAS one here; the
     * plain FIL-streaming entry falls back to the read-only info page, same
     * reasoning as NV_TRAINER above).
     *
     * app_can_edit() here, not a bare `ed` (b160 fix, same pattern as b153's
     * NV_FLY change below): pdna_gbpack() trusts its caller for the write gate
     * (no internal app_can_edit() call of its own). */
    if (gs) pdna_gbpack(gs, ed && app_can_edit());
    else    (void)gb_info_page(m);
  } else if (nv == NV_FLY) {
    /* BACKLOG #90: gb_fly.h has a real bit for both generations (see its own header)
     * -- same "needs a live GbSession to write through" gate as NV_TRAINER above. */
    if (gs) pdna_gb_fly(gs, ed && app_can_edit());
    else    (void)gb_info_page(m);
  } else if (nv == NV_CLOCK && kind == SE_KIND_GEN2) {
    /* BACKLOG #86/#108: Gen-2's own Clock fix screen, over gb_clock.h -- same
     * "needs a live GbSession to write through" gate as NV_TRAINER/NV_BAG above.
     * nav_avail's own GB_TABLE keeps this row NAV_NOT_IN_GAME for SE_KIND_GEN1 (no
     * clock at all), so this branch is never reached from the plain-Gen-1 nav menu;
     * gated on `kind` anyway, the same defensive posture NV_BAG's own two branches
     * take, so a stray Gen-1 press falls through to app_nav_refuse()'s honest
     * message instead of silently opening a Gen-2-shaped screen.
     *
     * app_can_edit() here, not a bare `ed` (b160 fix, same pattern as b153's
     * NV_FLY change below): pdna_gbclock() trusts its caller for the write
     * gate (no internal app_can_edit() call of its own). */
    if (gs) pdna_gbclock(gs, ed && app_can_edit());
    else    (void)gb_info_page(m);
  } else if (nv == NV_DAYCARE) {
    /* BACKLOG #85: same "needs a live GbSession to write through" gate as
     * NV_TRAINER/NV_BAG/NV_BAG above -- gbd_read()/gbd_deposit()/gbd_withdraw()
     * (gb_daycare.h) all take a GbSession*, which only the resident-image path
     * (g_ed) has. The deposit/withdraw source+destination is the box the grid
     * was ACTUALLY showing -- pdna_gbdaycare.h's own header documents this
     * (never the party; see that file for why).
     *
     * BACKLOG #85 review fix: a bare `m->current_box` here is the SAME
     * "not updated yet this visit" bug gb_create_hook() above already found and
     * fixed (its own header note: "CREATE from box 13 (17/20, real room) still
     * refused BOX FULL, because it was silently targeting box 0 (20/20)").
     * gbsrc_note_box() stores the box the grid switched to into `m->ui_box`,
     * NOT `m->current_box` (that field is only ever the save's own "live copy"
     * box, read once at mount) -- so this call site needs the EXACT SAME
     * ui_box-first fallback chain gb_create_hook() uses, not a direct read of
     * current_box. Caught by hand navigating the box grid to a box with a free
     * slot before entering the Day Care in an emulator screenshot: the
     * deposit/withdraw picker kept showing box 0's own mons regardless of
     * which box the grid had actually switched to.
     *
     * app_can_edit() here, NOT a bare `true` (unlike this branch's three
     * siblings above): pdna_gbdaycare's own `can_edit` is the ONLY gate its
     * deposit/withdraw/edit-commit paths check before writing (it has no
     * internal app_can_edit() call of its own, mirroring pdna_gbbag.c/
     * pdna_gbpack.c/pdna_gbtrainer.c, which also trust their caller) -- since
     * this is a NEW call site, passing the real cart state rather than
     * copying the sibling literal is the hard-rule-4-safe choice. */
    if (gs) {
      int box = (m->ui_box >= 0 && m->ui_box <= m->party_box) ? m->ui_box
              : (m->current_box >= 0 && m->current_box <= m->party_box) ? m->current_box
              : 0;
      if (gb_box_is_party(gs->gen, box)) box = 0;   /* the party is never a Day-Care source or landing (re-verify N1) */
      pdna_gbdaycare(gs, box, ed && app_can_edit());
    } else {
      (void)gb_info_page(m);
    }
  } else if (nv == NV_DATA) {
    /* BACKLOG #88: the Flags & counters screen, same "needs a live GbSession to
     * write through" gate every other real-art Gen-1/2 screen on this menu uses
     * (Trainer/Bag/Pack/Clock/Daycare above) -- pdna_gbflags() itself trusts its
     * caller (no internal app_can_edit() call, mirroring every sibling above),
     * so pass the real cart state, not a bare `true`. */
    if (gs) pdna_gbflags(gs, ed && app_can_edit());
    else    (void)gb_info_page(m);
  } else if (nv == NV_MAP && kind == SE_KIND_GEN1) {
    /* M1+M3 (BACKLOG #91): current-map view + in-map teleport, same "needs a
     * live GbSession to read the ROM's own tile bank through" gate every
     * other real-art Gen-1/2 screen on this menu uses (Trainer/Bag/Pack
     * above) -- g_ed->s is the same resident session those already
     * read/write through gb12_arena_tail(). The plain FIL-streaming mount
     * (no g_ed) falls back to the read-only info page, same as
     * Trainer/Bag/Pack. `ed && app_can_edit()` gates M3 (teleport writes)
     * exactly like every sibling write screen above; M1's own read-only
     * view stays available either way. */
    if (gs) pdna_gbmap_gen1(gs, ed && app_can_edit());
    else    (void)gb_info_page(m);
  } else if (nv == NV_BATTLEREC) {
    /* BACKLOG #89: the "Records" row hosts the Hall of Fame on a Game Boy save --
     * nav_avail's own GB_TABLE keeps this row NAV_OK on BOTH kinds (unlike every
     * other Hoenn/Frontier-shaped row), so this branch is reachable from either a
     * Gen-1 or a Gen-2 nav menu, not gated on `kind` the way NV_BAG/NV_CLOCK/NV_MAP
     * above are gated to their one supported generation. Same "needs a live
     * GbSession to write through" fallback as every sibling branch: the plain
     * FIL-streaming mount (no g_ed) falls back to the read-only info page. */
    if (gs) pdna_gbhof(gs, ed && app_can_edit());
    else    (void)gb_info_page(m);
  } else if (nv == NV_MAP && kind == SE_KIND_GEN2) {
    /* M1-G2 (BACKLOG #91): the Gen-2 twin of the branch above -- same gate,
     * same fallback. */
    if (gs) pdna_gbmap_gen2(gs);
    else    (void)gb_info_page(m);
  } else if (nv == NV_HISTORY) {
    pdna_gen12_history();       /* #234 s4: the journal's tree over this Game Boy save (a plain notice on a read-only mount) */
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
static void gb_session_core(Gb12Mount* m, GbSession* ro) {
  rmbl_fire(RCUE_ROOM);
  /* BACKLOG #279 (Guy: "there is an entering promt i dont understand... it should simply
   * enter it, pokedna becomes a tool to edit gen 2 and 1 saves just as its for gen 3"):
   * a GAME BOY SAVE info page (title, honest line, four counts, "A browse SEL list B
   * back") sat between the save pick and the box grid; a Gen-3 open has no such stop.
   * It is gone: the mount enters the grid directly. The page itself is still what the
   * no-session nav branches fall back to (gb_info_page below), and gb_report_page still
   * runs on the way OUT when something was locked/unreadable. */
  /* BACKLOG #77: at entry to the box grid, open the Gen-3 ROM wallpaper rung
   * (artless builds only, no-op if already open) so the box grid below can stream
   * the real wallpaper instead of the procedural grass when a Gen-3 ROM is
   * fused/registered. See app_gb_wallpaper_rom_open(). */
  /* Tell app_mon_menu that the ACTIVE box source is read-only, so its destructive
   * actions (PASTE / DUPLICATE / CREATE) are not offered on a source that cannot
   * accept them. This also suppresses the bank's deferred-delete bookkeeping for these
   * boxes — see pdna_main.c. Cleared unconditionally below; every exit from the box
   * screen passes through it. BACKLOG #120 S2: the three-gate install (readonly ->
   * ops -> hint, order load-bearing -- see gb_session_ops_install's own comment) is
   * now the same helper a Bank visit re-runs on its way back out. */
  gb_session_ops_install(m);
  /* D4 (review-opus, BACKLOG #93): s_dup_warned was never cleared anywhere, so the
   * once-per-visit sidecar warning (decision 2, gb_dup_confirm) was really once per
   * POWER-ON -- a second, later visit whose duplicated mon also has a sidecar claim
   * never saw the warning again. Same twin idiom as pdna_trainer.c:1020/
   * pdna_gbtrainer.c:1074's own s_id_warned reset: cleared here, once per visit,
   * where every entry into a GB session's box screen passes through. */
  s_dup_warned = false;
  BoxSource s = pdna_gen12_source(m);
  /* #77 (review, 2026-09-09): AFTER pdna_gen12_source() sets g_m, so app_save_kind()
   * reports GEN1/GEN2 when app_icon_rom_open()'s kind check runs. */
  app_gb_wallpaper_rom_open();
  /* BACKLOG #120 S2 [decided here]: the xfer peer is installed session-wide, not just
   * inside gb_bank_visit, so the chunk-DOWN deny at pdna_box.c:3178 (keyed on
   * `s_xfer_peer` alone) does not depend on whether the Bank has been visited yet --
   * one uniform behaviour for the whole session, cleared in the exit block below.
   * BACKLOG #150 S150-4 step 3: k_gb_xfer replaces the marker-only k_gb_xfer_s2 here
   * -- every existing predicate that keys on `s_xfer_peer` only reads its non-NULLness,
   * never dereferences a member through it, so installing the real table changes no
   * gate's verdict today; it is what makes start_carry's `src->xfer->lift_up` (this
   * step) and drop_held's UP branch (`s_xfer_peer->release_up`, step 5) reachable. */
  /* BACKLOG #150 S150-12 decision 2: same !g_ed ? selector as pdna_gen12_source()'s
   * own s.xfer install above -- the read-only mount gets the COPY-only, no-delete
   * table. */
  pdna_box_xfer_set(g_ed ? &k_gb_xfer : &k_gb_xfer_ro);
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
   * re-entry picks it back up. BACKLOG #188: the cursor CELL within the box is now
   * ALSO restored on this exact re-entry, via app_box_resume_take()/_note() --
   * pdna_box() applies the recorded cell whenever app_box_start_take()'s hint is 0
   * (every re-entry through this loop passes no directional hint) and the box
   * matches, same generic mechanism the PC/Bank share (parity, not a GB special
   * case; see app_box_resume_note()'s own header comment in pdna_app.h). */
  for (int r; (r = pdna_box(&s)) != 0; ) {
    if (g_ed) app_gb_rest();                       /* #234 s4: the grid was left = a rest point: the pending records reach the journal (rumble paused, verified) */
    if (r == 2) gb_nav_from_start(m, ro);
    else if (r == 4) gb_bank_visit(m, true);       /* BACKLOG #120 S2: bank_edge's UP hop */
    else if (r == 6) pdna_gen12_history();         /* #234 s4: the chord found the history diverged -> the History screen (the grid released its borrow) */
    else app_box_start_set(1);
    s = pdna_gen12_source(m);
  }
  /* BACKLOG #120 S2 (§8 H16): a GB-scope carry must not outlive this session -- a
   * Gen-3 carry (including a fresh DUPLICATE, s_orig_slot < 0) is untouched here and
   * survives back out, exactly as it did before this session existed. */
  if (xg_clear_carry_on_gb_exit(pdna_box_carry_is_gb())) pdna_box_clear_carry();
  pdna_box_xfer_set(0);
  pdna_origin_box_set_hint(0);
  app_src_readonly_clear();
  pdna_gen12_source(0);                      /* unmount: no dangling arena pointers */
  if (m->nblocked || m->nunreadable) gb_report_page(m);
}

/* BACKLOG #150 S150-12 decision 7: the read-only mount's ONE exit offer. Called at
 * EXACTLY two sites -- pdna_gen12_show() and pdna_gen12_show_fused(), each right
 * after their own FINAL app_arena_release() -- never anywhere else (not at Bank
 * open, not at the next mount, not from inside the GB session itself; OPEN QUESTION
 * 1 defers "repeats on the next Bank open" to S150-11). Why after the release and
 * not before: app_gen3_pc_live() reads !app_arena_held(), which is still true (held)
 * until the release actually runs, and the Bank the offer opens must see the real
 * g_pc, not the mount's borrowed arena view of it. Returns 1 when the user accepted
 * (the nav-menu caller opens the Bank); 0 otherwise -- pdna_gen12_show()/_fused()'s
 * own callers never read a nonzero return today besides this new meaning. */
static int __attribute__((noinline)) gb_ro_exit_offer(void) {
  if (!xg_pc_offer(g_pcq_count, app_gen3_pc_live())) {
    if (g_pcq_count)
      log_line("gen12: %d copies queued for the PC but no live Gen-3 PC -- offer skipped", g_pcq_count);
    g_pcq_count = 0;
    return 0;
  }
  char l1[40];
  siprintf(l1, PDNA_XFER_PCQ_L1_FMT, (int)g_pcq_count);
  bool yes = app_confirm(PDNA_XFER_PCQ_TITLE, l1);
  if (yes) pdna_bank_start_box_set(g_pcq_box);
  g_pcq_count = 0;
  return yes ? 1 : 0;
}

int pdna_gen12_show(const char* path, uint8_t met_game) {
  if (!path || !path[0]) return 0;

  /* The arena is the map screen's donor (pdna_app.h): it hands back g_pc, which is
   * only safe when the PC holds nothing unsaved. NULL means exactly that — say so
   * and stop, never "helpfully" commit on the user's behalf. */
  uint8_t* arena = app_arena_acquire(GB12_ARENA_NEED_RO);
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
  Gb12View* vw    = (Gb12View*)(uintptr_t)(stage + GB12_STAGE_BYTES);

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
  app_box_resume_clear();   /* BACKLOG #188: a previous save/session's resume cell must not leak in */
  /* BACKLOG #150 S150-12 decision 5/OPEN QUESTION 8: g_ro_path is set unconditionally
   * the moment the MOUNT itself succeeds -- a COPY lift only ever needs g_m->stage
   * (gb_copy_native_hook's own g_ed==NULL branch), never the streamed view session
   * below, so tying it to vw_ok would refuse a copy the mount can plainly serve.
   * decision 12: reset the "waiting for the PC" counter here too -- a stale count
   * from an entry that failed before this point (or from a previous mount) must
   * never leak into THIS one. */
  g_ro_path = path;
  g_pcq_count = 0;

  /* BACKLOG #64: a read-only STREAMED session over the SAME FIL/read-callback pair
   * the mount just used -- reachability + read parity for the eleven GB-screen nav
   * branches (gb_nav_from_start), never editing (g_ed stays untouched by this path;
   * see the brief's Option B decision and its STOP-LICENCE item (b)). Accept only on
   * GBS_OK AND the same generation cross-check pdna_gen12_show_image() makes against
   * its own gbs_open() -- a session that identified as the WRONG generation from the
   * same bytes the mount just parsed is a bug worth refusing loudly, not silently
   * trusting. Threaded into gb_session_core/gb_nav_from_start as the `ro` parameter,
   * NEVER as g_ed (which stays untouched on this path). */
  bool vw_ok = false;
  GbsStatus vst = gbs_open_streamed(&vw->s, gb_read, f, len, vw->scratch, sizeof vw->scratch);
  if (vst == GBS_OK && (vw->s.gen == GB_GEN1) == (m->kind == GB12_SAVE_RBY)) {
    vw_ok = true;
    /* BACKLOG #64 review Finding 6 (CRITICAL fix): install this session's own
     * read-only arena tail -- see g_ro_tail's own comment. The tail sits past
     * Gb12View, the same "+4 for 4-byte rounding" reasoning GB12_ARENA_NEED_RO's
     * own definition already uses. */
    g_tail_lent = false;
    g_ro_tail = (uint8_t*)(uintptr_t)vw + GB12_A4(sizeof(Gb12View)) + 4u;
    g_ro_tail_slack = (uint32_t)APP_ARENA_BYTES - (uint32_t)GB12_ARENA_NEED_RO;
  } else {
    log_line("gen12: read-only session refused (%s, gen %d vs mount kind %d)",
             gbs_status_text(vst), (int)vw->s.gen, (int)m->kind);
  }

  gb_session_core(m, vw_ok ? &vw->s : 0);

  /* MANDATORY: the arena block is about to go -- clear the tail exactly like every
   * `g_ed = 0` site already clears g_tail_lent, or the next screen (resident-image
   * or streamed) could be handed a pointer into an arena block that is no longer
   * this session's to use. g_ro_path clears in the SAME bracket (decision 5): a
   * stale pointer into a path string that is about to go out of scope must never
   * survive this function's return. */
  g_ro_tail = 0;
  g_ro_tail_slack = 0;
  g_tail_lent = false;
  g_ro_path = 0;

  f_close(f);
  /* BACKLOG #150 S150-12 decision 7: fires exactly once per mount exit, ONLY after
   * the arena release below has actually given g_pc back (app_gen3_pc_live() reads
   * !app_arena_held(), which is still true until this line runs). Replaces the old
   * unconditional `return 0;`. */
  app_arena_release();
  return gb_ro_exit_offer();
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

  uint8_t* arena = app_arena_acquire(GB12_ARENA_NEED_RO);
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
  Gb12View* vw    = (Gb12View*)(uintptr_t)(stage + GB12_STAGE_BYTES);

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
  app_box_resume_clear();   /* BACKLOG #188: a previous save/session's resume cell must not leak in */
  /* BACKLOG #150 S150-12 decision 5/12 (same reasoning as pdna_gen12_show() above,
   * `name` in place of `path` -- this entry mounts a fused ROM slice, not a FIL). */
  g_ro_path = name;
  g_pcq_count = 0;

  /* BACKLOG #64 review Finding (F1 ruling): this entry does NOT set g_ed either
   * (only pdna_gen12_show_image() does) -- the comment this replaces claimed
   * "already shows every screen through g_ed", which was false; every nav row here
   * fell back to gb_info_page exactly like the plain FIL entry did before this
   * lane. Same Option B installation as pdna_gen12_show() above, over the SAME
   * fused_gb_slice_read/s_gb_import_slice pair the mount just used. */
  bool vw_ok = (gbs_open_streamed(&vw->s, fused_gb_slice_read, &s_gb_import_slice, size,
                                  vw->scratch, sizeof vw->scratch) == GBS_OK) &&
               ((vw->s.gen == GB_GEN1) == (m->kind == GB12_SAVE_RBY));
  if (vw_ok) {
    g_tail_lent = false;
    g_ro_tail = (uint8_t*)(uintptr_t)vw + GB12_A4(sizeof(Gb12View)) + 4u;
    g_ro_tail_slack = (uint32_t)APP_ARENA_BYTES - (uint32_t)GB12_ARENA_NEED_RO;
  } else {
    log_line("gen12: fused read-only session refused (gen %d vs mount kind %d)",
             (int)vw->s.gen, (int)m->kind);
  }

  gb_session_core(m, vw_ok ? &vw->s : 0);

  /* MANDATORY: same clear-before-release bracket as pdna_gen12_show() above. */
  g_ro_tail = 0;
  g_ro_tail_slack = 0;
  g_tail_lent = false;
  g_ro_path = 0;

  app_arena_release();
  return gb_ro_exit_offer();
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
  app_box_resume_clear();   /* BACKLOG #188: a previous save/session's resume cell must not leak in */

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
      ed->origin_write_failed = false;      /* BACKLOG #172: fresh mount, fresh answer */
      ed->origin_cached = 0;
      g_ed = ed;
      gb_journal_session_open();            /* #234 s4: bind the recorder, open the journal, offer any unsaved steps */
    } else {
      log_line("gen12: edit session refused (%s, gen %d vs mount kind %d): read-only",
               gbs_status_text(st), ed->s.gen, (int)m->kind);
    }
  }

  gb_session_core(m, 0);   /* BACKLOG #64: no read-only session here either -- this
                            * entry shows every screen through g_ed when gbs_open
                            * succeeded above, or the plain read-only info page when
                            * it did not (same as before this lane) */

  if (g_ed) { gb_flush_on_exit(); app_gb_close(); }   /* #234 s4: the ONE exit confirm, then unbind the recorder */
  g_ed = 0;                                       /* the arena block is about to go */
  g_tail_lent = false;      /* U2b review 0b: the whole block goes away next line anyway,
                              * but a caller that checks the flag before that must see it
                              * cleared, not stale-true from this visit */
  app_arena_release();
  return GB12_ENTER_OK;
}

#endif /* PDNA_GEN12_HOST */
