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
 * Does NOT call gbs_finish() -- see gbdex_set's own header comment on why: the dex
 * screen's bulk Catch/See/Wipe-ALL op can call this up to 386 times in one user
 * gesture, and gbs_write_field's own contract is explicit that a caller doing many
 * field writes calls gbs_finish() ONCE after the last one, not after each ("so N field
 * edits cost one whole-file reparse instead of N"). */
static GbsStatus dex_set_bit(GbSession* s, GbGame g, GbField f, uint16_t dex, bool val) {
  uint32_t off = gbf_off(g, f);
  uint16_t len = gbf_len(g, f);
  if (!off || !len) return GBS_ERR_ARG;
  int i = (int)dex - 1, byte = i / 8, bit = i % 8;
  if ((uint16_t)byte >= len) return GBS_ERR_ARG;
  uint8_t cur;
  if (gbs_read_field(s, off + (uint32_t)byte, &cur, 1) != GBS_OK) return GBS_ERR_ARG;
  uint8_t next = val ? (uint8_t)(cur | (1u << bit)) : (uint8_t)(cur & ~(1u << bit));
  if (next == cur) return GBS_OK;   /* untouched is untouched, nothing to write */
  return gbs_write_field(s, off + (uint32_t)byte, &next, 1);
}

/* ---- D4 (BACKLOG #87 fix pass, DO-NOT-SHIP review): the Unown-dex GATE ------------
 *
 * STATUSFLAGS_UNOWN_DEX_F (bit 1 of wStatusFlags, GBF_STATUS_FLAGS) gates whether the
 * Pokedex screen even offers Unown's special multi-form display at all, and
 * wFirstUnownSeen (GBF_FIRST_UNOWN_SEEN) is the 1-based letter (1=A..26=Z, 0 = never
 * met one) Pokedex_LoadSelectedMonTiles uses to pick which Unown sprite tile to draw
 * for national dex #201 -- 0 there `dec a`'s to 255 and indexes an out-of-range
 * frontpic pointer. Neither byte is touched by gbdex_get/gbdex_set's own owned/seen
 * bitfields or by gbdex_unown_set's own wUnownDex list -- they are a SEPARATE gate.
 * R3 (b87 fix pass 2, correction): UpdateUnownDex (engine/pokedex/unown_dex.asm:1-19)
 * touches NEITHER byte -- it only maintains wUnownDex itself. The battle code writes
 * wFirstUnownSeen (engine/battle/core.asm:3444-3453 and :8199-8205, both guarded
 * "only while still 0", the exact rule first_unown_seen_write_if_zero below copies),
 * and wStatusFlags bit 1 is an ENGINE FLAG (data/events/engine_flags.asm:29,
 * ENGINE_UNOWN_DEX) set by a map script, not by any dex/battle routine. The only
 * direct set/res of that bit in the decomp are DebugRoomMenu_PokedexDex (seed) and
 * DebugRoomMenu_PokedexClr (clear) (engine/debug/debug_room.asm:336/:356), which this
 * code mirrors. An editor that can mark dex #201 seen or record an Unown letter
 * WITHOUT going through an actual in-game encounter must keep this gate in sync by
 * hand, the same way those two debug-room routines do. */
#define STATUSFLAGS_UNOWN_DEX_BIT 1

static GbsStatus status_flags_set_unown_bit(GbSession* s, GbGame g, bool on) {
  uint32_t off = gbf_off(g, GBF_STATUS_FLAGS);
  if (!off) return GBS_ERR_ARG;
  uint8_t cur;
  if (gbs_read_field(s, off, &cur, 1) != GBS_OK) return GBS_ERR_ARG;
  uint8_t next = on ? (uint8_t)(cur | (1u << STATUSFLAGS_UNOWN_DEX_BIT))
                    : (uint8_t)(cur & (uint8_t)~(1u << STATUSFLAGS_UNOWN_DEX_BIT));
  if (next == cur) return GBS_OK;
  return gbs_write_field(s, off, &next, 1);
}

static bool first_unown_seen_is_zero(const GbSession* s, GbGame g) {
  uint32_t off = gbf_off(g, GBF_FIRST_UNOWN_SEEN);
  if (!off) return false;
  uint8_t cur;
  GbSession* ncs = (GbSession*)(const void*)s;
  if (gbs_read_field(ncs, off, &cur, 1) != GBS_OK) return false;
  return cur == 0;
}

static GbsStatus first_unown_seen_write_if_zero(GbSession* s, GbGame g, uint8_t val) {
  uint32_t off = gbf_off(g, GBF_FIRST_UNOWN_SEEN);
  if (!off) return GBS_ERR_ARG;
  uint8_t cur;
  if (gbs_read_field(s, off, &cur, 1) != GBS_OK) return GBS_ERR_ARG;
  if (cur != 0) return GBS_OK;   /* already recorded -- the FIRST seen letter never changes */
  return gbs_write_field(s, off, &val, 1);
}

/* Does NOT call gbs_finish() -- BATCHED like gb_bag.c's own multi-field set does, one
 * level up: this call alone may touch up to 2 bytes (its own bit + the invariant's
 * companion bit); a caller doing MANY gbdex_set/gbdex_unown_set calls in one user
 * gesture (the dex screen's per-cell A-press, or its Catch/See/Wipe-ALL bulk op) must
 * call gbs_finish() itself exactly once after the LAST one (pdna_gbdex.c does this on
 * screen exit, matching gbs_write_field's own documented batching contract). Gen 1
 * sessions are unaffected either way: gbs_write_field's Gen-1 path already re-verifies
 * the whole image on EVERY call regardless (gb_session.h's own doc), and gbs_finish()
 * is a no-op for Gen 1 -- this only changes Gen 2's cost, from O(edits) whole-file
 * reparses to O(1). */
GbsStatus gbdex_set(GbSession* s, uint16_t dex, bool owned, bool on) {
  if (!s || !s->open || dex < 1) return GBS_ERR_ARG;
  if (dex > gb_max_species(s->gen)) return GBS_ERR_ARG;
  GbGame g = dex_game(s);
  GbField primary = owned ? GBF_DEX_OWNED : GBF_DEX_SEEN;
  if (!gbf_off(g, primary)) return GBS_ERR_ARG;

  GbsStatus st = dex_set_bit(s, g, primary, dex, on);
  if (st != GBS_OK) return st;

  /* Caught-implies-seen (header comment): setting owned=on=true also forces seen on;
   * clearing seen (owned=false, on=false) also forces owned off. Only one of these two
   * conditions can ever be true for a given call (owned's two values are mutually
   * exclusive), so at most one extra byte moves. */
  if (owned && on)        st = dex_set_bit(s, g, GBF_DEX_SEEN,  dex, true);
  else if (!owned && !on) st = dex_set_bit(s, g, GBF_DEX_OWNED, dex, false);
  if (st != GBS_OK) return st;

  /* D4 (BACKLOG #87 fix pass, DO-NOT-SHIP review): national dex #201 is Unown on
   * Gen 2 -- marking it seen/unseen directly (bypassing an actual in-game
   * encounter) must keep the SEPARATE Unown-dex gate (wStatusFlags bit 1 /
   * wFirstUnownSeen, see the header comment above unown_read) in sync, or the
   * game itself renders an out-of-range frontpic the next time dex entry #201 is
   * opened. Gen 1 never reaches here: dex 201 > gb_max_species(GB_GEN1)=151
   * already refused at the top of this function. */
  if (dex == 201 && !owned) {
    if (on) {
      /* Marking Unown seen: DebugRoomMenu_PokedexDex's own behaviour -- a save
       * that has never recorded ANY Unown letter gets seeded with UNOWN A
       * (letter 0), the same append+gate path a real encounter takes. A save
       * that already has a recorded letter keeps its letter/wUnownDex as-is,
       * but the gate bit itself must still come back: without this else, a
       * Wipe-ALL-then-Undo round trip (clear #201's seen bit, which clears the
       * gate via the branch below, then set it again) left the gate
       * permanently OFF on any save that had ever met an Unown -- R1 (b87 fix
       * pass 2, mutation-proven: Gold 0x43->0x41->0x41, Crystal 0xc3->0xc1-
       * >0xc1, should have returned to 0x43/0xc3). */
      if (first_unown_seen_is_zero(s, g)) {
        GbsStatus ust = gbdex_unown_set(s, 0, true);   /* UNOWN A -- also re-arms the gate */
        if (ust != GBS_OK && ust != GBS_ERR_FULL) return ust;
      } else {
        /* A letter is already on record: re-marking #201 seen must put the gate bit
         * back, or the clear half below is a one-way door (Wipe ALL + Undo would
         * silently disable the player's own Unown Pokedex page). */
        GbsStatus gst = status_flags_set_unown_bit(s, g, true);
        if (gst != GBS_OK) return gst;
      }
    } else {
      /* Clearing Unown seen: DebugRoomMenu_PokedexClr's own behaviour -- just the
       * gate bit, per the brief's own scope (wUnownDex/wFirstUnownSeen are left
       * as-is, matching what the debug tool itself does). */
      GbsStatus cst = status_flags_set_unown_bit(s, g, false);
      if (cst != GBS_OK) return cst;
    }
  }
  return GBS_OK;
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

/* R2 (b87 fix pass 2, DO-NOT-SHIP review): see gb_dex.h's own header comment on
 * why a caller needs the raw, ORDERED list (not just per-letter membership) to
 * detect a genuine change. unown_read already does exactly this read. */
bool gbdex_unown_list(const GbSession* s, uint8_t out[UNOWN_SLOTS]) {
  return out && unown_read(s, NULL, out);
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

  bool will_be_empty = false;
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
    will_be_empty = true;
    for (int i = 0; i < UNOWN_SLOTS; i++) if (slots[i] != 0) { will_be_empty = false; break; }
  }

  /* Does NOT call gbs_finish() -- same batching contract as gbdex_set above; the
   * caller finishes once after its own last gbdex-family call. */
  GbsStatus wst = gbs_write_field(s, off, slots, UNOWN_SLOTS);
  if (wst != GBS_OK) return wst;

  /* D4: a successful append records the Unown-dex GATE too -- exactly what
   * UpdateUnownDex does itself the moment a wild Unown is caught/seen (bit 1 of
   * wStatusFlags always ends up set; wFirstUnownSeen is written only the very
   * first time, never overwritten by a later letter). Emptying the list all the
   * way back out (the last letter removed) clears the gate bit again -- the
   * editor-only mirror of DebugRoomMenu_PokedexClr's own behaviour. */
  if (on) {
    (void)first_unown_seen_write_if_zero(s, g, want);
    (void)status_flags_set_unown_bit(s, g, true);
  } else if (will_be_empty) {
    (void)status_flags_set_unown_bit(s, g, false);
  }
  return GBS_OK;
}
