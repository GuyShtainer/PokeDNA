#ifndef PDNA_BOX_H
#define PDNA_BOX_H

#include <stdint.h>
#include <stdbool.h>
#include "pdna_app.h"      /* AppCommitFn */

/* A box "container" the box screen renders/edits, abstracted so the same screen
 * drives both the in-save PC storage and the external bank. Each source supplies:
 *  - records(box): pointer to that box's 30*80 raw records, in a buffer laid out
 *    pc-style (records at +0x0004) — for paged sources (the bank) this loads the
 *    box, flushing the previously-loaded one if dirty.
 *  - menu_block: the pc-layout buffer records() lives in (passed to app_mon_menu);
 *    the loaded box's index within it is `box` for the PC, 0 for the (paged) bank.
 *  - name/wallpaper get/set, commit (immediate verified write of the current box),
 *    and mark_dirty (deferred path — PC defers moves to save-file exit; the bank
 *    persists quietly there and then).
 * All function pointers act on module-singleton state, so they take no `self`. */

/* BACKLOG #120 S1: the carry's cross-generation discriminator. `is_bank` (below) stays a
 * pure LAYOUT flag (suppresses the PARTY tab + PC hand-off edges 1/4, same as always) —
 * BOTH the real Bank and a raw Game Boy save's own box set it true, so it can no longer
 * tell those two apart. `scope` is the new, three-way identity a carry actually checks:
 * PC and BANK behave exactly as they always have (S1 is a pure refactor, no filler here
 * changes its `can_lift`/`xfer`, which stay NULL); GB is wired starting S2/S3. */
#define BOXSCOPE_PC   0
#define BOXSCOPE_BANK 1
#define BOXSCOPE_GB   2

/* Carried-mon cross-generation transfer state; the full definition lands with S3 (the UP
 * mechanics). S1 only needs the incomplete type so BoxXferOps's function pointers can
 * name it — a pointer to an incomplete struct type is legal in a prototype. */
typedef struct XferCarry XferCarry;

/* BACKLOG #120 S1: the opt-in cross-generation transfer vtable (§3.1 of
 * docs/BANK-CROSSGEN-DESIGN.md). NULL on every Gen-3 source (PC, Bank) forever; the Game
 * Boy source installs it starting S3 (write session) or a read-only twin missing every
 * member but `lift_up` (read-only mount, §3.1 `k_gb_xfer_ro`). Every member is a real
 * implementation added by a later slice — S1 wires no GB body, only the shape. */
typedef struct {
  uint8_t gen;                                     /* PDNA_GEN1 / PDNA_GEN2, for the wording */
  bool (*lift_up)(const uint8_t* rec80, uint8_t* out80, XferCarry* xc);       /* GB -> Gen-3 native */
  bool (*preview_down)(const uint8_t* rec80, XferCarry* xc);                 /* pure Gen-3 -> GB conversion, no write */
  bool (*accept_down)(int box, const uint8_t* original80, XferCarry* xc);    /* commits the DOWN drop */
  /* BACKLOG #150 S150-4 decision 6/7: re-shaped from `(int, int, const XferCarry*)` --
   * this lane needs no XferCarry (the packed cell already lives in s_held[80]), so the
   * hook is handed the 80 bytes it must RE-VERIFY against before it deletes (decision
   * 7: the user may have walked to another screen between the lift and the drop; a
   * blind (box, slot) delete could hit a bystander). */
  bool (*release_up)(int box, int slot, const uint8_t cell80[80]);           /* consumes the GB origin after UP lands, re-verified */
  bool (*move_within)(int box, int slot, int dst_box);                      /* (GB,GB) different-box drop = a within-save move */
} BoxXferOps;

typedef struct {
  int  nboxes;
  bool last_box_is_party;    /* GB sources expose the party as one extra pseudo-box at
                              * nboxes-1 ("GB PARTY"); it has no ordinal, so the banner
                              * must not invent one ("13:" on a 12-box RBY save). All
                              * sources memset their BoxSource, so this defaults false. */
  int  start_box;
  bool is_bank;
  /* BACKLOG #48: is_bank already suppresses START (a Bank screen is a SUB-screen
   * reached off the main nav menu; opening the SAME menu again from inside it would
   * be a pointless nested trip back to the very place it came from) as well as the
   * PARTY tab and the PC<->Bank hand-off edges (codes 1/4) — none of which a raw
   * Game Boy save's OWN box (pdna_gen12.c) wants either, so it also sets is_bank.
   * But unlike the real Bank, a GB session's box IS the top-level screen for that
   * visit, with no PC to back out to first — so START must still work there. Default
   * false (PC/real-Bank behaviour unchanged); pdna_gen12_source() is the only source
   * that sets this true. */
  bool has_start;
  int  wp_count;                            /* selectable wallpapers: 16 or 32 (PC Emerald) */
  uint8_t* (*records)(int box);             /* -> 30*80 records (loads/flushes for the bank) */
  uint8_t* menu_block;                      /* pc-layout buffer (records at +0x0004)        */
  void (*get_name)(int box, char out[12]);
  void (*set_name)(int box, const char* s);
  int  (*get_wp)(int box);                  /* wallpaper RENDER id (0..31)                  */
  void (*set_wp)(int box, int wp);          /* store wallpaper id                            */
  bool (*can_edit)(void);
  AppCommitFn commit;                       /* persist the current box now                   */
  void (*mark_dirty)(void);                 /* deferred persist (move-mode drop)             */
  void (*note_add)(const uint8_t* rec);     /* opt: a mon landed here -> register its dex (PC only; NULL on bank) */
  /* BACKLOG #56: opt, called with the new box every time SWITCH_BOX changes what is
   * on screen -- the exact same call site as app_note_pc_box() below, just not
   * gated on `!is_bank` (a GB session sets is_bank true but still wants to hear
   * this). NULL on the PC and the real Bank (their own re-entry story is
   * app_note_pc_box()/app_box_start_set() and stays exactly as it was); only
   * pdna_gen12_source() sets it, to remember which box a re-entry (START -> menu ->
   * back, or the bank-hand-off edge) should land back on instead of replaying
   * whatever box the session originally opened on. */
  void (*note_box)(int box);
  /* BACKLOG #40(a): the box banner's occupancy denominator. NULL (every source before
   * #40) means the Gen-3 grid's own 30 -- the PC/bank's real capacity, unchanged. A GB
   * source's box holds fewer (20 for Gen 2, 12-99 for Gen 1 depending on version) than
   * the 30-cell grid it is drawn into; without this the banner read "20/30" for a full
   * Gen-2 box, 10 short of full. */
  int  (*capacity)(int box);
  /* BACKLOG #94: renaming is NARROWER than can_edit -- it writes a name table, never
   * a Pokemon record, so a source that must refuse cross-scope drops (the Game Boy
   * source) can still offer it. NULL => fall back to can_edit(). Appended at the END
   * of the struct rather than beside set_name on purpose: every field before it has
   * a hand-verified byte offset recorded in tools/stack_edges.txt's BoxSource table,
   * and inserting a field in the middle would shift every one of them, forcing a
   * cascade of offset/qualifier edits to that already-fragile, hand-tuned table for
   * a purely cosmetic adjacency win. Tacking a new field on the end changes nothing
   * for any existing field's offset. */
  bool (*can_rename)(void);
  /* F1b: the name to SEED the rename editor with: get_name() is a DISPLAY string
   * and may carry decoration (the Game Boy source prefixes "GB "). NULL =>
   * get_name() is already the raw stored name (the Gen-3 PC and the Bank). */
  void (*get_raw_name)(int box, char out[12]);
  /* BACKLOG #120 S1: appended at the END for the same reason `can_rename` above was —
   * tools/stack_edges.txt's hand-verified BoxSource field-offset table is keyed by byte
   * offset, so a field inserted mid-struct would shift every offset after it. `scope`
   * (BOXSCOPE_PC/BANK/GB) is the carry discriminator described at the top of this file.
   * `bank_edge`: true makes the single-carry UP-past-the-tabs edge return 4 (open the
   * Bank) even though `is_bank` is already true (only the Game Boy source sets this —
   * the real Bank has nothing above it to hop to, so it stays false there). `can_lift`:
   * NULL => can_edit() (PC/Bank behave exactly as today); a source can narrow this to
   * refuse individual cells (e.g. a GB cell whose native record cannot be read cleanly)
   * without touching can_edit()'s save-wide meaning. `xfer`: NULL on every Gen-3 source,
   * forever; the opt-in BoxXferOps capability described above. */
  uint8_t scope;
  bool bank_edge;
  bool (*can_lift)(int box, int slot);
  const BoxXferOps* xfer;
  /* BACKLOG #93: appended at the END, same offset-stability rule `can_rename`'s own
   * comment above states (tools/stack_edges.txt's hand-verified BoxSource field-offset
   * table is keyed by byte offset; a mid-struct insert would shift every field after
   * it). All three NULL on PC/Bank, so their behaviour there is byte-identical to
   * before this field existed -- the pdna_box.c gate falls back to `src_can_lift`/the
   * existing export_box_all/release_box_all when unset (step 5's own design).
   *   can_boxops(box)   narrower than can_lift/can_edit -- the box-menu open gate for
   *                      a source whose box menu is unreachable through the ordinary
   *                      lift/edit capability (the finding that shapes this step: GB
   *                      hardwires can_edit false and can_lift is S3's transfer field,
   *                      neither means "the box options menu may open").
   *   export_all(box)    EXPORT ALL: writes one .pk1/.pk2 per occupied slot, driving
   *                      the same progress screen export_box_all uses.
   *   release_all(box)   RELEASE ALL: confirms with the count, then deletes every
   *                      slot in one persist (never a half-written box).
   */
  bool (*can_boxops)(int box);
  bool (*export_all)(int box);
  bool (*release_all)(int box);
} BoxSource;

/* Game-faithful box screen over `src`: a left PKMN DATA panel + a 6x5 icon grid on
 * a wallpaper, with a ◄ box-name ► banner. D-pad moves the cursor, L/R (and LEFT/
 * RIGHT on the title) change box, A opens the action menu / 6-card summary.
 * Returns 0 if the user backed out (B), 1 to switch to party (SELECT), or 2 for the
 * trainer/menu (START — only reachable when `!src->is_bank || src->has_start`). */
int pdna_box(BoxSource* src);

/* Reset the mon-in-hand carry state — call once when a save is (re)opened, since the
 * carry persists across pdna_box runs to survive the PC<->Bank hand-off. */
void pdna_box_clear_carry(void);

/* BACKLOG #120 S1: install (or, passed NULL, uninstall) the xfer peer a Bank visit reached
 * from a Game Boy session brackets around it (§4 gb_bank_visit). S1 wires only the setter
 * and the stored pointer, always called with NULL — no caller installs a real peer until
 * S2. Passing NULL also clears any xfer carry in progress (S3's `s_xfer`, not yet added). */
void pdna_box_xfer_set(const BoxXferOps* ops);

/* True iff the mon (or chunk) currently in hand originated from a BOXSCOPE_GB source —
 * the §2.3 exit-clear rule a GB session's exit hook needs (a Gen-3 carry, including a
 * fresh DUPLICATE, must survive a START > GB import visit exactly as it does today; a
 * GB-origin carry must never outlive the arena). S1 adds the query; no caller uses it
 * yet (S2 wires `gb_session_core`'s exit hook). */
bool pdna_box_carry_is_gb(void);

#endif /* PDNA_BOX_H */
