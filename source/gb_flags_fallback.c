/* gb_flags_fallback.c — weak defaults so a clone that never ran
 * tools/gen_gbfields.py still LINKS (BACKLOG #49 P0). Same weak/strong mechanism as
 * gb_fields_fallback.c (see that file's header) and source/art_fallbacks.c: the REAL,
 * generated source/gb_flags.c's strong definitions win whenever present; when it is
 * not, every game reports zero named flags rather than a missing-symbol link error.
 *
 * Committed (tracked): this is the fallback itself, not the generator's output.
 */
#include "gb_flags.h"

__attribute__((weak)) int gbfl_count(GbGame g) {
  (void)g;
  return 0;
}

__attribute__((weak)) const char* gbfl_name(GbGame g, uint16_t flag) {
  (void)g; (void)flag;
  return 0;
}

__attribute__((weak)) bool gbfl_at(GbGame g, int i, uint16_t* flag_out, const char** label_out) {
  (void)g; (void)i; (void)flag_out; (void)label_out;
  return false;
}
