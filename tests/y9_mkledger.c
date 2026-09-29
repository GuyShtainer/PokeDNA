/* #280 (lane y9-280): host tool for the target-drop restore chains (tools/dgb_shots.py --y9-*).
 *   y9_mkledger ledger N OUT.pds   write case N's ledger file (bank_plant_y9_ledger) to OUT.pds;
 *                                  stdout: the on-card path (/PokeDNA/xfer/<key>.pds)
 *   y9_mkledger cell N OUT.bin     write case N's Bank cell (80 bytes)
 *   y9_mkledger savledger SAV OUT.pds   G3_HOME ledger file for SAV's box 0 slot 0 mon (the chain's lift)
 *   y9_mkledger liftcheck SAV BOX.box SLOT   exit 0 iff Bank slot SLOT holds SAV's box 0 slot 0 mon as-is
 * Built by the chain with the y7_plantcmp source list + -DPDNA_DELTA. Exit 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "bank_plant.h"
#include "bank_cell.h"
#include "gb_sidecar.h"
#include "gb_session.h"
#include "gb_edit.h"
#include "gen12_convert.h"
#include "xfer_rec.h"

static uint8_t g_img[64 * 1024 + 64];
static uint8_t g_scratch[GBS_SCRATCH_BYTES];
static uint8_t g_list[GBS_LIST_BYTES];

/* Opens SAV and loads box 0 slot 0 (the mon the chain lifts from Red's first box). */
static int load_first(const char* sav, GbEditMon* mon) {
  FILE* f = fopen(sav, "rb"); if (!f) return 2;
  size_t n = fread(g_img, 1, sizeof g_img, f); fclose(f);
  GbSession sess;
  if (gbs_open(&sess, g_img, (uint32_t)n, g_scratch, sizeof g_scratch) != GBS_OK) return 3;
  if (gbs_load_list(&sess, 0, g_list) != GBS_OK) return 4;
  if (gb_list_count(sess.gen, g_list, 0) < 1) return 5;
  return gb_load(mon, sess.gen, g_list, 0, 0) ? 0 : 6;
}

int main(int argc, char** argv) {
  if (argc < 4) return 2;
  int which = atoi(argv[2]);
  FILE* f;
  if (strcmp(argv[1], "ledger") == 0) {
    uint8_t file[GBSC_FILE_MAX]; uint64_t key = 0;
    int len = bank_plant_y9_ledger(which, file, sizeof file, &key);
    if (len <= 0) return 1;
    char path[GBSC_PATH_MAX];
    if (gbsc_path(path, sizeof path, "/PokeDNA/xfer", key) < 0) return 1;
    f = fopen(argv[3], "wb"); if (!f) return 2;
    fwrite(file, 1, (size_t)len, f); fclose(f);
    printf("%s\n", path);
    return 0;
  }
  if (strcmp(argv[1], "savledger") == 0) {   /* argv[2] = the .sav, argv[3] = OUT.pds */
    GbEditMon mon; int rc = load_first(argv[2], &mon);
    if (rc) return rc;
    uint8_t cell[80]; BcMeta meta; GbEditMon back; Gb12Mon view; uint8_t g3[80]; Gb12Notes notes;
    if (bc_pack(&mon, 0, BC_ORIGIN_RED, 0, 999u, cell) != 0 || !bc_unpack(cell, &back, &meta) ||
        !bc_view(&back, &meta, bc_ident32(cell), &view)) return 7;
    Gb12Target tgt; memset(&tgt, 0, sizeof tgt); tgt.met_game = 3;
    if (gen12_convert(&view, &tgt, g3, &notes) != GB12_OK) return 8;
    GbscEntry e; gbsc_entry_from(&e, &mon, g3, 0);
    uint8_t dv4[4] = { gb_get_dv(&mon, GB_ATK), gb_get_dv(&mon, GB_DEF), gb_get_dv(&mon, GB_SPE), gb_get_dv(&mon, GB_SPC) };
    uint64_t key = gbsc_key(mon.gen, gb_get_otid(&mon), dv4, mon.otname);
    uint8_t file[GBSC_FILE_MAX]; uint32_t len = (uint32_t)gbsc_init(file, key);
    if (gbsc_add(file, &len, sizeof file, &e) < 0) return 9;
    char path[GBSC_PATH_MAX];
    if (gbsc_path(path, sizeof path, "/PokeDNA/xfer", key) < 0) return 1;
    f = fopen(argv[3], "wb"); if (!f) return 2;
    fwrite(file, 1, len, f); fclose(f);
    printf("%s\n", path);
    return 0;
  }
  if (strcmp(argv[1], "liftcheck") == 0 && argc >= 5) {   /* SAV BOX.box SLOT: banked cell == the GB record as-is? */
    GbEditMon mon; int rc = load_first(argv[2], &mon);
    if (rc) return rc;
    uint8_t box[2400]; f = fopen(argv[3], "rb"); if (!f) return 2;
    size_t got = fread(box, 1, sizeof box, f); fclose(f);
    int slot = atoi(argv[4]);
    if (got != sizeof box || slot < 0 || slot > 29) return 2;
    GbEditMon b; BcMeta meta;
    if (!bc_is_native(box + slot * 80) || !bc_unpack(box + slot * 80, &b, &meta)) { printf("banked cell: NOT a native cell\n"); return 1; }
    int same = b.gen == mon.gen && b.rec_len == mon.rec_len && memcmp(b.rec, mon.rec, mon.rec_len) == 0 &&
               memcmp(b.otname, mon.otname, GB_NAME_BYTES) == 0 && memcmp(b.nick, mon.nick, GB_NAME_BYTES) == 0;
    printf("banked cell == the Game Boy record as-is (rec, OT name, nickname): %s\n", same ? "YES" : "NO");
    return same ? 0 : 1;
  }
  if (strcmp(argv[1], "cell") == 0) {
    uint8_t cell[80];
    if (!bank_plant_y9_cell(which, cell)) return 1;
    f = fopen(argv[3], "wb"); if (!f) return 2;
    fwrite(cell, 1, 80, f); fclose(f);
    return 0;
  }
  return 2;
}
