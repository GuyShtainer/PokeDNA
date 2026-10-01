/* source/journal.c + journal_undo.c (z9 / BACKLOG #301 v2 + #307): CHAINED multi-record steps, SEG_VER 2 (docs/briefs/234-UNDO-DESIGN.md D10)
 * over the REAL lib/fatfs on a RAM disk.
 *
 *   cc -std=c11 -DFF_USE_MKFS=1 -Dsiprintf=sprintf -Dsniprintf=snprintf -Dvsniprintf=vsnprintf -I tests/hostfat -I lib/fatfs -I source \
 *      tests/host_jrn_chain_test.c source/journal.c source/journal_undo.c source/journal_fs.c \
 *      lib/fatfs/ff.c lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c -o /tmp/hjch
 *
 * Every check was seen RED on a mutant of the real source (tools/journal_mutants.py lists each mutant and the check that catches it). */
#include "jrn_harness.h"
#include "journal_int.h"

static const uint64_t K = 0x2233445566778899ull;

static void world(Jrn* j) {
  card_fresh(FM_FAT);
  img_fill(3);
  rd_fattime_hook = jrn_fattime_filter;
  CHECK(jopen(j, K) == JRN_OK, "open fresh");
  CHECK(jrn_prepare(j) == JRN_OK, "prepare fresh");
}

/* ---- a hand-built record (the reader's pins must not depend on the writer under test) ---------------------------------- */
typedef struct { uint8_t region; uint16_t off, len; } HSpan;

static uint32_t mk_rec(uint8_t* b, int kind, int crossed, uint32_t seq, uint32_t parent, uint32_t aux, uint32_t pre, uint32_t post,
                       const char* name, const HSpan* sp, unsigned nsp, const uint8_t* before, const uint8_t* after) {
  uint32_t pos = JRN_REC_HDR, bo = 0;
  unsigned i;
  memset(b, 0, JRN_REC_HDR);
  jrn_wr32(b, 0x524A4450u);
  b[6] = (uint8_t)((crossed ? 1u : 0u) | ((unsigned)kind << 4));
  b[7] = (uint8_t)nsp;
  jrn_wr32(b + 8, seq); jrn_wr32(b + 12, parent); jrn_wr32(b + 16, aux); jrn_wr32(b + 20, pre); jrn_wr32(b + 24, post);
  for (i = 0; i < JRN_NAME_LEN && name[i]; i++) b[28 + i] = (uint8_t)name[i];
  for (i = 0; i < nsp; i++) {
    b[pos] = sp[i].region; b[pos + 1] = 0; jrn_wr16(b + pos + 2, sp[i].off); jrn_wr16(b + pos + 4, sp[i].len);
    memcpy(b + pos + JRN_SPAN_HDR, before + bo, sp[i].len);
    memcpy(b + pos + JRN_SPAN_HDR + sp[i].len, after + bo, sp[i].len);
    pos += JRN_SPAN_HDR + 2u * sp[i].len; bo += sp[i].len;
  }
  jrn_wr16(b + 4, (uint16_t)(pos + 4u));
  jrn_wr32(b + pos, jrn_crc32_update(0, b, pos));
  return pos + 4u;
}

static void set_hdr_ver(uint16_t ver) {   /* rewrite slot 1's header version in place, crc fixed (test-side tamper) */
  uint8_t h[32];
  CHECK(raw_read(K, 1, 0, h, 32) == 0, "read hdr");
  jrn_wr16(h + 4, ver);
  jrn_wr32(h + 28, jrn_crc32_update(0, h, 28));
  CHECK(raw_write(K, 1, 0, h, 32) == 0, "write hdr");
}

/* One chain of 3 records over regions 3 and 4, laid down raw after step 1 (seq 1). *tail = where step 1 ended, *total = the
 * chain's byte length; g_img is left at I1 (the chain's post state) and I0 is saved in `i0`. */
#define NPART 3u
#define MAXREC 9u
typedef struct { uint8_t rec[MAXREC][JRN_REC_MAX]; uint32_t len[MAXREC]; uint32_t tail, total; uint32_t pre, post; } Chain;
static uint8_t g_i0[NREG][RSZ];

static void build_chain_n(Jrn* j, Chain* c, unsigned n, uint32_t aux_head) {
  uint8_t bf[MAXREC][40], af[MAXREC][40];
  HSpan sp[MAXREC];
  unsigned k;
  CHECK(n <= MAXREC, "chain size");
  CHECK(stage(j, "first", 0, 1, 10, 4, fresh_val(1, 10)) == JRN_OK && jrn_flush(j) == JRN_OK, "step 1");
  c->tail = j->tail_off;
  memcpy(g_i0, g_img, sizeof g_i0);
  c->pre = ref_hash();
  for (k = 0; k < n; k++) {
    sp[k].region = (uint8_t)(3u + (k & 1u)); sp[k].off = (uint16_t)(100u * (k / 2u + 1u) + 8u * k); sp[k].len = 40;
    memcpy(bf[k], g_img[sp[k].region] + sp[k].off, 40);
    memset(g_img[sp[k].region] + sp[k].off, (int)(0x30 + k), 40);
    memcpy(af[k], g_img[sp[k].region] + sp[k].off, 40);
  }
  c->post = ref_hash();
  c->total = 0;
  for (k = 0; k < n; k++) {
    c->len[k] = mk_rec(c->rec[k], k ? JRN_KIND_PART : JRN_KIND_STEP, 0, 2u + k, k ? 2u : 1u, k ? k : aux_head,
                       c->pre, c->post, "Bulk", &sp[k], 1, bf[k], af[k]);
    c->total += c->len[k];
  }
}
static void build_chain(Jrn* j, Chain* c, uint32_t aux_head) { build_chain_n(j, c, NPART, aux_head); }

static void lay_chain(const Chain* c, unsigned nrec) {
  uint32_t off = c->tail;
  unsigned k;
  for (k = 0; k < nrec; k++) { CHECK(raw_write(K, 1, off, c->rec[k], c->len[k]) == 0, "lay record %u", k); off += c->len[k]; }
}

static int window_zero(uint32_t from, uint32_t n) {
  static uint8_t buf[4096];
  uint32_t i;
  if (n > sizeof buf) n = sizeof buf;
  if (raw_read(K, 1, from, buf, n) != 0) return 0;
  for (i = 0; i < n; i++) if (buf[i]) return 0;
  return 1;
}

/* ---- step 1: the reader --------------------------------------------------------------------------------------------- */
static void t_reader_whole_chain(void) {
  Jrn j; Chain c; JrnRec r;
  world(&j);
  build_chain(&j, &c, 3);
  set_hdr_ver(2);
  lay_chain(&c, 3);
  card_remount();
  CHECK(jopen(&j, K) == JRN_OK && !j.foreign && !j.readonly, "a v2 segment is LIVE for this build (not foreign)");
  CHECK(j.next_seq == 5u && j.tail_off == c.tail + c.total, "the whole chain is the valid prefix: next_seq %u (want 5) tail %u (want %u)", (unsigned)j.next_seq, (unsigned)j.tail_off, (unsigned)(c.tail + c.total));
  CHECK(j.anchor == JRN_ANCHOR_MATCH && j.cursor == 2u && j.tip == 2u && !jrn_offer(&j), "the image at I1 anchors on the HEAD (cursor 2, tip 2, no offer): anchor %d cursor %u tip %u", j.anchor, (unsigned)j.cursor, (unsigned)j.tip);
  CHECK(jrn_find(&j, 2, &r) == 0 && r.kind == JRN_KIND_STEP && r.aux == 3u && strcmp(r.name, "Bulk") == 0, "the head is found by seq: a ONE-step view of the chain");
  CHECK(jrn_find(&j, 3, &r) == JRN_E_FLOOR && jrn_find(&j, 4, &r) == JRN_E_FLOOR, "a part is not a step: find(part seq) refuses");
  /* the image back at I0: the offer is the ONE chained step */
  memcpy(g_img, g_i0, sizeof g_i0);
  CHECK(jopen(&j, K) == JRN_OK && j.cursor == 1u && j.tip == 2u && jrn_offer(&j), "the image at I0 = before the chain: cursor 1, offer 1 (cursor %u tip %u)", (unsigned)j.cursor, (unsigned)j.tip);
  { uint32_t av = 0, tot = 0;
    CHECK(jrn_redo_info(&j, &av, &tot, 0) == 0 && tot == 1u && av == 1u, "redo_info counts the chain as ONE step: total %u avail %u", (unsigned)tot, (unsigned)av); }
}

typedef struct { const char* what; unsigned nrec; int mut; } Cut;

/* Bend one thing about the chain; the reader must end the valid prefix AT THE HEAD and the open must zero the whole stale chain. */
static void t_reader_incomplete(void) {
  static const Cut cuts[] = {
    { "head alone (parts never landed)", 1, 0 }, { "head + 1 of 2 parts", 2, 0 }, { "a part with a flipped CRC bit", 3, 1 },
    { "a part with the wrong index", 3, 2 }, { "a part with the wrong parent", 3, 3 }, { "a part with a crossed flag", 3, 4 },
    { "a part whose seq is not consecutive", 3, 5 }, { "a head whose part count is 9 (> JRN_CHAIN_MAX)", 3, 6 },
    { "a head whose part count is 1", 3, 7 }, { "a head that claims 4 parts, 3 exist", 3, 8 }, { "a bare part with no head, seq in line", 1, 9 } };
  unsigned i;
  for (i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
    Jrn j; Chain c;
    world(&j);
    build_chain(&j, &c, cuts[i].mut == 6 ? 9u : cuts[i].mut == 7 ? 1u : cuts[i].mut == 8 ? 4u : 3u);
    /* re-derive records for the mutation (the builder wrote them correct): flip bytes, then re-CRC where only the VALUE is wrong */
    if (cuts[i].mut == 1) c.rec[2][60] ^= 1;
    else if (cuts[i].mut >= 2 && cuts[i].mut <= 5) {
      uint32_t l = c.len[2];
      if (cuts[i].mut == 2) jrn_wr32(c.rec[2] + 16, 7);
      if (cuts[i].mut == 3) jrn_wr32(c.rec[2] + 12, 1);
      if (cuts[i].mut == 4) c.rec[2][6] |= 1;
      if (cuts[i].mut == 5) jrn_wr32(c.rec[2] + 8, 9);
      jrn_wr32(c.rec[2] + l - 4, jrn_crc32_update(0, c.rec[2], l - 4));
    }
    if (cuts[i].mut == 9) {   /* the first part alone, wearing the seq the next record must have */
      memcpy(c.rec[0], c.rec[1], c.len[1]); c.len[0] = c.len[1];
      jrn_wr32(c.rec[0] + 8, 2);
      jrn_wr32(c.rec[0] + c.len[0] - 4, jrn_crc32_update(0, c.rec[0], c.len[0] - 4));
      c.total = c.len[0];
    }
    set_hdr_ver(2);
    lay_chain(&c, cuts[i].nrec);
    card_remount();
    CHECK(jopen(&j, K) == JRN_OK, "%s: open", cuts[i].what);
    CHECK(j.next_seq == 2u && j.tail_off == c.tail, "%s: the valid prefix ends AT THE HEAD (next_seq %u want 2, tail %u want %u)", cuts[i].what, (unsigned)j.next_seq, (unsigned)j.tail_off, (unsigned)c.tail);
    CHECK(window_zero(c.tail, c.total), "%s: the open zeroed the stale chain bytes", cuts[i].what);
    CHECK(j.anchor == JRN_ANCHOR_BRANCH || j.anchor == JRN_ANCHOR_NEWROOT || j.cursor == 1u, "%s: anchored somewhere honest", cuts[i].what);
  }
}

/* A chain longer than JRN_CHAIN_MAX is never written, so a reader must never accept one (the apply path's offset table and the open-time
 * zero window are sized by the cap): a COMPLETE, well-formed 9-record chain still ends the prefix at its head. */
static void t_reader_oversize(void) {
  Jrn j; Chain c;
  world(&j);
  build_chain_n(&j, &c, 9, 9);
  set_hdr_ver(2);
  lay_chain(&c, 9);
  card_remount();
  CHECK(jopen(&j, K) == JRN_OK && j.next_seq == 2u && j.tail_off == c.tail, "a whole 9-record chain is refused: next_seq %u tail %u (want 2 / %u)", (unsigned)j.next_seq, (unsigned)j.tail_off, (unsigned)c.tail);
  world(&j);
  build_chain_n(&j, &c, 8, 8);
  set_hdr_ver(2);
  lay_chain(&c, 8);
  card_remount();
  CHECK(jopen(&j, K) == JRN_OK && j.next_seq == 10u && j.tail_off == c.tail + c.total, "a whole 8-record chain (JRN_CHAIN_MAX) is accepted: next_seq %u", (unsigned)j.next_seq);
}

/* A v1 segment that holds only plain steps reads exactly as before (the version stamp is the only thing a v2 reader adds). */
static void t_reader_v1_unchanged(void) {
  Jrn j;
  world(&j);
  CHECK(stage(&j, "a", 0, 1, 10, 4, fresh_val(1, 10)) == JRN_OK && stage(&j, "b", 0, 2, 10, 4, fresh_val(2, 10)) == JRN_OK && jrn_flush(&j) == JRN_OK, "two steps");
  card_remount();
  CHECK(jopen(&j, K) == JRN_OK && j.next_seq == 3u && j.cursor == 2u && j.anchor == JRN_ANCHOR_MATCH, "v1 journal reads as before");
  { uint8_t h[32]; CHECK(raw_read(K, 1, 0, h, 32) == 0 && h[4] == 1 && h[5] == 0, "a plain journal is still activated as v1: ver byte %u", h[4]); }
}

int main(void) {
  t_reader_v1_unchanged();
  t_reader_whole_chain();
  t_reader_incomplete();
  t_reader_oversize();
  if (fails) { printf("host_jrn_chain_test: %d FAILED of %lu checks\n", fails, checks); return 1; }
  printf("host_jrn_chain_test: all %lu checks passed (real lib/fatfs over a RAM disk)\n", checks);
  return 0;
}
