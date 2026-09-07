/* gb_fields_fallback.c — weak defaults so a clone that never ran
 * tools/gen_gbfields.py still LINKS (BACKLOG #49 P0). Same mechanism as
 * source/art_fallbacks.c: these four accessors are __attribute__((weak)), so the
 * REAL, generated source/gb_fields.c's strong definitions win whenever that
 * git-ignored file is present and compiled; when it is not, every field reports
 * "the game lacks it" (0 offset, 0 length, GBFK_U8, "?") instead of a missing-symbol
 * link error. Not one caller needs to know which pair actually linked.
 *
 * Committed (tracked): this is the fallback itself, not the generator's output.
 */
#include "gb_fields.h"

__attribute__((weak)) uint32_t gbf_off(GbGame g, GbField f) {
  (void)g; (void)f;
  return 0;
}

__attribute__((weak)) uint16_t gbf_len(GbGame g, GbField f) {
  (void)g; (void)f;
  return 0;
}

__attribute__((weak)) GbFieldKind gbf_kind(GbGame g, GbField f) {
  (void)g; (void)f;
  return GBFK_U8;
}

__attribute__((weak)) const char* gbf_field_name(GbField f) {
  (void)f;
  return "?";
}
