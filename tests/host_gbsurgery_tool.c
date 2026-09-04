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
 *   host_gbsurgery_tool --in SAVE --out EDITED [--op ...]...
 *     --op nick BOX SLOT TEXT      gb_load -> gbe_set_text(GBE_NICK) -> gb_commit_checked
 *     --op ot BOX SLOT TEXT        same, GBE_OT
 *     --op level BOX SLOT N        gb_set_level(N); party slots also get gbe_settle_stats
 *     --op dv BOX SLOT STAT V      STAT in atk|def|spe|spc, V 0..15
 *     --op delete BOX SLOT         gbs_delete
 *     --op move FROM_BOX SLOT TO_BOX   gbs_move (prints the landing slot)
 *   host_gbsurgery_tool --in SAVE --list
 *     print every box: count, and per slot species dex / level / nickname
 *
 * BOX accepts 0..n-1 or the literal "party" (resolved through gbs_party_box() once the
 * session is open, so the same token works for both generations without the caller
 * knowing which one it is).
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

#define MAX_FILE_BYTES 65536u
#define MAX_OPS        64

static uint8_t g_img[MAX_FILE_BYTES];
static uint8_t g_scratch[GBS_SCRATCH_BYTES];
static uint8_t g_list[GBS_LIST_BYTES];
static uint8_t g_list2[GBS_LIST_BYTES];

typedef struct {
  const char* kind;   /* "nick" / "ot" / "level" / "dv" / "delete" / "move" */
  const char* a[4];
  int n;
} Op;

static void usage(const char* prog) {
  fprintf(stderr,
    "usage: %s --in SAVE --out EDITED [--op ...]...\n"
    "       %s --in SAVE --list\n"
    "  --op nick BOX SLOT TEXT\n"
    "  --op ot BOX SLOT TEXT\n"
    "  --op level BOX SLOT N\n"
    "  --op dv BOX SLOT STAT V     STAT in atk|def|spe|spc, V 0..15\n"
    "  --op delete BOX SLOT\n"
    "  --op move FROM_BOX SLOT TO_BOX\n"
    "BOX is 0..n-1 or the literal \"party\".\n", prog, prog);
}

/* Parse argv into (in, out, list_mode, ops[]). Returns 2 on any usage problem (already
 * reported to stderr), else 0. Never touches a file. */
static int parse_args(int argc, char** argv, const char** in, const char** out,
                      bool* list_mode, Op ops[MAX_OPS], int* nops) {
  static const struct { const char* kind; int n; } shape[] = {
    {"nick", 3}, {"ot", 3}, {"level", 3}, {"dv", 4}, {"delete", 2}, {"move", 3},
  };
  *in = NULL; *out = NULL; *list_mode = false; *nops = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--in") && i + 1 < argc) { *in = argv[++i]; continue; }
    if (!strcmp(argv[i], "--out") && i + 1 < argc) { *out = argv[++i]; continue; }
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

static int resolve_slot(const char* tok) {
  char* end = NULL;
  long v = strtol(tok, &end, 10);
  if (end == tok || *end != '\0' || v < 0) {
    fprintf(stderr, "bad slot %s\n", tok);
    return -1;
  }
  return (int)v;
}

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
  int lvl = resolve_slot(ntok);
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
  int v = resolve_slot(vtok);
  if (v < 0 || v > 15) { fprintf(stderr, "bad dv value %s (want 0..15)\n", vtok); return 2; }
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
