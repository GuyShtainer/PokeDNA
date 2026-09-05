/* Stands in for source/data_desc.c's pk_item_desc/pk_move_desc/pk_ability_desc when
 * that file is excluded from the build -- an artless build (PDNA_ARTLESS=1 forces
 * -DPDNA_DESC_TEXT_COMPILED=0, Makefile) or any other tree where the generated,
 * git-ignored data_desc.c has never been produced by tools/gen_data.py. Same shape as
 * art_fallbacks.c standing in for mon_icons.c/hand_oam.c/etc., except those use
 * ordinary weak-symbol override (both files always compile) while these three
 * accessors are the desc_gate.h family's own idiom: THIS file is ALWAYS in CFILES
 * (never filtered out, unlike data_desc.c), and its body compiles to an EMPTY
 * translation unit whenever data_desc.c's real, verbatim-string definitions are the
 * ones actually linked in -- see desc_gate.h and rom_chrome_gate.h's own comment on
 * why an empty TU (not a weak symbol) is the mechanism here: a plain function
 * definition here would collide with data_desc.c's at link time, since neither is
 * declared weak.
 *
 * Runtime behaviour: pdna_main.c's desc_or_fallback() tries the user's own ROM first
 * (rom_text_have/rom_text_get) and only reaches these when no ROM is registered this
 * session, or the ROM doesn't have the requested text kind (docs/AUDIT-2026-09-05-
 * backlog-3-19.md section B) -- so an artless build with a ROM attached still shows
 * the real description, read live off the cartridge; PDNA_DESC_PLACEHOLDER
 * (pdna_layout.h) is only ever seen with no ROM registered. */
#include "desc_gate.h"

#if !PDNA_DESC_TEXT_COMPILED
#include "data_tables.h"
#include "pdna_layout.h"   /* PDNA_DESC_PLACEHOLDER */

const char* pk_item_desc(uint16_t id)    { (void)id; return PDNA_DESC_PLACEHOLDER; }
const char* pk_move_desc(uint16_t id)    { (void)id; return PDNA_DESC_PLACEHOLDER; }
const char* pk_ability_desc(uint16_t id) { (void)id; return PDNA_DESC_PLACEHOLDER; }
#endif /* !PDNA_DESC_TEXT_COMPILED */
