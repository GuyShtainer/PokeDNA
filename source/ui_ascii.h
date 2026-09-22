#ifndef UI_ASCII_H
#define UI_ASCII_H
/* Pure-C UTF-8-to-fixed-glyph collapse (BACKLOG #222). No tonc/GBA headers -- dual-
 * compiles into tests/host_ui_ascii_test.c on the PC exactly as it runs on the cart.
 * Shared by source/ui.c's proportional-font walker (pnext(), unchanged behaviour) AND
 * the new bounded ui_text()/ui_text_sel() buffer, so both text paths collapse
 * non-ASCII the same way instead of drifting apart. */
#include <stddef.h>

/* Decode one glyph from *ps, advancing *ps past the bytes it consumed. ASCII (< 0x80)
 * passes through unchanged. The two-byte UTF-8 sequence C3 A9 (e-acute -- the games
 * spell it "POKeMON" with an accent) collapses to code 127: both ui_font.c's
 * proportional face and libtonc's sys8Font carry a real glyph there (tonc_tte.h:339,
 * "System font ' '-127", charOffset=32/charCount=96 -> glyph id 95 is the font's LAST
 * valid cell, not past it). Every other lead byte >= 0x80 collapses to '?' with its
 * UTF-8 continuation bytes (10xxxxxx) skipped, so a corrupt or truncated string can
 * never desynchronise the walk or read past its own NUL. */
unsigned ui_ascii_next(const char** ps);

/* Copy `in` into `out` (capacity outcap, outcap >= 1) through ui_ascii_next() so every
 * byte written to `out` is < 0x80 and safe for libtonc's tte_write() / the unchecked
 * tte_get_glyph_id() (BACKLOG #222: charCount has no bound check there, so a raw UTF-8
 * lead byte >= 0x80 reaching tte_write reads an out-of-range glyph cell). Always NUL-
 * terminates within outcap; truncates at outcap-1 bytes without ever cutting a
 * multi-byte source sequence in half (each loop step consumes one whole glyph or
 * none). Returns the number of bytes written, excluding the NUL. out==NULL/outcap==0
 * is a no-op (returns 0); in==NULL yields an empty (NUL-only) `out`. */
size_t ui_ascii_bound(char* out, size_t outcap, const char* in);

#endif
