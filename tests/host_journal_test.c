/* source/journal.c + journal_undo.c + journal_fs.c (the #234 undo journal, slice 1) over the
 * REAL lib/fatfs on a RAM disk: the format, open/anchor, batching, undo/redo, floors,
 * compaction, keys, redirects and the loud-failure paths. The power-cut sweep is its own
 * file (host_journal_cut_test.c).
 *
 *   cc -std=c11 -DFF_USE_MKFS=1 -Dsiprintf=sprintf -Dsniprintf=snprintf -Dvsniprintf=vsnprintf -I tests/hostfat -I lib/fatfs -I source \
 *      tests/host_journal_test.c source/journal.c source/journal_undo.c source/journal_fs.c \
 *      lib/fatfs/ff.c lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c -o /tmp/hjr
 *
 * Every check here was seen RED on a mutant of the real source (tools/journal_mutants.py
 * lists each mutant and the check that catches it). */
#include "jrn_harness.h"
#include "journal_int.h"   /* the test plays slice 3's history-jump by appending a cursor marker itself */

static const uint64_t K = 0x1122334455667788ull;

/* A fresh FAT16 card + a prepared journal (seg 1 = tail, seg 2 = spare) and a seeded image. */
static void world(Jrn* j, BYTE fmt) {
  card_fresh(fmt);
  img_fill(1);
  rd_fattime_hook = jrn_fattime_filter;
  CHECK(jopen(j, K) == JRN_OK, "open fresh");
  CHECK(jrn_prepare(j) == JRN_OK, "prepare fresh");
}

static void t_crc_and_keys(void) {
  static const char v[] = "123456789";
  uint32_t a = jrn_crc32_update(0, v, 9), b = jrn_crc32_update(jrn_crc32_update(0, v, 4), v + 4, 5);
  uint8_t nm[3] = { 0xC1, 0xC2, 0xC3 }, nm2[3] = { 0xC1, 0xC2, 0xC4 };
  uint64_t k = jrn_key64(nm, 3, 100, 200, 1, 0);
  char hex[17];
  CHECK(a == 0xCBF43926u, "crc32('123456789') = %08x", a);
  CHECK(a == b, "crc32 is chainable");
  CHECK(k == jrn_key64(nm, 3, 100, 200, 1, 0), "key64 deterministic");
  CHECK(k != jrn_key64(nm2, 3, 100, 200, 1, 0), "key64 sees the name");
  CHECK(k != jrn_key64(nm, 3, 101, 200, 1, 0), "key64 sees TID");
  CHECK(k != jrn_key64(nm, 3, 100, 201, 1, 0), "key64 sees SID");
  CHECK(k != jrn_key64(nm, 3, 100, 200, 0, 0), "key64 sees gender");
  CHECK(k != jrn_key64(nm, 3, 100, 200, 1, 1), "key64 sees the FRLG layout bit");
  jrn_key_hex(0x0123456789ABCDEFull, hex);
  CHECK(strcmp(hex, "0123456789abcdef") == 0, "key hex '%s'", hex);
}

/* The bytes on the card ARE the documented format. */
static void t_format(void) {
  Jrn j; uint8_t h[JRN_SEG_HDR], r[80]; uint32_t pre, post, crc;
  long sz; char p[96], hex[17];
  world(&j, FM_FAT);
  pre = jrn_hash(&j);
  CHECK(stage(&j, "Cur HP 23->31", 0, 3, 100, 4, 0xAB) == JRN_OK, "stage");
  post = jrn_hash(&j);
  CHECK(post == ref_hash(), "tracked hash == hash of the image");
  CHECK(jrn_flush(&j) == JRN_OK, "flush");
  CHECK(raw_read(K, 1, 0, h, JRN_SEG_HDR) == 0, "read seg hdr");
  CHECK(memcmp(h, "PDJS", 4) == 0 && h[4] == 1 && h[5] == 0 && h[6] == 17 && h[7] == 0 && h[8] == 1, "segment header magic/ver/ring(17 = max_segs 16 + 1)/index");
  CHECK(jrn_crc32_update(0, h, 28) == (uint32_t)(h[28] | h[29] << 8 | h[30] << 16 | (uint32_t)h[31] << 24), "segment header crc");
  CHECK(raw_read(K, 1, JRN_REC_BASE, r, sizeof r) == 0, "read record");
  CHECK(memcmp(r, "PDJR", 4) == 0, "record magic");
  CHECK(r[4] + (r[5] << 8) == 52 + 6 + 8 + 4, "record len %d", r[4] + (r[5] << 8));
  CHECK(r[6] == 0 && r[7] == 1, "flags/nspans");
  CHECK(r[8] == 1 && r[9] == 0 && r[12] == 0 && r[16] == 0, "seq 1, parent 0, aux 0");
  CHECK((uint32_t)(r[20] | r[21] << 8 | r[22] << 16 | (uint32_t)r[23] << 24) == pre, "pre_hash");
  CHECK((uint32_t)(r[24] | r[25] << 8 | r[26] << 16 | (uint32_t)r[27] << 24) == post, "post_hash");
  CHECK(memcmp(r + 28, "Cur HP 23->31", 13) == 0 && r[41] == 0, "step name");
  CHECK(r[52] == 3 && r[54] + (r[55] << 8) == 100 && r[56] + (r[57] << 8) == 4, "span region/off/len");
  crc = jrn_crc32_update(0, r, 66);
  CHECK((uint32_t)(r[66] | r[67] << 8 | r[68] << 16 | (uint32_t)r[69] << 24) == crc, "record crc32 (last 4 of the 70 B)");
  jrn_key_hex(K, hex);
  snprintf(p, sizeof p, ROOT "/%s/0001.pdj", hex);
  sz = jrn_fatfs.size(0, p);
  CHECK(sz == (long)JRN_SEG_SIZE, "segment is exactly %u B after an append (never grows): %ld", JRN_SEG_SIZE, sz);
}

static void t_roundtrip_undo_redo(void) {
  Jrn j; static uint8_t snap[4][NREG][RSZ]; int i; JrnRec u;
  world(&j, FM_FAT);
  memcpy(snap[0], g_img, sizeof g_img);
  for (i = 1; i <= 3; i++) {
    CHECK(stage(&j, "step", 0, (uint8_t)(i + 1), (uint16_t)(50 * i), 8, fresh_val((uint8_t)(i + 1), (uint16_t)(50 * i))) == JRN_OK, "stage %d", i);
    memcpy(snap[i], g_img, sizeof g_img);
  }
  CHECK(jrn_flush(&j) == JRN_OK, "flush 3");
  CHECK(jrn_pending(&j) == 0, "nothing pending after a flush");
  CHECK(jopen(&j, K) == JRN_OK, "reopen");
  CHECK(j.anchor == JRN_ANCHOR_MATCH && jrn_cursor(&j) == 3 && jrn_tip(&j) == 3, "reopen anchors on step 3 (anchor %d cursor %u)", j.anchor, (unsigned)jrn_cursor(&j));
  for (i = 3; i >= 1; i--) {
    CHECK(jrn_undo(&j, &IMG, &u) == JRN_OK && u.seq == (uint32_t)i, "undo %d", i);
    CHECK(memcmp(g_img, snap[i - 1], sizeof g_img) == 0, "image == state %d after undo", i - 1);
    CHECK(jrn_hash(&j) == ref_hash(), "tracked hash follows undo");
  }
  CHECK(jrn_undo(&j, &IMG, 0) == JRN_E_NOTHING, "undo at the root");
  for (i = 1; i <= 3; i++) {
    CHECK(jrn_redo(&j, &IMG, &u) == JRN_OK && u.seq == (uint32_t)i, "redo %d", i);
    CHECK(memcmp(g_img, snap[i], sizeof g_img) == 0, "image == state %d after redo", i);
  }
  CHECK(jrn_redo(&j, &IMG, 0) == JRN_NOOP, "redo at the tip is a no-op");
  /* the cursor markers survive a flush + reopen: undo twice, flush, reopen on that image */
  CHECK(jrn_undo(&j, &IMG, 0) == JRN_OK && jrn_undo(&j, &IMG, 0) == JRN_OK, "undo x2");
  CHECK(jrn_flush(&j) == JRN_OK, "flush the markers");
  CHECK(jopen(&j, K) == JRN_OK, "reopen after undos");
  CHECK(j.anchor == JRN_ANCHOR_MATCH && jrn_cursor(&j) == 1 && jrn_tip(&j) == 3, "cursor 1, redo tip 3 across a session (cursor %u tip %u)", (unsigned)jrn_cursor(&j), (unsigned)jrn_tip(&j));
  CHECK(jrn_offer(&j) == 0, "an undone-and-saved image is NOT a re-apply offer (the undo's cursor marker says so)");
  CHECK(jrn_redo(&j, &IMG, 0) == JRN_OK && jrn_redo(&j, &IMG, 0) == JRN_OK, "redo across a session");
  CHECK(memcmp(g_img, snap[3], sizeof g_img) == 0, "image back at state 3");
}

static void t_two_pass_no_partial(void) {
  Jrn j; static uint8_t before[NREG][RSZ], old_b[RSZ]; JrnRec u;
  world(&j, FM_FAT);
  memcpy(old_b, g_img[2], RSZ);
  memcpy(before, g_img, sizeof g_img);
  /* one step, TWO regions */
  memset(g_img[2] + 10, 0x5A, 6); memset(g_img[5] + 20, 0x6B, 6);
  CHECK(jrn_step_begin(&j, "two", 0) == JRN_OK, "begin");
  CHECK(jrn_step_region(&j, 2, old_b, g_img[2]) == JRN_OK, "region 2");
  memcpy(old_b, before[5], RSZ);
  CHECK(jrn_step_region(&j, 5, old_b, g_img[5]) == JRN_OK, "region 5");
  CHECK(jrn_step_end(&j) == JRN_OK && jrn_flush(&j) == JRN_OK, "end + flush");
  {
    static uint8_t keep[NREG][RSZ];
    g_img[5][22] ^= 0xFF;                                   /* the SECOND span no longer matches */
    memcpy(keep, g_img, sizeof g_img);
    CHECK(jrn_undo(&j, &IMG, &u) == JRN_E_DIVERGED, "undo must refuse: history diverged");
    CHECK(memcmp(keep, g_img, sizeof g_img) == 0, "pass 1 verifies EVERY span before pass 2 writes ANY (no half-applied undo)");
    CHECK(jrn_cursor(&j) == 1, "cursor unmoved after a refused undo");
  }
}

static void t_pending_pop_and_overflow(void) {
  static struct { Jrn j; uint8_t guard[256]; } w;   /* static: an overrun must show as a FAIL line, not a smashed stack */
  static uint8_t saved[NREG][RSZ]; unsigned long wr; int i; uint32_t ns;
  world(&w.j, FM_FAT);
  memset(w.guard, 0xC5, sizeof w.guard);
  CHECK(stage(&w.j, "s1", 0, 1, 10, 4, fresh_val(1, 10)) == JRN_OK && jrn_flush(&w.j) == JRN_OK, "s1 on disk");
  memcpy(saved, g_img, sizeof g_img);
  ns = w.j.next_seq;
  CHECK(stage(&w.j, "s2", 0, 1, 30, 4, fresh_val(1, 30)) == JRN_OK, "s2 pending");
  CHECK(jrn_pending(&w.j) == 1, "one pending");
  wr = rd_writes;
  CHECK(jrn_undo(&w.j, &IMG, 0) == JRN_OK, "undo of a pending step");
  CHECK(rd_writes == wr, "popping a pending step does NO SD writes");
  CHECK(jrn_pending(&w.j) == 0 && w.j.next_seq == ns && jrn_cursor(&w.j) == 1, "pending popped, seq reclaimed (next_seq %u want %u)", (unsigned)w.j.next_seq, (unsigned)ns);
  CHECK(memcmp(saved, g_img, sizeof g_img) == 0, "image restored by the pop");
  CHECK(jrn_redo(&w.j, &IMG, 0) == JRN_NOOP, "a popped step is gone, nothing to redo");
  /* overflow is LOUD and never overruns */
  CHECK(stage(&w.j, "a", 0, 2, 0, 100, 0x11) == JRN_OK, "step of 262 B: 250 B of the 512 left");
  CHECK(jrn_flush_wanted(&w.j), "the buffer asks for a flush once headroom is gone");
  {
    uint16_t pl = w.j.pend_len;
    int rc = stage(&w.j, "b", 0, 3, 0, 200, 0x22);
    CHECK(rc == JRN_E_FULL, "a 462-byte step into 250 free bytes must be refused LOUDLY (at _region, not at _begin), got %d", rc);
    CHECK(w.j.pend_len == pl && w.j.bld_len == 0, "a refused step leaves the buffer untouched");
  }
  for (i = 0; i < (int)sizeof w.guard; i++) if (w.guard[i] != 0xC5) { CHECK(0, "guard byte %d overwritten: the pending buffer overran", i); break; }
  CHECK(jrn_flush(&w.j) == JRN_OK, "flush");
  CHECK(stage(&w.j, "b", 0, 3, 0, 200, 0x22) == JRN_OK, "after the flush the same step fits");
  CHECK(jrn_flush(&w.j) == JRN_OK, "flush 2");
  CHECK(stage(&w.j, "huge", 0, 4, 0, 300, 0x33) == JRN_E_TOOBIG, "a record that can never fit is TOOBIG, not a silent drop");
}

static void t_crossed_floors(void) {
  Jrn j; static uint8_t st[4][NREG][RSZ]; JrnRec stop, r; uint32_t avail, total; int rc;
  world(&j, FM_FAT);
  memcpy(st[0], g_img, sizeof g_img);
  CHECK(stage(&j, "a", 0, 1, 10, 4, fresh_val(1, 10)) == JRN_OK, "S1");   memcpy(st[1], g_img, sizeof g_img);
  CHECK(stage(&j, "x-bank", 1, 2, 10, 4, fresh_val(2, 10)) == JRN_OK, "S2 crossed"); memcpy(st[2], g_img, sizeof g_img);
  CHECK(stage(&j, "c", 0, 3, 10, 4, fresh_val(3, 10)) == JRN_OK, "S3");   memcpy(st[3], g_img, sizeof g_img);
  CHECK(jrn_flush(&j) == JRN_OK, "flush");
  CHECK(jrn_undo(&j, &IMG, 0) == JRN_OK, "undo S3 (above the floor)");
  rc = jrn_undo(&j, &IMG, &r);
  CHECK(rc == JRN_E_CROSSED, "undo REFUSES a crossed step, got %d", rc);
  CHECK(memcmp(st[2], g_img, sizeof g_img) == 0 && jrn_cursor(&j) == 2, "a refused undo changes nothing");
  /* re-apply / redo from before the crossed record stops in front of it */
  memcpy(g_img, st[0], sizeof g_img);
  CHECK(jopen(&j, K) == JRN_OK, "reopen on the OLD image (as if the session's steps were lost)");
  CHECK(jrn_cursor(&j) == 0 && jrn_tip(&j) == 3, "anchored before S1, offer target S3 (cursor %u tip %u)", (unsigned)jrn_cursor(&j), (unsigned)jrn_tip(&j));
  CHECK(jrn_offer(&j) == 1, "steps recorded past the image ARE a re-apply offer");
  rc = jrn_redo_info(&j, &avail, &total, &stop);
  CHECK(rc == 1 && total == 3 && avail == 1 && stop.seq == 2, "redo_info: total %u avail %u blocked-by %u (rc %d)", (unsigned)total, (unsigned)avail, (unsigned)stop.seq, rc);
  CHECK(jrn_redo(&j, &IMG, 0) == JRN_OK, "re-apply S1");
  rc = jrn_redo(&j, &IMG, 0);
  CHECK(rc == JRN_E_CROSSED, "re-apply STOPS before the crossed record, got %d", rc);
  CHECK(memcmp(st[1], g_img, sizeof g_img) == 0, "image sits at S1 (stopped before the crossed step)");
  /* a crossed step that is still PENDING is a floor too */
  world(&j, FM_FAT);
  CHECK(stage(&j, "x", 1, 1, 10, 4, fresh_val(1, 10)) == JRN_OK, "pending crossed");
  CHECK(jrn_undo(&j, &IMG, 0) == JRN_E_CROSSED, "a pending crossed step is a floor as well");
}

static void t_anchor_branches(void) {
  Jrn j; static uint8_t stB[NREG][RSZ], stA[NREG][RSZ]; JrnRec r;
  world(&j, FM_FAT);
  memcpy(stA, g_img, sizeof g_img);
  CHECK(stage(&j, "toB", 0, 1, 10, 4, 0x77) == JRN_OK && jrn_flush(&j) == JRN_OK, "S1 A->B");
  memcpy(stB, g_img, sizeof g_img);
  CHECK(jrn_undo(&j, &IMG, 0) == JRN_OK, "undo to A");
  CHECK(stage(&j, "toB-again", 0, 1, 10, 4, 0x77) == JRN_OK && jrn_flush(&j) == JRN_OK, "S3 A->B on a NEW branch (same post hash as S1)");
  CHECK(jrn_find(&j, 3, &r) == JRN_OK && r.parent == 0, "S3 is a branch off the root (seq 3, parent %u)", (unsigned)r.parent);
  CHECK(jopen(&j, K) == JRN_OK, "reopen on B");
  CHECK(j.anchor == JRN_ANCHOR_MATCH && jrn_cursor(&j) == 3, "the anchor is the record ON THE LAST CURSOR PATH (S3), not the first record with that hash (cursor %u)", (unsigned)jrn_cursor(&j));
  /* an image the last path never held, but an older branch did: BRANCH */
  memcpy(g_img, stA, sizeof g_img);
  memset(g_img[4] + 40, 0x99, 4);                            /* a state no record produced */
  CHECK(jopen(&j, K) == JRN_OK && j.anchor == JRN_ANCHOR_NEWROOT && jrn_cursor(&j) == 0, "an unrecognized image starts a NEW ROOT (anchor %d)", j.anchor);
  CHECK(jrn_prepare(&j) == JRN_OK, "prepare");
  CHECK(stage(&j, "newroot", 0, 4, 60, 4, 0x55) == JRN_OK && jrn_flush(&j) == JRN_OK, "step on the new root");
  CHECK(jrn_find(&j, jrn_cursor(&j), &r) == JRN_OK && r.parent == 0, "the new root's parent is 0 (parent %u)", (unsigned)r.parent);
  /* a diverged branch point: image == an old post that is NOT on the last path */
  memcpy(g_img, stB, sizeof g_img);
  CHECK(jopen(&j, K) == JRN_OK, "reopen");
  CHECK(j.anchor == JRN_ANCHOR_BRANCH && (jrn_cursor(&j) == 1 || jrn_cursor(&j) == 3), "an older branch's record anchors as BRANCH (anchor %d cursor %u)", j.anchor, (unsigned)jrn_cursor(&j));
  CHECK(jrn_tip(&j) == jrn_cursor(&j), "no re-apply offer for a branch match");
}

/* Two records share a post hash; the cursor was last JUMPED to the OLDER one. The anchor must follow
 * the last cursor path, not "the newest record whose hash matches" (that would re-parent the next edit
 * on the wrong branch). */
static void t_anchor_follows_last_path(void) {
  Jrn j; uint32_t h;
  world(&j, FM_FAT);
  CHECK(stage(&j, "toB", 0, 1, 10, 4, 0x77) == JRN_OK && jrn_flush(&j) == JRN_OK, "S1 A->B");
  CHECK(jrn_undo(&j, &IMG, 0) == JRN_OK, "undo");
  CHECK(stage(&j, "toB2", 0, 1, 10, 4, 0x77) == JRN_OK && jrn_flush(&j) == JRN_OK, "S3 A->B on a new branch");
  h = jrn_hash(&j);
  CHECK(jrn_i_marker(&j, JRN_KIND_CURSOR, 1, 1, h, h, "jump") == JRN_OK && jrn_flush(&j) == JRN_OK, "a history jump back to S1");
  CHECK(jopen(&j, K) == JRN_OK, "reopen on the B image");
  CHECK(j.anchor == JRN_ANCHOR_MATCH && jrn_cursor(&j) == 1, "anchor follows the LAST CURSOR PATH (S1), not the newest record with that hash (cursor %u)", (unsigned)jrn_cursor(&j));
}

static void t_zero_in_place(void) {
  Jrn j; uint8_t junk[200], z[512], back[512]; long sz; unsigned i; char p[96], hex[17];
  world(&j, FM_FAT);
  CHECK(stage(&j, "a", 0, 1, 10, 4, fresh_val(1, 10)) == JRN_OK, "S1");
  CHECK(stage(&j, "b", 0, 1, 30, 4, fresh_val(1, 30)) == JRN_OK && jrn_flush(&j) == JRN_OK, "S2 flushed");
  {
    uint32_t tail = j.tail_off;
    /* junk after the valid prefix: a torn record that has a real magic and a plausible len */
    memset(junk, 0xA5, sizeof junk); memcpy(junk, "PDJR", 4); junk[4] = 100; junk[5] = 0;
    CHECK(raw_write(K, 1, tail, junk, sizeof junk) == 0, "plant a torn tail");
    CHECK(jopen(&j, K) == JRN_OK, "open");
    CHECK(j.tail_off == tail && j.next_seq == 3, "the torn tail is NOT consumed as a record (tail %u want %u, next_seq %u)", (unsigned)j.tail_off, (unsigned)tail, (unsigned)j.next_seq);
    memset(z, 0, sizeof z);
    CHECK(raw_read(K, 1, tail, back, sizeof back) == 0 && memcmp(back, z, sizeof back) == 0, "the torn tail was ZEROED in place");
    jrn_key_hex(K, hex); snprintf(p, sizeof p, ROOT "/%s/0001.pdj", hex);
    sz = jrn_fatfs.size(0, p);
    CHECK(sz == (long)JRN_SEG_SIZE, "zeroing never truncates or grows: size %ld", sz);
    /* a VALID record with the wrong seq (a stale replay) at the tail is not accepted either */
    { uint8_t rec[70];
      CHECK(raw_read(K, 1, JRN_REC_BASE, rec, sizeof rec) == 0, "read S1");
      CHECK(raw_write(K, 1, tail, rec, sizeof rec) == 0, "replay S1 at the tail");
      CHECK(jopen(&j, K) == JRN_OK && j.tail_off == tail && j.next_seq == 3, "a stale valid record (seq 1 where 3 is due) ends the prefix (tail %u, next_seq %u)", (unsigned)j.tail_off, (unsigned)j.next_seq); }
    /* a flipped byte inside the LAST record: the prefix ends before it */
    {
      uint8_t b; raw_read(K, 1, tail - 10, &b, 1); b ^= 0x40; raw_write(K, 1, tail - 10, &b, 1);
      CHECK(jopen(&j, K) == JRN_OK, "open after a bit flip");
      CHECK(j.next_seq == 2, "a flipped CRC is not accepted: prefix stops at S1 (next_seq %u)", (unsigned)j.next_seq);
      CHECK(raw_read(K, 1, j.tail_off, back, sizeof back) == 0 && memcmp(back, z, sizeof back) == 0, "the rejected record was zeroed too");
      for (i = 0; i < 1; i++) { /* the journal keeps working: the next step reuses seq 2 */
        memset(g_img[1] + 30, 0, 4);
        CHECK(jopen(&j, K) == JRN_OK, "reopen"); }
    }
  }
}

static void t_segments_and_full(void) {
  Jrn j; unsigned i; char p[96], hex[17]; long sz; unsigned long wr; int rc, last = 0;
  world(&j, FM_FAT);
  jrn_key_hex(K, hex);
  CHECK(j.seg_first == 1 && j.seg_last == 2 && j.tail_seg == 1, "prepare: tail 1 + spare 2 (%u..%u tail %u)", j.seg_first, j.seg_last, j.tail_seg);
  snprintf(p, sizeof p, ROOT "/%s/0002.pdj", hex);
  sz = jrn_fatfs.size(0, p);
  CHECK(sz == (long)JRN_SEG_SIZE, "the spare is pre-created full size, %ld", sz);
  CHECK(jrn_prepare(&j) == JRN_OK && j.seg_last == 2, "a second prepare creates nothing (spare exists)");
  /* fill seg 1 then seg 2 with no third segment: recording stops LOUDLY */
  for (i = 0; i < 600; i++) {
    rc = stage(&j, "fill", 0, 1, 0, 220, fresh_val(1, 0));
    if (rc == JRN_OK) { rc = jrn_flush(&j); }
    if (rc == JRN_E_FULL) { last = 1; break; }
    CHECK(rc == JRN_OK, "fill %u -> %d", i, rc);
    if (rc != JRN_OK) break;
  }
  CHECK(last == 1 && j.tail_seg == 2, "segment space runs out with a visible FULL (tail_seg %u, iter %u)", j.tail_seg, i);
  CHECK(jrn_pending(&j) == 1, "the refused record stays pending, nothing lost");
  wr = rd_writes;
  CHECK(jrn_flush(&j) == JRN_E_FULL && rd_writes == wr, "a refused flush writes NOTHING");
  CHECK(jrn_prepare(&j) == JRN_OK && j.seg_last == 3, "a safe moment creates the next segment");
  CHECK(jrn_flush(&j) == JRN_OK && j.tail_seg == 3, "and the pending record lands in it");
  /* the ring is whole from the first fill: every slot file exists, full size, header-less = free */
  snprintf(p, sizeof p, ROOT "/%s/0017.pdj", hex);
  CHECK(jrn_fatfs.size(0, p) == (long)JRN_SEG_SIZE, "the last of the 17 slot files exists at full size: %ld", jrn_fatfs.size(0, p));
  snprintf(p, sizeof p, ROOT "/%s/0018.pdj", hex);
  CHECK(jrn_fatfs.size(0, p) < 0, "and there is no 18th");
  CHECK(jopen(&j, K) == JRN_OK && j.ring == 17 && j.seg_last == 3, "a free slot (zero header) is not a segment (ring %u seg_last %u)", j.ring, j.seg_last);
}

/* FIRST FILL is create-or-complete: a cut leaves partial slot files and no header; prepare finishes. */
static void t_first_fill_partial(void) {
  Jrn j; char p[96], hex[17]; unsigned s; FIL f; UINT bw; uint8_t z[1000]; long sz; int ok = 1;
  card_fresh(FM_FAT); img_fill(1); rd_fattime_hook = jrn_fattime_filter;
  jrn_key_hex(K, hex);
  CHECK(f_mkdir(ROOT) == FR_OK, "mk root");
  snprintf(p, sizeof p, ROOT "/%s", hex);
  CHECK(f_mkdir(p) == FR_OK, "mk key dir");
  memset(z, 0, sizeof z);
  snprintf(p, sizeof p, ROOT "/%s/0003.pdj", hex);
  CHECK(f_open(&f, p, FA_WRITE | FA_CREATE_NEW) == FR_OK && f_write(&f, z, sizeof z, &bw) == FR_OK && f_close(&f) == FR_OK, "plant a 1000-byte partial slot");
  CHECK(jopen(&j, K) == JRN_OK && j.seg_last == 0 && j.anchor == JRN_ANCHOR_EMPTY, "no header: the journal reads as empty");
  CHECK(jrn_prepare(&j) == JRN_OK && j.seg_first == 1 && j.seg_last == 2 && j.ring == 17, "prepare finishes the first fill (%u..%u ring %u)", j.seg_first, j.seg_last, j.ring);
  for (s = 1; s <= 17; s++) {
    snprintf(p, sizeof p, ROOT "/%s/%04u.pdj", hex, s);
    sz = jrn_fatfs.size(0, p);
    if (sz != (long)JRN_SEG_SIZE) ok = 0;
  }
  CHECK(ok, "all 17 slot files are full size after completing the partial one");
}

static unsigned s_names; static char s_dirsig[4096]; static unsigned s_dirlen;
static void dir_sig_cb(void* a, const char* nm) { (void)a; s_names++; s_dirlen += (unsigned)snprintf(s_dirsig + s_dirlen, sizeof s_dirsig - s_dirlen, "%s;", nm); }
static void dir_sig(const char* hex, char* out, size_t n, unsigned* cnt) {
  char d[96]; snprintf(d, sizeof d, ROOT "/%s", hex);
  s_names = 0; s_dirlen = 0; s_dirsig[0] = 0;
  CHECK(jrn_fatfs.list(0, d, dir_sig_cb, 0) == 0, "list");
  snprintf(out, n, "%s", s_dirsig); *cnt = s_names;
}
static unsigned long seg_stamp(const char* hex, unsigned slot) { char p[96]; snprintf(p, sizeof p, ROOT "/%s/%04u.pdj", hex, slot); return jrn_fatfs.stamp(0, p); }

/* THE RING: wrap the journal round a 3-slot ring several times. The directory never changes. */
static void t_ring(void) {
  Jrn j; JrnCfg c = cfg_for(K, 2, 0); char hex[17], d0[4096], d1[4096]; unsigned n0, n1, i, wraps = 0; int rc;
  uint8_t h[JRN_SEG_HDR], b[600]; uint32_t idx; unsigned long st1, st1b;
  card_fresh(FM_FAT); img_fill(3); rd_fattime_hook = jrn_fattime_filter;
  jrn_key_hex(K, hex);
  CHECK(jrn_open(&j, &c, &IMG) == JRN_OK && jrn_prepare(&j) == JRN_OK, "open + prepare (cap 2 => ring 3)");
  CHECK(j.ring == 3, "ring = max_segs + 1 = %u", j.ring);
  dir_sig(hex, d0, sizeof d0, &n0);
  CHECK(n0 == 3, "exactly 3 slot files, %u", n0);
  st1 = seg_stamp(hex, 1);
  for (i = 0; i < 1100; i++) {
    rd_fattime_now = 0x5A2A6800u + i * 0x40u;
    rc = stage(&j, "fill", 0, 1, 0, 220, fresh_val(1, 0));
    if (!rc) rc = jrn_flush(&j);
    CHECK(rc == JRN_OK, "fill %u -> %d", i, rc);
    if (rc) break;
    if (j.tail_seg == j.seg_last) { rc = jrn_prepare(&j); CHECK(rc == JRN_OK, "prepare %u", i); if (rc) break; if (j.seg_last > 3 && j.seg_last % 3 == 1) wraps++; }
    if (i % 25 == 0 && i) {                       /* a fresh boot must map the ring to the SAME logical set */
      Jrn q; CHECK(jrn_open(&q, &c, &IMG) == JRN_OK && q.seg_first == j.seg_first && q.seg_last == j.seg_last && q.tail_seg == j.tail_seg && q.tail_off == j.tail_off,
                   "reopen at step %u maps the ring to %u..%u tail %u@%u, live state is %u..%u tail %u@%u", i, q.seg_first, q.seg_last, q.tail_seg, (unsigned)q.tail_off, j.seg_first, j.seg_last, j.tail_seg, (unsigned)j.tail_off);
      CHECK(memcmp(q.seg_seq, j.seg_seq, sizeof q.seg_seq) == 0, "the per-segment first-seq index a reopen builds equals the one the live journal kept (step %u)", i);
    }
  }
  CHECK(j.seg_last >= 9, "the journal wrapped the ring at least twice (seg_last %u)", j.seg_last);
  CHECK(j.seg_last - j.seg_first + 1u <= 3u, "live set within the ring: %u..%u", j.seg_first, j.seg_last);
  dir_sig(hex, d1, sizeof d1, &n1);
  CHECK(n1 == n0 && strcmp(d0, d1) == 0, "the directory listing never changed after the first fill (%u -> %u names)", n0, n1);
  st1b = seg_stamp(hex, 1);
  CHECK(st1 == st1b, "slot 1's directory timestamp is frozen across %u wraps (%lx vs %lx)", wraps, st1, st1b);
  /* the header index, not the slot number, orders the ring */
  CHECK(jopen(&j, K) == JRN_OK && j.ring == 3, "reopen");
  CHECK(j.seg_last - j.seg_first + 1u <= 3u && j.seg_first > 1, "open maps slots to logical order (%u..%u)", j.seg_first, j.seg_last);
  {
    unsigned s, slot_lo = 0, hi_first = 0; uint32_t lo_idx = 0;
    for (s = 1; s <= 3; s++) {
      CHECK(raw_read(K, s, 0, h, JRN_SEG_HDR) == 0, "hdr");
      idx = (uint32_t)(h[8] | h[9] << 8);
      if (idx && (!lo_idx || idx < lo_idx)) { lo_idx = idx; slot_lo = s; }
      if (idx == j.seg_first && s != 1 + (j.seg_first - 1) % 3) hi_first = 1;
    }
    CHECK(lo_idx == j.seg_first && !hi_first, "seg_first %u == lowest header index %u (slot %u)", j.seg_first, (unsigned)lo_idx, slot_lo);
    CHECK(slot_lo != 1 || (j.seg_first - 1) % 3 == 0, "order by header index, not slot: the oldest need not sit in slot 1");
  }
  CHECK(jrn_compact(&j) == JRN_OK, "compact");
  if (j.seg_first > 1 && j.seg_last - j.seg_first + 1u <= 2u) {                                       /* the slot the oldest segment left is FREE: header zero in place */
    unsigned k; CHECK(raw_read(K, 1 + (j.seg_first - 2u) % 3u, 0, h, JRN_SEG_HDR) == 0, "retired hdr");
    for (k = 0; k < JRN_SEG_HDR && !h[k]; k++) {}
    CHECK(k == JRN_SEG_HDR, "(ring) the retired slot's header is zero in place (compaction never deletes)");
  }
  /* a recycled slot has an all-zero body past its header, apart from the records its new life wrote */
  {
    unsigned s;
    CHECK(jrn_prepare(&j) == JRN_OK, "prepare (spare)");
    for (s = j.tail_seg + 1u; s <= j.seg_last; s++) {
      uint32_t off; int clean = 1;
      for (off = JRN_REC_BASE; off < JRN_SEG_SIZE && clean; off += sizeof b) {
        uint32_t m = JRN_SEG_SIZE - off < sizeof b ? JRN_SEG_SIZE - off : (uint32_t)sizeof b, k;
        CHECK(raw_read(K, 1 + (s - 1) % 3, off, b, m) == 0, "read spare body");
        for (k = 0; k < m; k++) if (b[k]) { clean = 0; break; }
      }
      CHECK(clean, "the recycled spare (logical %u) has an all-zero body: no stale record of its previous life", s);
    }
  }
}

/* A header that is half zeroed (a torn retire) or half written (a torn activation) is a FREE slot. */
static void t_half_header(void) {
  static const unsigned W[] = { 1, 4, 7, 8, 12, 16, 20, 27, 28 };
  unsigned w, s; Jrn j; char hex[17]; uint8_t h[JRN_SEG_HDR], z[JRN_SEG_HDR]; JrnCfg c = cfg_for(K, 3, 0);
  memset(z, 0, sizeof z);
  for (w = 0; w < sizeof W / sizeof W[0]; w++) {
    for (s = 0; s < 2; s++) {                     /* s0: zero the first W bytes; s1: zero the LAST W bytes' */
      card_fresh(FM_FAT); img_fill(1); rd_fattime_hook = jrn_fattime_filter; jrn_key_hex(K, hex);
      CHECK(jrn_open(&j, &c, &IMG) == JRN_OK && jrn_prepare(&j) == JRN_OK, "open + prepare");
      CHECK(stage(&j, "a", 0, 1, 10, 4, fresh_val(1, 10)) == JRN_OK && jrn_flush(&j) == JRN_OK, "one record");
      CHECK(raw_read(K, 1, 0, h, JRN_SEG_HDR) == 0, "hdr");
      if (s == 0) CHECK(raw_write(K, 1, 0, z, W[w]) == 0, "half-zero the header prefix");
      else        CHECK(raw_write(K, 1, JRN_SEG_HDR - 4, z, 4) == 0, "zero the crc");
      CHECK(jrn_open(&j, &c, &IMG) == JRN_OK, "reopen");
      CHECK(j.seg_first == 2 && j.seg_last == 2, "%s %u bytes zeroed: the slot reads as FREE, never live (%u..%u)", s ? "crc" : "prefix", s ? 4u : W[w], j.seg_first, j.seg_last);
    }
  }
}

static void t_compaction_floor(void) {
  Jrn j; unsigned i, undone = 0; int rc; JrnRec r; uint32_t min_seq = 0, last; char p[96], hex[17]; static uint8_t keep[NREG][RSZ];
  JrnCfg c = cfg_for(K, 2, 0);
  card_fresh(FM_FAT); img_fill(2); rd_fattime_hook = jrn_fattime_filter;
  CHECK(jrn_open(&j, &c, &IMG) == JRN_OK && jrn_prepare(&j) == JRN_OK, "open + prepare (cap 2)");
  for (i = 0; i < 420; i++) {
    rc = stage(&j, "fill", 0, 1, 0, 220, fresh_val(1, 0));
    if (!rc) rc = jrn_flush(&j);
    CHECK(rc == JRN_OK, "fill %u -> %d", i, rc);
    if (rc) return;
    if (j.tail_seg == j.seg_last) CHECK(jrn_prepare(&j) == JRN_OK, "prepare");
  }
  CHECK(j.seg_last >= 4, "grew to %u segments", j.seg_last);
  CHECK(jrn_compact(&j) == JRN_OK, "compact");
  CHECK(j.seg_last - j.seg_first + 1 <= 2 && j.seg_first > 1, "cap 2 enforced: %u..%u", j.seg_first, j.seg_last);
  jrn_key_hex(K, hex); snprintf(p, sizeof p, ROOT "/%s/0001.pdj", hex);
  {
    uint8_t hz[JRN_SEG_HDR]; unsigned k;
    snprintf(p, sizeof p, ROOT "/%s/%04u.pdj", hex, 1u + (j.seg_first - 2u) % 3u);
    CHECK(jrn_fatfs.size(0, p) == (long)JRN_SEG_SIZE, "the retired slot FILE stays (the directory never changes)");
    CHECK(raw_read(K, 1 + (j.seg_first - 2u) % 3u, 0, hz, JRN_SEG_HDR) == 0, "read retired header");
    for (k = 0; k < JRN_SEG_HDR && !hz[k]; k++) {}
    CHECK(k == JRN_SEG_HDR, "the oldest segment is RETIRED: its header is zero in place");
  }
  last = jrn_cursor(&j);
  for (i = 1; i <= last; i++) if (jrn_find(&j, i, &r) == JRN_OK) { min_seq = i; break; }
  CHECK(min_seq > 1, "the oldest surviving record is %u", (unsigned)min_seq);
  CHECK(jopen(&j, K) == JRN_OK, "reopen after compaction");
  CHECK(j.anchor == JRN_ANCHOR_MATCH && jrn_cursor(&j) == last, "still anchors after the oldest segment left");
  for (;;) {
    rc = jrn_undo(&j, &IMG, 0);
    if (rc != JRN_OK) break;
    undone++;
    if (undone > 1000) break;
  }
  CHECK(rc == JRN_E_FLOOR, "undo stops at the ORPHAN-PARENT floor, got %d", rc);
  CHECK(undone == last - min_seq, "undid %u steps, want %u (down to the floor record)", undone, (unsigned)(last - min_seq));
  memcpy(keep, g_img, sizeof g_img);
  CHECK(jrn_undo(&j, &IMG, 0) == JRN_E_FLOOR && memcmp(keep, g_img, sizeof g_img) == 0, "the floor refuses and touches nothing");
}

static void t_redirects(void) {
  const JrnFs* fs = &jrn_fatfs; uint64_t out = 0; char p[96], hex[17]; FIL f; UINT bw;
  card_fresh(FM_FAT);
  CHECK(jrn_redirect_write(fs, ROOT, 2, 1) == JRN_OK, "B -> A");
  CHECK(jrn_key_resolve(fs, ROOT, 2, &out) == JRN_OK && out == 1, "resolve(B) = A");
  CHECK(jrn_redirect_write(fs, ROOT, 3, 2) == JRN_OK, "C -> B (flattened to A)");
  CHECK(jrn_key_resolve(fs, ROOT, 3, &out) == JRN_OK && out == 1, "resolve(C) = A (flattened)");
  CHECK(jrn_redirect_write(fs, ROOT, 3, 2) == JRN_OK, "same redirect again is a no-op");
  CHECK(jrn_redirect_write(fs, ROOT, 3, 9) == JRN_E_EXISTS, "a redirect that already points elsewhere is never replaced");
  CHECK(jrn_redirect_write(fs, ROOT, 1, 2) == JRN_OK, "renaming BACK (target resolves to the key itself) needs no file");
  CHECK(jrn_key_resolve(fs, ROOT, 1, &out) == JRN_OK && out == 1, "no self loop was written");
  /* a hand-made cycle is detected, not followed forever */
  jrn_key_hex(7, hex); snprintf(p, sizeof p, ROOT "/%s.pdr", hex);
  {
    uint8_t b[16]; uint32_t c; int i;
    b[0] = 'P'; b[1] = 'D'; b[2] = 'R'; b[3] = 'D';
    for (i = 0; i < 8; i++) b[4 + i] = (uint8_t)((uint64_t)8 >> (8 * i));
    c = jrn_crc32_update(0, b, 12); b[12] = (uint8_t)c; b[13] = (uint8_t)(c >> 8); b[14] = (uint8_t)(c >> 16); b[15] = (uint8_t)(c >> 24);
    CHECK(f_open(&f, p, FA_WRITE | FA_CREATE_NEW) == FR_OK && f_write(&f, b, 16, &bw) == FR_OK && f_close(&f) == FR_OK, "plant 7->8");
    jrn_key_hex(8, hex); snprintf(p, sizeof p, ROOT "/%s.pdr", hex);
    for (i = 0; i < 8; i++) b[4 + i] = (uint8_t)((uint64_t)7 >> (8 * i));
    c = jrn_crc32_update(0, b, 12); b[12] = (uint8_t)c; b[13] = (uint8_t)(c >> 8); b[14] = (uint8_t)(c >> 16); b[15] = (uint8_t)(c >> 24);
    CHECK(f_open(&f, p, FA_WRITE | FA_CREATE_NEW) == FR_OK && f_write(&f, b, 16, &bw) == FR_OK && f_close(&f) == FR_OK, "plant 8->7");
  }
  CHECK(jrn_key_resolve(fs, ROOT, 7, &out) == JRN_E_LOOP, "a redirect LOOP is detected");
  /* a corrupt redirect is no redirect */
  jrn_key_hex(20, hex); snprintf(p, sizeof p, ROOT "/%s.pdr", hex);
  CHECK(f_open(&f, p, FA_WRITE | FA_CREATE_NEW) == FR_OK && f_write(&f, "garbagegarbageXX", 16, &bw) == FR_OK && f_close(&f) == FR_OK, "plant garbage");
  CHECK(jrn_key_resolve(fs, ROOT, 20, &out) == JRN_OK && out == 20, "a corrupt .pdr is treated as absent");
  /* the journal follows the redirect: history recorded under A is found through B */
  {
    Jrn j; JrnCfg c;
    img_fill(3); rd_fattime_hook = jrn_fattime_filter;
    CHECK(jopen(&j, 1) == JRN_OK && jrn_prepare(&j) == JRN_OK, "journal under A");
    CHECK(stage(&j, "s", 0, 1, 10, 4, fresh_val(1, 10)) == JRN_OK && jrn_flush(&j) == JRN_OK, "S1 under A");
    c = cfg_for(2, 0, 0);
    CHECK(jrn_open(&j, &c, &IMG) == JRN_OK && j.key == 1 && jrn_cursor(&j) == 1, "opening key B lands in A's journal at the cursor (key %llx cursor %u)", (unsigned long long)j.key, (unsigned)jrn_cursor(&j));
  }
}

static void t_verify_stops(void) {
  Jrn j; int rc;
  world(&j, FM_FAT);
  CHECK(stage(&j, "a", 0, 1, 10, 4, fresh_val(1, 10)) == JRN_OK, "stage");
  rd_lie_writes = 1;                                         /* the card ACKs and keeps nothing */
  rc = jrn_flush(&j);
  rd_lie_writes = 0;
  CHECK(rc == JRN_E_VERIFY, "the re-read catches a card that dropped the write, got %d", rc);
  CHECK(j.stopped == 1, "recording STOPS");
  CHECK(jrn_step_begin(&j, "b", 0) == JRN_E_STOPPED, "and never says 'recorded' again");
  CHECK(jrn_flush(&j) == JRN_E_STOPPED, "flush refuses too");
}

static void t_readonly(void) {
  Jrn j; unsigned long wr; JrnRec r;
  world(&j, FM_FAT);
  CHECK(stage(&j, "a", 0, 1, 10, 4, fresh_val(1, 10)) == JRN_OK && jrn_flush(&j) == JRN_OK, "a step exists");
  wr = rd_writes;
  CHECK(jopen_ro(&j, K) == JRN_OK && jrn_cursor(&j) == 1, "read-only open reads the history");
  CHECK(jrn_step_begin(&j, "x", 0) == JRN_E_RDONLY, "no recording");
  CHECK(jrn_prepare(&j) == JRN_E_RDONLY && jrn_compact(&j) == JRN_E_RDONLY && jrn_flush(&j) == JRN_E_RDONLY, "no safe-moment writes");
  CHECK(jrn_undo(&j, &IMG, &r) == JRN_E_RDONLY, "no undo on a read-only journal");
  CHECK(jrn_find(&j, 1, &r) == JRN_OK, "the history is still readable (history screen)");
  CHECK(rd_writes == wr, "a read-only open wrote %lu sectors", rd_writes - wr);
  { uint8_t junk[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    CHECK(raw_write(K, 1, j.tail_off, junk, 8) == 0, "plant junk");
    wr = rd_writes;
    CHECK(jopen_ro(&j, K) == JRN_OK && rd_writes == wr, "a read-only open does not even zero the tail"); }
}

/* Ruling 1: sector 0 of a segment is the header ALONE (32 B, then zeros) and no append ever rewrites it. */
static void t_header_sector_alone(void) {
  Jrn j; uint8_t h0[512], h1[512], rec[8]; unsigned i, k; static unsigned ch[64];
  world(&j, FM_FAT);
  CHECK(raw_read(K, 1, 0, h0, sizeof h0) == 0, "header sector");
  for (k = JRN_SEG_HDR; k < sizeof h0 && !h0[k]; k++) {}
  CHECK(k == sizeof h0, "bytes 32..511 of sector 0 are zero (the first record starts at %u)", JRN_REC_BASE);
  CHECK(j.tail_off == JRN_REC_BASE, "a fresh tail starts at the record base (%u)", (unsigned)j.tail_off);
  rd_snapshot();
  for (i = 0; i < 40; i++) CHECK(stage(&j, "s", 0, 1, 0, 100, fresh_val(1, 0)) == JRN_OK && jrn_flush(&j) == JRN_OK, "append %u", i);
  CHECK(raw_read(K, 1, 0, h1, sizeof h1) == 0 && memcmp(h0, h1, sizeof h0) == 0, "40 appends left the header sector byte-identical");
  CHECK(raw_read(K, 1, JRN_REC_BASE, rec, sizeof rec) == 0 && memcmp(rec, "PDJR", 4) == 0, "the first record sits at %u", JRN_REC_BASE);
  { char p[96], hex[17]; FIL f; unsigned lba0 = 0, n = rd_changed(ch, 64), hit = 0;
    jrn_key_hex(K, hex); snprintf(p, sizeof p, ROOT "/%s/0001.pdj", hex);
    CHECK(f_open(&f, p, FA_READ) == FR_OK, "open"); lba0 = (unsigned)(f.obj.fs->database + (LBA_t)f.obj.fs->csize * (f.obj.sclust - 2)); f_close(&f);
    for (i = 0; i < n && i < 64; i++) if (ch[i] == lba0) hit++;
    CHECK(n > 0 && hit == 0, "no append wrote the slot's first sector (%u of %u changed sectors)", hit, n); }
}

/* Ruling 2: the first fill allocates every slot but zeroes only each HEADER sector; the body zeroing moves
 * into the activation (one handle). The card here is poisoned (0xA5 free space), so an un-activated slot's
 * body must still read garbage, and the write cost of the first fill must be a fraction of 17 x 128. */
static void t_staged_first_fill(void) {
  Jrn j; unsigned long w0, cost; uint8_t b[16], hz[512]; unsigned k;
  card_fresh(FM_FAT32); img_fill(1); rd_fattime_hook = jrn_fattime_filter;
  g_jopen_segs = 0;
  CHECK(jopen(&j, K) == JRN_OK, "open");
  w0 = rd_writes; CHECK(jrn_prepare_first(&j) == JRN_OK, "first fill");
  cost = rd_writes - w0;
  CHECK(j.ring == 17 && cost < 400, "the first fill (17 slots) wrote %lu sectors: header sectors + ONE activation, not 17 body zero-fills (2176+)", cost);
  for (k = 1; k <= 17; k++) {
    CHECK(raw_read(K, k, 0, hz, sizeof hz) == 0, "slot %u header sector", k);
    { unsigned i; for (i = 0; i < sizeof hz && !(k > 1 ? hz[i] : 0); i++) {}
      CHECK(k == 1 || i == sizeof hz, "slot %u: the header sector is zero (a free slot)", k); }
  }
  CHECK(raw_read(K, 1, 4096, b, sizeof b) == 0 && !b[0] && !b[8], "the activated segment's body is zero");
  CHECK(raw_read(K, 17, 4096, b, sizeof b) == 0 && b[0] == 0xA5 && b[8] == 0xA5, "an un-activated slot's body was NOT zero-filled by the first fill (still the card's garbage)");
  CHECK(jrn_prepare(&j) == JRN_OK, "spare");
  CHECK(raw_read(K, 2, 4096, b, sizeof b) == 0 && !b[0] && !b[8], "the spare's body is zero-filled by its ACTIVATION");
  CHECK(raw_read(K, 3, 4096, b, sizeof b) == 0 && b[0] == 0xA5, "slot 3 still garbage");
  CHECK(jopen(&j, K) == JRN_OK && j.seg_first == 1 && j.seg_last == 2 && j.ring == 17, "reopen sees exactly the two activated segments (garbage bodies are never read)");
  g_jopen_segs = 0;
}

/* Ruling 5: bounds derive from the ring, never a fixed 512. One UNSAVED session of N steps, then the power
 * goes: the image on the card is the session's start, the load must anchor there and OFFER all N steps. */
static void t_long_session(void) {
  static const unsigned N[] = { 511, 512, 513, 600 };
  unsigned n, g; Jrn j; static uint8_t start[NREG][RSZ]; uint32_t av = 0, tot = 0, s0;
  for (n = 0; n < sizeof N / sizeof N[0]; n++) {
    card_fresh(FM_FAT32); img_fill(1); rd_fattime_hook = jrn_fattime_filter; g_jopen_segs = 0;
    CHECK(jopen(&j, K) == JRN_OK && jrn_prepare(&j) == JRN_OK, "prep");
    CHECK(stage(&j, "saved", 0, 1, 0, 4, fresh_val(1, 0)) == JRN_OK && jrn_flush(&j) == JRN_OK, "s0");
    s0 = j.cursor; memcpy(start, g_img, sizeof g_img);
    for (g = 0; g < N[n]; g++) {
      CHECK(stage(&j, "drop", 0, (uint8_t)(2 + g % 10), (uint16_t)(g % 200), 2, fresh_val((uint8_t)(2 + g % 10), (uint16_t)(g % 200))) == JRN_OK && jrn_flush(&j) == JRN_OK, "step %u", g);
      if (j.tail_seg == j.seg_last) CHECK(jrn_prepare(&j) == JRN_OK, "prepare");
    }
    memcpy(g_img, start, sizeof g_img); card_remount();
    CHECK(jopen(&j, K) == JRN_OK, "reopen");
    CHECK(jrn_redo_info(&j, &av, &tot, 0) == 0, "redo info");
    CHECK(j.cursor == s0 && jrn_offer(&j) == 1 && tot == N[n], "a %u-step unsaved session is offered whole (cursor %u want %u, offer %d, total %u)", N[n], (unsigned)j.cursor, (unsigned)s0, jrn_offer(&j), (unsigned)tot);
  }
}

/* Ruling 5: the read path is cheap. A FULL default ring (17 slots) on FAT32: disk_read calls for the three
 * operations the reviewer priced at 75,533 / 34,795 / 7,887,975 before the chunked scan + first-seq index. */
static void t_read_cost(void) {
  Jrn j; unsigned g; int rc = 0; unsigned long r0, c_open, c_undo, c_old; static uint8_t cur[NREG][RSZ];
  card_fresh(FM_FAT32); img_fill(1); rd_fattime_hook = jrn_fattime_filter; g_jopen_segs = 0;
  CHECK(jopen(&j, K) == JRN_OK && jrn_prepare(&j) == JRN_OK, "prep");
  for (g = 0; g < 20000 && !rc; g++) {
    rc = stage(&j, "w", 0, 1, 0, 150, fresh_val(1, 0));
    if (!rc) rc = jrn_flush(&j);
    if (!rc && j.tail_seg == j.seg_last) { if (j.seg_last - j.seg_first + 1u >= 17u) break; rc = jrn_prepare(&j); }
  }
  CHECK(rc == 0 && j.seg_last - j.seg_first + 1u == 17u, "the ring is full (%u..%u, rc %d)", j.seg_first, j.seg_last, rc);
  card_remount();
  r0 = rd_read_calls; CHECK(jopen(&j, K) == JRN_OK && j.anchor == JRN_ANCHOR_MATCH, "open full"); c_open = rd_read_calls - r0;
  r0 = rd_read_calls; rc = jrn_undo(&j, &IMG, 0); c_undo = rd_read_calls - r0; CHECK(rc == JRN_OK, "undo");
  memcpy(cur, g_img, sizeof g_img); img_fill(1); card_remount();
  r0 = rd_read_calls; CHECK(jopen(&j, K) == JRN_OK, "open with the OLD image"); c_old = rd_read_calls - r0;
  CHECK(j.anchor == JRN_ANCHOR_MATCH || j.anchor == JRN_ANCHOR_BRANCH || j.anchor == JRN_ANCHOR_NEWROOT, "anchored");
  memcpy(g_img, cur, sizeof g_img);
  printf("    read cost, FAT32, full 17-slot ring (%u steps): open %lu disk_read calls (was 75533), one undo %lu (was 34795), open with the OLD image / anchor walk %lu (was 7887975)\n", g, c_open, c_undo, c_old);
  CHECK(c_open < 4000, "open of a full ring costs %lu disk reads (bound 4000)", c_open);
  CHECK(c_undo < 800, "one undo costs %lu disk reads (bound 800)", c_undo);
  CHECK(c_old < 30000, "the anchor walk on an old image costs %lu disk reads (bound 30000)", c_old);
}

/* The first-seq index after a RETIRE: the retired slot's entry must be gone, exactly as a reopen would build it. */
static void t_index_after_retire(void) {
  Jrn j, q; JrnCfg c = cfg_for(K, 2, 0); unsigned g; JrnRec r;
  card_fresh(FM_FAT); img_fill(1); rd_fattime_hook = jrn_fattime_filter;
  CHECK(jrn_open(&j, &c, &IMG) == JRN_OK && jrn_prepare(&j) == JRN_OK && j.ring == 3, "open + prepare");
  for (g = 0; g < 400 && j.tail_seg < 2; g++) CHECK(stage(&j, "f", 0, 1, 0, 220, fresh_val(1, 0)) == JRN_OK && jrn_flush(&j) == JRN_OK, "fill");
  CHECK(j.tail_seg == 2 && j.seg_seq[0] == 1 && j.seg_seq[1] > 1, "segment 1 starts at seq 1, segment 2 after it (%u, %u)", (unsigned)j.seg_seq[0], (unsigned)j.seg_seq[1]);
  CHECK(jrn_find(&j, j.seg_seq[1], &r) == JRN_OK && r.seq == j.seg_seq[1], "the index finds the first record of segment 2");
  CHECK(jrn_find(&j, j.seg_seq[1] - 1u, &r) == JRN_OK, "and the last record of segment 1");
  CHECK(jrn_prepare(&j) == JRN_OK && j.seg_last == 3, "third segment");
  CHECK(jrn_compact(&j) == JRN_OK && j.seg_first == 2, "retire segment 1");
  CHECK(jrn_open(&q, &c, &IMG) == JRN_OK && q.seg_first == 2, "reopen");
  CHECK(memcmp(q.seg_seq, j.seg_seq, sizeof q.seg_seq) == 0, "the first-seq index a reopen builds equals the one the live journal kept after a retire");
  CHECK(jrn_find(&j, 1, &r) == JRN_E_FLOOR, "a record of the retired segment is a floor, not a stale hit");
}

/* Ruling 5 / bounce fix 5: read-error HONESTY. ONE transient read error at every read of jrn_open (and of a
 * redirect resolve): the call either fails LOUDLY or returns exactly the truth -- never a silently shorter
 * ring, an empty journal, a moved tail or another key. (Before: 53 silent wrong views, 31 lost history.) */
static void t_read_error_honesty(void) {
  Jrn j, t; unsigned g; long k; unsigned long r0, R; uint32_t want_seq; uint16_t wf, wl, wt; uint8_t wanc; uint32_t wcur;
  unsigned silent = 0, loud = 0, same = 0; static uint8_t img0[NREG][RSZ];
  g_jopen_segs = 3;
  card_fresh(FM_FAT); img_fill(1); rd_fattime_hook = jrn_fattime_filter;
  CHECK(jopen(&j, K) == JRN_OK && jrn_prepare(&j) == JRN_OK, "prep");
  for (g = 0; g < 400 && j.tail_seg < 2; g++) CHECK(stage(&j, "f", 0, 1, 0, 220, fresh_val(1, 0)) == JRN_OK && jrn_flush(&j) == JRN_OK, "fill");
  CHECK(jrn_prepare(&j) == JRN_OK && stage(&j, "x", 0, 1, 0, 4, fresh_val(1, 0)) == JRN_OK && jrn_flush(&j) == JRN_OK, "spare + step");
  card_remount(); CHECK(jopen(&t, K) == JRN_OK, "truth");
  want_seq = t.next_seq; wf = t.seg_first; wl = t.seg_last; wt = t.tail_seg; wanc = t.anchor; wcur = t.cursor;
  rd_snapshot(); memcpy(img0, g_img, sizeof g_img);
  r0 = rd_read_calls; CHECK(jopen(&t, K) == JRN_OK, "count"); R = rd_read_calls - r0;
  for (k = 0; k < (long)R; k++) {
    int rc;
    rd_restore(); memcpy(g_img, img0, sizeof g_img); card_remount();
    rd_fail_read_at = k; rc = jopen(&j, K); rd_fail_read_at = -1;
    if (rc != JRN_OK) { loud++; continue; }
    if (j.next_seq == want_seq && j.seg_first == wf && j.seg_last == wl && j.tail_seg == wt && j.anchor == wanc && j.cursor == wcur) { same++; continue; }
    silent++;
    if (silent <= 3) printf("  read #%ld failed: open OK but sees segs %u..%u tail %u next_seq %u anchor %d\n", k, j.seg_first, j.seg_last, j.tail_seg, (unsigned)j.next_seq, j.anchor);
  }
  CHECK(silent == 0, "jrn_open with one transient read error: %u SILENT wrong views of %lu reads (%u loud, %u still right)", silent, R, loud, same);
  CHECK(loud > 0, "the sweep reaches the read paths (%u loud)", loud);
  /* the same for the redirect resolve: another key's redirect to K */
  rd_restore(); card_remount();
  CHECK(jrn_redirect_write(&jrn_fatfs, ROOT, 0x77, K) == JRN_OK, "redirect");
  rd_snapshot();
  { uint64_t out = 0; unsigned bad = 0; r0 = rd_read_calls; CHECK(jrn_key_resolve(&jrn_fatfs, ROOT, 0x77, &out) == JRN_OK && out == K, "resolve"); R = rd_read_calls - r0;
    for (k = 0; k < (long)R; k++) {
      int rc; rd_restore(); card_remount(); out = 0;
      rd_fail_read_at = k; rc = jrn_key_resolve(&jrn_fatfs, ROOT, 0x77, &out); rd_fail_read_at = -1;
      if (rc == JRN_OK && out != K) bad++;
    }
    CHECK(bad == 0, "a transient read error made %u resolves return the WRONG key (of %lu reads)", bad, R); }
  g_jopen_segs = 0;
}

/* A flush whose write fails PART WAY leaves valid-CRC records past the tail: recording STOPS (no pop that
 * would reclaim their seqs), and the next session neither resurrects nor re-links them. */
static void t_partial_flush_stops(void) {
  Jrn j; int rc; uint8_t z[16], b[16];
  world(&j, FM_FAT);
  CHECK(stage(&j, "r1", 0, 1, 10, 4, fresh_val(1, 10)) == JRN_OK, "r1");
  CHECK(stage(&j, "r2", 0, 2, 10, 4, fresh_val(2, 10)) == JRN_OK && jrn_pending(&j) == 2, "r2");
  { JrnRec r1; uint16_t len1; CHECK(jrn_i_hdr_parse(j.pend, &r1) == 0, "hdr"); len1 = r1.len;   /* r2 (written first) lands ... */
    CHECK(raw_write(K, 1, JRN_REC_BASE + len1, j.pend + len1, j.pend_len - len1) == 0, "plant the landed r2"); }
  rd_fail_all_writes = 1;                            /* ... and r1's write then fails (EZ-Flash: no retry) */
  rc = jrn_flush(&j);
  rd_fail_all_writes = 0;
  CHECK(rc != JRN_OK && j.stopped == 1, "a flush that fails part way STOPS recording (rc %d, stopped %d)", rc, j.stopped);
  CHECK(jrn_undo(&j, &IMG, 0) == JRN_E_STOPPED, "so a pending step is NOT popped (its seq stays claimed)");
  CHECK(jrn_step_begin(&j, "x", 0) == JRN_E_STOPPED && jrn_flush(&j) == JRN_E_STOPPED, "and nothing more is recorded");
  card_remount();
  CHECK(jopen(&j, K) == JRN_OK, "reopen");
  CHECK(j.next_seq == 1 && j.tail_off == JRN_REC_BASE, "the orphan r2 is not a record (next_seq %u, tail %u)", (unsigned)j.next_seq, (unsigned)j.tail_off);
  memset(z, 0, sizeof z);
  CHECK(raw_read(K, 1, JRN_REC_BASE + 70, b, sizeof b) == 0 && memcmp(b, z, sizeof b) == 0, "the orphan was zeroed at open");
  img_fill(1);                                       /* the card's saved image: the session's edits never reached it */
  CHECK(jopen(&j, K) == JRN_OK, "reopen on the saved image");
  CHECK(stage(&j, "n1", 0, 3, 10, 4, fresh_val(3, 10)) == JRN_OK && jrn_flush(&j) == JRN_OK, "a new session records a step");
  card_remount();
  CHECK(jopen(&j, K) == JRN_OK && j.next_seq == 2 && jrn_cursor(&j) == 1, "the new step is seq 1 and nothing was resurrected behind it (next_seq %u, cursor %u)", (unsigned)j.next_seq, (unsigned)jrn_cursor(&j));
}

/* Sticky refusal (bounce fix 8): with no tail segment recording is refused at the door, every time. */
static void t_no_segment_refusal(void) {
  Jrn j; unsigned g;
  card_fresh(FM_FAT); img_fill(1); rd_fattime_hook = jrn_fattime_filter;
  CHECK(jopen(&j, K) == JRN_OK && !j.tail_seg, "a journal that was never prepared has no tail segment");
  for (g = 0; g < 12; g++) CHECK(jrn_step_begin(&j, "x", 0) == JRN_E_NOSEG, "begin %u without a segment says NOSEG (never a buffered step that cannot flush)", g);
  CHECK(jrn_pending(&j) == 0 && j.bld_len == 0, "nothing was buffered");
  CHECK(jrn_prepare(&j) == JRN_OK && jrn_step_begin(&j, "x", 0) == JRN_OK, "after prepare recording works");
  jrn_step_abort(&j);
}

/* A header the way a NEWER (or foreign) build would have left it: valid magic + crc, unknown shape. */
static void plant_hdr(uint64_t key, unsigned slot, uint32_t idx, uint16_t ring, uint16_t ver) {
  uint8_t h[JRN_SEG_HDR]; uint32_t c;
  memset(h, 0, sizeof h);
  memcpy(h, "PDJS", 4); jrn_wr16(h + 4, ver); jrn_wr16(h + 6, ring); jrn_wr32(h + 8, idx);
  c = jrn_crc32_update(0, h, 28); jrn_wr32(h + 28, c);
  CHECK(raw_write(key, slot, 0, h, sizeof h) == 0, "plant header slot %u", slot);
}

/* THE VERSION RULE (bounce ruling 4): a valid-magic + valid-crc header this build does not know is
 * FOREIGN. The whole journal opens read-only with JRN_E_VERSION and nothing is ever zeroed: a v1
 * reader must not treat a v2 slot as free and recycle it. */
static void t_version_foreign(void) {
  Jrn j; uint8_t rec0[64], rec1[64], mark[16], back[16]; unsigned long wr; unsigned k;
  static const struct { uint32_t idx; uint16_t ring, ver; const char* what; } F[] = {
    { 1, 17, 2, "a v2 header" }, { 0, 17, 1, "index 0" }, { 10000, 17, 1, "index past SEG_MAX" },
    { 1, 1, 1, "ring 1" }, { 1, 300, 1, "a ring bigger than this build's" } };
  for (k = 0; k < sizeof F / sizeof F[0]; k++) {
    world(&j, FM_FAT);
    CHECK(stage(&j, "a", 0, 1, 10, 4, fresh_val(1, 10)) == JRN_OK && stage(&j, "b", 0, 1, 30, 4, fresh_val(1, 30)) == JRN_OK && jrn_flush(&j) == JRN_OK, "two steps");
    CHECK(raw_read(K, 1, JRN_REC_BASE, rec0, sizeof rec0) == 0, "read rec");
    memset(mark, 0xEE, sizeof mark);
    CHECK(raw_write(K, 2, 4096, mark, sizeof mark) == 0, "mark the spare's body");
    plant_hdr(K, 1, F[k].idx, F[k].ring, F[k].ver);
    card_remount();
    wr = rd_writes;
    CHECK(jopen(&j, K) == JRN_E_VERSION, "%s: open must say JRN_E_VERSION", F[k].what);
    CHECK(j.readonly == 1 && j.foreign == 1 && j.anchor == JRN_ANCHOR_EMPTY && !j.seg_first && !j.tail_seg, "%s: read-only, foreign, empty", F[k].what);
    CHECK(jrn_prepare(&j) == JRN_E_VERSION && jrn_compact(&j) == JRN_E_VERSION && jrn_prepare_first(&j) == JRN_E_VERSION, "%s: every safe-moment write refuses", F[k].what);
    CHECK(jrn_step_begin(&j, "x", 0) == JRN_E_RDONLY && jrn_flush(&j) == JRN_E_RDONLY, "%s: no recording", F[k].what);
    CHECK(rd_writes == wr, "%s: wrote %lu sectors", F[k].what, rd_writes - wr);
    CHECK(raw_read(K, 1, JRN_REC_BASE, rec1, sizeof rec1) == 0 && memcmp(rec0, rec1, sizeof rec0) == 0, "%s: the foreign slot's records are untouched (NOT zero-filled)", F[k].what);
    CHECK(raw_read(K, 2, 4096, back, sizeof back) == 0 && memcmp(mark, back, sizeof mark) == 0, "%s: no other slot was zeroed", F[k].what);
  }
  /* second line of defence: the journal opened fine, then a newer build activates the NEXT slot behind
   * our back; activating it must refuse instead of zeroing the newer build's segment */
  card_fresh(FM_FAT); img_fill(1); rd_fattime_hook = jrn_fattime_filter;
  CHECK(jopen(&j, K) == JRN_OK && jrn_prepare_first(&j) == JRN_OK, "first segment only");
  memset(mark, 0xEE, sizeof mark);
  CHECK(raw_write(K, 2, 4096, mark, sizeof mark) == 0, "mark slot 2");
  plant_hdr(K, 2, 2, 17, 2);
  CHECK(jrn_prepare(&j) == JRN_E_VERSION && j.foreign == 1, "activating over a foreign slot refuses");
  CHECK(raw_read(K, 2, 4096, back, sizeof back) == 0 && memcmp(mark, back, sizeof mark) == 0, "the newer build's slot was NOT zeroed");
  CHECK(jrn_flush(&j) == JRN_OK, "the session's own tail still flushes nothing pending");
  /* a bad-crc header is FREE, not foreign (a torn write never reads as somebody else's data) */
  world(&j, FM_FAT);
  plant_hdr(K, 1, 1, 17, 2);
  { uint8_t z = 0x55; CHECK(raw_write(K, 1, 30, &z, 1) == 0, "break the crc"); }
  CHECK(jopen(&j, K) == JRN_OK && j.foreign == 0, "a bad-crc header is a free slot, not foreign");
}

static void t_discard_marker(void) {
  Jrn j; static uint8_t st0[NREG][RSZ]; uint32_t avail, total;
  world(&j, FM_FAT);
  memcpy(st0, g_img, sizeof g_img);
  CHECK(stage(&j, "a", 0, 1, 10, 4, fresh_val(1, 10)) == JRN_OK && stage(&j, "b", 0, 1, 30, 4, fresh_val(1, 30)) == JRN_OK && jrn_flush(&j) == JRN_OK, "two recorded steps");
  memcpy(g_img, st0, sizeof g_img);                          /* the session's steps never reached the .sav */
  CHECK(jopen(&j, K) == JRN_OK && jrn_cursor(&j) == 0 && jrn_tip(&j) == 2, "offer: cursor 0, target step 2");
  CHECK(jrn_redo_info(&j, &avail, &total, 0) == 0 && total == 2 && avail == 2, "the offer is 2 steps");
  CHECK(jrn_offer(&j) == 1, "two recorded steps the image lacks: an offer");
  CHECK(jrn_mark_discarded(&j) == JRN_OK && jrn_flush(&j) == JRN_OK, "the user declines");
  CHECK(jrn_offer(&j) == 0, "declined: no offer in this session");
  CHECK(jopen(&j, K) == JRN_OK && jrn_cursor(&j) == 0, "reopen");
  CHECK(jrn_offer(&j) == 0, "a discarded step is NOT re-offered on the next load");
  CHECK(jrn_redo_info(&j, &avail, &total, 0) == 0 && total == 0, "a discarded step is NOT re-offered (total %u)", (unsigned)total);
  CHECK(j.anchor == JRN_ANCHOR_MATCH, "anchor still matches after the marker (%d)", j.anchor);
}

int main(void) {
  t_crc_and_keys();
  t_format();
  t_roundtrip_undo_redo();
  t_two_pass_no_partial();
  t_pending_pop_and_overflow();
  t_crossed_floors();
  t_anchor_branches();
  t_anchor_follows_last_path();
  t_zero_in_place();
  t_segments_and_full();
  t_first_fill_partial();
  t_ring();
  t_half_header();
  t_compaction_floor();
  t_redirects();
  t_verify_stops();
  t_readonly();
  t_discard_marker();
  t_version_foreign();
  t_header_sector_alone();
  t_staged_first_fill();
  t_long_session();
  t_read_cost();
  t_index_after_retire();
  t_read_error_honesty();
  t_partial_flush_stops();
  t_no_segment_refusal();
  if (fails) { printf("host_journal_test: %d FAILED of %lu checks\n", fails, checks); return 1; }
  printf("host_journal_test: all %lu checks passed (real lib/fatfs over a RAM disk)\n", checks);
  return 0;
}
