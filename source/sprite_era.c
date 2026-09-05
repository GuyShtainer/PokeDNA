#include "sprite_era.h"

#include <string.h>

/* Config tokens -- see sprite_era.h's "config.cfg text" section. Kept private: the
 * public se_*_name() functions are UI labels and are free to read differently. Kind and
 * era tokens are all exactly 2 chars except "native", which is never WRITTEN (it is the
 * default, skipped by se_config_write) but IS matched on read, so it stays in the table
 * at its enum index for se_config_apply's linear scan. */
static const char* const KIND_CFG[SE_KIND_N]  = { "rs", "em", "fr", "g1", "g2" };
static const char* const PLACE_CFG[SE_PLACE_N] = { "pc", "party", "sum", "bank", "gbgrid" };
static const char* const ERA_CFG[SE_ERA_N]     = { "native", "g1", "g2", "rs", "em", "fr" };

static const char* const KIND_UI[SE_KIND_N]   = { "RS", "EM", "FRLG", "GEN1", "GEN2" };
static const char* const PLACE_UI[SE_PLACE_N] = { "PC", "PARTY", "SUMMARY", "BANK", "GBGRID" };
static const char* const ERA_UI[SE_ERA_N]     = { "Native", "Gen1", "Gen2", "RS", "EM", "FRLG" };

void se_default(SeSetting* s) {
  if (!s) return;
  for (int k = 0; k < SE_KIND_N; k++)
    for (int p = 0; p < SE_PLACE_N; p++)
      s->era[k][p] = (uint8_t)SE_ERA_NATIVE;
}

SeEra se_native_era(SeSaveKind kind, uint8_t origin_gen, uint8_t origin_certain) {
  (void)origin_certain;  /* already folded into origin_gen -- see the header comment */
  /* Unsigned-cast bounds check: an enum's underlying type is signed on the host build
   * but UNSIGNED under arm-none-eabi (ARM EABI default), where a plain `kind < 0` is a
   * compile-time-false comparison and a warning. Casting to unsigned makes an
   * out-of-range value (however it arose) wrap to a huge positive and get caught by the
   * single `>=` below on either platform. */
  if ((unsigned)kind >= SE_KIND_N) kind = SE_KIND_EM; /* defensive: never abort on a
                                                        * bad enum, degrade to a save
                                                        * that always exists */
  if (kind == SE_KIND_GEN1) return SE_ERA_GEN1;
  if (kind == SE_KIND_GEN2) return SE_ERA_GEN2;
  if (origin_gen == 1) return SE_ERA_GEN1;
  if (origin_gen == 2) return SE_ERA_GEN2;
  switch (kind) {
    case SE_KIND_RS:   return SE_ERA_G3_RS;
    case SE_KIND_FRLG: return SE_ERA_G3_FRLG;
    default:           return SE_ERA_G3_EM;  /* SE_KIND_EM, and the fallback above */
  }
}

bool se_species_exists(SeEra era, uint16_t national_dex) {
  switch (era) {
    case SE_ERA_NATIVE:                                     return true;
    case SE_ERA_GEN1:                                        return national_dex >= 1 && national_dex <= 151;
    case SE_ERA_GEN2:                                        return national_dex >= 1 && national_dex <= 251;
    case SE_ERA_G3_RS: case SE_ERA_G3_EM: case SE_ERA_G3_FRLG:
      return national_dex >= 1 && national_dex <= 386;
    default: return false;  /* out-of-range era: no picture, not even a guess */
  }
}

/* Is `era`'s ROM registered? NATIVE is always "available" -- it is the fallback
 * sentinel, not something with its own cartridge (see sprite_era.h). An out-of-range
 * era is defensively unavailable rather than indexing past the array. */
static bool era_has_rom(SeEra era, const SeRoms* roms) {
  if (era == SE_ERA_NATIVE) return true;
  if ((unsigned)era >= SE_ERA_N || !roms) return false;
  return roms->have[era];
}

SeEra se_resolve(const SeSetting* s, SeSaveKind kind, SePlace place,
                  uint8_t origin_gen, uint8_t origin_certain, uint16_t national_dex,
                  const SeRoms* roms, bool compiled_gen3, int* reason) {
  if (!s || !roms) { if (reason) *reason = SE_WHY_CHIP; return SE_ERA_NATIVE; }
  if ((unsigned)kind >= SE_KIND_N) kind = SE_KIND_EM;
  if ((unsigned)place >= SE_PLACE_N) place = SE_PLACE_SUMMARY;

  SeEra wanted = (SeEra)s->era[kind][place];
  int why = SE_WHY_WANTED;

  /* Step 1: PC_GRID + GEN1 is refused outright -- no per-species Gen-1 icons exist. */
  if (place == SE_PLACE_PC && wanted == SE_ERA_GEN1) {
    wanted = SE_ERA_NATIVE;
    why = SE_WHY_NO_SPECIES;
  }

  if (wanted != SE_ERA_NATIVE) {
    /* Step 2/3: the wanted era must both have this species and have its ROM open. */
    if (!se_species_exists(wanted, national_dex)) {
      why = SE_WHY_NO_SPECIES;
    } else if (!era_has_rom(wanted, roms)) {
      why = SE_WHY_NO_ROM;
    } else {
      if (reason) *reason = SE_WHY_WANTED;
      return wanted;                                   /* Step 4: draw it as asked */
    }
  }

  /* Fell to NATIVE (either the cell said so, or WANTED just failed above). */
  SeEra native = se_native_era(kind, origin_gen, origin_certain);
  if (era_has_rom(native, roms)) {
    if (reason) *reason = why;                          /* Step 5 */
    return native;
  }

  /* Step 6/7: native's own ROM is ALSO absent -- compiled Gen-3 art, else the chip. */
  if (reason) *reason = compiled_gen3 ? SE_WHY_COMPILED : SE_WHY_CHIP;
  return SE_ERA_NATIVE;
}

/* Bounded string append: copies as much of `src` as fits in `out[0..cap-1]`, always
 * leaving room for (and writing) a trailing NUL when cap >= 1. Returns the number of
 * bytes actually copied (never counting the NUL). A `src` that would not fit ENTIRELY
 * is not partially copied -- callers rely on that to keep config lines whole. */
static int append_all_or_nothing(char* out, int cap, const char* src) {
  int len = 0;
  while (src[len]) len++;
  if (cap <= len) return 0;              /* would not fit (cap<=0 also lands here) */
  memcpy(out, src, (size_t)len);
  out[len] = 0;
  return len;
}

int se_config_write(const SeSetting* s, char* out, int cap) {
  if (!s || !out || cap <= 0) return 0;
  int pos = 0;
  bool stop = false;
  out[0] = 0;
  for (int k = 0; k < SE_KIND_N && !stop; k++) {
    for (int p = 0; p < SE_PLACE_N && !stop; p++) {
      SeEra e = (SeEra)s->era[k][p];
      if (e == SE_ERA_NATIVE) continue;

      char line[24];
      int n = 0;
      n += append_all_or_nothing(line + n, (int)sizeof(line) - n, "era_");
      n += append_all_or_nothing(line + n, (int)sizeof(line) - n, KIND_CFG[k]);
      n += append_all_or_nothing(line + n, (int)sizeof(line) - n, "_");
      n += append_all_or_nothing(line + n, (int)sizeof(line) - n, PLACE_CFG[p]);
      n += append_all_or_nothing(line + n, (int)sizeof(line) - n, "=");
      n += append_all_or_nothing(line + n, (int)sizeof(line) - n, ERA_CFG[e]);
      n += append_all_or_nothing(line + n, (int)sizeof(line) - n, "\n");

      int room = append_all_or_nothing(out + pos, cap - pos, line);
      if (room == 0) { stop = true; break; }  /* would overflow -- stop for good, in
                                                * iteration order, so truncation is
                                                * always a clean whole-line prefix */
      pos += room;
    }
  }
  return pos;
}

bool se_config_apply(SeSetting* s, const char* key, const char* value) {
  if (!key || strncmp(key, "era_", 4) != 0) return false;
  const char* kpart = key + 4;

  int kind = -1;
  for (int i = 0; i < SE_KIND_N; i++)
    if (kpart[0] == KIND_CFG[i][0] && kpart[1] == KIND_CFG[i][1]) { kind = i; break; }
  if (kind < 0 || kpart[2] != '_') return false;

  const char* ppart = kpart + 3;
  int place = -1;
  for (int i = 0; i < SE_PLACE_N; i++)
    if (strcmp(ppart, PLACE_CFG[i]) == 0) { place = i; break; }
  if (place < 0) return false;

  int era = (int)SE_ERA_NATIVE;
  if (value)
    for (int i = 0; i < SE_ERA_N; i++)
      if (strcmp(value, ERA_CFG[i]) == 0) { era = i; break; }

  if (s) s->era[kind][place] = (uint8_t)era;
  return true;
}

const char* se_era_name(SeEra e)       { return (unsigned)e < SE_ERA_N   ? ERA_UI[e]   : "?"; }
const char* se_kind_name(SeSaveKind k) { return (unsigned)k < SE_KIND_N  ? KIND_UI[k]  : "?"; }
const char* se_place_name(SePlace p)   { return (unsigned)p < SE_PLACE_N ? PLACE_UI[p] : "?"; }

SeEra se_era_next(SeEra e, const SeRoms* roms, SePlace place) {
  if ((unsigned)e >= SE_ERA_N) e = SE_ERA_NATIVE;
  SeEra cur = e;
  for (int i = 0; i < SE_ERA_N; i++) {          /* bounded: at most SE_ERA_N steps */
    cur = (SeEra)((cur + 1) % SE_ERA_N);
    if (place == SE_PLACE_PC && cur == SE_ERA_GEN1) continue;
    if (era_has_rom(cur, roms)) return cur;
  }
  return SE_ERA_NATIVE;                         /* unreachable: NATIVE always qualifies */
}
