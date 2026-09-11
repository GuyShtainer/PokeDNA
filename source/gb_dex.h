#ifndef GB_DEX_H
#define GB_DEX_H

#include <stdint.h>
#include <stdbool.h>

#include "gb_fields.h"    /* GbGame, GbField, gbf_off/len/kind                          */
#include "gb_session.h"   /* GbSession, GbsStatus, gbs_read_field/gbs_write_field/finish */

/* gb_dex — pure-C Pokedex (seen/caught, Gen 2 also Unown-forms) bitfield core over the
 * generated field table (BACKLOG #87 item 2). Mirrors the shape of gb_fly.h's
 * destination-visited core -- same GBFK_BITFIELD idiom, same "byte = off + n/8, bit =
 * n & 7, LSB-first within the byte" convention (gen3_dex.c's pk_dex_owned/pk_dex_seen
 * use the identical i>>3/i&7 shape on the Gen-3 side, gb_fields.h's own "bit n =
 * flag/dex-entry/spawn n" doc).
 *
 * WHICH FIELDS: GBF_DEX_OWNED / GBF_DEX_SEEN (gb_fields.c: Red/Yellow 19 B @0x25A3/
 * 0x25B6, GS 32 B @0x2A4C/0x2A6C, Crystal 32 B @0x2A27/0x2A47) -- bit n = National dex
 * n+1 on every game (dex 1 is bit 0 of byte 0), PROVEN against the corpus by
 * tests/host_gbdex_test.c: a popcount over every bit this core can address must equal
 * gb_trainer.c's own dex_owned/dex_seen (gbt_read, itself cross-checked against the
 * in-game trainer card's own dex count on the retail gate).
 *
 * CAUGHT IMPLIES SEEN (the game's own invariant): pokecrystal's engine/pokedex/
 * pokedex_flags.asm SetSeenAndCaughtMon falls through into SetSeenMon after setting
 * wPokedexCaught, so the game itself never produces owned=1/seen=0; pokered's
 * give_pokemon.asm SetPokedexOwnedFlag (owned only) plus engine/battle/core.asm's own
 * wPokedexSeen writes are Gen 1's twin. gbdex_set() enforces this both directions for
 * an editor that can otherwise desync them: setting owned=on=true also forces seen on;
 * clearing seen (owned=false, on=false) also forces owned off.
 *
 * UNOWN FORMS (Gen 2 only, Crystal/GS -- Gen 1 has no Unown): wUnownDex, 26 bytes, is
 * NOT a per-letter bitfield -- it is the ORDER-of-first-seen LIST pokecrystal's
 * engine/pokedex/unown_dex.asm UpdateUnownDex builds: each byte holds the 1-based
 * letter id (1=A..26=Z) of the Nth-ever-seen distinct Unown form, 0 = an unused slot,
 * and the routine's own loop both dedupes (a letter already present short-circuits)
 * and appends at the first zero. gbdex_unown_seen()/gbdex_unown_set() take a 0-based
 * `letter` (A=0..Z=25) and reproduce exactly that membership/append shape; gbdex_unown_
 * set(letter,false) additionally COMPACTS the list (an editor-only operation the real
 * game never performs -- it only ever appends) so the "0 = end of list" invariant
 * UpdateUnownDex's own scan depends on stays true after a manual removal.
 *
 * HONEST DISCREPANCY FROM THE BRIEF, on the record: the brief describes "unlocking"
 * (wUnlockedUnowns, GBF_UNLOCKED_UNOWN) as part of UpdateUnownDex's own mechanism.
 * Read in place, UpdateUnownDex (cited above) touches ONLY wUnownDex -- it never reads
 * or writes wUnlockedUnowns. wUnlockedUnowns is a SEPARATE, 4-bit progression flag
 * (data/events/engine_flags.asm: UNLOCKED_UNOWNS_A_TO_K_F / _L_TO_R_F / _S_TO_W_F /
 * _X_TO_Z_F) gating which LETTER GROUPS can spawn wild in the Ruins of Alph
 * (data/wild/unlocked_unowns.asm) -- a wild-encounter unlock, not a dex-seen record.
 * This core therefore does NOT touch wUnlockedUnowns at all; GBF_UNOWN_DEX/wUnownDex
 * alone backs gbdex_unown_seen/set, matching what those two functions' own names say
 * ("seen"). GBF_UNLOCKED_UNOWN stays in the generated field table, unconsumed by this
 * slice, available for a future wild-encounter-unlock feature.
 */

/* True iff `dex` (National no., 1..gb_max_species(s->gen)) is owned (if `owned`) or
 * seen (if !owned) on this session. False on any malformed input, an out-of-range
 * `dex`, a game this field is absent on, or a read failure -- never distinguishable
 * from "not set" by design (same posture as gbfy_get). */
bool gbdex_get(const GbSession* s, uint16_t dex, bool owned);

/* Set/clear ONE species' owned or seen bit. Writes only the byte(s) that actually
 * change (untouched is untouched). Enforces caught-implies-seen (see above) with at
 * most one extra byte write in the OTHER field. GBS_ERR_ARG on a bad dex/session/field
 * before any byte moves. DOES NOT call gbs_finish() -- a caller making MANY calls in
 * one user gesture (the dex screen's per-cell edit, or its bulk Catch/See/Wipe-ALL,
 * up to 386 calls) calls gbs_finish() itself exactly once after the last one, per
 * gbs_write_field's own documented batching contract (O(1) Gen-2 reparses instead of
 * O(edits)); Gen 1 is unaffected (already re-verified on every gbs_write_field call,
 * gbs_finish is a no-op there). */
GbsStatus gbdex_set(GbSession* s, uint16_t dex, bool owned, bool on);

/* Has Unown letter `letter` (0=A..25=Z) ever been seen (present anywhere in
 * wUnownDex)? False on a bad letter, a session with no Unown-dex field (Gen 1, or a
 * malformed Gen 2 session), or a read failure. */
bool gbdex_unown_seen(const GbSession* s, int letter);

/* on=true: append `letter` at the first empty (0) slot, matching UpdateUnownDex's own
 * scan-then-append -- a no-op (GBS_OK, no write) if already present; GBS_ERR_FULL if
 * every one of the 26 slots already holds a distinct letter and this one is not among
 * them (never happens on a real save -- there are only 26 possible letters -- but a
 * caller must not assume the invariant an editor could break by hand still holds).
 * on=false: remove `letter` if present and COMPACT the remaining entries left by one
 * (the game itself never removes; this is an editor-only shift that keeps "0 = end of
 * list" true for UpdateUnownDex's own future scans). GBS_ERR_ARG on a bad letter/
 * session/field before any byte moves. DOES NOT call gbs_finish() -- same batching
 * contract as gbdex_set above. */
GbsStatus gbdex_unown_set(GbSession* s, int letter, bool on);

#endif /* GB_DEX_H */
