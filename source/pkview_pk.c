#include "pkview_pk.h"

#include <tonc.h>
#include <stdio.h>
#include <string.h>
#include "ff.h"
#include "savefile.h"
#include "data_tables.h"
#include "ui.h"

/* keep only [A-Za-z0-9] from `in`; collapse runs of other chars to one '_'. */
static void sanitize(char* out, const char* in, int cap) {
  int o = 0;
  for (int i = 0; in[i] && o < cap - 1; i++) {
    char c = in[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[o++] = c;
    else if (o > 0 && out[o - 1] != '_') out[o++] = '_';
  }
  while (o > 0 && out[o - 1] == '_') o--;
  out[o] = 0;
  if (o == 0) { out[0] = 'M'; out[1] = 'O'; out[2] = 'N'; out[3] = 0; }
}

static void msg(const char* l1, const char* l2, u16 col) {
  ui_clear();
  ui_text(20, 60, col, l1);
  if (l2) ui_text(20, 80, UI_DIM, l2);
  ui_text(20, 150, UI_DIM, "Press A");
  u16 k; do { VBlankIntrWait(); key_poll(); k = key_hit(KEY_A); } while (!k);
}

bool pkview_pk_export(const uint8_t* rec, const PkMon* m) {
  f_mkdir("/pokeviewer");                 /* ignore FR_EXIST */
  f_mkdir(PKVIEW_BANK_DIR);

  char base[16];
  sanitize(base, m->nickname[0] ? m->nickname : pk_species_name(m->species), sizeof(base));
  char path[SF_PATH_MAX];
  siprintf(path, PKVIEW_BANK_DIR "/%s_%08lX.pk3", base, (unsigned long)m->personality);

  SfStatus st = sf_write_verified(path, rec, 80);   /* first 80 bytes = the box record */
  if (st == SF_OK) {
    char p2[40]; ui_truncate(p2, path, 29);
    msg("EXPORTED", p2, UI_OK);
    return true;
  }
  msg("EXPORT FAILED", sf_status_str(st), UI_WARN);
  return false;
}
