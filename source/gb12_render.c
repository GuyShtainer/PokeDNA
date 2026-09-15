/*
 * gb12_render — the Gen-1/2 -> Gen-3-box-record display ladder (BACKLOG #150 S150-2).
 * See gb12_render.h for the contract. Pure C, no tonc/sys.h: this whole file dual-
 * compiles on the host (tests/host_gb12render_test.c) exactly as gen12_convert.c does.
 *
 * Moved out of pdna_gen12.c's gb_presentation (pdna_gen12.c:211-228), placeholder_pid
 * (:235-245) and gb_build_slot's tail (:259-311) on main 2161d74 -- see gb12_render.h's
 * header comment for why (a second caller, the native Bank cell, needs the identical
 * ladder byte-for-byte).
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "gb12_render.h"
#include "gen3_edit.h"   /* gen3_build_mon, em_set_egg, em_set_nickname, gen3_edit_load/commit */

/* NUL-terminated working copy of a name field. Verbatim twin of pdna_gen12.c's own
 * copy_z (pdna_gen12.c:115), which stays where it is -- both TUs need this and neither
 * is allowed to depend on the other for it (decision 1). */
static void copy_z(char* dst, const char* src, int cap) {
  int i = 0;
  for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
  dst[i] = 0;
}

int gb12_presentation(const Gb12Mon* in, Gb12Result r) {
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

/* Deterministic personality for a placeholder record. `id_salt` folds in what used to
 * be the separate (box, slot) pair (decision 3): box = id_salt / 20, slot = id_salt %
 * 20, so a GB-grid caller passing box*20+slot reproduces the exact byte sequence the
 * pre-refactor function hashed and every GB session's placeholder PID is unchanged.
 * Not the converter's identity hash (that one is private to gen12_convert.c and only
 * defined for records that convert); this only has to be STABLE for a given slot,
 * because the clipboard and the bank match mons by their first 8 bytes. FNV-1a over
 * an explicit byte list -- never over struct memory, whose padding is uninitialised. */
static uint32_t placeholder_pid(uint32_t id_salt, const Gb12Mon* in) {
  uint32_t h = 2166136261u;
  const uint8_t seq[8] = {
    (uint8_t)(id_salt / 20u), (uint8_t)(id_salt % 20u),
    (uint8_t)in->species_dex, (uint8_t)(in->species_dex >> 8),
    (uint8_t)in->ot_id, (uint8_t)(in->ot_id >> 8),
    in->level, in->gen
  };
  for (int i = 0; i < 8; i++) { h ^= seq[i]; h *= 16777619u; }
  return h ? h : 1u;                         /* 0 would read as an empty slot */
}

int gb12_render_rec(const Gb12Mon* in, const Gb12Target* tgt, uint32_t id_salt,
                    uint8_t rec80[80], uint8_t* reason) {
  memset(rec80, 0, 80);                      /* decision 2: this function's own obligation now */

  /* decision 4: one copy, one invariant -- Gb12Mon.slot_salt and the placeholder PID
   * can never be fed two different values because both read off `&m`. */
  Gb12Mon m = *in;
  m.slot_salt = id_salt;

  Gb12Result r = gen12_can_convert(&m);
  *reason = (uint8_t)r;
  int how = gb12_presentation(&m, r);

  if (how == GB_SHOW_FULL) {
    if (gen12_convert(&m, tgt, rec80, 0) == GB12_OK) return how;
    memset(rec80, 0, 80);                    /* documented-unreachable; fail visible, not wrong */
    *reason = (uint8_t)GB12_ERR_PID;
    how = (m.species_dex >= 1 && m.species_dex <= 251) ? GB_SHOW_PLACEHOLDER : GB_SHOW_NONE;
  }

  if (how == GB_SHOW_RELAXED) {
    Gb12Mon relaxed = m;
    bool was_egg = m.is_egg;
    relaxed.is_egg = false;
    relaxed.held_item = 0;
    if (gen12_convert(&relaxed, tgt, rec80, 0) == GB12_OK) {
      if (was_egg) {                         /* draw it as the Egg it really is */
        EditMon e;
        gen3_edit_load(rec80, false, &e);
        em_set_egg(&e, true);
        gen3_edit_commit(&e, rec80);
      }
      return how;
    }
    memset(rec80, 0, 80);
    how = (m.species_dex >= 1 && m.species_dex <= 251) ? GB_SHOW_PLACEHOLDER : GB_SHOW_NONE;
  }

  if (how == GB_SHOW_PLACEHOLDER) {
    /* A stand-in of the right species and (clamped) level carrying the GB nickname,
     * so the cell reads as the Pokemon the player remembers. It is NOT the real mon
     * — its IVs/moves are gen3_build_mon's defaults — which is exactly why the copy
     * veto below is unconditional for every non-OK reason. */
    char otname[sizeof m.ot_name], nick[sizeof m.nickname];
    uint8_t lv = m.level;
    if (lv < 1) lv = 1;
    if (lv > 100) lv = 100;
    copy_z(otname, m.ot_name, (int)sizeof otname);
    copy_z(nick, m.nickname, (int)sizeof nick);
    uint8_t metgame = (tgt->met_game >= 1 && tgt->met_game <= 15) ? tgt->met_game : 3;
    gen3_build_mon(m.species_dex, lv, placeholder_pid(id_salt, &m),
                   (uint32_t)m.ot_id, otname, metgame, rec80);
    if (nick[0]) {
      EditMon e;
      gen3_edit_load(rec80, false, &e);
      em_set_nickname(&e, nick);
      gen3_edit_commit(&e, rec80);
    }
    return how;
  }
  memset(rec80, 0, 80);                      /* GB_SHOW_NONE */
  return how;
}
