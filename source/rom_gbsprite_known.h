/* BACKLOG #185 F5 -- source/rom_gbsprite.c's known-ROM fast-path table.
 *
 * GENERATED, not hand-typed: pasted verbatim from a one-off run of the SAME
 * scanner (rom_gbsprite_open(), GB_ROM_NONE) this file already ships, against
 * Guy's own four corpus ROMs (/Users/guyshtainer/VSCodeProjects/gba-toolkit/
 * roms/gb/{Red.gb,Yellow.gb,Gold.gbc,Crystal.gbc} -- never committed, never
 * read by anything except this one-off generator and the tests). NO address
 * here was copied from a decomp or typed by hand -- see
 * tests/host_romgbsprite_known_test.c, which re-runs the SAME scan on the
 * SAME corpus on every test run and asserts byte-for-byte equality against
 * every entry below, so a hand-edit or a scanner change that would make this
 * table WRONG fails loudly instead of silently.
 *
 * Every hit is still re-verified through try_loc() (rom_gbsprite.c) before
 * use -- g1_bs_verify/g1_mew_verify or g2_bd_verify+g2_pp_verify+g2_pal_verify,
 * the EXACT same independent re-reads a .loc cache hit gets -- so a wrong or
 * stale entry here degrades to "fall through to the full scan", never a
 * wrong picture. This table is a HINT, never a source of truth, same
 * contract as the .loc cache file.
 *
 * No entry for Blue or Silver (no corpus dump exists for either): they, and
 * every hack/unknown revision, fall straight through to the full scan.
 *
 * Included from inside rom_gbsprite.c, after RomGbSpriteKnown's own typedef
 * -- this file is data only, not a header in the usual sense (no include
 * guard: it is never included from anywhere else, same convention this
 * codebase already uses for a few generated data tables).
 */
static const RomGbSpriteKnown k_known_gbsprite[] = {
  /* Red.gb */
  { "POKEMON RED\x00" "\x00" "\x00" "\x00" "\x00" "", 0x00, 0x91E6,
    { 0x197091B5U, 0x00100000U, 1, 1, 0, 0,
      0x000383DEU, 0x0000425BU, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
      {
        112,115,32,35,21,100,34,80,2,103,108,102,88,94,29,31,104,111,131,59,
        151,130,90,72,92,123,120,9,127,114,0,0,58,95,22,16,79,64,75,113,
        67,122,106,107,24,47,54,96,76,0,126,0,125,82,109,0,56,86,50,128,
        0,0,0,83,48,149,0,0,0,84,60,124,146,144,145,132,52,98,0,0,
        0,37,38,25,26,0,0,147,148,140,141,116,117,0,0,27,28,138,139,39,
        40,133,136,135,134,66,41,23,46,61,62,13,14,15,0,85,57,51,49,87,
        0,0,10,11,12,68,0,55,97,42,150,143,129,0,0,89,0,99,91,0,
        101,36,110,53,105,0,93,63,65,17,18,121,1,3,73,0,118,119,0,0,
        0,0,77,78,19,20,33,30,74,137,142,0,81,0,0,4,7,5,8,6,
        0,0,0,0,43,44,45,69,70,71,
      },
      {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      } } },
  /* Yellow.gb */
  { "POKEMON YELLOW\x00" "\x00" "", 0x00, 0x047C,
    { 0xA5B529B9U, 0x00100000U, 1, 0, 0, 0,
      0x000383DEU, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
      {
        112,115,32,35,21,100,34,80,2,103,108,102,88,94,29,31,104,111,131,59,
        151,130,90,72,92,123,120,9,127,114,0,0,58,95,22,16,79,64,75,113,
        67,122,106,107,24,47,54,96,76,0,126,0,125,82,109,0,56,86,50,128,
        0,0,0,83,48,149,0,0,0,84,60,124,146,144,145,132,52,98,0,0,
        0,37,38,25,26,0,0,147,148,140,141,116,117,0,0,27,28,138,139,39,
        40,133,136,135,134,66,41,23,46,61,62,13,14,15,0,85,57,51,49,87,
        0,0,10,11,12,68,0,55,97,42,150,143,129,0,0,89,0,99,91,0,
        101,36,110,53,105,0,93,63,65,17,18,121,1,3,73,0,118,119,0,0,
        0,0,77,78,19,20,33,30,74,137,142,0,81,0,0,4,7,5,8,6,
        0,0,0,0,43,44,45,69,70,71,
      },
      {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      } } },
  /* Gold.gbc */
  { "POKEMON_GLDAAUE\x00" "", 0x00, 0x682D,
    { 0xB60AD0FBU, 0x00200000U, 2, 0, 18, 0,
      0x00000000U, 0x00000000U, 0x00051B0BU, 0x00048000U, 0x00000000U, 0x0000AD3DU,
      {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,
      },
      {
        18,19,20,21,22,23,24,25,26,27,28,29,30,31,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      } } },
  /* Crystal.gbc */
  { "PM_CRYSTAL\x00" "BYTE\x00" "", 0x01, 0x18D2,
    { 0xA6A48B42U, 0x00200000U, 2, 0, 18, 0,
      0x00000000U, 0x00000000U, 0x00051424U, 0x00120000U, 0x00000000U, 0x0000A8CEU,
      {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,
      },
      {
        72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,
        88,89,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      } } },
};
