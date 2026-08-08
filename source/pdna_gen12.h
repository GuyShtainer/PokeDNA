#ifndef PDNA_GEN12_H
#define PDNA_GEN12_H

#include <stdint.h>
#include <stdbool.h>

#include "gen1_save.h"
#include "gen2_save.h"
#include "gen12_convert.h"
#include "pdna_box.h"        /* BoxSource */

/*
 * pdna_gen12 — mount a Game Boy (R/B/Y, G/S, Crystal) battery save as a READ-ONLY
 * BoxSource, so the existing Gen-3 box screen can show its Pokemon and the existing
 * clipboard can carry one forward.
 *
 * THERE WAS NEVER AN OFFICIAL GEN 1/2 -> GEN 3 TRANSFER (gen12_convert.h says why).
 * Everything this screen shows is a CONVERSION PokeDNA invents; the mons in the grid
 * are converted copies, not native Gen-3 Pokemon, and the UI says so out loud.
 *
 * READ-ONLY IS STRUCTURAL, NOT A POLICY FLAG:
 *   - can_edit() is a constant false, so pdna_box refuses move mode, item mode,
 *     multi-select, rename, wallpaper and every empty-slot action;
 *   - commit() is a no-op that returns FALSE, which is load-bearing: pdna_box's
 *     cross-scope drop path (drop_held / drop_chunk_pc_to_bank) writes the record
 *     into the destination, calls commit(), and REVERTS the destination when commit
 *     fails. A commit() that lied and returned true would make the box screen
 *     believe a mon had been written into a GB save that we never write to;
 *   - nothing in this file opens a file for writing. Ever.
 *
 * WHY is_bank = true. `is_bank` in pdna_box means "this source is not the loaded
 * save's PC", and setting it removes exactly the paths that would be dangerous here:
 * the PARTY tab (which could otherwise carry a live party Pokemon into a buffer that
 * is never persisted — clear_origin() would then remove it from the party for real),
 * the START/nav hand-off, and the "up past the PC top" transition. The one piece of
 * genuine bank bookkeeping it also switches on — hiding slots queued for a Bank->PC
 * deferred delete — is suppressed for read-only sources in pdna_main.c
 * (app_bank_hide_pending / app_bank_slot_pending consult app_src_readonly()), because
 * a GB box has nothing to do with the bank's queue and a GB Pokemon must never be
 * hidden by it.
 *
 * MEMORY. The hardware build has ~1.5 KB of free EWRAM, and this needs ~4.7 KB
 * (2404 B of box records + a 1152 B list staging buffer + the mount state + one FatFs
 * FIL). So it borrows the arena (app_arena_acquire, pdna_app.h) exactly like the map
 * screen: no new statics, and the arena's own rule — refuse when the PC is dirty —
 * is surfaced to the user ("save your moved Pokemon first"). While the arena is held,
 * g_pc is not the PC any more, which is why the read-only mon menu offers COPY (the
 * 80-byte clipboard) and NOT "TO GAME" (which writes into g_pc).
 *
 * PURE-C CORE + THIN GBA GLUE, IN ONE FILE. Everything above pdna_gen12_show() is
 * pure C (<stdint.h>/<string.h> + a caller-supplied seek/read callback) so
 * tests/host_gen12_test.c can build the REAL BoxSource over a synthetic image and
 * assert the determinism the clipboard depends on. The FatFs/tonc half is compiled
 * out with -DPDNA_GEN12_HOST. The read callback is the only difference between the
 * two builds: on the GBA it is f_lseek+f_read, in the test it is a memcpy.
 *
 * Design: docs/research-gen12.md section 6. Scope decision (one-way, view-first):
 * docs/OVERNIGHT-DECISIONS.md.
 */

/* ---- geometry ------------------------------------------------------------- */

#define GB12_SLOTS        30                       /* the Gen-3 grid is 6x5      */
#define GB12_RECS_BYTES   (0x0004 + GB12_SLOTS * 80)   /* pc-mini layout: 2404   */
/* Big enough for the largest list blob either generation has: Gen-1 box 0x462
 * (1122) and Gen-2 box 1102. Rounded up to a multiple of 4. */
#define GB12_STAGE_BYTES  1152
#define GB12_REPORT_MAX   16                       /* remembered problem slots   */
/* A wallpaper no in-save box uses by default, so "this is not your Gen-3 PC" is
 * readable at a glance from across the room. */
#define GB12_WALLPAPER    13

typedef enum {
  GB12_SAVE_NONE = 0,
  GB12_SAVE_RBY,          /* western Red/Blue/Yellow      */
  GB12_SAVE_GS,           /* western Gold/Silver          */
  GB12_SAVE_CRYSTAL       /* western Crystal              */
} Gb12SaveKind;

/* Seek+read callback. Identical signature to Gen1ReadFn / G2ReadFn on purpose — one
 * shim serves both parsers and this module. Must fill exactly `len` bytes from file
 * offset `off` or return false. */
typedef bool (*Gb12ReadFn)(void* ctx, uint32_t off, void* buf, uint32_t len);

/* One slot that will NOT convert, remembered so the user can be shown where their
 * Pokemon went instead of being left to conclude the tool lost it. */
typedef struct {
  uint8_t  box;           /* box index (party == Gb12Mount.party_box)            */
  uint8_t  slot;
  uint8_t  reason;        /* a Gb12Result                                        */
  uint16_t species;       /* national dex, 0 when the record has no real species */
} Gb12Report;

/* The whole mount. Deliberately a plain struct the CALLER places (the GBA build puts
 * it in the borrowed arena, the host test on its stack) — this module allocates
 * nothing, exactly like gen1_save/gen2_save. */
typedef struct {
  Gb12SaveKind kind;
  Gb12ReadFn   rd;
  void*        ctx;
  uint32_t     len;           /* file length, RTC tail included                  */
  Gb12Target   tgt;           /* origin game stamped on every converted record   */

  Gen1Save  g1;               /* kind == RBY                                     */
  G2Save    g2;               /* kind == GS / CRYSTAL                            */
  G2Offsets g2o;
  uint16_t  tid;              /* public trainer id, both generations              */
  int       g2_gender;        /* -1 unknown (G/S stores none)                    */
  char      player[G2_NAME_BYTES];
  uint8_t   g2names[G2_NUM_BOXES * 9];   /* raw box-name block, decoded on demand */

  int nboxes;                 /* real storage boxes: 12 (Gen 1) / 14 (Gen 2)     */
  int party_box;              /* == nboxes; the party is exposed as one more box */
  int current_box;            /* the box whose LIVE copy is in main data         */

  uint8_t* recs;              /* caller's GB12_RECS_BYTES pc-mini buffer         */
  uint8_t* stage;             /* caller's GB12_STAGE_BYTES list staging buffer   */

  int      loaded;            /* which box `recs` holds, -1 = none               */
  uint8_t  capacity;          /* slots the loaded box can hold (20 / 6)          */
  uint8_t  reason[GB12_SLOTS];/* per slot of the loaded box: a Gb12Result        */

  /* Whole-save census, taken once at mount (cheap: gen12_can_convert only, no PID
   * search) so the info screen can be honest before the user goes looking. */
  int nstored;                /* Pokemon the GB save says it holds               */
  int nready;                 /* ...that will convert                            */
  int nblocked;               /* ...that are shown but cannot be copied          */
  int nunreadable;            /* ...that have no Gen-3 form at all (glitch mons) */
  int nreport;
  Gb12Report report[GB12_REPORT_MAX];
} Gb12Mount;

/* ---- pure-C core ---------------------------------------------------------- */

/* Could a file of this size be a GB battery save? 32 KiB, plus the 44/48-byte MBC3
 * RTC footer emulators append (bgb.bircd.org/rtcsave.html). Deliberately a SIZE
 * test only — the file browser calls this before anything is parsed. */
bool pdna_gen12_size_is_gb(uint32_t size);

/* Identify + open. Tries Gen 2 FIRST (a 16-bit checksum over ~3 KB is a far stronger
 * claim than Gen 1's single 8-bit sum, so a Gen-1 save has ~1/65536 odds of being
 * mistaken for Gen 2 where a Gen-2 save would have ~1/256 odds the other way), then
 * Gen 1. `recs` and `stage` are the caller's buffers; the mount keeps the pointers.
 * On failure returns false and, if `why` is non-NULL, points it at a fixed string
 * explaining the refusal — never a generic "bad file". */
bool pdna_gen12_mount(Gb12Mount* m, Gb12ReadFn rd, void* ctx, uint32_t len,
                      uint8_t* recs, uint8_t* stage, uint8_t met_game,
                      const char** why);

const char* pdna_gen12_kind_name(Gb12SaveKind k);

/* Total boxes the source exposes: storage boxes + the party pseudo-box. */
int  pdna_gen12_nboxes(const Gb12Mount* m);

/* Banner name for `box` ("GB BOX 3", "GB PARTY", or Gen 2's own box name prefixed
 * with "GB "). Always NUL-terminated, never splits a UTF-8 sequence. */
void pdna_gen12_box_name(const Gb12Mount* m, int box, char out[12]);

/* Page box `box` into m->recs and return the RECORDS pointer (m->recs + 4).
 * DETERMINISTIC: the same box always produces the same 2400 bytes, because every
 * derived value (PID included) is a pure function of the GB record plus its slot
 * (gen12_convert.h). The clipboard and the bank match mons by the first 8 bytes of
 * a record, so a non-deterministic page-in would silently break identity matching —
 * which is what the host test pins. Returns NULL only if `box` is out of range. */
uint8_t* pdna_gen12_page(Gb12Mount* m, int box);

/* Why slot `slot` of the currently paged box cannot be copied (GB12_OK = it can). */
uint8_t pdna_gen12_slot_reason(const Gb12Mount* m, int slot);

/* One short sentence for the mon menu / the report list. `r` is a Gb12Result. */
const char* pdna_gen12_block_reason(uint8_t r);

/* The COPY veto app_mon_menu registers through app_src_readonly_set(): NULL when the
 * record at `rec80` may be copied, else the reason it may not. `rec80` must be a
 * pointer INTO the paged box buffer (which is what pdna_box hands the menu), because
 * the slot is identified by ADDRESS — two GB Pokemon can share every visible field,
 * so matching on contents would be a guess. Anything outside the buffer is not ours
 * and is never vetoed. */
const char* pdna_gen12_why_locked(const uint8_t* rec80);

/* The line the user must see, in as many places as it fits. */
const char* pdna_gen12_honest_line(void);

/* Fill a BoxSource over `m` and make it this module's active mount. The hooks act on
 * module-singleton state (the BoxSource contract takes no `self`), so exactly one GB
 * save is mounted at a time. Pass NULL to unmount. */
BoxSource pdna_gen12_source(Gb12Mount* m);

/* ---- GBA glue (FatFs + tonc) ---------------------------------------------- */
#ifndef PDNA_GEN12_HOST
/* Open the GB save at `path`, show its info page, then browse its boxes through the
 * shared box screen until the user backs out. `met_game` is the Gen-3 origin id to
 * stamp on converted records (1 Sapphire .. 5 LeafGreen; 0 -> Emerald) — pass the
 * LOADED save's game so a copied mon claims the cartridge it is going into.
 * Borrows the EWRAM arena for the duration; returns 0 always (a screen, not a
 * chooser). Safe on every cart: it only ever reads. */
int pdna_gen12_show(const char* path, uint8_t met_game);
#endif

#endif /* PDNA_GEN12_H */
