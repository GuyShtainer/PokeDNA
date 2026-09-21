#ifndef TAB_FOCUS_FOOTER_H
#define TAB_FOCUS_FOOTER_H

#include <stdbool.h>

/* BACKLOG #173 F2: pure decision for the box grid's tab-focus footer line,
 * factored out of pdna_box.c's draw_footer() (the `s_tab_focus >= 0` branch)
 * so tests/host_tabfocusfooter_test.c can pin it on the host. draw_footer()
 * itself stays GBA-UI (tonc.h, statics) and is not host-compilable; this is
 * the one line of logic in it worth a second copy to check against.
 *
 * Round-1 defect (review-sonnet, 2026-09-21): the original condition tested
 * the DISPLAYED screen's `is_bank`, not the carried item's origin, so lifting
 * a GB-origin mon then viewing it on a non-Bank screen showed the wrong
 * footer. The fix is `holding && carry_is_gb` — carry_is_gb is the caller's
 * pdna_box_carry_is_gb() (s_orig_scope == BOXSCOPE_GB), never is_bank. */
const char* tab_focus_footer(bool holding, bool carry_is_gb);

#endif /* TAB_FOCUS_FOOTER_H */
