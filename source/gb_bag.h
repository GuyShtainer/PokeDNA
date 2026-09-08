#ifndef GB_BAG_H
#define GB_BAG_H

#include <stdint.h>
#include <stdbool.h>

#include "gb_fields.h"    /* GbGame, GbField, gbf_off/len/kind                          */
#include "gb_session.h"   /* GbSession, GbsStatus, gbs_read_field/gbs_write_field/finish */

/* gb_bag — pure-C bag / PC-item-store core over the generated field table
 * (BACKLOG #49 P2a, docs/GEN12-PARITY-DESIGN.md §1.2 + §4.1 P2).
 *
 * Shape mirrors gb_trainer.h on purpose: a read that fills a plain struct, and a
 * write that goes through the session's field-write primitive (gbs_write_field /
 * gbs_finish, source/gb_session.h) the way P2b's screen (pdna_gbbag.c, not part of
 * this slice) will call it. Every offset is looked up through gbf_off/gbf_len/
 * gbf_kind — never a literal.
 *
 * LAYOUT (§1.2). Every pocket is `count byte, cap x entry, 0xFF terminator`, entries
 * are (id, qty) pairs EXCEPT Gen-2 Key items, which are single bytes (id only, no
 * quantity). Gen 2's TM/HM pocket is not a list at all: it is a flat 57-byte COUNT
 * ARRAY, index = TM/HM number - 1, no terminator.
 *
 *   Pocket     cap  entry   Red/Blue+Yellow          Gold/Silver     Crystal
 *   Items       20  id+qty  yes (the only pocket)    yes             yes
 *   Key items   25  id      no                        yes             yes
 *   Balls       12  id+qty  no                        yes             yes
 *   TM/HM       57  count   no (TMs are bag items)     yes             yes
 *   PC store    50  id+qty  yes                        yes             yes
 *
 * gbb_field_present(game, pocket) mirrors gbt_field_present's own rule: false means
 * the game has no such pocket at all (Gen 1 asking for KEY/BALLS/TMHM), never that
 * it happens to be empty.
 *
 * VALID ITEM IDS (§1.2). Gen 1 (Red/Blue, Yellow): 0x01..0xFA (0xC4..0xFA are the
 * TM01-50/HM01-05 item ids, legitimately bag/PC contents). Gen 2 (Gold/Silver,
 * Crystal): 0x01..0xBE are real items (NUM_ITEMS = 0xBE); 0xBF..0xF9 are the TM/HM
 * item ids used only by the TM pocket's OWN numbering (the count array above), never
 * a valid id inside an Items/Key/Balls/PC list. 0x00 and 0xFF (the terminator) are
 * never valid ids in either generation.
 *
 * WRITE DISCIPLINE (the P1a discipline gb_trainer.c already established, applied at
 * POCKET granularity here). gbb_write() diffs the CANDIDATE bytes it would write
 * against what is CURRENTLY on disk for that whole pocket (count byte + body) before
 * ever calling gbs_write_field — a pocket whose decoded entries are unchanged writes
 * zero bytes, even if that pocket's stored body still carries stale bytes after its
 * terminator (real Game Boy Pokemon games leave exactly that garbage behind when an
 * item is removed; this core does not manufacture a diff by "cleaning" it). When a
 * pocket DOES change, only the bytes up to and including the new terminator are
 * rewritten; everything past the new terminator is left as whatever the current
 * on-disk pocket already held there — never zero-filled, never synthesised. */

typedef enum {
  GBB_POCKET_ITEMS = 0,   /* every game                                    */
  GBB_POCKET_KEY,         /* Gen 2 only; entries carry id only, qty ignored */
  GBB_POCKET_BALLS,       /* Gen 2 only                                    */
  GBB_POCKET_TMHM,        /* Gen 2 only; NOT a list -- see tmhm_counts[]    */
  GBB_POCKET_PC,          /* every game                                    */
  GBB_POCKET_COUNT
} GbBagPocket;

/* Per-pocket entry cap (the list pockets only; GBB_POCKET_TMHM is fixed at 57). */
#define GBB_CAP_ITEMS 20
#define GBB_CAP_KEY   25
#define GBB_CAP_BALLS 12
#define GBB_CAP_PC    50
#define GBB_CAP_MAX   50   /* the largest of the above -- GbBagList's entries[] size */

#define GBB_TMHM_COUNT 57
#define GBB_QTY_CAP    99   /* "Quantity cap is 99 in both generations", §1.2        */
#define GBB_TMHM_CAP   99   /* "TM/HM counts 0..99", §4.1 P2 item 1                  */
#define GBB_MAX_BODY  101   /* widest pocket body: PC store, 50*2+1 (gbf_len GBF_PC_BODY) */

typedef struct {
  uint8_t id;
  uint8_t qty;   /* always 1 for GBB_POCKET_KEY entries (no stored quantity byte) */
} GbBagEntry;

typedef struct {
  uint8_t     count;                 /* number of occupied entries, <= this pocket's cap */
  GbBagEntry  entries[GBB_CAP_MAX];
} GbBagList;

typedef struct {
  GbBagList pockets[GBB_POCKET_COUNT];        /* GBB_POCKET_TMHM's .count/.entries unused */
  uint8_t   tmhm_counts[GBB_TMHM_COUNT];       /* Gen 2 only; index = TM/HM number - 1     */
} GbBag;

typedef enum {
  GBB_OK = 0,
  GBB_ERR_ARG,          /* NULL / out-of-range slot or TM/HM index                    */
  GBB_ERR_NOT_PRESENT,  /* this game does not have this pocket (gbb_field_present)    */
  GBB_ERR_FULL,         /* the pocket has no free slot and no matching id to merge into*/
  GBB_ERR_BADID,        /* id is 0x00, 0xFF, or past this game's item-id range        */
  GBB_ERR_QTY           /* quantity outside [1, GBB_QTY_CAP] (or TM/HM count > CAP)   */
} GbBagOpStatus;

/* Does `game` have `pocket` at all? Mirrors gbf_off(game, field) != 0 for the
 * pocket's count field, so a screen can hide a pocket tab this game lacks (Gen 1
 * asking for KEY/BALLS/TMHM) without repeating the field table's own rule. */
bool gbb_field_present(GbGame game, GbBagPocket pocket);

/* The entry cap for a list pocket (GBB_CAP_ITEMS/KEY/BALLS/PC). 0 for
 * GBB_POCKET_TMHM (not a list) or a pocket this game lacks. */
int gbb_pocket_cap(GbGame game, GbBagPocket pocket);

/* Highest valid item id for `game` (0xFA for Red/Blue/Yellow, 0xBE for Gold/
 * Silver/Crystal — §1.2). Applies to every list pocket (Items, Key items, Balls,
 * PC store) alike; this core does not maintain a per-pocket item-category table. */
uint8_t gbb_max_item_id(GbGame game);

/* Fill `out` from the session's resident image. Every pocket this game lacks (per
 * gbb_field_present) is left zeroed (count 0, no entries) rather than causing a
 * hard failure. A pocket whose stored count exceeds its cap, or whose entries hit
 * the 0xFF terminator before the stored count is reached, is read DEFENSIVELY: at
 * most `cap` entries are decoded and decoding stops at the first 0xFF id even if
 * the stored count claims more — this can never read past the field's own bytes.
 * False only on a malformed session (never opened). */
bool gbb_read(const GbSession* s, GbBag* out);

/* Write `in` back into the session, pocket by pocket, per the write discipline
 * documented above. Calls gbs_finish() once at the end IFF at least one pocket
 * (or the TM/HM count array) actually changed; a bag identical to what gbb_read()
 * would return right now writes nothing and returns GBS_OK. On any non-GBS_OK
 * return the image may already hold SOME of the intended pocket writes -- same
 * contract gb_session.h documents for every other field write: the caller rolls
 * back the whole image from its own pristine copy, this layer does not. */
GbsStatus gbb_write(GbSession* s, const GbBag* in);

/* ---- in-memory model edits (act on `bag`, not the session -- call gbb_write()
 * afterwards to persist) --------------------------------------------------- */

/* Insert `id` with quantity `qty` into `pocket`. If `id` already occupies a slot
 * in a quantity-bearing pocket, the quantities are ADDED and clamped to
 * GBB_QTY_CAP. NOTE: the real games instead open a SECOND slot for the
 * overflow (pokered engine/items/inventory.asm, pokecrystal
 * engine/items/items.asm — read for the format fact only); this core
 * deliberately does not, so a merge that would exceed GBB_QTY_CAP SATURATES
 * the existing stack at the cap and reports GBB_ERR_QTY instead of silently
 * discarding the remainder. GBB_POCKET_KEY entries carry no quantity: `qty`
 * must be 1 (any other value is GBB_ERR_QTY) and a duplicate id is refused
 * outright (GBB_ERR_ARG) rather than silently merged, since a key item is a
 * unique flag, not a stack.
 *   GBB_ERR_NOT_PRESENT  `game` lacks `pocket` (or pocket == GBB_POCKET_TMHM --
 *                        use gbb_tmhm_set for that one, it is not a list).
 *   GBB_ERR_BADID        id == 0x00, id == 0xFF, or id > gbb_max_item_id(game).
 *   GBB_ERR_QTY          qty outside [1, GBB_QTY_CAP], a Key-items qty != 1,
 *                        or the merge saturated at GBB_QTY_CAP (the stack was
 *                        set to the cap).
 *   GBB_ERR_FULL         the pocket already holds gbb_pocket_cap() entries and
 *                        `id` is not already present to merge into. */
GbBagOpStatus gbb_insert(GbGame game, GbBag* bag, GbBagPocket pocket,
                          uint8_t id, uint8_t qty);

/* Remove the occupied slot at `slot` (0-based, < list->count) from `pocket`,
 * compacting the remaining entries down and decrementing count. */
GbBagOpStatus gbb_remove(GbGame game, GbBag* bag, GbBagPocket pocket, int slot);

/* Set the quantity of the occupied slot at `slot` in a quantity-bearing pocket.
 * GBB_ERR_ARG if `pocket` is GBB_POCKET_KEY (no quantity to set) or GBB_POCKET_TMHM. */
GbBagOpStatus gbb_set_qty(GbGame game, GbBag* bag, GbBagPocket pocket,
                           int slot, uint8_t qty);

/* Get/set one TM/HM's count (`tmhm_index` 0-based, 0..GBB_TMHM_COUNT-1 = TM01..
 * HM07 in the decomp's own numbering). GBB_POCKET_TMHM is a flat count array, not
 * a list, so it has no insert/remove/slot notion -- every index always "exists"
 * once the game has the pocket at all. */
bool gbb_tmhm_get(const GbBag* bag, int tmhm_index, uint8_t* count_out);
GbBagOpStatus gbb_tmhm_set(GbGame game, GbBag* bag, int tmhm_index, uint8_t count);

#endif /* GB_BAG_H */
