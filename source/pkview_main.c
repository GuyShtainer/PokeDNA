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
#include "gen3_clip.h"     /* ClipMon, slot ops (copy/paste/dup/release) */
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
  uint32_t dosdt;                 /* (fdate<<16)|ftime, for the date sort            */
  bool     is_dir;
} BrowseEntry;

/* file-browser sort + filter state (sd-browser style) */
typedef enum { SORT_NAME = 0, SORT_SIZE = 1, SORT_DATE = 2 } BrSortKey;
static BrSortKey g_sort = SORT_NAME;
static bool      g_sortrev = false;
static bool      g_show_all = false;     /* false = folders + .sav only; true = all files */
static bool      g_show_hidden = false;

/* Big buffers live in EWRAM (.bss), never on the IWRAM stack. */
static u8          EWRAM_BSS g_save[G3_SAVE_FILE_SIZE];   /* 128 KiB raw image       */
static u8          EWRAM_BSS g_sb1[G3_SAVEBLOCK1_BYTES];  /* reassembled SaveBlock1  */
static BrowseEntry EWRAM_BSS g_entries[MAX_ENTRIES];      /* current-dir listing     */
static int         g_count = 0;
static char        EWRAM_BSS g_cwd[PATH_MAX];             /* current directory (set in main) */
static PkMon       EWRAM_BSS g_party[6];                  /* decoded party of the open save  */
static u8          EWRAM_BSS g_pc[G3_PC_BYTES];           /* reassembled PC storage (boxes)  */
static u8          EWRAM_BSS g_sb2[G3_SECTOR_DATA_SIZE];   /* SaveBlock2 (trainer card/stats) */
static ClipMon     EWRAM_BSS g_clip;                       /* one-slot mon clipboard          */

/* ---- VBlank / input discipline (key_poll exactly once per frame) -------- */
static void vsync(void) { VBlankIntrWait(); key_poll(); }

static u16 wait_keys(u16 mask) {
  u16 hit;
  do { vsync(); hit = key_hit(mask) | key_repeat(mask & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)); } while (!hit);
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

/* case-insensitive ASCII name compare (locale-free) */
static int name_ci(const char* a, const char* b) {
  for (;;) {
    unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;
    if (x >= 'a' && x <= 'z') x -= 32;
    if (y >= 'a' && y <= 'z') y -= 32;
    if (x != y) return (int)x - (int)y;
    if (!x) return 0;
  }
}

/* directories always first (not reversible); then by the active key, name as the
 * tiebreaker, negated for descending. */
static int entry_cmp(const BrowseEntry* x, const BrowseEntry* y) {
  if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
  int c;
  switch (g_sort) {
    case SORT_SIZE: c = (x->size < y->size) ? -1 : (x->size > y->size) ? 1 : 0; break;
    case SORT_DATE: c = (x->dosdt < y->dosdt) ? -1 : (x->dosdt > y->dosdt) ? 1 : 0; break;
    default:        c = name_ci(x->name, y->name); break;
  }
  if (c == 0) c = name_ci(x->name, y->name);
  return g_sortrev ? -c : c;
}

static void sort_entries(void) {                  /* stable insertion sort, never mid-transfer */
  for (int i = 1; i < g_count; i++) {
    BrowseEntry tmp = g_entries[i];
    int j = i - 1;
    while (j >= 0 && entry_cmp(&g_entries[j], &tmp) > 0) { g_entries[j + 1] = g_entries[j]; j--; }
    g_entries[j + 1] = tmp;
  }
}

/* Scan g_cwd into g_entries: subdirectories + (by default) *.sav files. The
 * filter (g_show_all / g_show_hidden) and the sort are sd-browser-style. */
static void scan_dir(void) {
  g_count = 0;
  DIR dir;
  FILINFO fno;
  if (f_opendir(&dir, g_cwd) != FR_OK) { log_line("opendir %s failed", g_cwd); return; }
  while (g_count < MAX_ENTRIES && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
    bool is_dir = (fno.fattrib & AM_DIR) != 0;
    if (!g_show_hidden && (fno.fattrib & (AM_HID | AM_SYS))) continue;
    if (!is_dir && !g_show_all && !has_sav_ext(fno.fname)) continue;  /* folders + .sav unless show-all */
    strncpy(g_entries[g_count].name, fno.fname, NAME_MAX - 1);
    g_entries[g_count].name[NAME_MAX - 1] = 0;
    g_entries[g_count].size = is_dir ? 0 : (uint32_t)fno.fsize;
    g_entries[g_count].dosdt = ((uint32_t)fno.fdate << 16) | (uint32_t)fno.ftime;
    g_entries[g_count].is_dir = is_dir;
    g_count++;
  }
  f_closedir(&dir);
  sort_entries();
  log_line("scan %s: %d entries", g_cwd, g_count);
}

static const char* sort_label(void) {
  switch (g_sort) {
    case SORT_SIZE: return g_sortrev ? "Size big-small" : "Size small-big";
    case SORT_DATE: return g_sortrev ? "Date new-old"   : "Date old-new";
    default:        return g_sortrev ? "Name Z-A"       : "Name A-Z";
  }
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

  char status[64], stc[40];
  siprintf(status, "%d/%d  %s  %s", g_count ? sel + 1 : 0, g_count,
           sort_label(), g_show_all ? "all" : ".sav");
  ui_truncate(stc, status, 29);
  ui_text(2, 138, UI_OK, stc);

  ui_hline(0, 147, UI_SCR_W, UI_BORDER);
  ui_text(2, 150, UI_DIM, "A open  B up  SE sort  ST menu");
}

/* Reboot back into the flashcart loader menu (no return on confirm). */
static void do_reboot(void) {
  ui_clear();
  ui_text(16, 56, UI_TITLE, "Reboot to flashcart menu?");
  ui_text(16, 78, UI_TEXT, "A = reboot to loader");
  ui_text(16, 90, UI_WARN, "B = cancel");
  u16 k; do { vsync(); k = key_hit(KEY_A | KEY_B); } while (!k);
  if (k & KEY_B) return;
  ui_clear();
  ui_text(16, 72, UI_TITLE, "Rebooting...");
  log_flush_to_sd(LOG_PATH);
  VBlankIntrWait();
  flashcartio_reboot();                 /* never returns */
}

/* START menu over the browser: sort key/order, file filter, show-hidden, reboot.
 * Returns true if a setting changed that needs a re-scan. */
static bool browse_menu(void) {
  int sel = 0;
  bool changed = false;
  for (;;) {
    ui_clear();
    ui_text(4, 4, UI_TITLE, "FILE MENU");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    char rows[6][40];
    siprintf(rows[0], "Sort key:  %s", g_sort == SORT_NAME ? "Name" : g_sort == SORT_SIZE ? "Size" : "Date");
    siprintf(rows[1], "Order:     %s", g_sortrev ? "descending" : "ascending");
    siprintf(rows[2], "Files:     %s", g_show_all ? "all files" : ".sav only");
    siprintf(rows[3], "Hidden:    %s", g_show_hidden ? "shown" : "hidden");
    strcpy(rows[4], "Reboot to flashcart menu...");
    strcpy(rows[5], "Close");
    for (int i = 0; i < 6; i++) {
      int y = 26 + i * 16; bool s = (i == sel);
      if (s) ui_panel(2, y - 2, 236, 13, UI_SEL, UI_TITLE);
      ui_text(10, y, s ? UI_SELTEXT : UI_TEXT, rows[i]);
    }
    ui_text(4, 152, UI_DIM, "A change  U/D move  B back");
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return changed;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : 5;
    else if (k & KEY_DOWN) sel = (sel + 1) % 6;
    else if (k & KEY_A) {
      switch (sel) {
        case 0: g_sort = (BrSortKey)((g_sort + 1) % 3); changed = true; break;
        case 1: g_sortrev = !g_sortrev; changed = true; break;
        case 2: g_show_all = !g_show_all; changed = true; break;
        case 3: g_show_hidden = !g_show_hidden; changed = true; break;
        case 4: do_reboot(); break;            /* returns only if cancelled */
        case 5: return changed;
      }
    }
  }
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

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_L | KEY_R | KEY_SELECT | KEY_START);
    if      (k & KEY_UP)   { if (sel > 0) sel--; }
    else if (k & KEY_DOWN) { if (sel < g_count - 1) sel++; }
    else if (k & KEY_L)    { sel = 0; top = 0; }
    else if (k & KEY_R)    { sel = g_count ? g_count - 1 : 0; }
    else if (k & KEY_SELECT) {           /* cycle the 6 sort states (key x order) */
      int s = ((int)g_sort * 2 + (g_sortrev ? 1 : 0) + 1) % 6;
      g_sort = (BrSortKey)(s / 2); g_sortrev = (s & 1) != 0;
      sort_entries(); sel = 0; top = 0;
    }
    else if (k & KEY_START) { if (browse_menu()) { scan_dir(); sel = 0; top = 0; } }
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
static uint16_t g_item_clip = 0;                          /* held-item move clipboard            */
static bool     g_item_held = false;

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

bool app_clip_occupied(void) { return g_clip.occupied; }

static bool app_confirm(const char* title, const char* l1) {
  ui_clear();
  ui_text(20, 56, UI_WARN, title);
  if (l1) ui_text(20, 78, UI_TEXT, l1);
  ui_text(20, 102, UI_TEXT, "A = yes");
  ui_text(20, 114, UI_DIM, "B = no");
  u16 k; do { vsync(); k = key_hit(KEY_A | KEY_B); } while (!k);
  return (k & KEY_A) != 0;
}

/* first empty (decodes-as-empty) slot in a box, or -1 if the box is full */
static int box_free_slot(const uint8_t* pc, int box) {
  for (int s = 0; s < G3_IN_BOX; s++) {
    PkMon m;
    if (!pk_decode_mon(pk_box_slot((uint8_t*)pc, box, s), false, &m)) return s;
  }
  return -1;
}

static bool app_copy(uint8_t* rec, bool is_party) {
  clip_copy_from(&g_clip, rec, is_party);
  msg_wait("COPIED", UI_OK, "PASTE places it in a slot.", "(it survives until overwritten)");
  return false;                                          /* no save change */
}

static bool app_paste(uint8_t* rec, bool is_party, int lo, int hi, uint8_t* block, bool occupied) {
  if (!g_clip.occupied) return false;
  if (occupied && !app_confirm("Overwrite this Pokemon?", "Paste the copied mon here?")) return false;
  uint8_t out[100];
  if (!clip_to_record(&g_clip, is_party, out)) return false;
  memcpy(rec, out, is_party ? 100 : 80);
  return app_commit_block(lo, hi, block);
}

static bool app_duplicate(uint8_t* rec, bool is_party, int lo, int hi, uint8_t* block, int box) {
  if (is_party) {
    if (party_count(block, g_frlg) >= 6) { msg_wait("PARTY FULL", UI_WARN, "Release a mon first.", 0); return false; }
    uint8_t out[100]; memcpy(out, rec, 100);
    party_append(block, g_frlg, out);
  } else {
    int fs = box_free_slot(block, box);
    if (fs < 0) { msg_wait("BOX FULL", UI_WARN, "No empty slot in this box.", 0); return false; }
    memcpy(pk_box_slot(block, box, fs), rec, 80);
  }
  return app_commit_block(lo, hi, block);
}

static bool app_release(uint8_t* rec, bool is_party, int lo, int hi, uint8_t* block, int box, int slot) {
  (void)rec;
  if (is_party && party_count(block, g_frlg) <= 1) {
    msg_wait("CAN'T RELEASE", UI_WARN, "The party can't be empty.", 0);
    return false;
  }
  if (!app_confirm("Release this Pokemon?", "It is permanently deleted.")) return false;
  if (is_party) party_release(block, g_frlg, slot);
  else          clip_clear_box_slot(block, box, slot);
  return app_commit_block(lo, hi, block);
}

/* Held-item move (take/give). TAKE stashes this mon's item and clears it; GIVE
 * puts the stashed item on a mon, swapping in that mon's old item so nothing is
 * ever lost (the stash empties when the swapped-out item is none). */
static bool app_take_item(uint8_t* rec, bool is_party, int lo, int hi, uint8_t* block) {
  EditMon e; gen3_edit_load(rec, is_party, &e);
  PkMon cur; em_preview(&e, &cur);
  if (!cur.heldItem) return false;
  g_item_clip = cur.heldItem; g_item_held = true;
  em_set_item(&e, 0);
  uint8_t out[100]; gen3_edit_commit(&e, out);
  memcpy(rec, out, is_party ? 100 : 80);
  return app_commit_block(lo, hi, block);
}
static bool app_give_item(uint8_t* rec, bool is_party, int lo, int hi, uint8_t* block) {
  if (!g_item_held) return false;
  EditMon e; gen3_edit_load(rec, is_party, &e);
  PkMon cur; em_preview(&e, &cur);
  uint16_t dst_old = cur.heldItem;
  em_set_item(&e, g_item_clip);
  uint8_t out[100]; gen3_edit_commit(&e, out);
  memcpy(rec, out, is_party ? 100 : 80);
  g_item_clip = dst_old; g_item_held = (dst_old != 0);   /* swap: keep dst's old item to re-home */
  return app_commit_block(lo, hi, block);
}

/* Gen-3-PC-style action menu on A. Omega: a navigable overlay of SUMMARY / ITEM /
 * MOVES / COPY / PASTE / DUPLICATE / RELEASE (the slot-aware ones use box/slot;
 * party ops use g_frlg). On an EMPTY slot it offers PASTE only. Everdrive
 * (read-only): jump to the summary. Returns true iff the save was modified.
 * For party callers pass box = -1, slot = party index. */
bool app_mon_menu(uint8_t* rec, bool is_party, int sect_lo, int sect_hi, uint8_t* block, int box, int slot) {
  PkMon m0;
  bool occupied = pk_decode_mon(rec, is_party, &m0);
  if (occupied) pk_resolve(&m0);

  if (!app_can_edit()) {                                 /* read-only carts: view only */
    if (occupied) { uint8_t d[100]; pkview_inspect(rec, is_party, false, d); }
    return false;
  }

  enum { A_SUMMARY, A_ITEM, A_MOVES, A_COPY, A_PASTE, A_DUP, A_RELEASE, A_TAKEITEM, A_GIVEITEM, A_CANCEL };
  int act[12]; const char* lab[12]; int n = 0;
  if (occupied) {
    lab[n]="SUMMARY"; act[n++]=A_SUMMARY;
    lab[n]="ITEM";    act[n++]=A_ITEM;
    lab[n]="MOVES";   act[n++]=A_MOVES;
    lab[n]="COPY";    act[n++]=A_COPY;
    if (g_clip.occupied) { lab[n]="PASTE"; act[n++]=A_PASTE; }
    lab[n]="DUPLICATE"; act[n++]=A_DUP;
    if (m0.heldItem && !g_item_held) { lab[n]="TAKE ITEM"; act[n++]=A_TAKEITEM; }
    if (g_item_held)                 { lab[n]="GIVE ITEM"; act[n++]=A_GIVEITEM; }
    lab[n]="RELEASE";   act[n++]=A_RELEASE;
  } else if (g_clip.occupied) {
    lab[n]="PASTE HERE"; act[n++]=A_PASTE;
  } else {
    return false;                                        /* empty + nothing to paste */
  }
  lab[n]="CANCEL"; act[n++]=A_CANCEL;

  char title[16];
  ui_truncate(title, occupied ? (m0.nickname[0] ? m0.nickname : pk_species_name(m0.species)) : "EMPTY", 11);
  const int mx = 138, mw = 100, mh = 18 + n * 13, my = 80 - mh / 2;
  int sel = 0;
  for (;;) {
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);
    ui_text(mx + 6, my + 4, UI_TITLE, title);
    ui_hline(mx + 2, my + 15, mw - 4, UI_BORDER);
    for (int i = 0; i < n; i++) {
      int y = my + 18 + i * 13; bool s = (i == sel);
      if (s) ui_panel(mx + 2, y - 1, mw - 4, 12, UI_SEL, UI_TITLE);
      ui_text(mx + 10, y, s ? UI_SELTEXT : UI_TEXT, lab[i]);
    }
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return false;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
    else if (k & KEY_A) {
      switch (act[sel]) {
        case A_SUMMARY: return app_edit_commit(rec, is_party, sect_lo, sect_hi, block);
        case A_ITEM:    return app_quick_item (rec, is_party, sect_lo, sect_hi, block);
        case A_MOVES:   return app_quick_moves(rec, is_party, sect_lo, sect_hi, block);
        case A_COPY:    return app_copy(rec, is_party);
        case A_PASTE:   return app_paste(rec, is_party, sect_lo, sect_hi, block, occupied);
        case A_DUP:     return app_duplicate(rec, is_party, sect_lo, sect_hi, block, box);
        case A_RELEASE: return app_release(rec, is_party, sect_lo, sect_hi, block, box, slot);
        case A_TAKEITEM:return app_take_item(rec, is_party, sect_lo, sect_hi, block);
        case A_GIVEITEM:return app_give_item(rec, is_party, sect_lo, sect_hi, block);
        default:        return false;                    /* CANCEL */
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
      if (app_mon_menu(rec, true, 1, 4, g_sb1, -1, sel)) {   /* PC-style menu -> chosen editor -> commit */
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
