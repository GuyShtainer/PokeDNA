#ifndef HAND_GATE_H
#define HAND_GATE_H

/*
 * Compile-time switch: is the GENERATED, git-ignored glove art (source/hand_oam.c,
 * from tools/gen_hand_oam.py) present in THIS build tree right now? Same technique
 * rom_chrome_gate.h uses for the card/pokeblock screens (__has_include on the
 * generated file itself), applied to the glove.
 *
 * WHY THIS MATTERS: box_oam.c's hand-tile upload code must be UNREACHABLE, not just
 * unused, when the real compiled poses are linked in — the Makefile globs every
 * source/ .c file unconditionally with no --gc-sections (rom_chrome_gate.h's own
 * comment explains why that matters: any code that COMPILES ends up in the .gba
 * whether it is called or not), but wrapping the ROM-glove branch in
 * `#if !PDNA_HAND_ART_COMPILED` makes the preprocessor delete it before compilation
 * — box_oam.o comes out byte-for-byte the same as before this rung existed whenever
 * the real art is present, which is the "full-art build is bit-identical" bar.
 *
 * A `-D` on the compile line wins over the __has_include probe, same as
 * rom_chrome_gate.h, so a host test can force either state regardless of what this
 * machine's source/ tree happens to have staged locally.
 */
#ifndef PDNA_HAND_ART_COMPILED
#if defined(__has_include) && __has_include("hand_oam.c")
#define PDNA_HAND_ART_COMPILED 1
#else
#define PDNA_HAND_ART_COMPILED 0
#endif
#endif

#endif /* HAND_GATE_H */
