#include "pdna_pk.h"

#include <tonc.h>
#include <stdio.h>
#include <string.h>
#include "ff.h"
#include "savefile.h"
#include "data_tables.h"
#include "ui.h"
#include "snd.h"
#include "rmbl.h"          /* rumble must not toggle the cart bus during an SD write */

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

static void msg(const char* l1, const char* l2, const char* l3, u16 col) {
  ui_clear();
  ui_panel(16, 48, 208, 74, UI_PANEL, col);
  ui_text(28, 58, col, l1);
  if (l2) ui_text(28, 80, UI_TEXT, l2);
  if (l3) ui_text(28, 92, UI_DIM, l3);
  ui_text(28, 108, UI_DIM, "Press A");
  u16 k; do { VBlankIntrWait(); snd_vblank(); key_poll(); k = key_hit(KEY_A); } while (!k);
}

/* Write one mon's 80-byte box record to a PKHeX-compatible .pk3 in the bank dir.
 * No UI (safe to call in a batch loop). Fills out_path (if non-NULL) with the path
 * written. Returns the verified-write status. */
SfStatus pdna_pk_export_silent(const uint8_t* rec, const PkMon* m, char* out_path, int cap) {
  f_mkdir("/PokeDNA");                 /* ignore FR_EXIST */
  f_mkdir(PDNA_BANK_DIR);

  char base[16];
  sanitize(base, m->nickname[0] ? m->nickname : pk_species_name(m->species), sizeof(base));
  char path[SF_PATH_MAX];
  int nprint = sniprintf(path, sizeof(path), PDNA_BANK_DIR "/%s_%08lX.pk3",
                         base, (unsigned long)m->personality);
  if (nprint < 0 || nprint >= (int)sizeof(path)) return SF_ERR_LAYOUT;   /* path truncated -> refuse */

  rmbl_pause();
  SfStatus st = sf_write_verified(path, rec, 80);   /* first 80 bytes = the box record */
  rmbl_resume();
  if (st == SF_OK && out_path && cap > 0) { strncpy(out_path, path, cap - 1); out_path[cap - 1] = 0; }
  return st;
}

bool pdna_pk_export(const uint8_t* rec, const PkMon* m) {
  char path[SF_PATH_MAX];
  SfStatus st = pdna_pk_export_silent(rec, m, path, sizeof(path));
  if (st == SF_OK) {
    char p2[40]; ui_truncate(p2, path, 29);
    snd_save();
    msg("EXPORTED", p2, "Open it from START > Bank.", UI_OK);
    return true;
  }
  snd_error();
  msg("EXPORT FAILED", sf_status_str(st), 0, UI_WARN);
  return false;
}
