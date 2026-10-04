#ifndef SCAN_CAP_H
#define SCAN_CAP_H
/*
 * scan_cap -- BACKLOG #411's one-entry lookahead for the file-picker directory scan.
 *
 * scan_dir() (pdna_main.c) admits at most `cap` entries. The old loop stopped when the count
 * reached the cap, so a folder holding EXACTLY cap entries looked the same as one holding more
 * and the status line said FULL for both. Now the loop asks scan_cap_admit() once per entry that
 * PASSED the filters: while there is room it admits; the first passing entry PAST the cap sets
 * *more and is refused. FULL is shown only when *more is set (something really is hidden).
 *
 * Pure C, header-only; tests/host_scancap_test.c includes this exact file.
 */
/* Returns 1 when the entry fits (count < cap); otherwise sets *more = 1 and returns 0. */
static inline int scan_cap_admit(int count, int cap, int* more) {
  if (count < cap) return 1;
  *more = 1;
  return 0;
}
#endif
