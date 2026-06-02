#ifndef OSK_H
#define OSK_H

#include <stdbool.h>

/* On-screen keyboard for Gen-3 names (adapted from the sd-browser OSK). Only
 * Gen-3-encodable characters are offered (digits / A-Z / a-z / space / -.,'!?),
 * so anything typed round-trips through gen3_encode_char. Returns true on START
 * (commit) with the text in `out` (NUL-terminated, <= cap-1), false on SELECT
 * (cancel). D-pad moves the key grid; L/R move the text caret; A inserts; B deletes. */
bool osk_input(const char* prompt, const char* initial, char* out, int cap);

#endif /* OSK_H */
