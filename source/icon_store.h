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

/* ---- the read ------------------------------------------------------------------ */

/* Row `row` (0..439, the axis art_icons_row_for and rom_mon both use), 1024 B: frame f
 * is at p + f * 512. NULL if no rung can produce it. Zero SD I/O on a hit.
 * SEE THE POINTER CONTRACT ABOVE. */
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

/* ---- what the app can ask about the store ------------------------------------- */
enum { ICON_RUNG_NONE = 0, ICON_RUNG_CACHE, ICON_RUNG_ROM };
int      icon_store_rung(void);        /* ICON_RUNG_*                               */
uint16_t icon_store_capacity(void);    /* rows the pool can hold resident right now  */

#endif /* ICON_STORE_H */
