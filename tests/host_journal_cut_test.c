/* THE POWER-CUT SWEEP for the #234 undo journal (slice 1's centre of gravity).
 *
 *   cc -std=c11 -DFF_USE_MKFS=1 -Dsiprintf=sprintf -Dsniprintf=snprintf -Dvsniprintf=vsnprintf -I tests/hostfat -I lib/fatfs -I source \
 *      tests/host_journal_cut_test.c source/journal.c source/journal_undo.c source/journal_fs.c \
 *      lib/fatfs/ff.c lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c -o /tmp/hjc
 *   /tmp/hjc            # FAT16 + exFAT + FAT32, every scenario, every cut point
 *   JRN_QUICK=1 /tmp/hjc   # FAT16 only
 *
 * WHAT A CUT IS. tests/host_savefat_test.c models "one honest error" (rd_fail_at: a write
 * returns RES_ERROR, the card recovers, the code runs on). That is NOT a power cut. Here the
 * RAM disk's rd_cut_sectors lets N sectors of an operation land, then the next sector lands
 * TORN (its first rd_cut_torn bytes new, the rest old) and nothing after it lands at all;
 * the test remounts the volume and reads what the dead process left. For EVERY journal
 * operation that touches the card -- a step flush, a multi-record batch flush, a flush that
 * crosses into the next segment, the first fill (ring of slot files + first segment, also completing
 * a partial fill), spare-segment activation (an in-place header write, also RECYCLING a retired
 * slot of a wrapped ring), compaction (RETIRE the oldest by zeroing its header in place, alone and
 * inside a prepare on a full ring), a .pdr redirect, the cursor marker an undo records, the
 * discarded marker, and the torn-tail zeroing an open performs -- and at EVERY sector boundary
 * of that operation, with and without a torn last sector (5 tear widths, 16 B = a half-zeroed
 * header), it asserts:
 *   (a) the volume mounts;
 *   (b) a co-resident .sav is byte-identical;
 *   (c) the journal reads as EXACTLY its before-state or its after-state (first + spare
 *       creation is two atomic segment creations, so its accepted set is before / one
 *       segment / both -- nothing else);
 *   (d) [t_frozen] with the timestamp hook held, a flush changes ONLY the segment's own data
 *       sectors: the directory entry sector is byte-identical (and, as the control, with
 *       the hook OFF the same flush DOES rewrite it -- so the check can fail);
 *   (e) the journal is still usable: reopen, prepare, record a step, flush, reopen, undo.
 */
#include "jrn_harness.h"
#include "journal_int.h"   /* jrn_wr32 for the composite redirect state */
#include <string.h>

#define T0 0x5A2A6800u
#define T1 0x5A2A6C55u   /* a later live clock: get_fattime WOULD change the directory entry */

typedef struct Scn {
  const char* name;
  void     (*setup)(void);
  int      (*op)(void);
  uint32_t (*state)(char* dbg, size_t n);
  int      may_equal;          /* the operation is a repair: before == after is legitimate */
  int      dirmut;             /* 1: FIRST FILL creates directory entries (any exFAT hazard is COUNTED and must be 0);
                                * 2: the .pdr redirect create (the accepted residual, counted apart) */
  void     (*mid)(uint32_t* out, int* n);   /* extra accepted states (multi-step ops)      */
} Scn;

/* THE TWO TEAR MODELS (D3 ruling 6). The SHORN pass (default; rd_cut_garbage = 0) is the safety case: the torn sector
 * is a new prefix over the OLD suffix. It must show 0 violations. The GARBAGE pass writes noise over the unwritten
 * suffix (rd_cut_garbage = 1): that is NOT asserted zero -- D3 states the shorn assumption and hardware owes the
 * real tear shape -- each scenario's residual is COUNTED per class and printed. SCHECK asserts in the shorn pass and
 * tallies in the garbage pass. */
enum { GC_MOUNT, GC_SAV, GC_STATE, GC_REOPEN, GC_PREPARE, GC_RECORD, GC_UNDO, GC_SAV2, GC_EXFAT, GC_N };
static int g_garbage = 0;
static unsigned long g_gtal[32][GC_N];
static int g_si = 0;   /* the scenario index the current run belongs to */
#define SCHECK(cls, c, ...) do { checks++; if (!(c)) { if (g_garbage) g_gtal[g_si][cls]++; else { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } } while (0)

static const uint64_t K = 0x00C0FFEE12345678ull;
static const uint64_t KB = 0x0BADF00D0BADF00Dull;

static uint32_t st_default(char* dbg, size_t n) { return jr_fingerprint(K, dbg, n); }

/* ---- helpers to build states uncut ---------------------------------------------------------- */
static void open_prep(Jrn* j) {
  CHECK(jopen(j, K) == JRN_OK, "setup open");
  CHECK(jrn_prepare(j) == JRN_OK, "setup prepare");
}
static void step_flush(Jrn* j, uint8_t reg, uint16_t off, uint16_t n) {
  CHECK(stage(j, "st", 0, reg, off, n, fresh_val(reg, off)) == JRN_OK && jrn_flush(j) == JRN_OK, "setup step");
}
/* Leave 242..483 bytes in the tail segment: room for ONE 242-byte record (a 90-byte change) but
 * not two, so a two-record batch has to spill its second record into the next segment. The
 * pending buffer holds 512 B, so a batch can never be more than two of these. */
static void fill_seg1(Jrn* j, unsigned unused) {
  unsigned g;
  (void)unused;
  for (g = 0; g < 200 && JRN_SEG_SIZE - j->tail_off >= 1000; g++) step_flush(j, 1, 0, 220);
  for (g = 0; g < 10 && JRN_SEG_SIZE - j->tail_off >= 484; g++) step_flush(j, 1, 0, 90);
}

/* ---- scenarios: each op opens its own journal from the card, exactly as a fresh boot --------- */
static void su_none(void) {}
static void su_prepared(void) { Jrn j; open_prep(&j); }
static void su_three(void) { Jrn j; open_prep(&j); step_flush(&j, 1, 10, 4); step_flush(&j, 2, 10, 4); step_flush(&j, 3, 10, 4); }

static int op_append1(void) {
  Jrn j; int rc = jopen(&j, K);
  if (!rc) rc = stage(&j, "Cur HP 23->31", 0, 1, 10, 4, fresh_val(1, 10));
  if (!rc) rc = jrn_flush(&j);
  return rc;
}
static int op_batch3(void) {
  Jrn j; int rc = jopen(&j, K);
  if (!rc) rc = stage(&j, "a", 0, 1, 10, 4, fresh_val(1, 10));
  if (!rc) rc = stage(&j, "b", 0, 2, 10, 4, fresh_val(2, 10));
  if (!rc) rc = stage(&j, "c", 0, 3, 10, 4, fresh_val(3, 10));
  if (!rc) rc = jrn_flush(&j);
  return rc;
}
static void su_nearfull(void) { Jrn j; open_prep(&j); fill_seg1(&j, 0); }
static int op_cross(void) {   /* two 242-byte records: r1 fits seg 1, r2 goes to seg 2, written FIRST */
  Jrn j; int rc = jopen(&j, K);
  if (!rc) rc = stage(&j, "a", 0, 1, 0, 90, fresh_val(1, 0));
  if (!rc) rc = stage(&j, "b", 0, 2, 0, 90, fresh_val(2, 0));
  if (!rc) rc = jrn_flush(&j);
  return rc;
}
static int op_prepare(void) { Jrn j; int rc = jopen(&j, K); if (!rc) rc = jrn_prepare(&j); return rc; }
static int op_prepare_first(void) { Jrn j; int rc = jopen(&j, K); if (!rc) rc = jrn_prepare_first(&j); return rc; }
static void mid_prepare_fresh(uint32_t* out, int* n) {   /* prepare = first segment, THEN the spare: one intermediate state */
  op_prepare_first();
  out[(*n)++] = jr_fingerprint(K, 0, 0);
}
static void su_first_only(void) { Jrn j; CHECK(jopen(&j, K) == JRN_OK && jrn_prepare_first(&j) == JRN_OK, "setup first segment"); }
/* A cut leaves an INCOMPLETE ring: the setup cuts the first fill half way (a partial slot file, no header). */
static void su_partial_seg(void) {
  Jrn j; CHECK(jopen(&j, K) == JRN_OK, "open");
  rd_cut_sectors = 15; rd_cut_torn = 0;   /* the staged fill is ~33 sectors of chain + header zeroing for 4 slots: 15 lands mid-ring */
  (void)jrn_prepare_first(&j);
  CHECK(rd_cut_fired, "setup: the first fill was cut part way (it wrote fewer than 15 sectors)");
  rd_cut_sectors = -1; rd_cut_fired = 0;
}
/* Fill segment `seg` (make it the tail's predecessor) then make sure the spare exists. */
static void fill_to(Jrn* j, unsigned tail) {
  unsigned g;
  for (g = 0; g < 400 && j->tail_seg < tail; g++) step_flush(j, 1, 0, 220);
  CHECK(j->tail_seg == tail, "tail reached segment %u (%u)", tail, j->tail_seg);
}
/* A WRAPPED ring (ring 4): segments 1..4 lived, compaction retired 1, the tail is in 4, so the
 * next spare (logical 5) RECYCLES slot 1, whose body still holds segment 1's old records. */
static void su_wrapped(void) {
  Jrn j; unsigned g; (void)g;
  open_prep(&j);
  fill_to(&j, 2); CHECK(jrn_prepare(&j) == JRN_OK, "spare 3");
  fill_to(&j, 3); CHECK(jrn_prepare(&j) == JRN_OK && j.seg_last == 4, "spare 4");
  { JrnCfg c = cfg_for(K, 2, 0); Jrn k; CHECK(jrn_open(&k, &c, &IMG) == JRN_OK && jrn_compact(&k) == JRN_OK && k.seg_first == 3, "compact to cap 2"); }
  CHECK(jopen(&j, K) == JRN_OK && j.seg_first == 3 && j.seg_last == 4, "reopen 3..4 (%u..%u)", j.seg_first, j.seg_last);
  fill_to(&j, 4);
  CHECK(j.seg_last == 4 && j.tail_seg == 4, "tail in the last live segment (%u/%u)", j.tail_seg, j.seg_last);
}
/* A FULL ring: all 4 slots live, the tail in the last: the next prepare must retire 1 then recycle. */
static void su_fullring(void) {
  Jrn j;
  open_prep(&j);
  fill_to(&j, 2); CHECK(jrn_prepare(&j) == JRN_OK, "spare 3");
  fill_to(&j, 3); CHECK(jrn_prepare(&j) == JRN_OK, "spare 4");
  fill_to(&j, 4);
  CHECK(j.seg_first == 1 && j.seg_last == 4 && j.tail_seg == 4, "ring full %u..%u tail %u", j.seg_first, j.seg_last, j.tail_seg);
}
static void mid_fullring(uint32_t* out, int* n) {   /* retire first, THEN recycle: one intermediate state */
  Jrn j; int rc = jopen(&j, K);
  if (!rc) rc = jrn_compact(&j);
  CHECK(rc == JRN_OK && j.seg_first == 2, "mid: retire only");
  out[(*n)++] = jr_fingerprint(K, 0, 0);
}
static int op_redirect(void) { return jrn_redirect_write(&jrn_fatfs, ROOT, KB, K); }
/* The redirect scenario's state is the resolve result AND K's own journal fingerprint: a torn create of the
 * .pdr that hid or changed ANOTHER journal (the reviewer's co-located-.pdr finding: 5 of 35 exFAT runs
 * hid other saves' journals) would show as a state that is neither before nor after. Setup: K's journal
 * exists (its first fill also made <root>/r/), so the create under test is the .pdr FILE alone. */
static uint32_t st_redirect(char* dbg, size_t n) {
  uint64_t out = 0; int rc = jrn_key_resolve(&jrn_fatfs, ROOT, KB, &out);
  char d2[96]; uint32_t fp = jr_fingerprint(K, d2, sizeof d2), code = rc == 0 ? (out == K ? 0xA1u : out == KB ? 0xB2u : 0xC3u) : 0xE4u;
  uint8_t b[8];
  jrn_wr32(b, code); jrn_wr32(b + 4, fp);
  if (dbg) snprintf(dbg, n, "resolve(B) rc %d -> %s; K's journal [%s]", rc, out == K ? "A" : out == KB ? "B(self)" : "?", d2);
  return jrn_crc32_update(0, b, 8);
}
static int op_undo_marker(void) {
  Jrn j; int rc = jopen(&j, K);
  if (!rc) rc = jrn_undo(&j, &IMG, 0);
  if (!rc) rc = jrn_flush(&j);
  return rc;
}
static int op_discard(void) {
  Jrn j; int rc = jopen(&j, K);
  if (!rc) rc = jrn_mark_discarded(&j);
  if (!rc) rc = jrn_flush(&j);
  return rc;
}
static void su_three_lost(void) {         /* steps recorded, but the image is the pre-session one */
  Jrn j; static uint8_t st0[NREG][RSZ];
  memcpy(st0, g_img, sizeof g_img);
  open_prep(&j); step_flush(&j, 1, 10, 4); step_flush(&j, 2, 10, 4);
  memcpy(g_img, st0, sizeof g_img);
}
static void su_three_seg(void) {          /* 3 segments: seg 1 full, seg 2 tail, seg 3 spare */
  Jrn j; unsigned g;
  open_prep(&j);
  for (g = 0; g < 140 && j.tail_seg == 1; g++) { step_flush(&j, 1, 0, 220); }
  CHECK(jrn_prepare(&j) == JRN_OK && j.seg_last == 3, "third segment");
}
static int op_compact(void) {   /* cap 2 over a 3-live journal: retires segment 1 IN PLACE */
  Jrn j; JrnCfg c = cfg_for(K, 2, 0); int rc = jrn_open(&j, &c, &IMG);
  if (!rc) rc = jrn_compact(&j);
  return rc;
}
static void su_torn_tail(void) {
  Jrn j; uint8_t junk[900]; open_prep(&j); step_flush(&j, 1, 10, 4);
  memset(junk, 0xA5, sizeof junk); memcpy(junk, "PDJR", 4); junk[4] = 100; junk[5] = 0;
  CHECK(raw_write(K, 1, j.tail_off, junk, sizeof junk) == 0, "plant torn tail");
}
static int op_open_only(void) { Jrn j; return jopen(&j, K); }

/* ---- z9: CHAINED steps (D10). A chain is written parts-first, HEAD LAST, into a v2 segment: a cut at any sector must leave the journal
 * reading as exactly its before-state or its after-state (the head's last sector is the one commit point), and the orphan parts a cut leaves
 * behind must be gone after the next open (checked in recover_and_use). The first chain into a v1 journal ALSO retires the empty v1 spare and
 * re-stamps it as v2 (no body zero-fill) -- both header writes are the existing atomic ones -- so that scenario has one more accepted state (no spare). A chain
 * never CREATES a spare mid-session (that is a zero-filled activation, a safe-moment job), so there is no "no spare" chain scenario: it is refused untouched
 * (host_jrn_chain_test). ---- */
static uint8_t g_cold[NREG][RSZ], g_cnew[NREG][RSZ];
static int chain_diff_op(unsigned runs, unsigned len, unsigned region0, const char* name) {
  Jrn j; JrnBlk blk[NREG]; unsigned i, k, r; int rc = jopen(&j, K);
  if (rc) return rc;
  memcpy(g_cold, g_img, sizeof g_cold); memcpy(g_cnew, g_img, sizeof g_cnew);
  for (i = 0; i < runs; i++) { unsigned reg = (region0 + i / 10u) % NREG, off = 16u + (i % 10u) * 390u; for (k = 0; k < len; k++) g_cnew[reg][off + k] ^= (uint8_t)(0x21u + (i & 7u)); }
  for (r = 0; r < NREG; r++) { blk[r].old_blk = g_cold[r]; blk[r].new_blk = g_cnew[r]; }
  rc = jrn_chain_record(&j, name, 0, blk, NREG);
  if (rc == JRN_OK) memcpy(g_img, g_cnew, sizeof g_img);
  return rc;
}
static int op_chain_a(void) { return chain_diff_op(5, 150, 2, "Bulk edit"); }
static int op_chain_b(void) { return chain_diff_op(6, 140, 4, "Bulk two"); }
static void su_v2tail(void) { Jrn j; open_prep(&j); CHECK(chain_diff_op(4, 120, 3, "setup chain") == JRN_OK && jopen(&j, K) == JRN_OK && j.tail_seg == 2, "setup: a chain landed in the (v2) segment 2"); }
static void su_v2full(void) {
  Jrn j; unsigned g;
  su_v2tail();
  CHECK(jopen(&j, K) == JRN_OK, "reopen");
  for (g = 0; g < 200 && JRN_SEG_SIZE - j.tail_off >= 1500; g++) step_flush(&j, 1, 0, 220);
  CHECK(JRN_SEG_SIZE - j.tail_off < 1700 && jrn_prepare(&j) == JRN_OK && j.seg_last == 3, "setup: segment 2 nearly full (%u B left), spare 3 prepared (inherits v2)", (unsigned)(JRN_SEG_SIZE - j.tail_off));
}
static void mid_retired_spare(uint32_t* out, int* n) {   /* the spare was retired (header zeroed) but not yet re-activated: one intermediate state */
  uint8_t z[JRN_SEG_HDR]; memset(z, 0, sizeof z);
  CHECK(raw_write(K, 2, 0, z, sizeof z) == 0, "mid: zero the spare's header");
  out[(*n)++] = jr_fingerprint(K, 0, 0);
}

static const Scn SCN[] = {
  /* name                            setup           op                state       eq dm  mid */
  { "append 1 record",              su_prepared,    op_append1,       st_default,  0, 0, 0 },
  { "flush 3-record batch",         su_prepared,    op_batch3,        st_default,  0, 0, 0 },
  { "flush crossing into seg 2",    su_nearfull,    op_cross,         st_default,  0, 0, 0 },
  { "first fill: ring + segment 1", su_none,        op_prepare_first, st_default,  0, 1, 0 },
  { "first fill + spare (fresh)",   su_none,        op_prepare,       st_default,  0, 1, mid_prepare_fresh },
  { "activate spare segment",       su_first_only,  op_prepare,       st_default,  0, 0, 0 },
  { "complete a partial first fill",su_partial_seg, op_prepare,       st_default,  0, 1, mid_prepare_fresh },
  { "recycle a retired slot (wrap)",su_wrapped,     op_prepare,       st_default,  0, 0, 0 },
  { "prepare on a full ring",       su_fullring,    op_prepare,       st_default,  0, 0, mid_fullring },
  { ".pdr redirect write (K live)", su_three,       op_redirect,      st_redirect, 0, 2, 0 },
  { "undo cursor marker",           su_three,       op_undo_marker,   st_default,  0, 0, 0 },
  { "discarded marker",             su_three_lost,  op_discard,       st_default,  0, 0, 0 },
  { "retire oldest in place",       su_three_seg,   op_compact,       st_default,  0, 0, 0 },
  { "torn-tail zeroing on open",    su_torn_tail,   op_open_only,     st_default,  1, 0, 0 },
  /* z9 (D10): chained steps */
  { "chain -> v1 spare, made v2",   su_prepared,    op_chain_a,       st_default,  0, 0, mid_retired_spare },
  { "chain appended to a v2 tail",  su_v2tail,      op_chain_b,       st_default,  0, 0, 0 },
  { "chain rolls v2 tail -> spare", su_v2full,      op_chain_a,       st_default,  0, 0, 0 },
  { "undo of a chain (marker)",     su_v2tail,      op_undo_marker,   st_default,  0, 0, 0 },
};
#define NSCN ((int)(sizeof SCN / sizeof SCN[0]))

/* tear widths: 0 = a pure sector prefix; 16 = a header HALF written / half zeroed; 32 / 64 = one / two directory entries new (the exFAT File and
 * Stream entries land, the rest of the set does not); 256 = half a sector */
static const int TEARS[] = { 0, 16, 32, 64, 256 };
#define NTEAR ((int)(sizeof TEARS / sizeof TEARS[0]))

static unsigned long g_ops, g_points, g_runs, sc_hazA[32], sc_hazB[32];
static int tear_pos_ok(int t) { return t > 0; }

static unsigned long g_hazA, g_hazB;   /* the exFAT torn-directory-set hazard, counted: every scenario AFTER the first fill (must be 0/0) */
static unsigned long g_ffL;            /* first fill: the torn create damaged the KEY directory and the journal open FAILS LOUDLY (JRN_E_IO): nothing was live, not a hidden segment */
static unsigned long g_ffA, g_ffB;     /* the same during the FIRST FILL, the one place the directory is mutated (hidden must be 0) */
static unsigned long g_pdrA, g_pdrB;   /* the same, for the .pdr redirect create only (the accepted residual) */

/* FatFs itself can no longer list or create in the key directory (a torn exFAT entry set hides
 * what follows it and stops every later create). Probing creates a file, so call it LAST. */
static int keydir_broken(void) {
  DIR d; FILINFO fi; FIL f; char p[96], hex[17]; FRESULT fr;
  jrn_key_hex(K, hex);
  snprintf(p, sizeof p, ROOT "/%s", hex);
  if (f_opendir(&d, p) == FR_OK) {
    fr = f_readdir(&d, &fi);
    f_closedir(&d);
    if (fr != FR_OK) return 1;
  }
  snprintf(p, sizeof p, ROOT "/%s/zzprobe.bin", hex);
  fr = f_open(&f, p, FA_WRITE | FA_CREATE_NEW);
  if (fr == FR_OK) { f_close(&f); f_unlink(p); return 0; }
  return fr != FR_EXIST;
}

/* After an open, NOTHING is left past the valid tail inside the zero window: not in the tail segment, not in a spare behind it. A chain's parts are laid
 * down before its head, so a cut leaves them standing beyond the tail -- invisible to the scan, but a later record that ends exactly where one starts
 * would re-link it. The open's repair must have zeroed them all. */
static int beyond_tail_clean(const Jrn* j) {
  static uint8_t buf[JRN_CHAIN_MAX * JRN_REC_MAX];
  unsigned seg, i, n;
  if (!j->tail_seg) return 1;
  for (seg = j->tail_seg; seg <= j->seg_last; seg++) {
    uint32_t from = seg == j->tail_seg ? j->tail_off : JRN_REC_BASE;
    n = JRN_SEG_SIZE - from < sizeof buf ? JRN_SEG_SIZE - from : (unsigned)sizeof buf;
    if (raw_read(K, seg, from, buf, n) != 0) return 1;   /* an unreadable file is another check's business */
    for (i = 0; i < n; i++) if (buf[i]) return 0;
  }
  return 1;
}

/* (e) the journal must still work after the cut, whatever state the cut left. On exFAT a torn
 * directory set can make CREATING impossible (FatFs itself refuses): that is counted, must be
 * confirmed at the FatFs level, and the journal must still append into what already exists. */
static void recover_and_use(BYTE fmt, const char* what, unsigned k, int tear, int dirmut, int si) {
  Jrn j; static uint8_t keep[NREG][RSZ]; int rc; uint8_t reg = 6; uint32_t nseq0;
  memcpy(keep, g_img, sizeof g_img);
  rc = jopen(&j, K);
  nseq0 = j.next_seq;
  if (rc && fmt == FM_EXFAT && tear > 0 && dirmut == 1 && keydir_broken()) { if (g_garbage) g_gtal[si][GC_EXFAT]++; else { g_ffB++; sc_hazB[si]++; } return; }   /* a torn first-fill create damaged the key directory: the open FAILS LOUDLY (counted, accepted residual) */
  SCHECK(GC_REOPEN, rc == JRN_OK, "%s k=%u tear=%d: reopen after the cut -> %d", what, k, tear, rc);
  if (rc) return;
  SCHECK(GC_REOPEN, beyond_tail_clean(&j), "%s k=%u tear=%d: bytes survive past the tail after the open (an orphan chain part: it could be re-linked behind a later record)", what, k, tear);
  rc = jrn_prepare(&j);
  if (rc && fmt == FM_EXFAT && tear > 0 && keydir_broken()) {
    if (getenv("HAZ")) printf("HAZB %s k=%u tear=%d rc=%d seg %u..%u tail %u\n", what, k, tear, rc, j.seg_first, j.seg_last, j.tail_seg);
    if (g_garbage) { g_gtal[si][GC_EXFAT]++; return; }
    if (dirmut == 2) g_pdrB++; else if (dirmut == 1) g_ffB++; else g_hazB++;
    sc_hazB[si]++;
    rc = j.tail_seg ? (stage(&j, "post-cut", 0, reg, 20, 4, fresh_val(reg, 20)) || jrn_flush(&j)) : (jrn_flush(&j) == JRN_E_NOSEG ? 0 : 1);
    SCHECK(GC_RECORD, rc == 0 || j.tail_seg == 0, "%s k=%u tear=%d: exFAT blocked dir: the journal must keep appending into existing space", what, k, tear);
    return;
  }
  SCHECK(GC_PREPARE, rc == JRN_OK, "%s k=%u tear=%d: prepare after the cut -> %d", what, k, tear, rc);
  if (rc) return;
  rc = stage(&j, "post-cut", 0, reg, 20, 4, fresh_val(reg, 20));
  if (!rc) rc = jrn_flush(&j);
  SCHECK(GC_RECORD, rc == JRN_OK, "%s k=%u tear=%d: record+flush after the cut -> %d", what, k, tear, rc);
  if (rc) return;
  rc = jopen(&j, K);
  /* CONTINUITY (the reviewer's test gap): one post-cut step is exactly ONE more record. An orphan of the cut that
   * repair left in place (e.g. a record a crossing flush wrote into the NEXT segment) would be RE-LINKED behind the
   * new step by seq continuity and inflate the count. */
  SCHECK(GC_RECORD, rc == JRN_OK && j.next_seq == nseq0 + 1u, "%s k=%u tear=%d: a post-cut step added %d records (next_seq %u -> %u): an orphan was re-linked", what, k, tear, rc ? -1 : (int)(j.next_seq - nseq0), (unsigned)nseq0, (unsigned)j.next_seq);
  if (!rc) rc = jrn_undo(&j, &IMG, 0);
  SCHECK(GC_UNDO, rc == JRN_OK && memcmp(keep, g_img, sizeof g_img) == 0, "%s k=%u tear=%d: undo after the cut -> %d", what, k, tear, rc);
}

static uint32_t empty_fp(void) {   /* fingerprint of a journal with no visible segment */
  uint8_t h[8] = { 0, 0, 0, 0, 0, 0, 1, 0 };
  return jrn_crc32_update(0, h, 8);
}

static void sweep_one(BYTE fmt, const Scn* sc, int garbage) {
  static uint8_t img0[NREG][RSZ];
  uint32_t before, after, extra[4]; int nextra = 0, t, i;
  unsigned long w0, W, k; char d1[96], d2[96], d3[96];
  g_garbage = garbage; g_si = (int)(sc - SCN);
  card_fresh(fmt); img_fill(1);
  rd_fattime_hook = jrn_fattime_filter; rd_fattime_now = T0;
  sc->setup();
  card_remount();
  rd_snapshot();
  memcpy(img0, g_img, sizeof g_img);
  rd_fattime_now = T1;
  before = sc->state(d1, sizeof d1);
  if (sc->mid) { sc->mid(extra, &nextra); rd_restore(); memcpy(g_img, img0, sizeof g_img); card_remount(); }
  w0 = rd_writes;
  CHECK(sc->op() == JRN_OK, "%s: the uncut op must succeed", sc->name);
  W = rd_writes - w0;
  card_remount();
  after = sc->state(d2, sizeof d2);
  CHECK(sav_intact(), "%s: .sav after the uncut op", sc->name);
  if (!sc->may_equal) CHECK(after != before, "%s: the op changed nothing (%s vs %s) -- a vacuous scenario", sc->name, d1, d2);
  CHECK(W > 0, "%s: the op wrote no sectors", sc->name);
  if (!garbage) g_ops++;
  for (k = 0; k < W; k++)
    for (t = 0; t < NTEAR; t++) {
      uint32_t got; int ok;
      rd_restore(); memcpy(g_img, img0, sizeof g_img);
      card_remount();
      rd_fattime_now = T1;
      rd_cut_sectors = (long)k; rd_cut_torn = TEARS[t]; rd_cut_garbage = garbage;
      (void)sc->op();                                 /* it dies mid-way: the verdict is whatever the card holds */
      CHECK(rd_cut_fired, "%s k=%lu/%lu tear=%d: the cut never fired (the op wrote fewer sectors than measured)", sc->name, k, W, TEARS[t]);
      rd_cut_sectors = -1; rd_cut_fired = 0; rd_cut_garbage = 0;
      f_mount(0, "", 0);
      ok = f_mount(&s_fs, "", 1) == FR_OK;
      SCHECK(GC_MOUNT, ok, "%s k=%lu/%lu tear=%d: (a) the volume does not mount", sc->name, k, W, TEARS[t]);
      if (!ok) continue;
      SCHECK(GC_SAV, sav_intact(), "%s k=%lu/%lu tear=%d: (b) the co-resident .sav changed", sc->name, k, W, TEARS[t]);
      got = sc->state(d3, sizeof d3);
      ok = got == before || got == after;
      for (i = 0; i < nextra; i++) ok = ok || got == extra[i];
      if (!ok && fmt == FM_EXFAT && tear_pos_ok(TEARS[t]) && sc->dirmut && (got == empty_fp() || got == 0xDEAD0001u) && keydir_broken()) {   /* empty view, or the honest loud open failure of a damaged directory */
        if (garbage) g_gtal[sc - SCN][GC_EXFAT]++;
        else if (got == 0xDEAD0001u) g_ffL++;
        else if (sc->dirmut == 2) g_pdrA++; else if (sc->dirmut == 1) g_ffA++; else g_hazA++;   /* exFAT: a torn entry set hid the live segments */
        ok = 1;
        if (!garbage) sc_hazA[sc - SCN]++;
      }
      if (garbage && !ok && getenv("JRN_VERBOSE")) printf("      garbage k=%lu tear=%d: [%s] (before [%s], after [%s])\n", k, TEARS[t], d3, d1, d2);
      SCHECK(GC_STATE, ok, "%s k=%lu/%lu tear=%d: (c) journal is neither before nor after: [%s] (before [%s], after [%s])", sc->name, k, W, TEARS[t], d3, d1, d2);
      recover_and_use(fmt, sc->name, (unsigned)k, TEARS[t], sc->dirmut, (int)(sc - SCN));
      SCHECK(GC_SAV2, sav_intact(), "%s k=%lu/%lu tear=%d: (b) .sav after recovery", sc->name, k, W, TEARS[t]);
      if (!garbage) g_runs++;
    }
  if (garbage) {
    unsigned long* q = g_gtal[sc - SCN];
    printf("    %-30s %4lu sectors x %d tears: garbage residual  mount %lu  sav %lu  state(neither before/after) %lu  reopen %lu  prepare %lu  record %lu  undo %lu  sav-after %lu  exfat-dir %lu\n",
           sc->name, W, NTEAR, q[GC_MOUNT], q[GC_SAV], q[GC_STATE], q[GC_REOPEN], q[GC_PREPARE], q[GC_RECORD], q[GC_UNDO], q[GC_SAV2], q[GC_EXFAT]);
    return;
  }
  g_points += W;
  printf("    %-30s %4lu sectors x %d tears  before=%08x after=%08x  [%s -> %s]\n", sc->name, W, NTEAR, before, after, d1, d2);
  if (fmt == FM_EXFAT && (sc_hazA[sc - SCN] || sc_hazB[sc - SCN])) printf("      (exFAT, running total for this scenario: %lu hidden-segment, %lu blocked-create cut runs)\n", sc_hazA[sc - SCN], sc_hazB[sc - SCN]);
}

/* Which sectors did the last operation change, and are they ALL data sectors of `seg`? */
static int seg_lba0(uint64_t key, unsigned seg, unsigned* lba0) {
  char p[96], hex[17]; FIL f;
  jrn_key_hex(key, hex); snprintf(p, sizeof p, ROOT "/%s/%04u.pdj", hex, seg);
  if (f_open(&f, p, FA_READ) != FR_OK) return 0;
  *lba0 = (unsigned)(f.obj.fs->database + (LBA_t)f.obj.fs->csize * (f.obj.sclust - 2));
  f_close(&f);
  return 1;
}
static unsigned outside_data(unsigned lo1, unsigned n1, unsigned lo2, unsigned n2) {
  static unsigned ch[4096]; unsigned n = rd_changed(ch, 4096), i, bad = 0;
  for (i = 0; i < n && i < 4096; i++) {
    int in1 = ch[i] >= lo1 && ch[i] < lo1 + n1, in2 = n2 && ch[i] >= lo2 && ch[i] < lo2 + n2;
    if (!in1 && !in2) bad++;
  }
  return bad;
}

static void t_frozen(BYTE fmt, const char* fname) {
  Jrn j; unsigned lo1 = 0, lo2 = 0, bad; const unsigned SEC = JRN_SEG_SIZE / 512u;
  card_fresh(fmt); img_fill(1);
  rd_fattime_hook = jrn_fattime_filter; rd_fattime_now = T0;
  open_prep(&j);
  CHECK(seg_lba0(K, 1, &lo1) && seg_lba0(K, 2, &lo2), "%s: segment lbas", fname);
  card_remount(); rd_snapshot(); rd_fattime_now = T1;
  CHECK(stage(&j, "a", 0, 1, 10, 4, fresh_val(1, 10)) == JRN_OK && jrn_flush(&j) == JRN_OK, "%s: flush", fname);
  bad = outside_data(lo1, SEC, 0, 0);
  CHECK(bad == 0, "%s: (d) HELD stamp: %u sector(s) outside the segment's data changed (the directory entry was rewritten)", fname, bad);
  CHECK(jrn_stamp_held() == 0, "%s: the hold is released after a flush", fname);
  rd_snapshot();
  CHECK(stage(&j, "b", 0, 1, 30, 4, fresh_val(1, 30)) == JRN_OK && stage(&j, "c", 0, 2, 30, 4, fresh_val(2, 30)) == JRN_OK &&
        stage(&j, "d", 0, 3, 30, 4, fresh_val(3, 30)) == JRN_OK && jrn_flush(&j) == JRN_OK, "%s: batch flush", fname);
  CHECK(outside_data(lo1, SEC, 0, 0) == 0, "%s: (d) held stamp across a 3-record batch", fname);
  /* the CONTROL: with the hook off the very same flush rewrites the directory entry */
  rd_snapshot(); rd_fattime_hook = 0;
  CHECK(stage(&j, "e", 0, 1, 50, 4, fresh_val(1, 50)) == JRN_OK && jrn_flush(&j) == JRN_OK, "%s: unheld flush", fname);
  bad = outside_data(lo1, SEC, 0, 0);
  CHECK(bad > 0, "%s: CONTROL: with the hook off the directory entry must change (%u) -- else check (d) proves nothing", fname, bad);
  rd_fattime_hook = jrn_fattime_filter;
  /* a flush that crosses segments touches two files, each with its own held stamp */
  rd_fattime_now = T1 + 0x40;
  fill_seg1(&j, 0); card_remount(); rd_snapshot();
  CHECK(stage(&j, "x", 0, 1, 0, 90, fresh_val(1, 0)) == JRN_OK && stage(&j, "y", 0, 2, 0, 90, fresh_val(2, 0)) == JRN_OK && jrn_flush(&j) == JRN_OK && j.tail_seg == 2, "%s: crossing flush", fname);
  CHECK(outside_data(lo1, SEC, lo2, SEC) == 0, "%s: (d) held stamps across a two-segment flush", fname);
  /* the torn-tail zeroing an open performs holds the stamp as well */
  { uint8_t junk[300]; memset(junk, 0xA5, sizeof junk); memcpy(junk, "PDJR", 4); junk[4] = 100; junk[5] = 0;
    CHECK(raw_write(K, 2, j.tail_off, junk, sizeof junk) == 0, "plant");
    rd_fattime_now = T1 + 0x80; card_remount(); rd_snapshot();
    CHECK(jopen(&j, K) == JRN_OK, "open with a torn tail");
    CHECK(outside_data(lo1, SEC, lo2, SEC) == 0, "%s: (d) the in-place zeroing keeps the directory entry byte-identical", fname); }
  printf("    frozen-timestamp check on %s: directory entry byte-identical for single flush, batch, 2-segment flush, zeroing; control (hook off) rewrites it\n", fname);
}


/* THE RING'S CONTRACT, byte level: once the first fill is done, wrapping the journal round its
 * slots (activate, recycle, retire, dozens of appends) changes ONLY the slots' own data sectors:
 * no directory sector, no FAT sector, ever. Also checked: every activation/retire left the
 * directory listing (names + sizes + timestamps) identical. */
static void t_ring_nodir(BYTE fmt, const char* fname) {
  Jrn j; unsigned lo[4], i, k, bad = 0, nch, wraps0; const unsigned SEC = JRN_SEG_SIZE / 512u;
  static unsigned ch[8192];
  card_fresh(fmt); img_fill(1);
  rd_fattime_hook = jrn_fattime_filter; rd_fattime_now = T0;
  open_prep(&j);
  CHECK(j.ring == 4, "%s: ring 4 (%u)", fname, j.ring);
  for (i = 0; i < 4; i++) CHECK(seg_lba0(K, i + 1, &lo[i]), "%s: slot %u lba", fname, i + 1);
  card_remount(); rd_snapshot(); rd_fattime_now = T1;
  wraps0 = j.seg_last;
  for (k = 0; k < 900; k++) {
    rd_fattime_now = T1 + k * 0x20u;
    CHECK(stage(&j, "w", 0, 1, 0, 220, fresh_val(1, 0)) == JRN_OK && jrn_flush(&j) == JRN_OK, "%s: wrap step %u", fname, k);
    if (j.tail_seg == j.seg_last) { CHECK(jrn_prepare(&j) == JRN_OK, "%s: prepare %u", fname, k); }
    if (fails) return;
  }
  CHECK(j.seg_last >= 8 && j.seg_last > wraps0, "%s: wrapped the 4-slot ring twice over (seg_last %u)", fname, j.seg_last);
  nch = rd_changed(ch, 8192);
  for (i = 0; i < nch && i < 8192; i++) {
    int in = 0; for (k = 0; k < 4; k++) if (ch[i] >= lo[k] && ch[i] < lo[k] + SEC) in = 1;
    if (!in) bad++;
  }
  CHECK(nch > 0 && bad == 0, "%s: (ring) %u of %u changed sectors are OUTSIDE the slots' data (a directory or FAT write after the first fill)", fname, bad, nch);
  /* and one explicit retire */
  rd_snapshot();
  { JrnCfg c = cfg_for(K, 1, 0); Jrn q; CHECK(jrn_open(&q, &c, &IMG) == JRN_OK && jrn_compact(&q) == JRN_OK, "%s: retire", fname); }
  nch = rd_changed(ch, 8192); bad = 0;
  for (i = 0; i < nch && i < 8192; i++) {
    int in = 0; for (k = 0; k < 4; k++) if (ch[i] >= lo[k] && ch[i] < lo[k] + SEC) in = 1;
    if (!in) bad++;
  }
  CHECK(nch > 0 && bad == 0, "%s: (ring) a retire changes only slot data (%u outside of %u)", fname, bad, nch);
  printf("    ring check on %s: %u appends wrapping the 4-slot ring (seg %u) + a retire changed 0 sectors outside the slots' data\n", fname, 900u, j.seg_last);
}

static const struct { BYTE fmt; const char* name; } FMTS[] = { { FM_FAT, "FAT16" }, { FM_EXFAT, "exFAT" }, { FM_FAT32, "FAT32" } };

int main(int argc, char** argv) {
  int nf = getenv("JRN_QUICK") ? 1 : 3, f, i;
  (void)argc; (void)argv;
  g_jopen_segs = 3;   /* ring of 4 slot files: keeps every first-fill sweep affordable; the ring logic is size-independent */
  for (f = 0; f < nf; f++) {
    printf("== %s ==\n", FMTS[f].name);
    t_frozen(FMTS[f].fmt, FMTS[f].name);
    t_ring_nodir(FMTS[f].fmt, FMTS[f].name);
    for (i = 0; i < NSCN; i++) if (!getenv("JRN_ONLY") || strstr(SCN[i].name, getenv("JRN_ONLY"))) sweep_one(FMTS[f].fmt, &SCN[i], 0);
    if (!getenv("JRN_NO_GARBAGE")) {
      unsigned long tot[GC_N]; int c;
      printf("  -- GARBAGE tear pass (%s): the torn sector's unwritten suffix is noise. Documented residual, NOT asserted zero (D3: hardware owes the tear shape) --\n", FMTS[f].name);
      memset(g_gtal, 0, sizeof g_gtal);
      for (i = 0; i < NSCN; i++) if (!getenv("JRN_ONLY") || strstr(SCN[i].name, getenv("JRN_ONLY"))) sweep_one(FMTS[f].fmt, &SCN[i], 1);
      memset(tot, 0, sizeof tot);
      for (i = 0; i < NSCN; i++) for (c = 0; c < GC_N; c++) tot[c] += g_gtal[i][c];
      printf("  garbage residual TOTAL on %s: mount %lu  sav %lu  state %lu  reopen %lu  prepare %lu  record %lu  undo %lu  sav-after %lu  exfat-dir %lu\n", FMTS[f].name,
             tot[GC_MOUNT], tot[GC_SAV], tot[GC_STATE], tot[GC_REOPEN], tot[GC_PREPARE], tot[GC_RECORD], tot[GC_UNDO], tot[GC_SAV2], tot[GC_EXFAT]);
      g_garbage = 0;
    }
  }
  CHECK(g_hazA == 0 && g_hazB == 0, "exFAT hazard AFTER the first fill: %lu hidden-segment runs, %lu blocked-create runs (must be 0)", g_hazA, g_hazB);
  CHECK(g_ffA == 0 && g_pdrA == 0, "exFAT: a torn create hid live segments (%lu first fill, %lu .pdr) (must be 0)", g_ffA, g_pdrA);
  if (nf == 3)
    printf("  exFAT torn-directory-set hazard, every scenario after the first fill (activate, recycle, retire, appends, markers, open): %lu hidden-segment, %lu blocked-create cut runs.\n"
           "  Only the two directory CREATES remain: first fill %lu hidden / %lu blocked-create / %lu loud-open-failure (nothing was live yet), .pdr redirect (in <root>/r/, a journal live beside it) %lu hidden / %lu blocked-create (accepted residual, D3).\n"
           "  In every run the volume mounted, the .sav was intact and no corrupt record was ever read as valid.\n", g_hazA, g_hazB, g_ffA, g_ffB, g_ffL, g_pdrA, g_pdrB);
  if (fails) { printf("host_journal_cut_test: %d FAILED of %lu checks\n", fails, checks); return 1; }
  printf("host_journal_cut_test: all %lu checks passed -- %lu operations, %lu cut points (sector boundaries), %lu cut runs (x%d tears), 0 violations\n",
         checks, g_ops, g_points, g_runs, NTEAR);
  return 0;
}
