/* source/art_session.c -- the app-level "is this kind's cache ready" resolver -- over
 * the REAL lib/fatfs on a RAM disk.
 *
 *   cc -std=c11 -DFF_USE_MKFS=1 -Dsiprintf=sprintf -I tests/hostfat -I lib/fatfs -I source \
 *      tests/host_artsession_test.c source/art_session.c source/art_cache.c \
 *      source/artbuf.c source/log.c lib/fatfs/ff.c lib/fatfs/ffunicode.c \
 *      tests/hostfat/ramdisk.c -o /tmp/has && /tmp/has
 *
 * rmbl_pause/rmbl_resume are stubbed below (this test does not link source/rmbl.c —
 * the motor-freeze bracketing is a GBA-hardware concern this test cannot observe;
 * what it CAN and does prove is the decision logic around it).
 *
 * What this proves:
 *   1. a good art.idx + a good icons.bin -> ready, with no RomCtx to cross-check;
 *   2. a good art.idx cross-checked against the SAME rom it was stamped for -> ready;
 *   3. cross-checked against a DIFFERENT rom (code/rev/size/fnv, one at a time) -> NOT
 *      ready -- a cache from the wrong ROM is never served, even though the cache
 *      file itself is perfectly well-formed;
 *   4. art.idx present but the kind file MISSING, wrong size, or wrong FNV (index
 *      disagrees with payload, the on-card version of that check) -> NOT ready;
 *   5. no art.idx at all -> NOT ready, cheaply (no crash, no false positive);
 *   6. memoization: once decided, the verdict does not change even if the file is
 *      corrupted AFTER the first check -- until art_session_invalidate() runs, then
 *      it re-checks and (correctly) flips to NOT ready.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "art_session.h"
#include "ff.h"
#include "ramdisk.h"

void rmbl_pause(void) {}
void rmbl_resume(void) {}

static int fails = 0, checks = 0;
#define CHK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static FATFS s_fs;
static BYTE s_work[FF_MAX_SS * 2];
#define ICONS_BYTES 4096u /* a small stand-in payload -- the FNV machinery does not
                              care about the real 451,096 B icons.bin size */

static void fresh_card(void) {
  MKFS_PARM opt = { FM_FAT | FM_SFD, 1, 1, 0, 0 };
  f_mount(0, "", 0);
  rd_init(4096);
  CHK(f_mkfs("", &opt, s_work, sizeof s_work) == FR_OK, "f_mkfs failed");
  CHK(f_mount(&s_fs, "", 1) == FR_OK, "f_mount failed");
  CHK(f_mkdir("/PokeDNA") == FR_OK, "f_mkdir /PokeDNA failed");
  CHK(f_mkdir(ART_DIR) == FR_OK, "f_mkdir " ART_DIR " failed");
  art_session_invalidate();
}

static bool write_raw(const char* path, const uint8_t* buf, uint32_t n) {
  FIL f; UINT bw = 0;
  if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
  if (n && (f_write(&f, buf, n, &bw) != FR_OK || bw != n)) { f_close(&f); return false; }
  return f_close(&f) == FR_OK;
}

static uint8_t s_icons[ICONS_BYTES];

/* Write a well-formed one-kind art.idx (ICONS only) stamped for the given rom fields,
 * describing s_icons (already filled by the caller) with its TRUE fnv. */
static void write_good_idx(const char rom_code[4], uint8_t rom_rev, uint8_t rom_kind,
                           uint32_t rom_bytes, uint32_t rom_fnv) {
  uint32_t icons_fnv = art_fnv1a(ART_FNV1A_INIT, s_icons, ICONS_BYTES);
  ArtIdxHead h;
  memset(&h, 0, sizeof h);
  h.format = ART_IDX_FORMAT_V1;
  h.builder = 1;
  memcpy(h.rom_code, rom_code, 4);
  h.rom_rev = rom_rev;
  h.rom_kind = rom_kind;
  h.kinds = (uint8_t)(1u << ART_KIND_ICONS);
  h.rom_bytes = rom_bytes;
  h.rom_fnv = rom_fnv;
  uint8_t buf[ART_IDX_HEAD_BYTES + ART_IDX_KIND_BYTES];
  art_idx_head_write(&h, buf);
  ArtIdxKindRow row = { (uint8_t)ART_KIND_ICONS, ICONS_BYTES, icons_fnv, 1 };
  art_idx_kind_write(&row, buf + ART_IDX_HEAD_BYTES);
  CHK(write_raw(ART_IDX_PATH, buf, sizeof buf), "setup: art.idx write failed");
}

#define FAKE_ROM_SIZE (64u * 1024u)
static uint8_t s_rom[FAKE_ROM_SIZE];
static bool rom_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  (void)ctx;
  if ((uint64_t)off + len > FAKE_ROM_SIZE) return false;
  memcpy(dst, s_rom + off, len);
  return true;
}
static void fill_rom(unsigned seed) {
  for (uint32_t i = 0; i < FAKE_ROM_SIZE; i++)
    s_rom[i] = (uint8_t)((i * 31u + seed * 7u + (i >> 9)) & 0xFF);
}

static void t_good_no_rom(void) {
  fresh_card();
  for (uint32_t i = 0; i < ICONS_BYTES; i++) s_icons[i] = (uint8_t)(i * 3 + 1);
  CHK(write_raw(ART_DIR "/icons.bin", s_icons, ICONS_BYTES), "good: icons.bin write failed");
  write_good_idx("BPEE", 0, 1, 999, 0x1234u);
  CHK(art_session_icons_ready(0), "good/no-rom: expected ready");
}

static void t_good_with_matching_rom(void) {
  fresh_card();
  fill_rom(1);
  uint32_t rfnv;
  CHK(art_rom_fnv(rom_read, 0, FAKE_ROM_SIZE, &rfnv), "good/rom: rom fnv failed");
  for (uint32_t i = 0; i < ICONS_BYTES; i++) s_icons[i] = (uint8_t)(i * 5 + 2);
  CHK(write_raw(ART_DIR "/icons.bin", s_icons, ICONS_BYTES), "good/rom: icons.bin write failed");
  write_good_idx("BPEE", 2, 1, FAKE_ROM_SIZE, rfnv);

  RomCtx rc; memset(&rc, 0, sizeof rc);
  rc.read = rom_read; rc.ctx = 0; rc.size = FAKE_ROM_SIZE;
  memcpy(rc.code, "BPEE", 5); rc.version = 2; rc.kind = 1;
  CHK(art_session_icons_ready(&rc), "good/rom: expected ready");
}

static void t_wrong_rom(void) {
  fill_rom(2);
  uint32_t rfnv;
  CHK(art_rom_fnv(rom_read, 0, FAKE_ROM_SIZE, &rfnv), "wrong_rom: rom fnv failed");
  RomCtx base; memset(&base, 0, sizeof base);
  base.read = rom_read; base.ctx = 0; base.size = FAKE_ROM_SIZE;
  memcpy(base.code, "BPEE", 5); base.version = 1; base.kind = 1;

  const char* which[] = { "code", "rev", "kind", "size", "fnv" };
  for (int w = 0; w < 5; w++) {
    fresh_card();
    for (uint32_t i = 0; i < ICONS_BYTES; i++) s_icons[i] = (uint8_t)(i + w);
    CHK(write_raw(ART_DIR "/icons.bin", s_icons, ICONS_BYTES), "wrong_rom: icons.bin write failed");
    write_good_idx("BPEE", 1, 1, FAKE_ROM_SIZE, rfnv); /* stamped for `base` exactly */

    RomCtx rc = base;
    if (w == 0) memcpy(rc.code, "AXVE", 5);
    else if (w == 1) rc.version = 9;
    else if (w == 2) rc.kind = 2;
    else if (w == 3) rc.size = FAKE_ROM_SIZE + 1;
    /* fnv case (w==4) uses a rom with the SAME code/rev/kind/size but different
       CONTENT (a "patched ROM of the same size", DESIGN.md Sec 5 risk 6) */
    RomReadFn read_fn = rom_read;
    if (w == 4) { fill_rom(99); read_fn = rom_read; } else fill_rom(2);
    rc.read = read_fn;

    CHK(!art_session_icons_ready(&rc), "wrong_rom(%s): should NOT be ready", which[w]);
    fill_rom(2); /* restore for the next iteration's base assumptions */
  }
}

static void t_missing_or_bad_kind_file(void) {
  fresh_card();
  for (uint32_t i = 0; i < ICONS_BYTES; i++) s_icons[i] = (uint8_t)(i * 9 + 3);
  write_good_idx("BPEE", 0, 1, 1, 1); /* rom fields irrelevant -- called with rc=NULL below */

  /* case A: art.idx present, icons.bin absent entirely */
  CHK(!art_session_icons_ready(0), "missing kind file: should NOT be ready");

  /* case B: icons.bin present but WRONG SIZE */
  fresh_card();
  CHK(write_raw(ART_DIR "/icons.bin", s_icons, ICONS_BYTES - 1), "size: short write failed");
  write_good_idx("BPEE", 0, 1, 1, 1); /* fnv/bytes computed from the FULL s_icons buffer */
  CHK(!art_session_icons_ready(0), "wrong-size kind file: should NOT be ready");

  /* case C: icons.bin present, right size, WRONG CONTENT (index disagrees with payload) */
  fresh_card();
  write_good_idx("BPEE", 0, 1, 1, 1); /* fnv computed from the correct s_icons */
  uint8_t tampered[ICONS_BYTES];
  memcpy(tampered, s_icons, ICONS_BYTES);
  tampered[100] ^= 0xFF;
  CHK(write_raw(ART_DIR "/icons.bin", tampered, ICONS_BYTES), "tamper: write failed");
  CHK(!art_session_icons_ready(0), "payload-disagrees-with-index: should NOT be ready");
}

static void t_memoized_getter_never_does_io(void) {
  fresh_card();
  CHK(!art_session_icons_ready_memoized(), "memoized getter: unchecked epoch must read false");
  for (uint32_t i = 0; i < ICONS_BYTES; i++) s_icons[i] = (uint8_t)(i * 13 + 1);
  CHK(write_raw(ART_DIR "/icons.bin", s_icons, ICONS_BYTES), "memoized: write failed");
  write_good_idx("BPEE", 0, 1, 1, 1);
  /* still false: nothing has triggered a real check yet */
  CHK(!art_session_icons_ready_memoized(), "memoized getter: must not trigger its own I/O");
  CHK(art_session_icons_ready(0), "memoized: the real check should now find it ready");
  CHK(art_session_icons_ready_memoized(), "memoized getter: should now read true");
}

static void t_no_idx_at_all(void) {
  fresh_card();
  CHK(!art_session_icons_ready(0), "no art.idx at all: should NOT be ready");
  CHK(!art_session_icons_ready(0), "no art.idx (repeat, memoized path): should NOT be ready");
}

static void t_memoization(void) {
  fresh_card();
  for (uint32_t i = 0; i < ICONS_BYTES; i++) s_icons[i] = (uint8_t)(i * 11 + 5);
  CHK(write_raw(ART_DIR "/icons.bin", s_icons, ICONS_BYTES), "memo: write failed");
  write_good_idx("BPEE", 0, 1, 1, 1);
  CHK(art_session_icons_ready(0), "memo: first check should be ready");

  /* corrupt the on-card file AFTER the first (memoized) check */
  uint8_t tampered[ICONS_BYTES];
  memcpy(tampered, s_icons, ICONS_BYTES);
  tampered[0] ^= 0xFF;
  CHK(write_raw(ART_DIR "/icons.bin", tampered, ICONS_BYTES), "memo: tamper write failed");
  CHK(art_session_icons_ready(0),
      "memo: verdict must NOT change mid-epoch even though the file is now bad "
      "(the memo IS the point -- re-hashing 450 KB every box entry would defeat it)");

  art_session_invalidate();
  CHK(!art_session_icons_ready(0),
      "memo: after invalidate(), the (now genuinely bad) file must be re-checked and refused");
}

int main(void) {
  t_good_no_rom();
  t_good_with_matching_rom();
  t_wrong_rom();
  t_missing_or_bad_kind_file();
  t_memoized_getter_never_does_io();
  t_no_idx_at_all();
  t_memoization();
  printf("%d checks, %d fail%s\n", checks, fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
