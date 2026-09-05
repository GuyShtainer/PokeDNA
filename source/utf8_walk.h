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
 * function (there is nothing before offset 0).
 *
 * On a MALFORMED string the result can fail to be a boundary u8w_next would ever
 * produce walking forward from 0 -- e.g. deleting the 'A' out of "{5AD}" (not a
 * real escape: three characters between the braces) leaves "{5D}", which IS a real
 * escape, so the caret this function hands back now sits mid-glyph in the new
 * string; deleting the 'A' out of C3 41 A9 (an orphaned UTF-8 lead byte followed by
 * a stray continuation byte) leaves C3 A9, a valid e-acute, with the same effect.
 * Both require a malformed sequence no producer in this tree emits --
 * gen3_decode_char/gb_name_decode only ever produce well-formed "{XX}" escapes and
 * complete UTF-8 sequences, FatFs's UTF-8 LFN decode does the same, and the OSK's
 * own key grid has no '{', '}', or non-ASCII key to type one by hand -- so build no
 * stronger invariant on this function's result than "in bounds, forward progress
 * made"; tests/host_osk_test.c documents both repros without asserting boundary-ness
 * for them. */
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
 * `*new_len` (may be NULL). On a malformed `buf`, the returned position can land
 * mid-glyph of the SHORTENED string -- see u8w_prev's comment; unreachable from
 * any real producer. */
int u8w_delete_before(char* buf, int len, int pos, int* new_len);

/* Copy `src` (NUL-terminated) into `dst` (capacity `cap` bytes; cap should be
 * >= 1, but cap <= 0 or a NULL `src` still terminates `dst` at dst[0] rather than
 * handing back an uninitialised string -- the one exception is `dst` itself being
 * NULL, which this function cannot write through at all), stopping at the last
 * WHOLE glyph that fits within cap-1 bytes -- never splits a UTF-8 sequence or a
 * "{XX}" escape the way a plain `strncpy`-style byte cap would. Always NUL-
 * terminates `dst` when `dst` is non-NULL. Returns the number of bytes written
 * (excluding the NUL), i.e. the resulting strlen(dst). Used both to seed the
 * OSK's edit buffer from a caller's `initial` string and to copy the result back
 * out on commit, so an untouched name that happens to end exactly at a byte-cap
 * boundary keeps its last glyph intact instead of losing its trailing half. */
int u8w_copy_capped(char* dst, int cap, const char* src);

/* ---- the OSK's own key dispatch, factored out of osk.c so the exact state
 * transition each content-changing key performs is host-testable without tonc
 * (osk.c's other keys -- the cursor-grid D-pad, START, SELECT -- never touch
 * buf/len/cpos and have no op here). ---- */
typedef enum {
  U8W_OP_INSERT,  /* KEY_A: insert byte `c` at the caret                        */
  U8W_OP_DELETE,  /* KEY_B: delete the glyph before the caret                   */
  U8W_OP_LEFT,    /* KEY_L: caret to the start of the previous glyph            */
  U8W_OP_RIGHT,   /* KEY_R: caret to the start of the next glyph                */
} U8wOp;

/* Apply one edit key to (*buf, *len, *cpos) in place -- byte-for-byte the same
 * transition osk_core's A/B/L/R branches perform, so tests/host_osk_test.c drives
 * the identical logic that ships rather than a second copy of it. `c` is used
 * only for U8W_OP_INSERT. `maxlen` is osk.c's own internal scratch-buffer cap
 * (OSK_MAXLEN); `cap` is the caller's byte-buffer capacity, already clamped by
 * osk.c to `buf`'s real size before this is called. A key that cannot apply (the
 * buffer is full, the caret is already at an end) leaves everything unchanged. */
void u8w_apply_key(char* buf, int* len, int* cpos, int maxlen, int cap, U8wOp op, char c);

#endif /* UTF8_WALK_H */
