#ifndef PDNA_BUILD_VARIANT_H
#define PDNA_BUILD_VARIANT_H
/* BACKLOG #255: the build identifies itself. See build_variant.c for why the accessor
 * exists -- gba.specs bakes --gc-sections into every link, and an orphan data-only
 * translation unit with nothing calling into it is exactly the kind of unreferenced
 * section that flag exists to strip; __attribute__((used)) alone did not survive it
 * (proven empirically: grep -c on the built .gba found zero copies of the marker until
 * a real call site was added). The accessor gives the linker a live relocation from an
 * always-called .text section into build_variant.o's .rodata, which keeps both. */
const char *pdna_build_variant_str(void);
#endif
