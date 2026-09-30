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
  CHECK(memcmp(h, "PDJS", 4) == 0 && h[4] == 1 && h[5] == 0 && h[8] == 1, "segment header magic/ver/index");
  CHECK(jrn_crc32_update(0, h, 28) == (uint32_t)(h[28] | h[29] << 8 | h[30] << 16 | (uint32_t)h[31] << 24), "segment header crc");
  CHECK(raw_read(K, 1, JRN_SEG_HDR, r, sizeof r) == 0, "read record");
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
      CHECK(raw_read(K, 1, JRN_SEG_HDR, rec, sizeof rec) == 0, "read S1");
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
  /* a cut mid-creation leaves an INCOMPLETE segment file: prepare completes it in place */
  snprintf(p, sizeof p, ROOT "/%s/0004.pdj", hex);
  {
    FIL f; UINT bw; uint8_t z[1000]; memset(z, 0, sizeof z);
    CHECK(f_open(&f, p, FA_WRITE | FA_CREATE_NEW) == FR_OK && f_write(&f, z, sizeof z, &bw) == FR_OK && f_close(&f) == FR_OK, "plant a 1000-byte partial segment");
  }
  CHECK(jrn_fatfs.size(0, p) == 1000, "planted");
  CHECK(jopen(&j, K) == JRN_OK && j.seg_last == 3, "an incomplete file is not a segment (seg_last %u)", j.seg_last);
  CHECK(jrn_prepare(&j) == JRN_OK && j.seg_last == 4, "prepare finishes it");
  CHECK(jrn_fatfs.size(0, p) == (long)JRN_SEG_SIZE, "the partial file was completed in place, size %ld", jrn_fatfs.size(0, p));
  CHECK(jopen(&j, K) == JRN_OK && j.seg_last == 4, "and it now reads as segment 4");
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
  CHECK(jrn_fatfs.size(0, p) < 0, "the oldest segment is gone");
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
  t_compaction_floor();
  t_redirects();
  t_verify_stops();
  t_readonly();
  t_discard_marker();
  if (fails) { printf("host_journal_test: %d FAILED of %lu checks\n", fails, checks); return 1; }
  printf("host_journal_test: all %lu checks passed (real lib/fatfs over a RAM disk)\n", checks);
  return 0;
}
