#ifndef ICON_STORE_H
#define ICON_STORE_H

#include <stdint.h>
#include <stdbool.h>

struct RomMon;

/*
 * icon_store — the SINGLE owner of every Pokemon-icon byte in RAM and of every
 * icon-related SD read in the artless build.
 *
 * WHAT IT REPLACES. Before this module the artless icon path was five caches deep,
 * none of them big enough to matter, all of them fighting each other:
 *
 *   1. art_fallbacks.c's s_icfr -- a 3-slot MRU over raw 512 B (row, frame) pairs,
 *      1,560 B of EWRAM. Against a 21-cell Pokedex page that is a 3/21 hit rate on the
 *      first pass and ~0 % on a scroll: the textbook LRU pathology, a cyclic sweep
 *      through a cache smaller than the working set, where EVERY access evicts the
 *      entry needed 21 accesses later. That is Guy's "it gets worse the more pokemon
 *      are moving", exactly.
 *   2. art_fallbacks.c's s_romloc -- a ONE-ENTRY row-locate memo. This, not the MRU,
 *      was the binding constraint: cycling between two species re-located AND re-read
 *      on every single flip, measured at n * 6 RomReadFn calls per flip for n >= 2.
 *   3. art_fallbacks.c's s_rompal -- the 3 ROM palette banks.
 *   4. art_icons_cache.c's 601 B of path-keyed metadata statics.
 *   5. box_oam.c's s_iconloc -- a SECOND, independent one-entry locate memo over the
 *      SAME open ROM, so the box and the dex re-located the same species for each other.
 *
 * One store replaces all five, and it is the only place a plan, a pin or an eviction
 * exists. Five layers to one; the count is the point.
 *
 * THE UNIT IS A ROW, NEVER A FRAME. A row is 1024 B: both bob frames, adjacent, on
 * both rungs (icons.bin by construction; the ROM because rom_mon range-checks 2 x 512 B
 * at the icon pointer). Measured on the EZ-Flash Omega DE path: a 1024 B read costs the
 * SAME one disk_read, the same one card chunk and the same 24 fixed halfword cart
 * writes as a 512 B read -- the second frame is ~46 us of DMA and nothing else. THAT is
 * what makes a bob flip free: the flip needs no I/O at all, because the other frame
 * arrived with the first one.
 *
 * ---------------------------------------------------------------------------------
 * THE POINTER CONTRACT -- READ THIS BEFORE CALLING icon_store_row().
 *
 * icon_store_row() returns a pointer INTO the row pool. It is valid until the NEXT
 * icon_store_* call of any kind. Exactly ONE live row pointer may exist at a time:
 * the slot it came from is marked "hot" and is exempt from eviction only until then.
 *
 *     Fetch A, fetch B, then blit A  IS A BUG.  It paints B's tiles under A's name.
 *
 * This is the same discipline artbuf.h's mon_decomp needs, and that one has shipped
 * broken TWICE (see pdna_origin_art.c's note). Making the fetch free makes the
 * temptation to hoist fetches out of a blit loop stronger, not weaker. Do not hoist.
 * ---------------------------------------------------------------------------------
 */

/* ---- session ------------------------------------------------------------------
 * Called ONLY from pdna_main.c's app_icon_cache_resolve(), which is already the one
 * place that resets every icons.bin-derived memo -- at boot, on every ROM
 * (re-)registration, and after a fresh extraction. NOTHING ELSE MAY INVALIDATE. In
 * particular a screen must never "clear the cache to be safe": that instinct is how
 * five layers ended up re-reading everything. Screen exit drops PINS only; contents
 * survive, which is why revisiting the party overlay costs nothing.
 *
 * Rows are immutable derived art -- nothing in this app writes icons.bin or a ROM --
 * so there is no per-row invalidation event and no write-back, and that is exactly why
 * a row can stay pinned for a whole screen with no staleness risk.
 *
 * `icons_path` is the validated icons.bin (NULL if none), `rm` the open RomMon (NULL
 * if none). The cache rung wins when both are present. */
void icon_store_reset(const char* icons_path, const struct RomMon* rm);

/* Close the icons.bin handle and refuse to reopen until the next icon_store_reset().
 *
 * MANDATORY around art_extract_screen, and the single most dangerous invariant in this
 * file. FF_FS_LOCK is 0, so FatFs will NOT stop this module holding a FIL across the
 * extraction screen's f_unlink + f_rename of icons.bin -- that is a stale cluster chain
 * on a card that also holds the user's saves. FatFs gives you no mechanism here; the
 * call is the whole defence. */
void icon_store_suspend(void);

/* ---- the plan: a screen says what it is about to draw, BEFORE it draws it ------
 *
 * WHY THIS EXISTS AT ALL. Every screen already knows its exact icon set one function
 * call before the paint loop -- the Pokedex grid's 21 cells are g_list[top..top+20],
 * the party's 6 are the party. Without a declaration that knowledge is thrown away and
 * rediscovered one cell at a time, and a cache smaller than the page then has a hit
 * rate of exactly ZERO on the cyclic sweep a repaint performs: every access evicts the
 * entry needed `page` accesses later. Measured on the host FatFs harness before this
 * call existed: a 21-cell page against a 6-row pool cost 21 separate transfers, and
 * cost them again on every bob flip.
 *
 * WHAT THE DECLARATION BUYS. The store sorts the misses BY SOURCE BYTE OFFSET, merges
 * strictly-consecutive rows into single transfers, and sweeps the file FORWARD once.
 * On icons.bin, consecutive rows are consecutive bytes, and under the dex's default
 * filter+sort 12 of 18 full pages are one contiguous 21-row span -- so a page that cost
 * 21 transfers costs as few as ceil(21/rows-that-fit). Ascending order is the
 * load-bearing half: FatFs restarts a chain walk only on a BACKWARD seek (ff.c:4527),
 * so a forward sweep is cheap even if the cluster link map failed to build.
 *
 * WHAT IT CANNOT BUY, stated plainly: the pool is the pool. Declaring 21 rows against a
 * pool that holds 6 does not make 21 rows resident -- it makes the store fetch them in
 * bulk GROUPS as the paint consumes them, which cuts transactions (the expensive part:
 * 24 fixed halfword cart writes + one card-latency poll each) without cutting sectors.
 * icon_store_borrow() is how a screen makes the set actually FIT (32 more rows, rented
 * from g_pc for one screen), and it must be taken BEFORE the plan is declared or the
 * sweep fills the small pool and comes up short. The box screen is the one screen that
 * can never borrow -- it is displaying the donor -- so it lives in the first case
 * permanently, by design.
 * icon_store_plan_resident() is how a caller finds out which case it is in, and it is
 * the ONLY honest gate for an animation: a flip is free iff every row is already here.
 *
 * `rows` are row indices (0..439); duplicates and out-of-range entries are dropped.
 * At most ICON_STORE_PLAN_MAX are kept. Returns how many of the plan are resident when
 * the call returns. Calling it with n <= 0 retires the plan.
 *
 * A PLAN INVALIDATES EVERY OUTSTANDING ROW POINTER -- it is an icon_store_* call like
 * any other, and it is the one most likely to move things, so declare BEFORE the paint
 * loop, never inside it. */
#define ICON_STORE_PLAN_MAX 40
int  icon_store_plan(const uint16_t* rows, int n);

/* True iff every row of the live plan is in RAM right now, so a redraw of the declared
 * set is provably ZERO SD transactions. False when no plan is live, or when the plan
 * does not fit the pool. True when there is no rung at all (a "flip" then redraws
 * nothing and costs nothing, which is the same answer for the caller). */
bool icon_store_plan_resident(void);

/* How many rows the live plan declares; 0 = no screen has declared one.
 *
 * It exists so a caller can tell the two `false`s of icon_store_plan_resident() apart:
 * "this screen's rows are not all in RAM" (an honest, expected answer -- fall back to a
 * static frame) and "this screen never declared anything" (a PROGRAMMING ERROR -- the
 * gate is being asked a question nobody supplied the input for, and the symptom is
 * exactly the silently-dead animation this whole redesign exists to fix). */
uint8_t icon_store_plan_count(void);

/* ---- the read ------------------------------------------------------------------ */

/* Row `row` (0..439, the axis art_icons_row_for and rom_mon both use), 1024 B: frame f
 * is at p + f * 512. NULL if no rung can produce it. Zero SD I/O on a hit.
 * SEE THE POINTER CONTRACT ABOVE.
 *
 * A miss on a row that IS in the live plan refills the pool with the next GROUP of
 * planned rows in one sorted, merged sweep -- so a 21-cell page walks its plan in
 * pool-sized bulk bites instead of 21 single reads. A miss on a row that is NOT in the
 * plan is one single-row fetch into the LRU victim, exactly as before. */
const uint8_t* icon_store_row(uint16_t row);

/* Row `row`'s 16 RGB15 palette entries. Never touches the card on the cache rung
 * (the whole tail is loaded once at reset); on the ROM rung it may fill one of the 3
 * shared banks the first time that bank is used, and then never again. */
bool icon_store_pal(uint16_t row, uint16_t out[16]);

/* Row `row`'s palette BANK index (0..2), or 0xFF -- what box_oam.c needs to point an
 * OBJ attribute at a bank it already uploaded, without expanding to colours. */
uint8_t icon_store_pal_id(uint16_t row);

/* Shared palette bank `i` (0..2) directly, for a caller uploading all three up front. */
bool icon_store_pal_at(int i, uint16_t out[16]);

/* ---- Tier B: borrow g_pc for 32 more rows ------------------------------------
 *
 * WHY A SECOND TIER EXISTS AT ALL. Tier A is 6 rows on the cache rung and FOUR on the
 * ROM rung, because that rung spends 1,760 B of the same 6,144 B pool on its resident
 * offset table (see the pool-carve comment in icon_store.c). Four rows cannot hold a
 * party of SIX, a day-care yard of seven, or a 21-cell Pokedex page -- so on the ROM
 * rung those screens thrash, and the animation gate (icon_store_plan_resident, and
 * therefore mon_icon_anim_cheap) honestly answers "no" and leaves them static. That is
 * the regression the ROM index table introduced and this is what pays it back.
 *
 * THE DONOR IS g_pc, and it is the well-trodden one: pdna_map.c and pdna_gen12.c
 * already borrow the same 35,712 B through app_arena_acquire(). Two rules make it safe
 * and they are NOT optional:
 *   - it REFUSES when the PC is dirty (unsaved box moves live only in g_pc, and handing
 *     it out would destroy them), and
 *   - releasing RE-DERIVES g_pc from g_save, byte for byte.
 * icon_store never touches g_pc except through those two calls.
 *
 * THE RULE FOR CALLERS, and it is the whole safety argument: the borrow may be held
 * only while the screen can promise not to touch the PC. A screen that opens a mon
 * menu, commits a save, or injects into a box MUST release first -- app_commit_all()
 * writes g_pc back into g_save, so committing with the borrow live would write icon
 * tiles over the user's boxes. In practice that means: acquire before the paint, and
 * release the moment the idle loop ends and a key is dispatched. The Pokedex is the one
 * screen that can hold across its whole lifetime, because nothing it opens reaches the
 * PC. pdna_main.c's `switch (nav_menu())` calls icon_store_borrow(false) unconditionally
 * after every case body, so a forgotten release cannot outlive one nav choice.
 *
 * Releasing also RETIRES THE LIVE PLAN. The release is exactly the moment a screen's
 * claim about what is on the glass expires, and a stale plan would let the next
 * screen's animation gate answer a stale "yes".
 *
 * Returns whether the borrow is held when the call returns. Idempotent both ways. */
bool icon_store_borrow(bool on);

/* ---- what the app can ask about the store ------------------------------------- */
enum { ICON_RUNG_NONE = 0, ICON_RUNG_CACHE, ICON_RUNG_ROM };
int      icon_store_rung(void);        /* ICON_RUNG_*                               */
uint16_t icon_store_capacity(void);    /* rows the pool can hold resident right now  */
bool     icon_store_borrowed(void);    /* Tier B is live (a test/telemetry question) */

#endif /* ICON_STORE_H */
