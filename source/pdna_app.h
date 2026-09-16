#ifndef PDNA_APP_H
#define PDNA_APP_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_mon.h"      /* PkMon (app_bank_hide_pending) */
#include "gen3_trainer.h"  /* PkGame (app_rom_path)         */
#include "gb_edit.h"       /* GbEditMon (AppSrcOps.copy_native)         */
#include "sprite_era.h"    /* SeRoms, SeSaveKind (app_era_roms/app_save_kind, E4) */

/* Shared app glue so the party list and box grid can open the editor and persist
 * safely. Implemented in pdna_main.c (which owns the loaded save + path). */

/* Hard rule 9 (one folder per tool): every file PokeDNA writes lives under this one
 * root. S5-B review fix (item #10): promoted here from a pdna_main.c-local #define
 * so pdna_gen12.c's sidecar code (and any other future writer) shares the SAME
 * literal instead of keeping its own copy that could silently drift from this one.
 * PDNA_SIDECAR_DIR is the one place both S5-B callers (pdna_main.c's
 * app_paste_gb_merge, pdna_gen12.c's gb_paste_write/gb_has_sidecar) get the sidecar
 * folder from. */
#define PDNA_DIR          "/PokeDNA"
#define PDNA_SIDECAR_DIR  PDNA_DIR "/sidecar"
/* BACKLOG #150 S150-6, decision 1: the one folder that holds both sidecars AND
 * transfer records going forward. PDNA_SIDECAR_DIR stays forever as the read
 * fallback (decision 4/D-Q7, G-M1) -- every existing card's files are never moved
 * or renamed, only copied. source/xfer_io.c's xr_path_for_key/xr_path_for_name are
 * the ONE place that resolves which folder a given key/filename actually lives in;
 * every reader and every in-place writer of an EXISTING record goes through them. */
#define PDNA_XFER_DIR     PDNA_DIR "/xfer"

/* Writes are EZ-Flash-Omega-only. */
bool app_can_edit(void);

/* BACKLOG #150 S150-8 decision 4: the loaded save's own Gen-3 origin-game id --
 * 1 Sapphire, 2 Ruby, 3 Emerald, 4 FireRed, 5 LeafGreen (Gen 3's origin field is 4
 * bits with no distinct value for Ruby vs Sapphire, so this cannot tell them apart
 * from the save alone -- it returns the SAME id pdna_main.c's own two GB-import call
 * sites already use, 1 for RS). Used as gen12_convert()'s Gb12Target.met_game for
 * a native cell converting DOWN into the Gen-3 PC (S11.20 item 12(b): the converted
 * mon's Gen-3 origin is the DESTINATION save's own game, never a fixed Emerald). */
uint8_t app_met_game(void);

/* Reason why writes are disabled (hack ROM vs. Omega cart). */
const char* app_readonly_why(void);
const char* app_readonly_footer(void);

/* Game Boy (Gen-1/2) variants: also honest about a streamed/view-only session,
 * which is neither the cart nor a hack ROM (review fix F2). */
const char* app_gb_readonly_why(void);
const char* app_gb_readonly_footer(void);

/* The session RNG every "create a Pokemon from nothing" caller seeds from (a counter
 * + this trainer's own TID + the cart RTC when present, NEVER the same across two
 * players or two carts) -- a thin public wrapper over pdna_main.c's own file-static
 * dc_seed(), exposed so pdna_gen12.c's gb_create_hook can share the identical
 * entropy source app_create_mon already uses instead of qran() (libtonc's PRNG,
 * whose seed is a FIXED constant, __qran_seed = 42, unless something calls sqran()
 * -- nothing in this tree does -- so every player's Nth created Gen-1/2 mon got the
 * IDENTICAL DVs/gender/shininess, G1 review BLOCKING-2). */
uint32_t app_session_seed(void);

/* g_vinfo.tid_public, narrowly exposed (BACKLOG #114): pdna_yard.c's dc_seed() moved
 * out of pdna_main.c and needs this ONE field without pdna_main.c exposing the whole
 * Gen3SaveInfo g_vinfo (golden rule 6, smallest scope -- the same posture app_session_seed
 * itself already takes for dc_seed as a whole). */
uint16_t app_tid_public(void);

/* Flush the RAM log to SD immediately (rmbl-paused). For anomaly evidence that must
 * survive a power-off; main-loop-synchronous callers only. A no-op on anything but
 * an EZ-Flash Omega — writes are Omega-only (hard rule 4), and a failed disk_write
 * would leave FatFs' write flag set and poison the mounted volume for reads too. */
void app_log_flush(void);

/* Save-open breadcrumb: "the box is on screen". Armed once per save-open by
 * view_save and fired by the box screen's first full paint; a no-op on every later
 * paint, so it costs exactly one SD write per opened save. */
void app_crumb_shown(void);

/* How an edited in-RAM `block` is persisted. Each kind of block (PC storage,
 * SaveBlock1, SaveBlock2, a bank box file) supplies its own verified-write
 * function; the mon menu / editors mutate `block` in RAM then call this to save.
 * Returns true iff a write happened. */
typedef bool (*AppCommitFn)(void);

/* Gen-3-PC-style action menu shown on A: SUMMARY / ITEM / MOVES / COPY / PASTE /
 * DUPLICATE / RELEASE on Omega (an empty slot offers PASTE only); straight to the
 * read-only summary on Everdrive. `block` is the pc-layout buffer the slot lives in
 * and `box`/`slot` locate the record within it (box = -1, slot = party index for
 * party callers). `commit` persists `block`. `is_bank` swaps the EXPORT action for
 * "TO GAME" (inject into the loaded save). `footer_y` is the caller's OWN footer
 * boundary (ui_layout.h's UI_FOOTER_Y for box/bank/party_list; the party overlay's
 * own PDNA_PTY_FOOTER_Y, pdna_layout.h, is well above it — its message box starts at
 * y=133, not the box/bank screens' y=150) — the popup windows/scrolls above THAT row
 * rather than the global one, so it never lands on a lower-sitting caller's own UI.
 * Returns true iff a write happened. */
bool app_mon_menu(uint8_t* rec, bool is_party, bool is_bank, AppCommitFn commit, uint8_t* block, int box, int slot, int footer_y);

/* ---- read-only BoxSource gate ------------------------------------------------
 * Editability has TWO axes and app_mon_menu used to know only one of them:
 *   - the CART (app_can_edit: writes are EZ-Flash-Omega-only), and
 *   - the SOURCE (BoxSource.can_edit: a mounted Gen-1/2 save is read-only no matter
 *     what cart it is running on — the conversion is one-way and the GB file is
 *     never written).
 * On an Omega, a read-only source therefore got the FULL menu: PASTE / RELEASE /
 * DUPLICATE / CREATE would mutate the source's RAM buffer, its commit() would
 * no-op, and the screen would show a change that never happened. pdna_box already
 * gates its own destructive paths on src->can_edit(); app_mon_menu could not,
 * because it is handed a record and a commit function, not the source.
 *
 * So a source that is read-only *as a source* registers itself for the duration of
 * its pdna_box() run. While registered:
 *   - app_mon_menu offers only VIEW / LEGALITY / COPY (+ the reason a specific
 *     record cannot even be copied), and
 *   - the singleton bank's deferred-delete bookkeeping (app_bank_hide_pending /
 *     app_bank_slot_pending) is skipped, because a foreign source's boxes are not
 *     the bank's boxes and a queued Bank->PC deletion must never hide one of their
 *     Pokemon.
 * Nothing registers by default, so every existing call site behaves exactly as
 * before. `why_locked(rec80)` returns NULL when that record may be copied, else a
 * short reason to show; `note` is a short line shown under the menu title (e.g.
 * "Converted copy"). Both may be NULL. */
void app_src_readonly_set(const char* (*why_locked)(const uint8_t* rec80), const char* note);
void app_src_readonly_clear(void);
bool app_src_readonly(void);
/* A read-only SOURCE whose records can nonetheless be edited IN PLACE through the
 * source's own pipeline (the resident Game Boy save, docs/GEN12-EDIT-DESIGN.md S2/S3):
 * the grid stays a lossy converted copy, so every hook addresses the GB record by
 * ADDRESS of `rec80` inside the paged buffer, exactly like why_locked. Registering this
 * adds EDIT / MOVE TO / RELEASE rows to the read-only popup (NULL = that row is not
 * offered); app_src_readonly_clear() drops all three. Every app_mon_menu call site
 * (pdna_box.c) discards a hook's return value and re-fetches src->records(box)
 * unconditionally, so the grid does not re-page on the return value at all -- it
 * re-pages because a true implementation sets its mount's `loaded = -1` before
 * returning, which forces the next records() to reload from the card. `ops` is
 * retained (not copied), so it must outlive the registration -- pdna_gen12.c's is a
 * static const. */
/* `copy_native` (S5-B, docs/GEN3-TO-GB-SIDECAR-DESIGN.md section 10) lets COPY on this
 * source's read-only popup additionally capture the record in ITS OWN native shape --
 * a Game Boy GbEditMon, not the lossy Gen-3-converted bytes the grid shows -- into the
 * clipboard, so a later PASTE in a Gen-3 session can look up the sidecar and merge the
 * ORIGINAL Gen-3 record back rather than re-degrading through the lossy path a second
 * time. NULL = no native record exists for this source (every source before S5-B).
 * Works on BOTH GB entry points as of review fix #5 -- see pdna_gen12.c's
 * gb_locate_addr/gb_copy_native_hook for how the read-only nav-menu mount (no edit
 * session) still reaches the raw bytes. Returns false (leaving `out` untouched) when
 * `rec80` cannot be resolved to a real record.
 *
 * `has_sidecar` (S5-B re-verification NEW-3, may be NULL) is set to whether the
 * captured record ALREADY has a /PokeDNA/sidecar/<key>.pds entry -- Gen 2 only (Gen 1
 * has no gen3_to_gb() target yet, S5-C, so it can never have one), and false whenever
 * the call itself fails. app_copy()'s toast uses this to say whether a later PASTE
 * will actually be lossless, instead of claiming "lossless" for every GB mon
 * regardless of whether it was ever transferred down. */
/* `paste` (S5-B, docs/GEN3-TO-GB-SIDECAR-DESIGN.md section 10, the DOWN direction) adds
 * a PASTE (GB) row to the read-only popup, but ONLY on an EMPTY cell and only while the
 * clipboard holds a Gen-3 record that did NOT itself come off a Game Boy source
 * (`g_clip.occupied && !g_clip.from_gb` -- pdna_main.c's app_mon_menu). `rec80` is the
 * empty cell's own address (exactly like every other hook here), so the hook can
 * gb_locate() the destination box; it never reads through `rec80` itself. Converts the
 * CLIPBOARD's Gen-3 record via gen3_to_gb(), writes the sidecar FIRST, then inserts
 * (gbs_insert) -- see pdna_gen12.c's gb_paste_hook for the full order and the
 * atomicity story. NULL = this source never accepts a paste (every source before
 * S5-B, and Gen 1 targets today -- see gb_paste_hook's own Gen-1 refusal). */
/* `view` (BACKLOG #41, "the edit page for gen 2 and 1 should feel the same as gen 3"):
 * lets a source's read-only popup open its OWN native summary/inspect screen for VIEW,
 * instead of app_mon_menu_readonly's default pdna_inspect() on the lossy Gen-3-
 * converted copy. NULL (every source before #41) keeps the old default. Handed the
 * cell's own rec80 exactly like every other hook here; returns whatever it returns
 * for logging purposes only — app_mon_menu_readonly always re-pages regardless. */
/* `editable` (bag/menu review fix): does the row this menu is about to draw actually
 * get to edit `rec80` if picked? Before this hook existed, the row's own label used
 * `edit && app_can_edit()` -- the CART gate only -- while the summary `view` actually
 * opens (gb_view_hook, pdna_gen12.c) additionally gates on gbs_box_writable(), the BOX
 * gate (a virgin Gen-1 bank can never be written even on an Omega). That mismatch let
 * the row say "VIEW/EDIT" on a box the summary would then silently refuse with a bare
 * buzz. NULL = no extra gate beyond the cart (app_mon_menu_readonly falls back to the
 * old `edit && app_can_edit()` expression); a source that has a box-level gate of its
 * own (pdna_gen12.c's gb_editable_hook) implements both checks here instead. */
/* `create` (BACKLOG #50): build a brand-new Pokemon into an EMPTY cell. Unlike every
 * other hook here it takes NO ARGUMENTS AT ALL -- not even the (box, slot) app_mon_
 * menu itself receives, which turned out to be the wrong source for either: an empty
 * cell has no rec80 to reverse-map through gb_locate() the way edit/move/release do,
 * but the naive fix of threading app_mon_menu's own `box`/`slot` parameters through is
 * ALSO wrong for an is_bank source (every GB session) -- pdna_box.c's own call sites
 * compute `int mbox = src->is_bank ? 0 : box;` before calling app_mon_menu, so a GB
 * session's `box` parameter is unconditionally 0 regardless of which box is actually
 * on screen (caught by hand on real emulator screenshots: CREATE from box 13, 17/20,
 * still refused "BOX FULL" because it was silently targeting box 0, 20/20). The
 * correct source is Gb12Mount.ui_box (BACKLOG #56, kept current by every box switch
 * via BoxSource.note_box) -- pdna_gen12.c's gb_create_hook reads that itself, with the
 * same "-1 (never switched) falls back to 0" rule pdna_gen12_source()'s own start_box
 * computation already uses, rather than trust a parameter this menu cannot supply
 * correctly. `slot` needs no equivalent: gbs_insert() always appends at the box's own
 * next free slot regardless of which empty cell the cursor was on, so the cell index
 * was never actually used. NULL = this source offers no CREATE (the empty-cell row
 * simply does not appear); a source that can build one implements it (pdna_gen12.c's
 * gb_create_hook). */
/* `item` (BACKLOG #92, Gen 2 only): adds an ITEM row to the read-only popup, right
 * after VIEW/EDIT and before LEGALITY -- the same relative position Gen 3's own
 * app_mon_menu uses for its A_ITEM row (pdna_main.c). Opens the SAME pick_item()
 * screen app_quick_item uses, restricted to ids 0..255 shown as "#n"
 * (pick_item_set_gen1_2_max(), the mode gb_editor.c's own GBE_ITEM row already
 * uses) -- #0 is NO_ITEM and REMOVES the held item, same as every other item
 * picker in this tree; it is not excluded, and doing so would make "take the
 * item off" unreachable from this row -- then commits through the identical
 * steps 3-5 EDIT does (BACKLOG #95 review C4/C5: gb_item_hook also refuses a
 * non-zero item on an Egg and confirms before setting Mail, since this tree
 * tracks no mailbox). NULL = this source has no held-item concept (Gen 1 --
 * gb_session_core installs a Gen-1 table with this left unset, so the row
 * never appears rather than appearing and refusing every press). */
typedef struct {
  bool (*edit)(uint8_t* rec80);
  bool (*move)(uint8_t* rec80);
  bool (*release)(uint8_t* rec80);
  bool (*copy_native)(const uint8_t* rec80, GbEditMon* out, bool* has_sidecar);
  bool (*paste)(uint8_t* rec80);
  bool (*view)(uint8_t* rec80);
  bool (*editable)(const uint8_t* rec80);
  bool (*create)(void);
  bool (*item)(uint8_t* rec80);
  /* BACKLOG #93: three more read-only-popup rows, appended at the end per the struct's
   * own append-only convention (see `create`'s comment above). All three mirror a
   * Gen-3 A_DUP/A_DAYCARE/A_EXPORT action for a source whose real record cannot travel
   * through the Gen-3 clipboard/commit path (app_mon_menu_readonly's own header
   * comment). NULL = the row is simply absent (the read-only menu's own omitted-row
   * convention), never shown-then-refused. */
  bool (*dup)(uint8_t* rec80);
  bool (*daycare)(uint8_t* rec80);
  bool (*export_one)(uint8_t* rec80);
  /* BACKLOG #166 (b166): appended at the end, same append-only convention as `dup`/
   * `daycare`/`export_one` above -- the read-only MOVE TO BOX row (RO_MOVE,
   * pdna_main.c's app_mon_menu_readonly) used to offer itself on ANY cell with a
   * `move` hook, regardless of whether THIS box/slot could actually be lifted --
   * the user only learned a Gen-1 one-mon party / Mail-holding Gen-2 party / an
   * unwritable box refused it after picking MOVE TO BOX, walking the box picker,
   * and having gbs_move() bounce (gb_move_hook's own msg_wait). `lift_why(rec80)`
   * answers the SAME question BoxSource.can_lift(box, slot) does, but as a reason
   * instead of a bare bool, so the row can be hidden up front with a one-line why
   * instead of a round trip to a refusal. Takes `rec80` (the record ADDRESS), the
   * same argument shape as `move`/`view`/`edit` above -- NOT (box, slot): the
   * caller (app_mon_menu_readonly) only ever has the address and app_mon_menu's own
   * `box` parameter, which pdna_box.c zeroes for any is_bank source (`mbox =
   * src->is_bank ? 0 : box`, and a GB session always sets is_bank true) -- so a
   * (box, slot)-shaped hook would silently read the wrong box for every box but 0.
   * The real body (gb_lift_why_hook, pdna_gen12.c) re-derives (box, slot) from the
   * address itself via gb_locate_addr(), exactly like every other hook here. NULL =
   * liftable (or the source has no opinion -- the row shows as it did before this
   * field existed); non-NULL = the reason text to show in place of the row.
   * gb_can_lift_hook_impl (BoxSource.can_lift's real body) now delegates to
   * gb_lift_why_hook too, so the bool and this reason can never disagree (one
   * source of truth, not two copies of the same rule table). */
  const char* (*lift_why)(const uint8_t* rec80);
} AppSrcOps;
void app_src_ops_set(const AppSrcOps* ops);

/* S5-B: the clipboard's raw 80-byte Gen-3 record, for a foreign source's own `paste`
 * hook (AppSrcOps.paste is handed the DESTINATION's rec80 only -- never the clip
 * itself). Points at g_clip's own bytes; valid to read for the duration of the paste
 * hook's call (nothing else runs in between the popup's A-press and the hook), and
 * only meaningful when the caller already knows the clipboard is occupied and NOT
 * `from_gb` -- app_mon_menu's own gate before PASTE (GB) is ever offered. Do not call
 * this speculatively and do not retain the pointer past the hook's return. */
const uint8_t* app_clip_rec(void);

/* S5-B review fix, renamed for BACKLOG #50: would app_mon_menu(rec80, ...) on THIS
 * empty cell right now offer PASTE (GB) OR CREATE? pdna_box.c's grid loop decides
 * whether to call app_mon_menu AT ALL on an empty cell before app_mon_menu ever runs
 * (`g_box[cur].species || src->can_edit()`), and a foreign read-only source's
 * can_edit() is always false -- so without this, an empty GB cell's A press was
 * silently swallowed and neither action was reachable. OR this into that gate. True
 * iff a read-only source is active AND EITHER: it registered a `paste` hook and the
 * clipboard holds a Gen-3 record that did not itself come off a Game Boy source (the
 * same check this predicate always made); OR it registered a `create` hook (which
 * decides for itself, per empty cell, whether it can actually build one there). */
bool app_src_empty_action_offered(void);

/* Bank "Copy to game": inject a stored 80-byte box record into the loaded save's
 * first free PC box slot (and commit). Returns true iff written. Omega-only. */
bool app_inject_to_game(const uint8_t* rec80);

/* The A-menu's MOVE action sets a one-shot flag; the box grid consumes it to
 * enter "pick up + reposition" mode. Returns true once per MOVE pick. */
bool app_take_move_request(void);

/* The A-menu's DUPLICATE action (box/bank only) sets a one-shot flag; the box grid
 * consumes it to copy the selected mon into a free slot and pick that copy up in the
 * glove, so the user positions it. Returns true once per DUPLICATE pick. */
bool app_take_dup_request(void);

/* A Day-Care withdraw-to-PC parks the mon in a free PC slot and sets a pending pickup;
 * the box grid consumes it on entry to open that box carrying the mon in the glove.
 * Returns true once and fills *box/*slot. */
bool app_take_pickup(int* box, int* slot);

/* Entry-cursor hint for the next box grid: 0 default, 1 = start on the top tabs,
 * 2 = start at the bottom row (used by the PC<->Bank up/down hand-off), 3 = start with
 * the PARTY strip already open (pdna_main.c's NV_PARTY routing — see pdna_box.c's
 * pcp_open_party_strip and its app_box_start_take()==3 caller). */
void app_box_start_set(int s);
int  app_box_start_take(void);
void app_note_pc_box(int b);              /* PC box screen reports its current box so the app remembers it */

/* BACKLOG #48: pdna_main.c's box-screen START menu (nav_menu, source/pdna_layout.h's
 * PDNA_NAV_ITEMS), reusable by any caller that owns its own BoxSource + pdna_box()
 * loop — namely pdna_gen12.c's gb_session_core, which wants the SAME menu for a raw
 * Game Boy save's own START press, with most rows unavailable (BACKLOG #49/#52 —
 * they act on Gen-3 SaveBlock state that does not exist in a GB session).
 *
 * `avail_mask` is a bitmask of `1u << NV_id`; NV_* itself lives in pdna_layout.h
 * (the X-macro list), kept OUT of this header so pdna_app.h — included from every
 * screen — stays free of screen-layout enums. A caller that needs to name a specific
 * item includes pdna_layout.h too (pdna_gen12.c already does, for its fixed strings).
 * A bit clear dims that row (still selectable) and makes picking it return
 * NAV_UNAVAILABLE instead of the item id, so the caller can show one honest message
 * instead of silently doing nothing. NAV_ALL_AVAILABLE reproduces the Gen-3 box
 * screen's own call byte-for-byte. */
#define NAV_ALL_AVAILABLE ((uint32_t)~0u)
#define NAV_UNAVAILABLE   (-2)     /* distinct from every NV_* id (0..NV_COUNT-1) and
                                    * from party_overlay/pdna_box's own -1 "cancelled" */
int  app_nav_menu(uint32_t avail_mask);
/* pdna_settings() (pdna_main.c) is static; this is its one exported door, for the
 * same reason app_nav_menu exists — a GB session's "Settings" nav row opens the
 * IDENTICAL screen, not a copy. */
void app_nav_settings(void);
/* BACKLOG #58: the shared "why not" for a nav row this save can't run -- consults
 * source/nav_avail.h's rule table and shows one honest msg_wait("COMING SOON" / "NOT
 * IN GEN 1" / ..., reason). `save_kind` is an SE_KIND_* value (see nav_avail.h). Two
 * callers: gb_nav_from_start (pdna_gen12.c) for every Game Boy row besides Settings/
 * Trainer/Back, and view_save's own Gen-3 nav switch (pdna_main.c) for a row
 * nav_avail() marks NOT_IN_GAME before it would otherwise dispatch into that row's
 * screen. A row nav_avail() calls NAV_OK is a caller bug, not a user-facing state --
 * this is a silent no-op then, not a wrong dialog. */
void app_nav_refuse(int nv_item, int save_kind);
/* Full-screen party screen (Gen-4/5-style: a big slot-1 box + 5 rows). ONLY caller left:
 * NV_PARTY (pdna_main.c) when the open save has NO PC storage at all (g_have_pc false) —
 * there is no box to show behind a strip in that case, so this full-screen browse-only
 * list (held=NULL, allow_move_to_box=false) is what stands in for it. Every OTHER path —
 * the box screen's own PARTY tab (pdna_box.c) AND NV_PARTY when g_have_pc IS true — opens
 * the compact party_strip_overlay (pdna_box.c, static, via pcp_open_party_strip) instead,
 * which leaves the box grid + PKMN DATA panel visible behind it
 * (docs/analysis-2026-08-20-pcparty/MEASUREMENTS.md — a DIFFERENT retail screen from the
 * field-menu list this function matches; Guy's own words on why NV_PARTY needed to move
 * off this function: "I expected to see the party menu on top of the PC pokemon in the
 * background"). Both share the same mutation core via the app_party_* accessors below, so
 * there is exactly one place that knows how to add/swap/browse a party slot.
 * PLACE mode (held != NULL): carrying a box mon -> A on a slot ADDS/SWAPS it into the party
 * (orig_box/orig_slot/orig_bank = the held mon's origin; can_swap = it has a clean PC origin
 * to receive a swapped-out member). Returns 1 (placed -> caller ends the carry) or 0.
 * GRAB mode (held == NULL): empty-handed -> A on a party mon picks it UP to move to a box:
 * fills grab80 (80-byte box form) + *grab_slot (party index) and returns 2; 0 = closed. */
int  app_party_overlay(const uint8_t* held, int orig_box, int orig_slot, bool orig_bank,
                       bool can_swap, uint8_t grab80[80], int* grab_slot, bool allow_move_to_box);
/* Remove party slot `idx` (gap-free) as a DEFERRED move (party -> box): stage SB1 + mark PC
 * dirty so it folds into the one exit save, and refresh the cached party. Called by the box
 * grid only when a carried party mon is successfully DROPPED into a box (lift-don't-clear). */
void app_party_remove_at(int idx);

/* ---- shared party-mutation/read core (pdna_main.c owns g_sb1/g_frlg; these let
 * pdna_box.c's party_strip_overlay act on the party without either duplicating the
 * SaveBlock1 offset math or exposing g_sb1 itself). Every one of these is the SAME
 * underlying logic app_party_overlay itself calls — see that function's body. */
int  app_party_n(void);                       /* party_count(g_sb1, g_frlg) */
int  app_party_read(PkMon out[6]);             /* decode + pk_resolve every party mon; returns n */
/* ADD (target == n) or SWAP (target < n) a held 80-byte box mon into the party. Same
 * contract/return as the PLACE branch above. */
bool app_party_place_held(const uint8_t* held80, int target, int orig_box, int orig_slot,
                          bool orig_bank, bool can_swap);
/* Open the full action menu (VIEW/EDIT, ITEM, LEGALITY, COPY, DUPLICATE, TO DAY-CARE,
 * EXPORT .pk, TAKE/GIVE ITEM, RELEASE, CANCEL) on party slot `slot`. If the user picks
 * MOVE TO BOX (only offered when allow_move_to_box), *tobox_hit is set true and
 * tobox_grab receives the 80-byte box form (the caller still owns removing the slot via
 * app_party_remove_at, same as app_party_overlay's own GRAB-mode contract) — otherwise
 * *tobox_hit is false and the return value is app_mon_menu's own "did it write" bool. */
bool app_party_mon_menu(int slot, int footer_y, bool allow_move_to_box,
                        uint8_t tobox_grab[80], bool* tobox_hit);

/* Shared framed message dialog (title + up to 2 lines + "Press A"). Used by every
 * screen's error/info popups; exposed here so party_strip_overlay (pdna_box.c) can show
 * the same read-only-cart denial app_party_overlay shows. */
void msg_wait(const char* title, uint16_t col, const char* l1, const char* l2);
/* Bank->PC / Bank->party carry: record the bank source slot AND the carried 80-byte record to
 * delete at the save phase (true move; matched by record so a re-arrange can't delete the wrong mon). */
void app_bank_defer_delete(int box, int slot, const uint8_t* rec80);
bool app_bank_defer_full(void);   /* 64-move queue full -> refuse the move (a silent drop would DUP) */
bool app_bank_defer_room(int n);  /* room for a whole chunk of n deferred deletions? (Bank->PC multi-move) */
void app_bank_defer_pop(int n);   /* undo the last n queued deletions (revert a Bank->PC move on write failure) */
int  app_bank_flush_deletions(void);  /* delete the queued Bank sources NOW — call ONLY after the PC destination is verified on disk.
                                        * BACKLOG #150 S150-8 decision 10: returns the number of
                                        * deletions that FAILED and are still queued (0 = all
                                        * flushed) instead of silently discarding failures. */

/* BACKLOG #150 S150-8 decision 9/G-F1: the ONE unpromoted native->Gen-3 transfer of
 * this session (16 B of plain .bss, pdna_main.c -- never EWRAM_BSS). `key`/`idx`
 * are the /PokeDNA/xfer entry xfer_down_write() just wrote at XR_STATE_PENDING.
 * app_xfer_promote() re-verifies the entry still matches before flipping it to
 * XR_STATE_CLAIMED once the PC destination is confirmed on disk; a failed
 * promotion just logs (the entry stays PENDING, fail-safe by construction).
 * app_xfer_pending_drop()/_undo() clear the slot -- drop() on a successful
 * promotion (nothing more to track), _undo() on a declined save (best-effort
 * gbsc_remove of the orphaned PENDING entry). */
bool app_xfer_pending(void);
void app_xfer_pending_set(uint64_t key, int idx);
bool app_xfer_promote(void);
void app_xfer_pending_drop(void);
void app_xfer_pending_undo(void);

/* A mon carried Bank->PC is deleted from the bank only at the save, but must LOOK gone at once.
 * hide_pending blanks those slots in a DECODED box (display only); slot_pending says a slot still
 * physically holds a moving-out mon (its only on-card copy) so nothing may overwrite it yet.
 * clear_slots does the cross-box MOVE's source clear+commit, refusing on an incomplete page-in. */
void app_bank_hide_pending(int box, PkMon g[30]);
bool app_bank_slot_pending(int box, int slot);
bool app_bank_clear_slots(int box, const uint8_t* slots, const uint8_t (*recs80)[80], int n);

/* Commit the loaded save's SaveBlock2 (section 0 — the trainer block) or
 * SaveBlock1 (sections 1..4 — where money lives) after an in-place edit of the
 * shared g_sb2 / g_sb1 buffers. Same verified-write+backup path as the editors.
 * Returns true iff a write happened. Used by the editable trainer card. */
bool app_commit_sb2(void);
bool app_commit_sb1(void);
bool app_commit_sb12(void);               /* SB2 + SB1 (sections 0..4) in one verified write */
bool app_commit_pc(void);                 /* PC storage (sections 5..13): box name/wallpaper */

/* Deferred-save for PC box MOVES. Repositioning mons in move-mode mutates g_pc in
 * RAM but does NOT write immediately (no per-drop "Saving" dialog); it marks the PC
 * dirty instead. The single "save the Pokemon you moved?" prompt fires when the
 * user leaves the open save (B out of the box/party screen). All OTHER PC edits
 * still commit immediately — and any such commit clears the dirty flag, since the
 * verified write flushes the whole g_pc (pending moves included). */
void app_mark_pc_dirty(void);
bool app_pc_dirty(void);

/* PC->Bank MOVE support: clear a PC box slot (release from the save) after the destination
 * bank box has been verified on SD, matched by the mon's 8-byte identity so a bystander is
 * never zeroed. Marks the PC dirty (committed at the one exit save). Omega-only in effect
 * (the caller gates on can_edit + only reaches this after a successful bank write). */
void app_pc_release_slot(int box, int slot, const uint8_t* id8);

/* Emerald "Walda" secret wallpaper: the graphic shown by box wallpaper 16. Pattern
 * is 0..15 (sWaldaWallpapers index) in SaveBlock1. app_walda_pattern returns -1 on
 * non-Emerald; app_set_walda edits g_sb1 in place (commit via app_commit_sb1). */
int  app_walda_pattern(void);
bool app_set_walda(uint8_t pattern);

/* Shared framed yes/no confirm (A = yes, B = no). */
bool app_confirm(const char* title, const char* l1);

/* U2b item 3: writes config.cfg NOW (Omega-only, same app_can_edit() gate every
 * config write already carries) -- the GB-screen shell calls this from its own
 * close path when gb_scale_mode changed during that screen's visit, so SELECT's
 * 1:1<->stretched toggle survives leaving ANY GB screen (not just Settings' own
 * B key, which already called this same writer directly before this existed). */
void app_cfg_save(void);

/* Per-place "moving sprites" toggles (Settings, persisted in config.cfg). Each
 * screen's idle bob/wiggle is gated on its own flag, so the user can calm one place
 * without killing the rest. Box/Party/Dex/Daycare default ON; the summary portrait
 * wiggle defaults OFF. Read by pdna_box / pdna_main (party + daycare) / pdna_pick
 * (dex) / pdna_summary. */
enum { ANIM_BOX,        /* PC + bank box-icon 2-frame bob   */
       ANIM_PARTY,      /* party list sprite bob            */
       ANIM_DEX,        /* Pokedex caught-cell bob          */
       ANIM_DAYCARE,    /* daycare scene mon bob            */
       ANIM_SUMMARY,    /* summary-card portrait wiggle     */
       ANIM_COUNT };
bool app_anim_enabled(int kind);   /* true iff animations are on for ANIM_<kind> */

/* ---- yard visitors + the icon-rent trio (BACKLOG #114) -----------------------
 * Exported so the Gen-1/2 Day-Care screen (pdna_gbdaycare.c) can draw the SAME
 * invented yard visitors + real-boarder icons pdna_main.c's own pdna_daycare()
 * does, instead of two bare text rows. All four were pdna_main.c-static before
 * this; bodies are unchanged. */

/* Draw the invented yard visitors? Only when the user asked for them (Settings >
 * Yard visitors) AND owns a registered ROM (so there is real icon art to draw
 * them with) AND that ROM's art is not switched off. A caller with this false
 * should say so on screen ("No visitors: register a Gen-3 ROM") rather than
 * silently show none. */
bool app_yard_visitors_ok(void);

/* The icon-store row a mon draws from (species+form, or the shared egg row 412).
 * Feed app_icons_hold()'s `rows` array with this. */
uint16_t app_icon_row_of(uint16_t species, uint8_t form, bool egg);

/* Rent/release the icon-store rows a screen's idle-bob paint needs (up to N
 * declared with app_icons_hold(), given back the instant the idle loop ends —
 * see pdna_main.c's own long comment on why the window is exactly "paint to
 * first key", never wider). MUST bracket every paint that calls
 * mon_icon_for_form_frame/mon_icon_egg_frame inside an idle-bob loop; a paint
 * that never persists anything (a pure VIEW) does not strictly need the
 * bracket, but every existing caller uses it uniformly rather than special-
 * casing "this one path never persists" — see pdna_gbdaycare.c's own use for
 * the shape to copy. */
void app_icons_hold(const uint16_t* rows, int n);
void app_icons_drop(void);

/* ---- borrowed EWRAM arena (the map screen) ---------------------------------
 * EWRAM has only ~6 KB genuinely free, and the map viewer needs ~33 KB for a
 * decompressed tileset. Rather than adding a buffer that would not link, the map
 * screen BORROWS `g_pc` — the reassembled PC storage. That buffer is the right
 * donor for three reasons: it is a single CONTIGUOUS 35,712-byte block (the other
 * candidates are scattered across translation units and cannot form one arena), it
 * is *derived* from g_save so it can be rebuilt byte-exactly on release, and the
 * map screen has no use for PC boxes.
 *
 * Deliberately NOT donors: g_save / g_sb1 / g_sb2 — the map screen must read g_sb1
 * for the player's position and write through g_save to commit a teleport, so
 * aliasing those is exactly the hard-rule-3 failure mode.
 *
 * app_arena_acquire() FAILS (returns NULL) when the PC is dirty, because unsaved box
 * moves live only in g_pc and would be destroyed. Callers must handle NULL by telling
 * the user to save first — never by proceeding.
 * app_arena_release() re-derives g_pc from g_save. */
#define APP_ARENA_BYTES (9 * 3968)          /* == G3_PC_BYTES, 35712 */
uint8_t* app_arena_acquire(uint32_t need);  /* NULL: too big, already held, or PC dirty */
void     app_arena_release(void);
bool     app_arena_held(void);

/* BACKLOG #150 S150-8 decision 16(b)/G-F2: a live Gen-3 PC in g_pc right now --
 * !app_arena_held() && a parsed save (g_vinfo.valid). False whenever a Game Boy
 * session has borrowed this arena (xg_pc_live, source/xfer_gate.h). */
bool app_gen3_pc_live(void);

/* ---- borrowed EWRAM cache (the box screen's SD/cache-sourced pose-swap frame-1s) ----
 * The real Gen-3 2-frame icon pose swap needs a persistent 15,360 B cache (30 grid
 * slots x one 512 B "other pose" frame) so a per-tick swap never touches the SD card
 * (box_oam.c's own header: OBJ VRAM only has room for ONE frame per icon, so animating
 * means re-uploading the other frame's tiles, and an SD read inside a vblank tick is
 * forbidden). The build's genuine EWRAM headroom (748 B, measured on both the full-art
 * and artless builds — box_oam.c's MUST-FIX 2 comment) cannot hold that — but
 * `g_entries` can donate it: 256 x 76 B = 19,456 B, and it is IDLE for the box screen's
 * entire lifetime. main()'s outer loop is
 *     for (;;) { if (browse_pick(path, ...)) view_save(path); }
 * so scan_dir()/browse_pick() (the ONLY code that ever touches g_entries — it is
 * `static` to pdna_main.c) has always finished before view_save(), and therefore the
 * box screen (which lives entirely inside view_save()), is ever entered. No runtime
 * dirty-check is needed the way app_arena_acquire needs one for g_pc: g_entries holds
 * no state that survives past the browser screen — scan_dir() unconditionally rebuilds
 * it from the SD directory listing the next time the browser is shown, never from
 * anything a caller could have left behind in it.
 *
 * box_oam.c acquires this exactly once per box-screen visit, inside boxoam_enter(), and
 * releases it inside boxoam_exit() — every one of pdna_box()'s return paths already
 * funnels through boxoam_exit(), so the borrow can never leak past a visit. A failed
 * acquire (NULL) is not fatal: box_oam.c degrades that box's non-cheap-ROM/cache-sourced
 * slots to the existing 1 px positional bob, exactly as it did before this cache
 * existed. */
#define APP_BOX_SWAP_BYTES (30u * 512u)             /* == 15360 */
uint8_t* app_box_swap_acquire(uint32_t need);  /* NULL: too big, or already held */
void     app_box_swap_release(void);

/* ---- borrowed-cache canary (2026-08-23, hardware A/B for the box-screen crash) ----
 * app_box_swap_acquire() stamps a known 16-byte pattern into TWO places a runaway
 * write into/around g_entries would hit before anything else does:
 *   - the tail of g_entries itself, past the APP_BOX_SWAP_BYTES the cache ever
 *     writes (4,096 B of slack before g_entries's own EWRAM neighbour, g_sb1 --
 *     the reassembled SaveBlock1, confirmed live, confirmed adjacent with ZERO
 *     padding via `arm-none-eabi-nm -S` on the ARTLESS binary, 2026-08-23);
 *   - the last 16 bytes of g_cwd, the buffer immediately BEFORE g_entries (same
 *     zero-gap adjacency) -- saved first and restored by app_box_swap_release, so
 *     a real (long) current-directory path is never actually altered.
 * app_box_swap_canary_ok() re-checks both every call and returns false (after
 * logging ONE loud RAM-log line per canary, not spamming) the first time either
 * changes; true whenever the borrow isn't held, so callers can call it
 * unconditionally every box tick at zero cost when the feature is off. This function
 * itself never touches the SD card (log_line is a RAM-buffer append, not a card
 * write) -- calling app_log_flush() unconditionally from inside a vblank tick is
 * exactly the hazard e37f14b's revert already flagged for the breadcrumb trail (a
 * flush EVERY tick, forever, healthy or not). Its one caller, box_oam.c's
 * boxoam_pose_pump(), is NOT unconditional the same way: the first time this
 * function returns false, it flushes ONCE (latched, exactly like this file's own
 * upload_tiles_verified/draw_wallpaper anomaly-flush idiom) and raises a zero-I/O
 * on-screen + audible cue, specifically so the one run that proves an overrun does
 * not throw its own proof away — see that function's own comment for the full
 * contract. A canary line in logs/log.txt after a crash is therefore close to
 * definitive (it is written in the SAME call that detected the trip, before
 * anything else in that tick runs); ABSENT still isn't a clean bill of health if the
 * crash was fast enough to abort the flush itself mid-write. */
bool app_box_swap_canary_ok(void);

/* Path of the Pokemon ROM the map screen reads map data from, remembered per game
 * (RS / Emerald / FRLG) because their map data differs and each needs its own ROM.
 * Returns "" when none has been picked yet. Set persists via config.cfg. */
/* ---- descriptions: the user's ROM first, the embedded table only as a fallback ----
 * Item/move/ability description TEXT is the games' own prose, and shipping it verbatim
 * is the largest remaining legal liability in the binary (741 strings). These three
 * read it from the registered/fused ROM when one is available, and fall back to
 * whatever the build has otherwise — so a ROM-gated build can drop the embedded copy
 * entirely without any screen losing its layout.
 *
 * The returned pointer is valid until the NEXT call of the same function (each keeps
 * one small static buffer). Copy it if you need to hold it. Never NULL. */
const char* app_item_desc(uint16_t item_id);
const char* app_move_desc(uint16_t move_id);
const char* app_ability_desc(uint16_t ability_id);

const char* app_rom_path(PkGame game);
/* Returns false (refused, unchanged) when `path` would not fit the shared
 * GB_ROM_PATH_MAX(128) cap (gb_art_source.h) -- E3 review item 3: silently
 * truncating used to be able to register a DIFFERENT, likely nonexistent path than
 * the one the user picked. An empty path (clearing) always succeeds. */
bool        app_rom_path_set(PkGame game, const char* path);
/* True once the user has registered ANY of their own game ROMs. The gate for extras
 * that need real art to mean anything (Day-Care yard visitors). */
bool        app_any_rom_registered(void);

/* BACKLOG #77: open the Gen-3 ROM wallpaper rung once for a GB session, artless
 * builds only -- a no-op if it is already open (nested import) or in the normal
 * build (the compiled wallpapers.c already serves the GB grid there). See the
 * definition in pdna_main.c for the full reasoning. */
void        app_gb_wallpaper_rom_open(void);

/* ---- Game Boy cartridge ROMs (Settings > Game ROM, slice E3) -----------------------
 * Same shape as app_rom_path()/app_rom_path_set() above, one slot per generation
 * (`gen` is PDNA_GEN1==1 or PDNA_GEN2==2, pdna_origin_art.h) instead of one per Gen-3
 * game — Gen-1 and Gen-2 sprite/palette tables are shaped too differently to share a
 * ROM the way RS and Emerald can't either. Persists via config.cfg's romgb1/romgb2
 * keys, restored by cfg_load(). Backed by a genuine resident array (gb_art_source.c's
 * gb_art_register() only VALIDATES a path; it does not remember it) sized
 * GB_ROM_PATH_MAX (128, not PATH_MAX's 256 — see gb_art_source.h's memory note); as
 * of E3 review item 3 the SAME array and cap as app_rom_path()'s three Gen-3 slots
 * (g_rom_path[5][GB_ROM_PATH_MAX], pdna_main.c). An out-of-range `gen` returns ""/
 * is a no-op. Returns false (refused, unchanged) when `path` would not fit — see
 * app_rom_path_set()'s comment, identical rule. */
const char* app_gb_rom_path(uint8_t gen);
bool        app_gb_rom_path_set(uint8_t gen, const char* path);
/* True iff generation `gen` can currently serve art -- an explicit Settings/boot
 * registration OR (while a raw GB save of some kind is open) a "beside the save"
 * session fallback that already proved out (gb_art_source.h's gb_art_have(), which
 * this proxies verbatim). "Openable" is what SeRoms.have[] (sprite_era.h, E4) wants,
 * not "the user visited a Settings row", so the fallback counting is deliberate. */
bool        app_gb_rom_registered(uint8_t gen);

/* BACKLOG #150 S150-7 D-Q7: the mounted GB session's own generation (GB_GEN1/GB_GEN2),
 * or 0 outside a GB session -- pdna_box.c's bank_down_dispatch needs this for
 * xg_bank_down_arm()'s `dst_gen` argument and cannot reach it through the xfer vtable
 * (see pdna_gen12.c's own comment on this function for why). */
uint8_t     app_gb_session_gen(void);

/* BACKLOG #47: Settings > Game ROM's "Turn ROM art OFF" switch (g_rom_art_off,
 * pdna_main.c) -- "no ROM art at all", not "no Gen-3 ROM art". Exposed here so
 * gb_art_source.c (a lower module that never reaches into pdna_main.c's statics
 * directly) can gate gb_art_have()/the pic()+icon() vtable callbacks on it exactly
 * like the Gen-3 chokepoints (app_icon_rom_open, g3cross_pic_cb) already do. The
 * registrations themselves (config.cfg's romgb1/romgb2, the Gen-3 g_rom_path[]
 * slots) are never touched by the switch -- only what THIS function reports. */
bool        app_rom_art_off(void);

/* The path of the currently open save (g_path — set the moment view_save() opens a
 * file, valid for the whole time a save is open) and whether that save is itself a
 * raw Game Boy battery file rather than a Gen-3 .sav. Both back gb_art_source.c's
 * "ROM beside the save" fallback (S5-C's rule): it only makes sense to go looking for
 * a sibling .gb/.gbc when the thing actually open IS a Game Boy save, never when a
 * Gen-3 save merely CONTAINS a Game Boy-origin import. */
const char* app_current_save_path(void);
bool        app_current_save_is_gb(void);

/* ---- sprite-era resolver plumbing (E4, docs/SPRITE-ERA-DESIGN.md) -----------------
 * pdna_main.c owns the SeSetting (g_era) and registers a PdnaEraResolverFn
 * (pdna_origin_art.h) that wraps se_resolve() with these two lookups plus the
 * setting itself -- these are exported mainly so the resolver callback and the
 * Settings "Sprites" grid can build a SeRoms/read the current kind without a second
 * copy of the plumbing. */

/* Which era ROMs are openable right now, one flag per SeEra (sprite_era.h). The
 * three Gen-3 slots mirror app_rom_path() (a non-empty path is "registered", exactly
 * what the icon/map/description rungs already treat as available); the two Game Boy
 * slots proxy app_gb_rom_registered(), which — see that function's own comment —
 * already exists FOR this. SE_ERA_NATIVE's slot is left false (se_resolve never
 * reads it; se_era_available()/se_cell_applies() special-case NATIVE as always
 * "available" without consulting SeRoms at all). */
SeRoms app_era_roms(void);

/* The save kind se_resolve() should use RIGHT NOW: se_kind_from_game(g_game) while a
 * Gen-3 save is open, or SE_KIND_GEN1/SE_KIND_GEN2 while a Game Boy session's box
 * screen is on screen (pdna_gen12_active_kind(), pdna_gen12.h) -- both are "set by
 * view_save()" in the sense that view_save() is the one call that ever puts either
 * kind of save on screen; which of the two branches answers is simply whichever kind
 * of session is currently live. Defaults to whatever g_game's own compiled default
 * is (PK_EMERALD) before the first save is ever opened, same as g_game itself. */
SeSaveKind app_save_kind(void);

/* ---- items + type badges: the compiled accessor first, the registered ROM second
 * (Phase 1, docs/analysis-2026-08-19-rom-art/DESIGN.md Sec 4.7) --------------------
 * Same shape as item_icon_for()/type_icon_for() (item_icons.h/type_icons.h) --
 * 0/0x8000|rgb RGB15, NULL on no art -- so these drop straight into those call
 * sites. `out_h` (may be NULL) receives the real badge height: TYPE_ICON_H (14) for
 * compiled art, 16 (RSE) or 12 (FRLG) for a ROM-served one -- callers that assumed a
 * fixed 14 need it, since a ROM badge is not the same crop.
 *
 * The returned pointer is valid ONLY until the next call to EITHER of these two (a
 * ROM-served icon decodes into the shared mon_decomp scratch, the same buffer the
 * summary portrait uses) -- draw it before making another call, exactly like every
 * other mon_decomp-backed accessor in this codebase. */
const uint16_t* app_item_icon(uint16_t item_id);
const uint16_t* app_type_badge(uint8_t type_id, uint8_t* out_h);

/* Decode a native Bank cell ("GBC1", source/bank_cell.h) into a PkMon, via the SAME
 * display ladder the Bank grid uses (source/gb12_render.h) -- so a native cell reads
 * the same everywhere it is decoded. Defined in pdna_box.c (review F1, BACKLOG #150
 * S150-2): app_mon_menu (pdna_main.c) needs this so a native cell's title/occupancy
 * do not come from pk_decode_mon running on bytes it cannot decrypt (a meaningless-
 * key decrypt -- "??? ? ?" titles, `occupied` a coin flip, CREATE/PASTE HERE
 * reachable over a cell that is never actually empty). `__attribute__((noinline))`:
 * this function's own GbEditMon + Gb12Mon + 80-byte scratch must never join a
 * caller's frame -- both pdna_box.c's box_decode_to and pdna_main.c's app_mon_menu
 * sit on stack-budget-gated chains. `hint` may be NULL. */
void pdna_native_cell_decode(const uint8_t* cell, PkMon* out, uint8_t* hint);

#endif /* PDNA_APP_H */
