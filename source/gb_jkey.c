/* SPDX-License-Identifier: GPL-3.0-or-later
 * gb_jkey.c -- see gb_jkey.h. Pure C. */
#include "gb_jkey.h"

#include <string.h>

#include "gb_fields.h"
#include "gb_trainer.h"
#include "journal.h"

#define GBJ_NAME_MAX 11u        /* the cart's name field is 11 bytes, 0x50-terminated */
#define GBJ_SID_MARK 0x4740u    /* 'G' + game: the SID slot's Game Boy marker         */

/* #308: 0xFF is the digit '9' in the Game Boy charset, but jrn_key64 canonicalizes on the FIRST 0xFF (the Gen-3 terminator), so
 * the legacy key below stops at the first '9' and two trainer names that differ only after one share a key. The fix is in
 * THIS layer (the journal core is frozen): a name that contains a 0xFF byte is hashed ESCAPED (0xFF -> 0xFE 0x01, 0xFE ->
 * 0xFE 0x00, which keeps the escape injective) and flagged by passing gender = 1 (a boolean the engine hashes as 0/1; the GB
 * layout never used it). A name WITHOUT a 0xFF byte hashes EXACTLY as before -- identical key, so nothing existing moves; only
 * names with a '9' get a new key, and jrnapp_open_gb_compat finds + adopts their old-keyed journal (jrn_app.c). */
static uint64_t gbj_key(const GbSession* s, int legacy) {
  uint8_t canon[2u * GBJ_NAME_MAX + 1u];
  uint32_t noff, ioff, n = 0, i, nlen = 0;
  uint16_t tid;
  uint8_t has_ff = 0, b;
  GbGame g;
  if (!s || !s->open || !s->img) return 0;
  g = gbt_game(s);
  noff = gbf_off(g, GBF_PLAYER_NAME);
  ioff = gbf_off(g, GBF_TRAINER_ID);
  if (!noff || !ioff || noff + GBJ_NAME_MAX > s->len || ioff + 2u > s->len) return 0;
  while (nlen < GBJ_NAME_MAX && s->img[noff + nlen] != 0x50u) { if (s->img[noff + nlen] == 0xFFu) has_ff = 1; nlen++; }
  if (legacy || !has_ff) {
    for (i = 0; i < nlen; i++) canon[n++] = s->img[noff + i];                      /* the plain name: the legacy bytes exactly */
    has_ff = 0;
  } else {
    for (i = 0; i < nlen; i++) {
      b = s->img[noff + i];
      if (b == 0xFFu) { canon[n++] = 0xFEu; canon[n++] = 0x01u; }
      else if (b == 0xFEu) { canon[n++] = 0xFEu; canon[n++] = 0x00u; }
      else canon[n++] = b;
    }
  }
  canon[n++] = 0xFFu;                                       /* the terminator the engine's canonicalization stops on */
  tid = (uint16_t)(((uint16_t)s->img[ioff] << 8) | s->img[ioff + 1u]);
  return jrn_key64(canon, (uint8_t)n, tid, (uint16_t)(GBJ_SID_MARK + (unsigned)g), has_ff, 0);
}

uint64_t gb_journal_key(const GbSession* s) { return gbj_key(s, 0); }
uint64_t gb_journal_key_legacy(const GbSession* s) { return gbj_key(s, 1); }

typedef struct { const char* tag; const char* name; } GbStepName;
static const GbStepName k_names[] = {
  { "edit", "Pokemon edit" },     { "view", "Pokemon edit" },     { "item", "Held item" },
  { "move", "Box move" },         { "release", "Release" },       { "release-all", "Release box" },
  { "boxname", "Box name" },      { "bag", "Bag" },               { "trainer", "Trainer" },
  { "dex", "Pokedex" },           { "gbflags", "Flags and counters" }, { "fly", "Fly destinations" },
  { "gbclock reset", "Clock" },   { "gbclock clear", "Clock" },   { "gbclock shift", "Clock" },
  { "daycare-take", "Day-Care" }, { "daycare-egg", "Day-Care" },  { "daycare-put", "Day-Care" },
  { "daycare-edit", "Day-Care" }, { "gb1 teleport", "Warp" },     { "gb1 teleport undo", "Warp" },
  { "hof edit", "Hall of Fame" }, { "hof clear", "Hall of Fame" }, { "hof setcount", "Hall of Fame" },
  { "hof add", "Hall of Fame" },  { "hof delete", "Hall of Fame" },
  { "paste", "Paste" },           { "dup", "Duplicate" },         { "create", "Create" },
  { "xferup", "Transfer up" },    { "xferdown", "Transfer down" }, { "bank-down", "Transfer down" },
  { "exit", "Save" }
};

const char* gb_step_name(const char* tag) {
  unsigned i;
  if (!tag) return "Edit";
  for (i = 0; i < sizeof k_names / sizeof k_names[0]; i++)
    if (strcmp(tag, k_names[i].tag) == 0) return k_names[i].name;
  return "Edit";
}
