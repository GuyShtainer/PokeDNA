/* tests/host_gbsurgery_tool.c — a CLI over gb_session/gb_edit/gb_editor for the S4
 * retail-boot regression gate (tools/gb_retail_gate.py) to build EDITED Gen-1/2 saves
 * from a corpus save without hand-writing bytes.
 *
 * Named *_tool (not *_test) so tests/run_host_tests.py's `host_*_test.c` glob ignores
 * it: this is a driver PROGRAM, not a self-checking test.
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_gbsurgery_tool.c \
 *      source/gb_session.c source/gb_editor.c source/gb_edit.c source/gen1_save.c \
 *      source/gen1_write.c source/gen2_save.c source/gen2_write.c \
 *      source/data_tables.c -o /tmp/hgbsurg
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
#include "gb_new_mon.h"
#include "data_tables.h"

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

typedef struct {
  const char* kind;   /* "nick" / "ot" / "level" / "dv" / "delete" / "move" / "create" */
  const char* a[4];
  int n;
} Op;

static void usage(const char* prog) {
  fprintf(stderr,
    "usage: %s --in SAVE --out EDITED [--rom ROM] [--op ...]...\n"
    "       %s --in SAVE --list\n"
    "  --op nick BOX SLOT TEXT\n"
    "  --op ot BOX SLOT TEXT\n"
    "  --op level BOX SLOT N\n"
    "  --op dv BOX SLOT STAT V     STAT in atk|def|spe|spc, V 0..15\n"
    "  --op delete BOX SLOT\n"
    "  --op move FROM_BOX SLOT TO_BOX\n"
    "  --op money VALUE            0..999999, gbs_write_field + gbs_finish\n"
    "  --op create BOX DEX  needs --rom; box only, not the party; level is the\n"
    "                       species' own lowest legal one (rom_gblearn_min_level)\n"
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
  };
  *in = NULL; *out = NULL; *list_mode = false; *nops = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--in") && i + 1 < argc) { *in = argv[++i]; continue; }
    if (!strcmp(argv[i], "--out") && i + 1 < argc) { *out = argv[++i]; continue; }
    if (!strcmp(argv[i], "--rom") && i + 1 < argc) { g_rom_path = argv[++i]; continue; }
    if (!strcmp(argv[i], "--list")) { *list_mode = true; continue; }
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

static bool tool_rom_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FILE* f = (FILE*)ctx;
  if (fseek(f, (long)off, SEEK_SET) != 0) return false;
  if (fread(dst, 1, len, f) != len) return false;
  return true;
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
                               g_romscratch, sizeof g_romscratch);
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
  fprintf(stderr, "unknown op %s\n", o->kind);   /* unreachable: parse_args validated */
  return 2;
}

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
