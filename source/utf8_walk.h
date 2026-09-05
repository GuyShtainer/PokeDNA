#ifndef UTF8_WALK_H
#define UTF8_WALK_H

/* Glyph-aware walking over a NUL-terminated text buffer that mixes three things a
 * PokeDNA name field can hold (BACKLOG #38):
 *
 *   - plain ASCII                                    1 byte  = 1 glyph
 *   - UTF-8 (e.g. e-acute, the Male/Female signs Guy hits on real NIDORAN names)
 *                                                     2-4 bytes = 1 glyph
 *   - a gb_name_decode() hex escape, "{5D}"           4 bytes = 1 glyph, literal braces
 *
 * osk.c used to walk such a buffer one BYTE at a time (render, caret, backspace), so
 * a multi-byte glyph showed as mojibake and a single backspace could split it, after
 * which the orphaned half is not valid UTF-8 and not a valid escape either. Every
 * function here treats one of the three as an indivisible unit; osk.c is the only
 * caller, but this module is pure C (no GBA headers, no dynamic allocation, no
 * static/global state) so tests/host_osk_test.c can drive it directly on the host.
 *
 * A malformed sequence (a lone continuation byte, a "{" with no closing brace) is
 * never trusted to describe its own length: every function here still makes forward
 * (u8w_next) or backward (u8w_prev) progress of at least one byte and never reads
 * past the string's NUL, so a hostile or corrupt buffer can degrade the display but
 * cannot hang the OSK or run off the buffer. */

/* Byte offset of the glyph AFTER the one starting at `i`. `s[i]` must not be the NUL
 * terminator (i.e. 0 <= i < strlen(s)); the caller loops on `s[i]` to know when to
 * stop, exactly like advancing a plain byte index. Handles, in order: the "{XX}" hex
 * escape (exactly two hex digits, brace to brace); a UTF-8 lead byte (1/2/3/4-byte
 * sequence, consuming only the continuation bytes actually present -- a truncated or
 * malformed sequence yields a shorter glyph rather than overrunning the string); a
 * lone continuation byte or any other byte, one glyph of one byte. */
int u8w_next(const char* s, int i);

/* Byte offset of the START of the glyph immediately before `i` (0 < i <= strlen(s)).
 * Mirrors u8w_next: recognizes a "{XX}" escape ending at `i` (checks the 4 bytes
 * s[i-4..i-1] spell exactly that), else walks back over up to 3 UTF-8 continuation
 * bytes (0x80-0xBF) to find the lead byte. i==0 is refused by the caller, not this
 * function (there is nothing before offset 0). */
int u8w_prev(const char* s, int i);

/* Number of glyphs in the NUL-terminated string `s` (0 for NULL or ""). */
int u8w_count(const char* s);

/* u8w_next(s, i) - i: how many bytes the glyph starting at `i` occupies. */
int u8w_len_bytes(const char* s, int i);

/* ---- edit-buffer operations, factored out of osk.c so they are host-testable on
 * their own (osk.c calls these exact functions -- there is no second copy of this
 * logic to drift out of sync with the tests). Both work on a plain C string `buf`
 * whose true capacity (bytes, INCLUDING the NUL) is `cap`; `len` is always the
 * caller's own strlen(buf) passed in rather than recomputed, since osk.c already
 * tracks it. ---- */

/* Insert one raw byte `c` at byte offset `pos` (0 <= pos <= len), shifting
 * buf[pos..len] right by one and re-terminating. `pos` is expected to sit on a
 * glyph boundary (osk.c's caret invariant), but this function does not itself
 * require that -- it only decides where the new byte lands, and it never touches a
 * byte outside 0..len, so an off-boundary `pos` cannot corrupt anything, only
 * misplace where the new character shows up. Returns the new length, or -1
 * (buf left unchanged) if there is no room for one more content byte before the
 * NUL, or if `pos`/`len`/`cap` are out of range. */
int u8w_insert_byte(char* buf, int len, int cap, int pos, char c);

/* Delete the glyph immediately before byte offset `pos` (`pos` must be a glyph
 * boundary and pos > 0; pos <= 0 is refused with -1 and `buf`/`*new_len`
 * unchanged). Removes 1 byte for a plain ASCII glyph, up to 4 for a UTF-8 or
 * escape glyph -- one B press, one glyph, regardless of its byte width. Returns
 * the new caret position (== the start of the glyph just removed, so callers can
 * assign it straight back to their caret variable) and writes the new length to
 * `*new_len` (may be NULL). */
int u8w_delete_before(char* buf, int len, int pos, int* new_len);

/* Copy `src` (NUL-terminated) into `dst` (capacity `cap` bytes, cap >= 1), stopping
 * at the last WHOLE glyph that fits within cap-1 bytes -- never splits a UTF-8
 * sequence or a "{XX}" escape the way a plain `strncpy`-style byte cap would.
 * Always NUL-terminates `dst`. Returns the number of bytes written (excluding the
 * NUL), i.e. the resulting strlen(dst). Used both to seed the OSK's edit buffer
 * from a caller's `initial` string and to copy the result back out on commit, so
 * an untouched name that happens to end exactly at a byte-cap boundary keeps its
 * last glyph intact instead of losing its trailing half. */
int u8w_copy_capped(char* dst, int cap, const char* src);

#endif /* UTF8_WALK_H */
