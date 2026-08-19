#ifndef ART_SESSION_H
#define ART_SESSION_H

#include <stdbool.h>

#include "art_cache.h"
#include "rom_map.h" /* RomCtx */

/*
 * The app-level ART LOAD-PATH RESOLVER (DESIGN.md Sec 4.1's "one session-scoped
 * resolver decides the rung once per kind, not per call"). GBA-only glue (touches
 * FatFs + the mon_decomp scratch buffer) — box_oam.c and pdna_main.c's icon
 * wrappers call THIS, never re-implement the art.idx dance themselves.
 *
 * Memoized per kind: the first call for a kind does real SD I/O (reads art.idx,
 * cross-checks the ROM if one is open, and re-hashes the WHOLE kind file once — the
 * "verify each kind's FNV once per session on first use" rule, DESIGN.md Sec 2.2
 * point 4); every call after that for the same kind, in the same invalidation epoch,
 * is a free memory read. Call art_session_invalidate() on a fresh save load, after a
 * successful extraction run, or whenever the registered ROM changes — anything that
 * could make a previously-rejected cache valid (or vice versa) again.
 */

/* Is kind `k`'s cache file present, internally valid, and (if `rc` is a currently-
 * open, successfully-identified RomCtx) stamped for THIS rom? Pass rc = NULL when no
 * ROM is open this session — DESIGN.md Sec 4.1: the cache "is the only [rung] that
 * exists in an artless build with no ROM registered this session", so with no RomCtx
 * to cross-check against, the cache is trusted on its own internal validity alone
 * (magic/format/length/FNVs) — never on a fabricated "no ROM means anything goes".
 * A false return means "use the next rung" (ROM stream, then procedural) — never
 * "retry", the memo already recorded why. */
bool art_session_kind_ready(ArtKind k, const RomCtx* rc);

/* Same check, MINUS the full re-hash of the kind file's content: size (against
 * art.idx's promise) is checked, but the FNV walk over up to 451 KB is skipped. For
 * the path that runs on EVERY boot (pdna_main.c's app_icon_rom_open) -- see
 * art_session.c's verify_kind_file comment for the exact trade-off this accepts and
 * why it is judged safe there. Shares the same per-kind memo as the deep check: a
 * caller that needs the deep guarantee must call art_session_invalidate() first if a
 * shallow check may already have memoized this epoch (app_icon_cache_resolve always
 * does, both at boot and after a fresh extraction, so this is automatic in practice). */
bool art_session_kind_ready_shallow(ArtKind k, const RomCtx* rc);

/* Convenience: art_session_kind_ready(ART_KIND_ICONS, rc) plus the path a caller
 * hands to art_icons_cache_open() on success. */
bool art_session_icons_ready(const RomCtx* rc);
bool art_session_icons_ready_shallow(const RomCtx* rc);
const char* art_session_icons_path(void);

/* Read-only: the CURRENT memoized verdict for a kind, with NO I/O and no cross-check
 * — false both for "never checked this epoch" and for "checked and refused". This
 * exists for a caller that has NO RomCtx of its own to offer (art_fallbacks.c's
 * mon_icon_for* rung, called from drawing code all over the app) and therefore must
 * NEVER be the one to trigger the first, rom-cross-checked resolution itself — doing
 * so with rc=NULL would memoize a verdict that never checked the open ROM, and a
 * LATER real check (from pdna_main.c's app_icon_rom_open, which always runs once at
 * boot / on every ROM re-registration BEFORE any screen draws) would then just return
 * that stale, uncross-checked answer instead of actually re-deriving it. Priming the
 * memo is app_icon_rom_open's job alone; every other caller only ever READS it. */
bool art_session_icons_ready_memoized(void);

void art_session_invalidate(void);

#endif /* ART_SESSION_H */
