/* Host test for rom_identify() (BACKLOG #54, T0: ROM-hack detection).
 *   cc -std=c11 -Wall -I source tests/host_romident_test.c source/rom_map.c \
 *      -o /tmp/hri && /tmp/hri
 *
 * rom_identify() is pure C (no I/O, no statics) so it is exercised directly with
 * synthetic 0xC0 headers here -- no ROM file needed for that half. The second half
 * is the BLOCKING anti-false-positive proof this feature exists to satisfy: all five
 * of Guy's own corpus ROMs (real retail dumps) must classify RETAIL, never HACK --
 * a misdetected retail ROM is the exact failure decision 3 (no content hash / no
 * fingerprint window) exists to avoid. Skips cleanly when the corpus is absent (this
 * runs on any machine, not just Guy's).
 */
#include <stdio.h>
#include <string.h>
#include "rom_map.h"

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
  } while (0)

/* Build a synthetic 0xC0 header: hdr[0xB2] = gba (0x96 unless told otherwise),
 * code at 0xAC (4 bytes), version at 0xBC, title at 0xA0 (12 bytes, space-padded
 * like a real header -- rom_identify() only ever compares the first 12 bytes,
 * never relies on a NUL). */
static void build_hdr(uint8_t hdr[0xC0], const char* title, const char* code,
                       uint8_t version, bool gba_marker) {
  memset(hdr, 0, 0xC0);
  hdr[0xB2] = gba_marker ? 0x96 : 0x00;
  size_t tl = strlen(title); if (tl > 12) tl = 12;
  memcpy(hdr + 0xA0, title, tl);
  size_t cl = strlen(code); if (cl > 4) cl = 4;
  memcpy(hdr + 0xAC, code, cl);
  hdr[0xBC] = version;
}

#define MIB16 (16u * 1024u * 1024u)
#define MIB32 (32u * 1024u * 1024u)

int main(void) {
  uint8_t hdr[0xC0];
  RomKind kind;
  RomIdent id;

  /* rule (b): pinned code+version, correct size, correct title -> RETAIL */
  build_hdr(hdr, "POKEMON EMER", "BPEE", 0, true);
  id = rom_identify(hdr, MIB16, &kind);
  CHECK(id == ROM_ID_RETAIL, "retail BPEE r0 @16 MiB got ident=%d, expected RETAIL", id);
  CHECK(kind == ROM_EMERALD, "retail BPEE r0 got kind=%d, expected ROM_EMERALD", kind);

  /* rule (c): pinned code+version, correct size, WRONG title -> HACK, base known */
  build_hdr(hdr, "POKEMON HACK", "BPEE", 0, true);
  id = rom_identify(hdr, MIB16, &kind);
  CHECK(id == ROM_ID_HACK, "BPEE r0 titled 'POKEMON HACK' got ident=%d, expected HACK", id);
  CHECK(kind == ROM_EMERALD, "BPEE r0 titled 'POKEMON HACK' got kind=%d, expected ROM_EMERALD "
        "(the impersonated game, per decision 4)", kind);

  /* rule (c): pinned code+version, correct title, WRONG size (32 MiB) -> HACK, base known.
   * This is the case that needed rom_open() to read the header even when size mismatches
   * a retail cart -- rom_identify() itself takes size as a parameter regardless. */
  build_hdr(hdr, "POKEMON EMER", "BPEE", 0, true);
  id = rom_identify(hdr, MIB32, &kind);
  CHECK(id == ROM_ID_HACK, "BPEE r0 @32 MiB got ident=%d, expected HACK", id);
  CHECK(kind == ROM_EMERALD, "BPEE r0 @32 MiB got kind=%d, expected ROM_EMERALD", kind);

  /* rule (d): code+version pair NOT on k_versions (BPEE version 9 does not exist),
   * but the title starts "POKEMON" -> HACK, base kind UNKNOWN (ROM_NONE). Decision 4:
   * an unattributable hack must never be guessed into a specific game's slot. */
  build_hdr(hdr, "POKEMON EMER", "BPEE", 9, true);
  id = rom_identify(hdr, MIB16, &kind);
  CHECK(id == ROM_ID_HACK, "BPEE version 9 got ident=%d, expected HACK (rule 1d)", id);
  CHECK(kind == ROM_NONE, "BPEE version 9 got kind=%d, expected ROM_NONE (base unknown)", kind);

  /* rule (d): unpinned code, title starts "POKEMON" -> HACK, base kind unknown */
  build_hdr(hdr, "POKEMON XYZ", "ZZZZ", 0, true);
  id = rom_identify(hdr, MIB16, &kind);
  CHECK(id == ROM_ID_HACK, "'ZZZZ'+'POKEMON XYZ' got ident=%d, expected HACK", id);
  CHECK(kind == ROM_NONE, "'ZZZZ'+'POKEMON XYZ' got kind=%d, expected ROM_NONE", kind);

  /* Review fix F3: genuine NON-US retail carts hit rule (d) too -- a real (code,
   * version) pair k_versions simply doesn't pin (only the 11 US builds are).
   * kind == ROM_NONE here means "the UI cannot say what this is" (unsupported
   * region/build), NOT "this is a hack" -- app_register_rom() (pdna_main.c) now
   * shows PDNA_ROMOTHER_* instead of PDNA_ROMHACK_* whenever kind == ROM_NONE, so
   * this asserts the ONE signal that banner-selection logic reads. */
  build_hdr(hdr, "POKEMON RUBY", "AXVD", 0, true);  /* Ruby, German */
  id = rom_identify(hdr, MIB16, &kind);
  CHECK(id == ROM_ID_HACK, "'AXVD' (Ruby DE) got ident=%d, expected HACK (rule 1d, "
        "genuine non-US retail, NOT a real hack)", id);
  CHECK(kind == ROM_NONE, "'AXVD' (Ruby DE) got kind=%d, expected ROM_NONE -- the UI "
        "must show 'unsupported ROM', never 'ROM HACK'", kind);

  build_hdr(hdr, "POKEMON EMER", "BPEJ", 0, true);  /* Emerald, Japanese */
  id = rom_identify(hdr, MIB16, &kind);
  CHECK(id == ROM_ID_HACK, "'BPEJ' (Emerald JP) got ident=%d, expected HACK (rule 1d)", id);
  CHECK(kind == ROM_NONE, "'BPEJ' (Emerald JP) got kind=%d, expected ROM_NONE", kind);

  build_hdr(hdr, "POKEMON EMER", "BPES", 0, true);  /* Emerald, Spanish */
  id = rom_identify(hdr, MIB16, &kind);
  CHECK(id == ROM_ID_HACK, "'BPES' (Emerald ES) got ident=%d, expected HACK (rule 1d)", id);
  CHECK(kind == ROM_NONE, "'BPES' (Emerald ES) got kind=%d, expected ROM_NONE", kind);

  /* rule (e): unpinned code, title does NOT start "POKEMON" -> NOT_POKEMON */
  build_hdr(hdr, "TETRIS", "ZZZZ", 0, true);
  id = rom_identify(hdr, MIB16, &kind);
  CHECK(id == ROM_ID_NOT_POKEMON, "'ZZZZ'+'TETRIS' got ident=%d, expected NOT_POKEMON", id);
  CHECK(kind == ROM_NONE, "'ZZZZ'+'TETRIS' got kind=%d, expected ROM_NONE", kind);

  /* rule (a): hdr[0xB2] != 0x96 -> NOT_GBA, checked before anything else in the
   * header is trusted (even a header that otherwise looks like retail Emerald). */
  build_hdr(hdr, "POKEMON EMER", "BPEE", 0, false);
  id = rom_identify(hdr, MIB16, &kind);
  CHECK(id == ROM_ID_NOT_GBA, "hdr[0xB2]=0 got ident=%d, expected NOT_GBA", id);
  CHECK(kind == ROM_NONE, "hdr[0xB2]=0 got kind=%d, expected ROM_NONE", kind);

  /* base_kind may be passed NULL (rom_open() itself always passes a real pointer,
   * but the contract should not crash a caller that doesn't care). */
  build_hdr(hdr, "POKEMON EMER", "BPEE", 0, true);
  id = rom_identify(hdr, MIB16, 0);
  CHECK(id == ROM_ID_RETAIL, "NULL base_kind still classifies RETAIL, got ident=%d", id);

  /* ---- the BLOCKING anti-false-positive proof: all five corpus ROMs -> RETAIL ---- */
  {
    static const char* names[] = { "Emerald", "Ruby", "Sapphire", "FireRed", "LeafGreen" };
    const char* dir = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms";
    char path[512];
    int seen = 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
      snprintf(path, sizeof path, "%s/%s.gba", dir, names[i]);
      FILE* f = fopen(path, "rb");
      if (!f) { printf("SKIP corpus %s (no %s)\n", names[i], path); continue; }
      seen++;
      uint8_t chdr[0xC0];
      size_t got = fread(chdr, 1, sizeof chdr, f);
      fseek(f, 0, SEEK_END);
      long sz = ftell(f);
      fclose(f);
      CHECK(got == sizeof chdr, "%s: short header read (%zu of %zu bytes)",
            names[i], got, sizeof chdr);
      if (got != sizeof chdr) continue;
      RomKind ck; RomIdent cid = rom_identify(chdr, (uint32_t)sz, &ck);
      CHECK(cid == ROM_ID_RETAIL,
            "%s: got ident=%d (%s), expected RETAIL -- a misdetected retail ROM is BLOCKING",
            names[i], cid,
            cid == ROM_ID_HACK ? "HACK" : cid == ROM_ID_NOT_POKEMON ? "NOT_POKEMON" : "NOT_GBA");
    }
    if (seen == 0) printf("SKIP: no corpus ROMs found at %s\n", dir);
  }

  printf("\n%d checks, %d FAILED\n", checks, fails);
  return fails ? 1 : 0;
}
