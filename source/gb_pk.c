#include <string.h>
#include "gb_pk.h"

int gb_pk_pack(const GbEditMon* e, uint8_t* out, int cap) {
  if (!e || !out) return -1;
  if (e->gen != GB_GEN1 && e->gen != GB_GEN2) return -1;

  int rec_len = gb_rec_size(e->gen, false);      /* box-shaped, never party's longer form */
  if (rec_len <= 0) return -1;
  int total = rec_len + GB_NAME_BYTES + GB_NAME_BYTES;
  if (cap < total) return -2;

  if (gb_is_egg(e)) return -3;                   /* decision 5: the file cannot carry it */

  memcpy(out,                       e->rec,    (size_t)rec_len);
  memcpy(out + rec_len,             e->otname, GB_NAME_BYTES);
  memcpy(out + rec_len + GB_NAME_BYTES, e->nick, GB_NAME_BYTES);
  return total;
}

bool gb_pk_unpack(const uint8_t* in, int len, uint8_t gen, GbEditMon* out) {
  if (!in || !out) return false;
  if (gen != GB_GEN1 && gen != GB_GEN2) return false;

  int rec_len = gb_rec_size(gen, false);
  if (rec_len <= 0) return false;
  int total = rec_len + GB_NAME_BYTES + GB_NAME_BYTES;
  if (len != total) return false;

  memset(out, 0, sizeof *out);
  const uint8_t* rec    = in;
  const uint8_t* otname = in + rec_len;
  const uint8_t* nick   = in + rec_len + GB_NAME_BYTES;
  /* rec[0] is the species byte in both generations' box-record layout -- reused as the
   * list byte so the unpacked record reads as itself, never as an Egg (see the header
   * comment: this file format has no way to recover the ORIGINAL list byte). */
  return gb_load_parts(out, gen, false, rec, otname, nick, rec[0]);
}

const char* gb_pk_ext(uint8_t gen) {
  if (gen == GB_GEN1) return ".pk1";
  if (gen == GB_GEN2) return ".pk2";
  return 0;
}
