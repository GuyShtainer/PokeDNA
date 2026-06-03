#ifndef OSK_H
#define OSK_H

#include <stdbool.h>

/* On-screen keyboard (QWERTY, like the sd-browser one). D-pad moves the key
 * grid, A inserts, B deletes (hold to clear), L/R move the text caret, START
 * commits, SELECT cancels. Returns true on commit with the text in `out`
 * (NUL-terminated, <= cap-1), false on cancel.
 *
 *  osk_input  -> for Gen-3 names: rejects an empty result.
 *  osk_search -> for live search filters: an EMPTY result is allowed (clears the
 *                filter), so you can wipe a query and start fresh. */
bool osk_input(const char* prompt, const char* initial, char* out, int cap);
bool osk_search(const char* prompt, const char* initial, char* out, int cap);

#endif /* OSK_H */
