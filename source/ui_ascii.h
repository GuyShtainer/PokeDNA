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
 * spell it "POKeMON" with an accent) collapses to code 127: ui_font.c's proportional
 * face carries a real glyph there (see source/ui_font.c's last entry). libtonc's
 * sys8Font does NOT -- its cell 127 is 8 bytes of 0x00 (blank), not a real glyph
 * (tonc_tte.h:339's "System font ' '-127" comment is misleading; the actual bitmap at
 * that offset is empty) -- so this variant, which collapses to 127, is for the
 * PROPORTIONAL-font path only (ui.c's pnext()). The FIXED-font path (libtonc's sys8,
 * used by ui_text()/ui_text_sel() via ui_ascii_bound()) must use ui_ascii_next_fixed()
 * below instead, which collapses e-acute to '?' like every other non-ASCII lead byte.
 * Every other lead byte >= 0x80 collapses to '?' with its UTF-8 continuation bytes
 * (10xxxxxx) skipped, so a corrupt or truncated string can never desynchronise the walk
 * or read past its own NUL. */
unsigned ui_ascii_next(const char** ps);

/* Same walk as ui_ascii_next(), but for the FIXED (libtonc sys8) font: e-acute (C3 A9)
 * collapses to '?' instead of 127, since sys8's cell 127 is blank (see ui_ascii_next's
 * comment above). Used by ui_ascii_bound() -- and therefore by ui.c's ui_text()/
 * ui_text_sel(), which draw through sys8 -- never by the proportional-font path. */
unsigned ui_ascii_next_fixed(const char** ps);

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
