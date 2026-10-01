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


/* ---- step 2: the writer ------------------------------------------------------------------------------------------------------ */
static uint8_t g_old[NREG][RSZ], g_new[NREG][RSZ];
static uint32_t g_rng = 12345u;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

static void mkblk(JrnBlk* b) { unsigned r; for (r = 0; r < NREG; r++) { b[r].old_blk = g_old[r]; b[r].new_blk = g_new[r]; } }

/* g_new = g_old with `runs` runs of `len` bytes (every byte of a run DIFFERS from the old one, so a run is one run), placed at region/offset
 * spaced far apart (> JRN_MERGE_GAP) so runs never merge. Returns the bytes changed. */
static unsigned gen_runs(unsigned runs, unsigned len, unsigned first_region) {
  unsigned i, k, tot = 0;
  memcpy(g_old, g_img, sizeof g_old);
  memcpy(g_new, g_img, sizeof g_new);
  for (i = 0; i < runs; i++) {
    unsigned reg = (first_region + i / 10u) % NREG, off = 16u + (i % 10u) * 390u;
    for (k = 0; k < len; k++) { g_new[reg][off + k] = (uint8_t)(g_old[reg][off + k] ^ (uint8_t)(1u + (rnd() & 0x7Eu))); tot++; }
  }
  return tot;
}

typedef struct { uint32_t off, len; JrnRec r; } RecAt;
/* every record of segment `seg` from `from` for `n` records, via the raw bytes */
static unsigned read_recs(unsigned seg, uint32_t from, unsigned max, RecAt* out, uint8_t* seg_bytes) {
  unsigned n = 0;
  uint32_t off = from;
  CHECK(raw_read(K, seg, 0, seg_bytes, JRN_SEG_SIZE) == 0, "read the segment");
  while (n < max && off + JRN_REC_MIN <= JRN_SEG_SIZE && jrn_i_hdr_parse(seg_bytes + off, &out[n].r) == 0) {
    out[n].off = off; out[n].len = out[n].r.len;
    if (jrn_crc32_update(0, seg_bytes + off, out[n].len - 4u) != jrn_rd32(seg_bytes + off + out[n].len - 4u)) break;
    off += out[n].len; n++;
  }
  return n;
}

/* Apply every span of the chain's records to `img` (forward = after, else before). */
static int apply_raw(const uint8_t* seg_bytes, const RecAt* recs, unsigned n, uint8_t img[NREG][RSZ], int forward, unsigned* nspans) {
  unsigned i, sp, bad = 0;
  *nspans = 0;
  for (i = 0; i < n; i++) {
    const uint8_t* b = seg_bytes + recs[i].off;
    uint32_t pos = JRN_REC_HDR;
    for (sp = 0; sp < recs[i].r.nspans; sp++) {
      uint8_t reg = b[pos]; uint16_t off = jrn_rd16(b + pos + 2), len = jrn_rd16(b + pos + 4);
      if (reg >= NREG || !len || (uint32_t)off + len > RSZ || pos + JRN_SPAN_HDR + 2u * len > recs[i].len - 4u) { bad++; break; }
      memcpy(img[reg] + off, b + pos + JRN_SPAN_HDR + (forward ? len : 0u), len);
      pos += JRN_SPAN_HDR + 2u * len; (*nspans)++;
    }
    if (pos != recs[i].len - 4u) bad++;
  }
  return bad;
}

static uint8_t g_segb[JRN_SEG_SIZE];
static uint8_t g_chk[NREG][RSZ];

/* The splitter: for diff sizes from one record's worth to the capacity edge, the chain on the card reassembles BYTE-EXACTLY (old + the spans'
 * `after` = new; new + the spans' `before` = old), every record is within the cap, and the framing fields are exactly D10's. */
static void t_writer_reassembly(void) {
  static const unsigned runs[] = { 1, 1, 2, 3, 5, 7, 9, 13, 20, 25 };
  static const unsigned lens[] = { 226, 460, 300, 200, 150, 120, 100, 80, 60, 61 };
  unsigned c;
  for (c = 0; c < sizeof runs / sizeof runs[0]; c++) {
    Jrn j; JrnBlk blk[NREG]; RecAt recs[JRN_CHAIN_MAX + 1]; unsigned n, i, nsp, changed, tail0, bad;
    uint32_t head_seq;
    world(&j);
    changed = gen_runs(runs[c], lens[c], 2);
    mkblk(blk);
    tail0 = JRN_REC_BASE; head_seq = j.next_seq;   /* the tail segment (1) is v1: the chain ROLLS into the spare (2), made v2, at its first record slot */
    CHECK(jrn_chain_record(&j, "Bulk edit", 0, blk, NREG) == JRN_OK, "case %u (%u runs x %u B = %u changed): the chain records", c, runs[c], lens[c], changed);
    n = read_recs(2, tail0, JRN_CHAIN_MAX + 1, recs, g_segb);
    CHECK(n >= 2u && n <= JRN_CHAIN_MAX, "case %u: %u records on disk (a chain, within the cap)", c, n);
    CHECK(j.tail_seg == 2u && j.tail_off == recs[n - 1].off + recs[n - 1].len && j.next_seq == head_seq + n && j.cursor == head_seq && j.tip == head_seq, "case %u: the engine's cursor/tail/next_seq follow the chain", c);
    for (i = 0; i < n; i++) {
      CHECK(recs[i].len <= JRN_REC_MAX && recs[i].r.nspans >= 1u, "case %u rec %u: <= 512 B and holds spans", c, i);
      CHECK(recs[i].r.seq == head_seq + i, "case %u rec %u: consecutive seq", c, i);
      if (i == 0) CHECK(recs[0].r.kind == JRN_KIND_STEP && recs[0].r.aux == n && recs[0].r.parent == 0u, "case %u: head kind 0, aux = %u, parent = the cursor", c, n);
      else CHECK(recs[i].r.kind == JRN_KIND_PART && recs[i].r.aux == i && recs[i].r.parent == head_seq && !recs[i].r.crossed, "case %u part %u: kind 3, aux = its index, parent = the head", c, i);
      CHECK(recs[i].r.pre == recs[0].r.pre && recs[i].r.post == recs[0].r.post && strcmp(recs[i].r.name, "Bulk edit") == 0, "case %u rec %u: pre/post/name are the step's", c, i);
    }
    memcpy(g_chk, g_old, sizeof g_chk);
    bad = apply_raw(g_segb, recs, n, g_chk, 1, &nsp);
    CHECK(bad == 0 && memcmp(g_chk, g_new, sizeof g_chk) == 0, "case %u: old + every span's AFTER == new, byte for byte (%u spans, %u malformed)", c, nsp, bad);
    memcpy(g_chk, g_new, sizeof g_chk);
    bad = apply_raw(g_segb, recs, n, g_chk, 0, &nsp);
    CHECK(bad == 0 && memcmp(g_chk, g_old, sizeof g_chk) == 0, "case %u: new + every span's BEFORE == old, byte for byte", c);
    { uint8_t h1[32], h2[32];
      CHECK(raw_read(K, 1, 0, h1, 32) == 0 && h1[4] == 1 && raw_read(K, 2, 0, h2, 32) == 0 && h2[4] == 2,
            "case %u: seg 1 (had records) stays v1 -- its header is never rewritten in place; the spare became v2 for the chain: ver bytes %u / %u", c, h1[4], h2[4]); }
  }
}

/* The record-boundary edges. One run of L bytes costs 6 + 2L, so a record holds 225 of its bytes: N = ceil(L / 225), N = 1 is a PLAIN step
 * (aux 0, any segment version, the tail), N = 2..8 a chain, L = 1801 is the first TOOBIG. A TOOBIG chain writes NOTHING. */
static void t_writer_edges(void) {
  static const unsigned L[] = { 1, 224, 225, 226, 449, 450, 451, 674, 675, 676, 1799, 1800, 1801, 1802, 3000 };
  unsigned c;
  for (c = 0; c < sizeof L / sizeof L[0]; c++) {
    Jrn j; JrnBlk blk[NREG]; RecAt recs[JRN_CHAIN_MAX + 1]; unsigned n, want = (L[c] + 224u) / 225u, nsp, bad; int rc;
    uint32_t seq0, tail0; unsigned seg0;
    world(&j);
    gen_runs(1, L[c], 6);
    mkblk(blk);
    seq0 = j.next_seq; tail0 = j.tail_off; seg0 = j.tail_seg;
    rc = jrn_chain_record(&j, "Edge", 0, blk, NREG);
    if (want > JRN_CHAIN_MAX) {
      CHECK(rc == JRN_E_TOOBIG, "L=%u: %u records needed (> %u) -> JRN_E_TOOBIG, got %d", L[c], want, JRN_CHAIN_MAX, rc);
      CHECK(j.next_seq == seq0 && j.tail_off == tail0 && j.tail_seg == seg0 && j.cursor == 0u && !j.stopped, "L=%u: TOOBIG changed nothing in the engine", L[c]);
      CHECK(window_zero(tail0, 1024), "L=%u: TOOBIG wrote nothing to the card", L[c]);
      { uint8_t z[1024]; CHECK(raw_read(K, 2, JRN_REC_BASE, z, sizeof z) == 0 && z[0] == 0 && z[200] == 0, "L=%u: nor to the spare", L[c]); }
      continue;
    }
    CHECK(rc == JRN_OK, "L=%u: records (rc %d)", L[c], rc);
    n = read_recs(want == 1u ? 1 : 2, want == 1u ? tail0 : JRN_REC_BASE, JRN_CHAIN_MAX + 1, recs, g_segb);
    if (want == 1u) n = 1;
    CHECK(n == want, "L=%u: %u records on disk, want %u", L[c], n, want);
    CHECK(want == 1u ? (recs[0].r.aux == 0u && j.tail_seg == 1u) : (recs[0].r.aux == want && j.tail_seg == 2u), "L=%u: N=1 is a plain step in the tail (aux 0); N>=2 a chain in the spare (aux N)", L[c]);
    memcpy(g_chk, g_old, sizeof g_chk);
    bad = apply_raw(g_segb, recs, n, g_chk, 1, &nsp);
    CHECK(bad == 0 && memcmp(g_chk, g_new, sizeof g_chk) == 0, "L=%u: reassembly byte-exact (after)", L[c]);
    memcpy(g_chk, g_new, sizeof g_chk);
    bad = apply_raw(g_segb, recs, n, g_chk, 0, &nsp);
    CHECK(bad == 0 && memcmp(g_chk, g_old, sizeof g_chk) == 0, "L=%u: reassembly byte-exact (before)", L[c]);
  }
}

/* A long mixed session: plain steps and chains interleaved until the chains roll into a THIRD segment (made v2 mid-session), then a fresh
 * open reads it all: every head's seq, the cursor, the tail, the per-segment versions. A chain with a step still pending refuses (FULL). */
static void t_writer_session(void) {
  Jrn j; JrnBlk blk[NREG]; unsigned it, chains = 0, plains = 0; uint32_t last_head = 0; uint8_t h[32];
  world(&j);
  for (it = 0; it < 400 && j.tail_seg < 3u; it++) {
    if (it % 3u == 0u) { CHECK(stage(&j, "plain", 0, (uint8_t)(1u + it % 5u), (uint16_t)(20u + it), 4, fresh_val((uint8_t)(1u + it % 5u), (uint16_t)(20u + it))) == JRN_OK, "plain %u", it); plains++; }
    gen_runs(4, 150, 7);
    mkblk(blk);
    if (jrn_pending(&j)) {
      CHECK(jrn_chain_record(&j, "Chain", 0, blk, NREG) == JRN_E_FULL && jrn_flush_wanted(&j), "a chain with a step pending refuses (FULL, flush wanted)");
      CHECK(jrn_flush(&j) == JRN_OK, "flush");
    }
    last_head = j.next_seq;
    CHECK(jrn_chain_record(&j, "Chain", 0, blk, NREG) == JRN_OK, "chain %u", it);
    memcpy(g_img, g_new, sizeof g_img);
    chains++;
    CHECK(j.cursor == last_head, "cursor on the head");
  }
  CHECK(j.tail_seg == 3u && chains > 20u, "the chains rolled into segment 3 (tail %u after %u chains, %u plain steps)", j.tail_seg, chains, plains);
  CHECK(raw_read(K, 1, 0, h, 32) == 0 && h[4] == 1 && raw_read(K, 2, 0, h, 32) == 0 && h[4] == 2 && raw_read(K, 3, 0, h, 32) == 0 && h[4] == 2,
        "versions: seg 1 v1 (plain-only), seg 2 v2, seg 3 v2 (made mid-session for the chain)");
  { uint32_t nxt = j.next_seq, cur = j.cursor, toff = j.tail_off; unsigned ts = j.tail_seg;
    card_remount();
    CHECK(jopen(&j, K) == JRN_OK && j.next_seq == nxt && j.cursor == cur && j.tip == cur && j.tail_seg == ts && j.tail_off == toff && j.anchor == JRN_ANCHOR_MATCH,
          "a fresh open reads the whole mixed journal: next_seq %u/%u cursor %u/%u tail %u@%u / %u@%u", (unsigned)j.next_seq, (unsigned)nxt, (unsigned)j.cursor, (unsigned)cur, j.tail_seg, (unsigned)j.tail_off, ts, (unsigned)toff); }
  CHECK(jrn_prepare(&j) == JRN_OK, "prepare");
  CHECK(raw_read(K, j.seg_last, 0, h, 32) == 0 && h[4] == 2, "the next spare INHERITS v2 (a journal that went v2 stays v2): seg %u ver %u", j.seg_last, h[4]);
}

int main(void) {
  t_reader_v1_unchanged();
  t_reader_whole_chain();
  t_reader_incomplete();
  t_reader_oversize();
  t_writer_reassembly();
  t_writer_edges();
  t_writer_session();
  if (fails) { printf("host_jrn_chain_test: %d FAILED of %lu checks\n", fails, checks); return 1; }
  printf("host_jrn_chain_test: all %lu checks passed (real lib/fatfs over a RAM disk)\n", checks);
  return 0;
}
