/*
 * Gen-1/2 Pokedex + (Gen 2) Unown-forms screens (BACKLOG #87 item 3). See
 * pdna_gbdex.h for the caller contract and the UX-parity rule (reuses pdna_pick.c's
 * shared pdna_dex_screen() UNCHANGED -- this file is only the GB wiring).
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_gbdex.h"
#include "gb_dex.h"
#include "gb_edit.h"       /* gb_max_species */
#include "pdna_pick.h"     /* pdna_dex_screen, pdna_dex_set_max, pdna_dex_set_cell_art */
#include "pdna_gen12.h"    /* gb_persist, gb12_arena_tail(_release) -- BACKLOG #208's cache */
#include "pdna_trainer.h"  /* trainer_flag_row_paint / trainer_key_legend             */
#include "pdna_app.h"      /* app_confirm                                            */
#include "pdna_origin_art.h" /* BACKLOG #124/#196: pdna_origin_art_icon,
                              * pdna_origin_art_portrait_by_dex, pdna_origin_cell_render */
#include "dex_cell_art_rule.h" /* BACKLOG #124: the pure-C selection rule this callback gates on */
#include "dex_gbart_cache.h" /* BACKLOG #208: the pure-C FIFO cache core (key/find/claim)       */
#include "log.h"           /* BACKLOG #208: PDNA_DELTA-only "dexart: page N fetch=X hit=Y" */
#include "sys.h"           /* BACKLOG #208: EWRAM_BSS -- the cache's own head/count/pointer must
                             * not cost IWRAM .bss (the stack ceiling reads that, not EWRAM) */
#include "ui.h"
#include "snd.h"

/* ---- BACKLOG #124/#196: the dex screen's cell-art override, GB ROM sprites --------
 * The dex grid's cell painter (dex_cell_grid(), pdna_pick.c, shared verbatim with the
 * Gen-3 dex) draws through mon_icon_for()/the icon store by default. That is a
 * PokeDNA-internal icon set, not the species' own Gen-1/2 ROM art -- the box grid two
 * screens away already shows the REAL Gen-2 party-menu icon for a GB-era cell
 * (pdna_origin_art.c's pdna_origin_box_art(), ERA_GEN2 branch, called from
 * pdna_box.c:~1343's era_cell_draw()). This installs the SAME fetch, reused (not
 * copied): pdna_origin_art_icon() is a public entry point in pdna_origin_art.c
 * that factors that branch's own have()/stack-room/fetch/pack sequence
 * (pdna_origin_art_have(PDNA_GEN2) -> pdna_origin_art_stack_room(PDNA_GB_ICON_NEED) ->
 * fetch_pic_ex(...,icon=1,...) -> s_gb.icon() -> gb_art_icon_cb() ->
 * gb_art_fetch_icon()) into ONE function pdna_origin_box_art() now also calls, so the
 * dex is that chain's SECOND caller, never a second implementation.
 *
 * Reached through pdna_pick.h's pdna_dex_set_cell_art() process-wide override (its own
 * comment there has the full contract) rather than a new pdna_dex_screen() parameter,
 * because a parameter would touch the Gen-3 caller's (pdna_main.c's) call site for a
 * feature it never uses.
 *
 * BACKLOG #196: Gen 1 gets its OWN GB rung now too, through pdna_origin_art_portrait_
 * by_dex() (pdna_origin_art.h) -- the front-pic TWIN of pdna_origin_art_icon(), NOT
 * pdna_origin_art_portrait() (the portrait router used for an OWNED box/party/summary
 * mon): that router's era decision needs a real PkMon carrying a provable Gen-1/2-
 * import signature and a registered sprite_era.h SE_PLACE_*, neither of which a bare
 * national dex number (this screen's whole input) has -- era_cell_blit() (pdna_box.c)
 * can lean on pdna_origin_art_portrait() because it is handed a REAL owned PkMon from
 * g_box[]; this screen never has one, it draws every species 1..151/251 regardless of
 * whether the player owns it. See pdna_origin_art_portrait_by_dex()'s own header
 * comment in pdna_origin_art.h for why that needed a genuinely new entry point rather
 * than reusing the router.
 *
 * NEITHER generation leaks into the other's session (a Gen-1 session must never draw
 * a Kanto species from a separately-registered Gen-2 ROM's icon, or vice versa): both
 * generations install this same override UNCONDITIONALLY at their own call site in
 * pdna_gbdex() below, and gbdex_cell_art()'s own per-cell gate (via dex_cell_art_
 * source(), which sees the SESSION'S OWN generation and the have() answer for THAT
 * SAME generation, never the other one) is the actual safety boundary, not caller
 * discipline -- both generations' ROMs may be registered at once (Settings tolerates
 * it, exactly like the box grid does for imports of both eras in one save). */

/* Split exactly like pdna_box.c's era_cell_draw()/era_cell_blit(): the scale+blit
 * buffer (2,048 B for a 32x32 RGB15 cell) must never coexist on the stack with the
 * fetch chain's own frame (gb_art_fetch_icon's ~5.6 KB tail, gated on
 * PDNA_GB_ICON_NEED=6,144) -- noinline so an inlined copy cannot silently merge the
 * two frames back together. Called only AFTER the fetch (icon or portrait-by-dex)
 * has already returned and popped, or for any geometry the cache below does not
 * cover (nothing calls this with w/h != 32 today, but it stays the honest fallback). */
static bool __attribute__((noinline))
gbdex_cell_blit(const PdnaArt* a, int x, int y, int w, int h) {
  u16 cell[32 * 32];             /* 2,048 B of STACK -- never a static, never EWRAM */
  if (w <= 0 || h <= 0 || w > 32 || h > 32) return false;   /* validate: the only
                                                             * caller passes 32x32
                                                             * today, but this buffer
                                                             * cannot cover more */
  if (!pdna_origin_cell_render(a, cell, w, h)) return false;
  ui_sprite(x, y, w, h, cell);
  return true;
}

/* ---- BACKLOG #208: a REAL per-page GB-art cache -----------------------------------
 * The #196 review instrumented the naive ladder above (every cell, every repaint,
 * unconditionally) at 21 MISS / 0 HIT on a cold page, a row scroll, scrolling back
 * AND a same-page repaint -- an 11-slot FIFO/LRU over a 21-cell page evicts exactly
 * the next cell needed, on every one of those access patterns. The fix is capacity,
 * not a smarter policy: a plain FIFO whose slot count is >= one page (21 cells, the
 * grid's own vrows*cols) cannot exhibit that pathology -- a same-page repaint asks
 * for nothing new (0 evictions, 0 misses) and a one-row scroll only evicts the
 * cells that just scrolled OFF screen (the 7 oldest, list order matches paint
 * order under the default No.-sort filter).
 *
 * STORAGE. Both generations share ONE record shape (2-byte key + 1,024-byte
 * payload = 1,026 B/slot) rather than the two different per-generation formats a
 * fully space-optimal design would use (Gen 1's 1,026 B/slot giving 23 slots, Gen
 * 2's smaller 512 B/slot giving 46) -- a session is always ONE generation for its
 * whole visit (GbSession.gen never changes mid-visit), so the two formats are never
 * live at once, and a single fixed layout means gb12_arena_tail() is borrowed with
 * ONE constant NEED regardless of which generation is open, with no per-generation
 * branch in the borrow/release pair. 23 slots (DEXCACHE_SLOTS * sizeof(DexArtSlot)
 * = 23,598 B) still clears the review's own bar (>= one 21-cell page) for BOTH
 * generations, just with less spare margin for Gen 2 than a two-tier design would
 * have bought -- documented, not hidden, in the BACKLOG #208 report.
 *
 * GEN 1 (portrait_by_dex): the record holds the FINAL RENDERED 32x32 CELL, not the
 * raw ROM sprite (which is up to 56x56 RGB15 = 6,272 B, far past this slot's
 * budget) -- caching post-render also means a HIT skips pdna_origin_cell_render's
 * own resample work, not just the fetch. 1 B/px: a Gen-1 sprite's opaque pixels are
 * R=G=B by construction (4 DMG greys through the same RGB15 expansion every other
 * Gen-1 picture in this codebase uses), so `0x80 | v` (v = the 5-bit grey, always
 * nonzero as an encoded byte) exactly reconstructs the RGB15 pixel; transparent
 * pixels are the literal byte 0. cache_insert_gen1() SCANS FIRST and refuses to
 * cache the whole cell if any opaque pixel is not grey (rule 7: never trust an
 * invariant across a module boundary silently) -- that dex number then simply
 * misses every time, the same "not enough memory right now" posture every other
 * gate in this file already has, never a corrupted cached picture.
 *
 * GEN 2 (icon): the record holds the RAW 16x16 NATIVE ROM ICON verbatim RGB15 (512
 * of the slot's 1,024 payload bytes; the rest is zeroed and never read) -- Gen-2
 * icons are real 4-colour GBC palette pictures, not grey, so they cannot be
 * quantised the way Gen 1's cell is. A HIT still calls pdna_origin_cell_render()
 * (it only centres a source already <= the destination cell -- see that function's
 * own header comment) -- the cache buys back the FETCH (an SD read on the real
 * cart), not the cheap centring loop.
 *
 * LIFETIME. Borrowed once per pdna_gbdex() VISIT (gb12_arena_tail(), "one slice at a
 * time" -- see that function's own header comment for why nothing else can hold it
 * at the same time: the dex screen is the only screen on the stack while it is
 * open), not re-borrowed per repaint -- a per-repaint re-borrow would immediately
 * fail (g_tail_lent already true) and disable the cache on its own second use. This
 * is narrower than "cleared on pdna_origin_art_invalidate() and on place change"
 * asks for, which is fine: a cache whose whole lifetime is one screen visit is
 * cleared at LEAST as often as either of those events could ever fire while that
 * screen is open (neither a ROM re-registration nor a place change can happen while
 * the dex screen has focus), so no separate hook into either signal is needed. If
 * the borrow fails (should never happen; defensive) every cell just misses forever
 * -- s_cache.slots stays NULL, dexcache_find()/dexcache_claim() (dex_gbart_cache.h)
 * short-circuit to "always miss, insert disabled" on their own.
 *
 * The FIFO bookkeeping itself (key packing, find, claim/evict) is BACKLOG #208's
 * pure-C core, source/dex_gbart_cache.{c,h} -- host-tested directly
 * (tests/host_dexgbartcache_test.c) without a GBA build. This file only owns the
 * GBA-specific half: borrowing the arena, and packing/unpacking pixels into the
 * generic DexArtSlot.payload bytes that module never itself looks inside. */
#define DEXCACHE_SLOTS 23   /* 23 * 1,026 = 23,598 B <= the arena tail's 23,744 B slack */
#define DEXCACHE_NEED ((uint32_t)DEXCACHE_SLOTS * (uint32_t)sizeof(DexArtSlot))
/* BACKLOG #208 fixes review D3: this file's own comment two lines up cites the
 * arena tail's slack (23,744 B) as a bare number -- pinned here against pdna_gen12.h's
 * GB12_ARENA_TAIL_SLACK so a future DEXCACHE_SLOTS bump (or a shrink of the arena
 * tail itself) that no longer fits fails the BUILD, not a hand review. */
_Static_assert(DEXCACHE_NEED <= GB12_ARENA_TAIL_SLACK,
               "BACKLOG #208's dex art cache no longer fits the GB12 arena tail");

/* EWRAM_BSS (same review posture as pdna_pick.c's s_cell_art): a plain file static
 * here would be IWRAM .bss, which the stack budget's ceiling counts against; EWRAM
 * does not and has slack for it (896 B free per the current EWRAM ok line). */
static EWRAM_BSS DexArtCache s_cache;

#ifdef PDNA_DELTA
/* BACKLOG #208 step 1: a PDNA_DELTA-only tally, reset once per full repaint
 * (pdna_pick.c's dex_page_begin hook) and printed as the PREVIOUS page's line the
 * next time a page begins (plus a final flush at visit exit for the last page --
 * see gbdex_dex_page_flush_last()). Compiled out entirely in every other variant:
 * no counter storage, no log_line call, matching this codebase's existing
 * PDNA_DELTA-only test-hook posture (bank_plant.h/xfer_plant.h).
 *
 * BACKLOG #208 fixes review D4: EWRAM_BSS, not a plain file static -- these three
 * scalars are PDNA_DELTA-only (never compiled into the stack-budgeted variants at
 * all), but the delta build has its OWN stack ceiling this file's s_cache already
 * follows the same posture for (see s_cache's comment above); a plain static here
 * would cost IWRAM .bss instead, which that ceiling counts against. */
static EWRAM_BSS int      s_page_no;
static EWRAM_BSS uint32_t s_page_fetch, s_page_hit;
#endif

static bool cache_lookup_gen1(uint16_t dex, u16 cell[32 * 32]) {
  int slot = dexcache_find(&s_cache, dexcache_key(PDNA_GEN1, dex));
  if (slot < 0) return false;
  const uint8_t* p = s_cache.slots[slot].payload;
  for (int i = 0; i < 32 * 32; i++) {
    uint8_t b = p[i];
    cell[i] = b ? (u16)(0x8000u | (b & 0x1Fu) | ((uint16_t)(b & 0x1Fu) << 5) |
                        ((uint16_t)(b & 0x1Fu) << 10))
               : 0u;
  }
  return true;
}

static void cache_insert_gen1(uint16_t dex, const u16 cell[32 * 32]) {
  for (int i = 0; i < 32 * 32; i++) {          /* validate FIRST: refuse the whole
                                                 * cell rather than cache a lie */
    uint16_t px = cell[i];
    if (!(px & 0x8000u)) continue;
    uint16_t r = px & 0x1Fu, g = (px >> 5) & 0x1Fu, b = (px >> 10) & 0x1Fu;
    if (r != g || g != b) return;
  }
  int slot = dexcache_claim(&s_cache, dexcache_key(PDNA_GEN1, dex));
  if (slot < 0) return;                        /* cache disabled this visit */
  uint8_t* p = s_cache.slots[slot].payload;
  for (int i = 0; i < 32 * 32; i++) {
    uint16_t px = cell[i];
    p[i] = (px & 0x8000u) ? (uint8_t)(0x80u | (px & 0x1Fu)) : 0u;
  }
}

static bool cache_lookup_gen2(uint16_t dex, u16 icon16[16 * 16]) {
  int slot = dexcache_find(&s_cache, dexcache_key(PDNA_GEN2, dex));
  if (slot < 0) return false;
  memcpy(icon16, s_cache.slots[slot].payload, 16 * 16 * sizeof(u16));
  return true;
}

static void cache_insert_gen2(uint16_t dex, const u16 icon16[16 * 16]) {
  int slot = dexcache_claim(&s_cache, dexcache_key(PDNA_GEN2, dex));
  if (slot < 0) return;                        /* cache disabled this visit */
  memcpy(s_cache.slots[slot].payload, icon16, 16 * 16 * sizeof(u16));
  memset(s_cache.slots[slot].payload + 16 * 16 * sizeof(u16), 0,
         DEXCACHE_PAYLOAD - 16 * 16 * sizeof(u16));   /* the unused half: deterministic, never read */
}

/* Gen 1 (BACKLOG #208 fixes review D1): the review found the ORIGINAL single dispatcher
 * kept the 2,048 B `cell` buffer live across the fetch call (pdna_origin_art_portrait_
 * by_dex(), whose own chain runs up to PDNA_GB_ICON_NEED=6,144 B under pdna_origin_art_
 * stack_room()) -- the dex chain went 2,728 -> 4,808 B, 2,104 B of the fetch's own
 * headroom spent on a buffer the fetch itself never touches. Splitting fetch/hit/render
 * into THREE separate noinline frames restores gbdex_cell_blit's own header-comment
 * invariant (the 2 KB buffer never coexists with the fetch chain's frame): the
 * dispatcher below only ever has ONE of the three frames open on the stack at a time. */
static bool __attribute__((noinline))
gbdex_fetch_gen1(uint16_t dex, PdnaArt* a) {
  return pdna_origin_art_portrait_by_dex(dex, a) && a->px;
}

static bool __attribute__((noinline))
gbdex_hit_gen1(uint16_t dex, int x, int y) {
  u16 cell[32 * 32];                          /* this frame's cell buffer never overlaps
                                                * gbdex_fetch_gen1's frame -- HIT never
                                                * calls the fetch at all */
  if (!cache_lookup_gen1(dex, cell)) return false;
  ui_sprite(x, y, 32, 32, cell);
  return true;
}

static bool __attribute__((noinline))
gbdex_render_gen1(uint16_t dex, const PdnaArt* a, int x, int y) {
  u16 cell[32 * 32];                          /* called AFTER gbdex_fetch_gen1 has
                                                * already returned and popped -- never
                                                * live at the same time as its frame */
  if (!pdna_origin_cell_render(a, cell, 32, 32)) return false;
  cache_insert_gen1(dex, cell);
  ui_sprite(x, y, 32, 32, cell);
  return true;
}

static bool
gbdex_cell_art_gen1(uint16_t dex, int x, int y, int w, int h) {
  if (w != 32 || h != 32) {                 /* the record format is fixed to the
                                             * grid's own cell size; nothing calls
                                             * this with any other geometry today */
    PdnaArt a;
    return gbdex_fetch_gen1(dex, &a) && gbdex_cell_blit(&a, x, y, w, h);
  }
  if (gbdex_hit_gen1(dex, x, y)) {
#ifdef PDNA_DELTA
    s_page_hit++;
#endif
    return true;
  }
#ifdef PDNA_DELTA
  s_page_fetch++;
#endif
  PdnaArt a;
  if (!gbdex_fetch_gen1(dex, &a)) return false;
  return gbdex_render_gen1(dex, &a, x, y);
}

/* Gen 2 (BACKLOG #208 fixes review D1): same frame-isolation reasoning as Gen 1 above --
 * the HIT path's raw-icon buffer gets its OWN noinline frame (gbdex_hit_gen2), never
 * live across pdna_origin_art_icon()'s own fetch frame on a MISS. A HIT reconstructs the
 * raw 16x16 icon into a LOCAL buffer and still calls pdna_origin_cell_render() (the
 * cache buys back the fetch, not the centring -- see this cache's own header comment);
 * a MISS fetches, caches the raw icon (only when it really is the expected 16x16 native
 * shape -- rule 7), then blits through the ordinary gbdex_cell_blit() path unchanged.
 * NOTE: a `false` from gbdex_hit_gen2 conflates "not cached" with "cached but the blit
 * was refused" (gbdex_cell_blit() can itself fail its own geometry/render validation) --
 * both fall through to the fetch below, which is safe (a spurious refetch, never a
 * corrupted picture) but worth knowing if the fetch tally ever runs higher than the
 * miss count alone would predict. */
static bool __attribute__((noinline))
gbdex_hit_gen2(uint16_t dex, int x, int y, int w, int h) {
  u16 icon16[16 * 16];
  if (!cache_lookup_gen2(dex, icon16)) return false;
  PdnaArt a; memset(&a, 0, sizeof a);
  a.px = icon16; a.w = 16; a.h = 16; a.gen = PDNA_GEN2;
  return gbdex_cell_blit(&a, x, y, w, h);
}

static bool
gbdex_cell_art_gen2(uint16_t dex, int x, int y, int w, int h) {
  if (gbdex_hit_gen2(dex, x, y, w, h)) {
#ifdef PDNA_DELTA
    s_page_hit++;
#endif
    return true;
  }
#ifdef PDNA_DELTA
  s_page_fetch++;
#endif
  PdnaArt a;
  if (!(pdna_origin_art_icon(dex, &a) && a.px)) return false;
  if (a.w == 16 && a.h == 16) cache_insert_gen2(dex, a.px);
  return gbdex_cell_blit(&a, x, y, w, h);
}

#ifdef PDNA_DELTA
/* BACKLOG #208 step 1: the page-begin hook installed via pdna_dex_set_page_begin()
 * (pdna_pick.h) -- fires once per full repaint, BEFORE that repaint's dex_cell_grid()
 * calls (see pdna_pick.c's dex_declare_page()). Prints the tally the PREVIOUS page
 * just finished (page 0 has none), then starts counting the new one. The very last
 * page's tally never gets a "next page begins" call to flush it from inside this
 * hook -- gbdex_dex_page_flush_last() below covers that at visit exit. */
static void gbdex_dex_page_begin(void) {
  if (s_page_no > 0)
    log_line("dexart: page %d fetch=%lu hit=%lu", s_page_no,
             (unsigned long)s_page_fetch, (unsigned long)s_page_hit);
  s_page_no++;
  s_page_fetch = 0; s_page_hit = 0;
}

static void gbdex_dex_page_flush_last(void) {
  if (s_page_no > 0)
    log_line("dexart: page %d fetch=%lu hit=%lu", s_page_no,
             (unsigned long)s_page_fetch, (unsigned long)s_page_hit);
}
#endif

/* The ONE place `session_gen && gb_have` is computed -- gbdex_cell_art()'s own per-cell
 * gate below AND pdna_gbdex()'s two install sites (which need the SAME answer at the
 * PAGE level, for pdna_dex_set_cell_art()'s `serves_page` -- see that function's own
 * header comment in pdna_pick.h for why it must not be re-derived from a live
 * pdna_origin_art_have() read: that global is boot-sticky and independent per era, so
 * a Gen-1 session with a Gen-2 ROM ALSO registered would wrongly answer "yes" from
 * have(GEN2) alone). One implementation, never two definitions of the same question
 * that could quietly re-diverge. BACKLOG #196: `have` is now computed for THIS
 * SESSION'S OWN generation (GEN1 for a GB_GEN1 session, GEN2 for a GB_GEN2 one) --
 * the exact per-generation discipline dex_cell_art_rule.h's own header comment
 * requires of every caller. */
static bool gbdex_serves_dex(const GbSession* s) {
  if (!s) return false;
  uint8_t want_gen = (s->gen == GB_GEN1) ? (uint8_t)PDNA_GEN1
                    : (s->gen == GB_GEN2) ? (uint8_t)PDNA_GEN2 : 0;
  bool have = want_gen && pdna_origin_art_have(want_gen);
  return dex_cell_art_serves_page((int)s->gen, have);
}

/* The PdnaDexCellArtFn itself: `ctx` is the GbSession* the caller is visiting (never
 * NULL -- pdna_gbdex() below only installs this while `s` is in scope), or when the
 * species is out of the Gen-1/2 range (`dex` is pdna_pick.c's own pk_national_no()
 * result, so this is defensive, not load-bearing -- pdna_dex_set_max() already caps
 * the visible list at 151/251).
 *
 * dex_cell_art_source() (BACKLOG #124/#196, source/dex_cell_art_rule.h) is the
 * pure-C gate: this function computes the real-world inputs and asks the RULE what
 * to do, rather than encoding the decision inline -- tests/host_dexcellart_test.c
 * exercises the exact same function against the whole truth table, so a review can
 * check the gate's LOGIC on the host without a GBA build. `store_ok` is always true
 * here (the caller, pdna_pick.c's dex_cell_grid(), always has its own unchanged
 * fallback ready for a `false` return) -- passed explicitly so the rule states its
 * whole contract.
 *
 * BACKLOG #196 shipped this callback fetching on EVERY cell, every repaint, with an
 * 11-slot FIFO cache that the #196 review found inert (21 MISS / 0 HIT against a
 * 21-cell page, on a cold page, a row scroll, scrolling back AND a same-page
 * repaint -- capacity smaller than the page, not the eviction policy, was the
 * defect). BACKLOG #208 replaces that with the real cache above (gbdex_cell_art_
 * gen1/gen2, DEXCACHE_SLOTS = 23 >= one page): this function is now just the gate +
 * the per-generation dispatch. */
static bool gbdex_cell_art(uint16_t dex, int x, int y, int w, int h, void* ctx) {
  const GbSession* s = (const GbSession*)ctx;
  int session_gen = s ? (int)s->gen : 0;
  bool gb_have = gbdex_serves_dex(s);
  if (dex_cell_art_source(session_gen, gb_have, /*store_ok=*/true) != DEX_CELL_ART_GB)
    return false;
  if (dex < 1 || dex > 251) return false;

  return (session_gen == GB_GEN1) ? gbdex_cell_art_gen1(dex, x, y, w, h)
                                  : gbdex_cell_art_gen2(dex, x, y, w, h);
}

static u16 s_wait(u16 mask) {
  u16 k, fresh;
  do {
    VBlankIntrWait(); snd_vblank(); key_poll();
    fresh = key_hit(mask);
    k = fresh | key_repeat(mask & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT));
  } while (!k);
  if      (fresh & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) snd_move();
  else if (fresh & KEY_A) snd_ok();
  else if (fresh & KEY_B) snd_back();
  return k;
}

/* ---- the get/set shims pdna_dex_screen() calls through -----------------------
 * File-static current session, same shape as pdna_main.c's own g_sb1/g_sb2/g_game
 * for the Gen-3 dex shims (dex_state/dex_set_state, pdna_main.c:5188-5193) -- the
 * DexGetState/DexSetState function pointer types carry no closure, so the session
 * has to live somewhere pdna_dex_screen's callback can reach without a parameter. */
static GbSession* s_gbdex_session;

static int gbdex_shim_get(int nat) {
  if (!s_gbdex_session) return 0;
  if (gbdex_get(s_gbdex_session, (uint16_t)nat, true)) return 2;    /* caught */
  return gbdex_get(s_gbdex_session, (uint16_t)nat, false) ? 1 : 0;  /* seen / none */
}

/* Does NOT call gbs_finish() -- gb_dex.h's own batching contract: pdna_dex_screen's
 * bulk Catch/See/Wipe-ALL op can call this up to gb_max_species() times in one user
 * gesture (dex_bulk's ALL loops); pdna_gbdex() below finishes ONCE after the whole
 * screen session, on confirm, matching the Gen-3 caller's own one-commit-at-exit
 * shape (pdna_main.c's pdna_dex_edit -> app_commit_dex, a single call after the
 * screen returns, never per A-press). */
static void gbdex_shim_set(int nat, int state) {
  if (!s_gbdex_session) return;
  gbdex_set(s_gbdex_session, (uint16_t)nat, true,  state >= 2);
  gbdex_set(s_gbdex_session, (uint16_t)nat, false, state >= 1);
}

/* ---- Unown forms (Gen 2 only): a 26-row A..Z toggle list, the plain PokeDNA list
 * idiom -- pdna_pick.c's own DV_LIST view geometry (x0=4, y0=24, row height 9, 13
 * visible rows) reproduced here as PLAIN NUMBERS, not shared code: the brief's own
 * acceptance gate restricts pdna_pick.c's diff to the item-1 cap only, so this row
 * list is entirely this file's own code, just drawn to match. trainer_flag_row_paint
 * (pdna_trainer.h, already public, already the "label ON/off" row every badge/flag
 * screen in this codebase uses) supplies the row painter itself. */
#define UNOWN_ROWS_VISIBLE 13
#define UNOWN_ROW_Y0       24
#define UNOWN_ROW_STEP     9

static void unown_clamp_scroll(int* sel, int* top) {
  if (*sel < 0) *sel = 0;
  if (*sel > 25) *sel = 25;
  if (*sel < *top) *top = *sel;
  if (*sel >= *top + UNOWN_ROWS_VISIBLE) *top = *sel - (UNOWN_ROWS_VISIBLE - 1);
  if (*top < 0) *top = 0;
  if (*top > 26 - UNOWN_ROWS_VISIBLE) *top = 26 - UNOWN_ROWS_VISIBLE;
}

static void unown_letter_label(int letter, char out[16]) {
  siprintf(out, "Letter %c", (char)('A' + letter));
}

static void unown_render(GbSession* s, int sel, int top, bool can_edit) {
  ui_clear();
  ui_text(4, 2, UI_TITLE, "UNOWN FORMS");
  ui_hline(0, 11, UI_SCR_W, UI_BORDER);
  for (int i = 0; i < UNOWN_ROWS_VISIBLE && top + i < 26; i++) {
    int letter = top + i;
    char lbl[16]; unown_letter_label(letter, lbl);
    trainer_flag_row_paint(lbl, gbdex_unown_seen(s, letter),
                           UNOWN_ROW_Y0 + i * UNOWN_ROW_STEP, letter == sel);
  }
  /* D3 (b87 fix pass, DO-NOT-SHIP review): a read-only cart must not advertise an
   * "A toggle" it will refuse -- the legend itself is the tell. */
  trainer_key_legend(can_edit ? "U/D select  A toggle  B back" : "U/D select  B back");
}

/* Returns true iff the raw wUnownDex bytes at exit differ from the bytes at entry.
 * Does NOT call gbs_finish() -- same batching contract as the dex shims above;
 * pdna_gbdex() finishes once after BOTH this screen and the Pokedex screen have had
 * their turn.
 * D3 (b87 fix pass, DO-NOT-SHIP review): this screen used to ignore can_edit entirely
 * -- a read-only cart (Everdrive, or any cart pdna_app.h's app_can_edit() refuses)
 * could still flip Unown letters. Gated the same way pdna_dex_screen (the sibling
 * screen this file also drives) already gates its own edits.
 * N2 (b87 fix pass, DO-NOT-SHIP review): dirty used to be an OR of every individual
 * toggle's own before/after change -- cycling a letter on then off again in the same
 * visit left dirty stuck true even though the net state matched what the session
 * started with, forcing an unnecessary "Save Pokedex changes?" prompt and write.
 * R2 (b87 fix pass 2, DO-NOT-SHIP review, MUTATION-PROVEN): N2's own fix compared
 * gbdex_unown_seen()'s per-letter MEMBERSHIP (26 bools), but wUnownDex is an ORDERED
 * list -- toggling a letter off then back on REMOVES then RE-APPENDS it (it moves
 * from its old slot to whatever the first empty slot now is), which changes the raw
 * bytes and the in-game Unown-page order without changing which 26 letters read
 * "seen". That left a genuine edit reporting clean -- neither committed (no confirm
 * prompt fired) nor rolled back (the reordered bytes stayed staged in the session
 * image for whatever LATER, unrelated persist came next). Fixed: gbdex_unown_list()
 * snapshots/compares the raw 26 bytes (same memcmp-no-op shape pdna_gbtrainer.c's
 * own card-commit path uses, `if (!memcmp(&t, &t0, sizeof t)) ... return`), not
 * membership. */
static bool unown_forms_screen(GbSession* s, bool can_edit) {
  uint8_t snap[26];
  bool have_snap = gbdex_unown_list(s, snap);

  int sel = 0, top = 0;
  unown_clamp_scroll(&sel, &top);
  unown_render(s, sel, top, can_edit);
  for (;;) {
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) {
      uint8_t now[26];
      if (!have_snap || !gbdex_unown_list(s, now)) return false;
      return memcmp(now, snap, sizeof now) != 0;
    }
    if (k & KEY_UP)   sel--;
    if (k & KEY_DOWN) sel++;
    if (k & KEY_A) {
      if (!can_edit) {
        snd_deny();
      } else {
        bool now = gbdex_unown_seen(s, sel);
        (void)gbdex_unown_set(s, sel, !now);
      }
    }
    unown_clamp_scroll(&sel, &top);
    unown_render(s, sel, top, can_edit);
  }
}

/* ---- the entry chooser (Gen 2 only): "Pokedex" / "Unown forms" ---------------
 * Gen 1 has neither Unown nor a choice to make -- pdna_gbdex() skips this chooser
 * entirely and opens pdna_dex_screen() directly for a Gen-1 session. Loops so a
 * visit can touch BOTH sub-screens (e.g. mark a species caught, then flip to Unown
 * forms) before the ONE confirm+commit at the very end -- never a confirm per
 * sub-screen, matching the Gen-3 caller's single end-of-visit commit. */
static bool gbdex_chooser(GbSession* s, bool can_edit) {
  enum { ROW_DEX = 0, ROW_UNOWN, ROW_N };
  static const char* const kLbl[ROW_N] = { "Pokedex", "Unown forms" };
  int sel = 0;
  bool dirty = false;
  for (;;) {
    ui_clear();
    ui_text(4, 2, UI_TITLE, "POKEDEX (GEN 2)");
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < ROW_N; i++) {
      int y = 24 + i * 9;
      if (i == sel) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
      ui_text(6, y, i == sel ? UI_SELTEXT : UI_TEXT, kLbl[i]);
    }
    trainer_key_legend("U/D select  A choose  B back");
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return dirty;
    if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : ROW_N - 1;
    if (k & KEY_DOWN) sel = (sel + 1) % ROW_N;
    if (k & KEY_A) {
      if (sel == ROW_DEX) {
        key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
        /* BACKLOG #124: installed tightly around this one call, not the whole chooser
         * loop -- unown_forms_screen() (the ROW_UNOWN branch below) never needs it, and
         * a bracket that outlived this call would still be live (with `s`, a stack
         * pointer this function received, as ctx) after gbdex_chooser() itself
         * returns. */
        pdna_dex_set_cell_art(gbdex_cell_art, s, gbdex_serves_dex(s));
    pdna_dex_set_detail_gen(s->gen == GB_GEN1 ? (uint8_t)PDNA_GEN1 : (uint8_t)PDNA_GEN2);   /* BACKLOG #203 */
#ifdef PDNA_DELTA
        pdna_dex_set_page_begin(gbdex_dex_page_begin);
#endif
        bool dex_dirty = pdna_dex_screen(gbdex_shim_get, gbdex_shim_set, NULL, NULL, can_edit);
#ifdef PDNA_DELTA
        gbdex_dex_page_flush_last();
        pdna_dex_set_page_begin(NULL);
#endif
        pdna_dex_set_cell_art(NULL, NULL, false);
        pdna_dex_set_detail_gen(0);
        if (dex_dirty) dirty = true;
      } else {
        if (unown_forms_screen(s, can_edit)) dirty = true;
      }
    }
  }
}

bool pdna_gbdex(GbSession* s, bool can_edit) {
  if (!s || !s->open) return false;
  s_gbdex_session = s;

  /* BACKLOG #208: borrow the GB12 arena's tail slack for the WHOLE visit (a real
   * per-page cache has to survive a row scroll, not just one repaint) -- see the
   * cache's own header comment above gbdex_cell_blit() for the full rationale,
   * including why nothing else can be holding this slice at the same time.
   * Defensive: a failed borrow (should never happen) just leaves the cache
   * disabled (dexcache_reset(NULL, 0)), and every cache_lookup_gen1/2 or
   * cache_insert_gen1/2 call below degrades to an ordinary miss. */
  uint8_t* tail = gb12_arena_tail(DEXCACHE_NEED);
  dexcache_reset(&s_cache, (DexArtSlot*)tail, DEXCACHE_SLOTS);
#ifdef PDNA_DELTA
  s_page_no = 0; s_page_fetch = 0; s_page_hit = 0;
#endif

  pdna_dex_set_max((int)gb_max_species(s->gen));   /* Gen 1 -> 151, Gen 2 -> 251 */

  bool dirty;
  if (s->gen == GB_GEN2) {
    dirty = gbdex_chooser(s, can_edit);
  } else {
    key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
    /* BACKLOG #124: installed here too (a Gen-1 visit) so gbdex_cell_art's own
     * session-gen check is the ONE place that decides which generation's ROM (if
     * any) serves this visit -- see this file's header comment for why that must
     * not be "just don't install it here" (a separately-registered Gen-2 ROM must
     * not leak into a Gen-1 dex's Kanto-range cells, or vice versa). BACKLOG #196:
     * Gen 1 now draws its own front sprites through this SAME override (previously
     * this branch's `s->gen != GB_GEN2` self-gate meant Gen 1 always fell through
     * to the icon-store ladder). */
    pdna_dex_set_cell_art(gbdex_cell_art, s, gbdex_serves_dex(s));
        pdna_dex_set_detail_gen(s->gen == GB_GEN1 ? (uint8_t)PDNA_GEN1 : (uint8_t)PDNA_GEN2);   /* BACKLOG #203 */
#ifdef PDNA_DELTA
    pdna_dex_set_page_begin(gbdex_dex_page_begin);
#endif
    dirty = pdna_dex_screen(gbdex_shim_get, gbdex_shim_set, NULL, NULL, can_edit);
#ifdef PDNA_DELTA
    gbdex_dex_page_flush_last();
    pdna_dex_set_page_begin(NULL);
#endif
    pdna_dex_set_cell_art(NULL, NULL, false);
    pdna_dex_set_detail_gen(0);
  }

  /* BACKLOG #208: release the arena tail borrowed above -- both branches above are
   * done touching the cache by this point (the whole VISIT's cache lifetime, not
   * just one repaint -- see the cache's own header comment for why). */
  if (s_cache.slots) gb12_arena_tail_release();
  dexcache_reset(&s_cache, 0, 0);

  s_gbdex_session = NULL;

  if (!dirty) return false;
  /* D2 (b87 fix pass, DO-NOT-SHIP review): gbdex_shim_set/gbdex_unown_set write
   * straight into the session's image (g_ed->img via gbdex_set/gbdex_unown_set,
   * not a staging buffer) -- a decline here must DISCARD those bytes or a later,
   * unrelated persist (e.g. a different screen's own confirm-and-commit later in
   * the same session) would silently carry this screen's declined dex edits out
   * to the .sav too. gb_rollback() restores g_ed->img from g_ed->pristine, same
   * pattern every other decline path in pdna_gen12.c already uses. BACKLOG #64
   * review: `dirty` can only become true when `can_edit` was true, and
   * gb_nav_from_start() only ever passes `ed && app_can_edit()` for this row --
   * `ed` is false on a streamed (read-only) session, so this whole branch (and
   * its g_ed-shaped assumptions) stays genuinely unreachable there; `s` may be
   * g_ed->s or a streamed session up above, but by the time control reaches HERE
   * it is provably g_ed->s. */
  /* #234 s4: with the journal recording the prompt is gone -- the commit is HELD (one recorded step, no write; the exit
   * confirm writes once). With it off the confirm + immediate write stay exactly as they were. */
  if (!gb_hold_live() && !app_confirm("Save Pokedex changes?", "Writes the dex now.")) { gb_rollback(); return false; }

  GbsStatus st = gbs_finish(s);
  if (st != GBS_OK) { gb_rollback(); return false; }
  return gb_hold_commit("dex");
}
