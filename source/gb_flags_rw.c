#include "gb_flags_rw.h"
#include "gb_edit.h"    /* GB_GEN1/GB_GEN2 (s->gen) */

static bool region_off_len(const GbSession* s, GbGame g, uint32_t* off, uint16_t* len) {
  GbField f = (s->gen == GB_GEN1) ? GBF_EVENT_FLAGS_BASE : GBF_EVENT_FLAGS_BASE_G2;
  uint32_t o = gbf_off(g, f);
  uint16_t l = gbf_len(g, f);
  if (!o || !l) return false;
  *off = o; *len = l;
  return true;
}

bool gbfl_get(const GbSession* s, GbGame g, uint16_t n) {
  if (!s || !s->open) return false;
  uint32_t base; uint16_t len;
  if (!region_off_len(s, g, &base, &len)) return false;
  if ((uint32_t)n >= (uint32_t)len * 8u) return false;
  uint8_t byte = 0;
  /* gbs_read_field takes a non-const GbSession* -- this cast is safe: the read path
   * never mutates the session, same posture pk_flag_get's own const sb1 pointer has
   * on the Gen-3 side. */
  if (gbs_read_field((GbSession*)s, base + (n >> 3), &byte, 1) != GBS_OK) return false;
  return (byte >> (n & 7)) & 1u;
}

GbsStatus gbfl_set(GbSession* s, GbGame g, uint16_t n, bool v) {
  if (!s || !s->open) return GBS_ERR_ARG;
  uint32_t base; uint16_t len;
  if (!region_off_len(s, g, &base, &len)) return GBS_ERR_ARG;
  if ((uint32_t)n >= (uint32_t)len * 8u) return GBS_ERR_ARG;

  uint32_t off = base + (n >> 3);
  uint8_t byte = 0;
  GbsStatus st = gbs_read_field(s, off, &byte, 1);
  if (st != GBS_OK) return st;

  uint8_t bit = (uint8_t)(1u << (n & 7));
  bool cur = (byte & bit) != 0;
  if (cur == v) return GBS_OK;   /* hard rule 3: a no-op writes nothing */

  uint8_t nb = v ? (uint8_t)(byte | bit) : (uint8_t)(byte & ~bit);
  return gbs_write_field(s, off, &nb, 1);
}
