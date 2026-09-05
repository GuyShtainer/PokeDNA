#ifndef DESC_GATE_H
#define DESC_GATE_H

/*
 * Compile-time switch: is the GENERATED, git-ignored description-text module
 * (source/data_desc.c, split out of data_tables.c by tools/gen_data.py) present in
 * THIS build tree right now? Same __has_include-on-the-generated-file technique
 * hand_gate.h/mon_icons_gate.h/rom_chrome_gate.h use for the art modules, applied to
 * the 741 verbatim item/move/ability description strings (docs/AUDIT-2026-09-05-
 * backlog-3-19.md section B / BACKLOG #19's last item).
 *
 * WHY THIS MATTERS (IP): unlike species/move/item NAMES (short, largely functional
 * labels data_tables.c still carries unconditionally), s_itemdesc/s_mvdesc/
 * s_abilitydesc are full sentences copied byte-for-byte from the retail games' text
 * banks -- ~50-65 KB of verbatim Game Freak copy, IP-RED per
 * ~/.claude/ip-publishing-policy.md the same way the ripped sprite/icon/background
 * art is. source/rom_text.c already reads this same text live from the user's own
 * ROM at runtime (preferred path, see pdna_main.c's desc_or_fallback()) -- this gate
 * lets the artless BINARY stop carrying the embedded copy too, not just prefer the
 * ROM at runtime while still shipping the fallback text in every .gba.
 *
 * WHY THIS MATTERS (SIZE): the Makefile globs every source/ .c file unconditionally
 * and links every resulting .o directly (no -ffunction-sections/--gc-sections), so
 * any code this repo compiles ends up in EVERY build's .gba, called or not. Excluding
 * data_desc.c from PDNA_ARTLESS=1's CFILES (Makefile) removes the ~50-65 KB of string
 * data outright; data_desc_shim.c's `#if !PDNA_DESC_TEXT_COMPILED` block stands in
 * with a short placeholder so pk_item_desc/pk_move_desc/pk_ability_desc are always
 * defined exactly once, in every variant.
 *
 * A `-D` on the compile line (the Makefile's artless target uses this; a host test
 * can too) wins over the __has_include probe, same as the other gates, so descgate
 * state can be forced regardless of what this machine's source/ tree happens to have
 * staged locally.
 */
#ifndef PDNA_DESC_TEXT_COMPILED
#if defined(__has_include) && __has_include("data_desc.c")
#define PDNA_DESC_TEXT_COMPILED 1
#else
#define PDNA_DESC_TEXT_COMPILED 0
#endif
#endif

#endif /* DESC_GATE_H */
