/*
 * gba-pokeviewer — entry point (milestone M0).
 *
 * Scaffold: bring up tonc + the bitmap UI, detect the flashcart, mount the SD,
 * and BROWSE the card (folders + .sav files, navigate with A/B) so saves in
 * subfolders are reachable. On select we parse the save (existing gen3_save.c)
 * and show a basic read-only summary (game, trainer, TID, play time, party
 * preview). This proves the whole pipeline — flashcart -> FatFs -> parse -> UI —
 * end to end before M1 adds the full hidden-data (IV/EV/stat) viewer.
 *
 * Include order matters: <tonc.h> first so its u8/u16 typedefs are in place
 * before flashcartio.h pulls in sys.h's u8/u16 macros (same pattern as the
 * record-mixer's main.c).
 */

#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "flashcartio.h"   /* active_flashcart, flashcartio_activate (pulls in sys.h) */
#include "sys.h"           /* EWRAM_BSS (idempotent; guarded)                          */
#include "ff.h"
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "data_tables.h"
#include "mon_icons.h"
#include "pkview_summary.h"
#include "pkview_box.h"
#include "gen3_trainer.h"
#include "pkview_trainer.h"
#include "pkview_edit.h"
#include "gen3_edit.h"     /* EditMon, gen3_edit_load/commit, em_set_*, em_preview */
#include "pkview_pick.h"   /* pick_item, pick_move (PC-menu quick editors) */
#include "pkview_app.h"
#include "savefile.h"
#include "log.h"
#include "ui.h"

#define LOG_PATH      "/pokeviewer_log.txt"
#define PATH_MAX      256
#define MAX_ENTRIES   256
#define NAME_MAX      64
#define LIST_COLS     28          /* display columns for a list row                 */
#define VIS_ROWS      12          /* visible rows in the framed browser panel        */

typedef struct {
  char     name[NAME_MAX];
  uint32_t size;                  /* file size in bytes (0 for folders)              */
  bool     is_dir;
} BrowseEntry;

/* Big buffers live in EWRAM (.bss), never on the IWRAM stack. */
static u8          EWRAM_BSS g_save[G3_SAVE_FILE_SIZE];   /* 128 KiB raw image       */
static u8          EWRAM_BSS g_sb1[G3_SAVEBLOCK1_BYTES];  /* reassembled SaveBlock1  */
static BrowseEntry EWRAM_BSS g_entries[MAX_ENTRIES];      /* current-dir listing     */
static int         g_count = 0;
static char        EWRAM_BSS g_cwd[PATH_MAX];             /* current directory (set in main) */
static PkMon       EWRAM_BSS g_party[6];                  /* decoded party of the open save  */
static u8          EWRAM_BSS g_pc[G3_PC_BYTES];           /* reassembled PC storage (boxes)  */
static u8          EWRAM_BSS g_sb2[G3_SECTOR_DATA_SIZE];   /* SaveBlock2 (trainer card/stats) */

/* ---- VBlank / input discipline (key_poll exactly once per frame) -------- */
static void vsync(void) { VBlankIntrWait(); key_poll(); }

static u16 wait_keys(u16 mask) {
  u16 hit;
  do { vsync(); hit = key_hit(mask); } while (!hit);
  return hit;
}

static void init_system(void) {
  irq_init(NULL);
  irq_add(II_VBLANK, NULL);
  ui_init();                               /* Mode 3 + bitmap TTE */
  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);  /* hold any d-pad dir to keep scrolling */
  key_repeat_limits(14, 3);                /* hold ~0.23s, then ~20/s */
}

static const char* flashcart_name(void) {
  switch (active_flashcart) {
    case EVERDRIVE_GBA_X5: return "EverDrive GBA X5";
    case EZ_FLASH_OMEGA:   return "EZ-Flash Omega/DE";
    default:               return "none";
  }
}

/* Writes (edit mode, later) are Omega-only; surface it from M0. */
static bool cart_writable(void) { return active_flashcart == EZ_FLASH_OMEGA; }

static void halt_msg(const char* msg) {
  log_line("HALT: %s", msg);
  log_flush_to_sd(LOG_PATH);
  ui_clear();
  ui_text(6, 60, UI_WARN, "HALT");
  ui_text(6, 76, UI_TEXT, msg);
  while (1) vsync();
}

static int has_sav_ext(const char* n) {
  unsigned L = (unsigned)strlen(n);
  if (L < 4 || n[L - 4] != '.') return 0;
  return (n[L - 3] == 's' || n[L - 3] == 'S') &&
         (n[L - 2] == 'a' || n[L - 2] == 'A') &&
         (n[L - 1] == 'v' || n[L - 1] == 'V');
}

/* ---- path helpers (ported from the record-mixer browser) ---------------- */
static bool at_root(void) { return g_cwd[0] == '/' && g_cwd[1] == 0; }

static bool path_join(const char* dir, const char* name, char* out) {
  unsigned dl = (unsigned)strlen(dir), nl = (unsigned)strlen(name);
  if (dl == 1 && dir[0] == '/') {
    if (1 + nl + 1 > PATH_MAX) return false;
    siprintf(out, "/%s", name);
  } else {
    if (dl + 1 + nl + 1 > PATH_MAX) return false;
    siprintf(out, "%s/%s", dir, name);
  }
  return true;
}

static void path_up(void) {
  int l = (int)strlen(g_cwd);
  if (l <= 1) return;
  int i = l - 1;
  while (i > 0 && g_cwd[i] != '/') i--;
  if (i == 0) g_cwd[1] = 0; else g_cwd[i] = 0;
}

static const char* base_name(const char* p) {
  const char* s = strrchr(p, '/');
  return s ? s + 1 : p;
}

/* Scan g_cwd into g_entries: subdirectories + *.sav files (any game). Unlike
 * the mixer we do NOT parse-filter here, so FRLG and other saves still show. */
static void scan_dir(void) {
  g_count = 0;
  DIR dir;
  FILINFO fno;
  if (f_opendir(&dir, g_cwd) != FR_OK) { log_line("opendir %s failed", g_cwd); return; }
  while (g_count < MAX_ENTRIES && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
    bool is_dir = (fno.fattrib & AM_DIR) != 0;
    if (fno.fattrib & (AM_HID | AM_SYS)) continue;      /* hide hidden/system        */
    if (!is_dir && !has_sav_ext(fno.fname)) continue;   /* only folders + .sav files */
    strncpy(g_entries[g_count].name, fno.fname, NAME_MAX - 1);
    g_entries[g_count].name[NAME_MAX - 1] = 0;
    g_entries[g_count].size = is_dir ? 0 : (uint32_t)fno.fsize;
    g_entries[g_count].is_dir = is_dir;
    g_count++;
  }
  f_closedir(&dir);

  /* stable partition: directories first, then files */
  for (int i = 1; i < g_count; i++) {
    BrowseEntry tmp = g_entries[i];
    int j = i - 1;
    while (j >= 0 && !g_entries[j].is_dir && tmp.is_dir) { g_entries[j + 1] = g_entries[j]; j--; }
    g_entries[j + 1] = tmp;
  }
  log_line("scan %s: %d entries", g_cwd, g_count);
}

/* short human-readable byte size (saves are 128 KiB) into out[] */
static const char* human_size(uint32_t b, char* out) {
  if (b >= 1024u * 1024u) siprintf(out, "%u.%uMB", (unsigned)(b >> 20), (unsigned)(((b >> 10) & 1023u) * 10u / 1024u));
  else if (b >= 1024u)    siprintf(out, "%uKB", (unsigned)(b >> 10));
  else                    siprintf(out, "%uB", (unsigned)b);
  return out;
}

/* sd-browser-style listing: cwd header, framed panel, (DIR) tags + right-aligned
 * size column, a per-selection detail block, and a green status line. */
static void render_browser(int sel, int top) {
  ui_clear();
  char cwdc[LIST_COLS * 4 + 1];
  ui_truncate(cwdc, g_cwd, 29);
  ui_text(2, 2, UI_TITLE, cwdc);
  ui_panel(0, 11, UI_SCR_W, 104, UI_PANEL, UI_BORDER);

  if (g_count == 0) {
    ui_text(6, 40, UI_WARN, "(no folders or .sav files here)");
    ui_text(6, 52, UI_DIM,  at_root() ? "Open a folder where your saves live."
                                      : "B = go up a folder.");
  } else {
    char row[LIST_COLS * 4 + 1], nm[NAME_MAX + 2], sz[12];
    for (int i = 0; i < VIS_ROWS && top + i < g_count; i++) {
      int idx = top + i;
      int y = 14 + i * UI_ROW_H;
      const BrowseEntry* e = &g_entries[idx];
      if (e->is_dir) {
        ui_truncate(nm, e->name, 21);
        siprintf(row, "%-21s (DIR)", nm);
        ui_text_sel(3, y, UI_SCR_W - 6, idx == sel, UI_DIRCLR, row);
      } else {
        ui_truncate(nm, e->name, 18);
        human_size(e->size, sz);
        siprintf(row, "%-18s %8s", nm, sz);
        ui_text_sel(3, y, UI_SCR_W - 6, idx == sel, UI_SAVECLR, row);
      }
    }
  }

  if (g_count > 0) {                          /* per-selection detail block */
    const BrowseEntry* e = &g_entries[sel];
    char dn[40]; ui_truncate(dn, e->name, 29);
    ui_text(2, 118, UI_SELTEXT, dn);
    char meta[40];
    if (e->is_dir) siprintf(meta, "folder");
    else { char sz[12]; human_size(e->size, sz); siprintf(meta, "save file   %s", sz); }
    ui_text(2, 128, UI_DIM, meta);
  }

  char status[48], stc[40];
  siprintf(status, "[%s]  %d/%d", flashcart_name(), g_count ? sel + 1 : 0, g_count);
  ui_truncate(stc, status, 29);
  ui_text(2, 138, UI_OK, stc);

  ui_hline(0, 147, UI_SCR_W, UI_BORDER);
  ui_text(2, 150, UI_DIM,
          at_root() ? "A open  U/D scroll  L/R top/bot"
                    : "A open  B up  U/D scroll  L/R top/bot");
}

/* Browse the SD for a .sav. Writes the chosen full path to out and returns true;
 * navigation never leaves the browser (A enters a folder, B goes up). */
static bool browse_pick(char* out, int cap) {
  scan_dir();
  int sel = 0, top = 0;
  for (;;) {
    if (sel >= g_count) sel = g_count > 0 ? g_count - 1 : 0;
    if (sel < 0) sel = 0;
    if (sel < top) top = sel;
    if (sel >= top + VIS_ROWS) top = sel - VIS_ROWS + 1;

    render_browser(sel, top);

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_L | KEY_R);
    if      (k & KEY_UP)   { if (sel > 0) sel--; }
    else if (k & KEY_DOWN) { if (sel < g_count - 1) sel++; }
    else if (k & KEY_L)    { sel = 0; top = 0; }
    else if (k & KEY_R)    { sel = g_count ? g_count - 1 : 0; }
    else if (k & KEY_B)    { if (!at_root()) { path_up(); scan_dir(); sel = 0; top = 0; } }
    else if (k & KEY_A) {
      if (g_count == 0) continue;
      const BrowseEntry* e = &g_entries[sel];
      char np[PATH_MAX];
      if (!path_join(g_cwd, e->name, np)) continue;
      if (e->is_dir) {
        strcpy(g_cwd, np);
        scan_dir();
        sel = 0; top = 0;
      } else if ((int)strlen(np) < cap) {
        strcpy(out, np);
        return true;
      }
    }
  }
}

static const char* ver_label(Gen3Version v, bool frlg) {
  if (frlg) return "FireRed/LeafGreen";
  switch (v) {
    case G3_VER_EMERALD: return "Emerald";
    case G3_VER_RS:      return "Ruby/Sapphire";
    default:             return "Gen-3";
  }
}

/* loaded-save state shared by the party list + box views */
static Gen3SaveInfo g_vinfo;
static int  g_nparty = 0;
static bool g_frlg = false, g_have_pc = false;
static PkGame g_game = PK_EMERALD;
static char   g_path[PATH_MAX];                           /* path of the open save (for commit) */

/* ===================== edit / commit (V4) =============================== */

bool app_can_edit(void) { return active_flashcart == EZ_FLASH_OMEGA; }

static void msg_wait(const char* title, u16 col, const char* l1, const char* l2) {
  ui_clear();
  ui_text(20, 60, col, title);
  if (l1) ui_text(20, 80, UI_TEXT, l1);
  if (l2) ui_text(20, 92, UI_DIM, l2);
  ui_text(20, 150, UI_DIM, "Press A");
  wait_keys(KEY_A);
}

/* Persist `block` (already-patched save-block sections [lo..hi]) into the in-RAM
 * image with fresh checksums, an immutable backup, and a verified write. The
 * single safe write path shared by the full editor and the PC-menu quick edits. */
static bool app_commit_block(int sect_lo, int sect_hi, uint8_t* block) {
  for (int id = sect_lo; id <= sect_hi; id++)
    gen3_write_full_section(g_save, g_vinfo.slot, id,
                            block + (uint32_t)(id - sect_lo) * G3_SECTOR_DATA_SIZE);

  int fail = -1;
  if (!gen3_verify_full_checksums(g_save, g_vinfo.slot, &fail)) {
    log_line("edit: checksum FAIL at section %d", fail);
    msg_wait("CHECKSUM ERROR", UI_WARN, "Edited image failed checks.", "NOT written.");
    return false;
  }

  log_line("=== edit commit -> %s ===", g_path);
  char bak[SF_PATH_MAX];
  SfStatus st = sf_backup(g_path, bak, sizeof(bak));
  if (st != SF_OK) {
    log_line("edit: backup failed (%s)", sf_status_str(st));
    log_flush_to_sd(LOG_PATH);
    msg_wait("BACKUP FAILED", UI_WARN, sf_status_str(st), "Save NOT modified.");
    return false;
  }
  st = sf_write_verified(g_path, g_save, G3_SAVE_FILE_SIZE);
  log_line("edit: write %s (backup %s)", st == SF_OK ? "OK" : sf_status_str(st), bak);
  log_flush_to_sd(LOG_PATH);
  if (st != SF_OK) {
    msg_wait("WRITE FAILED", UI_WARN, sf_status_str(st), "Backup kept; .tmp may remain.");
    return false;
  }
  msg_wait("SAVED", UI_OK, "Edit written + verified.", "Original backed up first.");
  return true;
}

bool app_edit_commit(uint8_t* rec, bool is_party, int sect_lo, int sect_hi, uint8_t* block) {
  uint8_t out[100];
  if (!pkview_inspect(rec, is_party, true, out)) return false; /* viewed only / discarded */
  memcpy(rec, out, is_party ? 100 : 80);                     /* patch in place (rec is inside block) */
  return app_commit_block(sect_lo, sect_hi, block);
}

/* PC-menu quick editors: load -> mutate one field -> losslessly re-encode -> patch
 * in place -> commit. Each returns true iff the save was written. */
static bool app_quick_item(uint8_t* rec, bool is_party, int lo, int hi, uint8_t* block) {
  EditMon e; gen3_edit_load(rec, is_party, &e);
  PkMon cur; em_preview(&e, &cur); pk_resolve(&cur);
  uint16_t id = pick_item(cur.heldItem);             /* 0xFFFF = cancel; 0 = no item */
  if (id == 0xFFFF) return false;
  em_set_item(&e, id);
  uint8_t out[100]; gen3_edit_commit(&e, out);
  memcpy(rec, out, is_party ? 100 : 80);
  return app_commit_block(lo, hi, block);
}

static bool app_quick_moves(uint8_t* rec, bool is_party, int lo, int hi, uint8_t* block) {
  EditMon e; gen3_edit_load(rec, is_party, &e);
  PkMon cur; em_preview(&e, &cur); pk_resolve(&cur);
  int sel = 0;
  for (;;) {                                          /* choose which of the 4 slots */
    ui_clear();
    ui_text(4, 2, UI_TITLE, "EDIT WHICH MOVE?");
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < 4; i++) {
      int y = 22 + i * 16; bool s = (i == sel);
      if (s) ui_panel(2, y - 2, 236, 13, UI_SEL, UI_TITLE);
      uint16_t mv = cur.moves[i];
      char row[28]; siprintf(row, "%d. %s", i + 1, mv ? pk_move_name(mv) : "-");
      ui_text(8, y, s ? UI_SELTEXT : UI_TEXT, row);
    }
    ui_text(4, 152, UI_DIM, "A edit slot  B back");
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return false;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : 3;
    else if (k & KEY_DOWN) sel = (sel + 1) % 4;
    else if (k & KEY_A)    break;
  }
  uint16_t id = pick_move(cur.moves[sel]);            /* 0xFFFF = cancel */
  if (id == 0xFFFF) return false;
  em_set_move(&e, sel, id);
  uint8_t out[100]; gen3_edit_commit(&e, out);
  memcpy(rec, out, is_party ? 100 : 80);
  return app_commit_block(lo, hi, block);
}

/* Gen-3-PC-style action menu on A. Omega: overlay SUMMARY/ITEM/MOVES/CANCEL and
 * run the chosen editor. Everdrive (read-only): jump straight to the summary.
 * Returns true iff the save was modified (caller should refresh its view). */
bool app_mon_menu(uint8_t* rec, bool is_party, int sect_lo, int sect_hi, uint8_t* block) {
  if (!app_can_edit()) {
    uint8_t dummy[100];
    pkview_inspect(rec, is_party, false, dummy);
    return false;
  }
  EditMon e0; gen3_edit_load(rec, is_party, &e0);
  PkMon m0; em_preview(&e0, &m0); pk_resolve(&m0);
  char title[16];
  ui_truncate(title, m0.nickname[0] ? m0.nickname : pk_species_name(m0.species), 11);

  static const char* const ITEMS[4] = { "SUMMARY", "ITEM", "MOVES", "CANCEL" };
  const int mx = 148, my = 34, mw = 88, mh = 18 + 4 * 13;
  int sel = 0;
  for (;;) {
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);   /* overlay (box stays behind) */
    ui_text(mx + 6, my + 4, UI_TITLE, title);
    ui_hline(mx + 2, my + 15, mw - 4, UI_BORDER);
    for (int i = 0; i < 4; i++) {
      int y = my + 18 + i * 13; bool s = (i == sel);
      if (s) ui_panel(mx + 2, y - 1, mw - 4, 12, UI_SEL, UI_TITLE);
      ui_text(mx + 10, y, s ? UI_SELTEXT : UI_TEXT, ITEMS[i]);
    }
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return false;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : 3;
    else if (k & KEY_DOWN) sel = (sel + 1) % 4;
    else if (k & KEY_A) {
      switch (sel) {
        case 0: return app_edit_commit(rec, is_party, sect_lo, sect_hi, block);
        case 1: return app_quick_item (rec, is_party, sect_lo, sect_hi, block);
        case 2: return app_quick_moves(rec, is_party, sect_lo, sect_hi, block);
        default: return false;                       /* CANCEL */
      }
    }
  }
}

/* Party list view. A opens the summary; SELECT switches to PC boxes; B exits.
 * Returns 0 (back to file browser) or 1 (switch to boxes). */
static int party_list(void) {
  int sel = 0;
  for (;;) {
    ui_clear();
    char line[48];
    siprintf(line, "%s  -  %s", g_vinfo.trainer_name, ver_label(g_vinfo.version_guess, g_frlg));
    ui_text(4, 2, UI_TITLE, line);
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);

    if (g_nparty == 0) ui_text(6, 40, UI_WARN, "No Pokemon in party.");
    for (int i = 0; i < g_nparty; i++) {
      int y = 16 + i * 22;
      PkMon* p = &g_party[i];
      if (i == sel) ui_panel(2, y - 2, 236, 20, UI_SEL, UI_TITLE);
      ui_icon_sub(6, y, mon_icon_for(p->species));   /* 16x16 from the 32x32 icon */
      char nm[16];
      ui_truncate(nm, p->nickname[0] ? p->nickname : pk_species_name(p->species), 11);
      siprintf(line, "%-11s Lv%u", nm, (unsigned)p->level);
      ui_text(26, y, i == sel ? UI_SELTEXT : UI_TEXT, line);
      siprintf(line, "%s%s%s", pk_species_name(p->species),
               p->isShiny ? "  *" : "", p->isEgg ? "  EGG" : "");
      ui_text(26, y + 9, UI_DIM, line);
    }

    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, g_have_pc ? "A view  SEL boxes  START card  B back"
                                      : "A view  START card  B back");

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_SELECT | KEY_START);
    if      (k & KEY_UP)   { if (sel > 0) sel--; }
    else if (k & KEY_DOWN) { if (sel < g_nparty - 1) sel++; }
    else if (k & KEY_B)    return 0;
    else if (k & KEY_START) return 2;
    else if (k & KEY_SELECT) { if (g_have_pc) return 1; }
    else if ((k & KEY_A) && g_nparty > 0) {
      uint16_t doff = g_frlg ? 0x0038 : 0x0238;
      uint8_t* rec = g_sb1 + doff + (uint32_t)sel * 100;     /* party lives in SaveBlock1 (ids 1..4) */
      if (app_mon_menu(rec, true, 1, 4, g_sb1)) {            /* PC-style menu -> chosen editor -> commit */
        g_nparty = pk_read_party_auto(g_sb1, g_party, &g_frlg);
        for (int i = 0; i < g_nparty; i++) pk_resolve(&g_party[i]);
        if (sel >= g_nparty) sel = g_nparty ? g_nparty - 1 : 0;
      }
    }
  }
}

/* Load the picked save and show it: start in the PC boxes; SELECT toggles to the
 * party list and back; B from either returns to the file browser. */
static void view_save(const char* path) {
  strncpy(g_path, path, sizeof(g_path) - 1);
  g_path[sizeof(g_path) - 1] = 0;
  uint32_t sz = 0;
  SfStatus st = sf_read_full(path, g_save, G3_SAVE_FILE_SIZE, &sz);
  if (st != SF_OK || sz < (uint32_t)G3_SLOT_BYTES ||
      !gen3_parse(g_save, sz, &g_vinfo) || !g_vinfo.valid || !g_vinfo.sb1_ok ||
      gen3_read_saveblock1(g_save, g_vinfo.slot, g_sb1) != G3_SAVEBLOCK1_BYTES) {
    ui_clear();
    ui_text(6, 60, UI_WARN, "Cannot read this save.");
    ui_text(6, 76, UI_DIM, st != SF_OK ? sf_status_str(st) : "not a valid Gen-3 .sav");
    ui_text(4, 150, UI_DIM, "B=back");
    wait_keys(KEY_B);
    return;
  }

  g_frlg = false;
  g_nparty = pk_read_party_auto(g_sb1, g_party, &g_frlg);
  for (int i = 0; i < g_nparty; i++) pk_resolve(&g_party[i]);
  g_have_pc = (gen3_read_pc_storage(g_save, g_vinfo.slot, g_pc) == G3_PC_BYTES);

  /* SaveBlock2 (section 0) for the trainer card + per-game layout for stats */
  int s0 = gen3_find_section(g_save, g_vinfo.slot, 0);
  if (s0 >= 0)
    memcpy(g_sb2, g_save + (uint32_t)g_vinfo.slot * G3_SLOT_BYTES + (uint32_t)s0 * G3_SECTOR_SIZE,
           G3_SECTOR_DATA_SIZE);
  g_game = g_frlg ? PK_FRLG : (g_vinfo.version_guess == G3_VER_RS ? PK_RS : PK_EMERALD);

  int mode = g_have_pc ? 0 : 1;          /* start in PC boxes (per request) */
  for (;;) {
    int r = (mode == 0) ? pkview_box(g_pc) : party_list();
    if (r == 0) return;                          /* B -> file browser */
    if (r == 2) { pkview_trainer(g_sb1, g_sb2, &g_vinfo, g_game); continue; }  /* START -> trainer card */
    if (!g_have_pc) return;                       /* nothing to toggle to */
    mode ^= 1;                                    /* SELECT -> toggle box/party */
  }
}

int main(void) {
  init_system();
  log_init();
  log_line("=== gba-pokeviewer (M0) ===");
  log_line("mGBA debug log: %s", log_under_mgba() ? "active" : "absent");

  ui_clear();
  ui_text(6, 70, UI_TITLE, "Detecting flashcart...");
  if (!flashcartio_activate()) halt_msg("No flashcart detected!");
  log_line("flashcart: %s", flashcart_name());

  FATFS fs;                                  /* lives forever (main never returns) */
  FRESULT fr = f_mount(&fs, "", 1);
  if (fr != FR_OK) { log_line("f_mount failed (fr=%d)", fr); halt_msg("SD mount failed!"); }
  log_line("SD mounted OK");
  log_flush_to_sd(LOG_PATH);

  strcpy(g_cwd, "/");
  for (;;) {
    char path[PATH_MAX];
    if (browse_pick(path, sizeof(path))) view_save(path);
  }
  return 0;
}
