#include "utf8_walk.h"

#include <string.h>

static int is_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

/* True if s[i..i+3] spell a "{XX}" hex escape, i.e. s[i]=='{', two hex digits, '}'.
 * Reads s[i+1] only after confirming s[i]=='{' (so s[i] itself is non-NUL and s[i+1]
 * is therefore in bounds), and each further byte only after the previous one tested
 * non-NUL -- so this never reads past the string's own terminator. */
static int is_escape_at(const char* s, int i) {
  if (s[i] != '{') return 0;
  char c1 = s[i + 1];
  if (!c1 || !is_hex(c1)) return 0;
  char c2 = s[i + 2];
  if (!c2 || !is_hex(c2)) return 0;
  return s[i + 3] == '}';
}

int u8w_next(const char* s, int i) {
  if (!s || i < 0 || s[i] == 0) return i;   /* nothing to advance over */

  if (is_escape_at(s, i)) return i + 4;

  unsigned char c = (unsigned char)s[i];
  int n;                                    /* expected sequence length from the lead byte */
  if      ((c & 0x80) == 0x00) n = 1;       /* ASCII                                        */
  else if ((c & 0xE0) == 0xC0) n = 2;       /* 2-byte lead                                  */
  else if ((c & 0xF0) == 0xE0) n = 3;       /* 3-byte lead (the Male/Female signs are here)  */
  else if ((c & 0xF8) == 0xF0) n = 4;       /* 4-byte lead                                  */
  else n = 1;                               /* stray continuation byte or invalid lead      */

  int j = i + 1, got = 1;
  while (got < n && s[j] && ((unsigned char)s[j] & 0xC0) == 0x80) { j++; got++; }
  return j;                                 /* a truncated/malformed sequence yields < n    */
}

int u8w_prev(const char* s, int i) {
  if (!s || i <= 0) return 0;

  if (i >= 4 && s[i - 1] == '}' && s[i - 4] == '{' &&
      is_hex(s[i - 3]) && is_hex(s[i - 2]))
    return i - 4;

  int j = i - 1, back = 0;
  while (j > 0 && back < 3 && ((unsigned char)s[j] & 0xC0) == 0x80) { j--; back++; }
  return j;
}

int u8w_count(const char* s) {
  if (!s) return 0;
  int n = 0;
  int guard = (int)strlen(s) + 1;           /* provable loop bound (golden rule 2) */
  for (int i = 0; s[i] && guard > 0; guard--) {
    i = u8w_next(s, i);
    n++;
  }
  return n;
}

int u8w_len_bytes(const char* s, int i) {
  if (!s) return 0;
  return u8w_next(s, i) - i;
}

int u8w_insert_byte(char* buf, int len, int cap, int pos, char c) {
  if (!buf || len < 0 || pos < 0 || pos > len || cap <= 0) return -1;
  if (len >= cap - 1) return -1;            /* no room left for one more content byte */
  for (int j = len; j > pos; j--) buf[j] = buf[j - 1];
  buf[pos] = c;
  buf[len + 1] = 0;
  return len + 1;
}

int u8w_delete_before(char* buf, int len, int pos, int* new_len) {
  if (!buf || len < 0 || pos <= 0 || pos > len) {
    if (new_len) *new_len = len < 0 ? 0 : len;
    return -1;
  }
  int start = u8w_prev(buf, pos);
  int n = pos - start;
  for (int j = start; j <= len - n; j++) buf[j] = buf[j + n];
  if (new_len) *new_len = len - n;
  return start;
}

int u8w_copy_capped(char* dst, int cap, const char* src) {
  if (!dst || cap <= 0) return 0;
  if (!src || cap == 1) { if (cap >= 1) dst[0] = 0; return 0; }

  int n = 0;
  int guard = (int)strlen(src) + 1;         /* provable loop bound (golden rule 2) */
  for (int i = 0; src[i] && guard > 0; guard--) {
    int j = u8w_next(src, i);
    if (j > cap - 1) break;                 /* this whole glyph would not fit -- stop short */
    for (int k = i; k < j; k++) dst[k] = src[k];
    n = j;
    i = j;
  }
  dst[n] = 0;
  return n;
}
