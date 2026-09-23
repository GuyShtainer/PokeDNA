#include "build_variant.h"

/* A shot chain is handed a .gba and has no way to ask what it is. This is that way:
 * grep the image for the marker. BACKLOG #255 -- the #253 corpus was fourteen frames
 * of the wrong build, and nothing could tell.
 *
 * __attribute__((used)) alone does NOT survive this Makefile's link: gba.specs bakes
 * --gc-sections into every link (not visible in Makefile LDFLAGS -- it is inside
 * /opt/devkitpro/devkitARM/arm-none-eabi/lib/gba.specs), and this translation unit has
 * nothing else in it, so its whole .rodata section is an unreferenced section with no
 * live relocation into it -- exactly what --gc-sections exists to strip. Proven: before
 * pdna_build_variant_str() existed and was called from boot, `grep -c` on every one of
 * PokeDNA.gba / PokeDNA-artless.gba / pokedna-delta-artless.gba found ZERO copies of
 * "PDNA-VARIANT" -- the marker compiled into build_variant.o (confirmed with nm/objdump
 * on the .o) but never reached the linked .elf. The accessor below is called once from
 * pdna_main.c's boot log (source/pdna_main.c, main()), which is unconditionally live;
 * that gives the linker a real relocation from a live .text section into this file's
 * .rodata section, so --gc-sections keeps both. */
const char pdna_build_variant[] __attribute__((used)) =
#if defined(PDNA_ARTLESS) && PDNA_ARTLESS
    "PDNA-VARIANT:ARTLESS"
#else
    "PDNA-VARIANT:ART"
#endif
#ifdef PDNA_DELTA
    "+DELTA"
#endif
    ;

const char *pdna_build_variant_str(void) {
  return pdna_build_variant;
}
