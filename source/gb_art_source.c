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
 * every call site. Needed by both builds (gb_art_have()/gb_art_register() below read
 * and write them either way, even the delta stand-ins). */
static bool s_reg_have[3];      /* last gb_art_register() outcome for gen, sticky      */
static bool s_reg_checked[3];   /* gb_art_register() has run at least once for gen     */
static bool s_fb_have[3];       /* "beside the save" fallback result, THIS open save   */
static bool s_fb_checked[3];    /* fallback has been probed for the CURRENT save       */

/* ---- E3 review item 8: everything below that touches FatFs/SD is real ONLY outside
 * PDNA_DELTA (the emulator build has no SD card at all). Previously each function had
 * its OWN #ifdef PDNA_DELTA/#else, which left several SD-only helpers (gb_art_load_loc,
 * gb_art_save_loc, gb_art_read, gb_art_open_and_identify, gb_art_resolve_path) fully
 * compiled but UNREACHABLE under PDNA_DELTA -- four -Wunused-function warnings, and a
 * real function to keep in sync by hand every time one of the per-function ifdefs
 * changed. ONE block, so "no SD in the emulator build" is a single fact instead of
 * five scattered ones. */
#ifndef PDNA_DELTA

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

/* The one seek+read shim gb_art_open_and_identify()/gb_art_fetch() go through -- same
 * shape as pdna_gen12.c's file-local gb_read(), duplicated rather than shared because
 * that one is `static` to a different translation unit and this module deliberately
 * does not reach into pdna_gen12.c's arena-resident session state (see
 * gb_art_source.h: this is a parallel, independent path, not a refactor of tested
 * paste-path code). */
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
 * its ~3 KB own frame is not the hot-path budget gb_art_fetch() has to answer for.
 *
 * Deliberately does NOT call gb_art_save_loc() itself (E3 review, second pass): with
 * ONE function serving both a caller that wants the loc cached and one that does
 * not, gcc could no longer prove the `false` caller (gb_art_have's fallback probe)
 * never reaches the loc-file f_open/FAT-walk subtree, and the STATIC call-graph tool
 * (path-insensitive -- it sees "this function calls gb_art_save_loc", not "only when
 * the argument is true") charged every caller for it, undoing the very fix this
 * split exists to prove. Splitting the WRITE out to the caller's own frame makes it
 * true at the call-graph level, not just at runtime, that gb_art_have() never reaches
 * it. `out_loc` may be NULL (the fallback probe's case). */
static GbArtRegStatus __attribute__((noinline))
gb_art_open_and_identify(uint8_t gen, const char* path, RomGbSpriteLoc* out_loc) {
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

  if (out_loc) rom_gbsprite_save_loc(&gs, out_loc);
  return GB_ART_REG_OK;
}

GbArtRegStatus gb_art_register(uint8_t gen, const char* path) {
  if (gen != PDNA_GEN1 && gen != PDNA_GEN2) return GB_ART_REG_BAD_ROM;
  if (!path || !path[0]) {
    s_reg_have[gen] = false;
    s_reg_checked[gen] = true;
    pdna_origin_art_invalidate();  /* E3 review: unconditional -- clearing a registration
                                    * is itself a reason to drop a stale memo, same as
                                    * setting one */
    return GB_ART_REG_EMPTY;
  }
  RomGbSpriteLoc loc;
  GbArtRegStatus st = gb_art_open_and_identify(gen, path, &loc);
  s_reg_have[gen] = (st == GB_ART_REG_OK);
  s_reg_checked[gen] = true;
  if (s_reg_have[gen]) gb_art_save_loc(gen, &loc);   /* only this caller wants the cache */
  /* E3 review: unconditional, not just on success -- a FAILED re-registration still
   * means "whatever the memo remembers for this gen may no longer be true" (the old
   * ROM might already be gone/replaced even though the new one didn't validate). */
  pdna_origin_art_invalidate();
  if (st != GB_ART_REG_OK)
    log_line("gb art: gen%u registration failed (%d) for %s", (unsigned)gen, (int)st, path);
  return st;
}

bool gb_rom_path_beside(const char* save_path, uint8_t gen, char* out, int cap) {
  (void)gen;
  if (!save_path || !out || cap < 5) return false;
  int len = 0; while (save_path[len]) len++;
  int slash = -1, dot = -1;
  for (int i = 0; i < len; i++) { if (save_path[i] == '/') slash = i; if (save_path[i] == '.') dot = i; }
  int baselen = (dot > slash) ? dot : len;         /* strip the LAST extension only */
  /* E3 review item 7: REFUSE rather than clip -- a truncated base would probe a
   * DIFFERENT (likely nonexistent, or worse, some unrelated file's) path than the
   * one actually beside the save, and a false "found it" there is worse than
   * finding nothing. GB_ROM_PATH_MAX(128) is comfortably larger than any real save
   * path this app itself ever produces, so this is a defensive refusal, not an
   * expected outcome. */
  if (baselen > cap - 5) return false;             /* room for the longest extension + NUL */

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
}

bool gb_art_have(uint8_t gen) {
  if (gen != PDNA_GEN1 && gen != PDNA_GEN2) return false;
  if (s_reg_checked[gen] && s_reg_have[gen]) return true;   /* an explicit registration always wins */
  if (!app_current_save_is_gb()) return false; /* the fallback only applies to a raw GB session       */
  if (!s_fb_checked[gen]) {
    s_fb_checked[gen] = true;
    char path[GB_ROM_PATH_MAX];
    /* EXISTENCE ONLY -- deliberately does NOT call gb_art_open_and_identify() (that
     * needs its own FIL+RomGbSprite+2,048 B scratch, ~2,992 B) to fully validate the
     * sibling file here. `have()`'s own contract is "cheap; used to skip work", and
     * gb_art_fetch() is going to open+identify this exact path for real in a moment
     * anyway (rom_gbsprite_open_loc()/open() inline in its own frame) -- a bad or
     * wrong-generation sibling still degrades correctly there (returns NULL, exactly
     * the router's normal "no art" outcome), so a second, heavier validation here
     * bought nothing but stack: E3 review's call-graph tool found this nested INSIDE
     * gb_art_fetch's own frame (called from pdna_origin_art_have(), itself called
     * from pdna_origin_art_portrait() before fetch_pic()) reaching a deeply nested
     * UI chain (Bank -> box -> party strip -> mon menu -> Daycare -> inspect ->
     * summary -> portrait) hard enough to push main()'s worst path over the 11.5 KB
     * stack even after gb_art_fetch's OWN frame was already fixed. */
    s_fb_have[gen] = gb_rom_path_beside(app_current_save_path(), gen, path, (int)sizeof path);
  }
  return s_fb_have[gen];
}

/* ---- the actual fetch: gb_art_source.h's whole memory-budget discussion ------------
 * E3 review BLOCKING 1: decodes straight into mon_decomp itself, IN PLACE, instead of
 * a separate 3,928 B GbSprite -- rom_gbsprite_pic_buf() writes the indexed pixels at
 * mon_decomp+ROM_GBSPRITE_MAX_PIXELS (the codec's own work[] scratch immediately
 * after that), and rom_gbsprite_to_rgb15_inplace() expands them back into
 * mon_decomp+0 -- see rom_gbsprite.h for why that specific offset is safe. This is
 * now ONE noinline frame (FIL + RomGbSpriteLoc + RomGbSprite + the 2,048 B scan
 * window + the path buffer), not two: there is no more scratch GbSprite to give a
 * split any reason to exist. */
static bool gb_art_resolve_path(uint8_t gen, char* out, int cap) {
  const char* reg = app_gb_rom_path(gen);
  if (reg && reg[0]) {
    int i = 0; for (; reg[i] && i < cap - 1; i++) out[i] = reg[i]; out[i] = 0;
    return true;
  }
  if (app_current_save_is_gb()) return gb_rom_path_beside(app_current_save_path(), gen, out, cap);
  return false;
}

static const uint16_t* __attribute__((noinline))
gb_art_fetch(uint8_t gen, uint16_t dex, uint8_t form, uint8_t back, uint8_t shiny,
            uint8_t* out_w, uint8_t* out_h) {
  /* have() is re-checked in gb_art_pic_cb() (24 B own frame) now, not here (E3
   * re-verification "free partial") -- pdna_origin_art_portrait() already checked it
   * before ever reaching this call, so this was a redundant re-check paid for INSIDE
   * gb_art_fetch's own ~3.7 KB frame; the graph tool cannot tell "redundant" from
   * "load-bearing" and charged the whole gb_art_have() subtree against this frame's
   * total either way. Measured: 5,664 -> 5,552 B. */
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
  /* Re-save the loc whenever it does NOT already match this exact ROM -- not just
   * when the file was missing. E3 review item 4: a STALE loc (the file at `path`
   * was SWAPPED for a different ROM since it was cached) still makes have_loc true
   * (a loc file was read) even though rom_gbsprite_open_loc() silently fell back to
   * a full scan internally because its id_hash/size no longer matched -- the
   * original `!have_loc` check never caught that case, so a swapped ROM paid the
   * full-ROM scan cost on EVERY fetch, forever, with no way to self-heal. */
  if (ok && (!have_loc || loc.id_hash != gs.id_hash || loc.size != sz)) {
    RomGbSpriteLoc fresh;
    rom_gbsprite_save_loc(&gs, &fresh);
    gb_art_save_loc(gen, &fresh);
  }
  if (!ok || (uint8_t)gs.gen != gen) { f_close(&fil); return 0; }

  RomGbSide side = back ? ROM_GBSPRITE_BACK : ROM_GBSPRITE_FRONT;
  RomGbPic info;
  const uint16_t* px = 0;
  uint8_t* mdbuf = (uint8_t*)mon_decomp;
  /* E3 review BLOCKING 2: claim BEFORE the very first write below (rom_gbsprite_pic_buf
   * writes the indexed px/work region even on a later failure) -- see artbuf.h. This
   * is also what lets pdna_origin_art.c's fetch_pic() memo() capture a POST-fetch
   * epoch that already accounts for this call's own writes. */
  artbuf_claim();
  /* px at [ROM_GBSPRITE_MAX_PIXELS, +3,136), work right after at
   * [ROM_GBSPRITE_RGB15_BYTES, +784) -- ROM_GBSPRITE_RGB15_BYTES (6,272) IS
   * ROM_GBSPRITE_MAX_PIXELS*2, so this is "px, then work" back to back, both past
   * the RGB15 region rom_gbsprite_to_rgb15_inplace() writes below. The
   * _Static_assert pins the arithmetic so a future constant change fails the build
   * instead of silently corrupting mon_decomp's neighbour in EWRAM. */
  _Static_assert(ROM_GBSPRITE_RGB15_BYTES + GB_SPRITE_WORK <= MON_DECOMP_BYTES,
                 "GB art in-place layout no longer fits mon_decomp");
  if (rom_gbsprite_pic_buf(&gs, side, dex, form, mdbuf + ROM_GBSPRITE_MAX_PIXELS,
                           mdbuf + ROM_GBSPRITE_RGB15_BYTES, &info)) {
    uint16_t pal[4];
    if (rom_gbsprite_pal(&gs, dex, shiny ? 1 : 0, pal) &&
        rom_gbsprite_to_rgb15_inplace(mdbuf, ROM_GBSPRITE_MAX_PIXELS, info.w, info.h, pal)) {
      *out_w = info.w; *out_h = info.h;
      px = mon_decomp;
    }
  }
  f_close(&fil);
  return px;
}

#else /* PDNA_DELTA: no SD card at all -- every one of the above is unreachable, so
       * none of it is compiled. The public surface still has to exist (the vtable
       * below references gb_art_pic_cb/have_cb unconditionally), just as
       * permanently-off stand-ins. */

GbArtRegStatus gb_art_register(uint8_t gen, const char* path) {
  (void)path;
  if (gen != PDNA_GEN1 && gen != PDNA_GEN2) return GB_ART_REG_BAD_ROM;
  s_reg_have[gen] = false;
  s_reg_checked[gen] = true;
  pdna_origin_art_invalidate();
  return GB_ART_REG_CANT_OPEN;
}

bool gb_art_have(uint8_t gen) {
  (void)gen;
  return false;                                 /* no SD, and no open save to sit beside on this build */
}

bool gb_rom_path_beside(const char* save_path, uint8_t gen, char* out, int cap) {
  (void)save_path; (void)gen; (void)out; (void)cap;
  return false;
}

static const uint16_t* gb_art_fetch(uint8_t gen, uint16_t dex, uint8_t form, uint8_t back,
                                    uint8_t shiny, uint8_t* out_w, uint8_t* out_h) {
  (void)gen; (void)dex; (void)form; (void)back; (void)shiny; (void)out_w; (void)out_h;
  return 0;
}

#endif /* PDNA_DELTA */

/* ---- the PdnaGbArtSource vtable -- compiled in BOTH builds -------------------------- */
static const uint16_t* gb_art_pic_cb(void* ctx, uint8_t gen, uint16_t dex, uint8_t form,
                                     uint8_t back, uint8_t shiny, uint8_t* out_w, uint8_t* out_h) {
  (void)ctx;
  /* have() re-checked HERE (24 B own frame), not inside gb_art_fetch (E3
   * re-verification "free partial") -- see gb_art_fetch's own comment. */
  if (!gb_art_have(gen)) return 0;
  return gb_art_fetch(gen, dex, form, back, shiny, out_w, out_h);
}
static int gb_art_have_cb(void* ctx, uint8_t gen) { (void)ctx; return gb_art_have(gen) ? 1 : 0; }

/* E3 review re-verification: the real stack-headroom check. __iheap_start is the
 * SAME linker symbol gba_cart.ld places right after every static IWRAM .text/.data/
 * .bss section -- the low-water mark the user stack (which starts at __sp_usr, just
 * under IWRAM's top, and grows DOWN) can never cross without colliding with code/
 * data. `sp - __iheap_start` is therefore exactly the number of bytes of stack still
 * free at the moment this runs, matching this project's OWN post-link EWRAM-overflow
 * guard's use of the equivalent EWRAM symbol (__eheap_start vs the EWRAM top,
 * Makefile) -- same idiom, other end of memory. Valid in any GBA-target build
 * (delta included; harmless there since gb_art_have() is already false with no SD),
 * so this is not wrapped in #ifndef PDNA_DELTA.
 * ASSUMES AN EMPTY HEAP: newlib's heap grows UP from this same symbol, so the subtraction
 * is exact only while nothing has malloc'd. True today (0 malloc / ff_memalloc call sites;
 * the tree uses the integer-only siprintf family), but a future malloc or a %f-capable
 * printf would silently eat the ~550 B of slack between the 6,144-B need and the measured
 * 5,592-B fetch subtree. Re-measure with the call-graph tool if either ever appears.
 *
 * D4 (E4 review): `need` is now the CALLER's own measured requirement, not a fixed
 * 6,144 B baked in here -- pdna_origin_art.c's portrait router passes
 * PDNA_GB_FETCH_NEED for its GB-rung call sites and the smaller PDNA_G3X_FETCH_NEED
 * for the cross-game Gen-3 rung, so each rung gates on its own subtree. This function
 * itself only ever reads the CPU's SP; it has no opinion on what any rung needs. */
extern char __iheap_start[];
static int gb_art_stack_room(int need) {
  register char* sp __asm__("sp");
  return (sp - __iheap_start) > need;
}

void gb_art_boot_register(void) {
  static const PdnaGbArtSource src = { gb_art_pic_cb, gb_art_have_cb, 0 };
  pdna_origin_art_register(&src);
  pdna_origin_art_set_stack_room_hook(gb_art_stack_room);
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

/* Called once, from view_save() (pdna_main.c), the moment a NEW save is opened -- the
 * "beside the save" fallback is scoped to THAT save, so a stale hit/miss from the
 * PREVIOUS save must not leak into this one. Registered ROMs (s_reg_*) are untouched:
 * they don't depend on which save is open. Compiled in both builds: harmless
 * bookkeeping under PDNA_DELTA (gb_art_have() never reads s_fb_* there), and
 * view_save() calls it unconditionally either way. */
void gb_art_session_reset(void) {
  s_fb_have[PDNA_GEN1] = s_fb_have[PDNA_GEN2] = false;
  s_fb_checked[PDNA_GEN1] = s_fb_checked[PDNA_GEN2] = false;
}
