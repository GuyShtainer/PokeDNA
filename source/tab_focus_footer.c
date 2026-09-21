#include "tab_focus_footer.h"
#include "pdna_layout.h"   /* PDNA_TAB_FOCUS_CARRY_FOOTER */

const char* tab_focus_footer(bool holding, bool carry_is_gb) {
  return (holding && carry_is_gb) ? PDNA_TAB_FOCUS_CARRY_FOOTER : "L/R tab  A pick  DN";
}
