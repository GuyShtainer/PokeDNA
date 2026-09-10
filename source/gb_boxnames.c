#include "gb_boxnames.h"

#include <string.h>

bool gbbn_supported(const GbSession* s) {
  return s && s->open && s->gen == GB_GEN2;
}

bool gbbn_read(const GbSession* s, int box, char* out, int cap) {
  if (out && cap > 0) out[0] = 0;
  if (!gbbn_supported(s) || !out || cap <= 0) return false;
  if (box < 0 || box >= G2_NUM_BOXES) return false;
  return g2_box_name_at(s->img, &s->g2w.sv, box, out, cap);
}

/* G2WStatus -> GbsStatus, same mapping convention gb_session.c's own status-translate
 * sites use (see that file's g2w-status switches). */
static GbsStatus map_g2w(G2WStatus st) {
  switch (st) {
    case G2W_OK:          return GBS_OK;
    case G2W_ERR_ARG:     return GBS_ERR_ARG;
    case G2W_ERR_TEXT:    return GBS_ERR_ARG;
    case G2W_ERR_STATE:   return GBS_ERR_ARG;
    case G2W_ERR_VERSION: return GBS_ERR_ARG;
    case G2W_ERR_UNSUPPORTED: return GBS_ERR_ARG;
    case G2W_ERR_VERIFY:  return GBS_ERR_VERIFY;
    default:              return GBS_ERR_ENGINE;
  }
}

GbsStatus gbbn_rename(GbSession* s, int box, const char* utf8) {
  if (!gbbn_supported(s) || !utf8) return GBS_ERR_ARG;
  if (box < 0 || box >= G2_NUM_BOXES) return GBS_ERR_ARG;

  char cur[GB_TEXT_MAX];
  if (g2_box_name_at(s->img, &s->g2w.sv, box, cur, sizeof cur) && strcmp(cur, utf8) == 0)
    return GBS_OK;   /* unchanged: never rewrite, never touch the checksums */

  return map_g2w(g2w_set_box_name(&s->g2w, box, utf8));
}
