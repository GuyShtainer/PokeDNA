/* source/savefile.c -- the never-corrupt-user-data layer -- over the REAL lib/fatfs, on a
 * RAM disk, with a flashcart's failures.
 *
 *   cc -std=c11 -DFF_USE_MKFS=1 -Dsiprintf=sprintf -I tests/hostfat -I lib/fatfs -I source \
 *      tests/host_savefat_test.c source/savefile.c source/log.c lib/fatfs/ff.c \
 *      lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c -o /tmp/hsf
 *
 * WHY. sf_write_verified's byte-compare re-read genuinely defeats a card that ACKs writes
 * it does not keep -- for the .tmp. Its LAST step did not: f_unlink(path) then
 * f_rename(tmp, path) were never read back, so FR_OK from f_rename was taken as proof the
 * save was in place. An EZ-Flash write has no retry and no read-back
 * (flashcartio_write_sector hands back _EZFO_writeSectors' verdict and nothing re-reads),
 * so FR_OK is not evidence that a byte landed -- the same lesson the log learned in
 * 94d9f0c.
 *
 * The test that matters is t_rename_hole_sweep: it starts the card lying at EVERY sector
 * of a real 128 KiB commit and holds the function to one invariant per position --
 *
 *     SF_OK  =>  the CARD (after a remount, i.e. what the user has next boot) holds the
 *                new bytes at the target name. No exceptions.
 *     error  =>  the user still holds recoverable data: the old save, the new save, or a
 *                complete .tmp.
 *
 * Before the read-back, positions existed where the .tmp was written AND verified, the
 * unlink of the original really took effect, the rename was swallowed, and the function
 * returned SF_OK -- so the app painted "SAVED / Edit written + verified" over a card with
 * no .sav on it at all. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "savefile.h"
#include "ramdisk.h"

#define DIR_  "/PokeDNA"
#define SAV   DIR_ "/emerald.sav"
#define TMP   SAV ".tmp"
#define BAK   SAV ".bak"
#define BAKTMP SAV ".baktmp"
#define SAVE_BYTES 131072u

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static FATFS s_fs;
static BYTE  s_work[FF_MAX_SS * 2];
static unsigned char s_old[SAVE_BYTES], s_new[SAVE_BYTES], s_rd[SAVE_BYTES];

static void fill(unsigned char* p, unsigned n, unsigned seed) {
  unsigned i;
  for (i = 0; i < n; i++) p[i] = (unsigned char)((i * 31u + seed * 7u + (i >> 9)) & 0xFF);
}

static void fresh_card(unsigned sectors) {
  MKFS_PARM opt = { FM_FAT | FM_SFD, 1, 1, 0, 0 };
  f_mount(0, "", 0);
  rd_init(sectors);
  CHECK(f_mkfs("", &opt, s_work, sizeof s_work) == FR_OK, "f_mkfs failed");
  CHECK(f_mount(&s_fs, "", 1) == FR_OK, "f_mount failed");
  CHECK(f_mkdir(DIR_) == FR_OK, "f_mkdir " DIR_ " failed");
}

/* Drop every FatFs cache and read the volume again -- "what the user has next boot",
 * which is the only question that matters after a card has been lying. */
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

/* -1 absent, else the byte count read (compared by the caller). */
static long slurp(const char* path, unsigned char* out, unsigned cap) {
  FIL f; UINT br = 0;
  if (f_open(&f, path, FA_READ) != FR_OK) return -1;
  if (f_read(&f, out, cap, &br) != FR_OK) { f_close(&f); return -1; }
  f_close(&f);
  return (long)br;
}

/* Does `path` hold exactly `want`(n)? */
static int holds(const char* path, const unsigned char* want, unsigned n) {
  long got = slurp(path, s_rd, SAVE_BYTES);
  return got == (long)n && memcmp(s_rd, want, n) == 0;
}

static int exists(const char* path) { FILINFO fi; return f_stat(path, &fi) == FR_OK; }

/* ---------------------------------------------------------------------------- */

/* The ordinary case still works, and leaves no scratch behind. */
static void t_happy_path(void) {
  fresh_card(4096);
  fill(s_old, SAVE_BYTES, 1);
  fill(s_new, SAVE_BYTES, 2);
  CHECK(write_raw(SAV, s_old, SAVE_BYTES), "happy: could not create the original");
  CHECK(sf_write_verified(SAV, s_new, SAVE_BYTES) == SF_OK, "happy: write failed");
  remount();
  CHECK(holds(SAV, s_new, SAVE_BYTES), "happy: the card does not hold the new save");
  CHECK(!exists(TMP), "happy: .tmp left behind on success");
}

/* Writing a save that does not exist yet (first export) still works. */
static void t_new_file(void) {
  fresh_card(4096);
  fill(s_new, SAVE_BYTES, 3);
  CHECK(sf_write_verified(SAV, s_new, SAVE_BYTES) == SF_OK, "new: write failed");
  remount();
  CHECK(holds(SAV, s_new, SAVE_BYTES), "new: the card does not hold it");
}

/* A card lying from the very first sector: the .tmp never verifies, so the ORIGINAL must
 * survive untouched and the status must not be SF_OK. (This half already worked -- it is
 * here so the sweep's baseline is not the only thing holding it.) */
static void t_lies_from_the_start(void) {
  SfStatus st;
  fresh_card(4096);
  fill(s_old, SAVE_BYTES, 4);
  fill(s_new, SAVE_BYTES, 5);
  CHECK(write_raw(SAV, s_old, SAVE_BYTES), "start: could not create the original");
  rd_lie_writes = 1;
  st = sf_write_verified(SAV, s_new, SAVE_BYTES);
  rd_lie_writes = 0;
  CHECK(st != SF_OK, "start: reported SF_OK on a card that kept nothing");
  remount();
  CHECK(holds(SAV, s_old, SAVE_BYTES), "start: the original save was damaged");
}

/* THE SWEEP. Start the card lying at sector k of the commit, for every k, and hold the
 * function to its contract at each position. */
static void t_rename_hole_sweep(void) {
  long k;
  long span;
  int saw_ok = 0, saw_late_failure = 0, saw_tmp_rescue = 0;
  int lied_ok = 0;      /* positions where the card lied and SF_OK was still returned */
  int saw_the_hole = 0; /* the exact interleaving: no .sav on the card, a complete .tmp */
  long hole_at = -1;

  /* How many sectors does an honest commit cost? Sweep a little past it. */
  fresh_card(4096);
  fill(s_old, SAVE_BYTES, 6);
  fill(s_new, SAVE_BYTES, 7);
  CHECK(write_raw(SAV, s_old, SAVE_BYTES), "sweep: setup");
  rd_writes = 0;
  CHECK(sf_write_verified(SAV, s_new, SAVE_BYTES) == SF_OK, "sweep: calibration write failed");
  span = (long)rd_writes + 8;
  CHECK(span > 260, "sweep: a 128 KiB commit only cost %ld sectors?", span);

  for (k = 0; k < span; k++) {
    SfStatus st;
    int path_new, path_old, tmp_new, lied;

    fresh_card(4096);
    CHECK(write_raw(SAV, s_old, SAVE_BYTES), "sweep k=%ld: setup", k);
    rd_lie_after = k;
    st = sf_write_verified(SAV, s_new, SAVE_BYTES);
    lied = (rd_lied != 0);
    rd_lie_after = -1; rd_lie_writes = 0;

    remount();
    path_new = holds(SAV, s_new, SAVE_BYTES);
    path_old = holds(SAV, s_old, SAVE_BYTES);
    tmp_new  = holds(TMP, s_new, SAVE_BYTES);

    /* (1) SF_OK is a promise about the CARD, not about a return code. */
    if (st == SF_OK) {
      saw_ok = 1;
      if (lied) lied_ok = 1;
      CHECK(path_new,
            "sweep k=%ld: reported SAVED but the card holds %s (tmp=%d)", k,
            path_old ? "the OLD save" : exists(SAV) ? "something else" : "NO SAVE AT ALL",
            tmp_new);
    } else {
      /* (2) A failure must never leave the user empty-handed: the old save, the new
       * save, or a complete .tmp has to be there. Nothing on the failure path deletes. */
      CHECK(path_old || path_new || tmp_new,
            "sweep k=%ld: %s and the user holds NOTHING recoverable", k, sf_status_str(st));
      if (lied) saw_late_failure = 1;
      if (st == SF_ERR_RENAME) {
        saw_tmp_rescue |= tmp_new;
        CHECK(tmp_new || path_new || path_old,
              "sweep k=%ld: SF_ERR_RENAME with no recoverable copy", k);
        /* THE hole, named exactly: the .tmp was written and verified, the unlink of the
         * original really took effect, and the rename was swallowed. Before the read-back
         * this position returned SF_OK. */
        if (!exists(SAV) && tmp_new) {
          if (!saw_the_hole) hole_at = k;
          saw_the_hole = 1;
          CHECK(st == SF_ERR_RENAME, "sweep k=%ld: the hole must report SF_ERR_RENAME", k);
        }
        /* The UI builds its message from this verdict, so a wrong verdict IS a lie told
         * to the user about their save. Check it against the card at every position. */
        {
          SfWhere w = sf_where_are_the_bytes(SAV, s_new, SAVE_BYTES);
          switch (w) {
            case SF_WHERE_TMP_ONLY:
              CHECK(tmp_new && !exists(SAV),
                    "sweep k=%ld: said TMP_ONLY but tmp=%d sav=%d", k, tmp_new, exists(SAV));
              break;
            case SF_WHERE_TMP_AND_OLD:
              CHECK(tmp_new && exists(SAV),
                    "sweep k=%ld: said TMP_AND_OLD with tmp=%d sav=%d", k, tmp_new,
                    exists(SAV));
              break;
            case SF_WHERE_TARGET:
              CHECK(path_new, "sweep k=%ld: said TARGET but the .sav does not match", k);
              break;
            case SF_WHERE_NEITHER:
              CHECK(!tmp_new && !path_new,
                    "sweep k=%ld: said NEITHER while a good copy existed (tmp=%d sav=%d)",
                    k, tmp_new, path_new);
              break;
          }
        }
      }
    }
  }

  CHECK(saw_ok, "sweep: no position ever succeeded -- the sweep is not exercising the write");
  CHECK(saw_late_failure, "sweep: no position ever caught the card lying late in the commit");
  /* The window this whole test exists for: the .tmp verified, the original really went
   * away, the rename did not land. It MUST be reachable (else the sweep proves nothing)
   * and it MUST be reported, with the bytes recoverable from the .tmp. */
  CHECK(saw_tmp_rescue,
        "sweep: the unlink-then-rename window never produced SF_ERR_RENAME + a good .tmp");
  CHECK(saw_the_hole,
        "sweep: the sweep never reached the state this fix exists for (no .sav on the "
        "card + a complete .tmp) -- the sweep proves nothing if it cannot get there");
  CHECK(!lied_ok,
        "sweep: SF_OK was returned at a position where the card swallowed writes");
  if (getenv("PDNA_TEST_VERBOSE"))
    printf("  sweep: %ld positions, the unlink/rename hole first opens at k=%ld\n",
           span, hole_at);
}

/* sf_backup_rolling has the identical unlink-then-rename shape, and a silently lost
 * rolling backup is worse than a loud one: the commit that follows it believes a backup
 * exists. Same sweep, same contract. */
static void t_rolling_backup_sweep(void) {
  long k, span;
  int saw_ok = 0, saw_fail = 0, lied_ok = 0;
  char bak[SF_PATH_MAX];

  fresh_card(4096);
  fill(s_old, SAVE_BYTES, 8);
  CHECK(write_raw(SAV, s_old, SAVE_BYTES), "roll: setup");
  rd_writes = 0;
  CHECK(sf_backup_rolling(SAV, bak, sizeof bak) == SF_OK, "roll: calibration failed");
  span = (long)rd_writes + 8;

  for (k = 0; k < span; k++) {
    SfStatus st;
    int lied;
    fresh_card(4096);
    CHECK(write_raw(SAV, s_old, SAVE_BYTES), "roll k=%ld: setup", k);
    rd_lie_after = k;
    bak[0] = 0;
    st = sf_backup_rolling(SAV, bak, sizeof bak);
    lied = (rd_lied != 0);
    rd_lie_after = -1; rd_lie_writes = 0;

    remount();
    if (st == SF_OK) {
      saw_ok = 1;
      if (lied) lied_ok = 1;
      CHECK(holds(BAK, s_old, SAVE_BYTES),
            "roll k=%ld: reported 'rolling backup OK' but %s", k,
            exists(BAK) ? "the .bak does not match the save" : "there is NO .bak on the card");
    } else {
      if (lied) saw_fail = 1;
    }
    /* Whatever happens to the backup, the SAVE itself is read-only here. */
    CHECK(holds(SAV, s_old, SAVE_BYTES), "roll k=%ld: the save was damaged by a BACKUP", k);
  }
  CHECK(saw_ok, "roll: no position succeeded");
  CHECK(saw_fail, "roll: no position ever caught the lying card");
  CHECK(!lied_ok, "roll: 'backup OK' was returned at a position where the card lied");
}

/* A rolling backup must never DELETE the verified copy it just made.
 *
 * This one needs an HONEST card that fails a single write and then recovers, not a liar: a
 * rolling backup replaces the user's previous .bak, so from the moment that .bak is
 * unlinked the verified .baktmp is the copy standing in for it -- and the rename-failure
 * branch used to f_unlink exactly that file. Under rd_lie_* the cleanup's own unlink is
 * swallowed along with everything else and the file survives by accident, which is why the
 * sweep above cannot see this at all.
 *
 * What this pins, precisely: at every position the save is intact and a byte-exact copy of
 * it still exists under .bak or .baktmp, AND the position where .baktmp is the only copy
 * is actually reached. With the old cleanup that position does not exist -- the copy is
 * deleted instead -- so this fails without the fix.
 *
 * What it does NOT claim: that the user was ever driven to zero backups. This sweep could
 * not produce that, because a FatFs rename that returns an error frequently still lands
 * (its dirty directory window is flushed by the next operation, which is often the cleanup
 * unlink itself). Retaining a known-good copy is defense in depth here, not a reproduced
 * data loss -- see the note in sf_backup_rolling. */
static void t_rolling_keeps_the_verified_copy(void) {
  long k;
  int saw_rescue = 0;
  fill(s_old, SAVE_BYTES, 11);
  for (k = 0; k < 300; k++) {
    SfStatus st;
    char bak[SF_PATH_MAX];
    fresh_card(4096);
    CHECK(write_raw(SAV, s_old, SAVE_BYTES), "keep k=%ld: setup", k);
    CHECK(write_raw(BAK, s_old, SAVE_BYTES), "keep k=%ld: prior backup", k);
    rd_fail_at = k;                       /* one honest error, then the card is fine again */
    bak[0] = 0;
    st = sf_backup_rolling(SAV, bak, sizeof bak);
    rd_fail_at = -1;
    remount();
    CHECK(holds(SAV, s_old, SAVE_BYTES), "keep k=%ld: a BACKUP damaged the save itself", k);
    CHECK(holds(BAK, s_old, SAVE_BYTES) || holds(BAKTMP, s_old, SAVE_BYTES),
          "keep k=%ld: %s left no byte-exact copy under .bak OR .baktmp (had one going in)",
          k, st == SF_OK ? "success" : sf_status_str(st));
    if (st != SF_OK && !holds(BAK, s_old, SAVE_BYTES) && holds(BAKTMP, s_old, SAVE_BYTES))
      saw_rescue = 1;                     /* the .baktmp really was the last copy standing */
  }
  CHECK(saw_rescue,
        "keep: never reached the state this check exists for (old .bak gone, the verified "
        ".baktmp the only copy) -- the check proves nothing if it cannot get there");
}

/* THE NEXT SAVE, TAKEN FROM THE STATE THE HOLE LEAVES THE USER IN.
 *
 * t_rename_hole_sweep proves the tool now REPORTS that state: no .sav on the card and a
 * complete verified <name>.tmp beside it (SF_WHERE_TMP_ONLY — which the UI puts on screen
 * as "the bytes are in the .tmp"). This starts there, which is the one starting card the
 * sweep never uses, and asks the next question the user asks: they press Save again.
 *
 * If that attempt cannot even OPEN its scratch, it has truncated nothing and written
 * nothing — the .tmp is still the only copy of their save in the world — so its cleanup
 * must not delete it. The shared `if (wst != SF_OK) { f_unlink(tmp); ... }` did exactly
 * that, which is why this file needs a case the sweep cannot express.
 *
 * WHY A NEW KNOB. This needs an f_open that fails while the REST of the card works, and
 * no rd_lie_* setting can produce one: a card that lies permanently swallows the cleanup's
 * own f_unlink too, so the deletion never lands and the loss is invisible — the same blind
 * spot the note on t_rolling_keeps_the_verified_copy describes, and the reason rd_fail_at
 * exists for writes. rd_fail_read_at is its read-side twin: one transient read error (a
 * flaky cart contact on the directory sector), then a healthy card that carries the unlink
 * out for real.
 *
 * WHAT IS ASSERTED, and why it is EXISTENCE rather than contents. f_open can fail before
 * it touches anything (the ordinary case: the .tmp is untouched and complete) or from
 * inside the truncation, after the directory entry is zeroed and while remove_chain walks
 * the FAT (the .tmp is already empty through no fault of the cleanup). f_open never
 * REMOVES the entry in either case, so `the .tmp still exists` is the exact line between
 * the two: only an unlink can cross it. Both are swept; the sweep also requires that the
 * position where the copy survives INTACT is actually reached, or it proves nothing. */
static void t_open_failure_keeps_the_tmp(void) {
  long k;
  int saw_open_fail = 0, saw_intact = 0;
  long first_open_fail = -1;
  fill(s_new, SAVE_BYTES, 12);
  for (k = 0; k < 40; k++) {
    SfStatus st;
    fresh_card(4096);
    /* the post-hole card: the verified scratch is there, the save is NOT */
    CHECK(write_raw(TMP, s_new, SAVE_BYTES), "open k=%ld: setup", k);
    CHECK(!exists(SAV), "open k=%ld: setup left a .sav behind", k);
    remount();                      /* drop FatFs' cached window, as a reboot would */
    rd_fail_read_at = k;            /* one transient read error, then a healthy card */
    st = sf_write_verified(SAV, s_new, SAVE_BYTES);
    rd_fail_read_at = -1;
    remount();

    if (st == SF_ERR_OPEN) {
      if (!saw_open_fail) first_open_fail = k;
      saw_open_fail = 1;
      CHECK(exists(TMP),
            "open k=%ld: the write could not even open its scratch and DELETED the "
            "user's only copy of the save", k);
      if (holds(TMP, s_new, SAVE_BYTES)) saw_intact = 1;
    } else if (st == SF_OK) {
      /* the read error landed somewhere the write could ride out: same promise as ever */
      CHECK(holds(SAV, s_new, SAVE_BYTES),
            "open k=%ld: reported SAVED but the card does not hold the save", k);
    }
    /* NO invariant is asserted for the later failures, and that is deliberate rather than
     * an omission. Once f_open HAS opened the scratch, FA_CREATE_ALWAYS has truncated it,
     * so from that instant the card's copy is gone by design — sf_write_verified is
     * rewriting the very file this card is starting from, with the same bytes, and the
     * caller still holds them in RAM. Demanding a recoverable copy from those positions
     * would be demanding a second scratch name, which is a different change. What IS
     * pinned above is the only part the cleanup controls. */
  }
  CHECK(saw_open_fail,
        "open: no position ever made f_open fail -- the sweep never reaches the branch it "
        "exists for");
  CHECK(saw_intact,
        "open: f_open failed but never with the .tmp still INTACT, so the sweep never "
        "reached the state this check exists for (a complete verified copy, and a write "
        "that failed before touching it)");
  if (getenv("PDNA_TEST_VERBOSE"))
    printf("  open: f_open first fails at read k=%ld\n", first_open_fail);
}

/* An honest but FULL card, mid-commit: the classic non-lying failure must still leave the
 * original in place (this is the invariant sf_write_verified was built for). */
static void t_full_card(void) {
  SfStatus st;
  fresh_card(400);                       /* ~200 KiB: room for one save, not two */
  fill(s_old, SAVE_BYTES, 9);
  fill(s_new, SAVE_BYTES, 10);
  CHECK(write_raw(SAV, s_old, SAVE_BYTES), "full: setup");
  st = sf_write_verified(SAV, s_new, SAVE_BYTES);
  CHECK(st != SF_OK, "full: a card with no room reported success");
  remount();
  CHECK(holds(SAV, s_old, SAVE_BYTES), "full: the original save was lost");
}

/* FINDING 2's shape, at config size. cfg_save used to be the app's one unverified write:
 * FA_CREATE_ALWAYS truncated first, both return codes were discarded, nothing re-read it,
 * so a lying card left /PokeDNA/config.cfg absent with nothing said. Routed through
 * sf_write_verified it cannot claim success on a card that kept nothing -- and a small
 * file exercises a different path from the 128 KiB save (one cluster, sub-sector tail). */
static void t_small_file_like_config(void) {
  static const char cfg[] = "dir=/\nsort=0\nrev=0\nall=0\nhidden=0\nbak=0\n";
  const unsigned n = (unsigned)(sizeof cfg - 1);
  SfStatus st;
  long k;
  int saw_ok = 0, saw_caught = 0;

  fresh_card(4096);
  CHECK(sf_write_verified(DIR_ "/config.cfg", (const unsigned char*)cfg, n) == SF_OK,
        "cfg: honest write failed");
  remount();
  CHECK(holds(DIR_ "/config.cfg", (const unsigned char*)cfg, n), "cfg: card lacks the config");

  for (k = 0; k < 12; k++) {
    fresh_card(4096);
    rd_lie_after = k;
    st = sf_write_verified(DIR_ "/config.cfg", (const unsigned char*)cfg, n);
    rd_lie_after = -1; rd_lie_writes = 0;
    remount();
    if (st == SF_OK) { saw_ok = 1; CHECK(holds(DIR_ "/config.cfg", (const unsigned char*)cfg, n),
                                         "cfg k=%ld: reported OK with nothing on the card", k); }
    else saw_caught = 1;
  }
  CHECK(saw_caught, "cfg: a lying card was never caught at any position");
  (void)saw_ok;
}

int main(void) {
  t_happy_path();
  t_small_file_like_config();
  t_new_file();
  t_lies_from_the_start();
  t_rename_hole_sweep();
  t_rolling_backup_sweep();
  t_rolling_keeps_the_verified_copy();
  t_open_failure_keeps_the_tmp();
  t_full_card();
  f_mount(0, "", 0);
  rd_free();
  if (fails) { printf("host_savefat_test: %d FAILURE(S)\n", fails); return 1; }
  printf("host_savefat_test: all checks passed (real lib/fatfs over a RAM disk)\n");
  return 0;
}
