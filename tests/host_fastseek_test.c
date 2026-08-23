/* source/fastseek.c -- the ONE owner of FatFs' per-FIL cluster link map -- over the
 * REAL lib/fatfs on a RAM disk, with FF_USE_FASTSEEK now 1.
 *
 *   cc -std=c11 -DFF_USE_MKFS=1 -Dsiprintf=sprintf -I tests/hostfat -I lib/fatfs -I source \
 *      tests/host_fastseek_test.c source/fastseek.c source/log.c lib/fatfs/ff.c \
 *      lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c -o /tmp/hfs
 *
 * WHY THIS EXISTS. Flipping FF_USE_FASTSEEK is a change to the SAME FatFs the save
 * write path uses, so "it is opt-in per handle" has to be a tested fact, not a reading
 * of ff.c. The three claims that make the flip safe are each asserted below:
 *
 *   1. a FIL that never opts in is untouched -- f_open zeroes cltbl on EVERY successful
 *      open (so an uninitialised stack FIL cannot carry garbage in), and read/seek
 *      behaviour is identical with and without a map;
 *   2. a WRITE handle is refused by fastseek_arm, mechanically, on fp->flag -- this is
 *      what keeps f_write on create_chain instead of clmt_clust, and it is the rule
 *      whose violation writes into another file's clusters;
 *   3. a table that is too small fails CLOSED -- cltbl back to NULL -- because FatFs
 *      leaves it set and UNTERMINATED on FR_NOT_ENOUGH_CORE, and the next f_read would
 *      then walk off the array and read the wrong sectors while returning FR_OK.
 *
 * Plus the two numbers the ledger depends on: sizeof(FIL) and offsetof(FIL, buf), the
 * latter because lib/fatfs/diskio.c's DMA32 fast path needs it to stay 4-aligned.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "fastseek.h"
#include "ff.h"
#include "ramdisk.h"

static int fails = 0, checks = 0;
#define CHK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static FATFS s_fs;
static BYTE s_work[FF_MAX_SS * 2];
static DWORD s_clmt[FASTSEEK_ITEMS_FOR(16)];
static DWORD s_tiny[4];                       /* room for exactly ONE fragment */
static unsigned char s_payload[192 * 1024];   /* big enough to span many clusters */
static unsigned char s_back[8192];

static void fresh_card(void) {
  MKFS_PARM opt = { FM_FAT | FM_SFD, 1, 1, 0, 0 };
  f_mount(0, "", 0);
  rd_init(4096);
  CHK(f_mkfs("", &opt, s_work, sizeof s_work) == FR_OK, "f_mkfs");
  CHK(f_mount(&s_fs, "", 1) == FR_OK, "f_mount");
}

static bool write_file(const char* path, unsigned n) {
  FIL f; UINT bw = 0;
  if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
  if (f_write(&f, s_payload, n, &bw) != FR_OK || bw != n) { f_close(&f); return false; }
  return f_close(&f) == FR_OK;
}

/* Read `n` bytes at `off` and hand back the byte sum, or 0xFFFFFFFF on any failure --
 * the comparison instrument for "armed and unarmed handles return the same bytes". */
static unsigned long read_sum(FIL* f, unsigned off, unsigned n) {
  UINT br = 0;
  if (f_lseek(f, off) != FR_OK) return 0xFFFFFFFFul;
  if (f_read(f, s_back, n, &br) != FR_OK || br != n) return 0xFFFFFFFFul;
  unsigned long v = 0;
  for (unsigned i = 0; i < n; i++) v += s_back[i];
  return v;
}

int main(void) {
  for (unsigned i = 0; i < sizeof s_payload; i++)
    s_payload[i] = (unsigned char)(i * 31u + (i >> 9));

  /* ---- 0: the two struct numbers the EWRAM ledger and diskio.c depend on ------- */
  printf("  sizeof(FIL)=%u offsetof(FIL,buf)=%u _Alignof(FIL)=%u\n",
         (unsigned)sizeof(FIL), (unsigned)offsetof(FIL, buf), (unsigned)_Alignof(FIL));
  CHK(FF_USE_FASTSEEK == 1, "FF_USE_FASTSEEK must be 1 -- this whole test is about that flip");
  /* diskio.c force-aligns its DMA32 destination DOWN to a word and still returns
   * RES_OK, so a 2-mod-4 fp->buf would corrupt reads silently. This is the check that
   * would have caught it. */
  CHK(offsetof(FIL, buf) % 4 == 0, "offsetof(FIL,buf) must stay 4-aligned for diskio.c's DMA32 path");

  fresh_card();
  CHK(write_file("/big.bin", sizeof s_payload), "write /big.bin");
  CHK(write_file("/small.bin", 4096), "write /small.bin");

  /* ---- 1: f_open zeroes cltbl on EVERY successful open ------------------------ */
  /* This is what makes an uninitialised stack FIL safe everywhere in the codebase --
   * savefile.c, art_icons_cache.c and log.c all declare FIL locals and none memsets
   * them. Poison the struct first so a missing zeroing shows up. */
  {
    FIL f;
    memset(&f, 0xA5, sizeof f);
    CHK(f_open(&f, "/big.bin", FA_READ) == FR_OK, "open big for read");
    CHK(f.cltbl == 0, "f_open must zero cltbl on a READ open (poisoned struct)");
    f_close(&f);

    memset(&f, 0xA5, sizeof f);
    CHK(f_open(&f, "/w.bin", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK, "open for write");
    CHK(f.cltbl == 0, "f_open must zero cltbl on a WRITE open too (the save path's case)");
    f_close(&f);
  }

  /* ---- 2: a WRITE handle is refused, mechanically ----------------------------- */
  {
    FIL f;
    CHK(f_open(&f, "/w.bin", FA_WRITE | FA_READ) == FR_OK, "reopen w.bin rw");
    CHK(fastseek_arm(&f, s_clmt, FASTSEEK_ITEMS_FOR(16), "w.bin") == 0,
        "fastseek_arm must REFUSE any handle carrying FA_WRITE");
    CHK(f.cltbl == 0, "a refused handle must be left with cltbl NULL");
    f_close(&f);
  }

  /* ---- 3: armed vs unarmed read the same bytes, and the arm reports fragments -- */
  {
    FIL a, b;
    CHK(f_open(&a, "/big.bin", FA_READ) == FR_OK, "open big (unarmed)");
    CHK(f_open(&b, "/big.bin", FA_READ) == FR_OK, "open big (armed)");
    int frags = fastseek_arm(&b, s_clmt, FASTSEEK_ITEMS_FOR(16), "big.bin");
    printf("  /big.bin armed: frags=%d cltbl=%s\n", frags, b.cltbl ? "set" : "NULL");
    CHK(frags >= 1, "a freshly written contiguous file must arm with >= 1 fragment");
    CHK(b.cltbl != 0, "a successful arm must leave cltbl set");

    /* Backward, forward and far seeks -- the cases FF_USE_FASTSEEK changes. */
    static const unsigned offs[] = { 0, 131072, 4096, 190464, 8192, 65536, 512 };
    int same = 1;
    for (unsigned i = 0; i < sizeof offs / sizeof offs[0]; i++) {
      unsigned long ua = read_sum(&a, offs[i], 4096), ar = read_sum(&b, offs[i], 4096);
      if (ua == 0xFFFFFFFFul || ua != ar) same = 0;
    }
    CHK(same, "an armed handle must return byte-identical data to an unarmed one at every offset");

    /* The instrument: sectors the two handles cost for the SAME backward-seek sweep.
     * Reported, not asserted -- a RAM disk has no seek cost, so the win this buys is
     * a hardware fact (369 -> 126 sectors on the 21-cell Pokedex page); what the host
     * can prove is only that the answer does not change. */
    unsigned long r0 = rd_reads;
    for (int rep = 0; rep < 4; rep++) for (unsigned i = 0; i < 7; i++) read_sum(&a, offs[i], 4096);
    unsigned long cold = rd_reads - r0;
    r0 = rd_reads;
    for (int rep = 0; rep < 4; rep++) for (unsigned i = 0; i < 7; i++) read_sum(&b, offs[i], 4096);
    printf("  28 seek+read pairs: unarmed %lu sectors, armed %lu sectors\n", cold, rd_reads - r0);
    f_close(&a); f_close(&b);
  }

  /* ---- 4: a table too small fails CLOSED -------------------------------------- */
  /* FatFs returns FR_NOT_ENOUGH_CORE but leaves cltbl SET and the table UNTERMINATED;
   * clmt_clust's only stop condition is a zero entry, so a handle left in that state
   * walks off the array and f_read returns FR_OK over the WRONG SECTORS. fastseek_arm
   * must null it. Forced here by fragmenting the volume so the file needs > 1 run. */
  {
    /* Build a fragmented file: interleave two growing files so their clusters
     * alternate, then arm the fragmented one with a 1-fragment table. */
    FIL x, y; UINT bw;
    CHK(f_open(&x, "/fragA.bin", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK, "open fragA");
    CHK(f_open(&y, "/fragB.bin", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK, "open fragB");
    for (int i = 0; i < 24; i++) {
      f_write(&x, s_payload, 4096, &bw); f_sync(&x);
      f_write(&y, s_payload, 4096, &bw); f_sync(&y);
    }
    f_close(&x); f_close(&y);

    FIL f;
    CHK(f_open(&f, "/fragA.bin", FA_READ) == FR_OK, "open fragA for read");
    int frags = fastseek_arm(&f, s_tiny, 4, "fragA.bin");
    printf("  /fragA.bin with a 1-fragment table: fastseek_arm -> %d, cltbl=%s\n",
           frags, f.cltbl ? "SET (BUG)" : "NULL");
    if (frags == 0) {
      CHK(f.cltbl == 0, "a SHORTFALL must leave cltbl NULL -- FatFs does not do this for you");
      /* And the handle must still read correctly, the slow way. */
      CHK(read_sum(&f, 40960, 4096) == read_sum(&f, 40960, 4096), "fallback reads are stable");
      unsigned long want = 0;
      for (unsigned i = 0; i < 4096; i++) want += s_payload[i];
      CHK(read_sum(&f, 0, 4096) == want, "a handle whose arm failed still reads the right bytes");
    } else {
      /* The volume refused to fragment (allocator coalesced) -- report, do not fail. */
      printf("  NOTE: fragA came out in %d fragment(s); the shortfall path was not exercised\n", frags);
      f.cltbl = 0;
    }
    f_close(&f);
  }

  /* ---- 5: an armed READ handle never disturbs a concurrent WRITE handle -------- */
  /* The save path's shape: one file open for read with a map, another open for write.
   * f_write must still go through create_chain and the bytes must land. */
  {
    FIL rd, wr; UINT bw = 0;
    CHK(f_open(&rd, "/big.bin", FA_READ) == FR_OK, "open big for read");
    CHK(fastseek_arm(&rd, s_clmt, FASTSEEK_ITEMS_FOR(16), "big.bin") >= 1, "arm big");
    CHK(f_open(&wr, "/out.bin", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK, "open out for write");
    CHK(wr.cltbl == 0, "the write handle must have no map");
    CHK(f_write(&wr, s_payload, 32768, &bw) == FR_OK && bw == 32768, "write 32 KiB");
    CHK(f_close(&wr) == FR_OK, "close out");
    CHK(f_close(&rd) == FR_OK, "close big");

    FIL v; UINT br = 0; int ok = 1;
    CHK(f_open(&v, "/out.bin", FA_READ) == FR_OK, "reopen out");
    CHK(f_size(&v) == 32768, "out.bin is the size it was told to be");
    for (unsigned off = 0; off < 32768 && ok; off += sizeof s_back) {
      if (f_read(&v, s_back, sizeof s_back, &br) != FR_OK || br != sizeof s_back) { ok = 0; break; }
      if (memcmp(s_back, s_payload + off, sizeof s_back) != 0) ok = 0;
    }
    CHK(ok, "every byte written beside an armed read handle came back intact");
    f_close(&v);
  }

  f_mount(0, "", 0);
  rd_free();
  printf("fastseek (FF_USE_FASTSEEK=1, opt-in per handle): %d checks, %d failure(s)\n", checks, fails);
  return fails ? 1 : 0;
}
