#include <stddef.h>    /* NULL */

#include "gb_dex.h"
#include "gb_edit.h"   /* gb_max_species */

/* Same shape as gb_fly.c's gbfy_game -- deliberately its own small lookup rather than
 * depending on gb_trainer.h, so this pure-C core stays a two-file (header+field-table)
 * dependency. */
static GbGame dex_game(const GbSession* s) {
  if (!s) return GBF_G_RED;
  if (s->gen == GB_GEN1) return GBF_G_RED;   /* Yellow == Red/Blue layout */
  return (s->g2w.sv.version == G2_VER_CRYSTAL) ? GBF_G_CRYSTAL : GBF_G_GS;
}

bool gbdex_get(const GbSession* s, uint16_t dex, bool owned) {
  if (!s || !s->open || dex < 1) return false;
  if (dex > gb_max_species(s->gen)) return false;
  GbGame g = dex_game(s);
  GbField f = owned ? GBF_DEX_OWNED : GBF_DEX_SEEN;
  uint32_t off = gbf_off(g, f);
  uint16_t len = gbf_len(g, f);
  if (!off || !len) return false;
  int i = (int)dex - 1, byte = i / 8, bit = i % 8;
  if ((uint16_t)byte >= len) return false;
  uint8_t cur;
  GbSession* ncs = (GbSession*)(const void*)s;
  if (gbs_read_field(ncs, off + (uint32_t)byte, &cur, 1) != GBS_OK) return false;
  return (cur & (1u << bit)) != 0;
}

/* Set exactly one (field, dex) bit to `val`, writing only if it actually changes.
 * Returns GBS_OK (no-op) / GBS_OK (wrote, caller still owes gbs_finish) / an error --
 * callers batch this with gbdex_set's own second call before finishing once. */
static GbsStatus dex_set_bit(GbSession* s, GbGame g, GbField f, uint16_t dex, bool val,
                             bool* wrote) {
  uint32_t off = gbf_off(g, f);
  uint16_t len = gbf_len(g, f);
  if (!off || !len) return GBS_ERR_ARG;
  int i = (int)dex - 1, byte = i / 8, bit = i % 8;
  if ((uint16_t)byte >= len) return GBS_ERR_ARG;
  uint8_t cur;
  if (gbs_read_field(s, off + (uint32_t)byte, &cur, 1) != GBS_OK) return GBS_ERR_ARG;
  uint8_t next = val ? (uint8_t)(cur | (1u << bit)) : (uint8_t)(cur & ~(1u << bit));
  if (next == cur) return GBS_OK;   /* untouched is untouched */
  GbsStatus st = gbs_write_field(s, off + (uint32_t)byte, &next, 1);
  if (st != GBS_OK) return st;
  *wrote = true;
  return GBS_OK;
}

GbsStatus gbdex_set(GbSession* s, uint16_t dex, bool owned, bool on) {
  if (!s || !s->open || dex < 1) return GBS_ERR_ARG;
  if (dex > gb_max_species(s->gen)) return GBS_ERR_ARG;
  GbGame g = dex_game(s);
  GbField primary = owned ? GBF_DEX_OWNED : GBF_DEX_SEEN;
  if (!gbf_off(g, primary)) return GBS_ERR_ARG;

  bool wrote = false;
  GbsStatus st = dex_set_bit(s, g, primary, dex, on, &wrote);
  if (st != GBS_OK) return st;

  /* Caught-implies-seen (header comment): setting owned=on=true also forces seen on;
   * clearing seen (owned=false, on=false) also forces owned off. Only one of these two
   * conditions can ever be true for a given call (owned's two values are mutually
   * exclusive), so at most one extra byte moves. */
  if (owned && on) {
    st = dex_set_bit(s, g, GBF_DEX_SEEN, dex, true, &wrote);
    if (st != GBS_OK) return st;
  } else if (!owned && !on) {
    st = dex_set_bit(s, g, GBF_DEX_OWNED, dex, false, &wrote);
    if (st != GBS_OK) return st;
  }

  if (!wrote) return GBS_OK;
  return gbs_finish(s);
}

/* ---- Unown forms (Gen 2 only): wUnownDex, an order-of-first-seen list, not a
 * per-letter bitfield -- see gb_dex.h's header comment for the full citation. */

#define UNOWN_SLOTS 26

static bool unown_read(const GbSession* s, GbGame* g_out, uint8_t slots[UNOWN_SLOTS]) {
  if (!s || !s->open) return false;
  GbGame g = dex_game(s);
  uint32_t off = gbf_off(g, GBF_UNOWN_DEX);
  uint16_t len = gbf_len(g, GBF_UNOWN_DEX);
  if (!off || len != UNOWN_SLOTS) return false;   /* absent on Gen 1 (off==0) */
  GbSession* ncs = (GbSession*)(const void*)s;
  if (gbs_read_field(ncs, off, slots, UNOWN_SLOTS) != GBS_OK) return false;
  if (g_out) *g_out = g;
  return true;
}

bool gbdex_unown_seen(const GbSession* s, int letter) {
  if (letter < 0 || letter > 25) return false;
  uint8_t slots[UNOWN_SLOTS];
  if (!unown_read(s, NULL, slots)) return false;
  uint8_t want = (uint8_t)(letter + 1);
  for (int i = 0; i < UNOWN_SLOTS; i++) if (slots[i] == want) return true;
  return false;
}

GbsStatus gbdex_unown_set(GbSession* s, int letter, bool on) {
  if (letter < 0 || letter > 25) return GBS_ERR_ARG;
  GbGame g;
  uint8_t slots[UNOWN_SLOTS];
  if (!unown_read(s, &g, slots)) return GBS_ERR_ARG;
  uint32_t off = gbf_off(g, GBF_UNOWN_DEX);
  uint8_t want = (uint8_t)(letter + 1);

  int found = -1;
  for (int i = 0; i < UNOWN_SLOTS; i++) if (slots[i] == want) { found = i; break; }

  if (on) {
    if (found >= 0) return GBS_OK;   /* already present, no-op */
    int empty = -1;
    for (int i = 0; i < UNOWN_SLOTS; i++) if (slots[i] == 0) { empty = i; break; }
    if (empty < 0) return GBS_ERR_FULL;   /* all 26 slots taken by 26 distinct letters */
    slots[empty] = want;
  } else {
    if (found < 0) return GBS_OK;   /* not present, no-op */
    /* compact: shift everything after `found` left by one, zero the freed tail slot */
    for (int i = found; i < UNOWN_SLOTS - 1; i++) slots[i] = slots[i + 1];
    slots[UNOWN_SLOTS - 1] = 0;
  }

  GbsStatus st = gbs_write_field(s, off, slots, UNOWN_SLOTS);
  if (st != GBS_OK) return st;
  return gbs_finish(s);
}
