#include "gb_boxnames.h"

#include <string.h>

#include "gb_edit.h"   /* gb_name_decode/gb_name_encode/gb_char_encode/gb_text_lossy,
                        * GB_TEXT_MAX/GB_GLYPH_MAX */

bool gbbn_supported(const GbSession* s) {
  return s && s->open && s->gen == GB_GEN2;
}

static GbGame bn_game(const GbSession* s) {
  return (s->g2w.sv.version == G2_VER_CRYSTAL) ? GBF_G_CRYSTAL : GBF_G_GS;
}

bool gbbn_read(const GbSession* s, int box, char* out, int cap) {
  if (out && cap > 0) out[0] = 0;
  if (!gbbn_supported(s) || !out || cap <= 0) return false;
  if (box < 0 || box >= G2_NUM_BOXES) return false;

  GbGame g = bn_game(s);
  uint32_t base = gbf_off(g, GBF_BOXNAMES);
  if (!base) return false;
  uint32_t off = base + (uint32_t)box * GB_BOXNAME_BYTES;

  uint8_t raw[GB_BOXNAME_BYTES];
  GbSession* ncs = (GbSession*)(const void*)s;
  if (gbs_read_field(ncs, off, raw, GB_BOXNAME_BYTES) != GBS_OK) return false;
  gb_name_decode(GB_GEN2, out, cap, raw, GB_BOXNAME_BYTES);
  return true;
}

/* Same "count what the encoder actually spells" shape as gb_trainer.c's own
 * (static) count_glyphs -- gb_text_lossy only judges CHARSET loss within the first
 * `max_glyphs` glyphs, it does not itself refuse a string that has more glyphs than
 * that, so the length check has to happen separately, the same way gb_trainer.c's own
 * set_name() does it. */
static int count_glyphs(uint8_t gen, const char* s) {
  int n = 0;
  uint8_t out;
  while (*s) {
    int c = gb_char_encode(gen, s, &out);
    if (c <= 0) break;
    s += c;
    n++;
  }
  return n;
}

GbsStatus gbbn_rename(GbSession* s, int box, const char* utf8) {
  if (!gbbn_supported(s) || !utf8) return GBS_ERR_ARG;
  if (box < 0 || box >= G2_NUM_BOXES) return GBS_ERR_ARG;

  /* P1a review D5: refuse empty / all-space outright -- retail's own _InitString
   * restores the OLD name rather than ever storing a blank one (home/string.asm), so
   * silently accepting "" here would diverge from what the game itself does. */
  const char* p = utf8;
  while (*p == ' ') p++;
  if (*p == '\0') return GBS_ERR_ARG;

  if (count_glyphs(GB_GEN2, utf8) > GB_BOXNAME_GLYPHS) return GBS_ERR_ARG;
  if (gb_text_lossy(GB_GEN2, utf8, GB_BOXNAME_GLYPHS, NULL) > 0) return GBS_ERR_ARG;

  GbGame g = bn_game(s);
  uint32_t base = gbf_off(g, GBF_BOXNAMES);
  if (!base) return GBS_ERR_ARG;
  uint32_t off = base + (uint32_t)box * GB_BOXNAME_BYTES;

  /* UNCHANGED IS UNTOUCHED: decode with the SEQUENCE-SAFE gb_name_decode (not the lossy
   * display decoder gen2_save.c's g2_box_name_at used to route through) so a name that
   * already round-trips through the escape mechanism is correctly recognised as
   * unchanged, never rewritten (and never has its checksums refreshed) for nothing. */
  uint8_t cur_raw[GB_BOXNAME_BYTES];
  if (gbs_read_field(s, off, cur_raw, GB_BOXNAME_BYTES) == GBS_OK) {
    char cur[GB_TEXT_MAX];
    gb_name_decode(GB_GEN2, cur, sizeof cur, cur_raw, GB_BOXNAME_BYTES);
    if (strcmp(cur, utf8) == 0) return GBS_OK;
  }

  uint8_t enc[GB_BOXNAME_BYTES];
  gb_name_encode(GB_GEN2, enc, GB_BOXNAME_BYTES, GB_BOXNAME_GLYPHS, utf8);
  GbsStatus st = gbs_write_field(s, off, enc, GB_BOXNAME_BYTES);
  if (st != GBS_OK) return st;
  return gbs_finish(s);
}
