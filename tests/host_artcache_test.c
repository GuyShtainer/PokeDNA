/* source/art_cache.c — the ROM-art manifest's pure-C parsing/validation core.
 *
 *   cc -std=c11 -I source tests/host_artcache_test.c source/art_cache.c -o /tmp/hac && /tmp/hac
 *
 * What DESIGN.md Sec 2.2 promises and what this proves, one test per promise:
 *   1. a good manifest round-trips and validates against its own ROM;
 *   2. a TRUNCATED file (cut off anywhere between "no bytes at all" and "header
 *      present but a promised kind row missing") is refused, never half-served —
 *      THE invariant an interrupted extraction leans on;
 *   3. wrong magic / wrong format version are refused;
 *   4. a cache stamped for a DIFFERENT rom_code / rev / kind / size / fnv is refused
 *      by art_idx_matches_rom even though art_idx_parse alone accepts its shape;
 *   5. a kind row whose stored `bytes`/`fnv` disagrees with the PAYLOAD actually on
 *      disk is caught by art_fnv_of_stream (the "index disagrees with payload" case);
 *   6. art_rom_fnv is deterministic (same source -> same hash) and sensitive to a
 *      changed byte anywhere in the sampled windows (different dump -> different
 *      hash), matching "same game, different dump" from DESIGN.md Sec 2.2 point 2.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "art_cache.h"

static int fails = 0, checks = 0;
#define CHK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- a tiny fake ROM the tests can hash/mutate ---------------------------------- */
#define FAKE_ROM_SIZE (256u * 1024u)
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

/* Build a one-kind manifest (ICONS only) into buf; returns its length. */
static uint32_t build_one_kind_idx(uint8_t* buf, const char rom_code[4], uint8_t rom_rev,
                                    uint8_t rom_kind, uint32_t rom_bytes, uint32_t rom_fnv,
                                    uint32_t kind_bytes, uint32_t kind_fnv,
                                    uint32_t kind_entries) {
  ArtIdxHead h;
  memset(&h, 0, sizeof h);
  h.format = ART_IDX_FORMAT_V1;
  h.builder = 42;
  memcpy(h.rom_code, rom_code, 4);
  h.rom_rev = rom_rev;
  h.rom_kind = rom_kind;
  h.kinds = (uint8_t)(1u << ART_KIND_ICONS);
  h.rom_bytes = rom_bytes;
  h.rom_fnv = rom_fnv;
  art_idx_head_write(&h, buf);

  ArtIdxKindRow r = { (uint8_t)ART_KIND_ICONS, kind_bytes, kind_fnv, kind_entries };
  art_idx_kind_write(&r, buf + ART_IDX_HEAD_BYTES);
  return ART_IDX_HEAD_BYTES + ART_IDX_KIND_BYTES;
}

/* ---- 1: good manifest round-trips and matches its own rom ----------------------- */
static void t_good(void) {
  fill_rom(1);
  uint32_t rfnv;
  CHK(art_rom_fnv(rom_read, 0, FAKE_ROM_SIZE, &rfnv), "good: rom fnv failed");

  uint8_t buf[ART_IDX_HEAD_BYTES + ART_IDX_KIND_BYTES];
  uint32_t len = build_one_kind_idx(buf, "BPEE", 0, 1 /*ROM_EMERALD*/, FAKE_ROM_SIZE, rfnv,
                                     451096, 0xdeadbeefu, 440);

  ArtIdxHead h; ArtIdxKindRow rows[ART_KIND_COUNT]; int n = 0;
  ArtIdxParseStatus st = art_idx_parse(buf, len, &h, rows, &n);
  CHK(st == ART_IDX_OK, "good: parse status %d", (int)st);
  CHK(n == 1, "good: n=%d", n);
  CHK(memcmp(h.rom_code, "BPEE", 4) == 0, "good: rom_code mangled");
  CHK(h.rom_bytes == FAKE_ROM_SIZE, "good: rom_bytes mangled");

  RomCtx rc; memset(&rc, 0, sizeof rc);
  memcpy(rc.code, "BPEE", 5);
  rc.version = 0; rc.kind = 1; rc.size = FAKE_ROM_SIZE;
  CHK(art_idx_matches_rom(&h, &rc, rfnv), "good: matches_rom should accept its own rom");

  const ArtIdxKindRow* row = art_idx_find(rows, n, ART_KIND_ICONS);
  CHK(row && row->bytes == 451096 && row->entries == 440, "good: icons row wrong");
  CHK(!art_idx_find(rows, n, ART_KIND_WALLPAPER), "good: wallpaper row should be absent");
}

/* ---- 2: truncation, at every interesting cut point ------------------------------ */
static void t_truncated(void) {
  uint8_t buf[ART_IDX_HEAD_BYTES + ART_IDX_KIND_BYTES];
  uint32_t full = build_one_kind_idx(buf, "BPEE", 0, 1, FAKE_ROM_SIZE, 0x1234u, 100, 1, 2);

  /* absent: zero bytes at all */
  ArtIdxHead h; ArtIdxKindRow rows[ART_KIND_COUNT]; int n;
  CHK(art_idx_parse(buf, 0, &h, rows, &n) == ART_IDX_ABSENT, "trunc: len=0 must be ABSENT");
  CHK(n == 0, "trunc: len=0 must report 0 rows");

  /* every length from 1 up to (full-1) must be TRUNCATED, never OK */
  for (uint32_t len = 1; len < full; len++) {
    ArtIdxParseStatus st = art_idx_parse(buf, len, &h, rows, &n);
    CHK(st == ART_IDX_TRUNCATED, "trunc: len=%u gave status %d, want TRUNCATED", len,
        (int)st);
    CHK(n == 0, "trunc: len=%u reported %d rows on a rejected parse", len, n);
  }
  /* the full length is accepted */
  CHK(art_idx_parse(buf, full, &h, rows, &n) == ART_IDX_OK, "trunc: full length rejected");
}

/* ---- 3: bad magic / bad format ---------------------------------------------------*/
static void t_bad_magic_format(void) {
  uint8_t buf[ART_IDX_HEAD_BYTES + ART_IDX_KIND_BYTES];
  uint32_t full = build_one_kind_idx(buf, "BPEE", 0, 1, FAKE_ROM_SIZE, 0x1234u, 100, 1, 2);

  uint8_t bad_magic[ART_IDX_HEAD_BYTES + ART_IDX_KIND_BYTES];
  memcpy(bad_magic, buf, full);
  bad_magic[0] = 'X';
  ArtIdxHead h; ArtIdxKindRow rows[ART_KIND_COUNT]; int n;
  CHK(art_idx_parse(bad_magic, full, &h, rows, &n) == ART_IDX_BAD_MAGIC, "bad magic accepted");
  CHK(n == 0, "bad magic reported rows");

  uint8_t bad_fmt[ART_IDX_HEAD_BYTES + ART_IDX_KIND_BYTES];
  memcpy(bad_fmt, buf, full);
  bad_fmt[8] = 99; bad_fmt[9] = 0; /* format = 99, a version this build never wrote */
  CHK(art_idx_parse(bad_fmt, full, &h, rows, &n) == ART_IDX_BAD_FORMAT, "bad format accepted");
  CHK(n == 0, "bad format reported rows");
}

/* ---- 4: wrong ROM (each field independently) ------------------------------------ */
static void t_wrong_rom(void) {
  uint32_t rfnv = 0xabcdef01u;
  uint8_t buf[ART_IDX_HEAD_BYTES + ART_IDX_KIND_BYTES];
  build_one_kind_idx(buf, "BPEE", 3, 1, 12345678u, rfnv, 100, 1, 2);
  ArtIdxHead h;
  art_idx_head_read(buf, &h);

  RomCtx rc; memset(&rc, 0, sizeof rc);
  memcpy(rc.code, "BPEE", 5); rc.version = 3; rc.kind = 1; rc.size = 12345678u;
  CHK(art_idx_matches_rom(&h, &rc, rfnv), "wrong_rom: the exact match should pass");

  RomCtx rc2 = rc; memcpy(rc2.code, "AXVE", 5);
  CHK(!art_idx_matches_rom(&h, &rc2, rfnv), "wrong_rom: different code accepted");

  RomCtx rc3 = rc; rc3.version = 4;
  CHK(!art_idx_matches_rom(&h, &rc3, rfnv), "wrong_rom: different rev accepted");

  RomCtx rc4 = rc; rc4.kind = 2;
  CHK(!art_idx_matches_rom(&h, &rc4, rfnv), "wrong_rom: different kind accepted");

  RomCtx rc5 = rc; rc5.size = 12345679u;
  CHK(!art_idx_matches_rom(&h, &rc5, rfnv), "wrong_rom: different size accepted");

  CHK(!art_idx_matches_rom(&h, &rc, rfnv ^ 1u), "wrong_rom: different fnv (patched rom) accepted");
}

/* ---- 5: index disagrees with payload -------------------------------------------- */
typedef struct { const uint8_t* data; uint32_t len; } MemStream;
static bool mem_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  MemStream* m = (MemStream*)ctx;
  if ((uint64_t)off + len > m->len) return false;
  memcpy(dst, m->data + off, len);
  return true;
}
static void t_index_disagrees_with_payload(void) {
  uint8_t payload[4096];
  for (int i = 0; i < 4096; i++) payload[i] = (uint8_t)(i * 3 + 7);
  MemStream ms = { payload, sizeof payload };
  uint8_t scratch[256];
  uint32_t actual_fnv;
  CHK(art_fnv_of_stream(mem_read, &ms, sizeof payload, scratch, sizeof scratch, &actual_fnv),
      "payload: fnv_of_stream failed");

  /* the row CLAIMS a different fnv than the payload actually hashes to */
  ArtIdxKindRow row = { (uint8_t)ART_KIND_ICONS, sizeof payload, actual_fnv ^ 0x55u, 1 };
  CHK(row.fnv != actual_fnv, "payload: test setup didn't actually disagree");

  /* the checker a loader would run: recompute and compare to the stored row */
  uint32_t recomputed;
  CHK(art_fnv_of_stream(mem_read, &ms, row.bytes, scratch, sizeof scratch, &recomputed),
      "payload: recompute failed");
  CHK(recomputed != row.fnv, "payload: a tampered/garbled file was NOT caught");

  /* and the matching-fnv case must NOT false-positive */
  row.fnv = actual_fnv;
  CHK(art_fnv_of_stream(mem_read, &ms, row.bytes, scratch, sizeof scratch, &recomputed) &&
          recomputed == row.fnv,
      "payload: a genuinely matching file was rejected");

  /* bytes mismatch: row claims a length longer than the stream actually has */
  bool ok = art_fnv_of_stream(mem_read, &ms, sizeof payload + 100, scratch, sizeof scratch,
                               &recomputed);
  CHK(!ok, "payload: reading past the real payload length should fail, not fabricate data");
}

/* ---- 6: rom fnv determinism + sensitivity ---------------------------------------- */
static void t_rom_fnv(void) {
  fill_rom(7);
  uint32_t a, b;
  CHK(art_rom_fnv(rom_read, 0, FAKE_ROM_SIZE, &a), "romfnv: first call failed");
  CHK(art_rom_fnv(rom_read, 0, FAKE_ROM_SIZE, &b), "romfnv: second call failed");
  CHK(a == b, "romfnv: not deterministic (%08x vs %08x)", a, b);

  /* flip one byte inside a sampled window (offset 0 is always sampled: anchor for
   * w=0 is 0) and the hash must change — "same game, different dump" must be caught. */
  s_rom[10] ^= 0xFF;
  uint32_t c;
  CHK(art_rom_fnv(rom_read, 0, FAKE_ROM_SIZE, &c), "romfnv: third call failed");
  CHK(c != a, "romfnv: a changed byte in a sampled window went undetected");

  /* too-small ROM: fails closed rather than hashing a short/garbage window */
  uint32_t d;
  CHK(!art_rom_fnv(rom_read, 0, 100, &d), "romfnv: a ROM smaller than one window must fail");
}

int main(void) {
  t_good();
  t_truncated();
  t_bad_magic_format();
  t_wrong_rom();
  t_index_disagrees_with_payload();
  t_rom_fnv();
  printf("%d checks, %d fail%s\n", checks, fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
