/* gb_art_source.c — see gb_art_source.h. The GB half of pdna_origin_art's
 * PdnaGbArtSource vtable, and rom_gbsprite.c's first real caller. */
#include <string.h>
#include <tonc.h>

#include "ff.h"
#include "log.h"
#include "pdna_app.h"
#include "artbuf.h"           /* mon_decomp, MON_DECOMP_BYTES */
#include "pdna_origin_art.h"  /* PdnaGbArtSource, PDNA_GEN1/2, pdna_origin_art_register */
#include "rom_gbsprite.h"
#include "gb_art_source.h"

/* ---- per-gen state -----------------------------------------------------------------
 * All plain (non-EWRAM) statics -- a handful of bytes each, the same "goes to IWRAM's
 * .bss, negligible against the ~11.5 KB stack" convention pdna_origin_art.c's own
 * s_gb/s_gb_on/memo fields already use. Indexed by `gen` directly (1 or 2); index 0 is
 * wasted on purpose, same call as sprite_era.h's SeRoms -- clearer than gen-1 math at
 * every call site. */
static bool s_reg_have[3];      /* last gb_art_register() outcome for gen, sticky      */
static bool s_reg_checked[3];   /* gb_art_register() has run at least once for gen     */
static bool s_fb_have[3];       /* "beside the save" fallback result, THIS open save   */
static bool s_fb_checked[3];    /* fallback has been probed for the CURRENT save       */

/* ---- the location cache file: /PokeDNA/gbart1.loc / gbart2.loc ---------------------
 * One RomGbSpriteLoc (~260 B) per gen slot, so rom_gbsprite_open_loc() can skip the
 * whole-ROM scan on every fetch after the first. Read fresh from the file at EVERY
 * fetch rather than held resident (two 260 B copies would eat half of the 524 B EWRAM
 * budget for a cache that is only a performance optimisation) -- see gb_art_source.h's
 * memory note. Self-validating: rom_gbsprite_open_loc() rejects a stale/foreign loc via
 * its own id_hash+size check and falls back to a full scan, so a corrupt or wrong-ROM
 * cache file degrades to "slow" rather than "wrong", and this is a plain (unverified)
 * f_write -- like config.cfg's own philosophy, there is no reason for a regenerable,
 * self-checked cache to be the one exception. */
static void gb_art_loc_path(uint8_t gen, char* out, int cap) {
  siprintf(out, "%.*s/gbart%u.loc", cap - 12, PDNA_DIR, (unsigned)gen);
}

static bool gb_art_load_loc(uint8_t gen, RomGbSpriteLoc* out) {
  char path[40];
  gb_art_loc_path(gen, path, (int)sizeof path);
  FIL f; UINT br = 0;
  if (f_open(&f, path, FA_READ) != FR_OK) return false;
  FRESULT fr = f_read(&f, out, (UINT)sizeof *out, &br);
  f_close(&f);
  return fr == FR_OK && br == sizeof *out;
}

static void gb_art_save_loc(uint8_t gen, const RomGbSpriteLoc* loc) {
  if (!app_can_edit()) return;                    /* Everdrive/read-only: don't even try */
  char path[40];
  gb_art_loc_path(gen, path, (int)sizeof path);
  FIL f; UINT bw = 0;
  if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return;
  f_write(&f, loc, (UINT)sizeof *loc, &bw);
  f_close(&f);
  if (bw != sizeof *loc) log_line("gb art: loc cache write short for gen%u", (unsigned)gen);
}

/* The one seek+read shim gb_art_validate()/gb_art_fetch() go through -- same shape as
 * pdna_gen12.c's file-local gb_read(), duplicated rather than shared because that one
 * is `static` to a different translation unit and this module deliberately does not
 * reach into pdna_gen12.c's arena-resident session state (see gb_art_source.h: this is
 * a parallel, independent path, not a refactor of tested paste-path code). */
static bool gb_art_read(void* ctx, uint32_t off, void* buf, uint32_t len) {
  FIL* f = (FIL*)ctx;
  UINT br = 0;
  if (!f || !buf) return false;
  if (f_lseek(f, (FSIZE_t)off) != FR_OK) return false;
  if (f_read(f, buf, (UINT)len, &br) != FR_OK) return false;
  return br == len;
}

/* ---- open + identify: shared by an explicit Settings registration and the lazy
 * "beside the save" probe. FIL (600 B) + RomGbSprite (~336 B) + the 2,048 B scan
 * window rom_gbsprite_open()/open_loc() both require unconditionally -- see
 * gb_art_source.h's memory note for why this cannot be smaller. Called rarely (once
 * per Settings action, once per boot, once per save-open's first fallback probe), so
 * its ~3 KB own frame is not the hot-path budget gb_art_fetch() has to answer for. */
static GbArtRegStatus __attribute__((noinline))
gb_art_validate(uint8_t gen, const char* path, bool save_loc_on_success) {
  FIL fil;
  memset(&fil, 0, sizeof fil);
  if (f_open(&fil, path, FA_READ) != FR_OK) return GB_ART_REG_CANT_OPEN;

  FSIZE_t fsz = f_size(&fil);
  uint32_t sz = (fsz > (FSIZE_t)0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)fsz;

  RomGbSprite gs;
  uint8_t scratch[ROM_GBSPRITE_SCRATCH_MIN];
  int ok = rom_gbsprite_open(&gs, gb_art_read, &fil, sz, scratch, (uint32_t)sizeof scratch);
  f_close(&fil);
  if (!ok) return GB_ART_REG_BAD_ROM;
  if ((uint8_t)gs.gen != gen) return GB_ART_REG_WRONG_GEN;

  if (save_loc_on_success) {
    RomGbSpriteLoc loc;
    rom_gbsprite_save_loc(&gs, &loc);
    gb_art_save_loc(gen, &loc);
  }
  return GB_ART_REG_OK;
}

GbArtRegStatus gb_art_register(uint8_t gen, const char* path) {
  if (gen != PDNA_GEN1 && gen != PDNA_GEN2) return GB_ART_REG_BAD_ROM;
  if (!path || !path[0]) {
    s_reg_have[gen] = false;
    s_reg_checked[gen] = true;
    return GB_ART_REG_EMPTY;
  }
#ifdef PDNA_DELTA
  (void)0;                                    /* no SD in the emulator build          */
  s_reg_have[gen] = false; s_reg_checked[gen] = true;
  return GB_ART_REG_CANT_OPEN;
#else
  GbArtRegStatus st = gb_art_validate(gen, path, true);
  s_reg_have[gen] = (st == GB_ART_REG_OK);
  s_reg_checked[gen] = true;
  if (s_reg_have[gen])
    pdna_origin_art_invalidate();             /* a different ROM may be behind the same gen */
  if (st != GB_ART_REG_OK)
    log_line("gb art: gen%u registration failed (%d) for %s", (unsigned)gen, (int)st, path);
  return st;
#endif
}

/* Called once, from view_save() (pdna_main.c), the moment a NEW save is opened -- the
 * "beside the save" fallback is scoped to THAT save, so a stale hit/miss from the
 * PREVIOUS save must not leak into this one. Registered ROMs (s_reg_*) are untouched:
 * they don't depend on which save is open. */
void gb_art_session_reset(void) {
  s_fb_have[PDNA_GEN1] = s_fb_have[PDNA_GEN2] = false;
  s_fb_checked[PDNA_GEN1] = s_fb_checked[PDNA_GEN2] = false;
}

bool gb_art_have(uint8_t gen) {
  if (gen != PDNA_GEN1 && gen != PDNA_GEN2) return false;
  if (s_reg_checked[gen] && s_reg_have[gen]) return true;   /* an explicit registration always wins */
#ifdef PDNA_DELTA
  return false;                               /* no SD, and no open save to sit beside on this build */
#else
  if (!app_current_save_is_gb()) return false; /* the fallback only applies to a raw GB session       */
  if (!s_fb_checked[gen]) {
    s_fb_checked[gen] = true;
    char path[GB_ROM_PATH_MAX];
    s_fb_have[gen] = gb_rom_path_beside(app_current_save_path(), gen, path, (int)sizeof path) &&
                     gb_art_validate(gen, path, true) == GB_ART_REG_OK;
  }
  return s_fb_have[gen];
#endif
}

/* ---- the actual fetch: gb_art_source.h's whole memory-budget discussion ------------
 * Split into two noinline frames purely for readability -- see the header comment:
 * they still coexist on the stack while gb_art_decode() runs, so this does NOT reduce
 * the peak the PR's -fstack-usage numbers report, only which locals live where. */
static const uint16_t* __attribute__((noinline))
gb_art_decode(RomGbSprite* gs, uint16_t dex, uint8_t form, uint8_t back, uint8_t shiny,
             uint8_t* out_w, uint8_t* out_h) {
  GbSprite spr;                               /* 3,928 B -- module A's decode buffer   */
  RomGbPic info;
  RomGbSide side = back ? ROM_GBSPRITE_BACK : ROM_GBSPRITE_FRONT;
  if (!rom_gbsprite_pic(gs, side, dex, form, &spr, &info)) return 0;
  uint16_t pal[4];
  if (!rom_gbsprite_pal(gs, dex, shiny ? 1 : 0, pal)) return 0;
  if (!rom_gbsprite_to_rgb15(&spr, pal, mon_decomp, (uint32_t)(MON_DECOMP_BYTES / 2))) return 0;
  *out_w = info.w; *out_h = info.h;
  return mon_decomp;
}

static bool gb_art_resolve_path(uint8_t gen, char* out, int cap) {
  const char* reg = app_gb_rom_path(gen);
  if (reg && reg[0]) {
    int i = 0; for (; reg[i] && i < cap - 1; i++) out[i] = reg[i]; out[i] = 0;
    return true;
  }
#ifndef PDNA_DELTA
  if (app_current_save_is_gb()) return gb_rom_path_beside(app_current_save_path(), gen, out, cap);
#endif
  return false;
}

static const uint16_t* __attribute__((noinline))
gb_art_fetch(uint8_t gen, uint16_t dex, uint8_t form, uint8_t back, uint8_t shiny,
            uint8_t* out_w, uint8_t* out_h) {
#ifdef PDNA_DELTA
  (void)gen; (void)dex; (void)form; (void)back; (void)shiny; (void)out_w; (void)out_h;
  return 0;
#else
  if (!gb_art_have(gen)) return 0;

  char path[GB_ROM_PATH_MAX];
  if (!gb_art_resolve_path(gen, path, (int)sizeof path)) return 0;

  FIL fil;
  memset(&fil, 0, sizeof fil);
  if (f_open(&fil, path, FA_READ) != FR_OK) return 0;
  FSIZE_t fsz = f_size(&fil);
  uint32_t sz = (fsz > (FSIZE_t)0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)fsz;

  RomGbSpriteLoc loc;
  bool have_loc = gb_art_load_loc(gen, &loc);

  RomGbSprite gs;
  uint8_t scratch[ROM_GBSPRITE_SCRATCH_MIN];
  int ok = rom_gbsprite_open_loc(&gs, gb_art_read, &fil, sz, scratch, (uint32_t)sizeof scratch,
                                 have_loc ? &loc : 0);
  if (ok && !have_loc) {                      /* a fresh scan just ran -- cache it */
    RomGbSpriteLoc fresh;
    rom_gbsprite_save_loc(&gs, &fresh);
    gb_art_save_loc(gen, &fresh);
  }
  if (!ok || (uint8_t)gs.gen != gen) { f_close(&fil); return 0; }

  const uint16_t* px = gb_art_decode(&gs, dex, form, back, shiny, out_w, out_h);
  f_close(&fil);
  return px;
#endif
}

/* ---- the PdnaGbArtSource vtable ------------------------------------------------- */
static const uint16_t* gb_art_pic_cb(void* ctx, uint8_t gen, uint16_t dex, uint8_t form,
                                     uint8_t back, uint8_t shiny, uint8_t* out_w, uint8_t* out_h) {
  (void)ctx;
  return gb_art_fetch(gen, dex, form, back, shiny, out_w, out_h);
}
static int gb_art_have_cb(void* ctx, uint8_t gen) { (void)ctx; return gb_art_have(gen) ? 1 : 0; }

void gb_art_boot_register(void) {
  static const PdnaGbArtSource src = { gb_art_pic_cb, gb_art_have_cb, 0 };
  pdna_origin_art_register(&src);
#ifndef PDNA_DELTA
  for (uint8_t gen = PDNA_GEN1; gen <= PDNA_GEN2; gen++) {
    const char* p = app_gb_rom_path(gen);
    if (p && p[0]) {
      GbArtRegStatus st = gb_art_register(gen, p);
      if (st != GB_ART_REG_OK)
        log_line("gb art: gen%u boot re-registration failed (%d)", (unsigned)gen, (int)st);
    }
  }
#endif
}

bool gb_rom_path_beside(const char* save_path, uint8_t gen, char* out, int cap) {
  (void)gen;
#ifdef PDNA_DELTA
  (void)save_path; (void)out; (void)cap;
  return false;
#else
  if (!save_path || !out || cap < 5) return false;
  int len = 0; while (save_path[len]) len++;
  int slash = -1, dot = -1;
  for (int i = 0; i < len; i++) { if (save_path[i] == '/') slash = i; if (save_path[i] == '.') dot = i; }
  int baselen = (dot > slash) ? dot : len;         /* strip the LAST extension only */
  if (baselen > cap - 5) baselen = cap - 5;        /* room for the longest extension + NUL */

  static const char* const kExt[2] = { ".gb", ".gbc" };
  for (int e = 0; e < 2; e++) {
    memcpy(out, save_path, (size_t)baselen);
    int p = baselen;
    for (int i = 0; kExt[e][i] && p < cap - 1; i++) out[p++] = kExt[e][i];
    out[p] = 0;
    FIL f;
    if (f_open(&f, out, FA_READ) == FR_OK) { f_close(&f); return true; }
  }
  return false;
#endif
}
