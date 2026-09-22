/* BACKLOG #201 F2 -- source/rom_gbicon.c's known-ROM fast-path table for the
 * Gen-2 menu-icon tables (MonMenuIcons/IconPointers/icon bank).
 *
 * GENERATED, not hand-typed: pasted verbatim from a one-off run of the SAME
 * scanner (rom_gbicon_open(), before this table existed) this file already
 * ships, against Guy's own two Gen-2 corpus ROMs
 * (/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb/{Gold.gbc,Crystal.gbc}
 * -- never committed, never read by anything except this one-off generator and
 * the tests). NO address here was copied from a decomp or typed by hand -- see
 * tests/host_gbicon_test.c's part_f2_known_table() (BACKLOG #201), which
 * re-runs the SAME scan on the SAME corpus on every test run (via a
 * checksum-poisoned reopen that forces this table's own known_icon_lookup()
 * to miss) and asserts byte-for-byte equality against every entry below, so a
 * hand-edit or a scanner change that would make this table WRONG fails loudly
 * instead of silently.
 *
 * Every hit is still re-verified through try_loc() (rom_gbicon.c) before use
 * -- menu_icons_cb's ten structural invariants plus the per-kind decode
 * sanity net, the EXACT same independent re-checks a .loc cache hit gets --
 * so a wrong or stale entry here degrades to "fall through to the full
 * scan", never a wrong picture. This table is a HINT, never a source of
 * truth, same contract as source/rom_gbsprite_known.h.
 *
 * The (title, version, global_checksum) identity triple is the SAME ROM
 * header rom_gbsprite_known.h already keys on -- both modules hash the exact
 * same 0x100..0x14F header bytes, so the id_hash values below are
 * byte-identical to that file's own Gold/Crystal entries (cross-checked, not
 * copied from it: a fresh generator run against these same two dumps
 * reproduced the same id_hash independently, since both modules run the
 * identical FNV-1a over the identical bytes).
 *
 * No entry for a Gen-1 ROM (no icon table shape exists there -- rom_gbicon.c
 * never even scans one, see parse_header()/locate()) or Silver (no corpus
 * dump exists): both fall straight through to the full scan.
 *
 * Included from inside rom_gbicon.c, after RomGbIconKnown's own typedef --
 * this file is data only, not a header in the usual sense (no include guard:
 * it is never included from anywhere else), same convention
 * rom_gbsprite_known.h already uses.
 */
static const RomGbIconKnown k_known_gbicon[] = {
  /* Gold.gbc */
  { { 0x50,0x4F,0x4B,0x45,0x4D,0x4F,0x4E,0x5F,0x47,0x4C,0x44,0x41,0x41,0x55,0x45,0x00 },
    0x00, 0x682D,
    { 0xB60AD0FBU, 0x00200000U, 0x0008E975U, 0x0008EA70U, 38, 0x23, { 0, 0 } } },
  /* Crystal.gbc */
  { { 0x50,0x4D,0x5F,0x43,0x52,0x59,0x53,0x54,0x41,0x4C,0x00,0x42,0x59,0x54,0x45,0x00 },
    0x01, 0x18D2,
    { 0xA6A48B42U, 0x00200000U, 0x0008EAC4U, 0x0008EBBFU, 38, 0x23, { 0, 0 } } },
};
