/* gb1_base_fallback.c -- weak default so a clone that never ran tools/gen_gb1_base.py
 * still LINKS (BACKLOG #276). Same mechanism as gb_fields_fallback.c: the REAL,
 * generated source/gb1_base_gen.c's strong definition wins whenever that git-ignored
 * file is present; otherwise the table reports "absent" and CREATE > FROM SCRATCH on a
 * Gen-1 save falls back to reading the base row from the registered ROM.
 * Committed (tracked): this is the fallback itself, not the generator's output. */
#include "gb1_base_tbl.h"

__attribute__((weak)) bool gb1_base_gen_row(uint16_t dex, uint8_t out[GB1_BASE_ROW_LEN]) {
  (void)dex; (void)out;
  return false;
}
