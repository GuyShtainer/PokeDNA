/* Host test for the lossless Gen 3 <-> Game Boy sidecar transfer:
 * source/gen3_to_gb.c (the down converter) and source/gb_sidecar.c (the sidecar file
 * format + the merge back up). docs/GEN3-TO-GB-SIDECAR-DESIGN.md is the design.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gen3gb_test.c \
 *      source/gen3_to_gb.c source/gb_sidecar.c source/evolutions.c \
 *      source/gen3_save.c source/gen3_mon.c source/gen3_box.c source/gen3_edit.c \
 *      source/gen3_daycare.c source/data_tables.c \
 *      source/gb_edit.c source/gb_session.c source/gen1_save.c source/gen1_write.c \
 *      source/gen2_save.c source/gen2_write.c -o /tmp/hg3gb
 *
 * source/evolutions.c is GENERATED and gitignored (`python3 tools/gen_evolutions.py
 * --from-rom`, evolutions.h) -- section 5 (BACKLOG #104 R1, MAKE LEGAL's level
 * correction) needs it linked in to exercise a real evolution floor; every other
 * section still runs (evolutions.h's own weak fallbacks answer "no data") if it is
 * absent, section 5 SKIPping rather than failing.
 *   /tmp/hg3gb /Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/ (.sav files)
 *
 * Two independent corpora, exactly like the modules under test:
 *   - the Gen-3 saves (Guy's own cartridge dumps) come in as argv, the same way every
 *     other Gen-3 host test takes them, and run_host_tests.py hands them over
 *     automatically to anything that indexes argv[];
 *   - the Game Boy saves are Guy's own dumps too, but OUTSIDE the repo at a fixed path
 *     (gitignored, never published) — host_gbsession_test.c's own convention, reused
 *     here rather than re-invented. Missing files SKIP rather than fail.
 *
 * THE GUARANTEE THIS FILE PINS: gen3_to_gb() converts a Gen-3 mon down, gb_sidecar.c
 * remembers everything that conversion could not carry, and gbsc_merge_up() rebuilds
 * the ORIGINAL 80 bytes EXACTLY when nothing changed on the Game Boy side, and folds
 * in precisely the field that DID change otherwise, leaving every other byte alone.
 * That is what section 2 and section 3 below measure, over the whole real corpus.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "gen3_edit.h"
#include "data_tables.h"
#include "gen3_to_gb.h"
#include "evolutions.h"
#include "gb_sidecar.h"
#include "gb_session.h"
#include "gen1_write.h"

#define GB_ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { \
    g_check++; \
    if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
  } while (0)

/* ---- CRC-16/CCITT-FALSE, re-derived locally just for the test-vector assertion.
 * gb_sidecar.c's own crc16() is file-static; this is NOT a second implementation the
 * library depends on, only the standard check value pinned independently. */
static uint16_t crc16_ccitt_false(const uint8_t* data, uint32_t len) {
  uint16_t crc = 0xFFFFu;
  for (uint32_t i = 0; i < len; i++) {
    crc = (uint16_t)(crc ^ ((uint16_t)data[i] << 8));
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
  }
  return crc;
}

/* ---- shared small helpers --------------------------------------------------- */

static bool gb_record_clean(const GbEditMon* e) {
  GbIssues iss;
  if (gb_check(e, &iss)) return true;
  return iss.stats_stale && !iss.species_bad && !iss.list_mismatch && !iss.level_range &&
         !iss.level_exp_bad && !iss.move_empty && !iss.move_hole && !iss.move_range &&
         !iss.move_dup && !iss.pp_over && !iss.pp_on_empty && !iss.gen1_type_bad;
}

static uint8_t iv_to_dv(uint8_t iv) { return (uint8_t)(iv / 2u); }

/* ============================================================================ */
/* 1. CRC vector, sidecar format round trip, corruption/version refusal, key    */
/*    stability.                                                                */
/* ============================================================================ */

static void test_format(void) {
  printf("== 1. sidecar format ==\n");
  CHECK(crc16_ccitt_false((const uint8_t*)"123456789", 9) == 0x29B1u,
        "CRC-16/CCITT-FALSE standard test vector");

  static uint8_t buf[GBSC_FILE_MAX];
  uint8_t dv4[4] = { 5, 6, 7, 8 };
  uint8_t otname[GB_NAME_BYTES] = { 0x81, 0x82, 0x83, 0x50, 0x50, 0x50,
                                    0x50, 0x50, 0x50, 0x50, 0x50 };
  uint64_t key = gbsc_key(GB_GEN2, 0x1234, dv4, otname);
  char hex[17];
  gbsc_key_hex(key, hex);
  CHECK(strlen(hex) == 16, "gbsc_key_hex writes 16 hex digits");

  uint32_t len = (uint32_t)gbsc_init(buf, key);
  CHECK((int)len == GBSC_HEADER, "gbsc_init returns the header length");
  CHECK(gbsc_count(buf, len) == 0, "a fresh file counts 0 entries");

  GbscEntry entries[GBSC_MAX_ENTRIES];
  for (int i = 0; i < GBSC_MAX_ENTRIES; i++) {
    memset(&entries[i], 0, sizeof entries[i]);
    entries[i].gen             = GB_GEN2;
    entries[i].species_written = (uint16_t)(1 + i);
    entries[i].otid16          = 0x1234;
    memcpy(entries[i].dv4, dv4, 4);
    memcpy(entries[i].otname_written, otname, GB_NAME_BYTES);
    memset(entries[i].nick_written, 0x50, GB_NAME_BYTES);
    entries[i].exp_written = 1000u * (uint32_t)i;
    entries[i].rtc_epoch   = 0;
    memset(entries[i].original80, (uint8_t)(0x10 + i), 80);
    int idx = gbsc_add(buf, &len, sizeof buf, &entries[i]);
    CHECK(idx == i, "entry %d lands at index %d (got %d)", i, i, idx);
  }
  CHECK(gbsc_count(buf, len) == GBSC_MAX_ENTRIES, "count == GBSC_MAX_ENTRIES after filling");
  {
    GbscEntry ninth; memset(&ninth, 0, sizeof ninth);
    CHECK(gbsc_add(buf, &len, sizeof buf, &ninth) == -1, "a 9th entry is refused (full)");
  }

  for (int i = 0; i < GBSC_MAX_ENTRIES; i++) {
    GbscEntry got;
    CHECK(gbsc_get(buf, len, i, &got), "gbsc_get(%d)", i);
    CHECK(memcmp(&got, &entries[i], sizeof got) == 0, "entry %d round-trips exactly", i);
  }

  /* corrupt one byte inside entry 3, then restore it */
  uint32_t victim = GBSC_HEADER + 3u * GBSC_ENTRY + 50u;
  uint8_t saved = buf[victim];
  buf[victim] ^= 0xFFu;
  CHECK(gbsc_count(buf, len) == -1, "a corrupted byte inside an entry makes count == -1");
  buf[victim] = saved;
  CHECK(gbsc_count(buf, len) == GBSC_MAX_ENTRIES, "restoring the byte makes it valid again");

  uint8_t saved_ver = buf[4];
  buf[4] = 2;
  CHECK(gbsc_count(buf, len) == -1, "an unknown version makes count == -1");
  buf[4] = saved_ver;

  /* gbsc_find: every entry above shares the same gen/otid16/dv4/otname (species is
   * excluded from the fingerprint by design), so `start` is what tells them apart. */
  uint8_t rec0[GB_MAX_REC]; uint8_t nm0[GB_NAME_BYTES];
  memset(rec0, 0, sizeof rec0); memset(nm0, 0x50, sizeof nm0);
  GbEditMon now;
  CHECK(gb_load_parts(&now, GB_GEN2, false, rec0, nm0, nm0, 0), "build a probe record");
  gb_set_otid(&now, 0x1234);
  gb_set_dv(&now, GB_ATK, dv4[0]); gb_set_dv(&now, GB_DEF, dv4[1]);
  gb_set_dv(&now, GB_SPE, dv4[2]); gb_set_dv(&now, GB_SPC, dv4[3]);
  gb_set_otname_raw(&now, otname);
  for (int i = 0; i < GBSC_MAX_ENTRIES; i++)
    CHECK(gbsc_find(buf, len, &now, i, true) == i, "gbsc_find(start=%d) returns %d", i, i);
  CHECK(gbsc_find(buf, len, &now, GBSC_MAX_ENTRIES, true) == -1, "gbsc_find past the end returns -1");

  /* ==== S5-C Part B2: claimed/keep-asked flags ================================
   * gbsc_set_claimed / gbsc_flags_get/set and gbsc_find's new include_claimed gate --
   * docs/GEN3-TO-GB-SIDECAR-DESIGN.md section 12: gb_reconcile_on_load() marks an
   * entry claimed once its Gen-3 original is released, and must never re-offer it;
   * the merge UP (gbsc_find(..., true)) must still find it regardless. */
  {
    CHECK(gbsc_set_claimed(buf, len, 3, true) == 0, "gbsc_set_claimed(3, true)");
    CHECK(gbsc_count(buf, len) == GBSC_MAX_ENTRIES,
          "the file still validates after claiming entry 3 (its own crc16 was rewritten)");
    GbscEntry got3;
    CHECK(gbsc_get(buf, len, 3, &got3) && got3.claimed == 1,
          "entry 3 decodes with claimed == 1");
    for (int i = 0; i < GBSC_MAX_ENTRIES; i++)
      CHECK((gbsc_find(buf, len, &now, i, false) == i) == (i != 3),
            "gbsc_find(start=%d, include_claimed=false) %s entry 3",
            i, i == 3 ? "skips" : "still returns");
    CHECK(gbsc_find(buf, len, &now, 0, true) == 0,
          "gbsc_find(..., include_claimed=true) still finds claimed entry 3 (from 0)");
    CHECK(gbsc_find(buf, len, &now, 3, true) == 3,
          "gbsc_find(..., include_claimed=true) finds entry 3 itself, unlike start=3 above");
    CHECK(gbsc_set_claimed(buf, len, 3, false) == 0, "gbsc_set_claimed(3, false) unclaims it");
    CHECK(gbsc_get(buf, len, 3, &got3) && got3.claimed == 0, "entry 3 decodes with claimed == 0 again");
    CHECK(gbsc_find(buf, len, &now, 0, false) == 0,
          "unclaimed again: include_claimed=false finds entry 0 first, same as before");
    CHECK(gbsc_set_claimed(buf, len, GBSC_MAX_ENTRIES, true) == -1,
          "gbsc_set_claimed refuses an out-of-range index");

    CHECK(gbsc_flags_get(buf, len) == 0, "flags start at 0 (gbsc_init never sets any bit)");
    CHECK(gbsc_flags_set(buf, len, GBSC_FLAG_KEEP_ASKED) == 0, "gbsc_flags_set(KEEP_ASKED)");
    CHECK(gbsc_count(buf, len) == GBSC_MAX_ENTRIES,
          "the file still validates after a flags write (header crc16 was rewritten)");
    CHECK(gbsc_flags_get(buf, len) == GBSC_FLAG_KEEP_ASKED, "flags read back KEEP_ASKED");
    CHECK(gbsc_flags_set(buf, len, 0) == 0, "flags cleared back to 0 (leaves the rest of the run clean)");
    CHECK(gbsc_flags_get(buf, len) == 0, "flags read back 0 after clearing");
  }

  /* S5-C review #3: GBSC_FLAG_KEEP_ASKED must not silence a NEW entry a key just
   * gained -- pdna_gen12.c's gb_paste_write() now clears it right after a
   * successful gbsc_add(), before the file is written. This is that exact
   * sequence (set flag -> add entry -> flag clear) at the codec level, on a FRESH
   * small file (the 8/8-full `buf` above has no room left for gbsc_add). */
  {
    uint8_t kbuf[GBSC_FILE_MAX];
    uint32_t klen = (uint32_t)gbsc_init(kbuf, 0xABCDu);
    GbscEntry e0; memset(&e0, 0, sizeof e0);
    e0.gen = GB_GEN2; e0.otid16 = 0x1111;
    memcpy(e0.otname_written, otname, GB_NAME_BYTES);
    CHECK(gbsc_add(kbuf, &klen, sizeof kbuf, &e0) == 0, "keep-asked: first entry added");

    CHECK(gbsc_flags_set(kbuf, klen, GBSC_FLAG_KEEP_ASKED) == 0, "keep-asked: flag set (user chose B once)");
    CHECK(gbsc_flags_get(kbuf, klen) == GBSC_FLAG_KEEP_ASKED, "keep-asked: flag reads back set");

    /* A second transfer to the SAME key (re-transferred after a merge-up, or a
     * genuinely new clone) appends a new entry -- gbsc_add() itself never touches
     * flags (this asserts that division of responsibility explicitly). */
    GbscEntry e1 = e0;
    CHECK(gbsc_add(kbuf, &klen, sizeof kbuf, &e1) == 1, "keep-asked: second entry added");
    CHECK(gbsc_flags_get(kbuf, klen) == GBSC_FLAG_KEEP_ASKED,
          "keep-asked: gbsc_add() alone does NOT clear the flag");

    /* gb_paste_write()'s own fix: clear the flag right after the add. */
    uint16_t kflags = gbsc_flags_get(kbuf, klen);
    CHECK(gbsc_flags_set(kbuf, klen, kflags & ~GBSC_FLAG_KEEP_ASKED) == 0,
          "keep-asked: gb_paste_write's clear-after-add succeeds");
    CHECK(gbsc_flags_get(kbuf, klen) == 0, "keep-asked: flag reads back CLEAR after the fix");
    CHECK(gbsc_count(kbuf, klen) == 2, "keep-asked: both entries still present after the flag round-trip");
  }
  /* ==== END S5-C Part B2 ======================================================== */

  /* remove entry 3, check compaction */
  uint32_t len2 = len;
  CHECK(gbsc_remove(buf, &len2, 3) == 0, "gbsc_remove(3)");
  CHECK(gbsc_count(buf, len2) == GBSC_MAX_ENTRIES - 1, "count decremented by one");
  for (int i = 0; i < GBSC_MAX_ENTRIES - 1; i++) {
    int src = (i < 3) ? i : i + 1;
    GbscEntry got;
    CHECK(gbsc_get(buf, len2, i, &got), "gbsc_get after remove, index %d", i);
    CHECK(memcmp(&got, &entries[src], sizeof got) == 0,
          "post-remove index %d matches original entry %d", i, src);
  }

  /* key stability: a PINNED known-answer, not `k1 == k2` on identical inputs (that
   * tautology passes even if gbsc_key were replaced by a constant). The value below
   * was computed once, by hand, from the FNV-1a-64 definition in gb_sidecar.h over
   * the same 18-byte sequence gbsc_key builds -- gen(1) otid16-LE(2) dv4(4)
   * otname(11) -- for exactly the `dv4`/`otname` fixture above with otid16 0x1234,
   * so this test also pins the byte ORDER, not just that some hash comes out. */
  uint64_t k1 = gbsc_key(GB_GEN2, 0x1234, dv4, otname);
  CHECK(k1 == 0xADB7F8303693F215ULL, "gbsc_key matches the pinned known-answer (got 0x%016llX)",
        (unsigned long long)k1);
  uint8_t otname2[GB_NAME_BYTES]; memcpy(otname2, otname, GB_NAME_BYTES); otname2[0] ^= 1;
  uint64_t k3 = gbsc_key(GB_GEN2, 0x1234, dv4, otname2);
  CHECK(k1 != k3, "a different otname byte changes the key");
}

/* ============================================================================ */
/* 1b. Codec negatives: malformed headers gbsc_count() must reject.             */
/* ============================================================================ */

static void test_codec_negatives(void) {
  printf("== 1b. codec negatives ==\n");
  uint8_t buf[GBSC_HEADER];
  CHECK(gbsc_init(buf, 0x1122334455667788ULL) == GBSC_HEADER, "init for negatives setup");

  uint8_t bad_magic[GBSC_HEADER];
  memcpy(bad_magic, buf, GBSC_HEADER);
  bad_magic[0] ^= 0xFFu;
  CHECK(gbsc_count(bad_magic, GBSC_HEADER) == -1, "bad magic -> count == -1");

  CHECK(gbsc_count(buf, GBSC_HEADER - 1) == -1, "len < GBSC_HEADER -> count == -1");

  uint8_t len_mismatch[GBSC_HEADER];
  memcpy(len_mismatch, buf, GBSC_HEADER);
  len_mismatch[5] = 1;   /* claims one entry follows, but len below still says zero */
  CHECK(gbsc_count(len_mismatch, GBSC_HEADER) == -1,
        "len != GBSC_HEADER + count*GBSC_ENTRY -> count == -1");
}

/* ============================================================================ */
/* 1c. Synthetic unit tests -- built fresh, independent of the corpus.          */
/* ============================================================================ */

/* STOP/REPORT (S5-A review item 8, "one synthetic lossy-name Gen-3 record...
 * exercising both _lossy branches and the first_bad fields"): that record cannot be
 * built. gen3_mon.c's decode_name() runs every nickname/OT byte through
 * gen3_save.c's gen3_decode_char(), whose only outputs are space, '0'-'9', 'A'-'Z',
 * 'a'-'z', eight punctuation marks (!?.-',/), and '?' for EVERY OTHER BYTE VALUE
 * ("default: return '?'", gen3_save.c). Every one of those characters has an
 * explicit, non-lossy case in gb_edit.c's enc_one() for BOTH generations, so
 * gen3_to_gb()'s nick_lossy/ot_lossy branches (and the gb_set_nickname_lossy/
 * gb_set_otname_lossy calls behind them) are UNREACHABLE from any record this
 * decoder can produce today -- not a gap in gen3_to_gb.c, which still carries the
 * correct, defensive handling for the day gen3_mon.c's decoder gets richer (real
 * cartridges DO store gender signs and accented letters in nicknames; this
 * decode_name() just does not surface them as such). Changing that decoder is out
 * of S5-A's scope. Reported rather than faking a test that would either never run
 * its intended branch or assert something false.
 *
 * What IS pinned here instead: the "unreachable" claim itself, by feeding every
 * character gen3_decode_char can ever produce through the real gb_text_lossy() and
 * requiring zero for both generations. If gen3_mon.c's decoder is ever widened, or
 * gb_edit.c's charset support ever narrows, this starts failing and says why. */
static void test_lossy_name(void) {
  printf("== 1c. nick/OT-name losslessness (nick_lossy is unreachable today -- see comment) ==\n");
  static const char reachable[] = " 0123456789"
                                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                  "abcdefghijklmnopqrstuvwxyz"
                                  "!?.-',/";
  for (uint8_t g = GB_GEN1; g <= GB_GEN2; g++) {
    char one[2] = { 0, 0 };
    for (const char* p = reachable; *p; p++) {
      one[0] = *p;
      CHECK(gb_text_lossy(g, one, 1, NULL) == 0,
            "gen %u: '%c' (everything gen3_decode_char can produce) is not lossy", g, *p);
    }
  }

  /* The synthetic build/convert path still works end to end for a plain name --
   * the same non-lossy result check_conversion() asserts across the real corpus. */
  uint8_t rec[80];
  gen3_build_mon(1 /* Bulbasaur */, 10, 0x87654321u, 0xBEEF0003u, "OTNAME", 3, rec);
  GbEditMon out; Gen3ToGbLoss loss;
  G3GbStatus st = gen3_to_gb(rec, GB_GEN2, NULL, &out, &loss);
  CHECK(st == G3GB_OK, "plain-name synthetic mon converts (%s)", g3gb_status_text(st));
  if (st == G3GB_OK) {
    CHECK(!loss.nick_lossy, "a plain name is not lossy");
    CHECK(!loss.ot_lossy, "a plain OT name is not lossy");
  }
}

/* Deterministic EV -> stat-exp and IV -> DV-halving checks on a synthetic record,
 * independent of what the real corpus happens to contain. */
static void test_stat_exp_and_ivs(void) {
  printf("== 1c. synthetic EV/IV -> stat-exp/DV ==\n");
  uint8_t rec[80];
  gen3_build_mon(25 /* Pikachu */, 20, 0x11112222u, 0xAAAA0002u, "EVTEST", 3, rec);

  EditMon em;
  gen3_edit_load(rec, false, &em);
  em_set_ev(&em, PK_HP, 4);
  em_set_ev(&em, PK_ATK, 100);
  em_set_ev(&em, PK_DEF, 0);
  em_set_ev(&em, PK_SPE, 252);
  em_set_ev(&em, PK_SPA, 6);
  em_set_ev(&em, PK_SPD, 252);
  em_set_iv(&em, PK_ATK, 31);   /* odd -> ivs_halved */
  em_set_iv(&em, PK_DEF, 20);
  em_set_iv(&em, PK_SPE, 20);
  em_set_iv(&em, PK_SPA, 20);
  em_set_iv(&em, PK_SPD, 20);
  gen3_edit_commit(&em, rec);

  GbEditMon out; Gen3ToGbLoss loss;
  G3GbStatus st = gen3_to_gb(rec, GB_GEN2, NULL, &out, &loss);
  CHECK(st == G3GB_OK, "EV/IV synthetic mon converts (%s)", g3gb_status_text(st));
  if (st == G3GB_OK) {
    CHECK(gb_get_statexp(&out, GB_HP)  == (uint16_t)(4u   * 257u), "stat exp HP = ev*257");
    CHECK(gb_get_statexp(&out, GB_ATK) == (uint16_t)(100u * 257u), "stat exp Atk = ev*257");
    CHECK(gb_get_statexp(&out, GB_DEF) == 0u,                      "stat exp Def = 0");
    CHECK(gb_get_statexp(&out, GB_SPE) == (uint16_t)(252u * 257u), "stat exp Spe = ev*257");
    CHECK(gb_get_statexp(&out, GB_SPC) == (uint16_t)(252u * 257u),
          "stat exp Spc = max(SpA=6, SpD=252)*257");
    CHECK(loss.ivs_halved, "an odd Atk IV sets ivs_halved");
  }

  /* all-even, SpA == SpD: ivs_halved must be false */
  em_set_iv(&em, PK_ATK, 20);
  em_set_iv(&em, PK_SPA, 18);
  em_set_iv(&em, PK_SPD, 18);
  gen3_edit_commit(&em, rec);
  st = gen3_to_gb(rec, GB_GEN2, NULL, &out, &loss);
  CHECK(st == G3GB_OK, "all-even synthetic mon converts (%s)", g3gb_status_text(st));
  if (st == G3GB_OK)
    CHECK(!loss.ivs_halved, "all-even IVs with SpA==SpD -> ivs_halved is false");
}

/* The Gen-3 charset is ASCII-only (gen3_edit.h's gen3_encode_char). A Game Boy
 * nickname decoded either to a "{XX}" escape (an unassigned byte with no text
 * spelling -- 0x5D, gb_edit.c's own example, <TRAINER>) or to a real non-ASCII glyph
 * the Game Boy CAN spell but Gen 3 cannot (0xEF, the male sign) must both be refused
 * by gbsc_merge_up's merge_nickname(), keeping the sidecar's own name untouched. */
static void test_rename_refused(void) {
  printf("== 1c. rename_refused: {XX} escape and a non-ASCII glyph ==\n");
  uint8_t rec[80];
  gen3_build_mon(1 /* Bulbasaur */, 10, 0x12345678u, 0xABCD0001u, "TESTER", 3, rec);

  GbEditMon out; Gen3ToGbLoss loss;
  G3GbStatus st = gen3_to_gb(rec, GB_GEN2, NULL, &out, &loss);
  CHECK(st == G3GB_OK, "synthetic mon converts for Gen 2 (%s)", g3gb_status_text(st));
  if (st != G3GB_OK) return;

  GbscEntry e;
  gbsc_entry_from(&e, &out, rec, 0);

  {
    GbEditMon chg = out;
    uint8_t nick[GB_NAME_BYTES] = { 0x5Du, 0x50u, 0x50u, 0x50u, 0x50u,
                                    0x50u, 0x50u, 0x50u, 0x50u, 0x50u, 0x50u };
    gb_set_nickname_raw(&chg, nick);
    uint8_t back80[80]; GbscMergeReport rep;
    CHECK(gbsc_merge_up(&e, &chg, back80, &rep), "escape: merge up succeeds");
    CHECK(rep.rename_refused && !rep.renamed, "escape: rename_refused set, renamed not set");
    CHECK(memcmp(back80, rec, 80) == 0, "escape: record byte-identical (name never applied)");
  }
  {
    GbEditMon chg = out;
    uint8_t nick[GB_NAME_BYTES] = { 0xEFu, 0x50u, 0x50u, 0x50u, 0x50u,
                                    0x50u, 0x50u, 0x50u, 0x50u, 0x50u, 0x50u };
    gb_set_nickname_raw(&chg, nick);
    uint8_t back80[80]; GbscMergeReport rep;
    CHECK(gbsc_merge_up(&e, &chg, back80, &rep), "gender sign: merge up succeeds");
    CHECK(rep.rename_refused && !rep.renamed,
          "gender sign: rename_refused set, renamed not set");
    CHECK(memcmp(back80, rec, 80) == 0,
          "gender sign: record byte-identical (name never applied)");
  }
  {
    /* A third reason, caught by review: '(' (GB byte 0x9A) is representable in BOTH
     * generations' own charsets, but gen3_edit.c's gen3_encode_char has no case for it
     * and falls to `default: return 0x00` ("unknown -> space") -- so letting it through
     * to em_set_nickname would silently rewrite the name to a space rather than refuse.
     * Same for ')' ':' ';' '[' ']' '&' '$'; '(' stands in for all eight here. */
    GbEditMon chg = out;
    uint8_t nick[GB_NAME_BYTES] = { 0x9Au, 0x50u, 0x50u, 0x50u, 0x50u,
                                    0x50u, 0x50u, 0x50u, 0x50u, 0x50u, 0x50u };
    gb_set_nickname_raw(&chg, nick);
    uint8_t back80[80]; GbscMergeReport rep;
    CHECK(gbsc_merge_up(&e, &chg, back80, &rep), "'(' : merge up succeeds");
    CHECK(rep.rename_refused && !rep.renamed, "'(' : rename_refused set, renamed not set");
    CHECK(memcmp(back80, rec, 80) == 0, "'(' : record byte-identical (name never applied)");
  }
}

/* ============================================================================ */
/* 2/3. The Gen-3 corpus: every party + PC-box mon, both target generations.    */
/* ============================================================================ */

static int g_tested[3], g_accepted[3], g_refused[3][7], g_roundtrip[3];

/* A representative accepted conversion per target generation, kept for sections 3
 * and 4 -- section 3's GB-side-change subtests use the GEN2 one (no base-stat table
 * needed); section 4 needs one insertable sample per generation, and a species over
 * 151 accepted for Gen 2 is routinely refused for Gen 1 (gb_max_species), so each
 * generation keeps its own rather than section 4 re-deriving one that may not exist. */
static bool     g_have_sample[3];
static uint8_t  g_sample_rec[3][80];
static GbEditMon g_sample_out[3];
static GbscEntry g_sample_entry[3];

static void check_conversion(const uint8_t* rec, uint8_t gen, const GbGen1Base* base) {
  PkMon m;
  if (!pk_decode_mon(rec, false, &m)) return;         /* empty slot: not this test's subject */
  if (m.isBadEgg) return;                              /* corrupt/hacked: skip, like host_edit_test.c */
  pk_resolve(&m);

  g_tested[gen]++;
  GbEditMon out; Gen3ToGbLoss loss;
  G3GbStatus st = gen3_to_gb(rec, gen, base, &out, &loss);
  g_refused[gen][st]++;
  if (st != G3GB_OK) return;
  g_accepted[gen]++;

  CHECK(gb_record_clean(&out), "gb_check clean (species=%u gen=%u)", m.species, gen);
  CHECK(gb_get_dv(&out, GB_ATK) == iv_to_dv(m.ivs[PK_ATK]), "DV Atk == IV Atk / 2");
  CHECK(gb_get_dv(&out, GB_DEF) == iv_to_dv(m.ivs[PK_DEF]), "DV Def == IV Def / 2");
  CHECK(gb_get_dv(&out, GB_SPE) == iv_to_dv(m.ivs[PK_SPE]), "DV Spe == IV Spe / 2");
  CHECK(gb_get_dv(&out, GB_SPC) == iv_to_dv(m.ivs[PK_SPA]), "DV Spc == IV SpA / 2");
  CHECK(gb_get_level(&out) == m.level, "level matches (%u vs %u)", gb_get_level(&out), m.level);
  for (int i = 0; i < 4; i++)
    CHECK(gb_get_move(&out, i) == (uint8_t)m.moves[i], "move %d matches", i);

  char nb[GB_TEXT_MAX], ob[GB_TEXT_MAX];
  gb_get_nickname(&out, nb, sizeof nb);
  if (!loss.nick_lossy) CHECK(strcmp(nb, m.nickname) == 0, "nickname round-trips exactly");
  else CHECK(gb_text_lossy(gen, m.nickname, GB_NICK_GLYPHS, NULL) != 0,
             "nick_lossy implies gb_text_lossy is non-zero");
  gb_get_otname(&out, ob, sizeof ob);
  if (!loss.ot_lossy) CHECK(strcmp(ob, m.otName) == 0, "OT name round-trips exactly");
  else CHECK(gb_text_lossy(gen, m.otName, GB_OT_GLYPHS, NULL) != 0,
             "ot_lossy implies gb_text_lossy is non-zero");

  /* sidecar round trip: encode, decode, find, merge up with an UNCHANGED `out` must
   * reproduce `rec`'s 80 bytes exactly -- the whole point of the feature. */
  GbscEntry e;
  gbsc_entry_from(&e, &out, rec, 0);
  static uint8_t filebuf[GBSC_FILE_MAX];
  uint8_t dv4[4] = { gb_get_dv(&out, GB_ATK), gb_get_dv(&out, GB_DEF),
                     gb_get_dv(&out, GB_SPE), gb_get_dv(&out, GB_SPC) };
  uint64_t key = gbsc_key(gen, gb_get_otid(&out), dv4, out.otname);
  uint32_t flen = (uint32_t)gbsc_init(filebuf, key);
  CHECK(gbsc_add(filebuf, &flen, sizeof filebuf, &e) == 0, "sidecar add");
  GbscEntry back;
  CHECK(gbsc_get(filebuf, flen, 0, &back), "sidecar get");
  CHECK(memcmp(&back, &e, sizeof back) == 0, "sidecar entry decodes back exactly");
  CHECK(gbsc_find(filebuf, flen, &out, 0, true) == 0, "gbsc_find locates the entry at index 0");

  uint8_t back80[80];
  GbscMergeReport rep;
  CHECK(gbsc_merge_up(&back, &out, back80, &rep), "gbsc_merge_up succeeds");
  if (memcmp(back80, rec, 80) == 0) {
    g_roundtrip[gen]++;
  } else {
    CHECK(false, "merge-up byte-identical round trip (species=%u gen=%u)", m.species, gen);
  }

  if (!g_have_sample[gen] && !rep.evolved && !rep.level_changed &&
      !rep.moves_changed && !rep.renamed) {
    g_have_sample[gen] = true;
    memcpy(g_sample_rec[gen], rec, 80);
    g_sample_out[gen] = out;
    g_sample_entry[gen] = e;
  }
}

static void run_corpus_file(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP %s (cannot open)\n", path); return; }
  static uint8_t save[G3_SAVE_FILE_SIZE];
  size_t n = fread(save, 1, sizeof save, f);
  fclose(f);
  Gen3SaveInfo info;
  if (!gen3_parse(save, (uint32_t)n, &info)) { printf("  SKIP %s (parse failed)\n", path); return; }

  static uint8_t sb1[G3_SAVEBLOCK1_BYTES];
  static uint8_t pc[G3_PC_BYTES];
  gen3_read_saveblock1(save, info.slot, sb1);
  gen3_read_pc_storage(save, info.slot, pc);

  PkMon party[6]; bool frlg = false;
  int party_n = pk_read_party_auto(sb1, party, &frlg);
  (void)party_n;
  uint16_t doff = frlg ? 0x0038 : 0x0238;
  uint16_t coff = frlg ? 0x0034 : 0x0234;
  uint8_t count = sb1[coff];
  if (count > 6) count = 6;

  GbGen1Base g1base;
  memset(g1base.base, 50, sizeof g1base.base);   /* test stand-in: no real Gen-1 base
                                                    * table is required for this slice
                                                    * (that lands with the UI slice, per
                                                    * the S5-A brief). */
  g1base.type1 = g1base.type2 = 0x14;            /* Fire, an arbitrary valid Gen-1 type */

  int tested = 0;
  for (int i = 0; i < count; i++) {
    const uint8_t* rec = sb1 + doff + (uint32_t)i * 100;   /* the 80-byte core only */
    check_conversion(rec, GB_GEN2, NULL);
    check_conversion(rec, GB_GEN1, &g1base);
    tested++;
  }
  for (int b = 0; b < G3_TOTAL_BOXES; b++)
    for (int s = 0; s < G3_IN_BOX; s++) {
      const uint8_t* rec = pc + 0x0004 + ((uint32_t)b * G3_IN_BOX + s) * 80;
      check_conversion(rec, GB_GEN2, NULL);
      check_conversion(rec, GB_GEN1, &g1base);
      tested++;
    }
  printf("  %s: %d slots scanned\n", path, tested);
}

/* ============================================================================ */
/* 3. GB-side changes between the down transfer and the merge up.               */
/* ============================================================================ */

static void test_gb_side_changes(void) {
  if (!g_have_sample[GB_GEN2]) { printf("== 3. SKIP (no clean sample conversion found) ==\n"); return; }
  printf("== 3. GB-side changes before the merge up ==\n");

  PkMon orig;
  CHECK(pk_decode_mon(g_sample_rec[GB_GEN2], false, &orig), "decode the sample's original record");
  pk_resolve(&orig);

  /* (a) level change */
  {
    GbEditMon chg = g_sample_out[GB_GEN2];
    uint8_t new_level = (gb_get_level(&chg) < 100) ? (uint8_t)(gb_get_level(&chg) + 1)
                                                    : (uint8_t)(gb_get_level(&chg) - 1);
    CHECK(gb_set_level(&chg, new_level), "(a) set new level");
    uint8_t back80[80]; GbscMergeReport rep;
    CHECK(gbsc_merge_up(&g_sample_entry[GB_GEN2], &chg, back80, &rep), "(a) merge up");
    PkMon merged; CHECK(pk_decode_mon(back80, false, &merged), "(a) merged record decodes");
    pk_resolve(&merged);
    CHECK(rep.level_changed, "(a) report says level_changed");
    CHECK(merged.level == new_level, "(a) the merged level reflects the GB value");
    CHECK(merged.species == orig.species, "(a) species unaffected");
    CHECK(memcmp(merged.ivs, orig.ivs, sizeof orig.ivs) == 0, "(a) IVs unaffected");
    CHECK(memcmp(merged.evs, orig.evs, sizeof orig.evs) == 0, "(a) EVs unaffected");
    CHECK(memcmp(merged.moves, orig.moves, sizeof orig.moves) == 0, "(a) moves unaffected");
    CHECK(strcmp(merged.nickname, orig.nickname) == 0, "(a) nickname unaffected");
    CHECK(memcmp(g_sample_rec[GB_GEN2], back80, 8) == 0, "(a) personality+otId raw bytes unchanged");
  }

  /* (a2) MUST-FIX regression (S5-A review item 2): bumping EXP by 1 WITHOUT a level
   * change must be a complete no-op. The bug this pins: an earlier revision gated
   * the level restore on "does EXP differ from exp_written" and then called
   * em_set_level(), which writes the EXP FLOOR for the level -- so any Game-Boy-side
   * EXP gain that had not yet produced a level-up (every single battle) silently
   * zeroed the Gen-3 mon's precise within-level EXP progress. Gating on LEVEL
   * instead (gb_sidecar.c's merge_species_and_level) means this must change
   * nothing at all. */
  {
    GbEditMon chg = g_sample_out[GB_GEN2];
    uint32_t old_exp = gb_get_exp(&chg);
    uint8_t old_level = gb_get_level(&chg);
    CHECK(gb_set_exp(&chg, old_exp + 1), "(a2) bump EXP by 1");
    if (gb_get_level(&chg) != old_level) {
      printf("   (a2) SKIP: +1 EXP crossed a level boundary for this particular sample\n");
    } else {
      uint8_t back80[80]; GbscMergeReport rep;
      CHECK(gbsc_merge_up(&g_sample_entry[GB_GEN2], &chg, back80, &rep), "(a2) merge up");
      CHECK(!rep.level_changed, "(a2) a same-level EXP bump does not report level_changed");
      CHECK(memcmp(back80, g_sample_rec[GB_GEN2], 80) == 0,
            "(a2) +1 EXP without a level-up is a byte-identical no-op (the fixed bug)");
    }
  }

  /* (b) move change: replace slot 0 with a different in-range move */
  {
    GbEditMon chg = g_sample_out[GB_GEN2];
    uint8_t old_mv = gb_get_move(&chg, 0);
    uint8_t new_mv = (uint8_t)((old_mv % gb_max_move(chg.gen)) + 1);
    if (new_mv == old_mv) new_mv = (uint8_t)((new_mv % gb_max_move(chg.gen)) + 1);
    CHECK(gb_set_move(&chg, 0, new_mv), "(b) set a different move");
    uint8_t back80[80]; GbscMergeReport rep;
    CHECK(gbsc_merge_up(&g_sample_entry[GB_GEN2], &chg, back80, &rep), "(b) merge up");
    PkMon merged; CHECK(pk_decode_mon(back80, false, &merged), "(b) merged record decodes");
    pk_resolve(&merged);
    CHECK(rep.moves_changed, "(b) report says moves_changed");
    CHECK(merged.moves[0] == new_mv, "(b) the merged move 0 reflects the GB value");
    CHECK(merged.species == orig.species, "(b) species unaffected");
    CHECK(merged.level == orig.level, "(b) level unaffected");
    CHECK(memcmp(merged.ivs, orig.ivs, sizeof orig.ivs) == 0, "(b) IVs unaffected");
    CHECK(strcmp(merged.nickname, orig.nickname) == 0, "(b) nickname unaffected");
    CHECK(memcmp(g_sample_rec[GB_GEN2], back80, 8) == 0, "(b) personality+otId raw bytes unchanged");
  }

  /* (c) nickname change */
  {
    GbEditMon chg = g_sample_out[GB_GEN2];
    CHECK(gb_set_nickname(&chg, "GATE"), "(c) set nickname to GATE");
    uint8_t back80[80]; GbscMergeReport rep;
    CHECK(gbsc_merge_up(&g_sample_entry[GB_GEN2], &chg, back80, &rep), "(c) merge up");
    PkMon merged; CHECK(pk_decode_mon(back80, false, &merged), "(c) merged record decodes");
    pk_resolve(&merged);
    CHECK(rep.renamed && !rep.rename_refused, "(c) report says renamed");
    CHECK(strcmp(merged.nickname, "GATE") == 0, "(c) the merged nickname reflects the GB value");
    CHECK(merged.species == orig.species, "(c) species unaffected");
    CHECK(merged.level == orig.level, "(c) level unaffected");
    CHECK(memcmp(merged.moves, orig.moves, sizeof orig.moves) == 0, "(c) moves unaffected");
    CHECK(memcmp(merged.ivs, orig.ivs, sizeof orig.ivs) == 0, "(c) IVs unaffected");
    CHECK(memcmp(g_sample_rec[GB_GEN2], back80, 8) == 0, "(c) personality+otId raw bytes unchanged");
  }

  /* (d) DV change on Atk -- design decision (2026-09-04, gb_sidecar.h): dv4 stays in
   * the sidecar's fingerprint, so editing a DV on the Game Boy ORPHANS the sidecar
   * rather than being folded into the merge. Two consequences, both asserted here:
   * gbsc_find() can no longer locate the entry by the changed record, and
   * gbsc_merge_up() called directly with the (now mismatched) original entry keeps
   * the sidecar's ORIGINAL, unedited IV rather than guessing which side is right. */
  {
    GbEditMon chg = g_sample_out[GB_GEN2];
    uint8_t old_dv = gb_get_dv(&chg, GB_ATK);
    uint8_t new_dv = (uint8_t)((old_dv + 1) & 15);
    CHECK(gb_set_dv(&chg, GB_ATK, new_dv), "(d) set a different Atk DV");

    static uint8_t filebuf[GBSC_FILE_MAX];
    uint8_t dv4[4] = { old_dv, gb_get_dv(&g_sample_out[GB_GEN2], GB_DEF),
                       gb_get_dv(&g_sample_out[GB_GEN2], GB_SPE),
                       gb_get_dv(&g_sample_out[GB_GEN2], GB_SPC) };
    uint64_t key = gbsc_key(GB_GEN2, gb_get_otid(&g_sample_out[GB_GEN2]), dv4,
                            g_sample_out[GB_GEN2].otname);
    uint32_t flen = (uint32_t)gbsc_init(filebuf, key);
    CHECK(gbsc_add(filebuf, &flen, sizeof filebuf, &g_sample_entry[GB_GEN2]) == 0,
          "(d) rebuild a one-entry sidecar file for the unedited sample");
    CHECK(gbsc_find(filebuf, flen, &chg, 0, true) == -1,
          "(d) gbsc_find no longer locates the entry once a DV changed (orphaned)");

    uint8_t back80[80]; GbscMergeReport rep;
    CHECK(gbsc_merge_up(&g_sample_entry[GB_GEN2], &chg, back80, &rep),
          "(d) merge up (bypassing find, on purpose)");
    PkMon merged; CHECK(pk_decode_mon(back80, false, &merged), "(d) merged record decodes");
    pk_resolve(&merged);
    CHECK(merged.ivs[PK_ATK] == orig.ivs[PK_ATK],
          "(d) the merged Atk IV keeps the SIDECAR's original, not the GB edit");
    CHECK(memcmp(merged.ivs, orig.ivs, sizeof orig.ivs) == 0, "(d) every IV unaffected");
    CHECK(merged.species == orig.species, "(d) species unaffected");
    CHECK(strcmp(merged.nickname, orig.nickname) == 0, "(d) nickname unaffected");
    CHECK(memcmp(back80, g_sample_rec[GB_GEN2], 80) == 0,
          "(d) an unreachable DV mismatch leaves the merge a complete no-op");
  }

  /* (e) species change (evolution stand-in): next dex number, if the generation has one */
  {
    uint16_t old_dex = gb_get_species_dex(&g_sample_out[GB_GEN2]);
    uint16_t new_dex = (uint16_t)(old_dex + 1);
    if (new_dex < 1 || new_dex > gb_max_species(g_sample_out[GB_GEN2].gen)) {
      printf("   (e) SKIP: species %u has no next dex slot in this generation\n", old_dex);
    } else {
      GbEditMon chg = g_sample_out[GB_GEN2];
      CHECK(gb_set_species(&chg, new_dex, NULL), "(e) set species to dex %u", new_dex);
      uint8_t back80[80]; GbscMergeReport rep;
      CHECK(gbsc_merge_up(&g_sample_entry[GB_GEN2], &chg, back80, &rep), "(e) merge up");
      PkMon merged; CHECK(pk_decode_mon(back80, false, &merged), "(e) merged record decodes");
      pk_resolve(&merged);
      CHECK(rep.evolved, "(e) report says evolved");
      CHECK(merged.species == new_dex, "(e) the merged species reflects the GB value (%u vs %u)",
            merged.species, new_dex);
      CHECK(merged.level == orig.level, "(e) level preserved across the species change");
      CHECK(memcmp(merged.ivs, orig.ivs, sizeof orig.ivs) == 0, "(e) IVs unaffected");
      CHECK(strcmp(merged.nickname, orig.nickname) == 0, "(e) nickname unaffected");
      CHECK(memcmp(g_sample_rec[GB_GEN2], back80, 8) == 0, "(e) personality+otId raw bytes unchanged");
    }
  }
}

/* ============================================================================ */
/* 4. Engine acceptance: insert the converted record into a real GB save.       */
/* ============================================================================ */

static int landed = 0;

static void test_engine_gen2(const char* file) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", GB_ROMS, file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP %s (not present)\n", file); return; }
  static uint8_t img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);
  if (!g_have_sample[GB_GEN2]) { printf("  SKIP %s (no sample conversion)\n", file); return; }

  GbSession s;
  static uint8_t scratch[GBS_SCRATCH_BYTES];
  GbsStatus st = gbs_open(&s, img, len, scratch, sizeof scratch);
  CHECK(st == GBS_OK, "%s: gbs_open", file);
  if (st != GBS_OK) return;

  int box = -1;
  static uint8_t list[GBS_LIST_BYTES];
  int nb = gbs_nboxes(&s);
  for (int b = 0; b < nb; b++) {
    if (gbs_box_writable(&s, b) != GBS_OK) continue;
    if (gbs_load_list(&s, b, list) != GBS_OK) continue;
    if (gb_list_count(GB_GEN2, list, b) < gb_list_capacity(GB_GEN2, b)) { box = b; break; }
  }
  if (box < 0) { printf("  %s: SKIP (no box with room)\n", file); return; }
  CHECK(gbs_load_list(&s, box, list) == GBS_OK, "%s: load box %d", file, box);

  G2Slot slot;
  memset(&slot, 0, sizeof slot);
  uint8_t rec[GB_MAX_REC], otname[GB_NAME_BYTES], nick[GB_NAME_BYTES], list_sp;
  CHECK(gb_commit_parts(&g_sample_out[GB_GEN2], rec, otname, nick, &list_sp),
        "%s: extract the sample's raw parts", file);
  memcpy(slot.rec, rec, G2_PARTY_ENTRY < sizeof rec ? G2_PARTY_ENTRY : sizeof rec);
  memcpy(slot.otname, otname, GB_NAME_BYTES);
  memcpy(slot.nickname, nick, GB_NAME_BYTES);
  slot.is_egg = false;
  slot.is_party = false;

  int slot_out = -1;
  G2WStatus ws = g2w_append(list, box, &slot, &slot_out);
  CHECK(ws == G2W_OK, "%s: g2w_append (%s)", file, g2w_status_text(ws));
  if (ws != G2W_OK) return;

  GbsStatus cs = gbs_commit_list(&s, box, list);
  CHECK(cs == GBS_OK, "%s: gbs_commit_list (%s)", file, gbs_status_text(cs));
  if (cs != GBS_OK) return;

  static uint8_t list2[GBS_LIST_BYTES];
  CHECK(gbs_load_list(&s, box, list2) == GBS_OK, "%s: reload box %d", file, box);
  GbEditMon back;
  CHECK(gb_load(&back, GB_GEN2, list2, box, slot_out), "%s: gb_load the new slot", file);
  CHECK(gb_verify_slot(&back, list2, box, slot_out), "%s: gb_verify_slot on the new slot", file);
  printf("  %s: landed in box %d slot %d\n", file, box, slot_out);
  landed++;
}

static void test_engine_gen1(const char* file) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", GB_ROMS, file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP %s (not present)\n", file); return; }
  static uint8_t img[GEN1_SAVE_SIZE];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);

  /* Uses the dedicated GEN1 sample captured in section 2 -- NOT the GEN2 one. The
   * two are independent: a GEN2-accepted species can sit at national dex 152..251,
   * which gb_max_species(GB_GEN1) (151) refuses outright, so re-deriving a Gen-1
   * record from the Gen-2 sample would fail for roughly 40% of real corpora (measured
   * while writing this test). Each generation keeping its own sample is what
   * check_conversion()'s g_sample_* arrays are indexed by `gen` for. */
  if (!g_have_sample[GB_GEN1]) { printf("  %s: SKIP (no Gen-1 sample conversion)\n", file); return; }

  Gen1Save s;
  Gen1Status gs = gen1_open(img, len, &s);
  CHECK(gs == GEN1_OK, "%s: gen1_open (%s)", file, gen1_status_text(gs));
  if (gs != GEN1_OK) return;

  int roomy = -1;
  for (int b = 0; b < GEN1_NUM_BOXES; b++) {
    if (b == s.current_box) continue;
    int c = gen1_count(&s, b);
    if (c >= 0 && c < GEN1_BOX_CAPACITY) { roomy = b; break; }
  }
  if (roomy < 0) { printf("  %s: SKIP (no box with room)\n", file); return; }

  uint8_t rec[GB_MAX_REC], otname[GB_NAME_BYTES], nick[GB_NAME_BYTES], list_sp;
  CHECK(gb_commit_parts(&g_sample_out[GB_GEN1], rec, otname, nick, &list_sp),
        "%s: extract raw parts", file);

  Gen1EditMon e;
  memset(&e, 0, sizeof e);
  e.is_party = false;
  memcpy(e.rec, rec, GEN1_BOX_REC_BYTES);
  memcpy(e.ot, otname, GEN1_NAME_BYTES);
  memcpy(e.nick, nick, GEN1_NAME_BYTES);

  Gen1Op op;
  op.kind = GEN1_OP_INSERT;
  op.box = roomy;
  op.slot = -1;
  op.mon = &e;
  static Gen1WriteScratch scratch;
  Gen1WStatus ws = gen1_write_apply(img, len, &s, &op, &scratch);
  CHECK(ws == GEN1W_OK, "%s: gen1_write_apply INSERT (%s)", file, gen1_write_status_text(ws));
  if (ws != GEN1W_OK) return;

  /* Re-open the MUTATED image fresh (mirroring the Gen-2 path's reload+gb_verify_slot,
   * rather than trusting gen1_write_apply's own internal verify alone) and compare
   * the landed slot's record/OT/nickname bytes against what was asked for. */
  Gen1Save s2;
  Gen1Status gs2 = gen1_open(img, len, &s2);
  CHECK(gs2 == GEN1_OK, "%s: re-open the mutated image (%s)", file, gen1_status_text(gs2));
  if (gs2 == GEN1_OK) {
    uint32_t off = gen1_list_offset(&s2, roomy);
    GbEditMon back;
    CHECK(gb_load(&back, GB_GEN1, img + off, roomy, op.slot),
          "%s: gb_load the new slot", file);
    CHECK(gb_verify_slot(&back, img + off, roomy, op.slot),
          "%s: gb_verify_slot on the new slot", file);
    CHECK(memcmp(back.rec, e.rec, GEN1_BOX_REC_BYTES) == 0,
          "%s: landed record bytes match what was inserted", file);
    CHECK(memcmp(back.otname, e.ot, GEN1_NAME_BYTES) == 0,
          "%s: landed OT name matches what was inserted", file);
    CHECK(memcmp(back.nick, e.nick, GEN1_NAME_BYTES) == 0,
          "%s: landed nickname matches what was inserted", file);
  }
  printf("  %s: landed in box %d slot %d\n", file, roomy, op.slot);
  landed++;
}

/* ============================================================================ */
/* 5. BACKLOG #104 R1: MAKE LEGAL's evolution-level correction + written_level. */
/* ============================================================================ */

static void test_make_legal(void) {
  printf("== 5. BACKLOG #104 R1: MAKE LEGAL level correction + written_level ==\n");
  if (!pk_evo_have_data()) { printf("  SKIP (no evolutions table linked)\n"); return; }

  /* Charizard (species 6): the chain (Charmander -> L16 Charmeleon -> L36
   * Charizard) makes pk_evo_min_level(6) == 36 -- a real, if synthetic, "an
   * underlevelled evolved pokemon" (Guy's own example). */
  uint8_t rec[80];
  gen3_build_mon(6, 20, 0x33334444u, 0xBBBB0003u, "MLTEST", 3, rec);

  GbEditMon out; Gen3ToGbLoss loss;
  G3GbStatus st = gen3_to_gb(rec, GB_GEN2, NULL, &out, &loss);
  CHECK(st == G3GB_OK, "Charizard L20 converts (%s)", g3gb_status_text(st));
  if (st != G3GB_OK) return;

  uint8_t from_lvl = 0, to_lvl = 0;
  bool need_fix = gen3_to_gb_evo_needs_fix(&out, &from_lvl, &to_lvl);
  CHECK(need_fix, "an underlevelled Charizard needs the MAKE LEGAL fix");
  CHECK(from_lvl == 20, "from_level reported as 20 (got %u)", from_lvl);
  CHECK(to_lvl == 36, "to_level reported as 36 (got %u)", to_lvl);

  /* KEEP AS IS: no correction applied -- written_level is just the original's own
   * level, same as every conversion section 2 already measured. */
  {
    GbEditMon keep = out;
    GbscEntry e; gbsc_entry_from(&e, &keep, rec, 0);
    CHECK(e.written_level == 20,
          "KEEP AS IS: written_level == the original level (got %u)", e.written_level);

    uint8_t back80[80]; GbscMergeReport rep;
    CHECK(gbsc_merge_up(&e, &keep, back80, &rep), "KEEP AS IS: merge up (no GB edit) succeeds");
    CHECK(!rep.level_changed, "KEEP AS IS: no GB-side edit -> level_changed is false");
    CHECK(memcmp(back80, rec, 80) == 0, "KEEP AS IS: byte-identical to the original 80 bytes");
  }

  /* MAKE LEGAL: gb_paste_hook's own sequence -- probe (already done above), then
   * apply exactly the level gen3_to_gb_evo_needs_fix() reported, via gb_set_level
   * (already shipped, gb_edit.h), same as the UI slice does. */
  GbEditMon fixed = out;
  CHECK(gb_set_level(&fixed, to_lvl), "MAKE LEGAL: gb_set_level raises the level");
  CHECK(gb_get_level(&fixed) == 36, "MAKE LEGAL: the GB record now shows level 36");

  GbscEntry e2; gbsc_entry_from(&e2, &fixed, rec, 0);   /* rec: the TRUE original,
                                                          * level 20, untouched --
                                                          * design doc section 3c's
                                                          * own rule. */
  CHECK(e2.written_level == 36,
        "MAKE LEGAL: written_level records the level ACTUALLY WRITTEN (got %u)", e2.written_level);
  CHECK(memcmp(e2.original80, rec, 80) == 0,
        "MAKE LEGAL: the sidecar's original80 is the TRUE, uncorrected original");

  /* (f1) merge-up with NO further Game-Boy-side change: must restore the ORIGINAL
   * 80 bytes EXACTLY -- the whole point of this fix. Before it, comparing against
   * the original's own level (20) instead of written_level (36) would have read
   * the correction as a genuine level-up and folded level 36 into the merged
   * record instead of restoring level 20. */
  {
    uint8_t back80[80]; GbscMergeReport rep;
    CHECK(gbsc_merge_up(&e2, &fixed, back80, &rep), "MAKE LEGAL, no further edit: merge up succeeds");
    CHECK(!rep.level_changed, "MAKE LEGAL, no further edit: level_changed is false (this is the fix)");
    CHECK(memcmp(back80, rec, 80) == 0,
          "MAKE LEGAL, no further edit: restores the ORIGINAL 80 bytes exactly");
  }

  /* (f2) a REAL level-up abroad after MAKE LEGAL (36 -> 40): must still be
   * reported and merged, exactly like today's ordinary level_changed path --
   * MAKE LEGAL must not make genuine in-game progress invisible. */
  {
    GbEditMon leveled = fixed;
    CHECK(gb_set_level(&leveled, 40), "level up further, abroad, to 40");
    uint8_t back80[80]; GbscMergeReport rep;
    CHECK(gbsc_merge_up(&e2, &leveled, back80, &rep), "MAKE LEGAL + a real level-up: merge up succeeds");
    CHECK(rep.level_changed, "MAKE LEGAL + a real level-up: level_changed is true");
    PkMon merged; CHECK(pk_decode_mon(back80, false, &merged), "merged record decodes");
    pk_resolve(&merged);
    CHECK(merged.level == 40, "the merged level reflects the real level-up (got %u)", merged.level);
  }

  /* (f3) a PRE-R1 entry (written_level == 0, the sentinel: every entry gbsc_add()
   * wrote before this field existed has that byte at its old pad value) falls
   * back to comparing against the ORIGINAL's own decoded level -- today's exact,
   * unchanged behaviour, proven directly rather than assumed. A caller with an old
   * sidecar entry and a MAKE-LEGAL-shaped write gets the pre-R1 behaviour (level
   * folded in as a "level-up"), not a silent original-bytes restore it never had
   * the data to promise. */
  {
    GbscEntry old = e2;
    old.written_level = 0;                 /* simulate a file written before R1 */
    uint8_t back80[80]; GbscMergeReport rep;
    CHECK(gbsc_merge_up(&old, &fixed, back80, &rep),
          "pre-R1 entry (written_level==0): merge up succeeds");
    CHECK(rep.level_changed,
          "pre-R1 entry: falls back to the original's own level (unchanged pre-R1 behaviour)");
    PkMon merged; CHECK(pk_decode_mon(back80, false, &merged), "pre-R1 entry: merged record decodes");
    pk_resolve(&merged);
    CHECK(merged.level == 36, "pre-R1 entry: the merged level reflects the GB value (got %u)", merged.level);

    /* Same sentinel entry, but with NO level difference at all (both at the
     * ORIGINAL's own level) -- the pre-R1 "untouched" case this field must never
     * break. */
    GbEditMon untouched = out;   /* level 20, same as `rec`'s own level -- never corrected */
    GbscEntry old2; gbsc_entry_from(&old2, &untouched, rec, 0);
    old2.written_level = 0;
    uint8_t back80b[80]; GbscMergeReport rep2;
    CHECK(gbsc_merge_up(&old2, &untouched, back80b, &rep2),
          "pre-R1 entry, untouched: merge up succeeds");
    CHECK(!rep2.level_changed, "pre-R1 entry, untouched: level_changed is false");
    CHECK(memcmp(back80b, rec, 80) == 0,
          "pre-R1 entry, untouched: still byte-identical (unchanged pre-R1 behaviour)");
  }

  /* (f4) a species with NO level-gated evolution constraint must never be offered
   * a fix -- "no constraint" must never manufacture a correction. Eevee (species
   * 133) evolves only by stone/friendship (no PK_EVO_LEVEL link), so
   * pk_evo_min_level() reports 1 regardless of how low its own level is. */
  {
    uint8_t rec2[80];
    gen3_build_mon(133, 5, 0x55556666u, 0xCCCC0004u, "NOFIX", 3, rec2);
    GbEditMon out2; Gen3ToGbLoss loss2;
    G3GbStatus st2 = gen3_to_gb(rec2, GB_GEN2, NULL, &out2, &loss2);
    CHECK(st2 == G3GB_OK, "Eevee L5 converts (%s)", g3gb_status_text(st2));
    if (st2 == G3GB_OK) {
      uint8_t fl = 0, tl = 0;
      CHECK(!gen3_to_gb_evo_needs_fix(&out2, &fl, &tl),
            "Eevee (no level-gated evolution) is never offered a fix");
    }
  }
}

/* ============================================================================ */

int main(int argc, char** argv) {
  test_format();
  test_codec_negatives();
  test_lossy_name();
  test_stat_exp_and_ivs();
  test_rename_refused();

  printf("== 2. the Gen-3 corpus, both target generations ==\n");
  for (int i = 1; i < argc; i++) run_corpus_file(argv[i]);
  if (argc <= 1) printf("  (no Gen-3 saves given on argv -- section 2/3 will be empty)\n");

  for (int g = GB_GEN1; g <= GB_GEN2; g++) {
    printf("  gen %d: tested=%d accepted=%d roundtrip-exact=%d\n",
           g, g_tested[g], g_accepted[g], g_roundtrip[g]);
    printf("    refused: ARG=%d EGG=%d SPECIES=%d MOVE=%d NEEDS_BASE=%d GLITCH=%d\n",
           g_refused[g][G3GB_ERR_ARG], g_refused[g][G3GB_ERR_EGG],
           g_refused[g][G3GB_ERR_SPECIES], g_refused[g][G3GB_ERR_MOVE],
           g_refused[g][G3GB_ERR_NEEDS_BASE], g_refused[g][G3GB_ERR_GLITCH]);
    /* Both are load-bearing, not vacuous: GLITCH used to be 113/68 (every mon with
     * fewer than 4 moves, wrongly refused by a gb_set_ppup-on-an-empty-slot bug,
     * fixed in source/gen3_to_gb.c). With that fixed, this corpus measures 314
     * accepted for Gen 2 and 141 for Gen 1 -- the floors below are a comfortable
     * margin under that so a future data_tables.c regen does not flake the suite. */
    CHECK(g_refused[g][G3GB_ERR_GLITCH] == 0,
          "gen %d: zero GLITCH refusals (the PP-Ups bug is fixed)", g);
    int floor = (g == GB_GEN2) ? 300 : 130;
    CHECK(g_accepted[g] >= floor,
          "gen %d: accepted >= %d (measured 314 Gen2 / 141 Gen1 on this corpus)", g, floor);
    CHECK(g_accepted[g] == g_roundtrip[g], "gen %d: every accepted conversion round-tripped", g);
  }

  test_gb_side_changes();
  test_make_legal();

  printf("== 4. engine acceptance ==\n");
  test_engine_gen2("Gold.sav");
  test_engine_gen2("Crystal.sav");
  test_engine_gen1("Red.sav");
  printf("  %d record(s) landed in a real save\n", landed);

  printf("\n%s: %d check(s), %d failure(s)\n", g_fail ? "FAIL" : "OK", g_check, g_fail);
  return g_fail ? 1 : 0;
}
