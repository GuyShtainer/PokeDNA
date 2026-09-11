#ifndef PDNA_GB_ITEM_NAMES_H
#define PDNA_GB_ITEM_NAMES_H
/*
 * Real Gen-1/Gen-2 item NAMES -- an embedded identifier table (GREEN per
 * docs/kb/licensing.md and BACKLOG.md:210: id->name lookups for
 * species/item/move/location are facts, not prose, safe to ship). No ROM
 * read, no locator: the two tables below are typed into this file's own
 * format from the decomps' CONSTANTS files (reference-only, cited per
 * table in gb_item_names.c), the same posture source/data_tables.c already
 * uses for pk_item_name(). TM/HM labels are SYNTHESIZED at call time, not
 * tabled (see gb_item_names.c for the exact per-generation id ranges, which
 * were RE-DERIVED from the constants files -- the Gen-2 range this file
 * ships differs from an earlier draft's guess, see the .c header comment).
 *
 * Pure C: no tonc/GBA headers, only <stdint.h>/<stdbool.h> -- dual-compiles
 * into tests/host_gbitemnames_test.c (hard rule 5).
 */
#include <stdint.h>
#include <stdbool.h>

/* Matches GB_GEN1/GB_GEN2 in gb_edit.h (1/2) numerically, but this header
 * stays dependency-free on purpose -- callers already have `gen` as a
 * uint8_t/int from a GbSession/GbScreen and pass it straight through. */
#define GBIN_GEN1 1
#define GBIN_GEN2 2

/* Table-only lookups (no TM/HM synthesis). Returns NULL for id 0 (NO_ITEM),
 * an out-of-table id, or a documented unused hole in that generation's real
 * item-id space -- NEVER a crash, NEVER an empty string. The returned
 * pointer is a string literal (static storage, never freed, never mutated
 * by the caller). */
const char* gb1_item_name(uint8_t id);
const char* gb2_item_name(uint8_t id);

/* The one call a screen makes: table name, else synthesized TM%02u/HM%02u,
 * else false (the caller keeps its own "ITEM-n" fallback). `gen` is
 * GBIN_GEN1 or GBIN_GEN2; any other value is treated as Gen 1. `out` must
 * hold at least GB_ITEM_NAME_MAXLEN+1 bytes; `cap` is the caller's actual
 * buffer size (never trusted blindly -- always bounded to it). */
bool gb_item_label(int gen, uint8_t id, char* out, int cap);

/* The longest real (tabled) name in either table -- 12, "FULL RESTORE" /
 * "BIKE VOUCHER" (Gen 1), "MASTER BALL" (Gen 2, len 11) tie the Gen-1
 * badges and several Gen-2 12-char names ("SILVERPOWDER", "RAGECANDYBAR",
 * ...). TM/HM synthesis never exceeds 4 ("TM01".."HM07"). */
#define GB_ITEM_NAME_MAXLEN 12

/* The two real-shell screens' own name-FIELD widths (columns), pinned here
 * so a future table entry longer than either cap fails the host test
 * instead of silently overflowing on screen (review-sonnet ruling,
 * gbnames brief): the quantity is drawn on the ROW BELOW the name (both
 * shells), so the name field itself runs from NAME_COL to the box's last
 * interior column, not to the shell's own QTY_COL constant.
 *   Gen 1 (source/pdna_gbbag.c): NAME_COL=6 .. last interior col 18 -> 13.
 *   Gen 2 (source/pdna_gbpack_body.inc): NAME_COL=8 .. last interior col
 *   19 (the pack list runs to the screen edge, no border there) -> 12. */
#define GB1_SHELL_NAME_CAP 13
#define GB2_SHELL_NAME_CAP 12

#endif
