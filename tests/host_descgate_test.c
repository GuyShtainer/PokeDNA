/* Host (PC) test for desc_gate.h / data_desc_shim.c -- the compile-time gate BACKLOG
 * #19's last item added (docs/AUDIT-2026-09-05-backlog-3-19.md section B) so an
 * artless PokeDNA build ships zero of the 741 verbatim Game Freak item/move/ability
 * description strings. -DPDNA_DESC_TEXT_COMPILED=0 forces data_desc_shim.c's stand-in
 * accessors to compile regardless of whether this machine's source/ tree happens to
 * have the generated (git-ignored) data_desc.c staged right now -- see desc_gate.h.
 *
 * Build + run (from the repo root):
 *   cc -std=c11 -I source -DPDNA_DESC_TEXT_COMPILED=0 \
 *      tests/host_descgate_test.c source/data_desc_shim.c \
 *      -o /tmp/hdg && /tmp/hdg
 *
 * What it proves:
 *   1) with the gate forced off, pk_item_desc/pk_move_desc/pk_ability_desc are all
 *      DEFINED (this file links at all) and all return the SAME non-empty placeholder
 *      string (pdna_layout.h's PDNA_DESC_PLACEHOLDER) for a sample of in-range ids;
 *   2) the placeholder is never the empty string a caller could mistake for "id out of
 *      range" -- pdna_main.c's desc_or_fallback() only ever reaches these when no ROM
 *      answered, so an empty return there would silently look like nothing is wrong;
 *   3) an out-of-range id still returns the SAME placeholder (no crash, no special
 *      case) -- the shim ignores `id` entirely, which this asserts rather than assumes.
 * host_textfit_test.c separately proves PDNA_DESC_PLACEHOLDER fits the tightest
 * single-line render budget it is ever drawn into; this file does not re-measure text.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "data_tables.h"
#include "pdna_layout.h"

static int fails = 0, checks = 0;
static void chk(const char* what, int cond) {
  checks++;
  if (!cond) { printf("FAIL: %s\n", what); fails++; }
}

int main(void) {
  const uint16_t sample_ids[] = { 0, 1, 13, 78, 200, 354, 355, 376, 377, 9999 };
  const size_t n = sizeof(sample_ids) / sizeof(sample_ids[0]);

  chk("PDNA_DESC_PLACEHOLDER is non-empty", PDNA_DESC_PLACEHOLDER[0] != '\0');

  for (size_t k = 0; k < n; k++) {
    uint16_t id = sample_ids[k];
    char label[64];

    snprintf(label, sizeof label, "pk_item_desc(%u) == placeholder", (unsigned)id);
    chk(label, strcmp(pk_item_desc(id), PDNA_DESC_PLACEHOLDER) == 0);

    snprintf(label, sizeof label, "pk_move_desc(%u) == placeholder", (unsigned)id);
    chk(label, strcmp(pk_move_desc(id), PDNA_DESC_PLACEHOLDER) == 0);

    snprintf(label, sizeof label, "pk_ability_desc(%u) == placeholder", (unsigned)id);
    chk(label, strcmp(pk_ability_desc(id), PDNA_DESC_PLACEHOLDER) == 0);
  }

  printf("%s: %d/%d checks passed\n", fails ? "FAIL" : "PASS", checks - fails, checks);
  return fails ? 1 : 0;
}
