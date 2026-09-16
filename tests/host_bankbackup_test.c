/* Host test for savefile.c's sf_save_rolling() AND sf_save_rolling_ok(), the two
 * functions box_save() (pdna_bank.c) calls for its backup + write + verdict (BACKLOG
 * #150 S150-0 review F3; BACKLOG #158). pdna_bank.c itself is not host-linkable (it
 * includes <tonc.h> and "ui.h"), but both savefile.c primitives have no such
 * dependency, so this links and calls the REAL functions over the REAL lib/fatfs on a
 * RAM disk (tests/hostfat).
 *
 * BACKLOG #158: earlier revisions of this test re-typed box_save's decision table by
 * hand (a local `save_ok()`), which passed even with the whole fix reverted (F3's
 * finding: zero mutations bitten) -- and, separately, drifted from box_save's own copy
 * once (the exact failure mode a re-typed copy invites). The verdict (SF_OK is a yes;
 * SF_ERR_RENAME is a yes only at SF_WHERE_TARGET; everything else is a no) now lives in
 * ONE place, sf_save_rolling_ok (savefile.c), and box_save calls that same function --
 * so this file calls it too, never a re-typed copy.
 *
 *   cc -std=c11 -DFF_USE_MKFS=1 -Dsiprintf=sprintf -Dsniprintf=snprintf -Dvsniprintf=vsnprintf -I tests/hostfat -I lib/fatfs -I source \
 *      tests/host_bankbackup_test.c source/savefile.c source/log.c lib/fatfs/ff.c \
 *      lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c -o /tmp/hbb
 *   /tmp/hbb
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "savefile.h"
#include "ramdisk.h"

#define DIR_ "/PokeDNA"
#define BOX  DIR_ "/box00.box"
#define TMP  BOX ".tmp"
#define BAK  BOX ".bak"
#define BAKTMP BOX ".baktmp"
#define BOX_BYTES 256u   /* size is irrelevant to this test; a real box is 2400 B */

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static FATFS s_fs;
static BYTE  s_work[FF_MAX_SS * 2];
static unsigned char s_old[BOX_BYTES], s_new[BOX_BYTES], s_rd[BOX_BYTES];

static void fill(unsigned char* p, unsigned n, unsigned seed) {
  unsigned i;
  for (i = 0; i < n; i++) p[i] = (unsigned char)((i * 31u + seed * 7u + (i >> 5)) & 0xFF);
}

static void fresh_card(unsigned sectors) {
  MKFS_PARM opt = { FM_FAT | FM_SFD, 1, 1, 0, 0 };
  f_mount(0, "", 0);
  rd_init(sectors);
  CHECK(f_mkfs("", &opt, s_work, sizeof s_work) == FR_OK, "f_mkfs failed");
  CHECK(f_mount(&s_fs, "", 1) == FR_OK, "f_mount failed");
  CHECK(f_mkdir(DIR_) == FR_OK, "f_mkdir " DIR_ " failed");
}

static void remount(void) {
  f_mount(0, "", 0);
  CHECK(f_mount(&s_fs, "", 1) == FR_OK, "remount failed");
}

static int write_raw(const char* path, const unsigned char* buf, unsigned n) {
  FIL f; UINT bw = 0;
  if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return 0;
  if (f_write(&f, buf, n, &bw) != FR_OK || bw != n) { f_close(&f); return 0; }
  return f_close(&f) == FR_OK;
}

static long slurp(const char* path, unsigned char* out, unsigned cap) {
  FIL f; UINT br = 0;
  if (f_open(&f, path, FA_READ) != FR_OK) return -1;
  if (f_read(&f, out, cap, &br) != FR_OK) { f_close(&f); return -1; }
  f_close(&f);
  return (long)br;
}

static int holds(const char* path, const unsigned char* want, unsigned n) {
  long got = slurp(path, s_rd, BOX_BYTES);
  return got == (long)n && memcmp(s_rd, want, n) == 0;
}

static int exists(const char* path) { FILINFO fi; return f_stat(path, &fi) == FR_OK; }

/* ---- tests ---- */

/* Absent source (a box file exists only after its first save): the backup step must be
 * a no-op, and the FIRST write into a never-used box must still succeed. */
static void t_virgin_box_first_write(void) {
  bool backed_up = true;   /* deliberately wrong default -- sf_save_rolling must clear it */
  fresh_card(4096);
  fill(s_new, BOX_BYTES, 1);
  CHECK(!exists(BOX), "virgin: setup left a box file behind");
  CHECK(sf_save_rolling(BOX, s_new, BOX_BYTES, &backed_up) == SF_OK,
        "virgin: first write into a never-used box refused");
  remount();
  CHECK(holds(BOX, s_new, BOX_BYTES), "virgin: the card does not hold the new box");
  CHECK(!exists(BAK), "virgin: a .bak appeared for a box that never existed");
  CHECK(!backed_up,
        "virgin: sf_save_rolling reported a backup for a box that never existed (review G2)");
}

/* Present source: backup is made, box.bak holds the OLD bytes, box holds the NEW ones. */
static void t_present_box_backs_up(void) {
  bool backed_up = false;
  fresh_card(4096);
  fill(s_old, BOX_BYTES, 2);
  fill(s_new, BOX_BYTES, 3);
  CHECK(write_raw(BOX, s_old, BOX_BYTES), "present: could not create the original box");
  CHECK(sf_save_rolling(BOX, s_new, BOX_BYTES, &backed_up) == SF_OK,
        "present: save refused on a healthy card");
  remount();
  CHECK(holds(BOX, s_new, BOX_BYTES), "present: the card does not hold the new box");
  CHECK(holds(BAK, s_old, BOX_BYTES), "present: box.bak does not hold the pre-save bytes");
  CHECK(backed_up, "present: sf_save_rolling did not report the backup it made (review G2)");

  /* review F4: sf_save_rolling_ok's contract (savefile.h) is *out_where left UNTOUCHED
   * unless sf_save_rolling actually returned SF_ERR_RENAME -- box_save's own sentinel
   * trick (pdna_bank.c, `SfWhere w = (SfWhere)-1;` before the call) depends on that
   * holding on a plain SF_OK. A mutation that writes *out_where on every call (even
   * SF_OK) would still pass every other check in this file (the return value is right,
   * only the untouched-ness is wrong) and survived until this was added. */
  SfWhere w = (SfWhere)-1;
  CHECK(sf_save_rolling_ok(BOX, s_new, BOX_BYTES, &w) && w == (SfWhere)-1,
        "present: out_where was written on a plain SF_OK success");
}

/* Backup fails on a present box: box_save must refuse and the ORIGINAL box must be
 * untouched -- never a half-write on a failed backup. rd_fail_all_writes would be
 * VACUOUS here (review F2): with the backup and the main write both inside
 * sf_save_rolling now, a permanently-dead card fails BOTH of them, so the test cannot
 * tell "the backup gate refused" from "the write itself just couldn't happen either
 * way" -- it would still pass with the gate deleted. rd_fail_at fails exactly ONE
 * write (landing inside copy_file's write into box.baktmp, the very first write this
 * call makes) and then HEALS, so the subsequent main write would succeed if nothing
 * stopped it -- the only thing that CAN stop it is sf_save_rolling noticing the backup
 * failed and returning before ever opening box00.box.tmp. That makes the mutation
 * bite: delete the "backup failed -> return" check and this test starts seeing the
 * NEW bytes in box00.box instead of the old ones. */
static void t_backup_failure_refuses(void) {
  fresh_card(4096);
  fill(s_old, BOX_BYTES, 4);
  fill(s_new, BOX_BYTES, 5);
  CHECK(write_raw(BOX, s_old, BOX_BYTES), "backupfail: could not create the original box");
  rd_fail_at = 0;                         /* fail exactly the first write, then heal */
  bool ok = sf_save_rolling_ok(BOX, s_new, BOX_BYTES, NULL);
  rd_fail_at = -1;
  CHECK(!ok, "backupfail: box_save reported success while the backup failed");
  remount();
  CHECK(holds(BOX, s_old, BOX_BYTES), "backupfail: the original box was touched despite the refusal");
  CHECK(!holds(BOX, s_new, BOX_BYTES),
        "backupfail: the NEW bytes landed in box00.box -- the backup-failed gate did not stop the write");
  CHECK(!exists(BAK), "backupfail: a .bak appeared despite the backup failing");
  CHECK(!exists(BAKTMP),
        "backupfail: a stray .baktmp was left behind -- sf_backup_rolling's own cleanup did not run");
}

/* THE review F1 scenario, reproduced directly: f_stat itself fails transiently (not
 * absent -- a card that would not even answer) on a PRESENT box. Before F1's fix this
 * read as "never written", skipped the backup, and overwrote the box unbacked; a fresh
 * remount (drops FatFs' cached directory window, as a reboot would) plus one injected
 * read fault at the very next read reliably lands inside f_stat's own directory lookup
 * (probed by hand: rd_fail_read_at = 0 right after a remount makes f_stat report
 * FR_DISK_ERR on a file that unquestionably exists). The card heals immediately after,
 * so nothing here is "unluckily still broken" -- this is exactly the one-bad-read,
 * then-fine card the guard exists for. */
static void t_stat_fault_refuses_unbacked_write(void) {
  fresh_card(4096);
  fill(s_old, BOX_BYTES, 8);
  fill(s_new, BOX_BYTES, 9);
  CHECK(write_raw(BOX, s_old, BOX_BYTES), "statfault: could not create the original box");
  remount();
  rd_fail_read_at = 0;                    /* lands inside f_stat's own lookup; heals after */
  bool ok = sf_save_rolling_ok(BOX, s_new, BOX_BYTES, NULL);
  rd_fail_read_at = -1;
  CHECK(!ok, "statfault: box_save reported success on a card that would not even answer f_stat");
  remount();
  CHECK(holds(BOX, s_old, BOX_BYTES),
        "statfault: the original box was overwritten despite the refusal");
  CHECK(!holds(BOX, s_new, BOX_BYTES),
        "statfault: the NEW bytes landed in box00.box UNBACKED -- the f_stat fault was "
        "mistaken for \"never written\" (review F1's exact bug)");
}

/* A forced rename failure whose bytes actually landed at TARGET must read as success --
 * the box_save triage, not a raw SF_ERR_RENAME refusal. Sweep a single transient read
 * error (the read-side twin of the write sweep in host_savefat_test.c) across the write
 * that follows a healthy backup, and require the sweep to actually reach that exact
 * interleaving at least once. */
static void t_rename_target_counts_as_success(void) {
  long k;
  int saw_rename_target = 0;
  fill(s_old, BOX_BYTES, 6);
  fill(s_new, BOX_BYTES, 7);
  for (k = 0; k < 40; k++) {
    fresh_card(4096);
    CHECK(write_raw(BOX, s_old, BOX_BYTES), "renametarget k=%ld: setup", k);
    remount();
    rd_fail_read_at = k;               /* one transient read error, then a healthy card */
    SfStatus st = sf_write_verified(BOX, s_new, BOX_BYTES);
    rd_fail_read_at = -1;
    remount();
    if (st == SF_ERR_RENAME) {
      SfWhere w = sf_where_are_the_bytes(BOX, s_new, BOX_BYTES);
      if (w == SF_WHERE_TARGET) {
        saw_rename_target = 1;
        /* box_save's own triage, applied to this exact outcome: TARGET => success. */
        CHECK(holds(BOX, s_new, BOX_BYTES),
              "renametarget k=%ld: SF_WHERE_TARGET but the card disagrees", k);
      }
    }
  }
  CHECK(saw_rename_target,
        "renametarget: the sweep never reached SF_ERR_RENAME + SF_WHERE_TARGET -- "
        "the triage's main branch is untested");
}

/* BACKLOG #158: sf_save_rolling_ok's verdict, checked against independent ground truth
 * (what the card actually holds after a remount, not a re-derivation of the same
 * where-logic), swept across every read-fault position a confirmed rename can land at
 * during a backup-then-write cycle. This is the test that must go red on:
 *   - inverting the SF_WHERE_TARGET check (a sweep position that landed the swap would
 *     flip from a true verdict to a false refusal on a save that DID land);
 *   - the function always returning true (a sweep position where the swap did NOT
 *     land -- old box intact, edit lost -- would flip from a false refusal to a false
 *     claim of success, which is exactly what used to let box_save clear g_dirty on a
 *     save that never happened);
 *   - returning true specifically on SF_WHERE_TMP_ONLY (subsumed by the point above:
 *     TMP_ONLY is one of the not-landed states this sweep is required to reach). */
static void t_verdict_matches_the_card(void) {
  long k;
  int saw_target_true = 0, saw_false_at_all = 0;
  fill(s_old, BOX_BYTES, 12);
  fill(s_new, BOX_BYTES, 13);
  for (k = 0; k < 100; k++) {
    fresh_card(4096);
    CHECK(write_raw(BOX, s_old, BOX_BYTES), "verdict k=%ld: setup", k);
    remount();
    rd_fail_read_at = k;                 /* one transient read error, then a healthy card */
    SfWhere w = (SfWhere)-1;
    bool ok = sf_save_rolling_ok(BOX, s_new, BOX_BYTES, &w);
    rd_fail_read_at = -1;
    remount();
    int card_has_new = holds(BOX, s_new, BOX_BYTES);
    CHECK(ok == (card_has_new != 0),
          "verdict k=%ld: sf_save_rolling_ok said %s but the card %s hold the new box",
          k, ok ? "ok" : "refused", card_has_new ? "DOES" : "does NOT");
    if (ok && w == SF_WHERE_TARGET) saw_target_true = 1;
    if (!ok) saw_false_at_all = 1;
  }
  CHECK(saw_target_true,
        "verdict: the sweep never reached a true SF_WHERE_TARGET verdict -- the pass "
        "branch is untested");
  CHECK(saw_false_at_all,
        "verdict: the sweep never reached a false verdict -- the refusal branches are "
        "untested");
}

/* ---- BACKLOG #163: structural check -- both dirty-flush callers in pdna_bank.c
 * (banksrc_records and pdna_bank_flush_deletions) must CONSUME the flush's verdict at
 * their page-out site, not fire it and move on. pdna_bank.c is not host-linkable (it
 * includes <tonc.h> and "ui.h"), so this is a text-level check over its own source,
 * comment-stripped so a call sitting only in a comment cannot satisfy (or hide from)
 * it -- the same "comment-strip + text search" discipline
 * tests/host_escape_gate_sites_test.py documents and uses on pdna_box.c. A
 * self-mutation (the exact PRE-FIX shape this ticket exists for, `if (g_dirty)
 * box_save();` with the return silently discarded) proves the check has teeth. ---- */

/* Comment-stripped copy of `text` into `out` (blanks block and line comment spans,
 * keeps every other byte and all newlines, so a later strstr never matches inside dead
 * prose). */
static void strip_c_comments(const char* text, size_t n, char* out, size_t outcap) {
  size_t bi = 0, i = 0;
  bool in_block = false, in_line = false;
  while (i < n && bi + 1 < outcap) {
    if (in_block) {
      if (text[i] == '*' && i + 1 < n && text[i + 1] == '/') { in_block = false; i += 2; out[bi++] = ' '; continue; }
      out[bi++] = (text[i] == '\n') ? '\n' : ' ';
      i++;
      continue;
    }
    if (in_line) {
      if (text[i] == '\n') { in_line = false; out[bi++] = '\n'; i++; continue; }
      i++;
      continue;
    }
    if (text[i] == '/' && i + 1 < n && text[i + 1] == '*') { in_block = true; i += 2; out[bi++] = ' '; continue; }
    if (text[i] == '/' && i + 1 < n && text[i + 1] == '/') { in_line = true; i += 2; continue; }
    out[bi++] = text[i++];
  }
  out[bi] = 0;
}

/* Slice [sig, next_sig) out of `stripped` -- the same "next signature marks this one's
 * end" idiom host_xfergate_test.c's own mutation demo uses on gb_session.c. */
static const char* slice_function(const char* stripped, const char* sig, const char* next_sig,
                                   char* out, size_t outcap) {
  const char* fn = strstr(stripped, sig);
  if (!fn) return NULL;
  const char* end = next_sig ? strstr(fn, next_sig) : fn + strlen(fn);
  if (!end) return NULL;
  size_t len = (size_t)(end - fn);
  if (len >= outcap) len = outcap - 1;
  memcpy(out, fn, len);
  out[len] = 0;
  return out;
}

/* The real fix's shape: box_save_or_keep_dirty()'s return is consumed (never a bare
 * `box_save();` statement whose result is thrown away) AND the exact pre-fix bug shape
 * -- `if (g_dirty) box_save();`, BACKLOG #163's whole reason for existing -- is gone. */
static bool flush_site_checked(const char* body) {
  return strstr(body, "!box_save_or_keep_dirty()") != NULL &&
         strstr(body, "if (g_dirty) box_save();") == NULL;
}

static void t_flush_callers_check_the_return(void) {
  FILE* f = fopen("source/pdna_bank.c", "rb");
  CHECK(f != NULL, "structural: could not open source/pdna_bank.c (run from the repo root)");
  if (!f) return;
  char raw[131072];
  size_t n = fread(raw, 1, sizeof raw - 1, f);
  fclose(f);
  raw[n] = 0;
  static char stripped[131072];
  strip_c_comments(raw, n, stripped, sizeof stripped);

  char records_body[4096], flush_body[4096];
  CHECK(slice_function(stripped, "static uint8_t* banksrc_records(int box) {",
                        "static void banksrc_get_name(int box, char out[12]) {",
                        records_body, sizeof records_body) != NULL,
        "structural: banksrc_records( not found in source/pdna_bank.c");
  CHECK(slice_function(stripped, "int pdna_bank_flush_deletions(void) {", "static bool has_pk_ext(const char* n) {",
                        flush_body, sizeof flush_body) != NULL,
        "structural: pdna_bank_flush_deletions( not found in source/pdna_bank.c");

  CHECK(flush_site_checked(records_body),
        "structural: banksrc_records() does not consume box_save_or_keep_dirty()'s "
        "return before paging out (BACKLOG #163's own refusal-goes-silent bug)");
  CHECK(flush_site_checked(flush_body),
        "structural: pdna_bank_flush_deletions() does not consume "
        "box_save_or_keep_dirty()'s return before paging out (BACKLOG #163's own "
        "refusal-goes-silent bug)");

  /* self-mutation: revert banksrc_records's real, checked site back to the exact
   * pre-fix bug shape and prove flush_site_checked() catches it -- every run, not just
   * when someone remembers to demonstrate it by hand. */
  {
    const char* real_line = "if (g_dirty && !box_save_or_keep_dirty()) return box_recs();";
    CHECK(strstr(records_body, real_line) != NULL,
          "MUT (BACKLOG #163): the real banksrc_records() line has drifted -- update this "
          "test's expected shape before trusting the mutation demonstration");
    char mutated[4096];
    const char* hit = strstr(records_body, real_line);
    if (hit) {
      size_t pre = (size_t)(hit - records_body);
      snprintf(mutated, sizeof mutated, "%.*s%s%s", (int)pre, records_body,
               "if (g_dirty) box_save();", hit + strlen(real_line));
      CHECK(!flush_site_checked(mutated),
            "MUT (BACKLOG #163): reverting banksrc_records to the pre-fix "
            "`if (g_dirty) box_save();` shape should have been caught but was not");
      printf("  MUT (BACKLOG #163) demonstration -- banksrc_records reverted to the "
             "pre-fix unchecked flush: correctly caught\n");
    }
  }
}

/* ---- BACKLOG #163 review F1: g_box_unsaved_box outlives g_dirty. The marker's whole
 * meaning is "the box g_dirty is currently about" -- SWITCH_BOX (pdna_box.c) refuses to
 * page away from it while banksrc_records's own page-out check runs on g_dirty, so if
 * the two ever come apart (marker set, g_dirty already false) the NEXT box_load of that
 * index shows BOX NOT SAVED on a box nobody has touched, and every L/R from there loads
 * a DIFFERENT box's file into g_bankbuf under the OLD box's still-displayed frame. Text
 * checks (comment-stripped, same discipline as t_flush_callers_check_the_return above)
 * pin the four sites review F1 named: box_load's page-in clear, box_save's
 * native-invariant refusal, box_save's main verdict, and pdna_bank_show's entry AND
 * exit (the exit is the box's last chance -- the screen has already returned). ---- */

static bool box_load_clears_marker(const char* body) {
  return strstr(body, "g_box_unsaved_box = -1;") != NULL;
}

static int count_occurrences(const char* haystack, const char* needle) {
  int n = 0;
  const char* p = haystack;
  size_t nl = strlen(needle);
  while ((p = strstr(p, needle)) != NULL) { n++; p += nl; }
  return n;
}

static void t_marker_never_outlives_g_dirty(void) {
  FILE* f = fopen("source/pdna_bank.c", "rb");
  CHECK(f != NULL, "invariant: could not open source/pdna_bank.c (run from the repo root)");
  if (!f) return;
  char raw[131072];
  size_t n = fread(raw, 1, sizeof raw - 1, f);
  fclose(f);
  raw[n] = 0;
  static char stripped[131072];
  strip_c_comments(raw, n, stripped, sizeof stripped);

  char load_body[4096], save_body[4096], show_body[8192];
  CHECK(slice_function(stripped, "static bool box_load(int box) {",
                        "static bool __attribute__((noinline)) native_invariant_ok(void) {",
                        load_body, sizeof load_body) != NULL,
        "invariant: box_load( not found in source/pdna_bank.c");
  CHECK(slice_function(stripped, "static bool box_save(void) {",
                        "bool pdna_bank_defer_full(void)",
                        save_body, sizeof save_body) != NULL,
        "invariant: box_save( not found in source/pdna_bank.c");
  CHECK(slice_function(stripped, "int pdna_bank_show(void) {", NULL,
                        show_body, sizeof show_body) != NULL,
        "invariant: pdna_bank_show( not found in source/pdna_bank.c");

  CHECK(box_load_clears_marker(load_body),
        "invariant: box_load() does not clear g_box_unsaved_box -- BACKLOG #163 review "
        "F1's split-brain bug (marker outlives g_dirty across a fresh page-in)");

  CHECK(strstr(save_body, "g_box_unsaved_box = g_loaded; g_dirty = true;") != NULL,
        "invariant: box_save()'s native-invariant refusal does not set g_dirty alongside "
        "the marker -- BACKLOG #163 review F1's marker/g_dirty split");
  CHECK(strstr(save_body, "g_box_unsaved_box = ok ? -1 : g_loaded;") != NULL &&
        strstr(save_body, "g_dirty = !ok;") != NULL,
        "invariant: box_save()'s main verdict does not set g_box_unsaved_box and g_dirty "
        "from the same `ok` -- BACKLOG #163 review F1's marker/g_dirty split");

  CHECK(strstr(show_body, "g_loaded = -1; g_dirty = false; g_box_unsaved_box = -1;") != NULL,
        "invariant: pdna_bank_show()'s entry does not clear g_box_unsaved_box alongside "
        "g_dirty -- a marker from a PRIOR session could survive onto an untouched box");
  /* the entry line above is ONE occurrence of each; the exit prompt's own clear (the
   * box's last chance -- box_save_or_keep_dirty()'s retry loop has already given up by
   * the time this runs) must be a SECOND. */
  CHECK(count_occurrences(show_body, "g_dirty = false;") >= 2 &&
        count_occurrences(show_body, "g_box_unsaved_box = -1;") >= 2,
        "invariant: pdna_bank_show()'s exit prompt does not clear g_box_unsaved_box "
        "alongside its own g_dirty = false -- a failed-then-abandoned save would leave "
        "the marker standing to wrongly flag whatever box this index next holds");

  /* self-mutation (review F1's own request: "a self-mutation that drops the :175
   * clear"): revert box_load's real clear back to the pre-fix shape and prove
   * box_load_clears_marker() -- the SAME function the real check above calls -- catches
   * it, every run. */
  {
    const char* needle = "g_box_unsaved_box = -1;";
    const char* hit = strstr(load_body, needle);
    CHECK(hit != NULL, "MUT (review F1): the real box_load() clear has drifted -- update "
                        "this test's expected shape before trusting the mutation");
    if (hit) {
      char mutated[4096];
      size_t pre = (size_t)(hit - load_body);
      snprintf(mutated, sizeof mutated, "%.*s%s", (int)pre, load_body, hit + strlen(needle));
      CHECK(!box_load_clears_marker(mutated),
            "MUT (review F1): dropping box_load's g_box_unsaved_box clear should have "
            "been caught but was not");
      printf("  MUT (review F1) demonstration -- box_load's g_box_unsaved_box clear "
             "removed: correctly caught\n");
    }
  }
}

int main(void) {
  t_virgin_box_first_write();
  t_present_box_backs_up();
  t_backup_failure_refuses();
  t_stat_fault_refuses_unbacked_write();
  t_rename_target_counts_as_success();
  t_verdict_matches_the_card();
  t_flush_callers_check_the_return();
  t_marker_never_outlives_g_dirty();
  printf("\n%s: %d failure(s)\n", fails ? "FAIL" : "OK", fails);
  return fails ? 1 : 0;
}
