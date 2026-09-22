/* See ui_ascii.h for the contract and why this exists (BACKLOG #222). */
#include "ui_ascii.h"

unsigned ui_ascii_next(const char** ps) {
  const unsigned char* p = (const unsigned char*)*ps;
  unsigned c = *p++;
  if (c == 0xC3u && *p == 0xA9u) { c = 127u; p++; }
  else if (c >= 0x80u) {
    while ((*p & 0xC0u) == 0x80u) p++;
    c = (unsigned)'?';
  }
  *ps = (const char*)p;
  return c;
}

size_t ui_ascii_bound(char* out, size_t outcap, const char* in) {
  if (out == NULL || outcap == 0) return 0;
  if (in == NULL) { out[0] = '\0'; return 0; }
  size_t o = 0;
  const char* p = in;
  while (*p != '\0' && o + 1 < outcap) {
    unsigned c = ui_ascii_next(&p);
    out[o++] = (char)c;
  }
  out[o] = '\0';
  return o;
}
