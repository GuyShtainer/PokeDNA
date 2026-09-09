/* Host test for source/fused_gb.c's parser -- the BACKLOG #62 delta-gb fused-corpus
 * reader -- compiled with a STUBBED cart base instead of real (unmapped, on a host)
 * cartridge address space (#62 review D9).
 *
 *   cc -std=c11 -Wall -DPDNA_DELTA -DFUSED_GB_TEST -I source \
 *      tests/host_fusedgb_test.c source/fused_gb.c source/pdna_romver.c \
 *      -o /tmp/hfg && /tmp/hfg
 *
 * FUSED_GB_TEST (fused_gb.c/fused_gb.h) does two things neither the GBA build nor the
 * plain --check path ever needs: it drops `const` off g_pdna_gbd so this test can point
 * it at a directory it built itself, and it makes fused_gb.c's cart_ptr() read through
 * g_fused_gb_test_base (a plain heap/static buffer) instead of computing
 * CART_BASE + offset -- a real host pointer would be silently truncated by that 32-bit
 * arithmetic (this Mac's pointers are 64-bit).
 *
 * Builds a directory BY HAND in the exact byte layout tools/fuse_gb.py writes (see
 * fused_gb.h's own doc comment): magic(8) + count(4) + count*[type(4)+name(32)+
 * offset(4)+size(4)+crc32(4)] + trailer[size(4)+magic(8)+reserved(4)]. Covers what the
 * PARSER itself must catch on its own -- fuse_gb.py's --check (host_fusegb_test.py)
 * already covers the tool side of this same format. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fused_gb.h"
#include "pdna_romver.h"

static int failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failed = 1; } \
  } while (0)

#define ENTRY_SIZE 48u
#define TRAILER_SIZE 16u

static void put_entry(uint8_t* e, uint32_t type, const char* name,
                      uint32_t off, uint32_t size, uint32_t crc) {
  memset(e, 0, ENTRY_SIZE);
  memcpy(e + 0, &type, 4);
  strncpy((char*)(e + 4), name, 31);
  memcpy(e + 36, &off, 4);
  memcpy(e + 40, &size, 4);
  memcpy(e + 44, &crc, 4);
}

/* Builds: [pad][payload0][payload1][directory]. Payload bytes are `pattern`-filled so
 * the CRC is deterministic. Returns the directory's own offset/size (into `buf`) via
 * out params, and fills the two entry records' crc32 fields with the REAL CRC32 of the
 * bytes actually written, using the exact same core the app itself uses at runtime
 * (pdna_rv_crc32) -- so a "GOOD" directory built this way is genuinely self-consistent,
 * not just superficially shaped like one. */
static void build_fixture(uint8_t* buf, uint32_t buf_cap,
                          uint32_t p0_off, uint32_t p0_size,
                          uint32_t p1_off, uint32_t p1_size,
                          uint32_t* out_dir_off, uint32_t* out_dir_size) {
  uint32_t crc_tab[16];
  pdna_rv_crc32_table(crc_tab);

  memset(buf, 0xAA, buf_cap);
  for (uint32_t i = 0; i < p0_size; i++) buf[p0_off + i] = (uint8_t)(i * 7);
  for (uint32_t i = 0; i < p1_size; i++) buf[p1_off + i] = (uint8_t)(i * 13 + 1);
  uint32_t crc0 = pdna_rv_crc32(crc_tab, buf + p0_off, p0_size);
  uint32_t crc1 = pdna_rv_crc32(crc_tab, buf + p1_off, p1_size);

  uint32_t dir_off = p1_off + p1_size;
  uint8_t* d = buf + dir_off;
  memcpy(d, "PDNAGBD1", 8);
  uint32_t count = 2;
  memcpy(d + 8, &count, 4);
  put_entry(d + 12 + 0 * ENTRY_SIZE, FUSED_GB_ROM_GEN1, "Red.gb", p0_off, p0_size, crc0);
  put_entry(d + 12 + 1 * ENTRY_SIZE, FUSED_GB_SAV, "Red.sav", p1_off, p1_size, crc1);
  uint32_t dir_size = 12 + ENTRY_SIZE * count + TRAILER_SIZE;
  uint32_t trailer_off = 12 + ENTRY_SIZE * count;
  memcpy(d + trailer_off, &dir_size, 4);
  memcpy(d + trailer_off + 4, "PDNAGBD1", 8);
  uint32_t reserved = 0;
  memcpy(d + trailer_off + 12, &reserved, 4);

  *out_dir_off = dir_off;
  *out_dir_size = dir_size;
}

/* Points g_pdna_gbd/g_fused_gb_test_base at a fresh buffer AND forces the next
 * parse_once() to actually re-parse it (fused_gb_test_reset(), test-only -- the real
 * app never needs this: its directory is immutable for a whole boot). */
static void set_record(uint8_t* buf, uint32_t dir_off, uint32_t dir_size) {
  fused_gb_test_reset();
  g_fused_gb_test_base = buf;
  memcpy((void*)g_pdna_gbd.magic, "PDNAGBD1", 8);
  g_pdna_gbd.offset = dir_off;
  g_pdna_gbd.size = dir_size;
}

int main(void) {
  /* ---- case 1: a GOOD directory -> fused_gb_present() true, both entries readable,
   * fused_gb_rom()/fused_gb_save() resolve to the right bytes. ---- */
  {
    static uint8_t buf[4096];
    uint32_t dir_off, dir_size;
    build_fixture(buf, sizeof buf, 64, 128, 256, 96, &dir_off, &dir_size);
    set_record(buf, dir_off, dir_size);

    CHECK(fused_gb_present(), "good directory: fused_gb_present() should be true");
    CHECK(fused_gb_entry_count() == 2, "good directory: expected 2 entries");

    uint32_t type = 0, size = 0; const char* name = 0;
    CHECK(fused_gb_entry(0, &type, &name, &size) && type == FUSED_GB_ROM_GEN1 &&
          name && strcmp(name, "Red.gb") == 0 && size == 128,
          "good directory: entry 0 should be ROM_GEN1 Red.gb/128B");
    CHECK(fused_gb_entry(1, &type, &name, &size) && type == FUSED_GB_SAV &&
          name && strcmp(name, "Red.sav") == 0 && size == 96,
          "good directory: entry 1 should be SAV Red.sav/96B");

    const uint8_t* base = 0; size = 0;
    CHECK(fused_gb_rom(1, &base, &size) && base == buf + 64 && size == 128,
          "good directory: fused_gb_rom(gen1) should resolve to the ROM payload");
    CHECK(fused_gb_save_count() == 1, "good directory: expected 1 SAV entry");
    name = 0; base = 0; size = 0;
    CHECK(fused_gb_save(0, &name, &base, &size) && base == buf + 256 && size == 96,
          "good directory: fused_gb_save(0) should resolve to the SAV payload");
  }
  printf(failed ? "  good -> entries: SOME FAILED\n" : "  good -> entries: OK\n");

  /* ---- case 2: BAD MAGIC (directory's own leading magic corrupted) -> rejected ---- */
  {
    static uint8_t buf[4096];
    uint32_t dir_off, dir_size;
    build_fixture(buf, sizeof buf, 64, 128, 256, 96, &dir_off, &dir_size);
    buf[dir_off] ^= 0xFF;                   /* corrupt "PDNAGBD1"'s first byte */
    set_record(buf, dir_off, dir_size);
    CHECK(!fused_gb_present(), "bad magic should be rejected");
    CHECK(fused_gb_entry_count() == 0, "bad magic: entry count should be 0");
  }
  printf(failed ? "  bad magic: SOME FAILED\n" : "  bad magic -> rejected: OK\n");

  /* ---- case 3: BAD COUNT (count implies a directory size the record doesn't state)
   * -> rejected (this is exactly the "never trust a single field in isolation" cross-
   * check parse_once()'s own header comment describes). ---- */
  {
    static uint8_t buf[4096];
    uint32_t dir_off, dir_size;
    build_fixture(buf, sizeof buf, 64, 128, 256, 96, &dir_off, &dir_size);
    uint32_t bad_count = 99;
    memcpy(buf + dir_off + 8, &bad_count, 4);   /* record's own dir_size is unchanged */
    set_record(buf, dir_off, dir_size);
    CHECK(!fused_gb_present(), "bad count should be rejected");
  }
  printf(failed ? "  bad count: SOME FAILED\n" : "  bad count -> rejected: OK\n");

  /* ---- case 4: BAD CRC on one entry -> that entry is dropped (never matched by
   * fused_gb_rom/fused_gb_save), same "corrupt entry is not trusted" posture as an
   * oversized/out-of-bounds entry (#62 D6) -- NOT the same as the whole directory
   * being rejected: a per-entry corruption stays scoped to that one entry. ---- */
  {
    static uint8_t buf[4096];
    uint32_t dir_off, dir_size;
    build_fixture(buf, sizeof buf, 64, 128, 256, 96, &dir_off, &dir_size);
    buf[64] ^= 0xFF;   /* flip a payload byte AFTER its crc32 was already computed */
    set_record(buf, dir_off, dir_size);
    CHECK(fused_gb_present(), "bad CRC on one entry: directory itself is still valid");
    const uint8_t* base = 0; uint32_t size = 0;
    CHECK(!fused_gb_rom(1, &base, &size),
          "bad CRC: the corrupted ROM entry must not be served as real data");
    /* The OTHER (untouched) entry must still resolve -- one bad CRC must not poison
     * the whole directory. */
    const char* name = 0; base = 0; size = 0;
    CHECK(fused_gb_save(0, &name, &base, &size) && base == buf + 256 && size == 96,
          "bad CRC: the untouched SAV entry should still resolve");
  }
  printf(failed ? "  bad CRC: SOME FAILED\n" : "  bad CRC -> entry rejected: OK\n");

  /* ---- case 5: OVERSIZED entry (claims to run past the directory, #62 D6) ---- */
  {
    static uint8_t buf[4096];
    uint32_t dir_off, dir_size;
    build_fixture(buf, sizeof buf, 64, 128, 256, 96, &dir_off, &dir_size);
    /* Entry 1 (Red.sav) claims a size that runs PAST dir_off -- must be dropped by the
     * D6 bound, not merely by the (now also-wrong) CRC. */
    uint32_t huge = dir_size + 4096;
    memcpy(buf + dir_off + 12 + ENTRY_SIZE + 40, &huge, 4);
    set_record(buf, dir_off, dir_size);
    CHECK(fused_gb_present(), "oversized entry: directory itself is still valid");
    const uint8_t* base = 0; uint32_t size = 0;
    CHECK(!fused_gb_save(0, 0, &base, &size),
          "oversized entry: must not be served as real data");
  }
  printf(failed ? "  oversized entry: SOME FAILED\n" : "  oversized entry -> rejected: OK\n");

  /* ---- BACKLOG #68b: fused_gb_loc() over a directory carrying a LOC entry.
   * Builds [ROM_GEN1 payload][LOC payload][directory], where the LOC payload is
   * itself [header(20B): magic+kind+gen+rec_size+id_hash+rom_size][record bytes] --
   * exactly tools/gbloc_driver.c's own framing (see fused_gb.h's fused_gb_loc() doc
   * comment). The "record" here is an arbitrary byte pattern; fused_gb_loc() never
   * interprets it, only hands back a pointer/length, so a real RomGbSpriteLoc is not
   * needed to exercise the parser. ---- */
  {
    static uint8_t buf[4096];
    uint32_t crc_tab[16];
    pdna_rv_crc32_table(crc_tab);
    memset(buf, 0xAA, sizeof buf);

    uint32_t rom_off = 64, rom_size = 128;
    for (uint32_t i = 0; i < rom_size; i++) buf[rom_off + i] = (uint8_t)(i * 7);
    uint32_t rom_crc = pdna_rv_crc32(crc_tab, buf + rom_off, rom_size);

    /* LOC payload: header(20) + a 16-byte fake "record". */
    uint32_t loc_off = rom_off + rom_size;
    uint8_t rec_bytes[16];
    for (int i = 0; i < 16; i++) rec_bytes[i] = (uint8_t)(0xC0 + i);
    uint16_t rec_size = sizeof rec_bytes;
    uint32_t claimed_id_hash = 0x11223344u;
    memcpy(buf + loc_off, "PDNALOC1", 8);
    buf[loc_off + 8] = 1;    /* kind = sprite */
    buf[loc_off + 9] = 1;    /* gen = 1 */
    memcpy(buf + loc_off + 10, &rec_size, 2);
    memcpy(buf + loc_off + 12, &claimed_id_hash, 4);
    memcpy(buf + loc_off + 16, &rom_size, 4);
    memcpy(buf + loc_off + 20, rec_bytes, sizeof rec_bytes);
    uint32_t loc_size = 20u + rec_size;
    uint32_t loc_crc = pdna_rv_crc32(crc_tab, buf + loc_off, loc_size);

    uint32_t dir_off = loc_off + loc_size;
    uint8_t* d = buf + dir_off;
    memcpy(d, "PDNAGBD1", 8);
    uint32_t count = 2;
    memcpy(d + 8, &count, 4);
    put_entry(d + 12 + 0 * ENTRY_SIZE, FUSED_GB_ROM_GEN1, "Red.gb", rom_off, rom_size, rom_crc);
    put_entry(d + 12 + 1 * ENTRY_SIZE, FUSED_GB_LOC, "LOC.sprite.g1", loc_off, loc_size, loc_crc);
    uint32_t dir_size = 12 + ENTRY_SIZE * count + TRAILER_SIZE;
    uint32_t trailer_off = 12 + ENTRY_SIZE * count;
    memcpy(d + trailer_off, &dir_size, 4);
    memcpy(d + trailer_off + 4, "PDNAGBD1", 8);
    uint32_t reserved = 0;
    memcpy(d + trailer_off + 12, &reserved, 4);

    set_record(buf, dir_off, dir_size);

    const uint8_t* rec = 0; uint32_t rlen = 0, idh = 0, rsz = 0;
    CHECK(fused_gb_loc(1, 1, &rec, &rlen, &idh, &rsz),
          "LOC good: fused_gb_loc(sprite, gen1) should resolve");
    CHECK(rec == buf + loc_off + 20, "LOC good: *rec should point past the 20-byte header");
    CHECK(rlen == rec_size, "LOC good: *rec_len should be the record size");
    CHECK(idh == claimed_id_hash, "LOC good: *id_hash should be the header's claimed id_hash");
    CHECK(rsz == rom_size, "LOC good: *rom_size should be the header's claimed rom_size");

    /* Wrong kind/gen must not match this entry. */
    rec = 0;
    CHECK(!fused_gb_loc(2, 1, &rec, 0, 0, 0), "LOC wrong kind: must not resolve");
    CHECK(!fused_gb_loc(1, 2, &rec, 0, 0, 0), "LOC wrong gen: must not resolve");

    /* No LOC entry present at all for a kind/gen this directory never fused. */
    CHECK(!fused_gb_loc(3, 2, &rec, 0, 0, 0), "LOC absent: ui/gen2 was never fused");
  }
  printf(failed ? "  fused_gb_loc good/absent: SOME FAILED\n" : "  fused_gb_loc good/absent: OK\n");

  /* ---- BACKLOG #68b: a LOC entry whose own inner rec_size does not match the
   * directory-recorded entry size (truncated/corrupted framing) must be rejected by
   * fused_gb_loc()'s own framing check, even though the outer per-entry CRC still
   * passes (the CRC covers exactly the bytes present -- it cannot know they were
   * supposed to be longer). ---- */
  {
    static uint8_t buf[4096];
    uint32_t crc_tab[16];
    pdna_rv_crc32_table(crc_tab);
    memset(buf, 0xAA, sizeof buf);

    uint32_t loc_off = 64;
    uint8_t rec_bytes[16];
    for (int i = 0; i < 16; i++) rec_bytes[i] = (uint8_t)(0xD0 + i);
    uint16_t claimed_rec_size = 999;   /* LIES about how long the record is */
    uint32_t fake_id = 1, fake_size = 2;
    memcpy(buf + loc_off, "PDNALOC1", 8);
    buf[loc_off + 8] = 1; buf[loc_off + 9] = 1;
    memcpy(buf + loc_off + 10, &claimed_rec_size, 2);
    memcpy(buf + loc_off + 12, &fake_id, 4);
    memcpy(buf + loc_off + 16, &fake_size, 4);
    memcpy(buf + loc_off + 20, rec_bytes, sizeof rec_bytes);
    /* the actual on-disk entry is only 20+16 bytes, NOT 20+999 */
    uint32_t loc_size = 20u + (uint32_t)sizeof rec_bytes;
    uint32_t loc_crc = pdna_rv_crc32(crc_tab, buf + loc_off, loc_size);

    uint32_t dir_off = loc_off + loc_size;
    uint8_t* d = buf + dir_off;
    memcpy(d, "PDNAGBD1", 8);
    uint32_t count = 1;
    memcpy(d + 8, &count, 4);
    put_entry(d + 12, FUSED_GB_LOC, "LOC.bad", loc_off, loc_size, loc_crc);
    uint32_t dir_size = 12 + ENTRY_SIZE * count + TRAILER_SIZE;
    uint32_t trailer_off = 12 + ENTRY_SIZE * count;
    memcpy(d + trailer_off, &dir_size, 4);
    memcpy(d + trailer_off + 4, "PDNAGBD1", 8);
    uint32_t reserved = 0;
    memcpy(d + trailer_off + 12, &reserved, 4);

    set_record(buf, dir_off, dir_size);

    const uint8_t* rec = 0;
    CHECK(!fused_gb_loc(1, 1, &rec, 0, 0, 0),
          "LOC bad framing: rec_size lying about its own length must be rejected");
  }
  printf(failed ? "  fused_gb_loc bad framing: SOME FAILED\n" : "  fused_gb_loc bad framing: OK\n");

  if (failed) { printf("host_fusedgb_test: FAILED\n"); return 1; }
  printf("host_fusedgb_test: ALL OK\n");
  return 0;
}
