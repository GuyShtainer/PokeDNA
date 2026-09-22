/* See ui_ascii.h for the contract and why this exists (BACKLOG #222). */
#include "ui_ascii.h"

/* Shared walk: decode one glyph from *ps, advancing *ps past the bytes it consumed.
 * `eacute_glyph` is the code the C3 A9 (e-acute) pair collapses to -- callers pick it
 * per target font (see ui_ascii_next / ui_ascii_next_fixed below). */
static unsigned ascii_next_impl(const char** ps, unsigned eacute_glyph) {
  const unsigned char* p = (const unsigned char*)*ps;
  unsigned c = *p++;
  if (c == 0xC3u && *p == 0xA9u) { c = eacute_glyph; p++; }
  else if (c >= 0x80u) {
    while ((*p & 0xC0u) == 0x80u) p++;
    c = (unsigned)'?';
  }
  *ps = (const char*)p;
  return c;
}

unsigned ui_ascii_next(const char** ps) {
  return ascii_next_impl(ps, 127u);
}

unsigned ui_ascii_next_fixed(const char** ps) {
  return ascii_next_impl(ps, (unsigned)'?');
}

size_t ui_ascii_bound(char* out, size_t outcap, const char* in) {
  if (out == NULL || outcap == 0) return 0;
  if (in == NULL) { out[0] = '\0'; return 0; }
  size_t o = 0;
  const char* p = in;
  while (*p != '\0' && o + 1 < outcap) {
    unsigned c = ui_ascii_next_fixed(&p);
    out[o++] = (char)c;
  }
  out[o] = '\0';
  return o;
}
