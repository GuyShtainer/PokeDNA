#include "flags_fold.h"

void ff_build_ord(const NamedFlag* nf, int nc, uint8_t* ord, int ord_cap) {
  if (!nf || !ord || ord_cap <= 0) return;
  int o = -1;
  int lim = (nc < ord_cap) ? nc : ord_cap;
  for (int i = 0; i < lim; i++) {
    if (nf[i].num == NAMED_FLAG_HEADER) o++;
    ord[i] = (uint8_t)(o < 0 ? 0 : (o >= FF_MAX_GROUPS ? FF_MAX_GROUPS - 1 : o));
  }
}

int ff_hdr_ord(const uint8_t* ord, int ord_cap, int r) {
  if (!ord || r < 0 || r >= ord_cap) return 0;
  /* ord[] is caller-supplied (flags_fold.h): clamp here too, not only in
   * ff_build_ord, so a shift by it can never exceed the 32-bit mask (review D5). */
  return (ord[r] >= FF_MAX_GROUPS) ? FF_MAX_GROUPS - 1 : ord[r];
}

bool ff_row_visible(const NamedFlag* nf, int nc, const uint8_t* ord, int ord_cap,
                     uint32_t folded, int r) {
  if (!nf || r >= nc || nf[r].num == NAMED_FLAG_HEADER) return true;
  return !((folded >> ff_hdr_ord(ord, ord_cap, r)) & 1u);
}

int ff_step(const NamedFlag* nf, int nc, const uint8_t* ord, int ord_cap,
            uint32_t folded, int total, int r, int dir) {
  if (dir == 0) return r;
  for (int i = r + dir; i >= 0 && i < total; i += dir)
    if (ff_row_visible(nf, nc, ord, ord_cap, folded, i)) return i;
  return r;   /* top/bottom stop: no visible row further in this direction */
}
