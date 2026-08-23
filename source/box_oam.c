/*
 * Hardware-OAM layer for the PC/Bank box screen — see box_oam.h for the design.
 *
 * The grid of 32x32 box icons + the overlays that sit on top of them are GBA OBJ
 * sprites. The wallpaper / banner / panel / tabs / footer stay software on the BG2
 * Mode-3 bitmap (sprites composite above them, for free, every frame).
 *
 * OBJ-VRAM tile budget (bitmap mode -> only ids 512..1023 usable):
 *   ids 512..991  (480) : the 30 grid icons (slot s -> 512 + s*16, 16 tiles each)
 *   ids 992..1007 ( 16) : cursor hand (32x32)            [region A, permanent]
 *   ids 1008..1023( 16) : grab fist (move) OR item glyphs (ITEM)  [region B, shared]
 * Exactly 512 tiles. There is no slack for a further icon kind, which is why the PC-box
 * party panel (boxoam_strip_open/slot/slot1/close, below) draws its OWN 6 icons by
 * borrowing HIDDEN grid slots' existing 512..991 tile ranges and OAM entries instead
 * of claiming any of its own — see that group's own comment.
 *
 * OBJ palette banks (16 total): 0..12 = the 13 shared icon palettes (uploaded once);
 *   13 = hand (normal) + grab + item glyph; 14 = hand (orange, MOVE); 15 = carried
 *   item icon.  ITEM "translucent" hand = OBJ alpha-blend (bank 13 + BLDCNT).
 */
#include <tonc.h>
#include <string.h>
#include "box_oam.h"
#include "mon_icons_oam.h"
#include "hand_oam.h"
#include "item_icons.h"
#include "rom_mon.h"      /* phase-1 ROM-streamed icons (artless + the user's own ROM) */
#include "art_icons_cache.h" /* phase-2 icons.bin cache -- tried BEFORE the rom rung   */
#include "hand_gate.h"       /* PDNA_HAND_ART_COMPILED -- see that header for why      */
#if !PDNA_HAND_ART_COMPILED
#include "rom_hand.h"        /* phase-3 ROM-streamed glove (DESIGN.md Sec 1.3/4.5)     */
#endif
#include "rumble.h"         /* rumble_io_suspend: freeze GPIO during verified CPU ROM reads */
#include "log.h"            /* icon-upload self-verify diagnostics */
#include "pdna_app.h"       /* app_log_flush: anomaly evidence must survive a power-off */
#include "snd.h"            /* snd_deny: zero-I/O audible+haptic canary-trip cue */
#include "perf.h"           /* PERF_ICON -- this ladder's rung counters (telemetry only) */

/* ---- hardware A/B switch for the borrowed-cache pose-swap crash (2026-08-23) --------
 * boxoam_enter()'s own comment (below) has the incident writeup. Every test run so far
 * changed TWO independent variables at once -- borrowing g_entries, AND performing the
 * VRAM<->cache DMA exchange -- so nothing so far can say which one the runaway is. This
 * switch separates them into three single-variable builds, plus the always-shippable
 * OFF state (bit-for-bit e37f14b: no borrow, no swap, 1 px bob everywhere non-cheap):
 *
 *   0 = OFF (default -- what ships).
 *   1 = 'A' BORROW WITHOUT SWAPPING. app_box_swap_acquire() IS called, and
 *       finish_slot_pose() does its real fill (same SD/cache reads, same memcpy into
 *       the borrowed g_entries, same load-time timing the shipped feature would have
 *       spent) -- but s_pose_ok[] is forced back off for every slot that fill would
 *       have enabled, so pose_swap_rom_slot() never runs its swap_cache_slot() branch
 *       and the grid keeps showing the ordinary 1 px bob throughout. If a hardware run
 *       with THIS variant crashes, the borrow itself is at fault (g_entries is not as
 *       idle as the acquire/release contract claims, or the fill sequence is the
 *       problem) -- the exchange logic never ran and is innocent.
 *   2 = 'B' SWAP WITHOUT BORROWING. app_box_swap_acquire() is NEVER called -- g_entries
 *       is not touched at all. Instead grid slot 0 ALONE gets a real per-tick VRAM<->
 *       cache DMA exchange (bit-for-bit swap_cache_slot's own three dma3_cpy calls),
 *       sourced from a small dedicated static buffer this file owns outright
 *       (s_expb_cache, 512 B) instead of the borrowed g_entries -- see finish_slot_pose
 *       and pose_swap_rom_slot below for the one extra branch each needs. (This is
 *       "s_stage can serve one slot" from the brief, done as a SEPARATE 512 B buffer
 *       rather than literal reuse of s_stage itself: s_stage is this file's shared
 *       load-time scratch -- rom_icon_read_verified, icon_tiles, the palette staging
 *       in boxoam_enter, restore_slot's re-fetch on a chunk-carry uncover -- and several
 *       of those CAN run between two pose-swap ticks, so parking slot 0's frame-1
 *       persistently in s_stage across ticks would not have the same "touched by
 *       nothing else" guarantee the real feature's single-synchronous-call use of
 *       s_stage has. A dedicated buffer removes that ambiguity for the price of the
 *       same 512 B, compiled in only for this variant.) Every other slot stays on the
 *       ordinary bob. If THIS variant crashes, the exchange mechanism itself is at
 *       fault -- g_entries was never touched and is innocent.
 *   3 = 'C' FULL VOLUME, NOT THE BORROW. Neither app_box_swap_acquire() nor the
 *       displayed content is touched -- swap_volume_slot() below runs the SAME
 *       three-dma3_cpy, 512 B shape as swap_cache_slot for EVERY one of the 30 grid
 *       slots (occupied or not: worst-case volume needs all 30 hit, not however many
 *       the loaded save happens to fill), split 15-now/15-next-tick exactly like the
 *       real feature, but content-invariant by construction (each slot's VRAM tiles
 *       are DMA'd out and straight back in; the third DMA is a pure scratch write for
 *       byte-volume parity, never read back) -- so what's on screen never changes and
 *       nothing 15 KiB is needed. If THIS variant crashes, the exchange VOLUME is
 *       implicated, independent of both the borrow (never touched) and the single-
 *       slot exchange mechanism (already covered by B).
 *
 * READING THE RESULTS -- what each outcome does and does NOT prove:
 *   - A crashes, B and C don't -> the borrow (g_entries donation) is the runaway.
 *   - B crashes, A and C don't -> the exchange MECHANISM itself is broken even at
 *     one slot -- content garbling, a bad DMA arg, an address miscalculation.
 *   - C crashes, A and B don't -> the exchange VOLUME/timing is the runaway -- 30
 *     slots' worth of DMA (see below) does something 1 slot does not, independent of
 *     the borrow or of any per-slot correctness bug.
 *   - A and B BOTH come back clean -> this does NOT clear the feature. It clears the
 *     borrow (A) and the single-slot mechanism (B) -- two of three variables. B moves
 *     three dma3_cpy calls of 512 B = 1,536 B in ONE tick; the real feature moves that
 *     many DMAs for up to 15 slots THIS tick and 15 more the NEXT (pose_swap_rom_slot's
 *     even half inline in boxoam_set_frame, the odd half in boxoam_pose_pump's
 *     `for (int s = 1; s < 30; s += 2)` loop) -- up to 23,040 B/tick, ~46,080 B across
 *     the two-tick pair a full toggle spans. That is a ~15-30x difference in exactly
 *     the dimension a vblank-overrun or bus-contention failure lives in, and this same
 *     code path already measured a real overrun once (2026-08-22 review: REG_VCOUNT
 *     probes showed 11/11 ticks running ~156% over a 68-scanline vblank window before
 *     the call-order fix). A clean A+B means the cause is LIKELY THE VOLUME -- run C.
 *   - A, B, and C all come back clean -> now the borrow, the mechanism, AND the volume
 *     are all cleared; the remaining suspects are outside this switch entirely (OAM/
 *     DISPCNT state, a timing interaction with rumble/RTC, or something the 2026-08-23
 *     OS-mode audit's checklist did not cover).
 *   - The canary (app_box_swap_canary_ok, checked every tick from boxoam_pose_pump,
 *     only actually held under 'A'): a trip means some write missed g_entries's
 *     intended 0..15,359 B span. `nm -S` on the ARTLESS elf confirms g_entries's
 *     immediate, ZERO-padding successor is g_sb1 -- the reassembled SaveBlock1, i.e.
 *     LIVE SAVE DATA, not idle scratch -- so a canary trip is not merely "a bug": it
 *     means the overrun that just happened was writing into the user's save, in RAM,
 *     the whole time. logs/log.txt PRESENT after a crash is definitive; ABSENT is
 *     inconclusive on the flush timing, never a clean bill of health by itself (see
 *     boxoam_pose_pump's own comment for the one-shot flush + on-screen/audible signal
 *     this now triggers, so the evidence and the tell-tale survive even without SD).
 *
 * Change the number below, `make artless`, flash. Run in this order: A, then B, then
 * (if A+B are both clean) C. */
#ifndef PDNA_POSE_EXPERIMENT
#define PDNA_POSE_EXPERIMENT 0   /* 0=off (ships)  1='A' borrow-only  2='B' swap-only  3='C' volume-only */
#endif

/* grid geometry — MUST match pdna_box.c */
#define COLS    6
#define ROWS    5
#define CELL_W  24
#define CELL_H  22
#define GRID_X  82
#define GRID_Y  30
#define WP_X    78
#define WP_W    162
#define WP_Y    12

/* top-tab row geometry — MUST match pdna_box.c's PANEL_W(76)/UI_SCR_W(240) and its
 * render_full()'s 3 draw_tab() calls: draw_tab(0,PANEL_W+1,"PKMN DATA",...),
 * draw_tab(PANEL_W+1,92,"PARTY",...), draw_tab(PANEL_W+93,UI_SCR_W-(PANEL_W+93),"SAVE",...)
 * -> left edges 0 / 77 / 169, each a 12px-tall bar (y 0..11). Added 2026-08-23 (glove-vs-
 * tab-row fix) — this row had no geometry of its own here before; boxoam_cursor's
 * title_row==1 (banner) branch was the only "off the grid" case and every tab-row caller
 * fell into it too, at the banner's own fixed (WP_X+1,13) regardless of which tab. */
#define TAB_X0_PKMN  0
#define TAB_X0_PARTY 77
#define TAB_X0_SAVE  169

/* OBJ tile ids (charblock-4-relative 4bpp indices; only 512..1023 valid in bitmap modes) */
#define TID_ICON0   512                 /* slot s -> TID_ICON0 + s*16            */
#define TID_HAND    992                 /* region A: cursor hand (16 tiles)      */
#define TID_REGB    1008                /* region B: grab fist OR full-size item  */
#define TID_GRAB    TID_REGB            /* 32x32 grab fist (16 tiles)            */
#define TID_CITEM   TID_REGB            /* 32x32 full-size item, real icon (16 tiles) */

/* OBJ palette banks */
#define PB_HAND     13                  /* hand(normal)+grab+item glyph           */
#define PB_HANDORG  14                  /* hand orange (MOVE)                     */
#define PB_CITEM    15                  /* carried item icon                      */

/* Retail Emerald's PC transparency, taken from the decomp rather than eyeballed:
 * SetMonIconTransparency (pokemon_storage_system.c) writes BLDCNT = BLDCNT_TGT2_ALL
 * (0x3F00) and BLDALPHA = BLDALPHA_BLEND(7, 11) (0x0B07), and it fires ONLY in the
 * MOVE-ITEMS mode, where item-less icons get objMode = ST_OAM_OBJ_BLEND and the mons
 * that DO hold an item stay opaque.
 *
 * Two things this gets right that the old (10, 8) constants did not: the second-target
 * mask is ALL layers, not just BG2+OBJ, so a faded icon blends the same over the
 * wallpaper, the banner and another icon; and the effect bits are deliberately NONE,
 * because a semi-transparent OBJ blends against the 2nd-target mask regardless of
 * BLDCNT's effect field — retail never switches on a global effect, which is what made
 * ours look like a screen-wide dimmer instead of per-icon transparency. */
#define PSS_BLDCNT   ((u16)((BLD_BG0 | BLD_BG1 | BLD_BG2 | BLD_BG3 | BLD_OBJ | BLD_BACKDROP) << 8))
#define PSS_BLDALPHA ((u16)(7 | (11 << 8)))

/* OAM entry assignment */
#define OE_ICON0    0                   /* 0..29 grid icons                       */
#define OE_HAND     30                  /* cursor hand                            */
#define OE_GRAB     31                  /* grab fist (move carry)                 */
#define OE_CARRY    32                  /* carried icon (move carry)              */
#define OE_CITEM    33                  /* carried item (ITEM carry)              */
#define OE_MARK0    34                  /* 34..63 ITEM-mode held-item markers     */
#define OE_COUNT    64

static OBJ_ATTR s_shadow[128];          /* OAM shadow; flushed in vblank          */
static uint8_t  s_iconbank[30];         /* palette bank per grid slot (0=empty)   */
static uint8_t  s_occupied[30];         /* 1 if slot has an icon                  */
static uint16_t s_species[30];          /* species per slot (for the frame swap)  */
static uint8_t  s_form[30];             /* form per slot                          */
static uint8_t  s_isegg[30];            /* 1 = this slot shows the Egg icon (no bob)  */
static uint8_t  s_icon_blend[30];       /* 1 = draw this icon semi-transparent (ITEM mode, non-holders) */
static int      s_frame = 0;            /* current bob frame (0/1) in OBJ VRAM     */
static int      s_bob = 0;              /* current unison Y-bob offset (0/1)      */
/* Real Gen-3 2-frame pose swap, ROM-streamed slots included when the source is
 * cheap enough (see boxoam_rom_icons below and boxoam_set_frame's header comment). */
static uint8_t  s_pose_ok[30];          /* per slot: frame 1 reachable without SD I/O in a tick */
static int      s_any_pose = 0;         /* any occupied slot can pose-swap                */
/* SD/cache-sourced pose swap (2026-08-22 review, MUST-FIX 2 follow-up): a borrowed
 * 15,360 B EWRAM cache (pdna_app.h app_box_swap_acquire) holding slot s's OTHER pose
 * frame, fetched ONCE per slot at load/restore time -- see finish_slot_pose(). A tick-
 * time swap is then a pure VRAM<->cache DMA exchange (swap_cache_slot, below), zero SD
 * I/O, for exactly the sources that used to be stuck on the 1 px bob. s_swapcache is
 * NULL whenever the borrow isn't held (box screen closed, or the acquire failed) --
 * every use of it is guarded, so a failed acquire degrades to the pre-existing bob. */
static uint8_t* s_swapcache = 0;
static uint8_t  s_cache_ok[30];         /* 1 = s_swapcache's slot s holds a verified frame */
#if PDNA_POSE_EXPERIMENT == 2 || PDNA_POSE_EXPERIMENT == 3
/* Dedicated buffer -- see the PDNA_POSE_EXPERIMENT block above for why this is
 * SEPARATE from s_stage. Under 'B' it holds slot 0's real other-pose frame (content
 * matters, s_expb_ok below tracks whether it's valid). Under 'C' it is pure DMA
 * scratch for every slot's third, volume-only copy -- its bytes are written and
 * never read back, so no validity flag is needed there. */
static uint16_t s_expb_cache[MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES / 2];  /* 512 B */
#endif
#if PDNA_POSE_EXPERIMENT == 2
static uint8_t  s_expb_ok = 0;          /* 1 = s_expb_cache holds slot 0's verified other frame */
#endif
#if PDNA_POSE_EXPERIMENT == 3
static int      s_vol_pend = 0;         /* 'C': odd-half volume exchange is due next tick   */
#endif
static int      s_pend = 0;             /* second half of a split (fused-ROM) swap is due  */
static int      s_pend_frame = 0;       /* which frame that pending half is swapping to    */
static int      s_regb = -1;            /* what region B holds: 0=grab fist 1=item -1=none */
static int      s_rega = -1;            /* what region A holds: 0=hand 1=full item 2=held mon -1 */
static uint8_t  s_selmark[30];          /* 1 = whiten this slot (rubber-band selection)   */
static uint8_t  s_select_on = 0;        /* selection brightness-blend registers active    */
static uint8_t  s_covered[30];          /* 1 = slot's tile region borrowed by the chunk   */
static uint8_t  s_chunk_on = 0;         /* chunk-carry visuals active                     */
static uint8_t  s_chunk_valid = 0;      /* borrowed uploads valid (load_box invalidates)  */
static int      s_chunk_tr = 0, s_chunk_tc = 0;  /* last-applied anchor (skip re-DMA when unmoved) */

static inline OBJ_ATTR* oe(int i) { return &s_shadow[i]; }

/* hide one shadow entry */
static void hide(int i) { obj_hide(oe(i)); }

/* DMA `bytes` from src into OBJ tile id `tid` (charblock-4-relative). */
static void upload_tiles(int tid, const void* src, int bytes) {
  /* OBJ tile memory base = tile_mem_obj[0] (0x06010000); each 4bpp tile = 32 B. */
  uint16_t* dst = (uint16_t*)((uint8_t*)tile_mem_obj[0] + (uint32_t)tid * 32);
  dma3_cpy(dst, src, bytes);
}

/* Verified copy for the ONE-SHOT icon uploads. The 30 grid icons composite OVER the
 * wallpaper region every frame, and their ROM reads were the last unverified path that
 * could paint a "jumble" there — garbled icons are visually indistinguishable from a
 * garbled wallpaper. Same contract as pdna_box's wp_copy_verified: IWRAM_CODE so the
 * cart bus carries pure data reads, volatile source so the compare is a real 2nd read. */
IWRAM_CODE __attribute__((noinline))
static int icopy_verified(uint16_t* dst, const uint16_t* src, int n) {
  volatile const uint16_t* vsrc = src;
  for (int a = 0; a < 4; a++) {
    for (int i = 0; i < n; i++) dst[i] = vsrc[i];
    int ok = 1;
    for (int i = 0; i < n; i++) if (dst[i] != vsrc[i]) { ok = 0; break; }
    if (ok) return a;
  }
  return -1;
}

static uint16_t s_stage[256];        /* one 512 B verify chunk (IWRAM .bss — EWRAM is full) */

/* ---- ROM-streamed icons (phase 1 of the ROM-gated build) --------------------
 * When the compiled icon art is absent (the artless build) and the app has an open
 * RomMon on the user's own ROM, the box streams the real icons from it: 512 B raw
 * 4bpp per slot, staged into s_stage and read TWICE AT LOAD TIME (the EZ-Flash read
 * path can return success holding garbage, and a fused read shares the compiled-art
 * risk class the verified uploads already treat).
 *
 * Whether the 2-frame pose swap can ALSO run off that ROM depends on WHICH ROM:
 *   fused into this image -> fused_rom_read is a memcpy from cart address space. No
 *                            SD, no OS-mode window. One unverified 512 B read + DMA
 *                            per slot, split over two vblanks (boxoam_pose_pump). The
 *                            swap runs.
 *   registered SD .gba    -> every read is f_lseek + f_read. A swap would need up to
 *                            30 SD reads inside the vblank tick (forbidden) or a
 *                            15,360 B frame-1 cache that does not exist. That source
 *                            keeps the 1 px OAM bob.
 * MUST-FIX 2 (2026-08-22 review, MEASURED not assumed): that 15,360 B cache was
 * checked against the ARTLESS build's ACTUAL EWRAM headroom, not the full-art number —
 * a genuine artless rebuild (art sources absent, confirmed via `nm`: no
 * mon_icons_oam.o/hand_oam.o/etc. linked) prints the SAME "EWRAM ok: 748 bytes free
 * below 0x02040000" as the full-art build, because none of the artless-only modules
 * own any EWRAM .bss/.data — they're ROM-resident .rodata pulled in by .incbin, so
 * removing them frees ROM, not RAM. 748 B does not hold 15,360 B, and it does not hold
 * a smaller per-slot unit either: even ONE slot (512 B, no LRU/recency metadata,
 * since the box renders all 30 slots every tick with no "currently selected" concept
 * an MRU cache could key off) would spend 68% of the remaining headroom on a single
 * icon animating for real while the other 29 keep bobbing — a worse, more confusing
 * result than a uniform bob, for a cost this EWRAM-overflow-is-FATAL build can't
 * really spare either. Keeping the uniform 1 px bob here is the correct, deliberate
 * choice for the SD-registered path, not a TODO — re-check the actual free-byte count
 * (Makefile's own EWRAM guard prints it) before re-attempting this, on THIS build,
 * not the full-art one; they are not always equal, but they were both 748 here.
 * `cheap_reads` is how the app tells us which one is currently open; s_rom_cheap
 * records it. It says nothing about the icons.bin CACHE rung below, which is real SD
 * I/O (f_open+f_lseek+f_read) every time regardless of what ROM is registered — see
 * icon_tiles()'s own `*cheap` output, which is the thing s_pose_ok[] is actually
 * built from. */
static const RomMon* s_rommon = 0;
static int           s_rom_cheap = 0;             /* s_rommon's reads are a cart-space memcpy */
/* Memoised icon LOCATION (see rom_mon.h RomMonLoc): the icon-table pointer and the
 * palette index for one species. Invalidated whenever the ROM source changes — the
 * RomMon POINTER can stay the same across a re-registration of a different file, so
 * the pointer is not a safe cache key. */
static RomMonLoc s_iconloc;
static uint16_t  s_iconloc_sp = 0xFFFF;          /* 0xFFFF = nothing memoised */
static uint8_t   s_iconloc_f;
void boxoam_rom_icons(const struct RomMon* rm, int cheap_reads) {
  s_rommon    = (rm && rm->ok) ? rm : 0;
  s_rom_cheap = s_rommon ? (cheap_reads != 0) : 0;
  s_iconloc.ok = 0; s_iconloc_sp = 0xFFFF;
}

/* ---- Phase 2: the icons.bin cache rung ------------------------------------------
 * Set by the app (pdna_main.c's art_session_icons_ready() gate) once per icon-source
 * resolution. This is the rung DESIGN.md Sec 4.1 puts FIRST: the only source that can
 * light the box up in an artless build with no ROM registered THIS session (the card
 * outlives the registration). A cache read is a single seek+read (see
 * art_icons_cache.c), unlike the ROM rung's separate locate + per-frame reads, and it
 * needs no per-icon double-read verify: the whole file's FNV was already checked once
 * this session (art_session_icons_ready), so a single read is trusted the same way a
 * compiled .rodata array is.
 *
 * Just a POINTER, not a held-open file: art_icons_cache.c opens/reads/closes its own
 * FIL per call (see that file's header comment for why -- a held-open FIL here plus
 * one in art_fallbacks.c measured at 2,272 B of new IWRAM .bss and crashed the
 * budget). The string itself is owned by art_cache.c's static filename table
 * (art_kind_filename), so this is 4 bytes, not a new buffer. */
static const char* s_iconcache_path = 0;

void boxoam_set_icon_cache(const char* path) {
  s_iconcache_path = path;
  if (!path) art_icons_meta_clear();
}

int  boxoam_icons_available(void) {
  const uint8_t* t; int b;
  return mon_icon_oam_for(1, &t, &b) || s_iconcache_path != 0 || s_rommon != 0;
}

static uint32_t stage_sum(void) {
  uint32_t v = 0;
  for (int i = 0; i < 256; i++) v += s_stage[i];
  return v;
}

/* Read one icon frame into s_stage, TWICE, and accept only when both passes sum the
 * same (with one buffer a full byte-compare needs a second read anyway; two whole
 * reads agreeing catches the transient-garbage failure the EZ driver can produce).
 * Retries like icopy_verified; on give-up the last read still stands (a maybe-garbled
 * icon beats a hole) and the anomaly is logged.
 *
 * `frame` used to be hardcoded to 0 (only the box's LOAD ever called this). Now the
 * pose swap can also serve frame 1 through this same ladder for a slot restored
 * mid-swap (chunk-carry uncover, party-panel close) — see icon_tiles()'s `frame < 2`
 * gate below. The live per-TICK swap itself does NOT come through here (see
 * rom_icon_pose_frame): a double-verified read is load-time-only spend, and reusing
 * THIS function's shared s_iconloc memo from a tick would let an unverified pose-swap
 * locate get silently trusted by a later verified caller — see rom_mon_locate_verified's
 * own header comment on exactly that hazard. */
static int rom_icon_read_verified(uint16_t species, uint8_t form, int egg, uint8_t frame,
                                  int* bank) {
  if (!s_rommon) return 0;
  uint16_t sp = egg ? 412 : species;
  uint8_t f  = egg ? 0 : form;
  rumble_io_suspend();
  /* LOCATE ONCE — VERIFIED — then read the same frame twice.
   *
   * Locating once per icon instead of once per verify pass is what removed four
   * RomCtx reads per icon (and all of them on a memo hit); what it must NOT remove is
   * the verify's cover over the lookup itself. The pointer and the palette id ARE the
   * answer to "which species is this": a garbled pointer that still lands inside the
   * image passes ptr_ok, and with the location cached BOTH frame passes then read that
   * same wrong offset, agree, and paint another species' icon with nothing logged.
   * rom_mon_locate_verified reads each field twice back to back and requires
   * agreement, which is exactly the coverage the pre-memo code got from re-locating
   * per pass — and it adds no FAR seek, because a same-offset re-read of 4 bytes stays
   * inside the current FatFs cluster (rom_mon.c read_small). The FRAME verify below is
   * untouched: still two whole reads that must agree, four attempts, same give-up log. */
  if (!(s_iconloc.ok && s_iconloc_sp == sp && s_iconloc_f == f)) {
    int unstable = 0;
    s_iconloc_sp = sp; s_iconloc_f = f;
    PERF_ICON(rom_loc);                            /* a real locate, not a memo hit */
    if (!rom_mon_locate_verified(s_rommon, sp, f, &s_iconloc, 4, &unstable)) {
      s_iconloc_sp = 0xFFFF;                     /* leaves ok = 0: the memo self-voids */
      rumble_io_resume();
      /* Fail CLOSED on an unstable lookup — the slot draws empty. Everywhere else here
       * "a maybe-garbled icon beats a hole", but that trade is about PIXELS; an address
       * we cannot agree on would confidently draw the wrong Pokemon, which reads as
       * the save being wrong. A bounds failure (a corrupt species id in the save) is
       * not logged, so it cannot spam the log on every box load. */
      if (unstable) { log_line("icons: rom locate unstable sp=%u", sp); app_log_flush(); }
      return 0;
    }
  }
  int ok = 0;
  for (int a = 0; a < 4 && !ok; a++) {
    /* Every ATTEMPT is counted, including the retries: an unstable cart that needs
     * four passes costs eight 512 B reads, and averaging that away would hide the one
     * thing this counter exists to expose. */
    PERF_ICON(rom_frm);
    if (!rom_mon_icon_at(s_rommon, &s_iconloc, frame, (uint8_t*)s_stage)) { rumble_io_resume(); return 0; }
    uint32_t s1 = stage_sum();
    PERF_ICON(rom_frm);
    if (!rom_mon_icon_at(s_rommon, &s_iconloc, frame, (uint8_t*)s_stage)) { rumble_io_resume(); return 0; }
    ok = (stage_sum() == s1);
  }
  rumble_io_resume();
  if (!ok) { log_line("icons: rom read unstable sp=%u", sp); app_log_flush(); }
  *bank = s_iconloc.pal;                          /* ROM pals live in OBJ banks 0..2 */
  return 1;
}

/* The cache rung: one seek+read (both frames at once, since extraction laid them out
 * contiguously per row) into s_stage, no per-read doubling -- the file's FNV was
 * already checked once this session (art_session_icons_ready), so this trusts a
 * single read the way it already trusts a compiled .rodata array. species/form map
 * to the icon table's row via art_icons_row_for, the SAME mapping rom_mon.c uses
 * internally. *bank is the row's palette id (0..2) straight out of the preloaded
 * metadata -- it addresses OBJ banks 0..2 directly, exactly like the ROM rung's
 * s_iconloc.pal, because boxoam_enter copies the SAME 3 palettes there (see below;
 * the cache's palette bytes are a verbatim copy of the ROM's, made at extraction
 * time), whichever rung actually served the tiles. */
static int cache_icon_read(uint16_t species, uint8_t form, int egg, uint8_t frame,
                           int* bank) {
  if (!s_iconcache_path) return 0;
  uint16_t sp = egg ? 412 : species;
  uint8_t f = egg ? 0 : form;
  uint16_t row = art_icons_row_for(sp, f);
  if (row >= ART_ICONS_ROWS) return 0;
  /* Same GPIO freeze the ROM rung below (rom_icon_read_verified) already brackets
   * its own SD/ROM reads with -- art_icons_read_frame is f_open+f_lseek+f_read+
   * f_close, real SD I/O, and had NO motor freeze at all until this fix. */
  rumble_io_suspend();
  bool ok = art_icons_read_frame(s_iconcache_path, row, frame, (uint8_t*)s_stage);
  rumble_io_resume();
  if (ok) PERF_ICON(bin);
  if (!ok) return 0;
  uint8_t id = art_icons_meta_pal_id(s_iconcache_path, row);
  if (id >= ART_ICONS_PALS) return 0;
  *bank = id;
  return 1;
}

/* compiled art first, then the cache, then the user's ROM (all staged into s_stage).
 * Returns the tile source or NULL; *from_rom tells the uploader the bytes are already
 * verified RAM (true for BOTH the cache and the ROM rung -- neither needs the staged-
 * verify DMA path a compiled .rodata array would).
 *
 * *cheap tells the caller whether THIS fetch could be repeated on an animation tick
 * with no SD I/O -- it is what s_pose_ok[] (the per-slot pose-swap capability array)
 * is built from. Compiled art is always cheap (a .rodata read). The icons.bin CACHE
 * rung is NEVER cheap: art_icons_read_frame is f_open+f_lseek+f_read+f_close, real SD
 * I/O, every call, regardless of what ROM (if any) is registered. The ROM rung is
 * cheap iff s_rom_cheap -- the app told us (boxoam_rom_icons's cheap_reads) that
 * s_rommon's reads are a fused cart-space memcpy, not an f_lseek+f_read.
 *
 * frame is now `< 2` on BOTH the cache and ROM rungs (used to be `== 0` on the ROM
 * rung -- box_oam.c's own header used to say the pose swap "stays OFF on this path").
 * A ROM/cache-streamed slot restored mid-swap (chunk-carry uncover, party-panel
 * close, boxoam_slot_blit_bitmap) can now be asked for frame 1 and must be able to
 * serve it, or it would blink out for as long as the box sits on the odd frame. */
static const uint8_t* icon_tiles(uint16_t species, uint8_t form, uint8_t frame,
                                 int egg, int* bank, int* from_rom, int* cheap) {
  const uint8_t* t; int b;
  *from_rom = 0; *cheap = 0;
  /* EMPTY SLOT. The compiled-art branch below already short-circuits on species==0, but
   * the ROM-streamed branch did NOT: species 0 is a VALID row of the icon table (the
   * "??????" dummy), so rom_icon_read_verified happily performed two full verified
   * 512-byte reads off the microSD — ~22 sectors — for a result boxoam_load_box then
   * discards. MEASURED on the artless build: 60 rom_mon_icon calls / 180 ROM reads per
   * box load REGARDLESS of occupancy, so an empty box cost exactly as much to open as a
   * full one. All four icon_tiles call sites already NULL-guard. */
  if (!egg && !species) return 0;
  if (egg ? mon_icon_oam_egg(&t, &b)
          : (species && mon_icon_oam_for_form_frame(species, form, frame, &t, &b))) {
    *bank = b; *cheap = 1; return t;
  }
  if (frame < 2 && cache_icon_read(species, form, egg, frame, bank)) {
    *from_rom = 1;
    return (const uint8_t*)s_stage;
  }
  if (frame < 2 && rom_icon_read_verified(species, form, egg, frame, bank)) {
    *from_rom = 1; *cheap = s_rom_cheap; return (const uint8_t*)s_stage;
  }
  /* THE hole on screen. Counted here, at the ladder's single terminal failure, rather
   * than inside cache_icon_read/rom_icon_read_verified: a cache miss that the ROM rung
   * then serves is not a failure, and counting it in the rung would make `null` read as
   * "12 icons failed" on a box where all 12 came through one rung down. The empty-slot
   * early return above deliberately does NOT count -- an empty PC cell was never asked
   * for an icon, and conflating the two is precisely what made a box holding 18 mons in
   * 30 slots indistinguishable from a box of 30 where 12 icons failed to read. */
  PERF_ICON(null_ans);
  return 0;
}

/* forward: upload_tiles_verified is defined below */
static void upload_tiles_verified(int tid, const void* src, int bytes);

/* Upload one icon: staged ROM bytes are already verified RAM (plain DMA); compiled
 * art goes through the staged-verify path as always. */
static void upload_icon(int tid, const uint8_t* tiles, int from_rom) {
  if (from_rom) upload_tiles(tid, tiles, MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES);
  else upload_tiles_verified(tid, tiles, MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES);
}

/* upload_tiles with staged verify: ROM -> RAM (verified, retried) -> DMA to VRAM
 * (RAM->VRAM cannot glitch). On give-up, the raw upload still runs (a maybe-garbled
 * icon beats a hole) and the anomaly is logged + flushed so it survives a power-off.
 * NOT used by boxoam_set_frame: a verified 15 KiB re-stage cannot fit the vblank
 * window, and any set_frame glitch self-heals at the next swap (<= 0.5 s). */
static void upload_tiles_verified(int tid, const void* src, int bytes) {
  const uint16_t* s = (const uint16_t*)src;
  uint16_t* dst = (uint16_t*)((uint8_t*)tile_mem_obj[0] + (uint32_t)tid * 32);
  int total = bytes / 2, done = 0, rr = 0, bad = 0;
  rumble_io_suspend();               /* same GPIO freeze the wallpaper's CPU reads get */
  while (done < total) {
    int n = total - done; if (n > 256) n = 256;
    int r = icopy_verified(s_stage, s + done, n);
    if (r < 0) { bad = 1; break; }
    rr += r;
    dma3_cpy(dst + done, s_stage, (uint32_t)n * 2);
    done += n;
  }
  rumble_io_resume();
  if (bad) { dma3_cpy(dst, src, bytes); log_line("icons: tid %d unstable rom reads", tid); }
  else if (rr) log_line("icons: tid %d %d re-reads", tid, rr);
  static int s_icon_flushes = 0;
  if ((bad || rr) && s_icon_flushes < 2) { s_icon_flushes++; app_log_flush(); }
}

#if !PDNA_HAND_ART_COMPILED
/* ------- Phase 3 (ROM-art, DESIGN.md Sec 1.3/4.5): the glove, streamed live -------
 *
 * All 16 of the hand's OAM tiles are ALREADY allocated (TID_HAND 992..1007) and the
 * grab fist already shares TID_GRAB (== TID_REGB) with the item glyphs -- this rung
 * changes only the DMA SOURCE those two uploads read from, exactly DESIGN.md Sec
 * 4.5's "zero new OBJ tiles" plan. Compiled out entirely when the real poses are
 * linked in (hand_gate.h), so this whole block is absent from a full-art build.
 *
 * s_romhand is the app's registered ROM (or NULL); s_romhand_pal_ready records
 * whether boxoam_enter() could ALSO read the ROM's own palette this session -- tiles
 * and palette are used as a PAIR, never mixed with the compiled/fallback array, so a
 * palette-read failure cannot pair ROM tiles with the wrong (fallback) colours. */
static const RomHand* s_romhand = 0;
static int s_romhand_pal_ready = 0;

void boxoam_rom_hand(const RomHand* rh) {
  s_romhand = rom_hand_have(rh) ? rh : 0;
  s_romhand_pal_ready = 0;              /* re-validated fresh on the next boxoam_enter() */
}

/* MUST-FIX 1 (2026-08-20 review): the retail ROM sheet is UNCROPPED -- each pose's
 * glove sits somewhere inside its 32x32 cell, not top-left like the compiled art
 * (tools/gen_hand_oam.py packs every crop at (0,0), so hand_xy()'s single anchor --
 * "hx+13 = cell_x+16", box_oam.c below -- is calibrated to that top-left crop only).
 * Measured against hand_oam.c (independently re-verified here, brute-force search
 * over dx,dy in Emerald.gba, perfect 1024/1024 opaque/transparent mask match at
 * each): the ROM sheet's glove sits (+7,+4) right/down of the compiled crop for the
 * CURSOR pose, and by a DIFFERENT amount for each other pose --
 * (+8,+5) bounce, (+4,+6) reach, (+7,+7) grab -- because retail drew each pose's art
 * at a different position within its own cell, not on a shared grid.
 *
 * THIS is the one place that knows those four numbers: every streamed-hand upload
 * (cursor/bounce/reach into TID_HAND, grab into TID_GRAB) funnels through
 * load_rom_hand_frame() below, so re-anchoring the pixels HERE, once, before the
 * DMA to VRAM, makes every consumer of the sprite -- hand_xy(), the carry offsets in
 * boxoam_carry_held(), the pop-free fist swap boxoam_cursor() relies on -- see
 * compiled-art coordinates and nothing else. hand_xy() stays the single source of
 * truth for WHERE the hand goes; this table is the only place that knows the sheet
 * itself needs correcting before it gets there. */
static const struct { uint8_t dx, dy; } k_hand_anchor[ROM_HAND_FRAMES] = {
  [ROM_HAND_FRAME_CURSOR] = { 7, 4 },
  [ROM_HAND_FRAME_BOUNCE] = { 8, 5 },
  [ROM_HAND_FRAME_REACH]  = { 4, 6 },
  [ROM_HAND_FRAME_GRAB]   = { 7, 7 },
};

/* One raw 4bpp pixel out of a 512 B 32x32 1D-tile-order frame (4x4 tiles, row-major)
 * -- same layout rom_hand.c's internal frame_px() decodes, duplicated here (that
 * helper is private to rom_hand.c, and this file already owns its own OAM-format
 * pixel access for the icon/wallpaper rungs). */
static uint8_t hand_px(const uint8_t* buf, int x, int y) {
  int tx = x >> 3, ty = y >> 3;
  const uint8_t* t = buf + (ty * 4 + tx) * 32 + (y & 7) * 4;
  uint8_t b = t[(x & 7) >> 1];
  return (uint8_t)((x & 1) ? (b >> 4) : (b & 0x0F));
}
static void hand_setpx(uint8_t* buf, int x, int y, uint8_t v) {
  int tx = x >> 3, ty = y >> 3;
  uint8_t* t = buf + (ty * 4 + tx) * 32 + (y & 7) * 4;
  uint8_t* b = &t[(x & 7) >> 1];
  *b = (x & 1) ? (uint8_t)((*b & 0x0F) | (v << 4))
               : (uint8_t)((*b & 0xF0) | (v & 0x0F));
}

/* Re-anchor a 512 B frame in place: out(x,y) = in(x+dx, y+dy), 0 (transparent) past
 * the sheet edge -- crops the retail cell down to the same top-left-aligned glove
 * tools/gen_hand_oam.py bakes into the compiled art. Safe fully in place with NO
 * scratch buffer (would otherwise be a 512 B addition -- this file's stack-buffer
 * budget is tight, see s_stage's own "IWRAM is full" note two screens up): every
 * source pixel (x+dx, y+dy) lies strictly past the destination (x,y) in row-major
 * order since dx,dy > 0, and this loop visits destinations in that same increasing
 * row-major order, so no source is ever read after it has already been overwritten. */
static void hand_anchor_shift(uint8_t* buf, int dx, int dy) {
  for (int y = 0; y < 32; y++) {
    int sy = y + dy;
    for (int x = 0; x < 32; x++) {
      int sx = x + dx;
      uint8_t v = (sx < 32 && sy < 32) ? hand_px(buf, sx, sy) : 0;
      hand_setpx(buf, x, y, v);
    }
  }
}

/* Read one verified 512 B pose off the ROM into s_stage, re-anchor it to the
 * compiled-art crop (see k_hand_anchor above -- MUST-FIX 1), and DMA it straight to
 * `tid` (no extra verify at the upload step -- rom_hand_frame already fetched-twice-
 * and-compared, same "already verified RAM" contract upload_icon()'s from_rom=1 path
 * relies on for the icon-streaming rung). Returns 1 on success, 0 (nothing uploaded,
 * caller falls back to the compiled/fallback array) otherwise. */
static int load_rom_hand_frame(int tid, uint8_t frame) {
  if (!s_romhand || !s_romhand_pal_ready) return 0;
  rumble_io_suspend();
  int ok = rom_hand_frame(s_romhand, frame, (uint8_t*)s_stage);
  rumble_io_resume();
  if (!ok) { log_line("hand: rom frame %d unstable/unavailable", frame); return 0; }
  hand_anchor_shift((uint8_t*)s_stage, k_hand_anchor[frame].dx, k_hand_anchor[frame].dy);
  upload_tiles(tid, s_stage, ROM_HAND_FRAME_BYTES);
  return 1;
}
#endif /* !PDNA_HAND_ART_COMPILED */

/* ------- region B (time-shared between move/item grab-fist and the small item) ------- */
static void load_regb_grab(void) {
  if (s_regb == 0) return;
#if !PDNA_HAND_ART_COMPILED
  if (load_rom_hand_frame(TID_GRAB, ROM_HAND_FRAME_GRAB)) { s_regb = 0; return; }
#endif
  upload_tiles_verified(TID_GRAB, hand_oam_grab_tiles, HAND_OAM_TILES * HAND_OAM_TILE_BYTES);
  s_regb = 0;
}

/* ------- region A (time-shared: cursor hand / held-mon icon / full grab item) -------
 * the cursor hand is hidden whenever we carry a mon or grab an item, so its 16 tiles
 * are free for the held mon's icon or the full-size item; restore the hand on the way back.
 * The hand has POSES now (normal glove / wide-open reach, for the retail grab dip), so
 * region A's state ids are 10+pose — distinct from 1 (item) and 2 (held mon), and a pose
 * change simply invalidates the region so the next load uploads the right tiles. */
static int s_hand_pose = BOXOAM_POSE_NORMAL;
static int s_cur_dy = 0;                /* grab/place dip offset (0 = rest) */
static int s_cur_dx = 0;                /* cursor-slide X offset (retail fix #4) */
#if !PDNA_HAND_ART_COMPILED
/* MUST-FIX 2 (2026-08-20 review): BOUNCE's positional fallback for a ROM/SD-streamed
 * hand -- see the long comment in boxoam_hand_pose() below. Applied as a plain OAM-
 * position offset in boxoam_cursor(); zero new EWRAM, zero ROM/SD traffic. */
static int s_hand_bob = 0;
#endif
void boxoam_hand_pose(int pose) {
  pose = (pose >= 0 && pose <= 2) ? pose : BOXOAM_POSE_NORMAL;
#if !PDNA_HAND_ART_COMPILED
  /* BOUNCE is the ONLY automatic, idle-driven pose (pdna_box.c's ANIM_PERIOD loop
   * toggles it every 30 vblanks, forever, while the player is simply sitting in the
   * box) -- REACH/GRAB stay genuinely user-driven (rest -> reach -> grab on an actual
   * button press), at most a handful of reads per action, never a standing per-tick
   * cost. Letting BOUNCE re-source content from ROM/SD on every idle tick was exactly
   * box_oam.c's icon-rung shape (boxoam_set_frame, see its header comment: "no
   * frame-1 source in RAM ... keeps the positional bob" on streamed icons) -- same
   * bug, same fix: a hand that is actually being streamed (s_romhand set) never calls
   * rom_hand_frame for BOUNCE. It only nudges the sprite 1 px, applied as plain OAM-
   * position arithmetic in boxoam_cursor() below. A fused/compiled hand has every
   * pose's tiles in RAM already (hand_oam.c), so it keeps the real 2-frame swap --
   * this guard only fires when the alternative is a genuine SD read. */
  if (pose == BOXOAM_POSE_BOUNCE && s_romhand) { s_hand_bob = 1; return; }
  s_hand_bob = 0;
#endif
  s_hand_pose = pose;
}
void boxoam_cursor_dy(int dy)   { s_cur_dy = dy; }
void boxoam_cursor_dxy(int dx, int dy) { s_cur_dx = dx; s_cur_dy = dy; }
static void load_rega_hand(void) {
  int want = 10 + s_hand_pose;
  if (s_rega == want) return;
#if !PDNA_HAND_ART_COMPILED
  uint8_t romframe = s_hand_pose == BOXOAM_POSE_REACH  ? ROM_HAND_FRAME_REACH
                    : s_hand_pose == BOXOAM_POSE_BOUNCE ? ROM_HAND_FRAME_BOUNCE
                                                        : ROM_HAND_FRAME_CURSOR;
  if (load_rom_hand_frame(TID_HAND, romframe)) { s_rega = want; return; }
#endif
  const uint8_t* t = s_hand_pose == BOXOAM_POSE_REACH  ? hand_oam_reach_tiles
                   : s_hand_pose == BOXOAM_POSE_BOUNCE ? hand_oam_bounce_tiles
                                                       : hand_oam_cursor_tiles;
  upload_tiles_verified(TID_HAND, t, HAND_OAM_TILES * HAND_OAM_TILE_BYTES);
  s_rega = want;
}

/* Render the FULL 24x24 RGB15 icon for `item` at native size, centred in a 16-tile
 * (32x32) 4bpp sprite at TID_CITEM with a private 16-colour palette in bank 15.
 * Used both for the carried item (grab) and the hovered holder's item preview, so it
 * reads as the real, full-size sprite. Switches region B to ITEM use. item 0 / unknown
 * icon -> a simple filled box. The 24x24 art sits at offset (+4,+4) inside the 32x32. */
#define CITEM_OFF 4   /* centre the 24x24 art in the 32x32 sprite */

/* map a pixel of the sprite to a palette index (0 = transparent).
 * full -> 24x24 art centred (+4) in a 32x32 sprite; !full -> 24x24 down-scaled to 16x16. */
static uint8_t citem_index(const uint16_t* ic, const uint16_t* cpal, int ncol, int px, int py, bool full) {
  int sx, sy;
  if (full) { sx = px - CITEM_OFF; sy = py - CITEM_OFF;
              if (sx < 0 || sx >= ITEM_ICON_W || sy < 0 || sy >= ITEM_ICON_H) return 0; }
  else      { sx = px * ITEM_ICON_W / 16; sy = py * ITEM_ICON_H / 16; }   /* down-scale 24->16 */
  uint16_t p = ic[sy * ITEM_ICON_W + sx];
  if (!(p & 0x8000)) return 0;                       /* transparent pixel */
  uint16_t c = p & 0x7FFF;
  for (int k = 1; k < ncol; k++) if (cpal[k] == c) return (uint8_t)k;
  int best = 1, bd = 0x7fffffff, pr = c & 31, pg = (c >> 5) & 31, pb = (c >> 10) & 31;
  for (int k = 1; k < ncol; k++) {                   /* palette full: nearest match */
    int dr = pr - (cpal[k] & 31), dg = pg - ((cpal[k] >> 5) & 31), db = pb - ((cpal[k] >> 10) & 31);
    int dd = dr * dr + dg * dg + db * db;
    if (dd < bd) { bd = dd; best = k; }
  }
  return (uint8_t)best;
}

static void load_regb_item(uint16_t carried_item, bool full, int tid) {
  /* full = 32x32 (16 tiles) carried/grab item; !full = 16x16 (4 tiles) hover preview.
   * tid = where the tiles go (TID_CITEM in region B for hover, TID_HAND in region A for grab). */
  uint16_t cpal[16]; for (int i = 0; i < 16; i++) cpal[i] = 0;
  uint8_t ctiles[16 * 32];                           /* up to 16 tonc tiles, 4bpp */
  for (unsigned b = 0; b < sizeof ctiles; b++) ctiles[b] = 0;
  int across = full ? 4 : 2;                          /* tiles per row -> 32x32 or 16x16 */
  const uint16_t* ic = carried_item ? app_item_icon(carried_item) : 0;   /* + the ROM rung */
  if (ic) {
    int ncol = 1;                                    /* build the 16-colour palette */
    for (int y = 0; y < ITEM_ICON_H; y++)
      for (int x = 0; x < ITEM_ICON_W; x++) {
        uint16_t p = ic[y * ITEM_ICON_W + x];
        if (!(p & 0x8000)) continue;
        uint16_t c = p & 0x7FFF; int found = 0;
        for (int k = 1; k < ncol; k++) if (cpal[k] == c) { found = 1; break; }
        if (!found && ncol < 16) cpal[ncol++] = c;
      }
    int bi = 0;                                      /* pack across*across tonc tiles */
    for (int ty = 0; ty < across; ty++)
      for (int tx = 0; tx < across; tx++)
        for (int ry = 0; ry < 8; ry++)
          for (int rx = 0; rx < 8; rx += 2) {
            int px = tx * 8 + rx, py = ty * 8 + ry;
            uint8_t lo = citem_index(ic, cpal, ncol, px, py, full);
            uint8_t hi = citem_index(ic, cpal, ncol, px + 1, py, full);
            ctiles[bi++] = (uint8_t)((lo & 0xF) | ((hi & 0xF) << 4));
          }
  } else {
    /* unknown item: a simple filled box (index 2 fill, 1 border) over the whole sprite */
    cpal[1] = RGB15(8, 6, 1); cpal[2] = RGB15(28, 24, 6);
    int dim = across * 8, bi = 0;
    for (int ty = 0; ty < across; ty++)
      for (int tx = 0; tx < across; tx++)
        for (int ry = 0; ry < 8; ry++)
          for (int rx = 0; rx < 8; rx += 2) {
            int px = tx * 8 + rx, py = ty * 8 + ry;
            uint8_t v0 = (px == 0 || px == dim - 1 || py == 0 || py == dim - 1) ? 1 : 2;
            uint8_t v1 = (px + 1 == dim - 1 || py == 0 || py == dim - 1) ? 1 : 2;
            ctiles[bi++] = (uint8_t)((v0 & 0xF) | ((v1 & 0xF) << 4));
          }
  }
  for (int i = 0; i < 16; i++) pal_obj_mem[PB_CITEM * 16 + i] = cpal[i];
  upload_tiles(tid, ctiles, across * across * 32);
}

void boxoam_enter(void) {
  oam_init(s_shadow, 128);                         /* clears shadow to hidden     */
  for (int i = 0; i < 30; i++) { s_occupied[i] = 0; s_iconbank[i] = 0;
                                 s_selmark[i] = 0; s_covered[i] = 0; s_cache_ok[i] = 0; }
  /* Borrow g_entries for the SD/cache-sourced pose-swap cache -- see pdna_app.h's
   * app_box_swap_acquire comment. boxoam_exit() (every one of pdna_box()'s return
   * paths already calls it) always releases this, so the borrow cannot outlive a box
   * visit. NULL (acquire already held, or somehow too big) just means those slots
   * keep the pre-existing 1 px bob -- boxoam_load_box()'s finish_slot_pose() and every
   * swap_cache_slot() call already guard on s_swapcache != 0.
   *
   * DISABLED PENDING A HARDWARE A/B (2026-08-23). Guy filmed a repeatable crash on the
   * box screen -- wash to white, RGB banding, then full-screen noise that EVOLVES over
   * ~12 frames, i.e. code still running and scribbling. His build's timestamp is
   * 17:10:12, three and a half minutes after this feature landed at 17:06:40, so that
   * run was its FIRST hardware exposure, on the one configuration it could never be
   * tested in: the artless build with the ROM registered on SD (his logs/log.txt:
   * "icons: streaming from SD ... (Emerald)", "art cache: kind 0 not ready"). Every
   * emulator proof it has -- 70 swap cycles, byte-identical checksums, 0 reads while
   * idle, 0/150 vblank overruns -- was taken through the FUSED rung with cheap_reads
   * forced to 0, because the SD rung is unreachable in mGBA. A fused read is a cart
   * memcpy; an SD read is an f_lseek + f_read with the ROM unmapped underneath. Those
   * are not the same experiment.
   * No defect has been proven here, and a full audit of the OS-mode rule found none.
   * This is the cheapest decisive test available: turning the borrow off costs Guy the
   * two-frame pose animation on the SD path (it falls back to the 1 px bob, exactly as
   * it behaved before 9224283) and nothing else, and it tells us in ONE hardware run
   * whether this feature is the runaway. Flip it back on to re-arm the A/B.
   *
   * PDNA_POSE_EXPERIMENT (top of this file) is the follow-up: OFF (0, this branch)
   * ships; 'A' (1) re-enables exactly this acquire so the borrow gets exercised for
   * real while the swap itself stays disabled (finish_slot_pose forces s_pose_ok back
   * off after its fill -- see that function); 'B' (2) leaves this acquire off and uses
   * a dedicated private buffer instead (s_expb_cache above). */
#if PDNA_POSE_EXPERIMENT == 1
  s_swapcache = app_box_swap_acquire(APP_BOX_SWAP_BYTES);
#else
  s_swapcache = 0;  /* was: app_box_swap_acquire(APP_BOX_SWAP_BYTES); */
  (void)app_box_swap_acquire;
#endif
#if PDNA_POSE_EXPERIMENT == 2
  s_expb_ok = 0;
#endif
  s_bob = 0; s_regb = -1; s_rega = -1;
  s_hand_pose = BOXOAM_POSE_NORMAL; s_cur_dy = 0;   /* no mid-beat leakage across screens */
#if !PDNA_HAND_ART_COMPILED
  s_hand_bob = 0;
#endif
  s_select_on = 0; s_chunk_on = 0; s_chunk_valid = 0;

  /* shared icon palettes -> banks 0..12 (416 bytes), staged + verified like the tiles
   * (a corrupted palette garbles every icon at once — the full-region jumble look) */
  { int n = MON_ICON_OAM_BANKS * MON_ICON_OAM_PALLEN;
    if (icopy_verified(s_stage, mon_icon_oam_pal, n) < 0)
      log_line("icons: palette unstable rom reads");        /* best-effort: last attempt still lands */
    for (int i = 0; i < n; i++) pal_obj_mem[i] = s_stage[i]; }
  /* Artless + (cache or the user's ROM): the compiled palettes above were weak
   * zeros, so pull the game's own 3 shared icon palettes into banks 0..2 — the banks
   * BOTH the cache's and the ROM's palette ids (0..2) address (the cache's palette
   * bytes are a verbatim copy of the ROM's, made at extraction time, so either
   * source lands the SAME 3 palettes in the SAME banks). Compiled art present ->
   * compiled wins; the cache is tried before the rom (it is a plain RAM-array read,
   * no per-entry SD I/O once opened, vs. 3 separate rom_mon_icon_pal calls). */
  { const uint8_t* t; int b;
    if (!mon_icon_oam_for(1, &t, &b)) {
      if (s_iconcache_path) {
        /* art_icons_meta_pal_at lazily does art_icons_meta_load's f_open+f_read on
         * its first call for this path (real SD I/O) -- same freeze as cache_icon_
         * read above and the ROM rung below, for consistency (was unbracketed). */
        rumble_io_suspend();
        for (int pnum = 0; pnum < (int)ART_ICONS_PALS; pnum++) {
          uint16_t pd[16];
          /* bin_pal, NOT rom_pal: this is the icons.bin metadata tail, and a session
           * with no ROM open at all used to log `icons rung: cache (rom none, ...)`
           * followed by a span reading `rom 0/0/3` -- three ROM palette reads in a
           * session the same log says has no ROM. A counter that fires on the wrong
           * rung is a defect in the instrument, not a rounding error. */
          PERF_ICON(bin_pal);
          if (art_icons_meta_pal_at(s_iconcache_path, pnum, pd))
            for (int i = 0; i < 16; i++) pal_obj_mem[pnum * 16 + i] = pd[i];
        }
        rumble_io_resume();
      } else if (s_rommon) {
        uint16_t pd[16];
        for (int pnum = 0; pnum < ROM_MON_PALS; pnum++) {
          PERF_ICON(rom_pal);
          if (rom_mon_icon_pal(s_rommon, pnum, pd))
            for (int i = 0; i < 16; i++) pal_obj_mem[pnum * 16 + i] = pd[i];
        }
      }
    } }

#if !PDNA_HAND_ART_COMPILED
  /* Phase 3 (ROM-art): the glove's own palette, read + verified ONCE per box entry
   * (pose changes reuse it -- load_rom_hand_frame only ever swaps the tiles). Tiles
   * and palette are used as a PAIR: this flag is what load_rega_hand/load_regb_grab
   * gate on, so a palette that failed to read never gets paired with ROM tiles under
   * the fallback's colours (or vice versa) -- see the s_romhand_pal_ready comment
   * above load_regb_grab. */
  uint16_t romhandpal[16];
  s_romhand_pal_ready = 0;
  if (s_romhand) {
    rumble_io_suspend();
    s_romhand_pal_ready = rom_hand_pal(s_romhand, romhandpal);
    rumble_io_resume();
    if (!s_romhand_pal_ready) log_line("hand: rom palette unstable/unavailable");
  }
#endif
  /* hand palette -> bank 13 (normal) and an orange-tinted copy -> bank 14 (MOVE) */
  for (int i = 0; i < 16; i++) {
#if !PDNA_HAND_ART_COMPILED
    uint16_t c = s_romhand_pal_ready ? romhandpal[i] : hand_oam_pal[i];
#else
    uint16_t c = hand_oam_pal[i];
#endif
    pal_obj_mem[PB_HAND * 16 + i] = c;
    /* push toward orange: keep luma, bias R up / B down */
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    uint32_t lum = (r * 2 + g * 5 + b) >> 3;
    uint32_t nr = lum + 6; if (nr > 31) nr = 31;
    uint16_t o = (i == 0) ? 0 : (uint16_t)(nr | (((lum * 5) >> 3) << 5) | ((lum >> 2) << 10));
    pal_obj_mem[PB_HANDORG * 16 + i] = o;
  }
  /* item-marker glyph colours: bank 13 slots 4/5 (the hand art only uses 1..3, so
   * these don't collide). slot 4 = tan fill, slot 5 = dark border. */
  pal_obj_mem[PB_HAND * 16 + 4] = RGB15(28, 24, 6);
  pal_obj_mem[PB_HAND * 16 + 5] = RGB15(8, 6, 1);

  /* hand tiles -> region A (id 992) */
  load_rega_hand();                               /* region A = cursor hand        */
  load_regb_grab();                               /* default region B = grab fist  */

  /* hide every overlay entry up front */
  for (int i = OE_HAND; i < OE_COUNT; i++) hide(i);
  oam_copy(oam_mem, s_shadow, 128);                /* clear ALL hw OAM (64..127 unused -> no garbage) */

  /* Mode-3 BG2 (the wallpaper) defaults to priority 0, which would sit IN FRONT of
   * the prio-1/2 icon/carry sprites and hide the whole grid. Drop it to priority 3
   * so every sprite (hand 0 > carry 1 > icons 2 > BG2 3) composites above it. */
  REG_BG2CNT = (REG_BG2CNT & ~3) | 3;
  REG_DISPCNT |= DCNT_OBJ | DCNT_OBJ_1D;           /* enable OBJ, 1D tile mapping  */
}

void boxoam_exit(void) {
  REG_DISPCNT &= ~DCNT_OBJ;                         /* OBJ off for every other screen */
  REG_BLDCNT = 0;                                   /* drop any ITEM-mode blend     */
  REG_BG2CNT &= ~3;                                 /* restore BG2 priority 0       */
  oam_init(s_shadow, 128);
  oam_copy(oam_mem, s_shadow, 128);                 /* clear hardware OAM           */
  if (s_swapcache) { app_box_swap_release(); s_swapcache = 0; }  /* hand g_entries back */
  for (int i = 0; i < 30; i++) s_cache_ok[i] = 0;
  /* Every one of pdna_box's ~9 exit paths calls this, which makes it the ONE place a
   * screen-scoped rollup can be emitted without repeating it nine times (and without a
   * tenth path being added later that forgets). Main-loop level, between transfers, so
   * the flush inside is safe here for the same reason app_log_flush is. */
  perf_rep_flush(PERF_REP_PAGE);
  perf_rep_flush(PERF_REP_BOB);
}

void boxoam_suspend(void) { REG_DISPCNT &= ~DCNT_OBJ; REG_BLDCNT = 0; }
void boxoam_resume(void)  { REG_DISPCNT |= DCNT_OBJ | DCNT_OBJ_1D; }

/* (re)place grid slot s's icon sprite at its cell (incl. the current bob offset).
 *
 * s_bob is applied ONLY to a slot that cannot pose-swap (!s_pose_ok[s]): a pose-capable
 * slot's "other pose" is a TILE change (boxoam_set_frame/pose_swap_rom_slot), never a
 * position change, and s_bob is a SHARED, whole-box variable boxoam_set_frame now also
 * writes (for the benefit of the slots that DO need it) even on a box where every
 * occupied slot pose-swaps — without this gate, a LATER re-place of a pose-capable slot
 * (restore_slot after a chunk-carry uncover, boxoam_show_slot after a lift-hide) would
 * pick up that stale offset and nudge an icon whose two frames are meant to sit at the
 * exact same pixel, exactly the bug class box_oam.h's boxoam_set_frame comment already
 * warns about ("the icons park 1 px low forever"). */
static void place_grid_slot(int s) {
  int x = GRID_X + (s % COLS) * CELL_W;
  int y = GRID_Y + (s / COLS) * CELL_H + (s_pose_ok[s] ? 0 : s_bob);
  u16 a0 = ATTR0_SQUARE | ATTR0_4BPP | (y & ATTR0_Y_MASK);
  /* ITEM mode fades non-holders; rubber-band selection whitens marks (never both) */
  if (s_icon_blend[s] || s_selmark[s]) a0 |= ATTR0_BLEND;
  obj_set_attr(oe(OE_ICON0 + s), a0,
               ATTR1_SIZE_32 | (x & ATTR1_X_MASK),
               ATTR2_ID(TID_ICON0 + s * MON_ICON_OAM_TILES) | ATTR2_PRIO(2) |
               ATTR2_PALBANK(s_iconbank[s]));
}

/* Decide slot s's pose-swap capability and, where possible, cache its OTHER pose frame
 * -- the one piece of bookkeeping that must stay IDENTICAL whether a slot is being
 * freshly loaded (boxoam_load_box, frame0 == 0 by construction, s_frame just reset) or
 * re-derived after a chunk-carry uncover / party-panel close (restore_slot, frame0 ==
 * whatever s_frame currently is, which the covered slot's own bookkeeping cannot have
 * tracked while it was hidden). ONE function computing "the other frame" as
 * `1 - frame0` (never a hardcoded 1) is what makes restore_slot safe to add on top of
 * this cache: reusing a cache built for a stale frame0 is exactly the asymmetric-
 * exchange bug class this feature had to avoid (see swap_cache_slot's own comment).
 *
 * Eggs never pose-swap (a single frame, see boxoam_set_frame) regardless of source, so
 * their capability bit stays 0 even though icon_tiles() itself reports whatever `cheap`
 * the Egg row happened to resolve to. Compiled art / a cheap fused-ROM source (cheap!=0)
 * needs no cache at all -- swap_cache_slot's read-based sibling path in pose_swap_slot
 * already serves those without any EWRAM cost. Only a non-cheap source (the icons.bin
 * SD cache, or the user's own ROM registered off the SD card) reaches the fetch below,
 * and only when the box screen actually holds the borrowed g_entries buffer
 * (s_swapcache != 0, see boxoam_enter). A failed fetch (SD hiccup, or no cache held)
 * leaves s_cache_ok[s] at 0 and s_pose_ok[s] exactly what it was before this feature
 * existed -- the 1 px bob, never a hole. */
static void finish_slot_pose(int s, uint16_t species, uint8_t form, bool egg,
                             uint8_t frame0, int cheap) {
  s_pose_ok[s] = (!egg && cheap) ? 1 : 0;
  s_cache_ok[s] = 0;
  /* s_swapcache is only ever non-NULL under PDNA_POSE_EXPERIMENT==1 ('A') -- see
   * boxoam_enter -- so this whole branch, byte-for-byte the shipped feature's fill,
   * naturally only runs there. */
  if (!egg && !cheap && s_swapcache) {
    int bank1, from_rom1, cheap1;
    uint8_t other = (uint8_t)(1 - frame0);
    const uint8_t* t1 = icon_tiles(species, form, other, egg, &bank1, &from_rom1, &cheap1);
    if (t1) {
      uint8_t* c = s_swapcache + (uint32_t)s * (MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES);
      memcpy(c, t1, (size_t)(MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES));
      s_cache_ok[s] = 1;
#if PDNA_POSE_EXPERIMENT == 1
      /* 'A' = BORROW WITHOUT SWAPPING: the fill above just did the real acquire +
       * real SD/cache read + real memcpy into the borrowed g_entries, at the real
       * load-time cost -- but the grid must keep showing the 1 px bob, not the pose
       * swap, so pose_swap_rom_slot() must never reach its swap_cache_slot() branch
       * for this slot. Leaving s_cache_ok[s]=1 (a fill genuinely happened) but forcing
       * s_pose_ok[s] back to 0 (place_grid_slot's bob-gate reads this) does exactly
       * that with no other code path touched. */
      s_pose_ok[s] = 0;
#else
      s_pose_ok[s] = 1;
#endif
    }
  }
#if PDNA_POSE_EXPERIMENT == 2
  /* 'B' = SWAP WITHOUT BORROWING: g_entries/app_box_swap_acquire is never touched --
   * slot 0 alone gets a real fill into the dedicated s_expb_cache instead, through the
   * SAME verified icon_tiles() ladder every other fill uses. Every other slot's
   * s_pose_ok stays exactly what the top-of-function default set (0 for a non-cheap
   * source), so only slot 0 ever pose-swaps in this variant. */
  if (s == 0 && !egg && !cheap) {
    int bank1, from_rom1, cheap1;
    uint8_t other = (uint8_t)(1 - frame0);
    const uint8_t* t1 = icon_tiles(species, form, other, egg, &bank1, &from_rom1, &cheap1);
    if (t1) {
      memcpy(s_expb_cache, t1, (size_t)(MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES));
      s_expb_ok = 1;
      s_pose_ok[0] = 1;
    } else {
      s_expb_ok = 0;
    }
  } else if (s == 0) {
    s_expb_ok = 0;
  }
#endif
}

void boxoam_load_box(const PkMon box[30]) {
  s_frame = 0;                                       /* a fresh box always shows frame 0 */
  s_pend = 0;                                         /* void any pump half-swap the old box owed */
  /* a reload rewrites all 30 tile regions and re-shows every entry, so any borrowed
   * chunk regions / selection marks are void — the caller re-applies the chunk after */
  s_chunk_valid = 0;
  for (int s = 0; s < 30; s++) { s_covered[s] = 0; s_selmark[s] = 0; s_cache_ok[s] = 0; }
  s_any_pose = 0;
  for (int s = 0; s < 30; s++) {
    int bank, from_rom, cheap;
    bool egg = box[s].isEgg && !box[s].isBadEgg;
    const uint8_t* tiles = icon_tiles(box[s].species, box[s].form, 0, egg, &bank, &from_rom, &cheap);
    if ((egg || box[s].species) && tiles) {
      upload_icon(TID_ICON0 + s * MON_ICON_OAM_TILES, tiles, from_rom);
      s_occupied[s] = 1; s_isegg[s] = egg ? 1 : 0; s_iconbank[s] = (uint8_t)bank;
      s_species[s] = box[s].species; s_form[s] = box[s].form;
      /* frame0 is 0 here (s_frame was just reset above) -- finish_slot_pose fetches
       * frame 1 verified and, off a non-cheap source with the swap cache held, caches
       * it so this slot's tick-time swap needs no SD I/O (MUST-FIX 2 follow-up,
       * 2026-08-22 review). */
      finish_slot_pose(s, box[s].species, box[s].form, egg, 0, cheap);
      if (s_pose_ok[s]) s_any_pose = 1;
      place_grid_slot(s);
    } else {
      s_occupied[s] = 0; s_isegg[s] = 0; s_pose_ok[s] = 0; s_cache_ok[s] = 0;
      hide(OE_ICON0 + s);
    }
  }
}

/* Lightweight, UNVERIFIED per-tick fetch of a cheap-ROM slot's OTHER pose frame:
 * rom_mon_icon() does its own fresh locate (2 small reads) + the 512 B frame read,
 * with NO shared memo and NO double-compare. Both omissions are deliberate:
 *   - sharing s_iconloc (the load-time memo) would let an unverified tick-time
 *     locate get silently TRUSTED by a later verified caller (rom_icon_read_verified
 *     skips its own re-locate whenever the memo already matches the species/form) —
 *     see rom_mon_locate_verified's header comment on exactly that hazard;
 *   - a double-compare read is the load-time cost this data already paid once at
 *     boxoam_load_box; a glitched tick-time swap self-heals at the very next swap
 *     (<= 1 tick later, same tolerance upload_tiles's plain-copy pose swap already
 *     has for compiled art, box_oam.h's boxoam_set_frame comment).
 * Only ever called when s_rom_cheap (a fused, cart-space memcpy, not SD I/O) — see
 * pose_tile() below, the only caller. */
static int rom_icon_pose_frame(uint16_t species, uint8_t form, uint8_t frame, int* bank) {
  if (!s_rommon) return 0;
  uint8_t pal = 0;
  /* rom_mon_icon() is a locate + ONE frame read in one call, with no memo and no
   * double-compare -- counted as both so the span line still adds up. */
  PERF_ICON(rom_loc); PERF_ICON(rom_frm);
  if (!rom_mon_icon(s_rommon, species, form, frame, (uint8_t*)s_stage, &pal)) return 0;
  *bank = pal;
  return 1;
}

/* True VRAM<->EWRAM-cache exchange for grid slot s (2026-08-22 review, MUST-FIX 2
 * follow-up): the SD/icons.bin-cache-sourced counterpart to rom_icon_pose_frame above,
 * for a slot finish_slot_pose() already proved s_cache_ok[s] for. Zero per-tick I/O —
 * the cache was populated ONCE, at load/restore time, by the same double-verified
 * ladder frame 0 uses.
 *
 * DELIBERATELY a blind swap, not a frame-indexed read: it never asks "which frame
 * number am I moving to", it only ever trades whatever VRAM currently holds for
 * whatever the cache currently holds. finish_slot_pose() is the ONE place that
 * establishes the invariant this depends on ("the cache always holds the frame that is
 * NOT currently in VRAM") for a given slot, and every call here preserves it by
 * construction: after the exchange, each buffer holds exactly what the other one held
 * a moment ago. A frame-indexed design (read frame X, write if going TO X, skip if
 * going FROM X) is one asymmetric branch away from a swap that copies data IN on one
 * transition with no matching copy OUT on the other — that class of bug is exactly
 * what silently drains every slot toward one stale value after enough cycles, and a
 * blind three-step exchange cannot have it: there is no frame-number branch to get
 * backwards. s_stage is the temp — 512 B, exactly one icon, touched by nothing else
 * between these three statements (one synchronous call, nothing reentrant can run
 * mid-exchange on this single-threaded ARM7TDMI main loop) — so reusing it per slot,
 * per call, is safe with no accumulation across slots or ticks. */
static void swap_cache_slot(int s) {
  const int n = MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES;      /* 512 B, 4-aligned */
  uint8_t* cache = s_swapcache + (uint32_t)s * (uint32_t)n;
  uint16_t* vram = (uint16_t*)((uint8_t*)tile_mem_obj[0] +
                               (uint32_t)(TID_ICON0 + s * MON_ICON_OAM_TILES) * 32);
  dma3_cpy(s_stage, vram, (uint32_t)n);      /* temp  = VRAM  (the frame now on screen) */
  dma3_cpy(vram, cache, (uint32_t)n);        /* VRAM  = cache (the frame NOT on screen) */
  dma3_cpy(cache, s_stage, (uint32_t)n);     /* cache = temp  (what VRAM just gave up)  */
}

#if PDNA_POSE_EXPERIMENT == 2
/* Experiment 'B' only: bit-for-bit swap_cache_slot's three-DMA exchange, but always
 * slot 0 and always against the dedicated s_expb_cache instead of a g_entries offset.
 * See the PDNA_POSE_EXPERIMENT block at the top of this file. */
static void swap_expb_slot(void) {
  const int n = MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES;      /* 512 B, 4-aligned */
  uint16_t* vram = (uint16_t*)((uint8_t*)tile_mem_obj[0] + (uint32_t)TID_ICON0 * 32);
  dma3_cpy(s_stage, vram, (uint32_t)n);            /* temp  = VRAM  (frame now on screen) */
  dma3_cpy(vram, s_expb_cache, (uint32_t)n);       /* VRAM  = cache (frame NOT on screen) */
  dma3_cpy(s_expb_cache, s_stage, (uint32_t)n);    /* cache = temp  (what VRAM gave up)   */
}
#endif

#if PDNA_POSE_EXPERIMENT == 3
/* Experiment 'C' only: bit-for-bit swap_cache_slot's three-dma3_cpy, 512 B shape --
 * same volume, same call count, same DMA-register/timing profile per slot -- but
 * CONTENT-INVARIANT: slot s's VRAM tiles are DMA'd out to s_stage and immediately
 * back in unchanged, so nothing on screen ever moves. The third DMA (s_expb_cache =
 * s_stage) exists ONLY to spend the same third 512 B of bus time the real exchange's
 * "cache = temp" step spends; s_expb_cache's bytes are never read back for display,
 * so its content doesn't matter and it can be shared, unsynchronized, across all 30
 * slots' calls within one tick (nothing reentrant runs between them -- straight-line
 * synchronous loop, exactly the same guarantee swap_cache_slot's own comment relies
 * on for s_stage). Applies to slot s regardless of s_occupied/s_pose_ok: reproducing
 * the real feature's WORST-CASE per-tick volume needs all 30 slots hit every time,
 * not however many the loaded save happens to occupy -- see the caller. */
static void swap_volume_slot(int s) {
  const int n = MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES;      /* 512 B, 4-aligned */
  uint16_t* vram = (uint16_t*)((uint8_t*)tile_mem_obj[0] +
                               (uint32_t)(TID_ICON0 + s * MON_ICON_OAM_TILES) * 32);
  dma3_cpy(s_stage, vram, (uint32_t)n);            /* temp    = VRAM (bytes unchanged)   */
  dma3_cpy(vram, s_stage, (uint32_t)n);            /* VRAM    = temp (restores itself)   */
  dma3_cpy(s_expb_cache, s_stage, (uint32_t)n);     /* scratch = temp (3rd DMA, volume only) */
}
#endif

/* Re-upload slot s's OTHER-pose tiles at `frame`, through the cheap-ROM rung
 * (rom_icon_pose_frame) or the SD/cache swap-cache (swap_cache_slot) — compiled art is
 * handled separately, synchronously, in boxoam_set_frame itself (see that function's
 * header for why the split MUST stop at the compiled/ROM boundary). Callers already
 * restrict WHICH slots they pass (the even half inline in boxoam_set_frame, the odd
 * half in boxoam_pose_pump's own `s += 2` loop), so this does not re-check parity.
 * Silently does nothing for a slot that is no longer eligible (unoccupied, egg,
 * covered, not pose_ok, or — despite s_pose_ok[s] — actually a COMPILED slot:
 * mon_icon_oam_for_form_frame is tried first and, on success, this function does
 * nothing further, because boxoam_set_frame already uploaded every compiled slot
 * synchronously, even and odd alike). s_cache_ok[s] and s_rom_cheap are mutually
 * exclusive per slot by construction (finish_slot_pose only builds a cache entry for a
 * `!cheap` source), so exactly one of the two branches below can ever fire for a given
 * slot — the pre-existing cheap-ROM path is untouched code, reached exactly as before,
 * for exactly the slots it always served. */
static void pose_swap_rom_slot(int s, uint8_t frame) {
  if (!s_occupied[s] || s_isegg[s] || s_covered[s] || !s_pose_ok[s]) return;
  const uint8_t* t; int b;
  if (mon_icon_oam_for_form_frame(s_species[s], s_form[s], frame, &t, &b)) return; /* compiled: already done */
  /* The box's retention ratio, and the same question art_fallbacks.c's 3-slot MRU
   * answers for the dex: did this bob tick come out of RAM, or off the card? The cache
   * branch is a pure VRAM<->EWRAM exchange (zero I/O); everything below it is a real
   * fetch. Without this pair a box span had a numerator (bin/rom reads) and no
   * denominator, so "the retention work landed" could not be told from "this box has
   * nothing to retain".
   *
   * EXPECT `icons 0/N mru` FROM A SHIPPING BUILD, and that is the correct reading, not
   * a broken counter: s_swapcache is NULL unless PDNA_POSE_EXPERIMENT==1 (see
   * boxoam_enter's DISABLED-PENDING-A-HARDWARE-A/B note), so finish_slot_pose never
   * fills a slot and s_cache_ok[] is never set. Every box bob tick is a real fetch
   * today. That zero is the BEFORE number the retention mandate is measured against. */
  if (s_cache_ok[s]) { PERF_ICON(mru_hit); swap_cache_slot(s); return; }
  PERF_ICON(mru_miss);
#if PDNA_POSE_EXPERIMENT == 2
  if (s == 0 && s_expb_ok) { swap_expb_slot(); return; }
#endif
  if (!s_rom_cheap || !rom_icon_pose_frame(s_species[s], s_form[s], frame, &b)) return;
  upload_tiles(TID_ICON0 + s * MON_ICON_OAM_TILES, s_stage,
              MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES);
}

/* The real Gen-3 box "bob" — a 2-frame pose swap. DMA the chosen frame's tiles for every
 * occupied icon into the SAME OBJ VRAM window (the two frames can't both fit, so we swap).
 *
 * WHY THE TWO FRAMES CANNOT BOTH BE RESIDENT, since it keeps getting re-asked: the box
 * screen is a BITMAP mode, so OBJ tile memory is only the upper half, tiles 512..1023 =
 * 16 KiB. This module already spends all of it — 30 icons x 16 tiles = 480 (TID_ICON0),
 * plus 16 for the hand (TID_HAND 992) and 16 for region B (TID_REGB 1008) = exactly 512
 * tiles. There is no second frame's worth of VRAM, at any price.
 *
 * The re-upload itself is cheap and DOES fit the vblank: upload_tiles is dma3_cpy, so a
 * full box is 15,360 B = 3,840 words, roughly 9 K cycles against vblank's 83,776 — about
 * 11%. (The "cannot fit the vblank window" note at the top of this file is about
 * upload_tiles_VERIFIED, which reads every word back a second time. This path uses the
 * plain copy: the icons were already verified when the box was staged, and a bob tick
 * re-sends bytes that are known good.)
 *
 * PER-SLOT capability, not whole-box: s_pose_ok[s] (built at boxoam_load_box from
 * icon_tiles()'s *cheap output) can differ slot to slot within the SAME box — e.g. one
 * species served by the icons.bin cache (real SD I/O, never cheap) sitting next to
 * another the cache doesn't have that fell through to a FUSED ROM (a cart-space
 * memcpy, cheap). Compiled art and a cheap ROM source pose-swap for real; everything
 * else keeps the 1 px positional bob — applied only to ITS OWN cell, via s_bob (which
 * this function also drives for exactly that purpose; boxoam_set_bob is the pure
 * "nothing at all can pose-swap" fallback the caller reaches for when this returns 0).
 *
 * COMPILED slots update HERE, synchronously, for every index — bit-for-bit the same
 * per-tick work the full-art build has always done, so a full-art box is unaffected
 * down to the frame. Only CHEAP-ROM slots (a fused cart-space memcpy: a fresh locate +
 * 512 B read per slot, not a bare DMA) split across two vblank ticks — even here, odd
 * in the very next vblank's boxoam_pose_pump() call, NOT a same-tick call after this
 * one (MUST-FIX 1, 2026-08-22 review — see boxoam_pose_pump's own header for the call-
 * order requirement that makes the split real) — which is a real cost only the artless
 * build's cheap-ROM rung ever pays, matching the design this file's header block
 * (icon_tiles' *cheap comment) already committed to.
 *
 * Returns 1 if it animated, 0 if it could not — the caller uses that to fall back to the
 * 1 px positional bob rather than leaving the grid dead. */
int boxoam_set_frame(int frame) {
#if PDNA_POSE_EXPERIMENT == 3
  /* Experiment 'C': fire on the SAME cadence the real feature would (every call this
   * function gets, i.e. every ANIM_PERIOD toggle from pdna_box.c's box loop) and
   * BEFORE the s_any_pose early-return below -- a box with nothing marked pose-
   * capable must still get the full volume, or this variant proves nothing about the
   * feature's worst case. Even half now, odd half deferred to boxoam_pose_pump's own
   * `s_vol_pend` drain next tick -- the same 15-now/15-next-tick split the real
   * feature uses (see this file's header block, variant 'C' entry). */
  for (int s = 0; s < 30; s += 2) swap_volume_slot(s);
  s_vol_pend = 1;
#endif
  /* CAPABILITY FIRST, BEFORE the already-on-that-frame short-circuit. Getting this
   * order wrong is not cosmetic: on a box that cannot pose-swap, s_frame never advances
   * past 0, so an equality test placed above this line answers "1, already there" on
   * every even tick. The caller then skips its boxoam_set_bob(0), the grid nudges down
   * on odd ticks and never comes back up, and the icons park 1 px low forever — worse
   * than the bob this was meant to replace. That shipped in 010ec90; this is the fix. */
  if (!s_any_pose) return 0;      /* NOTHING in this box can pose-swap: whole-grid bob */
  frame &= 1;
  if (frame == s_frame) return 1;               /* already there == animating fine */
  s_frame = frame;
  s_bob = frame;      /* the offset place_grid_slot applies to non-pose-capable slots */
  int any_rom_pending = 0;
  for (int s = 0; s < 30; s++) {
    if (!s_occupied[s] || s_isegg[s] || s_covered[s]) continue;
    if (!s_pose_ok[s]) {                        /* this ONE slot can't pose-swap: nudge it */
      int y = GRID_Y + (s / COLS) * CELL_H + s_bob;
      obj_set_pos(oe(OE_ICON0 + s), GRID_X + (s % COLS) * CELL_W, y);
      continue;
    }
    const uint8_t* t; int b;
    if (mon_icon_oam_for_form_frame(s_species[s], s_form[s], (uint8_t)frame, &t, &b)) {
      upload_tiles(TID_ICON0 + s * MON_ICON_OAM_TILES, t,
                   MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES);   /* compiled: always now */
      continue;
    }
    if ((s & 1) == 0) pose_swap_rom_slot(s, (uint8_t)frame);       /* cheap-ROM, even: now */
    else any_rom_pending = 1;                                       /* cheap-ROM, odd: pump */
  }
  if (any_rom_pending) { s_pend = 1; s_pend_frame = frame; }
  return 1;
}

/* The deferred half of a pose swap: the ODD-indexed cheap-ROM slots boxoam_set_frame
 * left for next tick (compiled slots never defer — see that function's header). A pure
 * no-op once caught up (s_pend clears the moment it runs, and is never even SET unless
 * a cheap-ROM slot actually deferred), so callers can call this UNCONDITIONALLY every
 * vblank tick — see box_oam.h. In a full-art build s_pend is never 1, so this compiles
 * to a single branch that is never taken: zero added per-tick cost there.
 *
 * CALL ORDER (MUST-FIX 1, 2026-08-22 review — see box_oam.h's header on this function
 * for the full story): must run at the TOP of a tick, right after that tick's own
 * s_vsync() and BEFORE that tick's own boxoam_set_frame() call, so the half it drains
 * here was queued by the PREVIOUS tick's set_frame — never the one about to run below
 * it. pdna_box.c's box loop is the only caller. */

/* Canary-trip response (MUST-FIX 1, 2026-08-23 review): a trip that only appends one
 * line to the RAM log is worthless as evidence -- nothing in the box-entry path ever
 * calls app_log_flush() on a clean run, so the ONE run that actually proves an overrun
 * is exactly the run whose proof never reaches the card before the console dies (the
 * incident writeup: white wash -> RGB banding -> evolving noise over ~12 frames, then
 * a power-cycle). Two responses, both fired at most ONCE per box visit (the static
 * latch below), the moment app_box_swap_canary_ok() first returns false:
 *   - app_log_flush() ONE TIME. This is deliberately NOT the periodic/unconditional
 *     breadcrumb flush e37f14b's revert already blocked (that one fired every tick on
 *     a fixed cadence, forever, on every run, healthy or not -- SD I/O inside vblank as
 *     routine cost). This is the opposite shape: it fires zero times on every run that
 *     never trips, and at most once on the run that does, latched by a counter exactly
 *     like upload_tiles_verified's own s_icon_flushes < 2 idiom just above in this
 *     file -- the anomaly-flush pattern this file already uses (also 336/348 above,
 *     and pdna_box.c's draw_wallpaper). One SD write is a real cost to pay on the
 *     screen under investigation, but the alternative is throwing away the only
 *     evidence a trip ever produces -- see this function's own definition below for
 *     why a stuck/looping tick can't turn this into unbounded I/O either.
 *   - canary_alarm() below: a zero-I/O, VRAM-only + PSG/rumble cue, so Guy can tell a
 *     canary trip from a plain crash even with no card to pull afterward (the crash
 *     itself is visually a wash-to-white/banding/noise progression -- this cue has to
 *     read as deliberate against that, not blend into it). */
static void canary_alarm(void) {
  /* Solid, sharp-edged, saturated magenta band across the very top of the screen --
   * unlike the crash's own white wash / RGB banding / evolving noise, this is a flat
   * fill with a hard bottom edge, so it can't be mistaken for another instance of the
   * symptom under investigation. Placed at y=0 rather than inside the grid/wallpaper
   * so it never depends on this screen's box-tab-specific layout (PANEL_W/GRID_X/
   * WP_X all vary by mode) -- what gets overdrawn there does not matter: a trip means
   * the console is expected to die within frames, so preserving any other on-screen
   * content is not a competing goal. Plain u16 vid_mem stores (Mode-3 bitmap, no
   * palette/DMA involved) -- zero SD/ROM I/O, matching this response's own contract. */
  const COLOR c = RGB15(31, 0, 31);
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 240; x++)
      vid_mem[y * 240 + x] = c;
  snd_deny();     /* audible buzz + rumble cue (already this file's/pdna_box.c's UI-deny
                    * sound; zero SD/ROM I/O, safe every-frame per snd.h) */
}

void boxoam_pose_pump(void) {
  /* Canary check (2026-08-23 A/B, part C): pdna_box.c's box loop already calls this
   * function UNCONDITIONALLY every vblank tick (box_oam.h's own doc on this function),
   * so it is the one place in this file guaranteed to run every tick regardless of
   * s_pend/s_frame state -- the right anchor for a check that must not be skippable.
   * app_box_swap_canary_ok() is a no-op returning true whenever the borrow isn't held
   * (PDNA_POSE_EXPERIMENT != 1, i.e. every shipped build and variant B), so this costs
   * one already-cheap function call, zero SD I/O, on every build but 'A'. See
   * canary_alarm()'s own comment just above for what happens the first time it
   * returns false -- ONE flush + a zero-I/O visible/audible cue, latched so a stuck
   * or looping tick can never turn either into per-tick cost. */
  static int s_canary_alarms = 0;
  if (!app_box_swap_canary_ok() && s_canary_alarms < 1) {
    s_canary_alarms++;
    app_log_flush();
    canary_alarm();
  }
#if PDNA_POSE_EXPERIMENT == 3
  /* Experiment 'C': drain the odd half boxoam_set_frame queued this same way the real
   * feature's odd half is drained -- unconditional, orthogonal to s_pend/s_frame (this
   * variant never sets those). */
  if (s_vol_pend) {
    s_vol_pend = 0;
    for (int s = 1; s < 30; s += 2) swap_volume_slot(s);
  }
#endif
  if (!s_pend) return;
  s_pend = 0;
  uint8_t frame = (uint8_t)s_pend_frame;
  for (int s = 1; s < 30; s += 2) pose_swap_rom_slot(s, frame);
}

void boxoam_set_bob(int dy) {
  if (dy == s_bob) return;
  s_bob = dy;
  for (int s = 0; s < 30; s++) {
    if (!s_occupied[s]) continue;
    int y = GRID_Y + (s / COLS) * CELL_H + s_bob;
    obj_set_pos(oe(OE_ICON0 + s), GRID_X + (s % COLS) * CELL_W, y);
  }
}

static void hand_xy(int cur, int* hx, int* hy) {
  /* The glove art's pointing fingertip sits at sprite-local (13,0): hx+13 =
   * cell_x+16 -> hx = cell_x+3. The REST HEIGHT is retail's (hand at cellY-12,
   * docs/retail-pickup-capture.md §1d): high enough that the grab dip (+8) puts the
   * fingers ON the mon, the fist closes at that exact spot, and the carried mon
   * rides 4 px under the fist -> floating at cellY-8, all with one anchor and no
   * pop at any pose swap. */
  *hx = GRID_X + (cur % COLS) * CELL_W + 3;
  *hy = GRID_Y + (cur / COLS) * CELL_H - 12;
  if (*hy < WP_Y) *hy = WP_Y;
}

void boxoam_cursor(int cur, int title_row, int mode) {
  load_rega_hand();                                  /* region A back to the hand (a grab/carry may have borrowed it) */
  int hx, hy;
  /* On the box name the hand used to park mid-banner — dead centre of "NAME  n/30", so it
   * covered the count on the Bank and the tail of the name on a PC box. The banner's own
   * left arrow is decoration (the right one says the same thing), so the hand goes there
   * instead: it still points at the banner, and no data is ever underneath it. */
  if (title_row == 1) { hx = WP_X + 1; hy = 13; }
  else if (title_row >= 2) {
    /* Tab row (2026-08-23 fix): re-anchor per SELECTED tab instead of reusing the
     * banner's single fixed spot. Same "point in from the region's own left edge"
     * convention hand_xy() uses for a grid cell (fingertip a few px inside the left
     * edge, not centred) — and hy lands INSIDE the tab bar's own y=0..11 band (at the
     * label text's own y=2, ui_text(tx,2,...) in draw_tab), not one row below its
     * bottom edge the way the reused banner y=13 did. */
    static const int TAB_X0[3] = { TAB_X0_PKMN, TAB_X0_PARTY, TAB_X0_SAVE };
    hx = TAB_X0[title_row - 2] + 1;
    hy = 2;
  }
  else { hand_xy(cur, &hx, &hy); hx += s_cur_dx; hy += s_cur_dy;   /* dip + slide ride the hand */
#if !PDNA_HAND_ART_COMPILED
         hy -= s_hand_bob;   /* MUST-FIX 2: streamed-hand BOUNCE fallback, see boxoam_hand_pose() */
#endif
       }

  int bank = (mode == BOXOAM_HAND_MOVE) ? PB_HANDORG : PB_HAND;
  int prio = 0;
  u16 a0 = ATTR0_SQUARE | ATTR0_4BPP | (hy & ATTR0_Y_MASK);
  if (mode == BOXOAM_HAND_ITEM) {
    /* ITEM mode: the hand stays OPAQUE. It used to carry ATTR0_BLEND (semi-transparent),
     * which dimmed the mon it hovered — but a mon that HOLDS an item must read normally
     * (Guy). Only the NON-holder icons fade (their own ATTR0_BLEND set in place_grid_slot);
     * keep BLDCNT configured for THEM. And drop the hand to priority 1 so the prio-0
     * held-item preview (boxoam_carry_item hover) pops IN FRONT of the hand, not behind it.
     * (BLD_OBJ stays in the 2nd/bottom target only, so overlapping faded icons blend
     * uniformly; never put it in the 1st-target mask or every object would fade.) */
    prio = 1;
    REG_BLDCNT = PSS_BLDCNT;
    REG_BLDALPHA = PSS_BLDALPHA;                     /* retail: 7/16 obj + 11/16 below */
  } else if (!s_select_on && !s_chunk_on) {
    /* don't kill the selection/chunk brightness blend — the select loop repositions
     * the cursor every step and would otherwise un-whiten the marked icons */
    REG_BLDCNT = 0;
  }
  obj_set_attr(oe(OE_HAND), a0,
               ATTR1_SIZE_32 | (hx & ATTR1_X_MASK),
               ATTR2_ID(TID_HAND) | ATTR2_PRIO(prio) | ATTR2_PALBANK(bank));
  /* showing the hand means we're not move-carrying: hide carry sprites */
  hide(OE_GRAB); hide(OE_CARRY);
}

/* Carry a HELD mon (move mode), mon-in-hand model. The held mon's icon is decoded into
 * region A (the cursor hand's 16 tiles, free while carrying) so it survives box reloads,
 * and rides FRONT-MOST (PRIO 0) above every box icon (PRIO 2). An orange, semi-transparent
 * grab fist sits BEHIND it (region B, PRIO 1). The cursor hand is hidden. species 0 -> just
 * the fist (empty hand). The caller hides the origin slot via boxoam_hide_slot(). */
void boxoam_carry_held(int cur, uint16_t species, uint8_t form, bool egg) {
  /* RETAIL GEOMETRY (measured + decomp §1d of docs/retail-pickup-capture.md): the
   * fist is the HAND at its rest anchor with swapped tiles — same position, so the
   * open->fist swap never pops — drawn IN FRONT, with the carried mon riding 4 px
   * BELOW it. +s_cur_dy is the grab/place dip driver as before. */
  int hx, hy; hand_xy(cur, &hx, &hy);
  int fx = hx + s_cur_dx, fy = hy + s_cur_dy; if (fy < WP_Y) fy = WP_Y;
  int ix = fx - 3 + 0, iy = fy + 4;              /* mon: centred under the fist */
  load_regb_grab();                                  /* fist tiles -> region B */
  REG_BLDCNT = 0;                                    /* carried mon is opaque  */
  int bank = 0, from_rom = 0, cheap = 0;
  const uint8_t* tiles = icon_tiles(species, form, 0, egg, &bank, &from_rom, &cheap);
  if (tiles) {                                       /* the held mon (or Egg) rides the glove */
    upload_icon(TID_HAND, tiles, from_rom);
    s_rega = 2;                                      /* region A now holds the held mon */
    obj_set_attr(oe(OE_CARRY),                       /* the mon, BEHIND the fist */
                 ATTR0_SQUARE | ATTR0_4BPP | (iy & ATTR0_Y_MASK),
                 ATTR1_SIZE_32 | (ix & ATTR1_X_MASK),
                 ATTR2_ID(TID_HAND) | ATTR2_PRIO(1) | ATTR2_PALBANK(bank));
  } else hide(OE_CARRY);
  /* the closed hand IN FRONT, white like retail's, at the hand's own anchor */
  obj_set_attr(oe(OE_GRAB),
               ATTR0_SQUARE | ATTR0_4BPP | (fy & ATTR0_Y_MASK),
               ATTR1_SIZE_32 | (fx & ATTR1_X_MASK),
               ATTR2_ID(TID_GRAB) | ATTR2_PRIO(0) | ATTR2_PALBANK(PB_HAND));
  hide(OE_HAND);                                     /* hand hidden while carrying */
}

void boxoam_carry_end(void) { hide(OE_CARRY); hide(OE_GRAB); }   /* stop carrying */
void boxoam_hide_slot(int s) { if (s >= 0 && s < 30) hide(OE_ICON0 + s); }  /* lift-hide the origin */
/* Undo a lift-hide WITHOUT re-uploading: a plain hide only touched the OAM entry, the
 * slot's tiles are still in VRAM, so re-placing the attrs is enough. The grab dip uses
 * this to keep the mon visibly in its cell while the open hand descends onto it. */
void boxoam_show_slot(int s) { if (s >= 0 && s < 30 && s_occupied[s]) place_grid_slot(s); }

/* Re-upload grid slot s's own icon (current bob frame) from the per-slot bookkeeping
 * and re-show/hide it — undoes a chunk borrow of that slot's tile region.
 *
 * MUST re-derive the swap cache here too, not just the VRAM tiles: a covered slot's
 * pose_swap_slot() calls are skipped outright (the `s_covered[s]` guard), but the
 * BOX-WIDE s_frame keeps toggling every ~0.5 s the whole time it's covered — so by the
 * time this runs, s_frame may have flipped an odd number of times since the slot was
 * covered, and the cache built back at boxoam_load_box (paired with frame 0) would no
 * longer be the correct "other frame" for whatever s_frame is NOW. finish_slot_pose()
 * always derives "other" as 1 - the frame it was just handed, so calling it with THIS
 * upload's actual frame (not a stale assumption) re-syncs the cache to the freshly
 * uploaded VRAM content regardless of how many toggles happened out of sight — the
 * same invariant boxoam_load_box establishes, just re-proven at a possibly different
 * frame parity. Skipping this would reintroduce exactly the "cache disagrees with
 * VRAM" class of bug this feature exists to avoid, just gated on chunk-carry/party-
 * panel use instead of a general per-tick failure. */
static void restore_slot(int s) {
  if (!s_occupied[s]) { hide(OE_ICON0 + s); return; }
  int bank, from_rom, cheap;
  const uint8_t* tiles = icon_tiles(s_species[s], s_form[s], (uint8_t)s_frame,
                                    s_isegg[s], &bank, &from_rom, &cheap);
  if (tiles) {
    upload_icon(TID_ICON0 + s * MON_ICON_OAM_TILES, tiles, from_rom);
    s_iconbank[s] = (uint8_t)bank;
    finish_slot_pose(s, s_species[s], s_form[s], s_isegg[s] != 0, (uint8_t)s_frame, cheap);
    place_grid_slot(s);
  } else { hide(OE_ICON0 + s); s_cache_ok[s] = 0; }
}

void boxoam_select_mark(const uint8_t sel[30]) {
  for (int s = 0; s < 30; s++) {
    uint8_t m = sel[s] ? 1 : 0;
    if (m != s_selmark[s]) { s_selmark[s] = m; if (s_occupied[s]) place_grid_slot(s); }
  }
  /* HW-PROVEN cue (Guy, 2026-07-18): the semi-transparent-OBJ -> brightness-mode
   * whiten (GBATEK-documented, emulator-clean) renders NO effect at all on the real
   * Omega DE + GBA SP display. Use the one per-sprite effect this hardware has
   * demonstrably honored (ITEM mode, same registers): alpha-blend the CHOSEN icons
   * against the wallpaper below — they go ghost-translucent while the rest stay
   * solid. (Emerald whitens; translucent is the nearest look the HW gives us.) */
  REG_BLDCNT = PSS_BLDCNT;
  REG_BLDALPHA = PSS_BLDALPHA;                       /* retail: 7/16 icon + 11/16 below */
  s_select_on = 1;
}

/* During the rubber-band drag the glove is HIDDEN: sprites cannot alpha-blend with
 * other sprites on this hardware, so the glove sitting on the corner cell made that
 * one chosen mon read as opaque (Guy's HW round 2026-07-27) — with the glove away,
 * every chosen mon ghosts uniformly and the ghost-block IS the cursor. */
void boxoam_select_cursor(void) { hide(OE_HAND); hide(OE_GRAB); hide(OE_CARRY); }

void boxoam_select_clear(void) {
  for (int s = 0; s < 30; s++)
    if (s_selmark[s]) { s_selmark[s] = 0; if (s_occupied[s]) place_grid_slot(s); }
  s_select_on = 0;
  REG_BLDCNT = 0;
}

/* §12b bitmap-understudy hooks: weak no-ops so box_oam links standalone; the box
 * screen (pdna_box.c) provides the real bitmap blit/restore. See box_oam.h. */
__attribute__((weak)) void boxoam_under_show(int slot) { (void)slot; }
__attribute__((weak)) void boxoam_under_hide(int slot) { (void)slot; }

void boxoam_chunk_carry(int tr, int tc, int fist_r, int fist_c,
                        const BoxOamChunkMon* mons, int n, bool fit, int lift) {
  uint8_t newcov[30];
  for (int s = 0; s < 30; s++) newcov[s] = 0;
  for (int i = 0; i < n && i < 30; i++)
    newcov[(tr + mons[i].rr) * COLS + (tc + mons[i].cc)] = 1;
  /* tiles need re-DMA only when the cell->mon mapping moved (or a reload voided it) */
  bool full = !s_chunk_valid || tr != s_chunk_tr || tc != s_chunk_tc;

  for (int s = 0; s < 30; s++)                       /* uncovered cells get their mon back */
    if (s_covered[s] && !newcov[s]) { s_covered[s] = 0; restore_slot(s); boxoam_under_hide(s); }

  for (int i = 0; i < n && i < 30; i++) {
    int r = tr + mons[i].rr, c = tc + mons[i].cc, s = r * COLS + c;
    int bank = 0, from_rom = 0, cheap = 0;
    const uint8_t* tiles = icon_tiles(mons[i].species, mons[i].form, 0, mons[i].egg,
                                      &bank, &from_rom, &cheap);
    bool have = (tiles != 0);
    if (!s_covered[s]) {                             /* cover TRANSITION: put the occupant into
                                                      * the bitmap so the ghost blends over it */
      s_covered[s] = 1;
      if (s_occupied[s]) boxoam_under_show(s);
    }
    hide(OE_ICON0 + s);                              /* the block occludes this cell anyway */
    if (!have) { hide(OE_MARK0 + i); continue; }     /* no icon data -> same degrade as load_box */
    if (full) upload_icon(TID_ICON0 + s * MON_ICON_OAM_TILES, tiles, from_rom);
    int x = GRID_X + c * CELL_W, y = GRID_Y + r * CELL_H - lift;   /* Emerald carries the block 8px up */
    if (y < WP_Y) y = WP_Y;
    u16 a0 = ATTR0_SQUARE | ATTR0_4BPP | ATTR0_BLEND | (y & ATTR0_Y_MASK);
    /* the block stays ghosted for the WHOLE carry (Emerald keeps the lifted block
     * whitened until placement — Guy: "keep the transparent effect until I place
     * them"); a blocked drop just ghosts HARDER (register weights below) */
    obj_set_attr(oe(OE_MARK0 + i), a0,
                 ATTR1_SIZE_32 | (x & ATTR1_X_MASK),
                 ATTR2_ID(TID_ICON0 + s * MON_ICON_OAM_TILES) | ATTR2_PRIO(1) |
                 ATTR2_PALBANK(bank));
  }
  for (int i = n; i < 30; i++) hide(OE_MARK0 + i);

  load_regb_grab();                                  /* fist rides the release cell, above the block */
  int fx = GRID_X + fist_c * CELL_W + 3, fy = GRID_Y + fist_r * CELL_H - 2 - lift;
  if (fy < WP_Y) fy = WP_Y;
  obj_set_attr(oe(OE_GRAB),
               ATTR0_SQUARE | ATTR0_4BPP | (fy & ATTR0_Y_MASK),
               ATTR1_SIZE_32 | (fx & ATTR1_X_MASK),
               ATTR2_ID(TID_GRAB) | ATTR2_PRIO(1) | ATTR2_PALBANK(PB_HANDORG));
  hide(OE_HAND); hide(OE_CARRY);
  /* ONE transparency, not two. This used to ghost the carried block HARDER when it
   * could not be dropped here; retail has no such state — an impossible multi-move
   * just plays a failure sound and leaves the look alone, and drop_chunk already
   * calls snd_deny. The carried block still never goes solid until it is placed. */
  (void)fit;
  REG_BLDCNT = PSS_BLDCNT;
  REG_BLDALPHA = PSS_BLDALPHA;
  s_chunk_tr = tr; s_chunk_tc = tc;
  s_chunk_on = 1; s_chunk_valid = 1;
}

void boxoam_chunk_end(void) {
  for (int s = 0; s < 30; s++)
    if (s_covered[s]) { s_covered[s] = 0; restore_slot(s); boxoam_under_hide(s); }
  for (int i = 0; i < 30; i++) hide(OE_MARK0 + i);
  hide(OE_GRAB);
  s_chunk_on = 0; s_chunk_valid = 0;
  REG_BLDCNT = 0;
}

/* See box_oam.h. Unpacks the SAME 4bpp tile bytes + OBJ palette bank the sprite path
 * uses (icon_tiles(), the shared lookup restore_slot/place_grid_slot already call)
 * into RGB15 pixels written straight to the Mode-3 framebuffer — no intermediate
 * buffer (a 32x32 RGB15 icon is 2 KiB, too big for a local per §0's stack rule, and
 * there is no EWRAM headroom to spare one statically either). Compiled .rodata tile
 * bytes get the same verified-staged read every OTHER icon upload in this file uses
 * (icopy_verified into s_stage) — this cartridge has measurably garbled raw CPU ROM
 * reads before (see icopy_verified's own comment) — but a ROM-streamed/cache tiles
 * pointer is already s_stage itself (icon_tiles's from_rom contract), so re-staging
 * it would be a same-buffer copy; read it directly. */
void boxoam_slot_blit_bitmap(int s) {
  if (s < 0 || s >= 30 || !s_occupied[s]) return;
  int bank, from_rom, cheap;
  const uint8_t* tiles = icon_tiles(s_species[s], s_form[s], (uint8_t)s_frame, s_isegg[s], &bank, &from_rom, &cheap);
  if (!tiles) return;
  const uint8_t* src = tiles;
  if (!from_rom) {
    if (icopy_verified(s_stage, (const uint16_t*)tiles, 256) < 0)
      log_line("icons: strip-blit unstable rom reads");
    src = (const uint8_t*)s_stage;
  }
  const uint16_t* pal = &pal_obj_mem[bank * 16];
  int x0 = GRID_X + (s % COLS) * CELL_W, y0 = GRID_Y + (s / COLS) * CELL_H;
  for (int ty = 0; ty < 4; ty++)
    for (int tx = 0; tx < 4; tx++) {
      const uint8_t* t = src + (unsigned)(ty * 4 + tx) * 32;   /* 32 B = one 8x8 4bpp tile */
      for (int ry = 0; ry < 8; ry++)
        for (int rx = 0; rx < 4; rx++) {
          uint8_t byte = t[ry * 4 + rx];
          uint8_t lo = byte & 0xF, hi = (uint8_t)(byte >> 4);
          int px = tx * 8 + rx * 2, py = ty * 8 + ry;
          if (lo) vid_mem[(y0 + py) * 240 + (x0 + px)]     = (COLOR)pal[lo];
          if (hi) vid_mem[(y0 + py) * 240 + (x0 + px + 1)] = (COLOR)pal[hi];
        }
    }
}

/* MUST-FIX 5 (2026-08-20 review): see box_oam.h's own comment for the full rationale
 * (bitmap, not OBJ, because it can be clipped, and it's safe now that MUST-FIX 2
 * hides the only OBJ that used to need to ride above these tiles). Mirrors
 * boxoam_slot_blit_bitmap's tile-unpack loop exactly, but bounds-checks each
 * destination pixel against the caller's clip rect instead of always writing the
 * full 32x32 span. */
int boxoam_icon_blit_clip(int x, int y, int cx0, int cy0, int cx1, int cy1,
                           uint16_t species, uint8_t form, bool egg) {
  if (!species && !egg) return 0;
  int bank, from_rom, cheap;
  const uint8_t* tiles = icon_tiles(species, form, 0, egg, &bank, &from_rom, &cheap);
  if (!tiles) return 0;                               /* artless / not in this source */
  const uint8_t* src = tiles;
  if (!from_rom) {
    if (icopy_verified(s_stage, (const uint16_t*)tiles, 256) < 0)
      log_line("icons: strip-blit-clip unstable rom reads");
    src = (const uint8_t*)s_stage;
  }
  const uint16_t* pal = &pal_obj_mem[bank * 16];
  for (int ty = 0; ty < 4; ty++)
    for (int tx = 0; tx < 4; tx++) {
      const uint8_t* t = src + (unsigned)(ty * 4 + tx) * 32;   /* 32 B = one 8x8 4bpp tile */
      for (int ry = 0; ry < 8; ry++) {
        int py = y + ty * 8 + ry;
        if (py < cy0 || py >= cy1) continue;                  /* row clipped out          */
        for (int rx = 0; rx < 4; rx++) {
          uint8_t byte = t[ry * 4 + rx];
          uint8_t lo = byte & 0xF, hi = (uint8_t)(byte >> 4);
          int px = x + tx * 8 + rx * 2;
          if (lo && px >= cx0 && px < cx1)     vid_mem[py * 240 + px]     = (COLOR)pal[lo];
          if (hi && px + 1 >= cx0 && px + 1 < cx1) vid_mem[py * 240 + px + 1] = (COLOR)pal[hi];
        }
      }
    }
  return 1;
}

/* MUST-FIX 2 (2026-08-20 review): see box_oam.h's own comment. */
void boxoam_strip_hide_cursor(void) {
  hide(OE_HAND); hide(OE_GRAB); hide(OE_CARRY); hide(OE_CITEM);
}

/* ------- PC-box party PANEL: hide the covered columns, borrow one slot per row
 * PLUS one extra slot anywhere in the scan, for the 6th (offset "slot 1") icon -------
 *
 * TILE ARITHMETIC (the panel now occludes 4 of the box's 6 grid columns, not ~1):
 * PDNA_PCP_OCCLUDE_X0..X1 spans x=82..176, i.e. GRID_X + 0*CELL_W .. GRID_X + 4*CELL_W
 * with CELL_W=24 -- columns 0-3 (x82-178, four 24px columns) all overlap that range at
 * least partially, column 4 (x178-202) does not (178 >= 176). An OBJ can't be partially
 * hidden, so EVERY cell in columns 0-3 gets hidden for all 5 box rows: 4 cols x 5 rows =
 * 20 hidden cells. Of those, the party panel needs exactly 6 borrowed OAM entries/tile
 * ranges (5 column tiles + 1 offset slot-1 tile) -- 1 per box row for the column (5
 * total, unchanged from before) plus ONE more taken from whichever cell is the SECOND
 * hit in any row's scan (here always row 0's own 2nd hidden column, since rows are
 * scanned in order and row 0 fills first) -- 6 of the 20 hidden cells are reused as the
 * panel's own OAM entries and tile ranges; the other 14 are genuinely just hidden (their
 * true icon is repainted at its native grid position as a Mode-3 BITMAP by the caller's
 * strip_restore_icons() before the panel's own chrome draws over the part that's really
 * covered -- see pdna_box.c). This borrows existing OBJ tile ranges (TID_ICON0 + s*16)
 * and OAM entries only; it claims ZERO new OBJ tiles, keeping the exactly-512-tile
 * budget this file's own header comment documents. */
static int s_strip_reuse[ROWS];      /* grid slot reused by strip row r (column slots 2-6);
                                      * -1 = none                                        */
static int s_strip_extra = -1;       /* one more hidden slot, reserved for the panel's
                                      * offset "slot 1" icon; -1 = none                  */

void boxoam_strip_open(int x0, int x1) {
  for (int r = 0; r < ROWS; r++) s_strip_reuse[r] = -1;
  s_strip_extra = -1;
  for (int s = 0; s < 30; s++) {
    int cx0 = GRID_X + (s % COLS) * CELL_W, cx1 = cx0 + CELL_W;
    if (cx1 <= x0 || cx0 >= x1) continue;              /* this cell's column misses the panel */
    if (!s_covered[s]) { s_covered[s] = 1; hide(OE_ICON0 + s); }
    int row = s / COLS;
    if (s_strip_reuse[row] < 0) s_strip_reuse[row] = s;        /* 1st hit this row -> the
                                                                 * column tile for this row */
    else if (s_strip_extra < 0) s_strip_extra = s;             /* 2nd hit anywhere -> the
                                                                 * offset slot-1 tile      */
  }
}

static void strip_draw(int s, int x, int y, uint16_t species, uint8_t form, bool egg) {
  if (s < 0) return;                          /* no grid cell under the panel to reuse */
  if (!species && !egg) { hide(OE_ICON0 + s); return; }
  int bank, from_rom, cheap;
  const uint8_t* tiles = icon_tiles(species, form, 0, egg, &bank, &from_rom, &cheap);
  if (!tiles) { hide(OE_ICON0 + s); return; }             /* artless / not in this source */
  upload_icon(TID_ICON0 + s * MON_ICON_OAM_TILES, tiles, from_rom);
  u16 a0 = ATTR0_SQUARE | ATTR0_4BPP | (y & ATTR0_Y_MASK);
  obj_set_attr(oe(OE_ICON0 + s), a0,
               ATTR1_SIZE_32 | (x & ATTR1_X_MASK),
               ATTR2_ID(TID_ICON0 + s * MON_ICON_OAM_TILES) | ATTR2_PRIO(2) |
               ATTR2_PALBANK(bank));
}

void boxoam_strip_slot(int row, int x, int y, uint16_t species, uint8_t form, bool egg) {
  if (row < 0 || row >= ROWS) return;
  strip_draw(s_strip_reuse[row], x, y, species, form, egg);
}

/* The 6th icon: retail's offset "slot 1", alone at the panel's own measured position
 * (PDNA_PCP_S1_*). Draws through the SAME borrowed-OAM mechanism as boxoam_strip_slot,
 * just off the s_strip_extra reservation instead of a per-row one. */
void boxoam_strip_slot1(int x, int y, uint16_t species, uint8_t form, bool egg) {
  strip_draw(s_strip_extra, x, y, species, form, egg);
}

void boxoam_strip_close(void) {
  for (int s = 0; s < 30; s++)
    if (s_covered[s]) { s_covered[s] = 0; restore_slot(s); }
  for (int r = 0; r < ROWS; r++) s_strip_reuse[r] = -1;
  s_strip_extra = -1;
}

void boxoam_item_markers(const PkMon box[30], bool show) {
  /* ITEM mode fades the icons so the cursor + item badges read clearly — but a mon
   * that HOLDS an item stays opaque (and its item badge/the carried item are opaque),
   * so you can see who has what. Update the per-icon blend bit + re-place any changed. */
  for (int s = 0; s < 30; s++) {
    uint8_t b = (show && box[s].species && !box[s].heldItem) ? 1 : 0;
    if (b != s_icon_blend[s]) { s_icon_blend[s] = b; if (s_occupied[s]) place_grid_slot(s); }
  }
  /* No per-holder glyph badges: region B now holds the FULL-SIZE item icon, and a mon
   * that holds an item is shown opaque (above) while the rest fade — so the cursor's
   * holder reveals its real item via the full-size preview (boxoam_carry_item). */
  for (int m = 0; m < 30; m++) hide(OE_MARK0 + m);
}

void boxoam_carry_item(int cur, uint16_t item, bool full) {
  int cx = GRID_X + (cur % COLS) * CELL_W, cy = GRID_Y + (cur / COLS) * CELL_H;
  if (!item) {                                       /* nothing held/hovered -> clear item sprites */
    hide(OE_CITEM); if (full) hide(OE_GRAB);
    return;
  }
  if (full) {
    /* GRAB: the FULL 32x32 item rides in front of EVERYTHING (region A, PRIO 0), held by an
     * orange grab fist behind it (region B, PRIO 1). Hand hidden.
     *
     * The fist is OPAQUE. Retail's cursor and the thing in its hand are both plain
     * ST_OAM_OBJ_NORMAL — the storage system's only alpha is on the item-less GRID
     * icons in MOVE-ITEMS mode. A see-through hand was ours, and it read as the hand
     * being the ghost rather than the icons it was hovering over. This also stops
     * boxoam_carry_item owning the blend registers at all: boxoam_cursor's ITEM branch
     * already set them on the frame before (oam_sync runs it first). */
    load_regb_item(item, true, TID_HAND); s_rega = 1;
    load_regb_grab();                                /* fist tiles -> region B */
    int ix = cx - (CITEM_OFF + 1), iy = cy - (CITEM_OFF - 1); if (iy < WP_Y) iy = WP_Y;
    int fx = cx + 3, fy = cy - 6; if (fy < WP_Y) fy = WP_Y;
    obj_set_attr(oe(OE_GRAB),                        /* orange grab fist, opaque, behind the item */
                 ATTR0_SQUARE | ATTR0_4BPP | (fy & ATTR0_Y_MASK),
                 ATTR1_SIZE_32 | (fx & ATTR1_X_MASK),
                 ATTR2_ID(TID_GRAB) | ATTR2_PRIO(1) | ATTR2_PALBANK(PB_HANDORG));
    obj_set_attr(oe(OE_CITEM),                       /* the full item, opaque, front-most */
                 ATTR0_SQUARE | ATTR0_4BPP | (iy & ATTR0_Y_MASK),
                 ATTR1_SIZE_32 | (ix & ATTR1_X_MASK),
                 ATTR2_ID(TID_HAND) | ATTR2_PRIO(0) | ATTR2_PALBANK(PB_CITEM));
    hide(OE_HAND);
  } else {
    /* HOVER: small 16x16 item at the cell's bottom-CENTRE, ON the mon (PRIO 0). Was at the
     * bottom-LEFT (cx-3), which spilled into the left-neighbour cell and read as "behind the
     * left neighbour" — centre it on the holder so it clearly belongs to THIS mon. */
    load_regb_item(item, false, TID_CITEM); s_regb = 1;
    int ix = cx + 6, iy = cy + CELL_H - 6; if (iy < WP_Y) iy = WP_Y;
    obj_set_attr(oe(OE_CITEM),
                 ATTR0_SQUARE | ATTR0_4BPP | (iy & ATTR0_Y_MASK),
                 ATTR1_SIZE_16 | (ix & ATTR1_X_MASK),
                 ATTR2_ID(TID_CITEM) | ATTR2_PRIO(0) | ATTR2_PALBANK(PB_CITEM));
  }
}

void boxoam_commit(void) {
  oam_copy(oam_mem, s_shadow, OE_COUNT);
}
