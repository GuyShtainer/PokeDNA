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
 * offset(4)+size(4)+crc32(4)+pair(4)] + trailer[size(4)+magic(8)+reserved(4)]. Covers
 * what the PARSER itself must catch on its own -- fuse_gb.py's --check
 * (host_fusegb_test.py) already covers the tool side of this same format.
 *
 * BACKLOG #98: format v2 -- the directory block's own magic is "PDNAGBD2" (distinct
 * from the g_pdna_gbd LOCATOR record's "PDNAGBD1", which set_record() below still
 * writes unchanged: that struct is unrelated to the entry-format version), entries are
 * 52 bytes (the new `pair` field at +48), and put_entry() below takes an explicit
 * `pair` argument (FUSED_GB_NO_PAIR when a fixture does not care about pairing). */
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

#define ENTRY_SIZE 52u
#define TRAILER_SIZE 16u

static void put_entry(uint8_t* e, uint32_t type, const char* name,
                      uint32_t off, uint32_t size, uint32_t crc, uint32_t pair) {
  memset(e, 0, ENTRY_SIZE);
  memcpy(e + 0, &type, 4);
  strncpy((char*)(e + 4), name, 31);
  memcpy(e + 36, &off, 4);
  memcpy(e + 40, &size, 4);
  memcpy(e + 44, &crc, 4);
  memcpy(e + 48, &pair, 4);
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
  memcpy(d, "PDNAGBD2", 8);
  uint32_t count = 2;
  memcpy(d + 8, &count, 4);
  put_entry(d + 12 + 0 * ENTRY_SIZE, FUSED_GB_ROM_GEN1, "Red.gb", p0_off, p0_size, crc0,
           FUSED_GB_NO_PAIR);
  put_entry(d + 12 + 1 * ENTRY_SIZE, FUSED_GB_SAV, "Red.sav", p1_off, p1_size, crc1,
           0 /* BACKLOG #98: paired with the ROM entry at directory index 0 */);
  uint32_t dir_size = 12 + ENTRY_SIZE * count + TRAILER_SIZE;
  uint32_t trailer_off = 12 + ENTRY_SIZE * count;
  memcpy(d + trailer_off, &dir_size, 4);
  memcpy(d + trailer_off + 4, "PDNAGBD2", 8);
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
    memcpy(d, "PDNAGBD2", 8);
    uint32_t count = 2;
    memcpy(d + 8, &count, 4);
    put_entry(d + 12 + 0 * ENTRY_SIZE, FUSED_GB_ROM_GEN1, "Red.gb", rom_off, rom_size, rom_crc,
             FUSED_GB_NO_PAIR);
    put_entry(d + 12 + 1 * ENTRY_SIZE, FUSED_GB_LOC, "LOC.sprite.g1", loc_off, loc_size, loc_crc,
             0 /* BACKLOG #98: paired with the ROM entry at directory index 0 */);
    uint32_t dir_size = 12 + ENTRY_SIZE * count + TRAILER_SIZE;
    uint32_t trailer_off = 12 + ENTRY_SIZE * count;
    memcpy(d + trailer_off, &dir_size, 4);
    memcpy(d + trailer_off + 4, "PDNAGBD2", 8);
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

    /* BACKLOG #98: fused_gb_loc() now resolves WHICH ROM's loc it wants before ever
     * looking at a LOC entry's own bytes -- give this fixture a real (single, so
     * unambiguous-by-fallback) ROM_GEN1 entry to pair the bad LOC entry against,
     * otherwise the lookup would fail at the "no gen-1 ROM at all" stage and never
     * reach the framing check this case exists to exercise. */
    uint32_t rom_off = 64, rom_size = 32;
    for (uint32_t i = 0; i < rom_size; i++) buf[rom_off + i] = (uint8_t)(i * 5 + 3);
    uint32_t rom_crc = pdna_rv_crc32(crc_tab, buf + rom_off, rom_size);

    uint32_t loc_off = rom_off + rom_size;
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
    memcpy(d, "PDNAGBD2", 8);
    uint32_t count = 2;
    memcpy(d + 8, &count, 4);
    put_entry(d + 12 + 0 * ENTRY_SIZE, FUSED_GB_ROM_GEN1, "Red.gb", rom_off, rom_size, rom_crc,
             FUSED_GB_NO_PAIR);
    put_entry(d + 12 + 1 * ENTRY_SIZE, FUSED_GB_LOC, "LOC.bad", loc_off, loc_size, loc_crc, 0);
    uint32_t dir_size = 12 + ENTRY_SIZE * count + TRAILER_SIZE;
    uint32_t trailer_off = 12 + ENTRY_SIZE * count;
    memcpy(d + trailer_off, &dir_size, 4);
    memcpy(d + trailer_off + 4, "PDNAGBD2", 8);
    uint32_t reserved = 0;
    memcpy(d + trailer_off + 12, &reserved, 4);

    set_record(buf, dir_off, dir_size);

    const uint8_t* rec = 0;
    CHECK(!fused_gb_loc(1, 1, &rec, 0, 0, 0),
          "LOC bad framing: rec_size lying about its own length must be rejected");
  }
  printf(failed ? "  fused_gb_loc bad framing: SOME FAILED\n" : "  fused_gb_loc bad framing: OK\n");

  /* ---- BACKLOG #68b review D1: a 15-entry directory (more than the OLD
   * FUSED_GB_MAX_ENTRIES==12) must parse ALL 15 entries, and in particular a SAV
   * entry placed after a run of LOC entries -- past where the old 12-entry cache cap
   * would have silently truncated it -- must still be visible and resolve to its
   * real payload bytes. This is the exact shape of the bug the review caught: the
   * real delta-gb recipe (3 ROM + 3 SAV + 8 LOC = 14 entries) put Crystal's own SAV
   * entry (#14, 0-indexed #13) past the old cap, and fused_gb_save_count() silently
   * reported 2 instead of 3. ---- */
  {
    static uint8_t buf[8192];
    uint32_t crc_tab[16];
    pdna_rv_crc32_table(crc_tab);
    memset(buf, 0xAA, sizeof buf);

    struct { uint32_t type; const char* name; } spec[15] = {
      { FUSED_GB_ROM_GEN1, "Red.gb" },
      { FUSED_GB_SAV,      "Red.sav" },
      { FUSED_GB_ROM_GEN2, "Gold.gbc" },
      { FUSED_GB_SAV,      "Gold.sav" },
      { FUSED_GB_LOC,      "LOC.sprite.g2" },
      { FUSED_GB_LOC,      "LOC.icon.g2" },
      { FUSED_GB_LOC,      "LOC.ui.g2" },
      { FUSED_GB_ROM_GEN2, "Crystal.gbc" },
      { FUSED_GB_SAV,      "Crystal.sav" },
      { FUSED_GB_LOC,      "LOC.sprite.g2c" },
      { FUSED_GB_LOC,      "LOC.icon.g2c" },
      { FUSED_GB_LOC,      "LOC.ui.g2c" },
      { FUSED_GB_LOC,      "extra1" },
      { FUSED_GB_LOC,      "extra2" },
      { FUSED_GB_SAV,      "Crystal2.sav" },
    };
    const int N = 15;
    const uint32_t psize = 8;
    uint32_t offs[15];
    uint32_t crcs[15];
    uint32_t cur = 64;
    for (int i = 0; i < N; i++) {
      for (uint32_t k = 0; k < psize; k++) buf[cur + k] = (uint8_t)((i + 1) * 3 + k);
      offs[i] = cur;
      crcs[i] = pdna_rv_crc32(crc_tab, buf + cur, psize);
      cur += psize;
    }
    uint32_t dir_off = cur;
    uint8_t* d = buf + dir_off;
    memcpy(d, "PDNAGBD2", 8);
    uint32_t count = (uint32_t)N;
    memcpy(d + 8, &count, 4);
    for (int i = 0; i < N; i++) {
      /* Pairing is irrelevant to this fixture's own assertions (entry_count/
       * save_count/save() lookups only, no fused_gb_rom()/fused_gb_loc() call) --
       * FUSED_GB_NO_PAIR throughout. */
      put_entry(d + 12 + (uint32_t)i * ENTRY_SIZE, spec[i].type, spec[i].name,
                offs[i], psize, crcs[i], FUSED_GB_NO_PAIR);
    }
    uint32_t dir_size = 12 + ENTRY_SIZE * count + TRAILER_SIZE;
    uint32_t trailer_off = 12 + ENTRY_SIZE * count;
    memcpy(d + trailer_off, &dir_size, 4);
    memcpy(d + trailer_off + 4, "PDNAGBD2", 8);
    uint32_t reserved = 0;
    memcpy(d + trailer_off + 12, &reserved, 4);

    set_record(buf, dir_off, dir_size);

    CHECK(fused_gb_present(), "15-entry fixture: directory should be present");
    CHECK(fused_gb_entry_count() == 15,
          "15-entry fixture: expected all 15 entries visible (#68b D1 capacity fix)");

    int sav_count = fused_gb_save_count();
    CHECK(sav_count == 4,
          "15-entry fixture: expected 4 SAV entries (Red/Gold/Crystal/Crystal2)");
    const char* name = 0; const uint8_t* base = 0; uint32_t size = 0;
    bool found_last = false;
    for (int i = 0; i < sav_count; i++) {
      name = 0; base = 0; size = 0;
      if (fused_gb_save(i, &name, &base, &size) && name && strcmp(name, "Crystal2.sav") == 0) {
        found_last = true;
        CHECK(base == buf + offs[14] && size == psize,
              "15-entry fixture: Crystal2.sav should resolve to its real payload bytes");
      }
    }
    CHECK(found_last,
          "15-entry fixture: the SAV entry after the run of LOC records must be visible");
  }
  printf(failed ? "  15-entry capacity (#68b D1): SOME FAILED\n" : "  15-entry capacity (#68b D1): OK\n");

  /* ---- BACKLOG #69(d): a name field with 32 non-NUL bytes (no NUL terminator).
   * fused_gb_entry() must return the shared empty string "" for it and never read
   * past the 32-byte field. ---- */
  {
    static uint8_t buf[1024];
    memset(buf, 0xAA, sizeof buf);

    /* Build a minimal directory with one entry whose name is 32 'X' bytes (no NUL). */
    uint32_t dir_off = 64;
    uint8_t* d = buf + dir_off;
    memcpy(d, "PDNAGBD2", 8);
    uint32_t count = 1;
    memcpy(d + 8, &count, 4);

    uint8_t* entry = d + 12;
    memset(entry, 0, ENTRY_SIZE);
    uint32_t type = FUSED_GB_ROM_GEN1;
    memcpy(entry + 0, &type, 4);
    /* Fill all 32 bytes of the name field with 'X' (no NUL) */
    memset(entry + 4, 'X', 32);
    uint32_t off = 0, size = 0, crc = 0;
    memcpy(entry + 36, &off, 4);
    memcpy(entry + 40, &size, 4);
    memcpy(entry + 44, &crc, 4);

    uint32_t dir_size = 12 + ENTRY_SIZE * count + TRAILER_SIZE;
    uint32_t trailer_off = 12 + ENTRY_SIZE * count;
    memcpy(d + trailer_off, &dir_size, 4);
    memcpy(d + trailer_off + 4, "PDNAGBD2", 8);
    uint32_t reserved = 0;
    memcpy(d + trailer_off + 12, &reserved, 4);

    set_record(buf, dir_off, dir_size);

    uint32_t etype = 0;
    const char* name = 0;
    uint32_t esize = 0;
    CHECK(fused_gb_entry(0, &etype, &name, &esize),
          "name without NUL: entry should be parseable");
    CHECK(name != 0, "name without NUL: name pointer should not be NULL");
    CHECK(strcmp(name, "") == 0,
          "name without NUL: name should resolve to the shared empty string");
    CHECK(esize == 0, "name without NUL: size should be 0");
  }
  printf(failed ? "  name without NUL (#69d): SOME FAILED\n" : "  name without NUL (#69d): OK\n");

  /* ---- BACKLOG #98: TWO ROMs of the SAME generation (Gold.gbc + Crystal.gbc, both
   * Gen 2), each with its own paired SAV and sprite LOC entry -- exactly the shape
   * the #98 bug lived in (fused_gb_rom(2)/fused_gb_loc(sprite,2) used to always
   * answer Gold's, even with Crystal's save open). Directory order: Gold.gbc(0),
   * Gold.sav(1,pair=0), LOC.sprite.g2(2,pair=0), Crystal.gbc(3), Crystal.sav
   * (4,pair=3), LOC.sprite.g2c(5,pair=3) -- fused_gb_save() enumerates SAV-type
   * entries only, so Gold.sav is save index 0 and Crystal.sav is save index 1. ---- */
  {
    static uint8_t buf[8192];
    uint32_t crc_tab[16];
    pdna_rv_crc32_table(crc_tab);
    memset(buf, 0xAA, sizeof buf);

    uint32_t gold_rom_off = 64, gold_rom_size = 32;
    uint32_t cur = gold_rom_off;
    for (uint32_t i = 0; i < gold_rom_size; i++) buf[cur + i] = (uint8_t)(i * 3 + 1);
    uint32_t gold_rom_crc = pdna_rv_crc32(crc_tab, buf + cur, gold_rom_size);
    cur += gold_rom_size;

    uint32_t gold_sav_off = cur, gold_sav_size = 16;
    for (uint32_t i = 0; i < gold_sav_size; i++) buf[cur + i] = (uint8_t)(i * 5 + 2);
    uint32_t gold_sav_crc = pdna_rv_crc32(crc_tab, buf + cur, gold_sav_size);
    cur += gold_sav_size;

    uint32_t gold_loc_off = cur;
    uint8_t gold_rec[8]; for (int i = 0; i < 8; i++) gold_rec[i] = (uint8_t)(0xA0 + i);
    memcpy(buf + gold_loc_off, "PDNALOC1", 8);
    buf[gold_loc_off + 8] = 1;   /* kind = sprite */
    buf[gold_loc_off + 9] = 2;   /* gen = 2 */
    uint16_t gold_rec_size = sizeof gold_rec;
    memcpy(buf + gold_loc_off + 10, &gold_rec_size, 2);
    uint32_t gold_claimed_hash = 0xDEAD0001u, gold_claimed_size = gold_rom_size;
    memcpy(buf + gold_loc_off + 12, &gold_claimed_hash, 4);
    memcpy(buf + gold_loc_off + 16, &gold_claimed_size, 4);
    memcpy(buf + gold_loc_off + 20, gold_rec, sizeof gold_rec);
    uint32_t gold_loc_size = 20u + gold_rec_size;
    uint32_t gold_loc_crc = pdna_rv_crc32(crc_tab, buf + gold_loc_off, gold_loc_size);
    cur += gold_loc_size;

    uint32_t crys_rom_off = cur, crys_rom_size = 40;
    for (uint32_t i = 0; i < crys_rom_size; i++) buf[cur + i] = (uint8_t)(i * 7 + 4);
    uint32_t crys_rom_crc = pdna_rv_crc32(crc_tab, buf + cur, crys_rom_size);
    cur += crys_rom_size;

    uint32_t crys_sav_off = cur, crys_sav_size = 24;
    for (uint32_t i = 0; i < crys_sav_size; i++) buf[cur + i] = (uint8_t)(i * 9 + 6);
    uint32_t crys_sav_crc = pdna_rv_crc32(crc_tab, buf + cur, crys_sav_size);
    cur += crys_sav_size;

    uint32_t crys_loc_off = cur;
    uint8_t crys_rec[8]; for (int i = 0; i < 8; i++) crys_rec[i] = (uint8_t)(0xC0 + i);
    memcpy(buf + crys_loc_off, "PDNALOC1", 8);
    buf[crys_loc_off + 8] = 1;   /* kind = sprite */
    buf[crys_loc_off + 9] = 2;   /* gen = 2 */
    uint16_t crys_rec_size = sizeof crys_rec;
    memcpy(buf + crys_loc_off + 10, &crys_rec_size, 2);
    uint32_t crys_claimed_hash = 0xDEAD0002u, crys_claimed_size = crys_rom_size;
    memcpy(buf + crys_loc_off + 12, &crys_claimed_hash, 4);
    memcpy(buf + crys_loc_off + 16, &crys_claimed_size, 4);
    memcpy(buf + crys_loc_off + 20, crys_rec, sizeof crys_rec);
    uint32_t crys_loc_size = 20u + crys_rec_size;
    uint32_t crys_loc_crc = pdna_rv_crc32(crc_tab, buf + crys_loc_off, crys_loc_size);
    cur += crys_loc_size;

    uint32_t dir_off = cur;
    uint8_t* d = buf + dir_off;
    memcpy(d, "PDNAGBD2", 8);
    uint32_t count = 6;
    memcpy(d + 8, &count, 4);
    put_entry(d + 12 + 0 * ENTRY_SIZE, FUSED_GB_ROM_GEN2, "Gold.gbc",
             gold_rom_off, gold_rom_size, gold_rom_crc, FUSED_GB_NO_PAIR);
    put_entry(d + 12 + 1 * ENTRY_SIZE, FUSED_GB_SAV, "Gold.sav",
             gold_sav_off, gold_sav_size, gold_sav_crc, 0);
    put_entry(d + 12 + 2 * ENTRY_SIZE, FUSED_GB_LOC, "LOC.sprite.g2",
             gold_loc_off, gold_loc_size, gold_loc_crc, 0);
    put_entry(d + 12 + 3 * ENTRY_SIZE, FUSED_GB_ROM_GEN2, "Crystal.gbc",
             crys_rom_off, crys_rom_size, crys_rom_crc, FUSED_GB_NO_PAIR);
    put_entry(d + 12 + 4 * ENTRY_SIZE, FUSED_GB_SAV, "Crystal.sav",
             crys_sav_off, crys_sav_size, crys_sav_crc, 3);
    put_entry(d + 12 + 5 * ENTRY_SIZE, FUSED_GB_LOC, "LOC.sprite.g2c",
             crys_loc_off, crys_loc_size, crys_loc_crc, 3);
    uint32_t dir_size = 12 + ENTRY_SIZE * count + TRAILER_SIZE;
    uint32_t trailer_off = 12 + ENTRY_SIZE * count;
    memcpy(d + trailer_off, &dir_size, 4);
    memcpy(d + trailer_off + 4, "PDNAGBD2", 8);
    uint32_t reserved = 0;
    memcpy(d + trailer_off + 12, &reserved, 4);

    set_record(buf, dir_off, dir_size);

    /* No active save set (fresh set_record()/fused_gb_test_reset() default: -1) --
     * two Gen-2 ROMs, genuinely ambiguous, must FAIL rather than silently pick
     * Gold (the pre-#98 bug). */
    CHECK(fused_gb_get_active_save() == -1,
          "two-ROM: no active save set yet by default");
    const uint8_t* base = 0; uint32_t size = 0;
    CHECK(!fused_gb_rom(2, &base, &size),
          "two-ROM, no active save: fused_gb_rom(gen2) must fail (ambiguous), not guess");
    CHECK(fused_gb_lookup_was_ambiguous(),
          "two-ROM, no active save: fused_gb_rom(gen2) failure must be flagged ambiguous");
    CHECK(fused_gb_lookup_failed_reason() == FUSED_GB_FAIL_AMBIGUOUS,
          "two-ROM, no active save: fused_gb_rom(gen2) reason must be FUSED_GB_FAIL_AMBIGUOUS");
    const uint8_t* rec = 0;
    CHECK(!fused_gb_loc(1, 2, &rec, 0, 0, 0),
          "two-ROM, no active save: fused_gb_loc(sprite,gen2) must fail (ambiguous)");
    CHECK(fused_gb_lookup_was_ambiguous(),
          "two-ROM, no active save: fused_gb_loc(sprite,gen2) failure must be ambiguous");
    CHECK(fused_gb_lookup_failed_reason() == FUSED_GB_FAIL_AMBIGUOUS,
          "two-ROM, no active save: fused_gb_loc(sprite,gen2) reason must be FUSED_GB_FAIL_AMBIGUOUS");

    /* A generation with ZERO ROMs at all (gen 1, never fused here) must fail WITHOUT
     * the ambiguous flag -- "nothing exists" is not "cannot tell which one". */
    CHECK(!fused_gb_rom(1, &base, &size),
          "two-ROM fixture: gen1 was never fused, fused_gb_rom(1) should just fail");
    CHECK(!fused_gb_lookup_was_ambiguous(),
          "two-ROM fixture: gen1 absence must NOT be flagged ambiguous");

    /* Active save = Gold.sav (fused_gb_save() index 0) -> Gold's ROM and LOC. */
    fused_gb_set_active_save(0);
    CHECK(fused_gb_get_active_save() == 0, "two-ROM: active save getter reflects the setter");
    base = 0; size = 0;
    CHECK(fused_gb_rom(2, &base, &size) && base == buf + gold_rom_off && size == gold_rom_size,
          "two-ROM, active=Gold.sav: fused_gb_rom(gen2) should resolve to Gold.gbc");
    CHECK(!fused_gb_lookup_was_ambiguous(),
          "two-ROM, active=Gold.sav: a resolved lookup must not be flagged ambiguous");
    CHECK(fused_gb_lookup_failed_reason() == FUSED_GB_FAIL_NONE,
          "two-ROM, active=Gold.sav: a resolved lookup's reason must be FUSED_GB_FAIL_NONE");
    rec = 0; uint32_t rlen = 0, idh = 0, rsz = 0;
    CHECK(fused_gb_loc(1, 2, &rec, &rlen, &idh, &rsz) && rec == buf + gold_loc_off + 20 &&
          idh == gold_claimed_hash && rsz == gold_claimed_size,
          "two-ROM, active=Gold.sav: fused_gb_loc(sprite,gen2) should resolve to Gold's LOC");

    /* Active save = Crystal.sav (fused_gb_save() index 1) -> Crystal's ROM and LOC,
     * off the exact same directory/active-save mechanism, nothing else changed --
     * this is the #98 proof: switching which save is active switches which ROM/LOC
     * comes back, from the SAME two-ROM image. */
    fused_gb_set_active_save(1);
    CHECK(fused_gb_get_active_save() == 1, "two-ROM: active save getter reflects the setter");
    base = 0; size = 0;
    CHECK(fused_gb_rom(2, &base, &size) && base == buf + crys_rom_off && size == crys_rom_size,
          "two-ROM, active=Crystal.sav: fused_gb_rom(gen2) should resolve to Crystal.gbc");
    rec = 0; rlen = 0; idh = 0; rsz = 0;
    CHECK(fused_gb_loc(1, 2, &rec, &rlen, &idh, &rsz) && rec == buf + crys_loc_off + 20 &&
          idh == crys_claimed_hash && rsz == crys_claimed_size,
          "two-ROM, active=Crystal.sav: fused_gb_loc(sprite,gen2) should resolve to Crystal's LOC");

    /* An active save index past the last SAV entry actually present must NOT crash
     * and must NOT silently pick one -- falls through to the (still ambiguous, 2
     * ROMs) ordinary case. */
    fused_gb_set_active_save(99);
    base = 0; size = 0;
    CHECK(!fused_gb_rom(2, &base, &size),
          "two-ROM, active save out of range: must fail rather than guess");
    CHECK(fused_gb_lookup_was_ambiguous(),
          "two-ROM, active save out of range: failure must be flagged ambiguous");
    CHECK(fused_gb_lookup_failed_reason() == FUSED_GB_FAIL_AMBIGUOUS,
          "two-ROM, active save out of range: reason must be FUSED_GB_FAIL_AMBIGUOUS "
          "(an out-of-range index degrades to NO_ACTIVE, same as none set)");

    /* Clearing back to "no active save" (-1) restores the original ambiguous-fail
     * behaviour -- fused_gb_set_active_save() is not a one-way ratchet. */
    fused_gb_set_active_save(-1);
    CHECK(fused_gb_get_active_save() == -1, "two-ROM: active save can be cleared back to -1");
    base = 0; size = 0;
    CHECK(!fused_gb_rom(2, &base, &size),
          "two-ROM, active save cleared: back to ambiguous-fail");
  }
  printf(failed ? "  #98 active-save pairing (two ROMs, same gen): SOME FAILED\n"
                : "  #98 active-save pairing (two ROMs, same gen): OK\n");

  /* ---- BACKLOG #98 D1 review fix: ORPHANED active save. The review's exact
   * reproduction -- an active save whose own `pair` is FUSED_GB_NO_PAIR (it was never
   * fused with a ROM at all), alongside a LONE ROM of the generation being asked for.
   * The pre-fix code fell back to "exactly one ROM of this generation" here and
   * silently handed Gold's session Crystal's ROM; the fix must fail instead, flagged
   * ORPHANED, never touching Crystal at all. Directory: Gold.sav(0, orphaned,
   * pair=NO_PAIR), Crystal.gbc(1, the lone Gen-2 ROM, unpaired). ---- */
  {
    static uint8_t buf[2048];
    uint32_t crc_tab[16];
    pdna_rv_crc32_table(crc_tab);
    memset(buf, 0xAA, sizeof buf);

    uint32_t gold_sav_off = 64, gold_sav_size = 16;
    for (uint32_t i = 0; i < gold_sav_size; i++) buf[gold_sav_off + i] = (uint8_t)(i * 5 + 2);
    uint32_t gold_sav_crc = pdna_rv_crc32(crc_tab, buf + gold_sav_off, gold_sav_size);

    uint32_t crys_rom_off = gold_sav_off + gold_sav_size, crys_rom_size = 40;
    for (uint32_t i = 0; i < crys_rom_size; i++) buf[crys_rom_off + i] = (uint8_t)(i * 7 + 4);
    uint32_t crys_rom_crc = pdna_rv_crc32(crc_tab, buf + crys_rom_off, crys_rom_size);

    uint32_t dir_off = crys_rom_off + crys_rom_size;
    uint8_t* d = buf + dir_off;
    memcpy(d, "PDNAGBD2", 8);
    uint32_t count = 2;
    memcpy(d + 8, &count, 4);
    put_entry(d + 12 + 0 * ENTRY_SIZE, FUSED_GB_SAV, "Gold.sav",
             gold_sav_off, gold_sav_size, gold_sav_crc, FUSED_GB_NO_PAIR);
    put_entry(d + 12 + 1 * ENTRY_SIZE, FUSED_GB_ROM_GEN2, "Crystal.gbc",
             crys_rom_off, crys_rom_size, crys_rom_crc, FUSED_GB_NO_PAIR);
    uint32_t dir_size = 12 + ENTRY_SIZE * count + TRAILER_SIZE;
    uint32_t trailer_off = 12 + ENTRY_SIZE * count;
    memcpy(d + trailer_off, &dir_size, 4);
    memcpy(d + trailer_off + 4, "PDNAGBD2", 8);
    uint32_t reserved = 0;
    memcpy(d + trailer_off + 12, &reserved, 4);
    set_record(buf, dir_off, dir_size);

    fused_gb_set_active_save(0);   /* Gold.sav, the only SAV entry -> save index 0 */
    CHECK(fused_gb_get_active_save() == 0, "orphaned: active save getter reflects the setter");
    const uint8_t* base = 0; uint32_t size = 0;
    CHECK(!fused_gb_rom(2, &base, &size),
          "orphaned active save + lone Crystal.gbc: fused_gb_rom(gen2) must NOT borrow Crystal");
    CHECK(fused_gb_lookup_failed_reason() == FUSED_GB_FAIL_ORPHANED,
          "orphaned active save: reason must be FUSED_GB_FAIL_ORPHANED, not AMBIGUOUS "
          "or a silent success");
    CHECK(!fused_gb_lookup_was_ambiguous(),
          "orphaned active save: the legacy ambiguous wrapper must be false (this is a "
          "DIFFERENT failure than genuine ambiguity)");
    const uint8_t* rec = 0;
    CHECK(!fused_gb_loc(1, 2, &rec, 0, 0, 0),
          "orphaned active save: fused_gb_loc(sprite,gen2) must also fail, not borrow Crystal's");
    CHECK(fused_gb_lookup_failed_reason() == FUSED_GB_FAIL_ORPHANED,
          "orphaned active save: fused_gb_loc's reason must also be FUSED_GB_FAIL_ORPHANED");
  }
  printf(failed ? "  #98 D1: orphaned active save must not borrow another ROM: SOME FAILED\n"
                : "  #98 D1: orphaned active save must not borrow another ROM: OK\n");

  /* ---- BACKLOG #98 D1 review fix: GENERATION-MISMATCHED active save. The active
   * save's `pair` resolves to a REAL entry, but that entry is the WRONG generation
   * (e.g. a corrupt/foreign directory pairing a save with the wrong ROM type) --
   * must fail as GEN_MISMATCH, and must NOT fall back to "the lone ROM of the
   * generation actually asked for" either. Directory: Red.gbc(0, Gen-1), Gold.sav
   * (1, paired with entry 0 -- i.e. paired with a Gen-1 ROM), Crystal.gbc(2, the lone
   * Gen-2 ROM, unpaired). Asking fused_gb_rom(2) (Gen-2) with Gold.sav active must
   * fail rather than resolve to Crystal.gbc; asking fused_gb_rom(1) (Gen-1) must
   * still resolve normally (the pairing DOES match that generation). ---- */
  {
    static uint8_t buf[2048];
    uint32_t crc_tab[16];
    pdna_rv_crc32_table(crc_tab);
    memset(buf, 0xAA, sizeof buf);

    uint32_t red_rom_off = 64, red_rom_size = 32;
    for (uint32_t i = 0; i < red_rom_size; i++) buf[red_rom_off + i] = (uint8_t)(i * 3 + 1);
    uint32_t red_rom_crc = pdna_rv_crc32(crc_tab, buf + red_rom_off, red_rom_size);

    uint32_t gold_sav_off = red_rom_off + red_rom_size, gold_sav_size = 16;
    for (uint32_t i = 0; i < gold_sav_size; i++) buf[gold_sav_off + i] = (uint8_t)(i * 5 + 2);
    uint32_t gold_sav_crc = pdna_rv_crc32(crc_tab, buf + gold_sav_off, gold_sav_size);

    uint32_t crys_rom_off = gold_sav_off + gold_sav_size, crys_rom_size = 40;
    for (uint32_t i = 0; i < crys_rom_size; i++) buf[crys_rom_off + i] = (uint8_t)(i * 7 + 4);
    uint32_t crys_rom_crc = pdna_rv_crc32(crc_tab, buf + crys_rom_off, crys_rom_size);

    uint32_t dir_off = crys_rom_off + crys_rom_size;
    uint8_t* d = buf + dir_off;
    memcpy(d, "PDNAGBD2", 8);
    uint32_t count = 3;
    memcpy(d + 8, &count, 4);
    put_entry(d + 12 + 0 * ENTRY_SIZE, FUSED_GB_ROM_GEN1, "Red.gbc",
             red_rom_off, red_rom_size, red_rom_crc, FUSED_GB_NO_PAIR);
    put_entry(d + 12 + 1 * ENTRY_SIZE, FUSED_GB_SAV, "Gold.sav",
             gold_sav_off, gold_sav_size, gold_sav_crc, 0 /* paired with Red.gbc, Gen-1 */);
    put_entry(d + 12 + 2 * ENTRY_SIZE, FUSED_GB_ROM_GEN2, "Crystal.gbc",
             crys_rom_off, crys_rom_size, crys_rom_crc, FUSED_GB_NO_PAIR);
    uint32_t dir_size = 12 + ENTRY_SIZE * count + TRAILER_SIZE;
    uint32_t trailer_off = 12 + ENTRY_SIZE * count;
    memcpy(d + trailer_off, &dir_size, 4);
    memcpy(d + trailer_off + 4, "PDNAGBD2", 8);
    uint32_t reserved = 0;
    memcpy(d + trailer_off + 12, &reserved, 4);
    set_record(buf, dir_off, dir_size);

    fused_gb_set_active_save(0);   /* Gold.sav, the only SAV entry -> save index 0 */
    const uint8_t* base = 0; uint32_t size = 0;
    CHECK(!fused_gb_rom(2, &base, &size),
          "gen-mismatched active save: fused_gb_rom(gen2) must NOT borrow Crystal.gbc");
    CHECK(fused_gb_lookup_failed_reason() == FUSED_GB_FAIL_GEN_MISMATCH,
          "gen-mismatched active save: reason must be FUSED_GB_FAIL_GEN_MISMATCH");
    CHECK(!fused_gb_lookup_was_ambiguous(),
          "gen-mismatched active save: the legacy ambiguous wrapper must be false");

    /* The SAME active save's pairing DOES resolve for the generation it actually
     * matches (Gen-1) -- gen-mismatch is scoped to the mismatching request only. */
    base = 0; size = 0;
    CHECK(fused_gb_rom(1, &base, &size) && base == buf + red_rom_off && size == red_rom_size,
          "gen-mismatched active save: fused_gb_rom(gen1) should still resolve to Red.gbc "
          "(the generation the pairing actually matches)");
    CHECK(fused_gb_lookup_failed_reason() == FUSED_GB_FAIL_NONE,
          "gen-mismatched active save: a resolved gen1 lookup's reason must be FUSED_GB_FAIL_NONE");
  }
  printf(failed ? "  #98 D1: gen-mismatched active save must not borrow another ROM: SOME FAILED\n"
                : "  #98 D1: gen-mismatched active save must not borrow another ROM: OK\n");

  if (failed) { printf("host_fusedgb_test: FAILED\n"); return 1; }
  printf("host_fusedgb_test: ALL OK\n");
  return 0;
}
