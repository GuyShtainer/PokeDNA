/* tests/host_gbsurgery_tool.c — a CLI over gb_session/gb_edit/gb_editor for the S4
 * retail-boot regression gate (tools/gb_retail_gate.py) to build EDITED Gen-1/2 saves
 * from a corpus save without hand-writing bytes.
 *
 * Named *_tool (not *_test) so tests/run_host_tests.py's `host_*_test.c` glob ignores
 * it: this is a driver PROGRAM, not a self-checking test.
 *
 * Build line kept in sync with tools/gb_retail_gate.py's SURGERY_SRCS (same order):
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests \
 *      tests/host_gbsurgery_tool.c \
 *      source/gb_session.c \
 *      source/gb_editor.c \
 *      source/gb_edit.c \
 *      source/gen1_save.c \
 *      source/gen1_write.c \
 *      source/gen2_save.c \
 *      source/gen2_write.c \
 *      source/data_tables.c \
 *      source/rom_gbsprite.c \
 *      source/gb_sprite_codec.c \
 *      source/rom_gbbase.c \
 *      source/rom_gblearn.c \
 *      source/gb_new_mon.c \
 *      source/gb_trainer.c \
 *      source/gb_fields.c \
 *      source/gb_bag.c \
 *      source/gb_daycare.c \
 *      source/gb_clock.c \
 *      source/gb_fly.c \
 *      source/gb_boxnames.c \
 *      source/gb_flags.c \
 *      source/gb_flags_rw.c \
 *      source/gb_hof.c \
 *      source/gb_dex.c \
 *      source/gen3_to_gb.c \
 *      source/gb_moves_legal.c \
 *      source/gen3_mon.c \
 *      source/gen3_box.c \
 *      source/evolutions.c \
 *      source/gen3_save.c \
 *      source/gen3_edit.c \
 *      source/gen3_daycare.c \
 *      source/rom_gbmap.c \
 *      -o /tmp/hgbsurg
 *
 * (gb_fields.c is also required -- gb_trainer.c already needs it -- but was already
 * missing from this comment before this change; tools/gb_retail_gate.py's own
 * SURGERY_SRCS list, the actual source of truth this comment claims to mirror, has
 * always carried it. Left as pre-existing drift, not introduced here; only gb_dex.c
 * is this commit's own addition to both places.)
 *
 * Usage
 * -----
 *   host_gbsurgery_tool --in SAVE --out EDITED [--rom ROM] [--op ...]...
 *     --op nick BOX SLOT TEXT      gb_load -> gbe_set_text(GBE_NICK) -> gb_commit_checked
 *     --op ot BOX SLOT TEXT        same, GBE_OT
 *     --op level BOX SLOT N        gb_set_level(N); party slots also get gbe_settle_stats
 *     --op dv BOX SLOT STAT V      STAT in atk|def|spe|spc, V 0..15
 *     --op delete BOX SLOT         gbs_delete
 *     --op move FROM_BOX SLOT TO_BOX   gbs_move (prints the landing slot)
 *     --op money VALUE             gbs_write_field + gbs_finish (BACKLOG #49 P0's field-
 *                                   write primitive, not a Pokemon op). VALUE is
 *                                   0..999999 decimal; encoded BCD for Gen 1 (0x25F3) or
 *                                   binary for Gen 2 (0x23DB G/S, 0x23DC Crystal) — the
 *                                   same value reads as the same digits on both,
 *                                   docs/GEN12-PARITY-DESIGN.md §1.1.
 *     --op create BOX DEX          BACKLOG #50: gb_new_mon() -> gbs_insert (box only,
 *                                  same as pdna_gen12.c's own gb_create_hook -- see
 *                                  its own comment for why the party is refused). NO
 *                                  level argument (G1 review LOW-3, 2026-09-08: this
 *                                  line still said "BOX DEX LEVEL" after the UX-parity
 *                                  rewrite dropped the level picker -- and this tool's
 *                                  own do_create() to match, see its header comment) --
 *                                  the level is rom_gblearn_min_level()'s own answer,
 *                                  computed the same way gb_create_hook computes it.
 *                                  Needs --rom (a Gen-1/2 ROM matching SAVE's own
 *                                  generation); the OT name/id are the fixed test
 *                                  values "GATEX"/12345, not SAVE's own trainer --
 *                                  this tool has no reader for that field and the
 *                                  retail-gate case this exists for does not need it.
 *     --op badges MASK             BACKLOG #49 P1a, via gb_trainer.h's gbt_read/gbt_write:
 *                                   MASK (0..255) lands on Gen 1's single BADGES byte, or
 *                                   on BOTH Gen 2's Johto and Kanto badge bytes. Every
 *                                   other trainer-card field round-trips unchanged (that
 *                                   is what tests/host_gbtrainer_test.c's diff check
 *                                   already proves) — this is a read-modify-write of the
 *                                   whole GbTrainer, not a raw byte poke.
 *     --op name TEXT               same gb_trainer.h path: sets the player name. Refused
 *                                   (exit 1) if TEXT is over GB_OT_GLYPHS (7) glyphs or
 *                                   has a glyph this generation's GB charset cannot spell
 *                                   exactly — the same refusal gbt_write documents.
 *     --op badges2 JOHTO KANTO     Gen 2 only (refused on Gen 1): sets the Johto and
 *                                   Kanto badge bytes to two DIFFERENT masks, so a
 *                                   test driver can tell them apart — --op badges sets
 *                                   both Gen-2 bytes to the SAME mask, which makes a
 *                                   byte swap between them invisible (P1a review D8).
 *     --op statusflags BYTE        BACKLOG #96 D10: GBF_STATUS_FLAGS raw byte write
 *                                   (gbs_write_field + gbs_finish), Gen 2 only —
 *                                   Gold/Silver 0x23D9, Crystal 0x23DA. Bit 0 is
 *                                   STATUSFLAGS_POKEDEX_F, gating the Gen-2 card's
 *                                   own POKeDEX row.
 *     --op gender 0|1              BACKLOG #96 Kris: GBF_GENDER raw byte write,
 *                                   Crystal only (0 male, 1 female) — Gold/Silver
 *                                   have no gender concept, refused.
 *   host_gbsurgery_tool --in SAVE --list
 *     print every box: count, and per slot species dex / level / nickname
 *
 * BOX accepts 0..n-1 or the literal "party" (resolved through gbs_party_box() once the
 * session is open, so the same token works for both generations without the caller
 * knowing which one it is).
 *
 * TEXT (nick/ot) longer than the game's glyph cap (GB_NICK_GLYPHS 10 / GB_OT_GLYPHS 7)
 * is NOT refused: gbe_set_text -> gb_set_nickname/gb_set_otname silently truncate at a
 * glyph boundary, the same limit the game's own keyboard enforces (gb_edit.h's NAMES
 * block calls this the one loss that is deliberately not refused). A caller that needs
 * to know whether its text got shortened should read it back with --list afterwards
 * rather than trust the exit code.
 *
 * OPS APPLY IN ORDER, on the SAME in-memory image, each seeing every prior op's result
 * — there is no batch/transaction semantics beyond "all ops succeed or nothing is
 * written". This matters most for delete/move: a delete SHIFTS every later slot in that
 * box/party down by one, so `--op delete party 0 --op nick party 0 X` renames the
 * Pokemon that WAS in slot 1 before the delete, not the one that was in slot 0. Chain
 * ops with that in mind (gb_retail_gate.py's box0->party case relies on exactly this:
 * `--op delete party N --op move 0 0 party` frees a party slot before the move needs
 * one, in a single invocation, on one file).
 *
 * Reads the whole file (up to 65536 B) into a static buffer, gbs_open()s it, applies
 * every --op IN ORDER, and on total success writes the WHOLE image — same length as the
 * input, RTC tail included, since none of these operations change a Game Boy save's file
 * length — to --out. A refusal (a GbsStatus other than GBS_OK, or a setter/gb_load that
 * returned false) prints the reason and exits 1 WITHOUT writing --out at all — so a
 * caller can tell "the edit was refused" from "the edit landed" by whether the output
 * file exists. Usage errors (bad flags, a bad BOX/STAT token, wrong argument count) exit
 * 2 before any file is touched.
 *
 * No stdio inside the pure-C modules this links (gb_session.c / gb_edit.c / gb_editor.c
 * / gen1_save.c / gen1_write.c / gen2_save.c / gen2_write.c); this file is the only place
 * in the link that is allowed to use it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gb_session.h"
#include "gb_editor.h"
#include "rom_gbsprite.h"
#include "rom_gbbase.h"
#include "rom_gblearn.h"
#include "rom_gbmap.h"
#include "gb1_warp.h"     /* map-gen1 review D4: gb1warp_viewptr/gb1warp_coord --
                            * the exact write shape do_warp_vp() below mirrors */
#include "gb_new_mon.h"
#include "data_tables.h"
#include "gb_trainer.h"
#include "gb_bag.h"
#include "gb_daycare.h"
#include "gb_clock.h"
#include "gb_fly.h"
#include "gb_boxnames.h"
#include "gb_flags.h"      /* BACKLOG #88: --op flagset/counter */
#include "gb_flags_rw.h"
#include "gb_hof.h"
#include "gb_dex.h"
/* BACKLOG #211: --op paste80, the Gen-3 -> Game Boy PASTE path (gen3_to_gb_fixed +
 * the per-slot move fill, BACKLOG #150 S150-10) for the retail-gate, mirroring
 * gb_paste_hook (source/pdna_gen12.c) end to end without the UI. */
#include "gen3_mon.h"
#include "gen3_to_gb.h"
#include "gb_moves_legal.h"

#define MAX_FILE_BYTES 65536u
#define MAX_OPS        64

static uint8_t g_img[MAX_FILE_BYTES];
static uint8_t g_scratch[GBS_SCRATCH_BYTES];
static uint8_t g_list[GBS_LIST_BYTES];
static uint8_t g_list2[GBS_LIST_BYTES];
/* --op create's own scratch: a host tool, so a static 2 KiB is not remotely tight
 * the way it would be on the GBA build this exercises (pdna_gen12.c's gb_create_
 * locate_rom reuses arena-resident storage for the identical reason). */
static uint8_t g_romscratch[ROM_GBSPRITE_SCRATCH_MIN];
static const char* g_rom_path;   /* set once in main() from --rom, read by do_create */
static bool g_list_moves;        /* BACKLOG #211: set once from --moves, read by list_box */

typedef struct {
  const char* kind;   /* "nick" / "ot" / "level" / "dv" / "delete" / "move" / "create" */
  const char* a[4];
  int n;
} Op;

static void usage(const char* prog) {
  fprintf(stderr,
    "usage: %s --in SAVE --out EDITED [--rom ROM] [--op ...]...\n"
    "       %s --in SAVE --list [--moves]\n"
    "  --op nick BOX SLOT TEXT\n"
    "  --op ot BOX SLOT TEXT\n"
    "  --op level BOX SLOT N\n"
    "  --op dv BOX SLOT STAT V     STAT in atk|def|spe|spc, V 0..15\n"
    "  --op delete BOX SLOT\n"
    "  --op move FROM_BOX SLOT TO_BOX\n"
    "  --op money VALUE            0..999999, gbs_write_field + gbs_finish\n"
    "  --op create BOX DEX  needs --rom; box only, not the party; level is the\n"
    "                       species' own lowest legal one (rom_gblearn_min_level)\n"
    "  --op badges MASK            0..255, via gb_trainer.h (Gen 1: BADGES; Gen 2: both)\n"
    "  --op badges2 JOHTO KANTO    Gen 2 only: set the two badge bytes independently\n"
    "  --op name TEXT              via gb_trainer.h; refused over 7 glyphs / bad charset\n"
    "  --op item POCKET ID QTY     BACKLOG #49 P2a, via gb_bag.h: insert-or-set. POCKET\n"
    "                               is items|key|balls|pc|tmhm. For tmhm, ID is the\n"
    "                               0-based TM/HM index and QTY is its count (0..99);\n"
    "                               every other pocket: if ID already occupies a slot\n"
    "                               its quantity is SET (not merged) to QTY, else a new\n"
    "                               entry is inserted with that QTY (both ID and QTY\n"
    "                               0..255 on the command line; gb_bag.h's own caps\n"
    "                               apply -- e.g. qty is refused outside 1..99).\n"
    "  --op daycare SLOT DEX       BACKLOG #85, via gb_daycare.h: deposit a fixed-stat\n"
    "                               test mon (level 5, nick TESTMON, OT TESTER; Gen 1\n"
    "                               uses a fixed placeholder GbGen1Base -- no --rom\n"
    "                               needed) into day-care slot 0 (Gen 1's only slot,\n"
    "                               or Gen 2's Day-Care Man) or 1 (Gen 2's Day-Care\n"
    "                               Lady, refused on Gen 1). Refused if occupied.\n"
    "  --op clockshift DAYS HOURS MINUTES SECONDS\n"
    "                               BACKLOG #86, P1a review D1, via gb_clock.h's\n"
    "                               gbc_shift: adds a SIGNED delta to the RTC-offset\n"
    "                               fields (the in-game clock is hardware RTC + this\n"
    "                               offset, never an absolute time -- see gb_clock.h).\n"
    "                               Gen 1 refused (no clock).\n"
    "  --op clockreset              BACKLOG #86, via gb_clock.h's gbc_request_time_reset:\n"
    "                               sets sRTCStatusFlags = RTC_RESET so the next\n"
    "                               CONTINUE runs the game's own clock-set prompt.\n"
    "                               Gen 1 refused (no clock).\n"
    "  --op clockclear               BACKLOG #86/#108, via gb_clock.h's\n"
    "                               gbc_clear_status_flags: zeroes sRTCStatusFlags,\n"
    "                               dismissing the clock-error banner only (a dead\n"
    "                               battery raises it again next boot). Gen 1\n"
    "                               refused (no clock).\n"
    "  --op fly INDEX              BACKLOG #90, via gb_fly.h: sets fly-destination\n"
    "                               bit INDEX visited. INDEX is 0..gbfy_count()-1 for\n"
    "                               this save's own generation.\n"
    "  --op boxname BOX TEXT       BACKLOG #94, via gb_boxnames.h: renames box BOX\n"
    "                               (0..13). Gen 1 refused (no box names).\n"
    "  --op helditem BOX SLOT ID   BACKLOG #95 review gate case: sets the held-item\n"
    "                               field on one mon (0..255). Gen 1 refused (rec+0x01\n"
    "                               is current HP there); a non-zero item on an Egg\n"
    "                               refused.\n"
    "  --op caught BOX SLOT T:L:LOC:G\n"
    "                               BACKLOG #95 review gate case: writes the capture\n"
    "                               record (time/level/loc/OT-gender, each 0..255,\n"
    "                               colon-separated) through the SAME Crystal-only gate\n"
    "                               the live editor uses (gb_session_is_crystal). Gen 1\n"
    "                               and Gold/Silver both refused.\n"
    "  --op dexset DEX STATE        BACKLOG #87 item 6 retail-gate case: sets National\n"
    "                               dex no. DEX's owned/seen state (0=none/1=seen/\n"
    "                               2=caught) via gb_dex.h's gbdex_set, same two-call\n"
    "                               shim shape pdna_gbdex.c uses. DEX is 1..gb_max_\n"
    "                               species(gen) (151 Gen 1, 251 Gen 2).\n"
    "  --op unownreset               BACKLOG #87 D4 retail-gate setup: clears wStatusFlags\n"
    "                               bit 1 (STATUSFLAGS_UNOWN_DEX_F), zeroes\n"
    "                               wFirstUnownSeen, and empties wUnownDex -- forcing the\n"
    "                               \"never met an Unown\" starting state a dexset 201 2\n"
    "                               case needs to prove the seed path. Gen 2 only,\n"
    "                               refused on Gen 1.\n"
    "  --op warp MAP X Y            m1 (BACKLOG #91) shot-retake gate: pokes the\n"
    "                               player's own current map/position (gb_fields.c's\n"
    "                               GBF_MAP_ID/GBF_POS_X/GBF_POS_Y + the two block-half\n"
    "                               bytes, X&1/Y&1) and refreshes the checksum. Gen 1\n"
    "                               only.\n"
    "  --op warp2 GROUP NUMBER X Y  M1-G2 fix-pass shot-retake gate: the Gen-2 twin of\n"
    "                               warp above (gb_fields.c's GBF_MAP_GROUP/NUMBER/\n"
    "                               POS_X/POS_Y, Gold/Silver vs Crystal columns picked\n"
    "                               via gb_session_is_crystal). Gen 2 only.\n"
    "  --op statusflags BYTE        BACKLOG #96 D10: GBF_STATUS_FLAGS (0..255), Gen 2\n"
    "                               only -- bit 0 is STATUSFLAGS_POKEDEX_F.\n"
    "  --op gender 0|1              BACKLOG #96 Kris: GBF_GENDER, Crystal only (0 M,\n"
    "                               1 F).\n"
    "  --op flagset INDEX 0|1       BACKLOG #88 gate case: gbfl_set over an ABSOLUTE\n"
    "                               event-flag bit index (0..2559 Gen 1 / 0..2047 Gen\n"
    "                               2) -- the same primitive pdna_gbflags.c's named\n"
    "                               toggles and raw browser both call.\n"
    "  --op counter FIELD VALUE     BACKLOG #88 gate case: FIELD is \"safari\" (Gen 1\n"
    "                               only, GBF_SAFARI_STEPS, 0..255) or \"lucky\" (Gen 2\n"
    "                               only, GBF_LUCKY_NUMBER_SHOW_FLAG, 0 or 1).\n"
    "  --op hofclear                BACKLOG #89: gbh_clear() -- erase every recorded\n"
    "                               Hall of Fame team + the lifetime win counter.\n"
    "  --op hofcount N               BACKLOG #89: gbh_set_count(N) -- the lifetime\n"
    "                               win counter (Gen 1 clamped to the teams present,\n"
    "                               <= 50; Gen 2 <= 200).\n"
    "  --op hofappend                BACKLOG #194 F3: gbh_append_team() -- one canned\n"
    "                               1-mon team (dex 1, level 5), the lifetime win\n"
    "                               counter's own +1 (saturating).\n"
    "  --op hofdelete                BACKLOG #194 F3: gbh_delete_team(0) -- delete the\n"
    "                               newest team, the lifetime win counter's own -1\n"
    "                               (floored at 0).\n"
    "  --op hofnick TEAM_IDX MON_IDX TEXT\n"
    "                               BACKLOG #198 item 7: gbh_team()+gbh_set_mon() --\n"
    "                               set one Hall of Fame mon's nickname (both gens).\n"
    "                               TEAM_IDX/MON_IDX use gbh_team()'s own newest-first\n"
    "                               UI numbering (0 = newest team; 0..5 within it).\n"
    "  --op hofdv TEAM_IDX MON_IDX STAT V\n"
    "                               BACKLOG #198 item 7: same shape as hofnick above,\n"
    "                               over one DV stat (atk|def|spe|spc, 0..15). Gen 2\n"
    "                               only -- GbHofMon.dv is always 0 on Gen 1.\n"
    "  --op paste80 BOX REC80FILE  BACKLOG #211: gen3_to_gb_fixed() + the per-slot\n"
    "                               move fill (BACKLOG #150 S150-10), mirroring\n"
    "                               gb_paste_hook -- box only. REC80FILE is an exact\n"
    "                               80-byte raw Gen-3 box record (tools/\n"
    "                               extract_gen3_record.c --moves). --rom is OPTIONAL\n"
    "                               here (a Gen-2 target needs no base-stats table);\n"
    "                               Gen 1 without --rom is refused\n"
    "                               (G3GB_ERR_NEEDS_BASE, same as production).\n"
"BOX is 0..n-1 or the literal \"party\".\n", prog, prog);
}

/* Parse argv into (in, out, list_mode, ops[]). Returns 2 on any usage problem (already
 * reported to stderr), else 0. Never touches a file. */
static int parse_args(int argc, char** argv, const char** in, const char** out,
                      bool* list_mode, Op ops[MAX_OPS], int* nops) {
  static const struct { const char* kind; int n; } shape[] = {
    {"nick", 3}, {"ot", 3}, {"level", 3}, {"dv", 4}, {"delete", 2}, {"move", 3},
    {"money", 1},
    {"create", 2},
    {"badges", 1}, {"name", 1}, {"badges2", 2},
    {"item", 3},
    {"daycare", 2}, {"clockshift", 4}, {"clockreset", 0}, {"fly", 1}, {"boxname", 2},
    /* -- append new ops HERE, last, one per line, with a marker comment (u5/gbdata
     * lanes append here too -- keeping new entries at the tail keeps concurrent
     * additions from the other lanes a clean append-only diff instead of a conflict). */
    {"helditem", 3},   /* BACKLOG #95 review gate case: BOX SLOT ID */
    {"caught", 3},     /* BACKLOG #95 review gate case: BOX SLOT time:level:loc:gender */
    {"clockclear", 0}, /* BACKLOG #86/#108: dismiss the clock-error banner */
    {"warp", 3},       /* m1 (BACKLOG #91) shot-retake gate: MAP X Y, Gen 1 only */
    {"statusflags", 1},/* BACKLOG #96 D10: GBF_STATUS_FLAGS byte, Gen 2 only */
    {"gender", 1},     /* BACKLOG #96 Kris: GBF_GENDER 0|1, Crystal only */
    {"flagset", 2},    /* BACKLOG #88: INDEX 0|1, via gbfl_set (gb_flags_rw.h) */
    {"counter", 2},    /* BACKLOG #88: FIELD(safari|lucky) VALUE */
    {"hofclear", 0},   /* BACKLOG #89: gbh_clear() -- erase every Hall of Fame team + count */
    {"hofcount", 1},   /* BACKLOG #89: gbh_set_count() N -- the lifetime win counter */
    {"hofappend", 0},  /* BACKLOG #194 F3: gbh_append_team() -- one canned 1-mon team */
    {"hofdelete", 0},  /* BACKLOG #194 F3: gbh_delete_team(0) -- delete the newest team */
    {"dexset", 2},     /* BACKLOG #87 item 6 retail-gate case: DEX STATE (0/1/2) */
    {"unownreset", 0}, /* BACKLOG #87 D4 retail-gate setup: force the Unown-dex gate clear */
    {"warp2", 4},      /* M1-G2 fix-pass shot-retake gate: GROUP NUMBER X Y, Gen 2 only */
    {"hofnick", 3},    /* BACKLOG #198 item 7: TEAM_IDX MON_IDX TEXT, via gbh_set_mon */
    {"hofdv", 4},      /* BACKLOG #198 item 7: TEAM_IDX MON_IDX STAT V, Gen 2 only */
    {"paste80", 2},    /* BACKLOG #211: BOX REC80FILE, via gen3_to_gb_fixed + the fill */
    {"mapquery", 1},   /* M3 (BACKLOG #91) retail-gate case: MAP, read-only, needs --rom */
    {"warpvp", 4},     /* map-gen1 review D4 retail-gate case: MAP WIDTH BX BY, Gen 1 only */
  };
  *in = NULL; *out = NULL; *list_mode = false; *nops = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--in") && i + 1 < argc) { *in = argv[++i]; continue; }
    if (!strcmp(argv[i], "--out") && i + 1 < argc) { *out = argv[++i]; continue; }
    if (!strcmp(argv[i], "--rom") && i + 1 < argc) { g_rom_path = argv[++i]; continue; }
    if (!strcmp(argv[i], "--list")) { *list_mode = true; continue; }
    if (!strcmp(argv[i], "--moves")) { g_list_moves = true; continue; }
    if (!strcmp(argv[i], "--op") && i + 1 < argc) {
      const char* kind = argv[++i];
      int need = -1;
      for (size_t k = 0; k < sizeof shape / sizeof shape[0]; k++)
        if (!strcmp(shape[k].kind, kind)) { need = shape[k].n; break; }
      if (need < 0) { fprintf(stderr, "unknown --op %s\n", kind); return 2; }
      if (*nops >= MAX_OPS) { fprintf(stderr, "too many --op\n"); return 2; }
      if (i + need >= argc) {
        fprintf(stderr, "--op %s needs %d argument(s)\n", kind, need); return 2;
      }
      Op* o = &ops[(*nops)++];
      o->kind = kind; o->n = need;
      for (int j = 0; j < need; j++) o->a[j] = argv[++i];
      continue;
    }
    fprintf(stderr, "unrecognized argument: %s\n", argv[i]);
    return 2;
  }
  if (!*in) { fprintf(stderr, "--in is required\n"); return 2; }
  if (!*list_mode && !*out) { fprintf(stderr, "--out is required\n"); return 2; }
  return 0;
}

static int read_whole(const char* path, uint32_t* out_len) {
  FILE* f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 2; }
  long sz = ftell(f);
  if (sz < 0 || (uint32_t)sz > MAX_FILE_BYTES) {
    fprintf(stderr, "%s is %ld bytes; this tool holds at most %u\n",
            path, sz, MAX_FILE_BYTES);
    fclose(f); return 2;
  }
  rewind(f);
  size_t n = fread(g_img, 1, (size_t)sz, f);
  fclose(f);
  if (n != (size_t)sz) { fprintf(stderr, "short read on %s\n", path); return 2; }
  *out_len = (uint32_t)sz;
  return 0;
}

static int write_whole(const char* path, uint32_t len) {
  FILE* f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "cannot create %s\n", path); return 2; }
  size_t n = fwrite(g_img, 1, len, f);
  fclose(f);
  if (n != len) { fprintf(stderr, "short write on %s\n", path); return 2; }
  return 0;
}

/* BOX token -> index, or -1 if the token is not "party" and not a plain non-negative
 * integer (a usage error, reported here). */
static int resolve_box(GbSession* s, const char* tok) {
  if (!strcmp(tok, "party")) return gbs_party_box(s);
  char* end = NULL;
  long v = strtol(tok, &end, 10);
  if (end == tok || *end != '\0' || v < 0) {
    fprintf(stderr, "bad box %s (want a number or \"party\")\n", tok);
    return -1;
  }
  return (int)v;
}

/* One non-negative-integer parser for every token that needs one, so the error message
 * always names the FIELD the caller was actually parsing ("bad level abc", not "bad slot
 * abc" for a level token that happens to fail the same strtol check a slot token would).
 */
static int resolve_uint(const char* tok, const char* what) {
  char* end = NULL;
  long v = strtol(tok, &end, 10);
  if (end == tok || *end != '\0' || v < 0) {
    fprintf(stderr, "bad %s %s\n", what, tok);
    return -1;
  }
  return (int)v;
}

/* Signed counterpart of resolve_uint, for clockshift's deltas (a shift can move the
 * clock backward). No range clamp here -- gbc_shift() itself does the wrap/carry math,
 * this just has to get a plain "-2"/"3" token into an int32_t. */
static int resolve_int(const char* tok, const char* what) {
  char* end = NULL;
  long v = strtol(tok, &end, 10);
  if (end == tok || *end != '\0') {
    fprintf(stderr, "bad %s %s\n", what, tok);
    return INT32_MIN;
  }
  return (int)v;
}

static int resolve_slot(const char* tok) { return resolve_uint(tok, "slot"); }

static int resolve_stat(const char* tok) {
  if (!strcmp(tok, "atk")) return GB_ATK;
  if (!strcmp(tok, "def")) return GB_DEF;
  if (!strcmp(tok, "spe")) return GB_SPE;
  if (!strcmp(tok, "spc")) return GB_SPC;
  fprintf(stderr, "bad stat %s (want atk|def|spe|spc)\n", tok);
  return -1;
}

/* Refuse-and-report: prints `why`, returns 1 (the caller's exit code). Never writes
 * --out — the caller only reaches write_whole() after every op returns 0. */
static int refuse(const char* why) {
  fprintf(stderr, "refused: %s\n", why);
  return 1;
}

/* nick / ot: load the slot, run the one text setter the field selects, verify, commit. */
static int do_text(GbSession* s, int field, int box, int slot, const char* text) {
  GbsStatus ls = gbs_load_list(s, box, g_list);
  if (ls != GBS_OK) return refuse(gbs_status_text(ls));
  GbEditMon e;
  if (!gb_load(&e, s->gen, g_list, box, slot))
    return refuse("gb_load: bad box/slot for this generation");
  char first_bad[GB_GLYPH_MAX];
  if (!gbe_set_text(&e, field, text, first_bad))
    return refuse(first_bad[0]
      ? "the generation's charset cannot spell that name exactly"
      : "the text setter refused");
  if (!gb_commit_checked(&e, g_list, box, slot))
    return refuse("gb_commit_checked: the write did not verify");
  GbsStatus cs = gbs_commit_list(s, box, g_list);
  if (cs != GBS_OK) return refuse(gbs_status_text(cs));
  return 0;
}

static int do_level(GbSession* s, int box, int slot, const char* ntok) {
  int lvl = resolve_uint(ntok, "level");
  if (lvl < 0) return 2;
  if (lvl < 1 || lvl > 100) { fprintf(stderr, "bad level %s (want 1..100)\n", ntok); return 2; }
  GbsStatus ls = gbs_load_list(s, box, g_list);
  if (ls != GBS_OK) return refuse(gbs_status_text(ls));
  GbEditMon e;
  if (!gb_load(&e, s->gen, g_list, box, slot))
    return refuse("gb_load: bad box/slot for this generation");
  if (!gb_set_level(&e, (uint8_t)lvl))
    return refuse("gb_set_level refused (glitch species has no growth rate)");
  if (e.is_party) (void)gbe_settle_stats(&e);   /* best-effort; Gen 1 cannot recompute */
  if (!gb_commit_checked(&e, g_list, box, slot))
    return refuse("gb_commit_checked: the write did not verify");
  GbsStatus cs = gbs_commit_list(s, box, g_list);
  if (cs != GBS_OK) return refuse(gbs_status_text(cs));
  return 0;
}

static int do_dv(GbSession* s, int box, int slot, const char* stok, const char* vtok) {
  int stat = resolve_stat(stok);
  if (stat < 0) return 2;
  int v = resolve_uint(vtok, "dv value");
  if (v < 0) return 2;
  if (v > 15) { fprintf(stderr, "bad dv value %s (want 0..15)\n", vtok); return 2; }
  GbsStatus ls = gbs_load_list(s, box, g_list);
  if (ls != GBS_OK) return refuse(gbs_status_text(ls));
  GbEditMon e;
  if (!gb_load(&e, s->gen, g_list, box, slot))
    return refuse("gb_load: bad box/slot for this generation");
  if (!gb_set_dv(&e, stat, (uint8_t)v)) return refuse("gb_set_dv refused");
  if (!gb_commit_checked(&e, g_list, box, slot))
    return refuse("gb_commit_checked: the write did not verify");
  GbsStatus cs = gbs_commit_list(s, box, g_list);
  if (cs != GBS_OK) return refuse(gbs_status_text(cs));
  return 0;
}

static int do_delete(GbSession* s, int box, int slot) {
  GbsStatus st = gbs_delete(s, box, slot, g_list);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  return 0;
}

static int do_move(GbSession* s, int from_box, int slot, int to_box) {
  int to_slot = -1;
  GbsStatus st = gbs_move(s, from_box, slot, to_box, &to_slot, g_list, g_list2);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  printf("moved to slot %d\n", to_slot);
  return 0;
}

/* BACKLOG #49 P0 — a field write, not a Pokemon op: exercises gbs_write_field +
 * gbs_finish end to end (tools/gb_retail_gate.py's "money" case is what proves this
 * against the real ROM). Money offsets and encodings, docs/GEN12-PARITY-DESIGN.md §1.1:
 * Red/Blue/Yellow 0x25F3 3B BE BCD; Gold/Silver 0x23DB, Crystal 0x23DC, both 3B BE binary. */
static void encode_bcd24(uint32_t v, uint8_t out[3]) {
  out[0] = (uint8_t)((((v / 100000u) % 10u) << 4) | ((v / 10000u) % 10u));
  out[1] = (uint8_t)((((v /   1000u) % 10u) << 4) | ((v /   100u) % 10u));
  out[2] = (uint8_t)((((v /     10u) % 10u) << 4) | ( v            % 10u));
}
static void encode_be24(uint32_t v, uint8_t out[3]) {
  out[0] = (uint8_t)(v >> 16); out[1] = (uint8_t)(v >> 8); out[2] = (uint8_t)v;
}

/* m1 (BACKLOG #91) shot-retake gate: reposition the player's own current map/
 * position for the Map screen's shot harness -- a warp, not a Pokemon op, same
 * "raw field write via gbs_write_field + one gbs_finish" shape do_money() above
 * uses. Offsets are gb_fields.c's own GBF_MAP_ID/GBF_POS_X/GBF_POS_Y/
 * GBF_POS_XBLOCK/GBF_POS_YBLOCK table (0x260a/0x260e/0x260d/0x2610/0x260f --
 * Red and Yellow share this block, per that table's own first two columns),
 * not re-derived here: the BLOCK-half bytes are simply (x & 1)/(y & 1), the
 * exact relation rom_gbmap.h's own gbmap_block_of() doc comment cites
 * (engine/overworld/tilesets.asm:49). Gen 1 only -- Gen 2's own current-map
 * fields live at different offsets this tool has no need for yet. */
static int do_warp(GbSession* s, const char* map_tok, const char* x_tok, const char* y_tok) {
  if (s->gen != GB_GEN1) return refuse("warp is Gen 1 only");
  int map = resolve_uint(map_tok, "warp map");
  int x = resolve_uint(x_tok, "warp x");
  int y = resolve_uint(y_tok, "warp y");
  if (map < 0 || x < 0 || y < 0) return 2;
  if (map > 255 || x > 255 || y > 255) { fprintf(stderr, "warp MAP/X/Y must each be 0..255\n"); return 2; }

  uint8_t map_b = (uint8_t)map, x_b = (uint8_t)x, y_b = (uint8_t)y;
  uint8_t xblock_b = (uint8_t)(x_b & 1u), yblock_b = (uint8_t)(y_b & 1u);

  GbsStatus st;
  if ((st = gbs_write_field(s, 0x260au, &map_b, 1)) != GBS_OK) return refuse(gbs_status_text(st));
  if ((st = gbs_write_field(s, 0x260eu, &x_b, 1)) != GBS_OK) return refuse(gbs_status_text(st));
  if ((st = gbs_write_field(s, 0x260du, &y_b, 1)) != GBS_OK) return refuse(gbs_status_text(st));
  if ((st = gbs_write_field(s, 0x2610u, &xblock_b, 1)) != GBS_OK) return refuse(gbs_status_text(st));
  if ((st = gbs_write_field(s, 0x260fu, &yblock_b, 1)) != GBS_OK) return refuse(gbs_status_text(st));
  if ((st = gbs_finish(s)) != GBS_OK) return refuse(gbs_status_text(st));
  return 0;
}

/* map-gen1 review D4 retail-gate case: the SAME write shape source/pdna_gbmap.c's
 * gbmap_write_pos() uses -- derive+validate the view pointer via gb1warp_viewptr()
 * BEFORE the first byte lands, then write ALL FIVE fields it writes (viewptr, Y, X,
 * YBLOCK=0, XBLOCK=0). Unlike do_warp() above (a general shot-retake helper that
 * also repositions map_id and accepts raw, possibly-odd coordinate bytes), this
 * always writes a BLOCK-ALIGNED destination and NEVER touches map_id -- exactly
 * what the shipped M3 screen does (gb1_warp.h's own scope note: teleport stays
 * within the current map). MAP is a read-only sanity check against the save's own
 * current map, never written -- a mismatch here means the corpus save moved out
 * from under the case, not something this op should silently paper over. */
static int do_warp_vp(GbSession* s, const char* map_tok, const char* width_tok,
                       const char* bx_tok, const char* by_tok) {
  if (s->gen != GB_GEN1) return refuse("warpvp is Gen 1 only");
  int map = resolve_uint(map_tok, "warpvp map");
  int width = resolve_uint(width_tok, "warpvp width");
  int bx = resolve_uint(bx_tok, "warpvp bx");
  int by = resolve_uint(by_tok, "warpvp by");
  if (map < 0 || width < 0 || bx < 0 || by < 0) return 2;
  if (map > 255 || width > 255 || bx > 127 || by > 127) {
    fprintf(stderr, "warpvp MAP must be 0..255, WIDTH 0..255, BX/BY 0..127\n");
    return 2;
  }

  uint8_t cur_map = 0;
  if (gbs_read_field(s, 0x260au, &cur_map, 1) != GBS_OK) return refuse("could not read current map");
  if (cur_map != (uint8_t)map) return refuse("warpvp MAP does not match the save's own current map");

  uint16_t vp;
  if (!gb1warp_viewptr((uint16_t)width, (int16_t)bx, (int16_t)by, &vp))
    return refuse("gb1warp_viewptr refused this destination");
  uint8_t vpbuf[2] = { (uint8_t)(vp & 0xFF), (uint8_t)(vp >> 8) };
  uint8_t xcoord = gb1warp_coord((int16_t)bx), ycoord = gb1warp_coord((int16_t)by), zero = 0;

  GbsStatus st;
  if ((st = gbs_write_field(s, 0x260bu, vpbuf, 2)) != GBS_OK) return refuse(gbs_status_text(st));
  if ((st = gbs_write_field(s, 0x260du, &ycoord, 1)) != GBS_OK) return refuse(gbs_status_text(st));
  if ((st = gbs_write_field(s, 0x260eu, &xcoord, 1)) != GBS_OK) return refuse(gbs_status_text(st));
  if ((st = gbs_write_field(s, 0x260fu, &zero, 1)) != GBS_OK) return refuse(gbs_status_text(st));
  if ((st = gbs_write_field(s, 0x2610u, &zero, 1)) != GBS_OK) return refuse(gbs_status_text(st));
  if ((st = gbs_finish(s)) != GBS_OK) return refuse(gbs_status_text(st));
  return 0;
}

/* M1-G2 (BACKLOG #91) fix-pass shot-retake gate: the Gen-2 twin of do_warp()
 * above, for the review-opus D1/D2/D3 re-shots (New Bark's own roof colour,
 * Olivine's, and Ice Path B2F Mahogany Side's -- the STOP-LICENCE remap shot
 * the brief required). Offsets are gb_fields.c's own GBF_MAP_GROUP/
 * GBF_MAP_NUMBER/GBF_POS_X/GBF_POS_Y table, Gold/Silver vs Crystal columns
 * (0x2868/0x2869/0x286b/0x286a vs 0x2843/0x2844/0x2846/0x2845 -- the exact
 * literals docs/GB-MAP-DESIGN-G2.md §8 and tests/host_romgbmap2_test.c's own
 * test_live_saves() already cite), not re-derived here, same "hardcode the
 * cited literal, cite gb_fields.c" precedent do_warp() sets for Gen 1. Gen 2
 * only -- Gen 1 already has its own do_warp(). */
static int do_warp2(GbSession* s, const char* group_tok, const char* number_tok,
                     const char* x_tok, const char* y_tok) {
  if (s->gen != GB_GEN2) return refuse("warp2 is Gen 2 only");
  int group = resolve_uint(group_tok, "warp2 group");
  int number = resolve_uint(number_tok, "warp2 number");
  int x = resolve_uint(x_tok, "warp2 x");
  int y = resolve_uint(y_tok, "warp2 y");
  if (group < 0 || number < 0 || x < 0 || y < 0) return 2;
  if (group > 255 || number > 255 || x > 255 || y > 255) {
    fprintf(stderr, "warp2 GROUP/NUMBER/X/Y must each be 0..255\n");
    return 2;
  }

  bool crystal = (s->g2w.sv.version == G2_VER_CRYSTAL);
  uint32_t group_off  = crystal ? 0x2843u : 0x2868u;
  uint32_t number_off = crystal ? 0x2844u : 0x2869u;
  uint32_t y_off       = crystal ? 0x2845u : 0x286au;
  uint32_t x_off       = crystal ? 0x2846u : 0x286bu;

  uint8_t group_b = (uint8_t)group, number_b = (uint8_t)number;
  uint8_t x_b = (uint8_t)x, y_b = (uint8_t)y;

  GbsStatus st;
  if ((st = gbs_write_field(s, group_off, &group_b, 1)) != GBS_OK) return refuse(gbs_status_text(st));
  if ((st = gbs_write_field(s, number_off, &number_b, 1)) != GBS_OK) return refuse(gbs_status_text(st));
  if ((st = gbs_write_field(s, x_off, &x_b, 1)) != GBS_OK) return refuse(gbs_status_text(st));
  if ((st = gbs_write_field(s, y_off, &y_b, 1)) != GBS_OK) return refuse(gbs_status_text(st));
  if ((st = gbs_finish(s)) != GBS_OK) return refuse(gbs_status_text(st));
  return 0;
}

/* BACKLOG #96 D10/Kris — two more raw field writes, same "gbs_write_field + one
 * gbs_finish" shape do_money()/do_warp() use.
 *
 * do_statusflags: GBF_STATUS_FLAGS (gb_fields.c, docs/GEN12-PARITY-DESIGN.md §1.1
 * row "status flags") -- Gold/Silver 0x23D9, Crystal 0x23DA; Gen 1 refused (no
 * equivalent field this backlog item's card gate reads). Bit 0 is
 * STATUSFLAGS_POKEDEX_F (constants/ram_constants.asm) -- the D10 gate case clears
 * it to prove the Gen-2 card's own POKeDEX row disappears.
 *
 * do_gender: GBF_GENDER (gb_trainer.h has_gender's own field, gb_fields.c's
 * S("sCrystalData", 0x3E3D)) -- Crystal ONLY (Gold/Silver's card has no gender
 * concept at all, per docs/GEN12-PARITY-DESIGN.md §1.1 row "gender": "(always
 * male)"); Gen 1 refused too. Outside every checksummed span (same row, "in
 * sCrystalData, outside the checksummed span") -- gbs_write_field()'s own checksum
 * refresh is therefore a no-op for this byte, but still runs (gbs_finish is not
 * optional) so a caller cannot forget it on a FUTURE field this tool reuses this
 * function's shape for. */
static int do_statusflags(GbSession* s, const char* vtok) {
  if (s->gen == GB_GEN1) return refuse("statusflags is Gen 2 only");
  int v = resolve_uint(vtok, "statusflags");
  if (v < 0) return 2;
  if (v > 255) { fprintf(stderr, "bad statusflags %s (want 0..255)\n", vtok); return 2; }

  uint32_t off = (s->g2w.sv.version == G2_VER_CRYSTAL) ? 0x23DAu : 0x23D9u;
  uint8_t b = (uint8_t)v;
  GbsStatus ws = gbs_write_field(s, off, &b, 1);
  if (ws != GBS_OK) return refuse(gbs_status_text(ws));
  GbsStatus fs = gbs_finish(s);
  if (fs != GBS_OK) return refuse(gbs_status_text(fs));
  return 0;
}

static int do_gender(GbSession* s, const char* vtok) {
  if (s->gen != GB_GEN2 || s->g2w.sv.version != G2_VER_CRYSTAL)
    return refuse("gender is Crystal only");
  int v = resolve_uint(vtok, "gender");
  if (v != 0 && v != 1) { fprintf(stderr, "bad gender %s (want 0 or 1)\n", vtok); return 2; }

  uint8_t b = (uint8_t)v;
  GbsStatus ws = gbs_write_field(s, 0x3E3Du, &b, 1);
  if (ws != GBS_OK) return refuse(gbs_status_text(ws));
  GbsStatus fs = gbs_finish(s);
  if (fs != GBS_OK) return refuse(gbs_status_text(fs));
  return 0;
}

/* BACKLOG #89 retail gate: gbh_clear() through source/gb_hof.h -- the case that
 * proves gen1_write_outside_sum's allowlist reaches a REAL booted Red/Gold/Crystal,
 * not just the host test's in-memory image. On a booted Red/Gold, the gate reads
 * wNumHoFTeams/wHallOfFameCount back out of WRAM (the game's own load already copied
 * the SRAM byte there) and the SRAM record itself, proving both "the count reads 0"
 * and "the PC's HALL OF FAME option is gone" (bills_pc.asm/main_menu.asm gate purely
 * on the count, per gb_hof.h's own header) without needing a screenshot of the PC
 * menu specifically. */
static int do_hofclear(GbSession* s) {
  GbsStatus st = gbh_clear(s);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  return 0;
}

/* BACKLOG #89 retail gate: gbh_set_count() -- the SET COUNT screen action, gated the
 * same clamp gbh_set_count() itself enforces (Gen 1 clamped to the teams present,
 * <= 50; Gen 2 <= 200); this tool does not re-clamp the token itself so an
 * over-range value still exercises the core's own clamp end to end, rather than
 * being rejected here before it ever reaches gbh_set_count(). */
static int do_hofcount(GbSession* s, const char* n_tok) {
  int n = resolve_uint(n_tok, "hof count");
  if (n < 0) return 2;
  GbsStatus st = gbh_set_count(s, n);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  /* BACKLOG #89 D1: the requested N and what actually lands can now differ on
   * Gen 1 (clamped to the teams present, not a flat 255) -- print the real
   * post-clamp count so tools/gb_retail_gate.py's own hofcount case can check
   * against what this call ACTUALLY wrote, not the token it was handed. */
  printf("hofcount result: %d\n", gbh_count(s));
  return 0;
}

/* BACKLOG #194 F3 retail gate: gbh_append_team() -- proves the append (and, on Gen 1
 * once the table is full, its eviction shift) reaches a REAL booted Red/Gold/Crystal,
 * not just the host test's in-memory image. One canned 1-mon team (dex 1, level 5,
 * nickname "TESTMON") -- species/level are validated by gbh_append_team() itself
 * (gb_hof.h's own contract), so any bad token here would refuse before ever reaching
 * the card, same posture as every other surgery op. */
static int do_hofappend(GbSession* s) {
  GbHofTeam t; memset(&t, 0, sizeof t);
  t.n = 1;
  t.mon[0].present = true;
  t.mon[0].dex = 1;
  t.mon[0].level = 5;
  snprintf(t.mon[0].nick, sizeof t.mon[0].nick, "TESTMON");
  GbsStatus st = gbh_append_team(s, &t);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  printf("hofappend result: %d\n", gbh_count(s));
  return 0;
}

/* BACKLOG #194 F3 retail gate: gbh_delete_team(0) -- deletes the newest team (UI
 * index 0 on both gens); proves the delete (the shift-and-decrement inverse of
 * append) reaches a real booted game the same way. */
static int do_hofdelete(GbSession* s) {
  GbsStatus st = gbh_delete_team(s, 0);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  printf("hofdelete result: %d\n", gbh_count(s));
  return 0;
}

/* BACKLOG #198 item 7: --op hofnick TEAM_IDX MON_IDX TEXT -- gbh_team() (read) ->
 * gbh_set_mon() (write), the SAME two-call shape the live EDIT MON screen uses
 * (hof_edit_mon, source/pdna_gbhof.c) to stage a nickname edit. Replaces
 * --b89-hof-extra's own undocumented byte-poked .sav fixtures (D6/NICK,
 * BACKLOG #89) with a reproducible, scriptable recipe: any Hall of Fame mon's
 * nickname can now be set through the same core the game itself edits through,
 * not a hand-found file offset. TEAM_IDX/MON_IDX use gbh_team()'s own UI
 * numbering (0 = newest team; 0..GBH_NUM_MONS-1 within it). */
static int do_hofnick(GbSession* s, const char* team_tok, const char* mon_tok, const char* text) {
  int team_idx = resolve_uint(team_tok, "hofnick team index");
  int mon_idx = resolve_uint(mon_tok, "hofnick mon index");
  if (team_idx < 0 || mon_idx < 0) return 2;
  if (mon_idx >= GBH_NUM_MONS) {
    fprintf(stderr, "bad hofnick mon index %s (want 0..%d)\n", mon_tok, GBH_NUM_MONS - 1);
    return 2;
  }
  GbHofTeam t;
  if (!gbh_team(s, team_idx, &t)) return refuse("gbh_team: bad team index or malformed session");
  if (mon_idx >= t.n) {
    fprintf(stderr, "hofnick mon index %d is past this team's own %d mon(s)\n", mon_idx, t.n);
    return 2;
  }
  if (strlen(text) >= sizeof t.mon[mon_idx].nick) {
    fprintf(stderr, "hofnick text too long (max %d chars)\n", (int)sizeof(t.mon[mon_idx].nick) - 1);
    return 2;
  }
  snprintf(t.mon[mon_idx].nick, sizeof t.mon[mon_idx].nick, "%s", text);
  GbsStatus st = gbh_set_mon(s, team_idx, mon_idx, &t.mon[mon_idx]);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  return 0;
}

/* BACKLOG #198 item 7: --op hofdv TEAM_IDX MON_IDX STAT V -- same gbh_team()/
 * gbh_set_mon() shape as do_hofnick() above, over GbHofMon.dv[4] instead of
 * .nick. Gen 2 only: gb_hof.h documents GbHofMon.dv as "always-0 Gen1" (Gen 1's
 * Hall of Fame record has no DV field at all), matching gbh_set_mon()'s own
 * Gen-1 write (species+level+nickname only, the 13-of-16-byte outside-sum
 * allowlist -- DVs are never part of that write on Gen 1). STAT reuses do_dv()'s
 * own resolve_stat() (atk|def|spe|spc, GB_ATK..GB_SPC) -- GbHofMon.dv's own
 * documented order is Atk/Def/Spd/Spc, i.e. index `stat - GB_ATK`, matching the
 * enum's own GB_ATK=1..GB_SPC=4 layout (gb_edit.h). Reproduces --b89-hof-extra's
 * own shiny-DV fixture (crystal-shiny, g2_dv_shiny's Atk&2/Def=Spe=Spc=10 quad)
 * as four ordinary --op hofdv calls instead of an undocumented byte poke. */
static int do_hofdv(GbSession* s, const char* team_tok, const char* mon_tok,
                     const char* stok, const char* vtok) {
  if (s->gen != GB_GEN2) return refuse("hofdv is Gen 2 only (GbHofMon.dv is always 0 on Gen 1)");
  int team_idx = resolve_uint(team_tok, "hofdv team index");
  int mon_idx = resolve_uint(mon_tok, "hofdv mon index");
  if (team_idx < 0 || mon_idx < 0) return 2;
  if (mon_idx >= GBH_NUM_MONS) {
    fprintf(stderr, "bad hofdv mon index %s (want 0..%d)\n", mon_tok, GBH_NUM_MONS - 1);
    return 2;
  }
  int stat = resolve_stat(stok);
  if (stat < 0) return 2;
  int v = resolve_uint(vtok, "hofdv value");
  if (v < 0) return 2;
  if (v > 15) { fprintf(stderr, "bad hofdv value %s (want 0..15)\n", vtok); return 2; }
  GbHofTeam t;
  if (!gbh_team(s, team_idx, &t)) return refuse("gbh_team: bad team index or malformed session");
  if (mon_idx >= t.n) {
    fprintf(stderr, "hofdv mon index %d is past this team's own %d mon(s)\n", mon_idx, t.n);
    return 2;
  }
  t.mon[mon_idx].dv[stat - GB_ATK] = (uint8_t)v;
  GbsStatus st = gbh_set_mon(s, team_idx, mon_idx, &t.mon[mon_idx]);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  return 0;
}

static int do_money(GbSession* s, const char* vtok) {
  int v = resolve_uint(vtok, "money");
  if (v < 0) return 2;
  if (v > 999999) { fprintf(stderr, "bad money %s (want 0..999999)\n", vtok); return 2; }

  uint32_t off;
  uint8_t enc[3];
  if (s->gen == GB_GEN1) {
    off = 0x25F3u;
    encode_bcd24((uint32_t)v, enc);
  } else {
    off = (s->g2w.sv.version == G2_VER_CRYSTAL) ? 0x23DCu : 0x23DBu;
    encode_be24((uint32_t)v, enc);
  }

  GbsStatus ws = gbs_write_field(s, off, enc, sizeof enc);
  if (ws != GBS_OK) return refuse(gbs_status_text(ws));
  GbsStatus fs = gbs_finish(s);
  if (fs != GBS_OK) return refuse(gbs_status_text(fs));
  return 0;
}

/* BACKLOG #87 item 6 retail-gate case: sets National dex no. `dex`'s owned/seen state
 * via gb_dex.h's gbdex_set -- same two-call shim shape pdna_gbdex.c's gbdex_shim_set
 * uses (state 0/1/2 = none/seen/caught), then ONE gbs_finish (gb_dex.h's own batching
 * contract: gbdex_set does not finish internally). */
static int do_dexset(GbSession* s, const char* dex_tok, const char* state_tok) {
  int dex = resolve_uint(dex_tok, "dexset dex");
  int state = resolve_uint(state_tok, "dexset state");
  if (dex < 0 || state < 0) return 2;
  int max = (int)gb_max_species(s->gen);
  if (dex < 1 || dex > max) {
    fprintf(stderr, "dexset DEX must be 1..%d for this save's generation\n", max);
    return 2;
  }
  if (state < 0 || state > 2) { fprintf(stderr, "dexset STATE must be 0, 1, or 2\n"); return 2; }

  GbsStatus st = gbdex_set(s, (uint16_t)dex, true, state >= 2);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  st = gbdex_set(s, (uint16_t)dex, false, state >= 1);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  st = gbs_finish(s);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  return 0;
}

/* D4 (BACKLOG #87 fix pass, DO-NOT-SHIP review) retail-gate setup case: force the
 * "never met an Unown" starting state directly -- wStatusFlags bit 1
 * (STATUSFLAGS_UNOWN_DEX_F) clear, wFirstUnownSeen 0, AND wUnownDex fully emptied.
 * The real game's UpdateUnownDex always sets all three atomically together, so
 * "wUnownDex already has a letter but wFirstUnownSeen reads 0" is a combination the
 * retail ROM never actually produces -- clearing the list too keeps this a state a
 * real cartridge could genuinely be in (a save that has simply never gone to the
 * Ruins of Alph), not a synthetic impossible one, before `dexset 201 2` proves the
 * seed path on real hardware/an emulated CPU. Gen 2 only; refuses on Gen 1 (none of
 * the three fields exist there). */
static int do_unownreset(GbSession* s) {
  GbGame g = (s->gen == GB_GEN1) ? GBF_G_RED
           : (s->g2w.sv.version == G2_VER_CRYSTAL) ? GBF_G_CRYSTAL : GBF_G_GS;
  uint32_t status_off = gbf_off(g, GBF_STATUS_FLAGS);
  uint32_t fus_off = gbf_off(g, GBF_FIRST_UNOWN_SEEN);
  uint32_t dex_off = gbf_off(g, GBF_UNOWN_DEX);
  uint16_t dex_len = gbf_len(g, GBF_UNOWN_DEX);
  if (!status_off || !fus_off || !dex_off) { fprintf(stderr, "unownreset: not available on this save's generation\n"); return 2; }
  uint8_t cur;
  if (gbs_read_field(s, status_off, &cur, 1) != GBS_OK) return refuse("unownreset: read wStatusFlags");
  uint8_t next = (uint8_t)(cur & (uint8_t)~(1u << 1));
  if (next != cur) {
    GbsStatus st = gbs_write_field(s, status_off, &next, 1);
    if (st != GBS_OK) return refuse(gbs_status_text(st));
  }
  uint8_t zero = 0;
  GbsStatus st = gbs_write_field(s, fus_off, &zero, 1);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  uint8_t zeros[32] = {0};
  if (dex_len > sizeof zeros) { fprintf(stderr, "unownreset: GBF_UNOWN_DEX unexpectedly wide\n"); return 2; }
  st = gbs_write_field(s, dex_off, zeros, dex_len);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  st = gbs_finish(s);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  return 0;
}

static bool tool_rom_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FILE* f = (FILE*)ctx;
  if (fseek(f, (long)off, SEEK_SET) != 0) return false;
  if (fread(dst, 1, len, f) != len) return false;
  return true;
}

/* M3 (BACKLOG #91) retail-gate case: read-only ROM query, no save bytes touched.
 * Prints the located header's own width/height (in BLOCKS, rom_gbmap.h's own
 * GbMap1Header) for `map` so run_teleport_case (tools/gb_retail_gate.py) can
 * pick a destination it has PROVEN is inside the map's own bounds instead of
 * guessing a coordinate and hoping -- the exact "never trust a shape it could
 * not confirm" posture this whole locator module is built around (rom_gbmap.h's
 * own top comment). Reuses g_rom_path/g_romscratch/tool_rom_read exactly as
 * do_create() below does for rom_gbbase_gen1/rom_gblearn_open. */
static int do_mapquery(const GbSession* s, const char* map_tok) {
  if (s->gen != GB_GEN1) return refuse("mapquery is Gen 1 only");
  int map = resolve_uint(map_tok, "mapquery map");
  if (map < 0 || map > 255) { fprintf(stderr, "mapquery MAP must be 0..255\n"); return 2; }
  if (!g_rom_path) { fprintf(stderr, "--op mapquery needs --rom PATH\n"); return 2; }

  FILE* rf = fopen(g_rom_path, "rb");
  if (!rf) return refuse("cannot open --rom file");
  if (fseek(rf, 0, SEEK_END) != 0) { fclose(rf); return refuse("cannot seek --rom file"); }
  long rsz = ftell(rf);
  if (rsz <= 0) { fclose(rf); return refuse("empty --rom file"); }
  rewind(rf);

  RomGbMap1 g;
  bool gok = rgm1_open(&g, tool_rom_read, rf, (uint32_t)rsz, g_romscratch, sizeof g_romscratch);
  if (!gok) { fclose(rf); return refuse("rgm1_open: no Gen-1 map tables located"); }
  GbMap1Header hdr;
  bool hok = rgm1_header(&g, (uint8_t)map, &hdr);
  fclose(rf);
  if (!hok) return refuse("rgm1_header: could not resolve that map id");

  printf("MAPQUERY %d WIDTH %u HEIGHT %u\n", map, (unsigned)hdr.width, (unsigned)hdr.height);
  return 0;
}

/* BACKLOG #50, retail-gate case: build a fresh mon off --rom (rom_gbbase_gen1/2 +
 * rom_gblearn, the SAME facts pdna_gen12.c's gb_create_hook assembles) and insert
 * it into `box` -- box only, gbs_insert() itself refuses the party pseudo-box, the
 * exact limitation gb_create_hook's own party guard exists for (see its comment,
 * source/pdna_gen12.c). OT name/id are the fixed test values "GATEX"/12345: this
 * tool has no reader for SAVE's own trainer block, and the retail-gate assertion
 * this exists for (does the mon SHOW UP, named and levelled right) does not need
 * a real one.
 *
 * NO `lvl` ARGUMENT any more (BACKLOG #50 UX-parity, Guy 2026-09-07): the level
 * picker this used to accept a caller-supplied level for is gone from
 * gb_create_hook itself, replaced by rom_gblearn_min_level() -- keeping a level
 * argument HERE after removing it there would let this tool "mirror
 * gb_create_hook's logic" (this comment's own claim) while silently drifting
 * from it the moment anyone ran it against a species whose floor isn't 5. This
 * now computes the SAME way production does. */
static int do_create(GbSession* s, const char* box_tok, const char* dex_tok) {
  int box = resolve_box(s, box_tok);
  if (box < 0) return 2;
  int dex = resolve_uint(dex_tok, "dex");
  if (dex < 1) return 2;
  if (!g_rom_path) { fprintf(stderr, "--op create needs --rom PATH\n"); return 2; }

  FILE* rf = fopen(g_rom_path, "rb");
  if (!rf) return refuse("cannot open --rom file");
  if (fseek(rf, 0, SEEK_END) != 0) { fclose(rf); return refuse("cannot seek --rom file"); }
  long rsz = ftell(rf);
  if (rsz <= 0) { fclose(rf); return refuse("empty --rom file"); }
  rewind(rf);

  RomGbSprite gs;
  int gsok = rom_gbsprite_open(&gs, tool_rom_read, rf, (uint32_t)rsz,
                               g_romscratch, sizeof g_romscratch, GB_ROM_NONE);
  GbRomGen want = (s->gen == GB_GEN1) ? GB_ROM_GEN1 : GB_ROM_GEN2;
  if (!gsok || gs.gen != want) { fclose(rf); return refuse("--rom did not open as SAVE's own generation"); }

  RomGbLearn rl;
  if (!rom_gblearn_open(&rl, s->gen, tool_rom_read, rf, (uint32_t)rsz)) {
    fclose(rf); return refuse("rom_gblearn_open: no learnset table located");
  }
  int lvl = (int)rom_gblearn_min_level(&rl, (uint16_t)dex);

  GbNewMonSrc src;
  memset(&src, 0, sizeof src);
  uint8_t g1_start[4]; const uint8_t* g1_start_p = NULL;
  if (s->gen == GB_GEN1) {
    RomGb1Species sp;
    if (!rom_gbbase_gen1(&gs, tool_rom_read, rf, (uint16_t)dex, &sp)) {
      fclose(rf); return refuse("rom_gbbase_gen1: no row for that dex");
    }
    memcpy(src.base, sp.base.base, GB_NSTATS);
    src.type1 = sp.base.type1; src.type2 = sp.base.type2; src.growth = sp.growth;
    memcpy(g1_start, sp.start, 4);
    g1_start_p = g1_start;
  } else {
    RomGb2Species sp;
    if (!rom_gbbase_gen2(&gs, tool_rom_read, rf, (uint16_t)dex, &sp)) {
      fclose(rf); return refuse("rom_gbbase_gen2: no row for that dex");
    }
    src.growth = sp.growth;
  }
  int kept = g1_start_p
    ? rom_gblearn_moves_at_seeded(&rl, (uint16_t)dex, (uint8_t)lvl, g1_start_p, src.moves)
    : rom_gblearn_moves_at(&rl, (uint16_t)dex, (uint8_t)lvl, src.moves);
  fclose(rf);
  if (kept < 0) return refuse("rom_gblearn_moves_at: bad dex/level for this table");

  src.species_name = pk_species_name((uint16_t)dex);
  src.ot_name = "GATEX";
  src.ot_id = 12345;

  GbEditMon party_mon;
  if (!gb_new_mon(s->gen, (uint16_t)dex, (uint8_t)lvl, &src, 0xC0FFEEu, &party_mon))
    return refuse("gb_new_mon refused (growth-rate cross-check, or a bad dex/level)");

  /* party -> box: the byte-identical-prefix technique gb_new_mon.h documents and
   * pdna_gen12.c's gb_create_hook uses -- gbs_insert() requires is_party == false. */
  GbEditMon box_mon;
  if (!gb_load_parts(&box_mon, s->gen, false, party_mon.rec, party_mon.otname,
                     party_mon.nick, party_mon.list_species))
    return refuse("gb_load_parts (party -> box) failed");

  int slot_out = 0;
  GbsStatus ist = gbs_insert(s, box, &box_mon, &slot_out, g_list);
  if (ist != GBS_OK) return refuse(gbs_status_text(ist));
  printf("created dex=%d lv=%d into box %d slot %d\n", dex, lvl, box, slot_out);
  return 0;
}

/* BACKLOG #211: --op paste80 BOX REC80FILE -- gen3_to_gb_fixed() + (when a ROM was
 * given) the SAME per-slot fill gb_paste_fill_moves()/g3gb_moves_fill() apply in
 * production (BACKLOG #150 S150-10), then gbs_insert() into `box`. Mirrors
 * gb_paste_hook's own hook order end to end, minus the UI:
 *   1. g3gb_moves_ok_rec(rec80, gen, bad4) -- the SAME per-slot predicate
 *      gb_clip_moves() wraps in production.
 *   2. gen3_to_gb_fixed(rec80, gen, crystal, g1base_or_NULL, bad4_or_NULL, &mon, &loss)
 *      -- refuses G3GB_ERR_NEEDS_BASE if gen is GB_GEN1 and no --rom was given (same
 *      as production's own retry gate; this tool never retries mid-op, --rom is
 *      resolved up front instead of gb_paste_hook's two-call dance, since a host CLI
 *      has no cost pressure to defer the ROM open).
 *   3. nbad > 0 -- fill from the ROM's learnset AT THE LEVEL THAT WILL BE WRITTEN
 *      (review D7: gen3_to_gb_evo_needs_fix(&mon, ...) ? fix_to : gb_get_level(&mon),
 *      mirroring gb_paste_hook exactly -- this tool does not itself SET the mon's
 *      level to fix_to, same as production before its own MAKE LEGAL choice) via
 *      g3gb_moves_fill(); no --rom means learn4 stays {0,0,0,0}, the documented
 *      "never block" input (decision 3) -- every bad slot is simply left empty.
 *   4. decision 8.7's ONE exception: the record would be WRITTEN with no moves left
 *      at all (nleft == 0, review D1) refuses outright (nothing written) -- the same
 *      zero-move guard gb_paste_hook applies.
 *   5. gbs_insert(box) -- gen3_to_gb_fixed's own gb_load_parts(..., false, ...) already
 *      built `mon` box-shaped (is_party = false), so no party->box copy is needed
 *      here, unlike do_create() above (which starts from a party record). */
static int do_paste80(GbSession* s, const char* box_tok, const char* rec_path) {
  int box = resolve_box(s, box_tok);
  if (box < 0) return 2;

  FILE* recf = fopen(rec_path, "rb");
  if (!recf) return refuse("cannot open the 80-byte record file");
  uint8_t rec80[80];
  size_t got = fread(rec80, 1, sizeof rec80, recf);
  fclose(recf);
  if (got != sizeof rec80) return refuse("record file is not exactly 80 bytes");

  uint8_t bad4[4] = { 0, 0, 0, 0 };
  int nbad = g3gb_moves_ok_rec(rec80, s->gen, bad4);
  if (nbad < 0) return refuse("g3gb_moves_ok_rec: decode failure or an Egg");

  GbGen1Base g1base_buf;
  const GbGen1Base* g1base = NULL;
  RomGbSprite gs;
  RomGbLearn rl;
  bool have_rom = false;
  FILE* rf = NULL;
  uint8_t g1_start_buf[4]; const uint8_t* g1_start = NULL;

  if (g_rom_path) {
    rf = fopen(g_rom_path, "rb");
    if (!rf) return refuse("cannot open --rom file");
    if (fseek(rf, 0, SEEK_END) != 0) { fclose(rf); return refuse("cannot seek --rom file"); }
    long rsz = ftell(rf);
    if (rsz <= 0) { fclose(rf); return refuse("empty --rom file"); }
    rewind(rf);

    int gsok = rom_gbsprite_open(&gs, tool_rom_read, rf, (uint32_t)rsz,
                                 g_romscratch, sizeof g_romscratch, GB_ROM_NONE);
    GbRomGen want = (s->gen == GB_GEN1) ? GB_ROM_GEN1 : GB_ROM_GEN2;
    if (!gsok || gs.gen != want) { fclose(rf); return refuse("--rom did not open as SAVE's own generation"); }

    if (s->gen == GB_GEN1) {
      PkMon m;
      if (!pk_decode_mon(rec80, false, &m) || m.isEgg || m.isBadEgg) {
        fclose(rf); return refuse("record decode failure or an Egg");
      }
      uint16_t dex = pk_national_no(m.species);
      RomGb1Species sp;
      if (!rom_gbbase_gen1(&gs, tool_rom_read, rf, dex, &sp)) {
        fclose(rf); return refuse("rom_gbbase_gen1: no row for that dex");
      }
      g1base_buf = sp.base;
      g1base = &g1base_buf;
      memcpy(g1_start_buf, sp.start, 4);
      g1_start = g1_start_buf;
    }

    if (!rom_gblearn_open(&rl, s->gen, tool_rom_read, rf, (uint32_t)rsz)) {
      fclose(rf); return refuse("rom_gblearn_open: no learnset table located");
    }
    have_rom = true;
  }

  GbEditMon mon;
  Gen3ToGbLoss loss;
  G3GbStatus cst = gen3_to_gb_fixed(rec80, s->gen, gb_session_is_crystal(s), g1base,
                                    nbad > 0 ? bad4 : NULL, &mon, &loss);
  if (cst != G3GB_OK) { if (rf) fclose(rf); return refuse(g3gb_status_text(cst)); }

  /* review D7: fetch at the level that WILL be written, mirroring gb_paste_hook
   * exactly (pdna_gen12.c ~4260) -- fix_to when an evolution correction applies,
   * else the mon's own current level. Computed once, used only by the learnset
   * lookup below; this tool never itself sets mon's level to fix_to (no MAKE LEGAL
   * choice exists here, same as production before that choice is made). */
  uint8_t fix_from = 0, fix_to = 0;
  bool fix = gen3_to_gb_evo_needs_fix(&mon, &fix_from, &fix_to);
  (void)fix_from;

  int nfill = 0;
  if (nbad > 0) {
    uint8_t learn4[4] = { 0, 0, 0, 0 };
    if (have_rom) {
      uint16_t dex = gb_get_species_dex(&mon);
      uint8_t lvl = fix ? fix_to : gb_get_level(&mon);
      int kept = g1_start
        ? rom_gblearn_moves_at_seeded(&rl, dex, lvl, g1_start, learn4)
        : rom_gblearn_moves_at(&rl, dex, lvl, learn4);
      if (kept < 0) memset(learn4, 0, sizeof learn4);   /* decision 3: no eligible fill -> empty */
    }
    uint8_t fill4[4] = { 0, 0, 0, 0 };
    nfill = g3gb_moves_fill(&mon, bad4, learn4, fill4);
    /* review D1: "the record would be WRITTEN with no moves at all" -- nbad == 4
     * misses the 1-3-bad case where every non-empty slot was out of range and the
     * fill ran dry (a mirror of pdna_gen12.c's gb_paste_hook / gb_bank_down_bridge
     * fix). */
    int nleft = 0;
    for (int i = 0; i < 4; i++) if (gb_get_move(&mon, i)) nleft++;
    if (nleft == 0) {
      if (rf) fclose(rf);
      return refuse("zero-move refusal (decision 8.7): the record would land with no moves at all");
    }
  }
  if (rf) fclose(rf);

  int slot_out = 0;
  GbsStatus ist = gbs_insert(s, box, &mon, &slot_out, g_list);
  if (ist != GBS_OK) return refuse(gbs_status_text(ist));
  printf("paste80: %d bad slot(s), %d filled, into box %d slot %d\n", nbad, nfill, box, slot_out);
  return 0;
}

/* BACKLOG #49 P1a — via gb_trainer.h, not a raw byte poke: read the whole trainer
 * record, change only the field this op names, write the whole record back. Every
 * other field lands back at its own current value, which is what
 * tests/host_gbtrainer_test.c's byte-diff round trip already proves is a no-op. */
static int do_badges(GbSession* s, const char* mtok) {
  char* end = NULL;
  long v = strtol(mtok, &end, 0);   /* base 0: accepts "0x.." or decimal */
  if (end == mtok || *end != '\0' || v < 0 || v > 255) {
    fprintf(stderr, "bad badges mask %s (want 0..255)\n", mtok);
    return 2;
  }
  GbTrainer t;
  if (!gbt_read(s, &t)) return refuse("gbt_read failed");
  if (s->gen == GB_GEN1) {
    t.badges = (uint8_t)v;
  } else {
    t.badges_johto = (uint8_t)v;
    t.badges_kanto = (uint8_t)v;
  }
  GbsStatus st = gbt_write(s, &t);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  return 0;
}

/* P1a review D8: Gen 2's Johto and Kanto badge bytes are independently addressable
 * (source/gb_fields.c GBF_BADGES_JOHTO/GBF_BADGES_KANTO) but --op badges above sets
 * both to the SAME mask, which makes a byte-order swap between them invisible to a
 * gate that only checks "the mask landed somewhere in this pair". This op sets them
 * to two DIFFERENT masks so tools/gb_retail_gate.py's trainer case can tell. Gen 1
 * has no such split (one BADGES byte covers all 8 gyms) -- refused there. */
static int do_badges2(GbSession* s, const char* jtok, const char* ktok) {
  if (s->gen == GB_GEN1) return refuse("badges2 is Gen 2 only (Gen 1 has one BADGES byte)");
  char* end = NULL;
  long jv = strtol(jtok, &end, 0);
  if (end == jtok || *end != '\0' || jv < 0 || jv > 255) {
    fprintf(stderr, "bad johto mask %s (want 0..255)\n", jtok);
    return 2;
  }
  end = NULL;
  long kv = strtol(ktok, &end, 0);
  if (end == ktok || *end != '\0' || kv < 0 || kv > 255) {
    fprintf(stderr, "bad kanto mask %s (want 0..255)\n", ktok);
    return 2;
  }
  GbTrainer t;
  if (!gbt_read(s, &t)) return refuse("gbt_read failed");
  t.badges_johto = (uint8_t)jv;
  t.badges_kanto = (uint8_t)kv;
  GbsStatus st = gbt_write(s, &t);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  return 0;
}

static int do_name(GbSession* s, const char* text) {
  GbTrainer t;
  if (!gbt_read(s, &t)) return refuse("gbt_read failed");
  if (strlen(text) >= sizeof t.name) return refuse("name longer than this tool's buffer");
  strcpy(t.name, text);
  GbsStatus st = gbt_write(s, &t);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  return 0;
}

/* BACKLOG #49 P2a — via gb_bag.h, not a raw byte poke: read the whole bag, insert-
 * or-set one entry (tmhm: set one TM/HM's count), write the whole bag back. Every
 * OTHER pocket lands back at its own current bytes, which is what
 * tests/host_gbbag_test.c's pocket-granularity byte-diff round trip already
 * proves is a no-op -- this op is what tools/gb_retail_gate.py's bag case exercises
 * against the real ROM. "insert-or-set": gb_bag.h's own gbb_insert() ADDS quantity
 * into an id already present (the real AddItem-style merge, gb_bag.h's documented
 * contract); this op instead SETS the slot's quantity outright when the id is
 * already there, so a gate case that asks for "Potion x7" always lands on exactly
 * 7 on a corpus save that might already be carrying some Potions, rather than a
 * merged total that depends on what the corpus happened to hold. */
static const char* bag_status_text(GbBagOpStatus st) {
  switch (st) {
    case GBB_OK:             return "ok";
    case GBB_ERR_ARG:        return "bad argument";
    case GBB_ERR_NOT_PRESENT: return "this game does not have that pocket";
    case GBB_ERR_FULL:       return "pocket is full";
    case GBB_ERR_BADID:      return "item id out of range";
    case GBB_ERR_QTY:        return "quantity out of range";
    default:                 return "unknown gb_bag status";
  }
}

static int resolve_pocket(const char* tok, GbBagPocket* out) {
  static const struct { const char* name; GbBagPocket p; } tbl[] = {
    { "items", GBB_POCKET_ITEMS }, { "key",   GBB_POCKET_KEY },
    { "balls", GBB_POCKET_BALLS }, { "tmhm",  GBB_POCKET_TMHM },
    { "pc",    GBB_POCKET_PC },
  };
  for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; i++)
    if (!strcmp(tbl[i].name, tok)) { *out = tbl[i].p; return 0; }
  fprintf(stderr, "bad pocket %s (want items|key|balls|pc|tmhm)\n", tok);
  return -1;
}

static int do_item(GbSession* s, const char* pocket_tok, const char* id_tok,
                   const char* qty_tok) {
  GbBagPocket pocket;
  if (resolve_pocket(pocket_tok, &pocket)) return 2;
  GbGame g = gbt_game(s);   /* identical Gen->GbGame mapping gb_bag.c uses internally */
  if (!gbb_field_present(g, pocket))
    return refuse("this game does not have that pocket");

  GbBag bag;
  if (!gbb_read(s, &bag)) return refuse("gbb_read failed");

  if (pocket == GBB_POCKET_TMHM) {
    int idx = resolve_uint(id_tok, "tmhm index");
    int cnt = resolve_uint(qty_tok, "tmhm count");
    if (idx < 0 || cnt < 0) return 2;
    if (idx > 255 || cnt > 255) { fprintf(stderr, "index/count must be 0..255\n"); return 2; }
    GbBagOpStatus ost = gbb_tmhm_set(g, &bag, idx, (uint8_t)cnt);
    if (ost != GBB_OK) return refuse(bag_status_text(ost));
  } else {
    int id = resolve_uint(id_tok, "item id");
    int qty = resolve_uint(qty_tok, "quantity");
    if (id < 0 || qty < 0) return 2;
    if (id > 255 || qty > 255) { fprintf(stderr, "id/qty must be 0..255\n"); return 2; }

    GbBagList* list = &bag.pockets[pocket];
    int found = -1;
    for (int i = 0; i < list->count; i++)
      if (list->entries[i].id == (uint8_t)id) { found = i; break; }

    GbBagOpStatus ost;
    if (found >= 0) {
      ost = gbb_set_qty(g, &bag, pocket, found, (uint8_t)qty);
    } else {
      ost = gbb_insert(g, &bag, pocket, (uint8_t)id, (uint8_t)qty);
    }
    if (ost != GBB_OK) return refuse(bag_status_text(ost));
  }

  GbsStatus st = gbb_write(s, &bag);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  return 0;
}

/* BACKLOG #85 -- via gb_daycare.h. A fixed-stat test mon (never --rom, unlike --op
 * create): Gen 2 needs no base-stat table at all (gb_set_species(NULL) works, same as
 * --op create's Gen-2 path); Gen 1 gets a FIXED placeholder GbGen1Base (Bulbasaur's
 * real Gen-1 base stats/types, docs/kb/pokemon -- HP45/Atk49/Def49/Spe45/Spc65,
 * GRASS/POISON) regardless of which dex is asked for, since the retail-gate case this
 * exists for only needs a record that decodes without corrupting the save, not an
 * accurate stat line for an arbitrary species. */
static int do_daycare(GbSession* s, const char* slot_tok, const char* dex_tok) {
  int slot = resolve_uint(slot_tok, "daycare slot");
  int dex = resolve_uint(dex_tok, "dex");
  if (slot < 0 || dex < 0) return 2;

  GbEditMon mon;
  memset(&mon, 0, sizeof mon);
  mon.gen = s->gen;
  mon.rec_len = (uint8_t)gb_rec_size(s->gen, false);
  mon.is_party = false;
  static const GbGen1Base g1_placeholder = {
    .base = { 45, 49, 49, 45, 65 }, .type1 = 0x16, .type2 = 0x03
  };
  if (!gb_set_species(&mon, (uint16_t)dex, s->gen == GB_GEN1 ? &g1_placeholder : NULL))
    return refuse("gb_set_species refused (bad dex for this generation?)");
  if (!gb_set_level(&mon, 5)) return refuse("gb_set_level refused");
  if (!gb_set_nickname(&mon, "TESTMON")) return refuse("gb_set_nickname refused");
  if (!gb_set_otname(&mon, "TESTER")) return refuse("gb_set_otname refused");
  /* P1a review D2: gbd_deposit now gates on gb_check(), which refuses a moveless record
   * -- give it move 1 (Pound), same as host_gbdaycare_test.c's own fixture. */
  if (!gb_set_move(&mon, 0, 1)) return refuse("gb_set_move refused");

  GbsStatus st = gbd_deposit(s, slot, &mon);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  printf("deposited dex=%d lv=5 into day-care slot %d\n", dex, slot);
  return 0;
}

/* BACKLOG #86, P1a review D1 -- via gb_clock.h. gb_clock no longer offers a "set an
 * absolute time" op (it cannot: the in-game clock is the hardware RTC plus a stored
 * OFFSET, see gb_clock.h's header note) -- --op clock is replaced by --op clockshift
 * (adds a signed delta to the offset, the same thing a player does by hand with no
 * password) and --op clockreset (asks the game to re-run its own clock-set prompt at
 * next CONTINUE, the same effect the password-protected reset has). Gen 1 refused
 * either way (gbc_shift/gbc_request_time_reset's own GBS_ERR_ARG, no clock at all). */
static int do_clockshift(GbSession* s, const char* days_tok, const char* hours_tok,
                         const char* min_tok, const char* sec_tok) {
  int days = resolve_int(days_tok, "clockshift days");
  int hours = resolve_int(hours_tok, "clockshift hours");
  int min = resolve_int(min_tok, "clockshift minutes");
  int sec = resolve_int(sec_tok, "clockshift seconds");
  if (days == INT32_MIN || hours == INT32_MIN || min == INT32_MIN || sec == INT32_MIN)
    return 2;
  GbsStatus st = gbc_shift(s, days, hours, min, sec);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  return 0;
}

static int do_clockreset(GbSession* s) {
  GbsStatus st = gbc_request_time_reset(s);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  return 0;
}

/* BACKLOG #86/#108 -- via gb_clock.h's gbc_clear_status_flags. Dismisses the
 * clock-error banner only (does not fix a dead battery, see gb_clock.h's own
 * header note); Gen 1 refused the same way clockshift/clockreset are. */
static int do_clockclear(GbSession* s) {
  GbsStatus st = gbc_clear_status_flags(s);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  return 0;
}

/* BACKLOG #90 -- via gb_fly.h. Sets one fly-destination bit visited. */
static int do_fly(GbSession* s, const char* idx_tok) {
  int idx = resolve_uint(idx_tok, "fly index");
  if (idx < 0) return 2;
  GbsStatus st = gbfy_set(s, idx, true);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  return 0;
}

/* BACKLOG #88 retail-gate case: gbfl_set (source/gb_flags_rw.h) over an ABSOLUTE
 * event-flag bit index -- the same primitive pdna_gbflags.c's own named-shortlist
 * toggle and raw browser both call. `idx_tok` is the bit number (not a shortlist
 * ROW -- there is no row-order dependency here, matching pk_flag equivalent gate
 * shapes elsewhere in this file), `val_tok` is "0" or "1". */
static int do_flagset(GbSession* s, const char* idx_tok, const char* val_tok) {
  int idx = resolve_uint(idx_tok, "flag index");
  if (idx < 0) return 2;
  int val = resolve_uint(val_tok, "flag value");
  if (val != 0 && val != 1) { fprintf(stderr, "bad flag value %s (want 0 or 1)\n", val_tok); return 2; }
  GbGame g = gbt_game(s);
  GbsStatus st = gbfl_set(s, g, (uint16_t)idx, val != 0);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  /* gbfl_set only calls gbs_write_field (gb_flags_rw.h's own contract: pdna_gbflags.c
   * batches many toggles behind ONE gbs_finish() on B) -- this CLI issues exactly one
   * op per invocation, so it must close the batch itself, same as do_money/do_counter
   * above. Missing this left Gen 2's stored checksums stale: the edited .sav failed to
   * even gbs_open() back, and the real cartridge's own CONTINUE screen reported "The
   * save file is / corrupted!" (caught live against Gold.sav, BACKLOG #88 gate case). */
  GbsStatus fs = gbs_finish(s);
  if (fs != GBS_OK) return refuse(gbs_status_text(fs));
  return 0;
}

/* BACKLOG #88 retail-gate case: the two counters pdna_gbflags.c's COUNTERS tab adds
 * that no EXISTING --op already covers (money/coins/badges/name all have their own
 * ops above) -- both are plain U8 fields through gb_fields.h's gbf_off/len, written
 * via gbs_write_field + gbs_finish, the exact primitive BACKLOG #49 P0 established.
 * FIELD is "safari" (GBF_SAFARI_STEPS, Gen 1 only, 0..255) or "lucky"
 * (GBF_LUCKY_NUMBER_SHOW_FLAG, Gen 2 only, 0 or 1 -- the "already shown today" flag,
 * never the derived lucky NUMBER itself, per docs/GB-FLAGS-RESEARCH.md). */
static int do_counter(GbSession* s, const char* field_tok, const char* value_tok) {
  GbGame g = gbt_game(s);
  GbField f;
  int maxv;
  if (!strcmp(field_tok, "safari")) { f = GBF_SAFARI_STEPS; maxv = 255; }
  else if (!strcmp(field_tok, "lucky")) { f = GBF_LUCKY_NUMBER_SHOW_FLAG; maxv = 1; }
  else { fprintf(stderr, "unknown counter field %s (want safari|lucky)\n", field_tok); return 2; }

  uint32_t off = gbf_off(g, f);
  if (!off) { fprintf(stderr, "counter %s does not exist on this game\n", field_tok); return 2; }
  int v = resolve_uint(value_tok, "counter value");
  if (v < 0 || v > maxv) { fprintf(stderr, "bad counter value %s (want 0..%d)\n", value_tok, maxv); return 2; }

  uint8_t b = (uint8_t)v;
  GbsStatus ws = gbs_write_field(s, off, &b, 1);
  if (ws != GBS_OK) return refuse(gbs_status_text(ws));
  GbsStatus fs = gbs_finish(s);
  if (fs != GBS_OK) return refuse(gbs_status_text(fs));
  return 0;
}

/* BACKLOG #94 -- via gb_boxnames.h. Gen 1 refused (gbbn_rename's own GBS_ERR_ARG, no
 * box names at all). */
static int do_boxname(GbSession* s, const char* box_tok, const char* text) {
  int box = resolve_uint(box_tok, "box");
  if (box < 0) return 2;
  GbsStatus st = gbbn_rename(s, box, text);
  if (st != GBS_OK) return refuse(gbs_status_text(st));
  return 0;
}

/* BACKLOG #95 review gate case: the held-item field on ONE mon, box-shaped exactly
 * like do_level/do_text above (gbs_load_list -> gb_load -> setter -> gb_commit_checked
 * -> gbs_commit_list). gb_set_held_item itself refuses a Gen-1 record (rec+0x01 is
 * current HP there, not an item) and a non-zero item on a Gen-2 Egg (review C5) --
 * both surface here as an ordinary refusal, not a crash, so tools/gb_retail_gate.py
 * can drive this against a real Gold AND a real Crystal boot and read back the WRAM
 * party struct to prove the write actually reached the booted game. */
static int do_helditem(GbSession* s, int box, int slot, const char* id_tok) {
  int id = resolve_uint(id_tok, "held item id");
  if (id < 0) return 2;
  if (id > 255) { fprintf(stderr, "held item id must be 0..255\n"); return 2; }
  GbsStatus ls = gbs_load_list(s, box, g_list);
  if (ls != GBS_OK) return refuse(gbs_status_text(ls));
  GbEditMon e;
  if (!gb_load(&e, s->gen, g_list, box, slot))
    return refuse("gb_load: bad box/slot for this generation");
  if (!gb_set_held_item(&e, (uint8_t)id))
    return refuse("gb_set_held_item refused (Gen 1, or a non-zero item on an Egg)");
  if (!gb_commit_checked(&e, g_list, box, slot))
    return refuse("gb_commit_checked: the write did not verify");
  GbsStatus cs = gbs_commit_list(s, box, g_list);
  if (cs != GBS_OK) return refuse(gbs_status_text(cs));
  return 0;
}

/* BACKLOG #95 review gate case, gbmon C11 fix's own binding test: --op caught proves
 * the PRODUCTION wiring, not just gb_set_caught in isolation -- host_gbeditor_test.c's
 * existing coverage sets has_caught ITSELF (the review's own complaint: it could not
 * see a missing caller), so it could not catch source/pdna_gen12.c ever forgetting to
 * call gb_mark_caught. This op goes through the EXACT SAME decision the live editor
 * uses (gb_mark_caught, source/pdna_gen12.c) -- gb_session_is_crystal(s), the one
 * function both the editor and this tool call so they cannot diverge -- rather than
 * re-deriving "is this Crystal" a third way. PACKED is "time:level:loc:gender", one
 * argument so the op keeps do_helditem's box-shaped 3-argument style (--op caught BOX
 * SLOT PACKED) instead of growing Op.a past its [4] capacity. Refuses on a Gold/
 * Silver target (has_caught stays false, gb_set_caught's own gate) and succeeds on
 * Crystal -- tools/gb_retail_gate.py drives both against a real boot. */
static int do_caught(GbSession* s, int box, int slot, const char* packed) {
  int time, level, loc, gender;
  if (sscanf(packed, "%d:%d:%d:%d", &time, &level, &loc, &gender) != 4) {
    fprintf(stderr, "bad caught PACKED %s (want time:level:loc:gender)\n", packed);
    return 2;
  }
  if (time < 0 || time > 255 || level < 0 || level > 255 ||
      loc < 0 || loc > 255 || gender < 0 || gender > 255) {
    fprintf(stderr, "caught PACKED fields must each be 0..255: %s\n", packed);
    return 2;
  }
  GbsStatus ls = gbs_load_list(s, box, g_list);
  if (ls != GBS_OK) return refuse(gbs_status_text(ls));
  GbEditMon e;
  if (!gb_load(&e, s->gen, g_list, box, slot))
    return refuse("gb_load: bad box/slot for this generation");
  gb_set_caught_available(&e, gb_session_is_crystal(s));   /* the live editor's own gate */
  if (!gb_set_caught(&e, (uint8_t)time, (uint8_t)level, (uint8_t)loc, (uint8_t)gender))
    return refuse("gb_set_caught refused (Gen 1, Gold/Silver, or an out-of-range field)");
  if (!gb_commit_checked(&e, g_list, box, slot))
    return refuse("gb_commit_checked: the write did not verify");
  GbsStatus cs = gbs_commit_list(s, box, g_list);
  if (cs != GBS_OK) return refuse(gbs_status_text(cs));
  return 0;
}

/* Dispatch one already-shaped Op. Returns 0 ok, 1 refused (reported), 2 usage (reported). */
static int apply_op(GbSession* s, const Op* o) {
  if (!strcmp(o->kind, "nick") || !strcmp(o->kind, "ot")) {
    int box = resolve_box(s, o->a[0]);
    int slot = resolve_slot(o->a[1]);
    if (box < 0 || slot < 0) return 2;
    return do_text(s, !strcmp(o->kind, "nick") ? GBE_NICK : GBE_OT, box, slot, o->a[2]);
  }
  if (!strcmp(o->kind, "level")) {
    int box = resolve_box(s, o->a[0]);
    int slot = resolve_slot(o->a[1]);
    if (box < 0 || slot < 0) return 2;
    return do_level(s, box, slot, o->a[2]);
  }
  if (!strcmp(o->kind, "dv")) {
    int box = resolve_box(s, o->a[0]);
    int slot = resolve_slot(o->a[1]);
    if (box < 0 || slot < 0) return 2;
    return do_dv(s, box, slot, o->a[2], o->a[3]);
  }
  if (!strcmp(o->kind, "delete")) {
    int box = resolve_box(s, o->a[0]);
    int slot = resolve_slot(o->a[1]);
    if (box < 0 || slot < 0) return 2;
    return do_delete(s, box, slot);
  }
  if (!strcmp(o->kind, "move")) {
    int from_box = resolve_box(s, o->a[0]);
    int slot = resolve_slot(o->a[1]);
    int to_box = resolve_box(s, o->a[2]);
    if (from_box < 0 || slot < 0 || to_box < 0) return 2;
    return do_move(s, from_box, slot, to_box);
  }
  if (!strcmp(o->kind, "money")) {
    return do_money(s, o->a[0]);
  }
  if (!strcmp(o->kind, "create")) return do_create(s, o->a[0], o->a[1]);
  if (!strcmp(o->kind, "badges")) {
    return do_badges(s, o->a[0]);
  }
  if (!strcmp(o->kind, "name")) {
    return do_name(s, o->a[0]);
  }
  if (!strcmp(o->kind, "badges2")) {
    return do_badges2(s, o->a[0], o->a[1]);
  }
  if (!strcmp(o->kind, "item")) {
    return do_item(s, o->a[0], o->a[1], o->a[2]);
  }
  if (!strcmp(o->kind, "daycare")) {
    return do_daycare(s, o->a[0], o->a[1]);
  }
  if (!strcmp(o->kind, "clockshift")) {
    return do_clockshift(s, o->a[0], o->a[1], o->a[2], o->a[3]);
  }
  if (!strcmp(o->kind, "clockreset")) {
    return do_clockreset(s);
  }
  if (!strcmp(o->kind, "clockclear")) {
    return do_clockclear(s);
  }
  if (!strcmp(o->kind, "fly")) {
    return do_fly(s, o->a[0]);
  }
  if (!strcmp(o->kind, "boxname")) {
    return do_boxname(s, o->a[0], o->a[1]);
  }
  /* -- append new dispatch cases HERE, last (see the shape[] append note above). */
  if (!strcmp(o->kind, "helditem")) {
    int box = resolve_box(s, o->a[0]);
    int slot = resolve_slot(o->a[1]);
    if (box < 0 || slot < 0) return 2;
    return do_helditem(s, box, slot, o->a[2]);
  }
  if (!strcmp(o->kind, "caught")) {
    int box = resolve_box(s, o->a[0]);
    int slot = resolve_slot(o->a[1]);
    if (box < 0 || slot < 0) return 2;
    return do_caught(s, box, slot, o->a[2]);
  }
  if (!strcmp(o->kind, "flagset")) {
    return do_flagset(s, o->a[0], o->a[1]);
  }
  if (!strcmp(o->kind, "counter")) {
    return do_counter(s, o->a[0], o->a[1]);
  }
  if (!strcmp(o->kind, "warp")) {
    return do_warp(s, o->a[0], o->a[1], o->a[2]);
  }
  if (!strcmp(o->kind, "warpvp")) {
    return do_warp_vp(s, o->a[0], o->a[1], o->a[2], o->a[3]);
  }
  if (!strcmp(o->kind, "mapquery")) {
    return do_mapquery(s, o->a[0]);
  }
  if (!strcmp(o->kind, "warp2")) {
    return do_warp2(s, o->a[0], o->a[1], o->a[2], o->a[3]);
  }
  if (!strcmp(o->kind, "statusflags")) {
    return do_statusflags(s, o->a[0]);
  }
  if (!strcmp(o->kind, "gender")) {
    return do_gender(s, o->a[0]);
  }
  if (!strcmp(o->kind, "hofclear")) {
    return do_hofclear(s);
  }
  if (!strcmp(o->kind, "hofcount")) {
    return do_hofcount(s, o->a[0]);
  }
  if (!strcmp(o->kind, "hofappend")) {
    return do_hofappend(s);
  }
  if (!strcmp(o->kind, "hofdelete")) {
    return do_hofdelete(s);
  }
  if (!strcmp(o->kind, "dexset")) {
    return do_dexset(s, o->a[0], o->a[1]);
  }
  if (!strcmp(o->kind, "unownreset")) {
    return do_unownreset(s);
  }
  if (!strcmp(o->kind, "hofnick")) {
    return do_hofnick(s, o->a[0], o->a[1], o->a[2]);
  }
  if (!strcmp(o->kind, "hofdv")) {
    return do_hofdv(s, o->a[0], o->a[1], o->a[2], o->a[3]);
  }
  if (!strcmp(o->kind, "paste80")) {
    return do_paste80(s, o->a[0], o->a[1]);
  }
  fprintf(stderr, "unknown op %s\n", o->kind);   /* unreachable: parse_args validated */
  return 2;
}

/* BACKLOG #211: `--list --moves` appends one extra line per slot with the four raw
 * move ids, current PP, and PP-Ups -- a SEPARATE line (never appended to the existing
 * "  slot N: dex=... level=... nick=..." line), so tools/gb_retail_gate.py's own
 * LIST_SLOT_RE keeps matching byte-for-byte and every existing caller of --list is
 * unaffected; only a caller that also passes --moves ever sees the new line. Read
 * straight off the DUMP the real game wrote after booting (gb_retail_gate.py boots
 * `--in` on the real ROM, mGBA then dumps SRAM back out) -- this is "the gate's own
 * readback path", not a re-check of what the surgery tool itself computed. */
static void list_box(GbSession* s, int box, const char* label) {
  GbsStatus ls = gbs_load_list(s, box, g_list);
  if (ls != GBS_OK) { printf("%s: %s\n", label, gbs_status_text(ls)); return; }
  int count = gb_list_count(s->gen, g_list, box);
  printf("%s: count=%d\n", label, count);
  for (int slot = 0; slot < count; slot++) {
    GbEditMon e;
    if (!gb_load(&e, s->gen, g_list, box, slot)) {
      printf("  slot %d: (unreadable)\n", slot);
      continue;
    }
    char nick[GB_TEXT_MAX];
    gb_get_nickname(&e, nick, sizeof nick);
    printf("  slot %d: dex=%u level=%u nick=%s\n", slot,
           (unsigned)gb_get_species_dex(&e), (unsigned)gb_get_level(&e), nick);
    if (g_list_moves) {
      /* ppmax = pk_move_pp(move id) -- the table this SAME tool already links
       * (data_tables.c) -- so a Python-side check can assert "pp == base PP"
       * without keeping a second copy of the PP table. ppmax is 0 for an empty
       * slot (pk_move_pp(0) -- never asserted on there, an empty slot's pp/ppup
       * are always 0 too). */
      printf("    moves=%u,%u,%u,%u pp=%u,%u,%u,%u ppmax=%u,%u,%u,%u ppup=%u,%u,%u,%u\n",
             (unsigned)gb_get_move(&e, 0), (unsigned)gb_get_move(&e, 1),
             (unsigned)gb_get_move(&e, 2), (unsigned)gb_get_move(&e, 3),
             (unsigned)gb_get_pp(&e, 0), (unsigned)gb_get_pp(&e, 1),
             (unsigned)gb_get_pp(&e, 2), (unsigned)gb_get_pp(&e, 3),
             (unsigned)pk_move_pp(gb_get_move(&e, 0)), (unsigned)pk_move_pp(gb_get_move(&e, 1)),
             (unsigned)pk_move_pp(gb_get_move(&e, 2)), (unsigned)pk_move_pp(gb_get_move(&e, 3)),
             (unsigned)gb_get_ppup(&e, 0), (unsigned)gb_get_ppup(&e, 1),
             (unsigned)gb_get_ppup(&e, 2), (unsigned)gb_get_ppup(&e, 3));
    }
  }
}

static void do_list(GbSession* s) {
  int nb = gbs_nboxes(s), pb = gbs_party_box(s);
  for (int b = 0; b < nb; b++) {
    char label[16];
    snprintf(label, sizeof label, "box %d", b);
    list_box(s, b, label);
  }
  list_box(s, pb, "party");
}

int main(int argc, char** argv) {
  const char* in_path = NULL;
  const char* out_path = NULL;
  bool list_mode = false;
  Op ops[MAX_OPS];
  int nops = 0;

  int pr = parse_args(argc, argv, &in_path, &out_path, &list_mode, ops, &nops);
  if (pr) { usage(argv[0]); return pr; }

  uint32_t len = 0;
  int rr = read_whole(in_path, &len);
  if (rr) return rr;

  GbSession s;
  GbsStatus os = gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch);
  if (os != GBS_OK) return refuse(gbs_status_text(os));

  if (list_mode) { do_list(&s); return 0; }

  for (int i = 0; i < nops; i++) {
    int rc = apply_op(&s, &ops[i]);
    if (rc) return rc;
  }
  return write_whole(out_path, len);
}
