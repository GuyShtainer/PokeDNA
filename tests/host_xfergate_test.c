/* Host test for source/xfer_gate.{c,h} -- BACKLOG #120 S2: the six gate predicates that
 * close every Gen-3 write path a Bank visit from a Game Boy session exposes, plus
 * BACKLOG #150 S150-3's two escape-route predicates, plus (REVIEW F6) gbs_can_delete's
 * own lift-refusal table over a SYNTHESISED session.
 *
 *   cc -std=c11 -O2 -Wall -Wextra -I source -I tests tests/host_xfergate_test.c \
 *      source/xfer_gate.c source/bank_cell.c source/gb_edit.c source/gen1_save.c \
 *      source/gen1_write.c source/gen2_save.c source/gen2_write.c source/data_tables.c \
 *      source/gb_session.c tests/gen12_fixture.c \
 *      -o /tmp/hxgt && /tmp/hxgt
 *
 * Covers: the full truth table (all 4 input combinations, by value) for each of the five
 * boolean-pair predicates, plus all 9 scope pairs for xg_drop_denied, plus (H)/(I) below.
 * REVIEW F6: (J)-(O) drive gbs_can_delete (source/gb_session.c) over a synthesised
 * Gen-1/Gen-2 image (tests/gen12_fixture.c -- Guy owns no real Gen-1/2 saves with Mail,
 * so the corpus other tests use cannot cover it) -- party floor (both gens), Gen-2 Mail
 * (refuse, never strip), a closed session, an unwritable (streamed) session, and a bad
 * box/slot. */
#include <stdio.h>
#include <string.h>

#include "xfer_gate.h"
#include "bank_cell.h"
#include "gb_session.h"
#include "gb_edit.h"
#include "gen12_fixture.h"

static int checks = 0, fails = 0;
#define CHECK(c, msg) do { checks++; if (!(c)) { printf("  !! FAIL: %s\n", msg); fails++; } } while (0)

#define BOXSCOPE_PC   0u
#define BOXSCOPE_BANK 1u
/* BOXSCOPE_GB's value (2) is NOT redefined here -- the single source of truth is
 * source/pdna_box.c's own _Static_assert(BOXSCOPE_GB == 2, ...) right after its
 * "#include "xfer_gate.h"", which fails the build if source/pdna_box.h's
 * BOXSCOPE_GB and source/xfer_gate.c's hard-coded XG_SCOPE_GB ever drift apart.
 * This test still needs the literal value (xfer_gate.h deliberately does not
 * include pdna_box.h), so it is spelled out at each use below instead of a second
 * local #define that could itself go stale unnoticed (BACKLOG #120 S2 F2). */

static void test_xg_pc_live(void) {
  CHECK(xg_pc_live(false, false) == false, "pc_live: no save, arena free -> false");
  CHECK(xg_pc_live(false, true)  == false, "pc_live: no save, arena held -> false");
  CHECK(xg_pc_live(true,  false) == true,  "pc_live: save + arena free -> true");
  CHECK(xg_pc_live(true,  true)  == false, "pc_live: save but arena held -> false");
  printf("(A) xg_pc_live: full 2x2 truth table\n");
}

static void test_xg_togame_row(void) {
  for (int is_bank = 0; is_bank <= 1; is_bank++)
    for (int pc_live = 0; pc_live <= 1; pc_live++)
      for (int have_pc = 0; have_pc <= 1; have_pc++) {
        bool want = is_bank && pc_live && have_pc;
        CHECK(xg_togame_row(is_bank, pc_live, have_pc) == want,
              "togame_row: true iff Bank cell AND live Gen-3 PC AND something in hand");
      }
  printf("(B) xg_togame_row: full 2x2x2 truth table\n");
}

static void test_xg_paste_row(void) {
  CHECK(xg_paste_row(false, false) == false, "paste_row: nothing clipped, no PC -> false");
  CHECK(xg_paste_row(false, true)  == false, "paste_row: nothing clipped, live PC -> false");
  CHECK(xg_paste_row(true,  false) == false, "paste_row: clipped but no live PC -> false");
  CHECK(xg_paste_row(true,  true)  == true,  "paste_row: clipped AND live PC -> true");
  printf("(C) xg_paste_row: full 2x2 truth table\n");
}

static void test_xg_create_row(void) {
  CHECK(xg_create_row(false, false) == true,  "create_row: PC/party cell, no live PC -> still true (unaffected)");
  CHECK(xg_create_row(false, true)  == true,  "create_row: PC/party cell, live PC -> true");
  CHECK(xg_create_row(true,  false) == false, "create_row: Bank cell, no live Gen-3 PC -> false");
  CHECK(xg_create_row(true,  true)  == true,  "create_row: Bank cell, live Gen-3 PC -> true");
  printf("(G) xg_create_row: full 2x2 truth table\n");
}

static void test_xg_inject_refuse(void) {
  CHECK(xg_inject_refuse(false, false) == true,  "inject_refuse: arena free, no save -> refuse");
  CHECK(xg_inject_refuse(false, true)  == false, "inject_refuse: arena free, live save -> allow");
  CHECK(xg_inject_refuse(true,  false) == true,  "inject_refuse: arena held, no save -> refuse");
  CHECK(xg_inject_refuse(true,  true)  == true,  "inject_refuse: arena held even with a save -> refuse");
  printf("(D) xg_inject_refuse: full 2x2 truth table\n");
}

static void test_xg_clear_carry_on_gb_exit(void) {
  CHECK(xg_clear_carry_on_gb_exit(false) == false, "clear_carry: PC/Bank-scope carry survives exit");
  CHECK(xg_clear_carry_on_gb_exit(true)  == true,  "clear_carry: GB-scope carry is dropped on exit");
  printf("(E) xg_clear_carry_on_gb_exit: both inputs\n");
}

/* BACKLOG #150 S150-4 decision 9/step 6(b): the full 3x3x2 = 18-combination truth
 * table -- false for EXACTLY ONE (dst=BANK, src=GB, have_xfer=true), the UP edge this
 * lane adds. Every other combination stays denied, including have_xfer=true on any
 * OTHER scope pair (a vtable existing says nothing about whether GB is even
 * involved) and (dst=BANK, src=GB, have_xfer=false) (S2's "no lift path yet" case,
 * still denied when the real vtable isn't installed). */
static void test_xg_drop_denied(void) {
  uint8_t scopes[3] = { BOXSCOPE_PC, BOXSCOPE_BANK, 2u /* BOXSCOPE_GB, see the comment above */ };
  int allowed = 0;
  for (int d = 0; d < 3; d++)
    for (int s = 0; s < 3; s++)
      for (int hx = 0; hx <= 1; hx++) {
        bool have_xfer = (bool)hx;
        bool allow = (scopes[d] == BOXSCOPE_BANK) && (scopes[s] == 2u) && have_xfer;
        bool want = !allow && ((scopes[d] == 2u) || (scopes[s] == 2u));
        if (allow) allowed++;
        CHECK(xg_drop_denied(scopes[d], scopes[s], have_xfer) == want,
              "drop_denied: denied unless dst=BANK, src=GB, have_xfer -- the one S150-4 allow-rule");
      }
  CHECK(allowed == 1, "drop_denied: exactly ONE of the 18 combinations is allowed");
  printf("(F) xg_drop_denied: all 3x3x2 = 18 scope/have_xfer combinations, exactly one allowed\n");
}

/* BACKLOG #150 S150-3 step 1: build a native fixture WITHOUT a corpus -- fill 80 bytes
 * with a recognisable pattern, stamp the magic + the old-build byte-19 guard + a
 * plausible gen/rec_len, then let bc_ident32() (works on any 80-byte buffer, bank_cell.h
 * :143-144) fill in its own identity hash at bytes 4..7 so bc_is_native() reads it back
 * as true. The all-zero cell (memset(0)) must read false (no magic). */
static void make_native_fixture(uint8_t out80[80]) {
  for (int i = 0; i < 80; i++) out80[i] = (uint8_t)(0x40 + i);   /* recognisable, non-zero pattern */
  out80[0] = 'G'; out80[1] = 'B'; out80[2] = 'C'; out80[3] = '1';
  out80[8] = 1;    /* gen = GB_GEN1 */
  out80[9] = 33;   /* rec_len = Gen-1 box record (GEN1_BOX_REC_BYTES) */
  out80[19] = 0x01;   /* the old-build guard byte, BC_OLDBUILD_BYTE */
  uint32_t id = bc_ident32(out80);
  out80[4] = (uint8_t)(id & 0xFF);
  out80[5] = (uint8_t)((id >> 8) & 0xFF);
  out80[6] = (uint8_t)((id >> 16) & 0xFF);
  out80[7] = (uint8_t)((id >> 24) & 0xFF);
}

/* A real Gen-3 corpus record has none of these bytes right, so it is enough to build
 * one with a plausible-but-wrong shape (no "GBC1" magic) rather than pull in a whole
 * .sav -- bc_is_native()'s FIRST check is the magic (bank_cell.h:130-132), so any
 * buffer that fails it is treated identically regardless of what follows. */
static void make_gen3_like_fixture(uint8_t out80[80]) {
  memset(out80, 0, 80);
  out80[0] = 0x34; out80[1] = 0x12; out80[2] = 0xAB; out80[3] = 0xCD;   /* personality, not "GBC1" */
  out80[19] = 0x00;                                                     /* no old-build guard bit */
}

#define XG_SCOPE_GB 2u

static void test_xg_native_escape_denied(void) {
  uint8_t native[80]; make_native_fixture(native);
  uint8_t allzero[80]; memset(allzero, 0, 80);
  uint8_t gen3like[80]; make_gen3_like_fixture(gen3like);
  if (!(bc_is_native(native) && !bc_is_native(allzero) && !bc_is_native(gen3like)))
    { printf("  !! FAIL: fixture setup: native/all-zero/gen3-like bc_is_native() disagree with the plan\n"); fails++; }
  checks++;

  const uint8_t* cells[3] = { native, allzero, gen3like };
  const char* names[3] = { "native", "all-zero", "gen3-like" };
  const uint8_t scopes[3] = { BOXSCOPE_PC, BOXSCOPE_BANK, XG_SCOPE_GB };
  const char* snames[3] = { "PC", "BANK", "GB" };
  for (int c = 0; c < 3; c++)
    for (int s = 0; s < 3; s++) {
      bool want = (c == 0) && (scopes[s] != BOXSCOPE_BANK);   /* denied only for native x {PC, GB} */
      char msg[96];
      snprintf(msg, sizeof msg, "native_escape_denied: %s cell -> %s scope", names[c], snames[s]);
      CHECK(xg_native_escape_denied(cells[c], scopes[s]) == want, msg);
    }
  printf("(H) xg_native_escape_denied: 3 cells x 3 dst scopes = 9 assertions, true only native x {PC, GB}\n");
}

static void test_xg_chunk_crossgen_denied(void) {
  /* Matches pdna_box.c's own `if (src->scope == BOXSCOPE_GB || s_ch_scope == BOXSCOPE_GB)`
   * (drop_chunk's cross-generation refusal) exactly -- all 9 scope pairs. */
  uint8_t scopes[3] = { BOXSCOPE_PC, BOXSCOPE_BANK, XG_SCOPE_GB };
  for (int d = 0; d < 3; d++)
    for (int s = 0; s < 3; s++) {
      bool want = (scopes[d] == XG_SCOPE_GB) || (scopes[s] == XG_SCOPE_GB);
      CHECK(xg_chunk_crossgen_denied(scopes[d], scopes[s]) == want,
            "chunk_crossgen_denied: true iff either scope is BOXSCOPE_GB (== 2)");
    }
  printf("(I) xg_chunk_crossgen_denied: all 9 scope pairs\n");
}

/* ---- REVIEW F6: gbs_can_delete's own lift-refusal table, over a SYNTHESISED session
 * (the corpus has no Mail) ------------------------------------------------------- */
static uint8_t g_cd_img[GBF_MAX_BYTES];
static uint8_t g_cd_scratch[GBS_SCRATCH_BYTES];
static uint8_t g_cd_list[GBS_LIST_BYTES];

static bool cd_stream_read(void* ctx, uint32_t off, void* dst, uint32_t n) {
  memcpy(dst, (const uint8_t*)ctx + off, n);
  return true;
}

static void test_gbs_can_delete_table(void) {
  /* ---- (J) Gen-1 party floor: reduce to 1 member (gbs_delete, already proven correct
   * by host_gbsession_test.c), then the last delete refuses ---- */
  {
    uint32_t len = gbf_build(GBF_RBY, g_cd_img, 0);
    GbSession s; memset(&s, 0, sizeof s);
    CHECK(gbs_open(&s, g_cd_img, len, g_cd_scratch, sizeof g_cd_scratch) == GBS_OK, "G1 floor: session opens");
    int pb = gbs_party_box(&s);
    CHECK(gbs_delete(&s, pb, 2, g_cd_list) == GBS_OK, "G1 floor: delete slot 2 (3 -> 2)");
    CHECK(gbs_delete(&s, pb, 1, g_cd_list) == GBS_OK, "G1 floor: delete slot 1 (2 -> 1)");
    bool ack;
    CHECK(gbs_can_delete(&s, pb, 0, g_cd_list, &ack) == GBS_ERR_PARTY_FLOOR,
          "G1 floor: the last party member cannot be lifted");
  }

  /* ---- (K) Gen-2 non-egg floor: same shape, the Gen-2 predicate ---- */
  {
    uint32_t len = gbf_build(GBF_GS, g_cd_img, 0);
    GbSession s; memset(&s, 0, sizeof s);
    CHECK(gbs_open(&s, g_cd_img, len, g_cd_scratch, sizeof g_cd_scratch) == GBS_OK, "G2 floor: session opens");
    int pb = gbs_party_box(&s);
    CHECK(gbs_delete(&s, pb, 2, g_cd_list) == GBS_OK, "G2 floor: delete slot 2 (3 -> 2)");
    CHECK(gbs_delete(&s, pb, 1, g_cd_list) == GBS_OK, "G2 floor: delete slot 1 (2 -> 1)");
    bool ack;
    CHECK(gbs_can_delete(&s, pb, 0, g_cd_list, &ack) == GBS_ERR_PARTY_FLOOR,
          "G2 floor: the last non-egg party member cannot be lifted");
  }

  /* ---- (L) Gen-2 Mail: refuse, NEVER strip -- g2_party_has_mail scans the WHOLE
   * party, so a delete of a DIFFERENT slot is refused too, not just the mail holder's
   * own slot (this is the row the mutation demonstration below proves matters). ---- */
  {
    uint32_t len = gbf_build(GBF_GS, g_cd_img, 0);
    GbSession s; memset(&s, 0, sizeof s);
    CHECK(gbs_open(&s, g_cd_img, len, g_cd_scratch, sizeof g_cd_scratch) == GBS_OK, "G2 mail: session opens");
    int pb = gbs_party_box(&s);
    CHECK(gbs_load_list(&s, pb, g_cd_list) == GBS_OK, "G2 mail: party loads");
    GbEditMon mon;
    CHECK(gb_load(&mon, GB_GEN2, g_cd_list, pb, 1) == true, "G2 mail: slot 1 loads");
    CHECK(gb_set_held_item(&mon, 0xB5u) == true, "G2 mail: item set to a Mail id (G2_MAIL_LO)");
    CHECK(gb_commit(&mon, g_cd_list, pb, 1) == true, "G2 mail: committed into the list");
    CHECK(gbs_commit_list(&s, pb, g_cd_list) == GBS_OK, "G2 mail: list committed (checksums refreshed)");
    bool ack;
    CHECK(gbs_can_delete(&s, pb, 0, g_cd_list, &ack) == GBS_ERR_MAIL,
          "G2 mail: Mail anywhere in the party refuses every delete in that box");
  }

  /* ---- (M) a closed session ---- */
  {
    GbSession s; memset(&s, 0, sizeof s);
    bool ack;
    CHECK(gbs_can_delete(&s, 0, 0, g_cd_list, &ack) == GBS_ERR_ARG, "closed session: refused (GBS_ERR_ARG)");
  }

  /* ---- (N) an unwritable (streamed) session -- BACKLOG #64, never gbs_box_writable ---- */
  {
    uint32_t len = gbf_build(GBF_RBY, g_cd_img, 0);
    GbSession s; memset(&s, 0, sizeof s);
    CHECK(gbs_open_streamed(&s, cd_stream_read, g_cd_img, len, g_cd_scratch, sizeof g_cd_scratch) == GBS_OK,
          "streamed: session opens");
    bool ack;
    CHECK(gbs_can_delete(&s, gbs_party_box(&s), 0, g_cd_list, &ack) == GBS_ERR_UNWRITABLE,
          "streamed: refused (GBS_ERR_UNWRITABLE), never writable");
  }

  /* ---- (O) bad box / bad slot ---- */
  {
    uint32_t len = gbf_build(GBF_RBY, g_cd_img, 0);
    GbSession s; memset(&s, 0, sizeof s);
    CHECK(gbs_open(&s, g_cd_img, len, g_cd_scratch, sizeof g_cd_scratch) == GBS_OK, "bad box/slot: session opens");
    bool ack;
    CHECK(gbs_can_delete(&s, 999, 0, g_cd_list, &ack) == GBS_ERR_BOX, "bad box: refused (GBS_ERR_BOX)");
    CHECK(gbs_can_delete(&s, gbs_party_box(&s), 999, g_cd_list, &ack) == GBS_ERR_SLOT, "bad slot: refused (GBS_ERR_SLOT)");
  }

  printf("(J-O) gbs_can_delete: floor (both gens) / Mail / closed / unwritable / bad box-slot, over a synthesised session\n");
}

/* MUTATION DEMONSTRATION: proves check (L) above actually has teeth. Reads
 * source/gb_session.c's own text (same "comment-strip + text search" discipline
 * tests/host_escape_gate_sites_test.py uses on pdna_box.c), confirms gbs_can_delete's
 * body really calls g2_party_has_mail(, then builds an IN-MEMORY copy of that body
 * with the row deleted and re-checks -- proving a caller relying only on presence
 * (not this exact call) would have missed the Mail-refusal row entirely. */
static void test_mail_row_mutation(void) {
  FILE* f = fopen("source/gb_session.c", "rb");
  if (!f) { CHECK(false, "mutation demo: could not open source/gb_session.c (run from the repo root)"); return; }
  char buf[65536];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;

  const char* fn = strstr(buf, "GbsStatus gbs_can_delete(GbSession* s");
  CHECK(fn != NULL, "mutation demo: gbs_can_delete( not found in source/gb_session.c");
  if (!fn) return;
  const char* end = strstr(fn, "\nGbsStatus gbs_delete(");   /* the next function starts here */
  CHECK(end != NULL, "mutation demo: could not find gbs_can_delete's end (next function marker)");
  if (!end) return;

  size_t body_len = (size_t)(end - fn);
  char body[8192];
  CHECK(body_len < sizeof body, "mutation demo: gbs_can_delete grew past the scratch buffer -- widen it");
  if (body_len >= sizeof body) return;
  memcpy(body, fn, body_len);
  body[body_len] = 0;

  const char* mail_call = "g2_party_has_mail(list, box)";
  CHECK(strstr(body, mail_call) != NULL,
        "mutation demo: the REAL gbs_can_delete no longer calls g2_party_has_mail(list, box) -- fix the source");

  /* the mutation: the exact same body text with the mail-check LINE removed (the whole
   * `if (g2_party_has_mail(list, box)) return GBS_ERR_MAIL;` statement, not just the
   * call, so the demonstration matches "delete the row" literally). */
  char mutated[8192]; size_t mp = 0;
  const char* line_start = body;
  size_t mail_call_len = strlen(mail_call);
  while (*line_start) {
    const char* nl = strchr(line_start, '\n');
    size_t line_len = nl ? (size_t)(nl - line_start) : strlen(line_start);
    bool is_mail_line = false;
    if (line_len >= mail_call_len) {
      for (size_t i = 0; i + mail_call_len <= line_len; i++) {
        if (memcmp(line_start + i, mail_call, mail_call_len) == 0) { is_mail_line = true; break; }
      }
    }
    if (!is_mail_line) {
      memcpy(mutated + mp, line_start, line_len);
      mp += line_len;
      mutated[mp++] = '\n';
    }
    if (!nl) break;
    line_start = nl + 1;
  }
  mutated[mp] = 0;

  bool mutated_still_checks_mail = (strstr(mutated, mail_call) != NULL);
  CHECK(!mutated_still_checks_mail,
        "mutation demo: the mail-check line survived removal -- the mutation itself is broken, fix this test");
  printf("  mutation demonstration -- g2_party_has_mail(list, box) row deleted from a scratch copy of "
         "gbs_can_delete's body: %s\n",
         mutated_still_checks_mail ? "STILL PRESENT (test is broken)" : "gone, as expected (the real check (L) above is what catches this in practice)");
}

int main(void) {
  test_xg_pc_live();
  test_xg_togame_row();
  test_xg_paste_row();
  test_xg_create_row();
  test_xg_inject_refuse();
  test_xg_clear_carry_on_gb_exit();
  test_xg_drop_denied();
  test_xg_native_escape_denied();
  test_xg_chunk_crossgen_denied();
  test_gbs_can_delete_table();
  test_mail_row_mutation();

  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
