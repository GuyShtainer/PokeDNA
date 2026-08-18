/*
 * pdna_romver_data.c — the window table, alone in its own translation unit.
 *
 * It is separate from pdna_romver.c ON PURPOSE, exactly as rom-load-lab keeps
 * diag_data.c apart from selfcheck.c (diag_data.c:11-19). The initialiser below is
 * mostly zeroes; a compiler that could SEE it would be entitled to fold the verifier's
 * reads into compile-time constants and the instrument would end up checksumming zeroes
 * instead of the cartridge. Only an `extern` is visible to the verifier, which denies it
 * that knowledge.
 *
 *   >>> Do NOT add -flto. <<<  It restores exactly that cross-TU visibility.
 *
 * ANCHORS ARE WEAK ON PURPOSE. The artless build (Guy's product bar, 2026-08-08) and
 * `make sd` (Makefile drops source/embed, which defines mon_back_blob,
 * mon_back_shiny_blob and mon_front_shiny_blob) link without some of these symbols. A
 * weak undefined symbol resolves to address 0 instead of erroring, so those windows
 * stamp as "absent" and the runtime skips them: no false alarm, and the mechanism stays
 * live through the control/code/table windows, which exist in EVERY build.
 *
 * The offset is a SEPARATE field from base_addr and must stay that way: `&sym[0x200000]`
 * on an absent weak symbol resolves to 0x00200000, not to 0, and the absence test breaks.
 *
 * TWO TOOLCHAIN FACTS, BOTH MEASURED HERE ON devkitARM gcc 15.2.0, NOT REMEMBERED:
 *  * A weak undefined anchor links clean as 0x00000000; a defined one relocates to its
 *    address. (Test: five anchors, one absent, linked with gba.specs and dumped.)
 *  * An R_ARM_ABS32 against a THUMB FUNCTION symbol carries the interworking bit: `nm`
 *    reported 0800026c and the relocated word read 0800026d. So a function anchor lands
 *    here ODD. pdna_rv_check masks bit 0, and the stamper masks identically. Data and
 *    linker symbols (ui_font_bits, __text_end, g_pdna_fuse, the blobs) are unaffected.
 *  * `main` is deliberately NOT an anchor: declaring it as an object trips -Wmain, and
 *    this project builds -Wall. pdna_map stands in for that slab of .text.
 *
 * TWO KINDS OF WINDOW
 * -------------------
 * PDNA_RVW_EXACT — the anchor IS the object being sampled (the art blobs). The stamper
 *   makes the window stay inside the anchor's extent, so if a blob shrinks the build
 *   fails loudly instead of silently checksumming its neighbour.
 * (no EXACT flag) — a REGION PROBE. The anchor is just the nearest global symbol below
 *   the region; the window deliberately runs past it into whatever follows. That is the
 *   only way to sample this build's generated lookup tables at all: s_species, s_base,
 *   s_wild_*, k_stored_off, k_box_off, k_substruct_pos, the TM/tutor learnset indices —
 *   every one of them is a FILE-STATIC array (`nm` shows lowercase `r`), so none can be
 *   named by an extern. What is stamped is "the 4 KiB at this address in THIS build",
 *   re-derived on every link; the tag names the region, not one array.
 *
 * WHY THE SAMPLE IS AIMED WHERE IT IS
 * -----------------------------------
 * The obvious plan — one window per art blob — samples only the region whose corruption
 * CANNOT produce the symptom being chased. Garbage pixels give a garbled screen; a hang
 * needs a wild pointer or length out of a corrupted TABLE, or corrupted code. So three
 * windows sit in .text (the Gen-3 save-open/verify call graph, the flashcart+FatFs
 * sector path, and main), four sit on generated lookup tables including the save
 * parser's own offset tables, one is the low-ROM control, and nine art windows remain as
 * a spread probe across 0.76 - 12.14 MB so a mid-file hole still has a chance of being
 * seen. Totals and honest detection rates are in pdna_romver.h.
 *
 * OFFSET RULES, all enforced at build time by tools/stamp_rom_windows.py, which fails
 * the build rather than shipping a window the loader will rewrite behind our back:
 * 4-aligned; not in the first 0x100 bytes; not in the last 64 KiB; not overlapping the
 * descriptor's own 624 bytes; not overlapping the 16-byte g_pdna_fuse locator (tools/
 * fuse_rom.py patches it post-link); no aligned 0x03007FFC / 0x03FFFFFC word inside;
 * not a single repeated byte (a constant window cannot tell a good load from a zero
 * fill); and, for EXACT windows, inside the anchor's extent.
 */

#include "pdna_romver.h"

#define RV_ANCHOR(sym) extern const unsigned char sym[] __attribute__((weak))

/* --- present in every build ------------------------------------------------ */
RV_ANCHOR(ui_font_bits);          /* PokeDNA's own 5x7 font (source/ui_font.c)  */
RV_ANCHOR(__text_end);            /* linker symbol: start of .rodata            */
RV_ANCHOR(g_pdna_fuse);           /* fused-ROM locator (source/fused_rom.c)     */
RV_ANCHOR(gen3_find_section);     /* Gen-3 save open/verify call graph          */
RV_ANCHOR(flashcartio_activate);  /* cart detect + sector I/O + FatFs disk glue */
RV_ANCHOR(pdna_map);              /* a third slab of .text, further up          */
/* --- art; any of these may be absent -------------------------------------- */
RV_ANCHOR(hand_oam_cursor_tiles); /* also the anchor for the learnset tables    */
RV_ANCHOR(bag_bg_blob);
RV_ANCHOR(card_bg_blob);
RV_ANCHOR(mon_front_blob);
RV_ANCHOR(mon_icon_blob);
RV_ANCHOR(pokeblock_bg_blob);
RV_ANCHOR(mon_back_blob);         /* absent in `make sd` */
RV_ANCHOR(mon_front_shiny_blob);  /* absent in `make sd` */

#define RV_W(SYM, OFF, LEN, FLAGS, A, B, C, D) \
  { (uint32_t)(SYM), (OFF), (LEN), 0u, (FLAGS), { A, B, C, D } }

#define RV_ART (PDNA_RVW_EXACT | PDNA_RVW_K_ART)

const PdnaRomVerify g_pdna_romver __attribute__((used, aligned(4))) = {
    .magic0    = PDNA_RV_MAGIC0,
    .magic1    = PDNA_RV_MAGIC1,
    .version   = PDNA_RV_VERSION,
    .stamped   = 0u,   /* everything below is filled in post-link by the tool */
    .n_windows = 17u,
    .w = {
        /* control: low ROM, tiny, in every build including artless. If we are executing
         * at all this region arrived, so a mismatch here is evidence about the BUS. */
        RV_W(ui_font_bits, 0x000000u, 512u,
             PDNA_RVW_CONTROL | PDNA_RVW_EXACT | PDNA_RVW_K_TEXT, 'C','T','L','O'),

        /* --- code: the two call graphs whose corruption could actually hang us ---- */
        RV_W(gen3_find_section,    0x000000u, 4096u, PDNA_RVW_K_TEXT, 'T','S','A','V'),
        RV_W(flashcartio_activate, 0x000000u, 4096u, PDNA_RVW_K_TEXT, 'T','S','D','I'),
        RV_W(pdna_map,             0x000000u, 4096u, PDNA_RVW_K_TEXT, 'T','M','A','P'),

        /* --- generated lookup tables (all file-static; region probes) ------------- */
        /* +0x3000: s_tmhm / s_nature / s_type / s_contest / the s_nflag_* blocks      */
        RV_W(__text_end,   0x003000u, 4096u, PDNA_RVW_K_TABLE, 'T','B','L','0'),
        /* +0x8000: s_t1 / s_base (base stats) / s_location                           */
        RV_W(__text_end,   0x008000u, 4096u, PDNA_RVW_K_TABLE, 'T','B','L','1'),
        /* +0x40: clear of BOTH post-link locator records — g_pdna_fuse (16 B, patched
         * by tools/fuse_rom.py) and g_pdna_sav, which the DELTA link places immediately
         * after it, at g_pdna_fuse+0x10, and which tools/fuse_sav.py patches. A window
         * at +0x10 covered the second one, so every fused emulator image would have
         * reported a false "ROM IMAGE MODIFIED" and turned editing off. The stamper now
         * refuses any window overlapping either record, so this cannot come back
         * silently. Past them lie k_dex_from_index, k_spans, k_stored_off, k_box_off,
         * k_substruct_pos: the Gen-3 parser's OWN offset tables, i.e. the likeliest
         * source of a wild read if corrupted.                                          */
        RV_W(g_pdna_fuse,  0x000040u, 4096u, PDNA_RVW_K_TABLE, 'T','B','L','2'),
        /* +0x200: past the 512-byte cursor tile set — off_tbl, s_tm_move,
         * s_tutor_move, s_tut_ix (the TM/tutor learnset indices).                     */
        RV_W(hand_oam_cursor_tiles, 0x000200u, 4096u, PDNA_RVW_K_TABLE, 'T','B','L','3'),

        /* --- art: a spread probe from 0.76 MB to 12.14 MB ------------------------ */
        RV_W(bag_bg_blob,          0x000000u, 4096u, RV_ART, 'B','A','G',' '),
        RV_W(card_bg_blob,         0x000000u, 4096u, RV_ART, 'C','B','G','0'),
        RV_W(card_bg_blob,         0x200000u, 4096u, RV_ART, 'C','B','G','1'),
        RV_W(card_bg_blob,         0x400000u, 4096u, RV_ART, 'C','B','G','2'),
        RV_W(mon_front_blob,       0x000000u, 4096u, RV_ART, 'F','R','N','0'),
        RV_W(mon_icon_blob,        0x100000u, 4096u, RV_ART, 'I','C','N','1'),
        RV_W(pokeblock_bg_blob,    0x000000u, 4096u, RV_ART, 'P','B','L','K'),
        RV_W(mon_back_blob,        0x0A0000u, 4096u, RV_ART, 'B','C','K','1'),
        RV_W(mon_front_shiny_blob, 0x0A0000u, 4096u, RV_ART, 'S','H','F','1'),
        /* entries 17..23 stay zero; the stamper asserts len==0 beyond n_windows, which
         * is what catches a table edit that forgets to bump .n_windows. */
    },
};
