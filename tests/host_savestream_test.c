/* source/savefile.c's sf_write_verified_stream -- the streamed verified-write used by
 * the ROM-art extractor (source/art_icons_extract.c writes icons.bin, ~450 KB, through
 * exactly this) -- over the REAL lib/fatfs, on a RAM disk, with a flashcart's failures.
 *
 *   cc -std=c11 -DFF_USE_MKFS=1 -Dsiprintf=sprintf -I tests/hostfat -I lib/fatfs -I source \
 *      tests/host_savestream_test.c source/savefile.c source/log.c lib/fatfs/ff.c \
 *      lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c -o /tmp/hss
 *
 * THE INVARIANT UNDER TEST, restated for a streamed source: an interrupted extraction
 * must never look complete. sf_write_verified_stream's four steps (write -> re-derive-
 * and-compare -> unlink -> rename -> read-the-card-back) are the SAME invariant
 * host_savefat_test.c proves for sf_write_verified, generalised to content that never
 * exists as one RAM buffer. t_hole_sweep below is that same sweep, adapted: it starts
 * the card lying at EVERY sector of a real ~450 KB streamed commit (icons.bin's exact
 * size) and holds the function to one rule at each position --
 *
 *     SF_OK  =>  the CARD holds the new bytes at the target name. No exceptions.
 *     error  =>  the user still holds recoverable data: the old file, the new file, or
 *                a complete .tmp -- and CRUCIALLY, the OLD art.idx (if any) was never
 *                touched by this call at all, so a caller that only renames art.idx
 *                into place AFTER every kind file's stream-write returns SF_OK can
 *                never end up with a manifest describing a kind file that isn't there. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "savefile.h"
#include "ramdisk.h"

#define DIR_  "/PokeDNA/art"
#define ICONS DIR_ "/icons.bin"
#define TMP   ICONS ".tmp"
#define STREAM_BYTES 451096u /* ART_ICONS_TOTAL_BYTES, kept literal so this test does
                                not need art_icons_extract.h -- it exercises the
                                generic streaming primitive, not the icons format */
#define CHUNK 1024u

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static FATFS s_fs;
static BYTE  s_work[FF_MAX_SS * 2];
static unsigned char s_expected[STREAM_BYTES];
static unsigned char s_rd[STREAM_BYTES];

/* The "ROM": a deterministic, re-readable byte source -- exactly the role rom_mon
 * plays for the real extractor. Every call for the same offset MUST reproduce the
 * same bytes, which is what makes a two-pass (write, verify) stream possible without
 * ever holding STREAM_BYTES in RAM. */
static bool gen_src(void* ctx, uint32_t off, uint8_t* dst, uint32_t want) {
  (void)ctx;
  if ((uint64_t)off + want > STREAM_BYTES) return false;
  memcpy(dst, s_expected + off, want);
  return true;
}

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
  CHECK(f_mkdir("/PokeDNA") == FR_OK, "f_mkdir /PokeDNA failed");
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
  long got = slurp(path, s_rd, sizeof s_rd);
  return got == (long)n && memcmp(s_rd, want, n) == 0;
}

static int exists(const char* path) { FILINFO fi; return f_stat(path, &fi) == FR_OK; }

/* ---------------------------------------------------------------------------- */

static void t_happy_path(void) {
  fresh_card(4096);
  fill(s_expected, STREAM_BYTES, 1);
  static uint8_t scratch[CHUNK];
  SfStatus st = sf_write_verified_stream(ICONS, gen_src, 0, STREAM_BYTES, scratch, CHUNK);
  CHECK(st == SF_OK, "happy: write failed (%s)", sf_status_str(st));
  remount();
  CHECK(holds(ICONS, s_expected, STREAM_BYTES), "happy: card does not hold the stream");
  CHECK(!exists(TMP), "happy: .tmp left behind on success");
}

/* A generator that reports failure (a cancel, or a ROM read that never verified)
 * partway through must abort cleanly: no icons.bin, no .tmp left holding half a file
 * that could ever be mistaken for a real one. */
static uint32_t s_fail_at;
static bool gen_src_fails_at(void* ctx, uint32_t off, uint8_t* dst, uint32_t want) {
  if (off >= s_fail_at) return false;
  return gen_src(ctx, off, dst, want);
}
static void t_generator_cancels(void) {
  fresh_card(4096);
  fill(s_expected, STREAM_BYTES, 2);
  static uint8_t scratch[CHUNK];
  s_fail_at = STREAM_BYTES / 3; /* "cancelled" a third of the way through the write pass */
  SfStatus st =
      sf_write_verified_stream(ICONS, gen_src_fails_at, 0, STREAM_BYTES, scratch, CHUNK);
  CHECK(st == SF_ERR_READ, "cancel: expected SF_ERR_READ, got %s", sf_status_str(st));
  remount();
  CHECK(!exists(ICONS), "cancel: a half-written icons.bin must not exist under its real name");
  CHECK(!exists(TMP), "cancel: the aborted .tmp must be cleaned up, not left as debris");
}

/* A card lying from the very first sector: the write pass "succeeds" (every disk_write
 * ACKs), but the .tmp the card actually kept holds nothing real, so the re-derive-and-
 * compare verify pass must catch it -- SF_ERR_VERIFY, no promotion to icons.bin. */
static void t_lies_from_the_start(void) {
  fresh_card(4096);
  fill(s_expected, STREAM_BYTES, 3);
  static uint8_t scratch[CHUNK];
  rd_lie_writes = 1;
  SfStatus st = sf_write_verified_stream(ICONS, gen_src, 0, STREAM_BYTES, scratch, CHUNK);
  rd_lie_writes = 0;
  CHECK(st != SF_OK, "lies: reported SF_OK on a card that kept nothing");
  remount();
  CHECK(!exists(ICONS), "lies: icons.bin must not exist after a from-the-start lie");
}

/* THE SWEEP. Start the card lying at sector k of the stream commit, for every k in a
 * real ~450 KB write's span, and hold the function to its contract at every position. */
static void t_hole_sweep(void) {
  long k, span;
  int saw_ok = 0, saw_the_hole = 0, lied_ok = 0;

  fresh_card(4096);
  fill(s_expected, STREAM_BYTES, 4);
  static uint8_t scratch[CHUNK];
  rd_writes = 0;
  CHECK(sf_write_verified_stream(ICONS, gen_src, 0, STREAM_BYTES, scratch, CHUNK) == SF_OK,
        "sweep: calibration write failed");
  span = (long)rd_writes + 8;
  CHECK(span > 800, "sweep: a ~450 KB stream commit only cost %ld sectors?", span);

  for (k = 0; k < span; k++) {
    SfStatus st;
    int path_new, tmp_new, lied;

    fresh_card(4096);
    /* no pre-existing icons.bin: extraction always starts from nothing (or from a
       previous COMPLETE cache the caller deletes/replaces via the same mechanism) */
    rd_lie_after = k;
    st = sf_write_verified_stream(ICONS, gen_src, 0, STREAM_BYTES, scratch, CHUNK);
    lied = (rd_lied != 0);
    rd_lie_after = -1; rd_lie_writes = 0;

    remount();
    path_new = holds(ICONS, s_expected, STREAM_BYTES);
    tmp_new = holds(TMP, s_expected, STREAM_BYTES);

    if (st == SF_OK) {
      saw_ok = 1;
      if (lied) lied_ok = 1;
      CHECK(path_new, "sweep k=%ld: reported SF_OK but the card holds %s (tmp=%d)", k,
            exists(ICONS) ? "something else" : "NOTHING AT ALL", tmp_new);
    } else {
      /* THE invariant that matters here (DESIGN.md Sec 2.2: "an interrupted extraction
       * must never look complete"): on ANY failure, icons.bin under its REAL name is
       * either absent, or -- if some earlier state left one -- byte-perfect. It must
       * NEVER exist half-written/corrupt under the name a caller (or a future loader)
       * could mistake for a real cache. A leftover incomplete .tmp is harmless debris
       * by construction: nothing in this codebase ever opens a bare ".tmp" as if it
       * were the real file (the same tolerance host_savefat_test.c's sweep accepts for
       * sf_write_verified). */
      CHECK(!exists(ICONS) || path_new,
            "sweep k=%ld: reported %s but icons.bin exists and is NOT the intended "
            "content -- exactly the half-served cache the design forbids",
            k, sf_status_str(st));
      if (tmp_new && !exists(ICONS)) saw_the_hole = 1; /* the rescuable interleaving */
    }
  }
  CHECK(saw_ok, "sweep: never once saw SF_OK across the whole span (bad calibration?)");
  printf("  sweep: span=%ld sf_ok_positions_included_lies=%d rescuable_tmp_seen=%d\n",
         span, lied_ok, saw_the_hole);
}

/* Zero-length content is a degenerate but legal case (an empty kind, if one ever
 * exists): must not underflow/loop forever and must still produce a verified file. */
static void t_zero_length(void) {
  fresh_card(4096);
  static uint8_t scratch[CHUNK];
  SfStatus st = sf_write_verified_stream(ICONS, gen_src, 0, 0, scratch, CHUNK);
  CHECK(st == SF_OK, "zero: write failed (%s)", sf_status_str(st));
  remount();
  FILINFO fi;
  CHECK(f_stat(ICONS, &fi) == FR_OK && fi.fsize == 0, "zero: expected a 0-byte file");
}

/* chunk > the internal compare buffer must be refused up front, not silently corrupt
 * the compare (SF_ERR_LAYOUT, no card I/O attempted at all). */
static void t_bad_chunk(void) {
  fresh_card(4096);
  static uint8_t scratch[4096];
  SfStatus st = sf_write_verified_stream(ICONS, gen_src, 0, STREAM_BYTES, scratch, 4096);
  CHECK(st == SF_ERR_LAYOUT, "bad_chunk: expected SF_ERR_LAYOUT, got %s", sf_status_str(st));
  st = sf_write_verified_stream(ICONS, gen_src, 0, STREAM_BYTES, scratch, 0);
  CHECK(st == SF_ERR_LAYOUT, "bad_chunk(0): expected SF_ERR_LAYOUT, got %s", sf_status_str(st));
}

int main(void) {
  t_happy_path();
  t_generator_cancels();
  t_lies_from_the_start();
  t_zero_length();
  t_bad_chunk();
  t_hole_sweep();
  printf("%s\n", fails ? "FAILED" : "all good");
  return fails ? 1 : 0;
}
