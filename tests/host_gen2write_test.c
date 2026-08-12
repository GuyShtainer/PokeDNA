/* Host test for the Generation-II write path (source/gen2_write.c).
 *
 *   cc -std=c11 -I source -I tests tests/host_gen2write_test.c tests/gen12_fixture.c \
 *      source/gen2_save.c source/gen2_write.c -o /tmp/hg2w && /tmp/hg2w
 *
 * Two corpora, on purpose:
 *
 *   - Guy's REAL Gold and Crystal cartridge saves, when they are present. They live
 *     outside the repo (gitignored at gba-toolkit/roms/gb/) and are only ever read, so a
 *     missing corpus SKIPS rather than fails — but everything that touches them here
 *     works on an in-memory copy and the files on disk are never opened for writing.
 *   - tests/gen12_fixture.c's synthetic G/S and Crystal images, so the test still means
 *     something on a machine without the corpus.
 *
 * The distinction matters more than usual in this module. The G/S backup mirror's second
 * region was written as 0x3D96 in both the parser and the fixture — the same transposed
 * digit twice — and every synthetic test passed because the fixture was built from the
 * constant it was supposedly checking. Only the real Gold save caught it. So the
 * strongest assertions below (no-op round trip, mirror exactness, checksum agreement)
 * are the ones aimed at real cartridge data, and the tests that can only be built from
 * our own constants are labelled as the weaker evidence they are.
 *
 * There is also a section that deliberately BREAKS things — malformed lists, a storage
 * backend that silently drops writes, a post-commit byte flip — because a verification
 * gate nobody has ever seen fail is not evidence of anything.
 *
 * Takes no arguments.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "gen2_save.h"
#include "gen2_write.h"
#include "gen12_fixture.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_fail = 0, g_check = 0;
#define CHECK(c, msg) do { g_check++; if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)
#define CHECK_ST(st, want, msg) do {                                                  \
    g_check++;                                                                        \
    G2WStatus st_ = (st);                                                             \
    if (st_ != (want)) {                                                              \
      printf("  !! FAIL: %s (got %s)\n", msg, g2w_status_text(st_)); g_fail++;        \
    }                                                                                 \
  } while (0)

/* ------------------------------------------------- an in-memory save file */

typedef struct {
  uint8_t  buf[64 * 1024];
  uint32_t len;
  uint32_t writes;          /* bytes actually written — "nothing happened" assertions */
  /* Sabotage: pretend writes in [sab_lo, sab_hi) succeeded without performing them.
   * Models the failure this module exists to survive — a storage layer that says yes. */
  uint32_t sab_lo, sab_hi;
  bool     sab_on;
} Mem;

static bool mem_rd(void* ctx, uint32_t off, void* dst, uint32_t n) {
  Mem* m = (Mem*)ctx;
  if (off + n > m->len) return false;
  memcpy(dst, m->buf + off, n);
  return true;
}
static bool mem_wr(void* ctx, uint32_t off, const void* src, uint32_t n) {
  Mem* m = (Mem*)ctx;
  if (off + n > m->len) return false;
  if (m->sab_on && off < m->sab_hi && m->sab_lo < off + n) return true;   /* silently lost */
  memcpy(m->buf + off, src, n);
  m->writes += n;
  return true;
}

static uint8_t g_scratch[256];

static G2WStatus open_w(G2Writer* w, Mem* m, G2Version expect) {
  return g2w_begin(w, mem_rd, mem_wr, m, m->len, g_scratch, sizeof g_scratch, expect);
}

static Mem g_a, g_b;          /* 64 KiB each — too big for a stack frame */

static bool load_real(const char* name, Mem* m) {
  char p[512];
  snprintf(p, sizeof p, "%s/%s", ROMS, name);
  FILE* f = fopen(p, "rb");
  if (!f) return false;
  memset(m, 0, sizeof *m);
  m->len = (uint32_t)fread(m->buf, 1, sizeof m->buf, f);
  fclose(f);
  return m->len >= G2_SAVE_SIZE;
}

static void load_fixture(GbfGame game, Mem* m, uint32_t tail) {
  memset(m, 0, sizeof *m);
  m->len = gbf_build(game, m->buf, tail);
}

/* The reader's own verdict on the whole image, used as the independent oracle after
 * every edit: gen2_save.c computes its checksums from its private span table, this
 * module derives them from g2_mirror_map(), and the two agreeing is evidence. */
static void assert_healthy(const Mem* m, G2Version ver, const char* what) {
  G2Save sv;
  g2_detect(m->buf, m->len, &sv);
  char msg[160];
  snprintf(msg, sizeof msg, "%s: save still parses as %s", what, g2_version_name(ver));
  CHECK(sv.supported && sv.version == ver, msg);
  snprintf(msg, sizeof msg, "%s: PRIMARY checksum valid", what);
  CHECK(sv.primary_ok, msg);
  snprintf(msg, sizeof msg, "%s: BACKUP checksum valid", what);
  CHECK(sv.backup_ok, msg);

  uint32_t p_off = g2_checksum_primary_off(ver), b_off = g2_checksum_backup_off(ver);
  uint16_t p_stored = (uint16_t)(m->buf[p_off] | (m->buf[p_off + 1] << 8));
  uint16_t b_stored = (uint16_t)(m->buf[b_off] | (m->buf[b_off + 1] << 8));
  snprintf(msg, sizeof msg, "%s: stored primary sum == recomputed", what);
  CHECK(p_stored == g2_checksum_primary(m->buf, ver), msg);
  snprintf(msg, sizeof msg, "%s: stored backup sum == recomputed", what);
  CHECK(b_stored == g2_checksum_backup(m->buf, ver), msg);
}

/* Every byte of the checksummed span must equal its mirror. Done directly, region by
 * region, because two different regions can share a 16-bit sum — a checksum that
 * validates is not proof the mirror is right. */
static void assert_mirror_exact(const Mem* m, G2Version ver, const char* what) {
  const G2MirrorRegion* mr;
  int n = g2_mirror_map(ver, &mr);
  CHECK(n > 0, "the version has a mirror map");
  for (int i = 0; i < n; i++) {
    uint32_t len = mr[i].to - mr[i].from + 1;
    char msg[160];
    snprintf(msg, sizeof msg, "%s: mirror region %d (%04X-%04X -> %04X) byte-exact",
             what, i, mr[i].from, mr[i].to, mr[i].dest);
    CHECK(memcmp(m->buf + mr[i].from, m->buf + mr[i].dest, len) == 0, msg);
  }
}

/* ------------------------------------------------------------------ helpers */

static int all_boxes(int i) { return i == G2_NUM_BOXES ? G2_BOX_PARTY : i; }

typedef struct { uint8_t species, level, dv[4]; char nick[G2_NAME_BYTES], ot[G2_NAME_BYTES]; bool egg; } Snap;

static bool snap_slot(const uint8_t* list, int box, int slot, Snap* s) {
  G2Mon m;
  if (!g2_list_mon(list, box, slot, &m)) return false;
  s->species = m.species; s->level = m.level; s->egg = m.is_egg;
  memcpy(s->dv, m.dv, 4);
  snprintf(s->nick, sizeof s->nick, "%s", m.nickname);
  snprintf(s->ot,   sizeof s->ot,   "%s", m.otname);
  return true;
}
static bool snap_eq(const Snap* a, const Snap* b) {
  return a->species == b->species && a->level == b->level && a->egg == b->egg &&
         memcmp(a->dv, b->dv, 4) == 0 &&
         strcmp(a->nick, b->nick) == 0 && strcmp(a->ot, b->ot) == 0;
}

/* A BANKED box — never the open one, so the current-box duality is not in play — holding
 * at least `min_count` Pokemon, and with a free slot when `need_room`. Leaves the chosen
 * box staged in `list`. Returns -1 if the image has no such box.
 *
 * This replaces a hardcoded `w.current_box == 1 ? 2 : 1` that four tests shared. That was
 * true of Guy's cartridge saves and false of the synthetic fixture (which fills boxes 0,
 * 5 and 8, and opens box 5), so on any machine WITHOUT the private corpus three of those
 * tests did not skip — they ran against an empty box and reported thirty failures. A test
 * that is red for want of a file it says is optional teaches everyone to ignore it. */
static int pick_box(G2Writer* w, uint8_t* list, int min_count, bool need_room) {
  for (int b = 0; b < G2_NUM_BOXES; b++) {
    if (b == w->current_box) continue;
    if (g2w_load_list(w, b, list) != G2W_OK) continue;
    int n = g2_list_count(list, b);
    if (n < min_count) continue;
    if (need_room && n >= g2_list_capacity(b)) continue;
    if (g2w_check_list(list, b) != G2W_OK) continue;
    return b;
  }
  return -1;
}

/* ============================================================ 1. no-op round trip */

/* The strongest single assertion in the file: read every box and the party out of a REAL
 * cartridge save, hand them straight back, refresh both checksums — and require the
 * 32816-byte file to come out bit for bit identical. That covers the RTC tail (Gold has
 * 48 bytes of MBC3 clock footer past the save), the two padding bytes after every box
 * list, the junk past every name terminator and both stored checksums at once. */
static void t_noop_roundtrip(const char* file, G2Version ver) {
  if (!load_real(file, &g_a)) { printf("  SKIP %s (corpus not present)\n", file); return; }
  memcpy(&g_b, &g_a, sizeof g_b);          /* the pristine reference */
  printf("  -- no-op round trip: %s (%u bytes, tail %u)\n", file, g_a.len,
         g_a.len - G2_SAVE_SIZE);

  G2Writer w;
  CHECK_ST(open_w(&w, &g_a, ver), G2W_OK, "g2w_begin on a real save");
  if (!w.ready) return;

  static uint8_t list[G2_MAX_LIST_SIZE];
  for (int i = 0; i <= G2_NUM_BOXES; i++) {
    int box = all_boxes(i);
    CHECK_ST(g2w_load_list(&w, box, list), G2W_OK, "load a real box");
    /* Both tiers have to ACCEPT real cartridge data — a gate that rejects the genuine
     * article is just a bug with a good excuse. */
    CHECK_ST(g2w_check_list(list, box), G2W_OK, "the gate accepts a real box");
    CHECK_ST(g2w_check_list_strict(list, box), G2W_OK, "and so does the strict check");
    CHECK_ST(g2w_commit_list(&w, box, list), G2W_OK, "no-op commit of a real box");
  }
  CHECK_ST(g2w_finish(&w), G2W_OK, "g2w_finish");

  CHECK(g_a.len == g_b.len, "file length unchanged");
  CHECK(memcmp(g_a.buf, g_b.buf, g_a.len) == 0,
        "a no-op edit leaves the save BYTE-IDENTICAL (RTC tail included)");
  if (memcmp(g_a.buf, g_b.buf, g_a.len) != 0)
    for (uint32_t k = 0; k < g_a.len; k++)
      if (g_a.buf[k] != g_b.buf[k]) { printf("     first difference at %04X: %02X -> %02X\n",
                                             k, g_b.buf[k], g_a.buf[k]); break; }
  assert_healthy(&g_a, ver, "after no-op");
  assert_mirror_exact(&g_a, ver, "after no-op");
}

/* ================================================ 2+3. an edit reaches BOTH copies */

/* Box names live inside the checksummed span, so editing one has to land in the primary
 * block AND in the backup mirror. Checked by comparing the two regions directly. */
static void t_edit_mirrors(const char* file, G2Version ver) {
  if (!load_real(file, &g_a)) { printf("  SKIP %s (corpus not present)\n", file); return; }
  printf("  -- mirrored edit: %s\n", file);
  G2Writer w;
  if (open_w(&w, &g_a, ver) != G2W_OK) { CHECK(0, "g2w_begin"); return; }

  G2Offsets o;
  CHECK(g2_offsets(ver, &o), "offsets for this version");
  uint32_t name_off = o.box_names + 3 * 9;          /* box index 3 */
  uint32_t mirror_off = 0;
  CHECK(g2w_mirror_of(ver, name_off, &mirror_off),
        "a box name is inside the checksummed span and therefore mirrored");

  CHECK_ST(g2w_set_box_name(&w, 3, "PokeDNA"), G2W_OK, "set a box name");

  char back[G2_NAME_BYTES];
  G2Save sv; g2_detect(g_a.buf, g_a.len, &sv);
  CHECK(g2_box_name_at(g_a.buf, &sv, 3, back, sizeof back) && strcmp(back, "PokeDNA") == 0,
        "the reader reads the new box name back");
  /* THE assertion: same bytes in both copies, compared directly rather than via a sum. */
  CHECK(memcmp(g_a.buf + name_off, g_a.buf + mirror_off, 9) == 0,
        "the box name landed in the PRIMARY and in the MIRROR");
  assert_healthy(&g_a, ver, "after a box-name edit");
  assert_mirror_exact(&g_a, ver, "after a box-name edit");

  /* Same again for a party record, which is a different mirror region in G/S. */
  static uint8_t list[G2_MAX_LIST_SIZE];
  CHECK_ST(g2w_load_list(&w, G2_BOX_PARTY, list), G2W_OK, "load the party");
  CHECK_ST(g2w_set_nickname(list, G2_BOX_PARTY, 0, "GATEKEEPER"), G2W_OK, "rename a party mon");
  CHECK_ST(g2w_commit_list(&w, G2_BOX_PARTY, list), G2W_OK, "commit the party");

  uint32_t p_mirror = 0;
  CHECK(g2w_mirror_of(ver, o.party_list, &p_mirror), "the party is inside the span");
  CHECK(memcmp(g_a.buf + o.party_list, g_a.buf + p_mirror, G2_PARTY_LIST_SIZE) == 0,
        "the party edit landed in the PRIMARY and in the MIRROR");
  G2Mon m;
  g2_detect(g_a.buf, g_a.len, &sv);
  G2Header hd; g2_read_header(g_a.buf, &sv, &hd);
  CHECK(g2_box_mon_at(g_a.buf, &sv, &hd, G2_BOX_PARTY, 0, &m) &&
        strcmp(m.nickname, "GATEKEEPER") == 0, "the reader sees the new nickname");
  assert_healthy(&g_a, ver, "after a party edit");
  assert_mirror_exact(&g_a, ver, "after a party edit");
}

/* ==================================== 6. the current box exists twice, and both change */

static void t_current_box_duality(const char* file, G2Version ver) {
  if (!load_real(file, &g_a)) { printf("  SKIP %s (corpus not present)\n", file); return; }
  G2Writer w;
  if (open_w(&w, &g_a, ver) != G2W_OK) { CHECK(0, "g2w_begin"); return; }
  int cur = w.current_box;
  printf("  -- current-box duality: %s (open box %d)\n", file, cur);

  bool agree = false;
  CHECK_ST(g2w_current_box_agrees(&w, &agree), G2W_OK, "compare the two copies");
  CHECK(agree, "a game-written save has identical live and banked copies of the open box");

  uint32_t live   = g2_list_offset(&w.sv, cur, cur);   /* main data           */
  uint32_t banked = g2_list_offset(&w.sv, cur, -1);    /* SRAM bank 2/3       */
  CHECK(live != banked, "the two copies really are two places");

  static uint8_t list[G2_MAX_LIST_SIZE];
  CHECK_ST(g2w_load_list(&w, cur, list), G2W_OK, "load the open box");
  CHECK_ST(g2w_set_nickname(list, cur, 0, "TWOPLACES"), G2W_OK, "rename slot 0");
  CHECK_ST(g2w_commit_list(&w, cur, list), G2W_OK, "commit the open box");

  /* Writing only one of these is the silent-data-loss bug this whole module is shaped
   * around: the game reloads the BANKED copy over the live one at boot. */
  CHECK(memcmp(g_a.buf + live, list, G2_BOX_LIST_SIZE) == 0,
        "the LIVE copy of the open box got the edit");
  CHECK(memcmp(g_a.buf + banked, list, G2_BOX_LIST_SIZE) == 0,
        "the BANKED copy of the open box got the edit too");
  assert_healthy(&g_a, ver, "after an open-box edit");
}

/* ====================================== 4. a G/S edit must not reach a Crystal save */

static void t_wrong_version(void) {
  printf("  -- wrong-version refusal\n");
  G2Writer w;

  if (load_real("Gold.sav", &g_a)) {
    CHECK_ST(open_w(&w, &g_a, G2_VER_CRYSTAL), G2W_ERR_VERSION,
             "a Gold save opened as Crystal is refused");
    CHECK(g_a.writes == 0, "...and nothing was written");
    CHECK(!w.ready, "...and the writer is left unusable");
    /* The refusal is not advice: with the version sealed by g2w_begin, forcing it after
     * the fact invalidates the seal, so there is no path at all that aims a Crystal
     * layout at a G/S image. */
    CHECK_ST(open_w(&w, &g_a, G2_VER_NONE), G2W_OK, "reopen it honestly");
    w.sv.version = G2_VER_CRYSTAL;                        /* the tamper */
    static uint8_t list[G2_MAX_LIST_SIZE];
    CHECK_ST(g2w_load_list(&w, 0, list), G2W_ERR_STATE, "a tampered writer refuses to load");
    uint8_t junk[4] = { 1, 2, 3, 4 };
    uint32_t before = g_a.writes;
    CHECK_ST(g2w_write_range(&w, 0x2100, junk, 4), G2W_ERR_STATE,
             "a tampered writer refuses to write");
    CHECK_ST(g2w_finish(&w), G2W_ERR_STATE, "a tampered writer refuses to finish");
    CHECK(g_a.writes == before, "...and really did not write");
  } else {
    printf("     SKIP Gold.sav (corpus not present)\n");
  }

  if (load_real("Crystal.sav", &g_b)) {
    CHECK_ST(open_w(&w, &g_b, G2_VER_GS), G2W_ERR_VERSION,
             "a Crystal save opened as Gold/Silver is refused");
    CHECK(g_b.writes == 0, "...and nothing was written");
  } else {
    printf("     SKIP Crystal.sav (corpus not present)\n");
  }

  /* A writer nobody initialised must be inert rather than interestingly wrong. */
  G2Writer zero;
  memset(&zero, 0, sizeof zero);
  CHECK_ST(g2w_finish(&zero), G2W_ERR_STATE, "an uninitialised writer refuses everything");

  /* Same refusal on the synthetic images, so this test still runs without the corpus. */
  load_fixture(GBF_GS, &g_a, 0);
  CHECK_ST(open_w(&w, &g_a, G2_VER_CRYSTAL), G2W_ERR_VERSION,
           "fixture G/S opened as Crystal is refused");
  CHECK(g_a.writes == 0, "...and nothing was written");
  load_fixture(GBF_CRYSTAL, &g_b, GBF_RTC_TAIL_64);
  CHECK_ST(open_w(&w, &g_b, G2_VER_GS), G2W_ERR_VERSION,
           "fixture Crystal opened as G/S is refused");
  CHECK(g_b.writes == 0, "...and nothing was written");
}

/* ============================================= 5. the gate fires on a broken structure */

/* Each mutation is something that would make the GAME misread the box — a count that
 * runs past the records into the OT-name array, a missing terminator, a species byte the
 * record disagrees with. All of them have to be refused BEFORE anything is written. */
static void t_broken_structures(void) {
  printf("  -- the structural gate\n");
  if (!load_real("Gold.sav", &g_a)) {
    load_fixture(GBF_GS, &g_a, 0);
    printf("     (using the synthetic G/S image — corpus not present)\n");
  }
  G2Writer w;
  if (open_w(&w, &g_a, G2_VER_NONE) != G2W_OK) { CHECK(0, "g2w_begin"); return; }

  static uint8_t good[G2_MAX_LIST_SIZE], bad[G2_MAX_LIST_SIZE];
  int box = pick_box(&w, good, 2, false);
  if (box < 0) { printf("     SKIP (no banked box with 2+ Pokemon)\n"); return; }
  int n = g2_list_count(good, box);

  struct { const char* why; int off; uint8_t val; } muts[] = {
    { "count byte above the capacity",            0,                        21   },
    { "count byte one too large (runs past the records)", 0,                (uint8_t)(n + 1) },
    { "the 0xFF terminator overwritten",          1 + n,                    0x19 },
    { "a listed species of 0 (a hole in the list)", 1,                      0x00 },
    { "a species-list byte that the record disagrees with", 1,              0xFC },
    { "an early 0xFF, so the count and the list disagree",  1,              0xFF },
  };
  for (unsigned i = 0; i < sizeof muts / sizeof muts[0]; i++) {
    memcpy(bad, good, (size_t)g2_list_size(box));
    bad[muts[i].off] = muts[i].val;
    uint32_t before = g_a.writes;
    char msg[160];
    snprintf(msg, sizeof msg, "refused: %s", muts[i].why);
    CHECK_ST(g2w_check_list(bad, box), G2W_ERR_STRUCT, msg);
    snprintf(msg, sizeof msg, "not written: %s", muts[i].why);
    CHECK_ST(g2w_commit_list(&w, box, bad), G2W_ERR_STRUCT, msg);
    CHECK(g_a.writes == before, "a refused commit writes nothing at all");
  }

  /* The list byte and the record's own species byte disagreeing is subtler and worse:
   * the box menu would show one Pokemon and the box would contain another. */
  memcpy(bad, good, (size_t)g2_list_size(box));
  bad[1] = (uint8_t)(bad[1] == 25 ? 26 : 25);
  CHECK_ST(g2w_check_list(bad, box), G2W_ERR_STRUCT,
           "refused: species list disagrees with the record");

  /* An early terminator where the RECORD agrees with it, so the species-agreement rule
   * cannot save us and only the terminator rule can. Without it the count byte says N
   * and the games' own list walk says fewer — RemoveMonFromPartyOrBox stops at the 0xFF
   * while the box menu trusts the count, and they start editing different Pokemon. */
  memcpy(bad, good, (size_t)g2_list_size(box));
  bad[1] = G2_LIST_TERMINATOR;
  bad[g2_off_record(box, 0)] = G2_LIST_TERMINATOR;
  CHECK_ST(g2w_check_list(bad, box), G2W_ERR_STRUCT,
           "refused: a 0xFF inside the occupied range, even with a matching record");

  /* The SECOND tier. An impossible level or a glitch species does not stop the games
   * walking the list, so it is a warning, not a refusal — otherwise a box holding one
   * glitch entry could never be edited, not even to delete the glitch. The strict check
   * has to catch it and the structural one has to let it through. */
  memcpy(bad, good, (size_t)g2_list_size(box));
  bad[g2_off_record(box, 0) + 0x1F] = 101;
  CHECK_ST(g2w_check_list(bad, box), G2W_OK, "level 101 is walkable...");
  CHECK_ST(g2w_check_list_strict(bad, box), G2W_ERR_CONTENT, "...but the strict check flags it");
  memcpy(bad, good, (size_t)g2_list_size(box));
  bad[g2_off_record(box, 0) + 0x1F] = 0;
  CHECK_ST(g2w_check_list_strict(bad, box), G2W_ERR_CONTENT, "strict check flags level 0");
  memcpy(bad, good, (size_t)g2_list_size(box));
  bad[g2_off_record(box, 0)] = 0xFC;            /* glitch species, in BOTH places */
  bad[1] = 0xFC;
  CHECK_ST(g2w_check_list(bad, box), G2W_OK, "a glitch species is walkable...");
  CHECK_ST(g2w_check_list_strict(bad, box), G2W_ERR_CONTENT, "...and the strict check flags it");
  CHECK_ST(g2w_commit_list(&w, box, bad), G2W_OK,
           "...and a box containing one can still be committed, so the user can fix it");
  CHECK_ST(g2w_delete(bad, box, 0), G2W_OK, "the glitch entry can be deleted");
  CHECK_ST(g2w_check_list_strict(bad, box), G2W_OK, "and the box is clean afterwards");
  CHECK_ST(g2w_commit_list(&w, box, good), G2W_OK, "put the real box back");

  /* A name field with no terminator anywhere in its 11 bytes. */
  memcpy(bad, good, (size_t)g2_list_size(box));
  memset(bad + g2_off_nickname(box, 0), 0x80, 11);
  CHECK_ST(g2w_check_list(bad, box), G2W_ERR_STRUCT, "refused: unterminated nickname");
  memcpy(bad, good, (size_t)g2_list_size(box));
  memset(bad + g2_off_otname(box, 0), 0x80, 11);
  CHECK_ST(g2w_check_list(bad, box), G2W_ERR_STRUCT, "refused: unterminated OT name");

  /* And the control: the untouched list still passes, so the gate is discriminating
   * rather than merely grumpy. */
  CHECK_ST(g2w_check_list(good, box), G2W_OK, "the unmutated list still passes");
  assert_healthy(&g_a, w.sv.version, "after a run of refusals");
}

/* ============================ prove the verification gate can actually fail ========= */

/* A gate nobody has watched fail is decoration. Three ways to make the write go wrong
 * underneath it, each of which the gate has to notice. */
static void t_gate_has_teeth(void) {
  printf("  -- the verify gate under sabotage\n");
  bool real = load_real("Gold.sav", &g_a);
  if (!real) { load_fixture(GBF_GS, &g_a, 0); printf("     (synthetic G/S)\n"); }
  G2Version ver = G2_VER_GS;

  static uint8_t list[G2_MAX_LIST_SIZE];
  G2Writer w;

  /* (a) the storage silently drops every write to the BANKED box copies. The live copy
   *     of the open box would still look right; the copy the game boots from would not. */
  {
    Mem* m = &g_a;
    if (open_w(&w, m, ver) != G2W_OK) { CHECK(0, "g2w_begin"); return; }
    int cur = w.current_box;
    CHECK_ST(g2w_load_list(&w, cur, list), G2W_OK, "load the open box");
    CHECK_ST(g2w_set_nickname(list, cur, 0, "SABOTAGE"), G2W_OK, "edit it");
    m->sab_lo = 0x4000; m->sab_hi = 0x8000; m->sab_on = true;
    CHECK_ST(g2w_commit_list(&w, cur, list), G2W_ERR_VERIFY,
             "a dropped BANKED-copy write is caught by the gate");
    m->sab_on = false;
  }

  /* (b) the storage silently drops every write below 0x2000 — which, in Crystal, is
   *     exactly the backup mirror. Nothing about the primary block looks wrong. */
  {
    if (!load_real("Crystal.sav", &g_b)) load_fixture(GBF_CRYSTAL, &g_b, 0);
    if (open_w(&w, &g_b, G2_VER_CRYSTAL) != G2W_OK) { CHECK(0, "g2w_begin crystal"); return; }
    CHECK_ST(g2w_load_list(&w, G2_BOX_PARTY, list), G2W_OK, "load the party");
    CHECK_ST(g2w_set_nickname(list, G2_BOX_PARTY, 0, "SABOTAGE"), G2W_OK, "edit it");
    g_b.sab_lo = 0x0000; g_b.sab_hi = 0x2000; g_b.sab_on = true;
    CHECK_ST(g2w_commit_list(&w, G2_BOX_PARTY, list), G2W_ERR_VERIFY,
             "a dropped MIRROR write is caught by the gate");
    g_b.sab_on = false;
  }

  /* (c) a byte rots after a successful commit: g2w_verify must stop saying yes. */
  {
    if (!load_real("Gold.sav", &g_a)) load_fixture(GBF_GS, &g_a, 0);
    if (open_w(&w, &g_a, ver) != G2W_OK) { CHECK(0, "g2w_begin"); return; }
    CHECK_ST(g2w_finish(&w), G2W_OK, "a clean file verifies");
    g_a.buf[0x2500] ^= 0xFF;                     /* inside the checksummed span */
    CHECK_ST(g2w_finish(&w), G2W_OK,
             "...and finishing again repairs the checksums it owns");
    g_a.buf[g2_checksum_primary_off(ver)] ^= 0xFF;
    CHECK_ST(g2w_verify(&w, -1, 0), G2W_ERR_VERIFY,
             "a corrupted stored PRIMARY checksum fails the gate");
    g_a.buf[g2_checksum_primary_off(ver)] ^= 0xFF;                  /* put it back */
    CHECK_ST(g2w_verify(&w, -1, 0), G2W_OK, "...and is fine again once restored");
    /* The backup on its own. The games only consult it when the primary fails, so a
     * gate that settles for primary_ok would happily leave a save carrying a stale
     * second copy — which surfaces as old data the day the primary gets torn. */
    g_a.buf[g2_checksum_backup_off(ver)] ^= 0xFF;
    CHECK_ST(g2w_verify(&w, -1, 0), G2W_ERR_VERIFY,
             "a corrupted stored BACKUP checksum fails the gate too");
  }

  /* (d) the expectation itself is wrong: verify against a list the file does not hold. */
  {
    if (!load_real("Gold.sav", &g_a)) load_fixture(GBF_GS, &g_a, 0);
    if (open_w(&w, &g_a, ver) != G2W_OK) { CHECK(0, "g2w_begin"); return; }
    CHECK_ST(g2w_load_list(&w, 1, list), G2W_OK, "load box 1");
    CHECK_ST(g2w_verify(&w, 1, list), G2W_OK, "verify against the truth passes");
    list[g2_off_record(1, 0) + 0x01] ^= 0xFF;    /* change the held item in the copy */
    CHECK_ST(g2w_verify(&w, 1, list), G2W_ERR_VERIFY,
             "verify against a list the file does NOT hold fails");
  }
}

/* ================================== the bytes this module refuses to let you write === */

static void t_range_guards(void) {
  printf("  -- range guards\n");
  if (!load_real("Crystal.sav", &g_a)) load_fixture(GBF_CRYSTAL, &g_a, GBF_RTC_TAIL_64);
  G2Writer w;
  if (open_w(&w, &g_a, G2_VER_CRYSTAL) != G2W_OK) { CHECK(0, "g2w_begin"); return; }
  uint8_t junk[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  uint32_t before = g_a.writes;

  CHECK_ST(g2w_write_range(&w, G2_SAVE_SIZE, junk, 8), G2W_ERR_RANGE,
           "the RTC tail is not writable");
  CHECK_ST(g2w_write_range(&w, G2_SAVE_SIZE - 4, junk, 8), G2W_ERR_RANGE,
           "a write running off the end of the save is refused");
  CHECK_ST(g2w_write_range(&w, g2_checksum_primary_off(G2_VER_CRYSTAL), junk, 2),
           G2W_ERR_RANGE, "the stored primary checksum is this module's to write");
  CHECK_ST(g2w_write_range(&w, g2_checksum_backup_off(G2_VER_CRYSTAL), junk, 2),
           G2W_ERR_RANGE, "the stored backup checksum is this module's to write");
  CHECK_ST(g2w_write_range(&w, 0x1209, junk, 8), G2W_ERR_RANGE,
           "the backup copy is this module's to maintain, not the caller's");

  /* Pokemon are not reachable by byte poke — that is the one door the structural gate
   * guards, so it must not have a side entrance. */
  G2Offsets go; g2_offsets(G2_VER_CRYSTAL, &go);
  CHECK_ST(g2w_write_range(&w, go.party_list, junk, 8), G2W_ERR_RANGE,
           "the party list is not patchable byte-wise");
  CHECK_ST(g2w_write_range(&w, go.party_list + G2_PARTY_LIST_SIZE - 2, junk, 8), G2W_ERR_RANGE,
           "...not even overlapping its last bytes");
  CHECK_ST(g2w_write_range(&w, go.current_box_list + 500, junk, 8), G2W_ERR_RANGE,
           "the live copy of the open box is not patchable byte-wise");
  CHECK_ST(g2w_write_range(&w, g2_list_offset(&w.sv, 9, -1), junk, 8), G2W_ERR_RANGE,
           "nor is a banked box");
  CHECK_ST(g2w_write_range(&w, go.current_box_no, junk, 1), G2W_ERR_RANGE,
           "nor the current-box number, which would discard a boxful at the next boot");
  CHECK(g_a.writes == before, "not one of those refusals wrote a byte");

  /* The control, so the guard is discriminating rather than a blanket no: the box NAMES,
   * which sit three bytes away from the current-box number, are ordinary header data. */
  CHECK_ST(g2w_write_range(&w, go.box_names, junk, 8), G2W_OK,
           "box names ARE ordinary header data");
  CHECK(g_a.writes > before, "...and that one really did write");

  /* And the control: a legal patch inside the span works and gets mirrored. */
  G2Offsets o; g2_offsets(G2_VER_CRYSTAL, &o);
  uint8_t money[3] = { 0x01, 0x02, 0x03 };
  CHECK_ST(g2w_write_range(&w, o.money, money, 3), G2W_OK, "patching money is allowed");
  CHECK_ST(g2w_finish(&w), G2W_OK, "and finishes clean");
  uint32_t mm = 0;
  CHECK(g2w_mirror_of(G2_VER_CRYSTAL, o.money, &mm), "money is inside the span");
  CHECK(memcmp(g_a.buf + o.money, g_a.buf + mm, 3) == 0, "money was mirrored");
  G2Save sv; g2_detect(g_a.buf, g_a.len, &sv);
  G2Header hd; g2_read_header(g_a.buf, &sv, &hd);
  CHECK(hd.money == 0x010203u, "the reader reads the patched money back");
  assert_healthy(&g_a, G2_VER_CRYSTAL, "after a header patch");
  assert_mirror_exact(&g_a, G2_VER_CRYSTAL, "after a header patch");
}

/* ============================================ list surgery keeps all five in step === */

static void t_list_surgery(void) {
  printf("  -- list surgery (delete / insert / move / swap)\n");
  if (!load_real("Gold.sav", &g_a)) load_fixture(GBF_GS, &g_a, 0);
  G2Writer w;
  if (open_w(&w, &g_a, G2_VER_NONE) != G2W_OK) { CHECK(0, "g2w_begin"); return; }

  static uint8_t list[G2_MAX_LIST_SIZE], before[G2_MAX_LIST_SIZE];
  int box = pick_box(&w, list, 3, false);
  if (box < 0) { printf("     SKIP (no banked box with 3+ Pokemon)\n"); return; }
  int n = g2_list_count(list, box);
  memcpy(before, list, (size_t)g2_list_size(box));

  Snap orig[G2_BOX_CAPACITY];
  for (int i = 0; i < n; i++) CHECK(snap_slot(list, box, i, &orig[i]), "snapshot a slot");

  /* DELETE: everything after the hole moves up by one, nothing else changes, and the
   * count and terminator move together. */
  const int k = 1;
  G2Slot removed;
  CHECK_ST(g2w_get(list, box, k, &removed), G2W_OK, "take the slot out whole");
  CHECK_ST(g2w_delete(list, box, k), G2W_OK, "delete slot 1");
  CHECK(g2_list_count(list, box) == n - 1, "the count went down by exactly one");
  CHECK(list[1 + n - 1] == G2_LIST_TERMINATOR, "the terminator moved with it");
  for (int i = 0; i < n - 1; i++) {
    Snap s;
    CHECK(snap_slot(list, box, i, &s), "every surviving slot still decodes");
    CHECK(snap_eq(&s, &orig[i < k ? i : i + 1]),
          "each survivor is EXACTLY the Pokemon it was, one place up");
  }

  /* INSERT it back where it was: the list must decode identically to the original. */
  CHECK_ST(g2w_insert(list, box, k, &removed), G2W_OK, "put it back");
  CHECK(g2_list_count(list, box) == n, "the count came back");
  for (int i = 0; i < n; i++) {
    Snap s;
    CHECK(snap_slot(list, box, i, &s) && snap_eq(&s, &orig[i]),
          "delete-then-insert is lossless for every Pokemon in the box");
  }
  /* Bytes past the count legitimately differ (the games leave the vacated slot's stale
   * copy behind and so do we), so the assertion above is on decoded content, not memcmp.
   * The occupied part, though, has to be identical. */
  CHECK(memcmp(list, before, (size_t)(2 + g2_list_capacity(box))) == 0,
        "count + species list restored byte-for-byte");
  CHECK(memcmp(list + g2_off_record(box, 0), before + g2_off_record(box, 0),
               (size_t)(n * g2_list_entry_size(box))) == 0,
        "the occupied records restored byte-for-byte");

  /* MOVE and SWAP. */
  CHECK_ST(g2w_move_slot(list, box, 0, 2), G2W_OK, "move slot 0 to 2");
  { Snap s;
    CHECK(snap_slot(list, box, 2, &s) && snap_eq(&s, &orig[0]), "the moved mon is at 2");
    CHECK(snap_slot(list, box, 0, &s) && snap_eq(&s, &orig[1]), "the others closed up"); }
  CHECK_ST(g2w_move_slot(list, box, 2, 0), G2W_OK, "move it back");
  for (int i = 0; i < n; i++) {
    Snap s;
    CHECK(snap_slot(list, box, i, &s) && snap_eq(&s, &orig[i]), "move round-trips");
  }
  CHECK_ST(g2w_swap_slots(list, box, 0, n - 1), G2W_OK, "swap the ends");
  { Snap s;
    CHECK(snap_slot(list, box, 0, &s)     && snap_eq(&s, &orig[n - 1]), "swap moved one end");
    CHECK(snap_slot(list, box, n - 1, &s) && snap_eq(&s, &orig[0]),     "swap moved the other"); }
  CHECK_ST(g2w_swap_slots(list, box, 0, n - 1), G2W_OK, "swap back");
  CHECK(memcmp(list, before, (size_t)g2_list_size(box)) == 0,
        "a swap and its inverse are byte-identical");

  /* A full box takes nothing more. */
  while (g2_list_count(list, box) < g2_list_capacity(box)) {
    G2WStatus st = g2w_append(list, box, &removed, 0);
    if (st != G2W_OK) { CHECK_ST(st, G2W_OK, "filling the box"); break; }
  }
  CHECK_ST(g2w_append(list, box, &removed, 0), G2W_ERR_FULL, "a full box refuses one more");
  CHECK_ST(g2w_check_list(list, box), G2W_OK, "a completely full box is still well formed");
  CHECK_ST(g2w_commit_list(&w, box, list), G2W_OK, "and commits");
  assert_healthy(&g_a, w.sv.version, "after filling a box");

  /* Emptying it right down to zero has to work too — the count byte and the terminator
   * at [1] are the whole of an empty box, and getting that wrong is how a box turns into
   * twenty glitch Pokemon. */
  while (g2_list_count(list, box) > 0)
    if (g2w_delete(list, box, 0) != G2W_OK) { CHECK(0, "emptying the box"); break; }
  CHECK(g2_list_count(list, box) == 0, "the box is empty");
  CHECK(list[1] == G2_LIST_TERMINATOR, "an empty box terminates at [1]");
  CHECK_ST(g2w_check_list(list, box), G2W_OK, "an empty box is well formed");
  CHECK_ST(g2w_commit_list(&w, box, list), G2W_OK, "an empty box commits");
  assert_healthy(&g_a, w.sv.version, "after emptying a box");
  CHECK_ST(g2w_delete(list, box, 0), G2W_ERR_EMPTY, "deleting from an empty box is refused");
}

/* ================== a failed list op leaves the caller's buffer byte-identical ===== */

/* THE most important section in this file, and the least obvious.
 *
 * Everywhere else, "the write went wrong" means the save ends up damaged, and damage is
 * something a gate can see. Not here. A list op that shifts the five parallel structures
 * and then fails partway does not leave a broken box — it leaves a perfectly WELL-FORMED
 * box that is simply missing a Pokemon, with its neighbour duplicated into the hole.
 * g2w_check_list passes it. g2w_commit_list writes it. g2w_verify re-reads it, re-parses
 * it, compares both copies and both stored checksums, and reports success, because there
 * is nothing wrong with it. It just is not the box the player had.
 *
 * No amount of verification downstream can ever catch that, so the property has to hold
 * where the bytes move: after ANY failed call, the caller's staged list is byte-identical
 * to what they passed in. Every assertion below is that memcmp.
 *
 * The failures are driven with real Pokemon out of Guy's cartridge save, glitched the way
 * a Game Boy actually glitches them (species 252, past the 251 the games have), and with
 * lists that are walkable but not well formed — because the two up-front checks catch
 * different things and both are load-bearing: slot_check() rejects a bad ARGUMENT, and
 * survivors_ok() rejects a list that is walkable enough for the op to start but holds a
 * slot the finished list would be refused for, somewhere the op is not even touching.
 * The second is what used to be caught by a 1102-byte shadow copy, after the fact;
 * gen2_write.c's THE PROOF is why it is now decidable before the first byte moves, and
 * these tests are its evidence. Removing either check loses Pokemon. */

typedef struct {
  uint8_t saved[G2_MAX_LIST_SIZE];
  Snap    mon[G2_BOX_CAPACITY];
  /* Some of the lists driven through here hold a slot the READER refuses (species 0xFF,
   * species 0) — that is the point of them. g2_list_mon returns false for those, so the
   * census has to remember which slots it could actually snapshot and hold the rest to
   * "still undecodable" rather than to a Snap it never took. */
  bool    decoded[G2_BOX_CAPACITY];
  int     box, n;
} Guard;

static void guard_arm(Guard* g, const uint8_t* list, int box) {
  g->box = box;
  g->n   = g2_list_count(list, box);
  memcpy(g->saved, list, (size_t)g2_list_size(box));
  for (int i = 0; i < g->n && i < G2_BOX_CAPACITY; i++)
    g->decoded[i] = snap_slot(list, box, i, &g->mon[i]);
}

/* Two assertions, not one: the memcmp is the real guarantee, and the census is there so
 * that when it does fail, the output says WHAT was lost rather than just "bytes differ". */
static void guard_intact(const Guard* g, const uint8_t* list, const char* what) {
  char msg[200];
  snprintf(msg, sizeof msg, "%s: the caller's list is byte-identical", what);
  CHECK(memcmp(list, g->saved, (size_t)g2_list_size(g->box)) == 0, msg);

  snprintf(msg, sizeof msg, "%s: the count did not move", what);
  CHECK(g2_list_count(list, g->box) == g->n, msg);
  for (int i = 0; i < g->n && i < G2_BOX_CAPACITY; i++) {
    Snap s;
    bool now = snap_slot(list, g->box, i, &s);
    snprintf(msg, sizeof msg, "%s: slot %d is still the same Pokemon (no delete+clone)",
             what, i);
    CHECK(now == g->decoded[i] && (!now || snap_eq(&s, &g->mon[i])), msg);
  }
}

/* A box with room to grow and enough Pokemon to reorder. Prefers the real corpus and
 * falls back to the fixture's box 0 (ten mons, capacity twenty). */
static int pick_editable_box(G2Writer* w, uint8_t* list) {
  return pick_box(w, list, 4, true);
}

static void t_atomicity(void) {
  printf("  -- failed list ops leave the caller's buffer untouched\n");
  if (!load_real("Gold.sav", &g_a)) {
    load_fixture(GBF_GS, &g_a, 0);
    printf("     (using the synthetic G/S image — corpus not present)\n");
  }
  G2Writer w;
  if (open_w(&w, &g_a, G2_VER_NONE) != G2W_OK) { CHECK(0, "g2w_begin"); return; }

  static uint8_t list[G2_MAX_LIST_SIZE], broken[G2_MAX_LIST_SIZE];
  static Guard gd;
  int box = pick_editable_box(&w, list);
  if (box < 0) { printf("     SKIP (no box with 4..cap-1 Pokemon)\n"); return; }
  int n = g2_list_count(list, box), size = g2_list_size(box);
  printf("     box %d, %d/%d occupied\n", box, n, g2_list_capacity(box));

  /* A real Pokemon out of the save, with the one byte a GB glitch corrupts. Species 252
   * is past the 251 the games have; the record is otherwise a genuine, valid mon, which
   * is exactly what makes it a good driver — nothing else about it is wrong. */
  G2Slot glitch;
  CHECK_ST(g2w_get(list, box, 0, &glitch), G2W_OK, "take a real Pokemon out whole");
  glitch.rec[0x00] = 0xFC;

  G2Slot good;
  CHECK_ST(g2w_get(list, box, 1, &good), G2W_OK, "and a second one, left valid");

  /* ---- INSERT: every reason it can refuse, and none of them may move a byte ---- */

  {
    G2Slot hole = good;      hole.rec[0x00] = 0;                 /* species 0 = a hole  */
    G2Slot noterm = good;    memset(noterm.nickname, 0x80, 11);  /* no 0x50 anywhere    */
    G2Slot notermot = good;  memset(notermot.otname, 0x80, 11);
    G2Slot wrongkind = good; wrongkind.is_party = true;          /* party mon into a box */
    G2Slot ffspecies = good; ffspecies.rec[0x00] = 0xFF;         /* 0xFF is the terminator */

    struct { const char* why; const G2Slot* in; int at; G2WStatus want; } cases[] = {
      { "a glitch species (252) from outside the list", &glitch,    1, G2W_ERR_CONTENT },
      { "a species byte of 0xFF (the terminator)",      &ffspecies, 1, G2W_ERR_CONTENT },
      { "species 0",                                    &hole,      1, G2W_ERR_STRUCT  },
      { "a nickname with no 0x50 terminator",           &noterm,    1, G2W_ERR_STRUCT  },
      { "an OT name with no 0x50 terminator",           &notermot,  1, G2W_ERR_STRUCT  },
      { "a party record aimed at a box",                &wrongkind, 1, G2W_ERR_STATS   },
      { "an index past the count",                      &good,  n + 1, G2W_ERR_ARG     },
      { "a negative index",                             &good,     -1, G2W_ERR_ARG     },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
      char msg[200];
      guard_arm(&gd, list, box);
      snprintf(msg, sizeof msg, "insert refuses %s", cases[i].why);
      CHECK_ST(g2w_insert(list, box, cases[i].at, cases[i].in), cases[i].want, msg);
      snprintf(msg, sizeof msg, "insert refusing %s", cases[i].why);
      guard_intact(&gd, list, msg);
    }
  }

  /* A full box, which is the one refusal the old code already got right — kept so the
   * fix cannot regress it. */
  {
    static uint8_t full[G2_MAX_LIST_SIZE];
    memcpy(full, list, (size_t)size);
    while (g2_list_count(full, box) < g2_list_capacity(box))
      if (g2w_append(full, box, &good, 0) != G2W_OK) break;
    guard_arm(&gd, full, box);
    CHECK_ST(g2w_insert(full, box, 0, &good), G2W_ERR_FULL, "insert refuses a full box");
    guard_intact(&gd, full, "insert into a full box");
  }

  /* ---- APPEND inherits all of it, and must not report a slot it never created ---- */
  {
    int landed = -12345;
    guard_arm(&gd, list, box);
    CHECK_ST(g2w_append(list, box, &glitch, &landed), G2W_ERR_CONTENT,
             "append refuses a glitch species");
    guard_intact(&gd, list, "append refusing a glitch species");
    CHECK(landed == -12345, "a failed append does not report a slot it never created");
  }

  /* ---- MOVE and SWAP: relocating what is already there ------------------------
   * The old code built these out of delete-then-insert / put-then-put on the caller's
   * own buffer, so a mon the second half would not take had ALREADY been removed or
   * overwritten by the time anyone found out. Two things have to be true now: the
   * relocation of a glitch Pokemon SUCCEEDS (it is already in the box; refusing to
   * reorder a box because of it would be the uselessness the header rejects), and every
   * refusal that does happen leaves the buffer alone. */
  {
    static uint8_t g[G2_MAX_LIST_SIZE];
    memcpy(g, list, (size_t)size);
    g[g2_off_record(box, 1)] = 0xFC;      /* plant the glitch mon IN the list, both */
    g[1 + 1]                 = 0xFC;      /* places, so the list stays well formed  */
    CHECK_ST(g2w_check_list(g, box), G2W_OK, "a box holding a glitch mon is still walkable");
    CHECK_ST(g2w_check_list_strict(g, box), G2W_ERR_CONTENT, "...and strict-checks as content");

    Snap was[G2_BOX_CAPACITY];
    int have = g2_list_count(g, box);
    for (int i = 0; i < have; i++) snap_slot(g, box, i, &was[i]);

    CHECK_ST(g2w_move_slot(g, box, 1, 3), G2W_OK,
             "a glitch Pokemon CAN be moved (the header's promise)");
    CHECK(g[g2_off_record(box, 3)] == 0xFC && g[1 + 3] == 0xFC,
          "...and it arrived, species byte and list byte together");
    CHECK(g2_list_count(g, box) == have, "...without changing the count");
    /* The specific damage the old code did: one gone, a neighbour cloned. */
    int survivors = 0;
    for (int i = 0; i < have; i++) {
      Snap s;
      if (!snap_slot(g, box, i, &s)) continue;
      for (int j = 0; j < have; j++) if (snap_eq(&s, &was[j])) { survivors++; break; }
    }
    CHECK(survivors >= have - 1, "no Pokemon was eaten by the move");

    CHECK_ST(g2w_swap_slots(g, box, 3, 0), G2W_OK, "and swapped");
    CHECK(g[g2_off_record(box, 0)] == 0xFC, "...landing where it was asked to");
  }

  /* The failure survivors_ok() exists for: a list that is walkable (so g2_list_count is
   * happy and the op starts) but not well formed somewhere the op is not even touching.
   * Checking only the op's own ARGUMENTS cannot see it — this is the half that used to
   * need a shadow copy, and it is only decidable in advance because the gate's per-slot
   * test does not depend on where the slot sits. */
  {
    memcpy(broken, list, (size_t)size);
    /* Slot 3's species-list byte no longer agrees with its record. The box menu would
     * show one Pokemon and the box would contain another. */
    broken[1 + 3] = (uint8_t)(broken[g2_off_record(box, 3)] == 25 ? 26 : 25);
    CHECK(g2_list_count(broken, box) == n, "the sabotaged list is still walkable");
    CHECK_ST(g2w_check_list(broken, box), G2W_ERR_STRUCT, "...but not well formed");

    struct { const char* what; int a, b; } ops[] = { { "move", 0, 2 }, { "swap", 0, 1 } };
    for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; i++) {
      char msg[200];
      guard_arm(&gd, broken, box);
      G2WStatus st = i == 0 ? g2w_move_slot(broken, box, ops[i].a, ops[i].b)
                            : g2w_swap_slots(broken, box, ops[i].a, ops[i].b);
      snprintf(msg, sizeof msg, "%s refuses a list that fails the gate elsewhere", ops[i].what);
      CHECK_ST(st, G2W_ERR_STRUCT, msg);
      snprintf(msg, sizeof msg, "%s on a list broken elsewhere", ops[i].what);
      guard_intact(&gd, broken, msg);
    }
    guard_arm(&gd, broken, box);
    CHECK_ST(g2w_insert(broken, box, 0, &good), G2W_ERR_STRUCT,
             "insert refuses a list that fails the gate elsewhere");
    guard_intact(&gd, broken, "insert into a list broken elsewhere");

    guard_arm(&gd, broken, box);
    CHECK_ST(g2w_delete(broken, box, 0), G2W_ERR_STRUCT,
             "delete refuses a list that fails the gate elsewhere");
    guard_intact(&gd, broken, "delete from a list broken elsewhere");

    /* ...and the exemption that keeps such a box repairable: the slot that IS broken can
     * still be deleted. That is the whole reason survivors_ok() takes a `skip` — an entry
     * too damaged to keep is exactly the one the user needs to be able to remove, and a
     * pre-flight that checked the WHOLE list would take that away. */
    CHECK_ST(g2w_delete(broken, box, 3), G2W_OK, "the broken slot itself CAN be deleted");
    CHECK(g2_list_count(broken, box) == n - 1, "...the count went down by one");
    CHECK_ST(g2w_check_list(broken, box), G2W_OK, "...and deleting it repaired the box");
  }

  /* A species byte of 0xFF, in both places so the list stays walkable. The old code could
   * not see this before the shift even in principle: it is not a content violation (the
   * content gate is off for a resident slot being reordered) and the old slot_check only
   * looked for species 0 and the name terminators. Only the gate on the finished list
   * objected — by which time the finished list was the caller's. slot_wellformed() sees
   * it, because 0xFF in a species byte IS the list terminator. */
  {
    memcpy(broken, list, (size_t)size);
    broken[1 + 2]                 = 0xFF;
    broken[g2_off_record(box, 2)] = 0xFF;
    CHECK(g2_list_count(broken, box) == n, "a 0xFF species byte leaves the list walkable");
    CHECK_ST(g2w_check_list(broken, box), G2W_ERR_STRUCT, "...but not well formed");

    guard_arm(&gd, broken, box);
    CHECK_ST(g2w_move_slot(broken, box, 2, 0), G2W_ERR_STRUCT,
             "move refuses a slot whose species byte is the terminator");
    guard_intact(&gd, broken, "move of a 0xFF-species slot");
    guard_arm(&gd, broken, box);
    CHECK_ST(g2w_swap_slots(broken, box, 2, 0), G2W_ERR_STRUCT,
             "swap refuses a slot whose species byte is the terminator");
    guard_intact(&gd, broken, "swap of a 0xFF-species slot");
    guard_arm(&gd, broken, box);
    CHECK_ST(g2w_insert(broken, box, 0, &good), G2W_ERR_STRUCT,
             "insert refuses a list holding a 0xFF-species slot");
    guard_intact(&gd, broken, "insert into a list with a 0xFF-species slot");
    CHECK_ST(g2w_delete(broken, box, 2), G2W_OK, "...and it, too, can be deleted");
    CHECK_ST(g2w_check_list(broken, box), G2W_OK, "...which repairs the box");
  }

  /* Move and swap also refuse a slot whose own record is unusable, before moving it. */
  {
    memcpy(broken, list, (size_t)size);
    memset(broken + g2_off_nickname(box, 2), 0x80, 11);     /* no terminator */
    guard_arm(&gd, broken, box);
    CHECK_ST(g2w_move_slot(broken, box, 2, 0), G2W_ERR_STRUCT,
             "move refuses a slot with an unterminated nickname");
    guard_intact(&gd, broken, "move of a slot with an unterminated nickname");
    guard_arm(&gd, broken, box);
    CHECK_ST(g2w_swap_slots(broken, box, 2, 0), G2W_ERR_STRUCT,
             "swap refuses a slot with an unterminated nickname");
    guard_intact(&gd, broken, "swap of a slot with an unterminated nickname");
  }

  /* Bad arguments to move/swap/delete: refused, nothing moved. */
  {
    struct { const char* why; int a, b; } bad[] = {
      { "a source past the count", n,  0 },
      { "a target past the count", 0,  n },
      { "a negative source",      -1,  0 },
      { "a negative target",       0, -1 },
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
      char msg[200];
      guard_arm(&gd, list, box);
      snprintf(msg, sizeof msg, "move refuses %s", bad[i].why);
      CHECK_ST(g2w_move_slot(list, box, bad[i].a, bad[i].b), G2W_ERR_ARG, msg);
      guard_intact(&gd, list, msg);
      snprintf(msg, sizeof msg, "swap refuses %s", bad[i].why);
      CHECK_ST(g2w_swap_slots(list, box, bad[i].a, bad[i].b), G2W_ERR_ARG, msg);
      guard_intact(&gd, list, msg);
    }
    guard_arm(&gd, list, box);
    CHECK_ST(g2w_move_slot(0, box, 0, 1), G2W_ERR_ARG, "move refuses a NULL list");
    CHECK_ST(g2w_swap_slots(0, box, 0, 1), G2W_ERR_ARG, "swap refuses a NULL list");
    CHECK_ST(g2w_delete(0, box, 0), G2W_ERR_ARG, "delete refuses a NULL list");
    CHECK_ST(g2w_insert(0, box, 0, &good), G2W_ERR_ARG, "insert refuses a NULL list");
    CHECK_ST(g2w_insert(list, box, 0, 0), G2W_ERR_ARG, "insert refuses a NULL slot");
    guard_intact(&gd, list, "the NULL-argument refusals");
    CHECK_ST(g2w_delete(list, box, n), G2W_ERR_EMPTY, "delete refuses an empty slot");
    guard_intact(&gd, list, "delete of an empty slot");
  }

  /* ---- PUT: no shadow copy, so its atomicity rests entirely on validating first ---- */
  {
    G2Slot hole = good;   hole.rec[0x00] = 0;
    G2Slot noterm = good; memset(noterm.nickname, 0x80, 11);
    struct { const char* why; const G2Slot* in; G2WStatus want; } cases[] = {
      { "a glitch species from outside",      &glitch, G2W_ERR_CONTENT },
      { "species 0",                          &hole,   G2W_ERR_STRUCT  },
      { "an unterminated nickname",           &noterm, G2W_ERR_STRUCT  },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
      char msg[200];
      guard_arm(&gd, list, box);
      snprintf(msg, sizeof msg, "put refuses %s", cases[i].why);
      CHECK_ST(g2w_put(list, box, 1, cases[i].in), cases[i].want, msg);
      guard_intact(&gd, list, msg);
    }
    /* And the proof that the unterminated-name check is load-bearing rather than
     * decorative: without it the put would "succeed" and hand the caller a list its own
     * gate refuses. */
    guard_arm(&gd, list, box);
    CHECK_ST(g2w_put(list, box, 1, &good), G2W_OK, "put still takes a valid slot");
    CHECK_ST(g2w_check_list(list, box), G2W_OK, "and what it leaves behind passes the gate");
    memcpy(list, gd.saved, (size_t)size);
  }

  /* ---- set_move: the move id must not be stored before pp/pp_ups are checked ---- */
  {
    struct { const char* why; int i; uint8_t pp, ups; } bad[] = {
      { "pp above 63 (the field is 6 bits)", 0, 64, 0 },
      { "pp 255",                            1, 255, 0 },
      { "pp_ups above 3 (the field is 2 bits)", 2, 10, 4 },
      { "a negative move index",            -1, 10, 0 },
      { "a move index of 4",                 4, 10, 0 },
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
      char msg[200];
      guard_arm(&gd, list, box);
      /* The move id has to DIFFER from what the slot already holds, or the assertion
       * below is vacuous: storing 33 over an existing 33 changes no byte, and the buggy
       * ordering would sail through a test that looked like it was checking something.
       * (Found exactly that way — the first driver here was silently passing against the
       * reintroduced bug because box 12 slot 0's first move really is TACKLE.) */
      int mi = bad[i].i >= 0 && bad[i].i <= 3 ? bad[i].i : 0;
      uint8_t cur = list[g2_off_record(box, 0) + 0x02 + mi];
      uint8_t mv  = (uint8_t)(cur == 33 ? 34 : 33);
      snprintf(msg, sizeof msg, "set_move refuses %s", bad[i].why);
      CHECK_ST(g2w_set_move(list, box, 0, bad[i].i, mv, bad[i].pp, bad[i].ups),
               G2W_ERR_ARG, msg);
      /* The old code had already stored the move id by this point, leaving a Pokemon
       * with the wrong move and a PP byte still describing the old one — from a call
       * that reported failure. */
      snprintf(msg, sizeof msg, "set_move refusing %s stored nothing", bad[i].why);
      CHECK(memcmp(list, gd.saved, (size_t)size) == 0, msg);
      guard_intact(&gd, list, msg);
    }
    /* The control: a legal call still works, so the hoisted check is not just refusing
     * everything. */
    guard_arm(&gd, list, box);
    CHECK_ST(g2w_set_move(list, box, 0, 0, 33, 35, 3), G2W_OK, "set_move still takes a legal move");
    CHECK(list[g2_off_record(box, 0) + 0x02] == 33, "...and stored the move");
    CHECK(list[g2_off_record(box, 0) + 0x17] == (uint8_t)((3 << 6) | 35),
          "...with the PP and PP Ups packed into one byte");
    memcpy(list, gd.saved, (size_t)size);
  }

  /* The save itself was never a party to any of this. */
  CHECK(g_a.writes == 0, "not one byte reached the file — these are pure list ops");
  assert_healthy(&g_a, w.sv.version, "after a run of refused list ops");
}

/* ======================== the proof's conclusion, stated over every index there is ==== */

/* The named cases above are the interesting ones; this is the whole claim, exhaustively.
 * For every list op, at every index, on a clean list AND on the same list with each kind
 * of structural damage planted at each of several slots, exactly one of two things has to
 * happen:
 *
 *     it returned G2W_OK  and the list now passes the structural gate, or
 *     it refused          and the list is byte-identical to before the call.
 *
 * Nothing else — never "refused but shifted", never "succeeded into a list its own gate
 * would reject". gen2_write.c's THE PROOF argues that from the code; this measures it,
 * and it is deliberately blunt about how, because an argument nobody has run over 2000
 * index combinations is an argument.
 *
 * The sabotages all leave the list WALKABLE (g2_list_count still returns the same count)
 * while failing g2w_check_list. That is the specific shape of damage that used to need a
 * shadow copy to detect: the op happily starts, and only the finished article is wrong. */

typedef enum { SAB_LISTBYTE, SAB_NICK, SAB_OT, SAB_FF, SAB_ZERO, SAB_NONE } Sabotage;

static void sabotage(uint8_t* list, int box, int j, Sabotage k) {
  switch (k) {
    /* the menu's species cache no longer names the Pokemon in the record */
    case SAB_LISTBYTE: list[1 + j] = (uint8_t)(list[g2_off_record(box, j)] == 25 ? 26 : 25);
                       break;
    case SAB_NICK: memset(list + g2_off_nickname(box, j), 0x80, 11); break;
    case SAB_OT:   memset(list + g2_off_otname(box, j),   0x80, 11); break;
    /* a second 0xFF inside the occupied range: the game's list walk stops early */
    case SAB_FF:   list[1 + j] = 0xFF; list[g2_off_record(box, j)] = 0xFF; break;
    /* a hole in the middle of the list */
    case SAB_ZERO: list[1 + j] = 0;    list[g2_off_record(box, j)] = 0;    break;
    case SAB_NONE: break;
  }
}

static int g_sweep_ok, g_sweep_refused;

static void sweep_check(G2WStatus st, const uint8_t* list, const uint8_t* orig, int box,
                        const char* what) {
  char msg[280];
  if (st == G2W_OK) {
    g_sweep_ok++;
    snprintf(msg, sizeof msg, "%s: OK, so the result must pass the gate", what);
    CHECK(g2w_check_list(list, box) == G2W_OK, msg);
  } else {
    g_sweep_refused++;
    snprintf(msg, sizeof msg, "%s: refused (%s), so the list must be byte-identical",
             what, g2w_status_text(st));
    CHECK(memcmp(list, orig, (size_t)g2_list_size(box)) == 0, msg);
  }
}

static void t_preflight_total(void) {
  printf("  -- every op x every index x every sabotage: OK-and-well-formed, or untouched\n");
  if (!load_real("Gold.sav", &g_a)) {
    load_fixture(GBF_GS, &g_a, 0);
    printf("     (using the synthetic G/S image — corpus not present)\n");
  }
  G2Writer w;
  if (open_w(&w, &g_a, G2_VER_NONE) != G2W_OK) { CHECK(0, "g2w_begin"); return; }

  static uint8_t list[G2_MAX_LIST_SIZE], orig[G2_MAX_LIST_SIZE], clean[G2_MAX_LIST_SIZE];
  int targets[2], nt = 0;
  int b = pick_box(&w, clean, 4, true);
  if (b >= 0) targets[nt++] = b;
  targets[nt++] = G2_BOX_PARTY;      /* 48-byte records, capacity 6: different geometry */

  g_sweep_ok = g_sweep_refused = 0;
  for (int ti = 0; ti < nt; ti++) {
    int box = targets[ti];
    if (g2w_load_list(&w, box, clean) != G2W_OK) continue;
    if (g2w_check_list(clean, box) != G2W_OK) continue;
    int n = g2_list_count(clean, box), size = g2_list_size(box);
    if (n < 3) { printf("     box %d: only %d Pokemon, skipped\n", box, n); continue; }
    G2Slot in;
    if (g2w_get(clean, box, 0, &in) != G2W_OK) continue;

    int victims[4] = { 0, 1, n / 2, n - 1 };
    for (int ki = 0; ki <= SAB_NONE; ki++) {
      for (int vi = 0; vi < 4; vi++) {
        int j = victims[vi];
        if (ki == SAB_NONE && vi) break;              /* undamaged needs no victim */
        memcpy(orig, clean, (size_t)size);
        sabotage(orig, box, j, (Sabotage)ki);

        char what[240];
        snprintf(what, sizeof what, "box %d sabotage %d at slot %d stays walkable", box, ki, j);
        CHECK(g2_list_count(orig, box) == n, what);
        if (ki != SAB_NONE) {
          snprintf(what, sizeof what, "box %d sabotage %d at slot %d really breaks the gate",
                   box, ki, j);
          CHECK(g2w_check_list(orig, box) == G2W_ERR_STRUCT, what);
        }

        for (int s = 0; s < n; s++) {
          memcpy(list, orig, (size_t)size);
          snprintf(what, sizeof what, "box %d sab %d@%d delete %d", box, ki, j, s);
          sweep_check(g2w_delete(list, box, s), list, orig, box, what);
          /* The `skip` exemption, at every index: removing the damaged slot is allowed
           * however damaged it is, because that is the repair. */
          if (ki != SAB_NONE && s == j) {
            snprintf(what, sizeof what,
                     "box %d sab %d@%d: deleting the damaged slot repairs the list", box, ki, j);
            CHECK(g2_list_count(list, box) == n - 1 &&
                  g2w_check_list(list, box) == G2W_OK, what);
          }
        }
        for (int s = 0; s <= n; s++) {
          memcpy(list, orig, (size_t)size);
          snprintf(what, sizeof what, "box %d sab %d@%d insert at %d", box, ki, j, s);
          sweep_check(g2w_insert(list, box, s, &in), list, orig, box, what);
        }
        for (int s = 0; s < n; s++) {
          memcpy(list, orig, (size_t)size);
          snprintf(what, sizeof what, "box %d sab %d@%d move 0->%d", box, ki, j, s);
          sweep_check(g2w_move_slot(list, box, 0, s), list, orig, box, what);
          memcpy(list, orig, (size_t)size);
          snprintf(what, sizeof what, "box %d sab %d@%d move %d->0", box, ki, j, s);
          sweep_check(g2w_move_slot(list, box, s, 0), list, orig, box, what);
          memcpy(list, orig, (size_t)size);
          snprintf(what, sizeof what, "box %d sab %d@%d swap 0<->%d", box, ki, j, s);
          sweep_check(g2w_swap_slots(list, box, 0, s), list, orig, box, what);
        }
      }
    }
  }
  printf("     %d calls returned OK with a well-formed list, %d refused without moving a byte\n",
         g_sweep_ok, g_sweep_refused);
  CHECK(g_sweep_ok > 0 && g_sweep_refused > 0,
        "the sweep exercised both outcomes (a sweep that only ever refuses proves nothing)");
  CHECK(g_a.writes == 0, "and no list op touched the file");
}

/* ==================================== the party's mail array is not ours to reshuffle */

static void t_party_mail_guard(void) {
  printf("  -- party restructure guard\n");
  if (!load_real("Crystal.sav", &g_a)) load_fixture(GBF_CRYSTAL, &g_a, 0);
  G2Writer w;
  if (open_w(&w, &g_a, G2_VER_CRYSTAL) != G2W_OK) { CHECK(0, "g2w_begin"); return; }

  static uint8_t list[G2_MAX_LIST_SIZE];
  if (g2w_load_list(&w, G2_BOX_PARTY, list) != G2W_OK) { CHECK(0, "load party"); return; }
  int n = g2_list_count(list, G2_BOX_PARTY);
  if (n < 2) { printf("     SKIP (party of %d)\n", n); return; }

  /* An in-place field edit keeps every slot where it is, so mail stays attached to the
   * right Pokemon and no acknowledgement is needed. */
  CHECK_ST(g2w_set_friendship(list, G2_BOX_PARTY, 0, 200), G2W_OK, "edit a party field");
  CHECK_ST(g2w_commit_list(&w, G2_BOX_PARTY, list), G2W_OK,
           "an in-place party edit needs no acknowledgement");

  /* Changing the slot layout does. */
  CHECK_ST(g2w_delete(list, G2_BOX_PARTY, 0), G2W_OK, "delete a party member (in memory)");
  uint32_t before = g_a.writes;
  CHECK_ST(g2w_commit_list(&w, G2_BOX_PARTY, list), G2W_ERR_PARTY_MAIL,
           "reshuffling the party without acknowledging held Mail is refused");
  CHECK(g_a.writes == before, "...and nothing was written");

  w.party_mail_ack = true;
  CHECK_ST(g2w_commit_list(&w, G2_BOX_PARTY, list), G2W_OK,
           "...and goes through once the caller has taken responsibility");
  G2Save sv; g2_detect(g_a.buf, g_a.len, &sv);
  G2Header hd; g2_read_header(g_a.buf, &sv, &hd);
  CHECK(g2_box_count_at(g_a.buf, &sv, &hd, G2_BOX_PARTY) == n - 1,
        "the reader sees the shorter party");
  assert_healthy(&g_a, G2_VER_CRYSTAL, "after a party delete");
  assert_mirror_exact(&g_a, G2_VER_CRYSTAL, "after a party delete");
}

/* ============================================================== text encoding ===== */

static void t_text(void) {
  printf("  -- Gen-2 text encoding\n");
  static const char* ok[] = {
    "PIKACHU", "FARFETCH'D", "MR.MIME", "NIDORAN\xE2\x99\x82", "NIDORAN\xE2\x99\x80",
    "A B-C", "abc XYZ 09", "HO-OH", "(A):[B];", "IT'S 100$", "a,b/c&d", "WHAT?!",
  };
  uint8_t field[11];
  char back[G2_NAME_BYTES];
  for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++) {
    int glyphs = g2w_encode_text(field, 11, ok[i]);
    char msg[128];
    snprintf(msg, sizeof msg, "encodes: %s", ok[i]);
    CHECK(glyphs > 0, msg);
    g2_decode_text(field, 10, back, sizeof back);
    snprintf(msg, sizeof msg, "round-trips: %s -> %s", ok[i], back);
    CHECK(strcmp(back, ok[i]) == 0, msg);
    /* The whole field is 0x50-padded, the way the naming screen leaves it. */
    for (int k = glyphs; k < 11; k++) CHECK(field[k] == 0x50, "the field is 0x50-padded");
  }
  /* GROUND TRUTH, straight off Guy's Gold cartridge: the three species names that stress
   * the charset. The apostrophe in FARFETCH'D is the PLAIN 0xE0 followed by an uppercase
   * D, not the 0xD0 "'d" contraction — the contraction is lowercase and the games do not
   * use it here, which is exactly the sort of thing a wiki gets backwards. The encoder
   * has to reproduce these byte for byte. */
  struct { const char* name; uint8_t raw[11]; } truth[] = {
    { "FARFETCH'D", { 0x85,0x80,0x91,0x85,0x84,0x93,0x82,0x87,0xE0,0x83,0x50 } },
    { "MR.MIME",    { 0x8C,0x91,0xE8,0x8C,0x88,0x8C,0x84,0x50,0x50,0x50,0x50 } },
    { "NIDORAN\xE2\x99\x80", { 0x8D,0x88,0x83,0x8E,0x91,0x80,0x8D,0xF5,0x50,0x50,0x50 } },
    { "NIDORAN\xE2\x99\x82", { 0x8D,0x88,0x83,0x8E,0x91,0x80,0x8D,0xEF,0x50,0x50,0x50 } },
  };
  for (unsigned i = 0; i < sizeof truth / sizeof truth[0]; i++) {
    char msg[128];
    g2w_encode_text(field, 11, truth[i].name);
    snprintf(msg, sizeof msg, "encodes %s exactly as the cartridge stores it", truth[i].name);
    CHECK(memcmp(field, truth[i].raw, 11) == 0, msg);
  }
  CHECK(g2w_text_glyphs("FARFETCH'D") == 10, "FARFETCH'D is 10 glyphs, and only just fits");
  /* The contraction glyphs are lowercase and DO collapse two characters into one. */
  CHECK(g2w_text_glyphs("you'd") == 4, "the 'd contraction counts as one glyph");
  g2w_encode_text(field, 11, "you'd");
  CHECK(field[3] == 0xD0, "...and is stored as the single 0xD0 glyph");
  g2_decode_text(field, 10, back, sizeof back);
  CHECK(strcmp(back, "you'd") == 0, "...and decodes straight back");
  CHECK(g2w_text_glyphs("NIDORAN\xE2\x99\x82") == 8, "a gender sign counts as one glyph");

  static const char* nope[] = { "caf\xC3\xA9", "A#B", "TAB\tX", "\xE2\x98\x85", "a+b", "%", };
  for (unsigned i = 0; i < sizeof nope / sizeof nope[0]; i++) {
    char msg[128];
    snprintf(msg, sizeof msg, "refuses unrepresentable input #%u", i);
    CHECK(g2w_encode_text(field, 11, nope[i]) < 0, msg);
    CHECK(g2w_text_glyphs(nope[i]) < 0, msg);
  }
  /* EXHAUSTIVE charset symmetry. For every byte the decoder turns into a glyph,
   * re-encoding that glyph has to produce a byte that decodes to the same thing. This is
   * the invariant every name setter leans on (encode, decode, compare, refuse), tested
   * over the whole 256-entry table instead of the dozen names above — so an edit to
   * either table that introduces an asymmetry cannot slip past unnoticed. */
  int glyphs_seen = 0;
  for (int b = 0; b < 256; b++) {
    if (b == 0x50) continue;                       /* the terminator is not a glyph */
    uint8_t one[2] = { (uint8_t)b, 0x50 };
    char dec[G2_NAME_BYTES], dec2[G2_NAME_BYTES];
    g2_decode_text(one, 1, dec, sizeof dec);
    if (dec[0] == 0) continue;
    if (b != 0x7F && strcmp(dec, " ") == 0) continue;   /* the decoder's "no idea" answer */
    uint8_t re[2];
    char msg[128];
    snprintf(msg, sizeof msg, "0x%02X decodes to '%s', which re-encodes to one glyph", b, dec);
    if (g2w_encode_text(re, 2, dec) != 1) { CHECK(0, msg); continue; }
    g2_decode_text(re, 1, dec2, sizeof dec2);
    snprintf(msg, sizeof msg, "charset symmetry at 0x%02X: '%s' -> 0x%02X -> '%s'",
             b, dec, re[0], dec2);
    CHECK(strcmp(dec, dec2) == 0, msg);
    glyphs_seen++;
  }
  CHECK(glyphs_seen > 80, "the sweep actually covered the charset");

  CHECK(g2w_encode_text(field, 11, "ELEVENCHARS") < 0, "11 glyphs do not fit a 10-glyph field");
  CHECK(g2w_encode_text(field, 9, "NINECHARS") < 0, "9 glyphs do not fit a box name");
  CHECK(g2w_encode_text(field, 9, "EIGHTCHR") == 8, "8 glyphs do fit a box name");

  /* And through the real setter, where the refusal has to leave the buffer alone. */
  if (!load_real("Gold.sav", &g_a)) load_fixture(GBF_GS, &g_a, 0);
  G2Writer w;
  if (open_w(&w, &g_a, G2_VER_NONE) != G2W_OK) { CHECK(0, "g2w_begin"); return; }
  static uint8_t list[G2_MAX_LIST_SIZE], copy[G2_MAX_LIST_SIZE];
  int box = pick_box(&w, list, 1, false);
  if (box < 0) { printf("     SKIP (no banked box with a Pokemon in it)\n"); return; }
  memcpy(copy, list, (size_t)g2_list_size(box));
  CHECK_ST(g2w_set_nickname(list, box, 0, "caf\xC3\xA9"), G2W_ERR_TEXT,
           "an unrepresentable nickname is refused");
  CHECK(memcmp(list, copy, (size_t)g2_list_size(box)) == 0,
        "...and the refused write left the buffer untouched");
  CHECK_ST(g2w_set_nickname(list, box, 0, "FARFETCH'D"), G2W_OK, "a 9-glyph name fits");
  G2Mon m;
  CHECK(g2_list_mon(list, box, 0, &m) && strcmp(m.nickname, "FARFETCH'D") == 0,
        "the reader reads it back exactly");
  CHECK_ST(g2w_set_box_name(&w, 0, "\xE2\x98\x85"), G2W_ERR_TEXT,
           "an unrepresentable box name is refused");
}

/* ===================================== DVs move four derived things at once ======== */

static void t_dv_effects(void) {
  printf("  -- DV edits and their consequences\n");
  if (!load_real("Crystal.sav", &g_a)) load_fixture(GBF_CRYSTAL, &g_a, 0);
  G2Writer w;
  if (open_w(&w, &g_a, G2_VER_CRYSTAL) != G2W_OK) { CHECK(0, "g2w_begin"); return; }
  static uint8_t list[G2_MAX_LIST_SIZE];
  int box = pick_box(&w, list, 1, false);
  if (box < 0) { printf("     SKIP (no banked box with a Pokemon in it)\n"); return; }

  const uint8_t RATIO_HALF = 127;                 /* 50% female — DV <= 7 is female  */
  G2WEffects fx;
  G2Mon m;

  /* Before anything else: the four DVs have to come back out in the ORDER they went in.
   * The effects struct is computed from the caller's own array, so it would report a
   * perfectly coherent story about DVs that were never stored — only the reader can say
   * what actually landed in the record. Four asymmetric values, so a swapped nibble or a
   * transposed pair cannot hide. */
  const uint8_t asym[4] = { 1, 2, 4, 8 };
  CHECK_ST(g2w_set_dvs(list, box, 0, asym, RATIO_HALF, &fx), G2W_OK, "set DVs 1/2/4/8");
  CHECK(g2_list_mon(list, box, 0, &m), "decodes");
  CHECK(memcmp(m.dv, asym, 4) == 0,
        "the READER reads back exactly the four DVs, in Atk/Def/Spd/Spc order");
  CHECK(m.hp_dv == fx.hp_dv_after, "and derives the same HP DV the setter reported");

  /* A male, non-shiny starting point. */
  const uint8_t male[4] = { 15, 15, 15, 15 };
  CHECK_ST(g2w_set_dvs(list, box, 0, male, RATIO_HALF, &fx), G2W_OK, "set DVs to 15/15/15/15");
  CHECK(g2_list_mon(list, box, 0, &m), "decodes");
  CHECK(memcmp(m.dv, male, 4) == 0, "15/15/15/15 stored");
  CHECK(!m.is_shiny, "15/15/15/15 is not shiny");

  /* The canonical Gen-2 shiny: Def=Spd=Spc=10 and an Attack DV with bit 1 set. */
  const uint8_t shiny[4] = { 10, 10, 10, 10 };
  CHECK_ST(g2w_set_dvs(list, box, 0, shiny, RATIO_HALF, &fx), G2W_OK, "set the shiny DVs");
  CHECK(fx.shiny_changed && fx.shiny_after && !fx.shiny_before,
        "the setter REPORTS that the Pokemon just became shiny");
  CHECK(fx.gender_changed == (fx.gender_before != fx.gender_after), "gender report is coherent");
  CHECK(fx.hp_dv_changed, "and that the HP DV moved with them");
  CHECK(g2_list_mon(list, box, 0, &m) && m.is_shiny,
        "the READER agrees the Pokemon is now shiny");
  CHECK(m.hp_dv == fx.hp_dv_after, "the reported HP DV is the one the reader derives");

  /* Gender is the Attack DV against the species ratio, and nothing else. */
  const uint8_t female[4] = { 0, 10, 10, 10 };
  CHECK_ST(g2w_set_dvs(list, box, 0, female, RATIO_HALF, &fx), G2W_OK, "drop the Attack DV");
  CHECK(fx.gender_changed && fx.gender_before == 0 && fx.gender_after == 1,
        "the setter REPORTS that the Pokemon just changed sex");
  CHECK(!fx.shiny_after, "and that it stopped being shiny on the way");
  CHECK(fx.shiny_changed, "which it also reported");
  CHECK(g2_list_mon(list, box, 0, &m) && memcmp(m.dv, female, 4) == 0,
        "and the record really holds 0/10/10/10");
  CHECK(!m.is_shiny, "the reader agrees it is no longer shiny");
  CHECK(g2_gender_from_dv(m.dv[0], RATIO_HALF) == 1,
        "the reader derives the same sex from the stored Attack DV");

  /* Unown's letter is the middle two bits of each DV nibble, so a DV edit is a form
   * edit for exactly one species. */
  CHECK_ST(g2w_set_species(list, box, 0, 201), G2W_OK, "make it an Unown");
  const uint8_t u1[4] = { 2, 2, 2, 2 }, u2[4] = { 15, 15, 15, 15 };
  CHECK_ST(g2w_set_dvs(list, box, 0, u1, 0xFF, &fx), G2W_OK, "set DVs");
  int letter1 = fx.unown_after;
  CHECK(fx.is_unown, "the setter knows this one is an Unown");
  CHECK_ST(g2w_set_dvs(list, box, 0, u2, 0xFF, &fx), G2W_OK, "set different DVs");
  CHECK(fx.unown_changed && fx.unown_before == letter1,
        "the setter REPORTS that the Unown's letter changed");
  CHECK(g2_list_mon(list, box, 0, &m) && m.unown_letter == fx.unown_after,
        "the reader derives the same letter");
  CHECK(fx.gender_after == 2, "a genderless ratio reports genderless");

  /* A genuinely out-of-range DV is refused rather than masked. */
  const uint8_t bad[4] = { 16, 0, 0, 0 };
  CHECK_ST(g2w_set_dvs(list, box, 0, bad, RATIO_HALF, &fx), G2W_ERR_ARG, "DV 16 is refused");

  /* Level edits admit they leave EXP behind. */
  CHECK_ST(g2w_set_level(list, box, 0, 50, &fx), G2W_OK, "set the level");
  CHECK(fx.exp_stale, "the setter says the EXP no longer matches the level");
  CHECK_ST(g2w_set_level(list, box, 0, 101, &fx), G2W_ERR_ARG, "level 101 is refused");
  CHECK_ST(g2w_set_level(list, box, 0, 0, &fx), G2W_ERR_ARG, "level 0 is refused");

  CHECK_ST(g2w_check_list(list, box), G2W_OK, "the edited list is still well formed");
  CHECK_ST(g2w_commit_list(&w, box, list), G2W_OK, "and commits");
  assert_healthy(&g_a, G2_VER_CRYSTAL, "after DV edits");
}

/* =========================== the fixture's deliberately-stale banked copy ========== */

/* The synthetic images plant DIFFERENT Pokemon in the banked copy of the open box than
 * in the live one, which no game-written save does. It is the one place we can exercise
 * "the two copies disagree" without hand-corrupting a real cartridge dump. */
static void t_fixture_stale_bank(void) {
  printf("  -- the fixture's stale banked copy\n");
  for (int g = 0; g < 2; g++) {
    GbfGame game = g ? GBF_CRYSTAL : GBF_GS;
    G2Version ver = g ? G2_VER_CRYSTAL : G2_VER_GS;
    load_fixture(game, &g_a, g ? GBF_RTC_TAIL_64 : GBF_RTC_TAIL_32);
    G2Writer w;
    if (open_w(&w, &g_a, ver) != G2W_OK) { CHECK(0, "g2w_begin on the fixture"); continue; }
    CHECK(w.current_box == gbf_current_box(game), "the writer found the open box");

    bool agree = true;
    CHECK_ST(g2w_current_box_agrees(&w, &agree), G2W_OK, "compare the copies");
    CHECK(!agree, "the fixture's two copies of the open box differ, as designed");

    static uint8_t list[G2_MAX_LIST_SIZE];
    CHECK_ST(g2w_load_list(&w, w.current_box, list), G2W_OK, "load the live copy");
    /* The fixture plants a deliberate glitch species in the open box. The list is still
     * perfectly walkable, so the two tiers have to disagree about it — which is the whole
     * reason there are two. */
    CHECK_ST(g2w_check_list(list, w.current_box), G2W_OK,
             "the fixture's open box is structurally walkable");
    CHECK_ST(g2w_check_list_strict(list, w.current_box), G2W_ERR_CONTENT,
             "...while the strict check flags its planted glitch species");
    CHECK_ST(g2w_commit_list(&w, w.current_box, list), G2W_OK, "commit it unchanged");
    CHECK_ST(g2w_current_box_agrees(&w, &agree), G2W_OK, "compare again");
    CHECK(agree, "committing the open box brings the banked copy back into line");

    assert_healthy(&g_a, ver, "fixture after a commit");
    assert_mirror_exact(&g_a, ver, "fixture after a commit");

    /* The RTC tail is past 0x8000 and nothing here may touch it. */
    uint32_t tail = g_a.len - G2_SAVE_SIZE;
    CHECK(tail == (g ? GBF_RTC_TAIL_64 : GBF_RTC_TAIL_32), "the tail is still there");
  }
}

/* ============================================ every real box passes the gate ======= */

static void t_real_corpus_structures(void) {
  static const char* files[] = { "Gold.sav", "Crystal.sav",
                                 "Gold-VC.sav.dat", "Crystal-VC.sav.dat" };
  for (unsigned f = 0; f < sizeof files / sizeof files[0]; f++) {
    if (!load_real(files[f], &g_a)) { printf("  SKIP %s\n", files[f]); continue; }
    G2Writer w;
    G2WStatus st = open_w(&w, &g_a, G2_VER_NONE);
    printf("  -- %s: %s, open box %d\n", files[f],
           st == G2W_OK ? g2_version_name(w.sv.version) : g2w_status_text(st),
           st == G2W_OK ? w.current_box : -1);
    CHECK_ST(st, G2W_OK, "a real save opens for writing");
    if (st != G2W_OK) continue;
    assert_mirror_exact(&g_a, w.sv.version, files[f]);
    static uint8_t list[G2_MAX_LIST_SIZE];
    int total = 0;
    for (int i = 0; i <= G2_NUM_BOXES; i++) {
      int box = all_boxes(i);
      if (g2w_load_list(&w, box, list) != G2W_OK) { CHECK(0, "load"); continue; }
      CHECK_ST(g2w_check_list(list, box), G2W_OK, "a real box passes the structural gate");
      CHECK_ST(g2w_check_list_strict(list, box), G2W_OK, "and the strict content check");
      total += g2_list_count(list, box);
    }
    printf("     %d Pokemon across 14 boxes + party, all structurally sound\n", total);
    uint16_t p = 0, b = 0;
    CHECK_ST(g2w_calc_checksums(&w, &p, &b), G2W_OK, "stream the checksums");
    /* The cross-oracle: gen2_write derives the summed spans from g2_mirror_map(), while
     * gen2_save keeps its own span table. Agreement is real evidence; it is also the
     * check that would have caught the transposed 0x3D96. */
    CHECK(p == g2_checksum_primary(g_a.buf, w.sv.version),
          "map-derived primary sum == the reader's own");
    CHECK(b == g2_checksum_backup(g_a.buf, w.sv.version),
          "map-derived backup sum == the reader's own");
    CHECK(p == b, "in Gen 2 the backup mirrors the primary, so the sums are equal");
  }
}

int main(void) {
  printf("== Gen-2 write path ==\n");
  t_real_corpus_structures();
  t_noop_roundtrip("Gold.sav", G2_VER_GS);
  t_noop_roundtrip("Crystal.sav", G2_VER_CRYSTAL);
  t_noop_roundtrip("Gold-VC.sav.dat", G2_VER_GS);
  t_noop_roundtrip("Crystal-VC.sav.dat", G2_VER_CRYSTAL);
  t_edit_mirrors("Gold.sav", G2_VER_GS);
  t_edit_mirrors("Crystal.sav", G2_VER_CRYSTAL);
  t_current_box_duality("Gold.sav", G2_VER_GS);
  t_current_box_duality("Crystal.sav", G2_VER_CRYSTAL);
  t_wrong_version();
  t_broken_structures();
  t_gate_has_teeth();
  t_range_guards();
  t_list_surgery();
  t_atomicity();
  t_preflight_total();
  t_party_mail_guard();
  t_text();
  t_dv_effects();
  t_fixture_stale_bank();

  printf("\ngen2 write test: %d checks, %d failure(s)\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}
