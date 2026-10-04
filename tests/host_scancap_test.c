/* Host test for source/scan_cap.h (BACKLOG #411): the file picker's FULL flag must mean
 * "an entry past the cap exists", not "the cap was reached". Drives the same loop shape
 * scan_dir() uses (admit-or-stop per filtered entry) over folders of cap-1, cap and cap+1
 * entries, plus a cap+3 folder and a folder with filtered-out extras past the cap.
 *
 *   cc -std=c11 -O2 -Wall -Wextra -I source tests/host_scancap_test.c -o /tmp/hscap && /tmp/hscap
 */
#include <stdio.h>
#include "scan_cap.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* n listing entries, of which the first `pass` pass the filter; returns count, sets *more. */
static int scan(int n, int pass, int cap, int* more) {
  int count = 0;
  *more = 0;
  for (int i = 0; i < n; i++) {
    if (i >= pass) continue;                       /* filtered out: never reaches the cap test */
    if (!scan_cap_admit(count, cap, more)) break;  /* the lookahead */
    count++;
  }
  return count;
}

int main(void) {
  int more, n;
  const int cap = 107;
  n = scan(cap - 1, cap - 1, cap, &more); CHECK(n == cap - 1 && !more);
  n = scan(cap,     cap,     cap, &more); CHECK(n == cap     && !more);   /* the #411 case */
  n = scan(cap + 1, cap + 1, cap, &more); CHECK(n == cap     && more);
  n = scan(cap + 3, cap + 3, cap, &more); CHECK(n == cap     && more);
  n = scan(cap + 5, cap,     cap, &more); CHECK(n == cap     && !more);   /* extras are filtered out */
  n = scan(0, 0, cap, &more);             CHECK(n == 0       && !more);
  n = scan(256, 256, 256, &more);         CHECK(n == 256     && !more);
  n = scan(257, 257, 256, &more);         CHECK(n == 256     && more);
  if (fails) { printf("host_scancap_test: %d FAILED\n", fails); return 1; }
  printf("host_scancap_test: all cases passed\n");
  return 0;
}
