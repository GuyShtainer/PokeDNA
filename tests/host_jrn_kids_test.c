/* source/journal.c (BACKLOG #304): the CHILDREN WALK -- jrn_kid_counts / jrn_kid_list, over the REAL lib/fatfs on a RAM disk.
 * ADDITIVE-ONLY: the format stores parent pointers and nothing else, so the walk is a scan; this file pins (1) the answers on forks of
 * 2 and 3 children, the orphaned-by-new-step shape, a root fork, crossed siblings, a chain HEAD as ONE step, a fork whose child lives in
 * the tail segment, v1 AND v2 journals, pending vs flushed vs reopened; (2) the frozen layout (static asserts) and (3) the read cost.
 *
 *   cc -std=c11 -Wall -Wextra -Wno-unused-function -DFF_USE_MKFS=1 -Dsiprintf=sprintf -Dsniprintf=snprintf -Dvsniprintf=vsnprintf -I tests/hostfat -I lib/fatfs -I source \
 *      tests/host_jrn_kids_test.c source/journal.c source/journal_undo.c source/journal_fs.c \
 *      lib/fatfs/ff.c lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c -o /tmp/hjkids
 *
 * tools/journal_mutants.py ("kids" test) rebuilds this against mutated copies of journal.c; each mutant must go RED by the named line. */
#include "jrn_harness.h"
#include "journal_int.h"

static const uint64_t K = 0x5566778899AABBCCull;

/* THE FROZEN FORMAT: the children walk added no stored byte -- the layout constants and the record/segment shapes are exactly as before. */
_Static_assert(JRN_REC_HDR == 52u && JRN_REC_MIN == 56u && JRN_REC_MAX == 512u, "record header/min/max moved");
_Static_assert(JRN_SEG_HDR == 32u && JRN_REC_BASE == 512u && JRN_SEG_SIZE == 65536u, "segment header/base/size moved");
_Static_assert(JRN_SEG_VER == 1u && JRN_SEG_VER2 == 2u && JRN_RING_MAX == 17u, "versions / ring moved");
_Static_assert(sizeof(JrnKid) <= 32u, "JrnKid is a RAM-only view (<= 32 B: 16 of them fit one 512-B record buffer)");

static void world(Jrn* j) {
  card_fresh(FM_FAT);
  img_fill(5);
  rd_fattime_hook = jrn_fattime_filter;
  CHECK(jopen(j, K) == JRN_OK, "open fresh");
  CHECK(jrn_prepare(j) == JRN_OK, "prepare fresh");
}

static unsigned g_off = 16;
/* One plain step of region 1; returns its seq (the cursor afterwards). */
static uint32_t step(Jrn* j, const char* name, int crossed) {
  g_off = 16u;                                                   /* 200 bytes per step (a ~460-byte record: ~140 fill one segment) */
  CHECK(stage(j, name, crossed, 1, (uint16_t)g_off, 200, fresh_val(1, (uint16_t)g_off)) == JRN_OK, "stage %s", name);
  return jrn_cursor(j);
}
/* the same, then flushed: an undo of a still-PENDING step just pops it (no fork), so a fork needs its steps on disk first */
static uint32_t stepf(Jrn* j, const char* name, int crossed) {
  uint32_t q = step(j, name, crossed);
  CHECK(jrn_flush(j) == JRN_OK, "flush %s", name);
  return q;
}
static void undo1(Jrn* j) { CHECK(jrn_undo(j, &IMG, 0) == JRN_OK && jrn_flush(j) == JRN_OK, "undo + flush (the cursor marker must be on disk: a 200-byte step does not fit the pending buffer beside it)"); }

/* the answers for ONE parent: count (with skip) and the listed children */
static unsigned cnt1(Jrn* j, uint32_t parent, uint32_t skip) {
  uint16_t c = 0xFFFFu;
  CHECK(jrn_kid_counts(j, &parent, &skip, &c, 1) == JRN_OK, "counts(%u)", (unsigned)parent);
  return c;
}
static unsigned list1(Jrn* j, uint32_t parent, uint32_t skip, uint32_t* seqs, unsigned cap) {
  JrnKid kid[16];
  uint8_t got = 0xFF;
  unsigned i;
  CHECK(cap <= 16u && jrn_kid_list(j, parent, skip, 0, kid, (uint8_t)cap, &got) == JRN_OK, "list(%u)", (unsigned)parent);
  for (i = 0; i < got && i < cap; i++) seqs[i] = kid[i].seq;
  return got;
}

/* ---- forks of 2 and 3 children ---------------------------------------------------------------------------------------------- */
static void t_two_and_three(void) {
  Jrn j; uint32_t a, b, c, d, s[8]; unsigned n;
  world(&j);
  a = stepf(&j, "A", 0); b = stepf(&j, "B", 0);                 /* A -> B */
  undo1(&j);                                                   /* cursor A */
  c = step(&j, "C", 0);                                        /* A -> C : A has TWO children B, C (the tip is C) */
  CHECK(jrn_flush(&j) == JRN_OK, "flush");
  CHECK(cnt1(&j, a, c) == 1u, "FORK-2: A's children other than the branch child C = 1 (B)");
  CHECK(cnt1(&j, a, 0) == 2u, "FORK-2: A has 2 children in all (skip 0 counts them all)");
  n = list1(&j, a, c, s, 8);
  CHECK(n == 1u && s[0] == b, "FORK-2: the listed sibling is B (%u items, first %u, want %u)", n, n ? (unsigned)s[0] : 0u, (unsigned)b);
  CHECK(cnt1(&j, b, 0) == 0u && cnt1(&j, c, 0) == 0u, "leaves have no children");
  undo1(&j);                                                   /* cursor A again */
  d = step(&j, "D", 0);                                        /* A -> D : A has THREE children */
  CHECK(cnt1(&j, a, d) == 2u, "FORK-3: A's children other than D = 2 (B, C)");
  n = list1(&j, a, d, s, 8);
  CHECK(n == 2u && s[0] == b && s[1] == c, "FORK-3: the listed siblings are B, C in seq order (%u items: %u %u)", n, (unsigned)s[0], (unsigned)s[1]);
  { JrnKid k[16]; uint8_t g = 0;                               /* the paging: first = 1 skips the first sibling */
    CHECK(jrn_kid_list(&j, a, d, 1, k, 1, &g) == JRN_OK && g == 1 && k[0].seq == c && strcmp(k[0].name, "C") == 0, "list page: first=1 cap=1 yields C (%u)", (unsigned)k[0].seq); }
  CHECK(cnt1(&j, a, 0) == 3u, "FORK-3: A has 3 children in all");
}

/* ---- the orphaned-by-new-step shape (#304's motivating one): undo, then a NEW step; the old tail is now a sibling -------------------- */
static void t_orphan(void) {
  Jrn j; uint32_t a, x1, x2, x3, nw, s[4];
  world(&j);
  a = stepf(&j, "A", 0); x1 = stepf(&j, "X1", 0); x2 = stepf(&j, "X2", 0); x3 = stepf(&j, "X3", 0);
  undo1(&j); undo1(&j); undo1(&j);                             /* cursor A, tip X3 */
  nw = step(&j, "NEW", 0);                                     /* NEW's parent is A: the tail X1-X2-X3 is orphaned */
  CHECK(jrn_tip(&j) == nw, "the tip is the new step");
  CHECK(cnt1(&j, a, nw) == 1u, "ORPHAN: A's other child is X1 (the old tail's first step)");
  CHECK(list1(&j, a, nw, s, 4) == 1u && s[0] == x1, "ORPHAN: the listed sibling is X1");
  CHECK(cnt1(&j, x1, 0) == 1u && cnt1(&j, x2, 0) == 1u && cnt1(&j, x3, 0) == 0u, "the orphaned chain is itself linked X1->X2->X3 (a sibling's own children are NOT siblings of anything on the branch)");
}

/* ---- a ROOT fork (parent 0) and crossed siblings ---------------------------------------------------------------------------------- */
static void t_root_and_crossed(void) {
  Jrn j; uint32_t a, b, s[4];
  JrnKid k[4]; uint8_t g;
  world(&j);
  a = stepf(&j, "A", 0);
  undo1(&j);                                                   /* cursor 0 */
  b = step(&j, "B", 1);                                        /* a CROSSED step (a floor) that is a second child of the root */
  CHECK(cnt1(&j, 0, b) == 1u, "ROOT FORK: the root has one child besides B (A)");
  CHECK(list1(&j, 0, b, s, 4) == 1u && s[0] == a, "ROOT FORK: A listed");
  CHECK(jrn_kid_list(&j, 0, a, 0, k, 4, &g) == JRN_OK && g == 1 && k[0].seq == b && k[0].crossed == 1 && strcmp(k[0].name, "B") == 0, "a CROSSED sibling reports crossed=1 and its name");
  CHECK(jrn_kid_list(&j, 0, b, 0, k, 4, &g) == JRN_OK && g == 1 && k[0].crossed == 0, "a plain sibling reports crossed=0");
}

/* ---- parents must be strictly descending; arguments are checked ----------------------------------------------------------------- */
static void t_args(void) {
  Jrn j; uint32_t par[3] = { 5, 5, 1 }, par2[2] = { 3, 7 }, sk[3] = { 0, 0, 0 }; uint16_t c[3]; JrnKid k[1]; uint8_t g;
  world(&j);
  stepf(&j, "A", 0);
  CHECK(jrn_kid_counts(&j, par, sk, c, 3) == JRN_E_ARG, "equal parents are refused");
  CHECK(jrn_kid_counts(&j, par2, sk, c, 2) == JRN_E_ARG, "ascending parents are refused");
  CHECK(jrn_kid_counts(&j, par, sk, c, 0) == JRN_E_ARG && jrn_kid_counts(&j, 0, sk, c, 1) == JRN_E_ARG, "n == 0 / NULL are refused");
  CHECK(jrn_kid_list(&j, 0, 0, 0, k, 0, &g) == JRN_E_ARG && jrn_kid_list(&j, 0, 0, 0, 0, 1, &g) == JRN_E_ARG, "cap 0 / NULL out are refused");
}

/* ---- a chain HEAD is ONE step: its parts are never children ------------------------------------------------------------------------- */
static uint8_t g_old[NREG][RSZ], g_new[NREG][RSZ];
static void chain_step(Jrn* j, const char* name) {              /* one bulk step (two 300-byte runs: > one record = a chain) */
  JrnBlk blk[NREG]; unsigned r, k;
  memcpy(g_old, g_img, sizeof g_old);
  memcpy(g_new, g_img, sizeof g_new);
  for (k = 0; k < 300; k++) { g_new[2][40 + k] = (uint8_t)(g_old[2][40 + k] ^ 0x5A); g_new[2][900 + k] = (uint8_t)(g_old[2][900 + k] ^ 0x3C); }
  for (r = 0; r < NREG; r++) { blk[r].old_blk = g_old[r]; blk[r].new_blk = g_new[r]; }
  CHECK(jrn_chain_record(j, name, 0, blk, NREG) == JRN_OK, "chain record %s", name);
  memcpy(g_img, g_new, sizeof g_img);
}
static void t_chain_head(void) {
  Jrn j; uint32_t a, head, nw, s[4]; uint8_t v;
  world(&j);
  a = stepf(&j, "A", 0);
  chain_step(&j, "BULK");
  head = jrn_cursor(&j);
  CHECK(head > a + 1u || head >= a + 1u, "the chain head's seq");
  CHECK(jrn_flush(&j) == JRN_OK, "flush");
  undo1(&j);                                                   /* the whole chain undone: cursor A */
  CHECK(jrn_cursor(&j) == a, "chain undone");
  nw = step(&j, "NEW", 0);
  CHECK(jrn_flush(&j) == JRN_OK, "flush");
  CHECK(cnt1(&j, a, nw) == 1u, "CHAIN: A's other child is the chain HEAD (one step, not head + parts)");
  CHECK(list1(&j, a, nw, s, 4) == 1u && s[0] == head, "CHAIN: the listed sibling is the HEAD seq");
  CHECK(cnt1(&j, head, 0) == 0u, "CHAIN: the head has NO children (its parts name it as their parent but are kind 3)");
  CHECK(raw_read(K, j.tail_seg, 4, &v, 1) == 0 && (v == 1u || v == 2u), "the tail segment's version byte is a known one");
  CHECK(cnt1(&j, a, 0) == 2u, "CHAIN: A has 2 children (the head + NEW), never 1 + parts");
}

/* ---- the tail segment: a child written into the LAST (tail) segment is found; a reopen sees the same answers; pending counts too ---- */
static void t_segments_reopen_pending(void) {
  Jrn j; uint32_t a, b, c, i; unsigned seg0;
  world(&j);
  a = stepf(&j, "A", 0);
  for (i = 0; i < 200; i++) b = stepf(&j, "fill", 0);          /* a long chain past one segment (~140 records of this size per segment) */
  CHECK(jrn_flush(&j) == JRN_OK, "flush");
  seg0 = j.tail_seg;
  CHECK(seg0 >= 2u, "the chain spans segments (tail seg %u)", seg0);
  for (i = 0; i < 200; i++) undo1(&j);                         /* back to A (each undo is a disk step) */
  CHECK(jrn_cursor(&j) == a, "cursor back at A");
  c = step(&j, "FORK", 0);                                     /* A's second child: lands in the TAIL segment, far from A */
  CHECK(jrn_pending(&j) >= 1, "the fork step is still PENDING (not on disk)");
  CHECK(cnt1(&j, a, c) == 1u && cnt1(&j, a, 0) == 2u, "TAIL/PENDING: a child only in the pending buffer is counted (1 sibling of FORK, 2 in all)");
  { uint32_t s[2]; CHECK(list1(&j, a, c, s, 2) == 1u && s[0] == a + 1u, "the sibling is A's first child (seq %u)", (unsigned)s[0]); }
  CHECK(jrn_flush(&j) == JRN_OK && j.tail_seg >= seg0, "flush");
  CHECK(cnt1(&j, a, c) == 1u && cnt1(&j, a, 0) == 2u, "TAIL/FLUSHED: the same answers once the child is on disk (no double count of pending + disk)");
  { uint32_t s[2]; CHECK(list1(&j, c, 0, s, 2) == 0u, "the new step has no children"); }
  card_remount();
  CHECK(jopen(&j, K) == JRN_OK, "reopen");
  CHECK(cnt1(&j, a, c) == 1u && cnt1(&j, a, 0) == 2u, "REOPEN: identical answers (the walk keeps no state; nothing double-counts)");
  CHECK(cnt1(&j, a, c) == 1u, "REOPEN: and a second call agrees (no counter left behind)");
  (void)b;
}

/* ---- v2 segments: the same shape on a journal whose tail is a v2 segment (a chain stamped it) ---------------------------------------- */
static void t_v2(void) {
  Jrn j; uint32_t a, x, y, z, nw, s[4]; uint8_t v1 = 0, v2 = 0;
  world(&j);
  a = stepf(&j, "A", 0);
  chain_step(&j, "BULK");                                      /* rolls the tail onto the v2 spare */
  CHECK(raw_read(K, j.tail_seg, 4, &v2, 1) == 0 && v2 == 2u, "the tail segment IS v2 (version %u)", v2);
  CHECK(raw_read(K, 1, 4, &v1, 1) == 0 && v1 == 1u, "while segment 1 stays v1: a MIXED journal");
  x = jrn_cursor(&j);
  y = stepf(&j, "Y", 0);
  CHECK(jrn_flush(&j) == JRN_OK, "flush");
  undo1(&j); undo1(&j);                                        /* cursor A (Y undone, then the chain) */
  z = stepf(&j, "Z", 0);
  nw = step(&j, "W", 0);                                       /* A->Z->W ... undo W, Z? keep it simple: A has children BULK head and Z */
  CHECK(jrn_flush(&j) == JRN_OK, "flush");
  CHECK(cnt1(&j, a, z) == 1u, "V2: A's other child (besides Z) is the chain head");
  CHECK(list1(&j, a, z, s, 4) == 1u && s[0] == x, "V2: the sibling is the chain head");
  CHECK(cnt1(&j, x, y) == 0u && cnt1(&j, x, 0) == 1u, "V2: the head's child is Y (a step AFTER a chain: the chain's parts are not children)");
  (void)nw;
}

/* ---- the COST: ONE pass, independent of how many parents are asked about; reads reported ----------------------------------------------------- */
static void t_cost(void) {
  Jrn j; uint32_t par[48], skip[48], a, i; uint16_t cnt[48]; unsigned long r1, r48, rl, n_steps = 200;
  world(&j);
  a = stepf(&j, "A", 0);
  for (i = 0; i < n_steps; i++) stepf(&j, "s", 0);
  CHECK(jrn_flush(&j) == JRN_OK, "flush");
  card_remount();
  CHECK(jopen(&j, K) == JRN_OK, "reopen so every read is a CARD read");
  par[0] = a; skip[0] = 0;
  rd_reads = 0; CHECK(jrn_kid_counts(&j, par, skip, cnt, 1) == JRN_OK, "1 parent"); r1 = rd_reads;
  for (i = 0; i < 48; i++) { par[i] = a + 100u - i; skip[i] = 0; }   /* 48 strictly descending parents, all inside the chain; the lowest is a+53 */
  rd_reads = 0; CHECK(jrn_kid_counts(&j, par, skip, cnt, 48) == JRN_OK, "48 parents"); r48 = rd_reads;
  par[0] = a;
  { JrnKid k[1]; uint8_t g; rd_reads = 0; CHECK(jrn_kid_list(&j, a, 0, 0, k, 1, &g) == JRN_OK && g == 1, "list one"); rl = rd_reads; }
  printf("  cost: counts(1 parent from seq %u) = %lu sector reads; counts(48 parents from seq %u) = %lu; list(cap 1, early stop) = %lu (a segment is %u sectors)\n",
         (unsigned)a, r1, (unsigned)(a + 53u), r48, rl, JRN_SEG_SIZE / 512u);
  CHECK(r1 <= 2u * (JRN_SEG_SIZE / 512u), "ONE parent: one pass over at most the segments from its own (%lu <= %u)", r1, 2u * (JRN_SEG_SIZE / 512u));
  CHECK(r48 <= r1, "48 parents cost no MORE than 1 (the pass is shared, and it starts later: %lu <= %lu)", r48, r1);
  CHECK(rl < r1, "a list with an early stop reads less than the full count (%lu < %lu)", rl, r1);
}

int main(void) {
  t_two_and_three();
  t_orphan();
  t_root_and_crossed();
  t_args();
  t_chain_head();
  t_segments_reopen_pending();
  t_v2();
  t_cost();
  if (fails) { printf("host_jrn_kids_test: %d FAILED of %lu checks\n", fails, checks); return 1; }
  printf("host_jrn_kids_test: all %lu checks passed (real lib/fatfs over a RAM disk)\n", checks);
  return 0;
}
