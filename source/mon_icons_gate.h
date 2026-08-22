#ifndef MON_ICONS_GATE_H
#define MON_ICONS_GATE_H

/*
 * Compile-time switch: is the GENERATED, git-ignored icon art (source/mon_icons.c +
 * mon_icons_data.s, from tools/gen_icons.py) present in THIS build tree right now?
 * Same __has_include-on-the-generated-file technique as hand_gate.h/rom_chrome_gate.h,
 * probing mon_icons_data.s (the actual tile blob tools/gen_icons.py's OUT_S writes) --
 * the same signal rom_chrome_gate.h uses for card/pokeblock/bag ("*_data.s" is the
 * real art, the .c is a thin extern wrapper around it, and both are always written
 * together by the same generator run).
 *
 * WHY THIS MATTERS: mon_icons.c's strong mon_icon_for_* / mon_icon_egg_* definitions
 * already win at link time over art_fallbacks.c's weak ones (ordinary weak-symbol
 * resolution, no preprocessor needed there) -- but mon_icon_anim_cheap() has no
 * counterpart in mon_icons.c at all, so it is ALWAYS art_fallbacks.c's single
 * non-weak definition, in every build. That definition answers "is the ROM rung
 * cheap", which is the wrong question in a full-art build: compiled .rodata icons
 * cost zero I/O regardless of s_rommon/icons.bin, so the true answer there is always
 * yes. This gate lets art_fallbacks.c give the full-art build its own body instead
 * of silently reusing the artless one -- see art_fallbacks.c's mon_icon_anim_cheap
 * for the fix this makes possible.
 *
 * A `-D` on the compile line wins over the __has_include probe, same as the other
 * gates, so a host test could force either state regardless of what this machine's
 * source/ tree happens to have generated locally.
 */
#ifndef PDNA_MON_ICONS_ART_COMPILED
#if defined(__has_include) && __has_include("mon_icons_data.s")
#define PDNA_MON_ICONS_ART_COMPILED 1
#else
#define PDNA_MON_ICONS_ART_COMPILED 0
#endif
#endif

#endif /* MON_ICONS_GATE_H */
