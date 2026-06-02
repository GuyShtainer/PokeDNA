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
#include "savefile.h"
#include "log.h"
#include "ui.h"

#define LOG_PATH      "/pokeviewer_log.txt"
#define PATH_MAX      256
#define MAX_ENTRIES   256
#define NAME_MAX      64
#define LIST_COLS     28          /* display columns for a list row                 */
#define VIS_ROWS      16          /* visible rows in the browser                     */

typedef struct {
  char name[NAME_MAX];
  bool is_dir;
} BrowseEntry;

/* Big buffers live in EWRAM (.bss), never on the IWRAM stack. */
static u8          EWRAM_BSS g_save[G3_SAVE_FILE_SIZE];   /* 128 KiB raw image       */
static u8          EWRAM_BSS g_sb1[G3_SAVEBLOCK1_BYTES];  /* reassembled SaveBlock1  */
static BrowseEntry EWRAM_BSS g_entries[MAX_ENTRIES];      /* current-dir listing     */
static int         g_count = 0;
static char        EWRAM_BSS g_cwd[PATH_MAX];             /* current directory (set in main) */
static PkMon       EWRAM_BSS g_party[6];                  /* decoded party of the open save  */
static u8          EWRAM_BSS g_pc[G3_PC_BYTES];           /* reassembled PC storage (boxes)  */

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
  key_repeat_mask(KEY_UP | KEY_DOWN);
  key_repeat_limits(16, 4);                /* hold ~0.27s, then ~15/s */
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

static void render_browser(int sel, int top) {
  ui_clear();
  char cwdc[LIST_COLS * 4 + 1];
  ui_truncate(cwdc, g_cwd, LIST_COLS);
  char hdr[80];
  siprintf(hdr, "POKEVIEWER [%s] %s", flashcart_name(), cwdc);
  ui_text(4, 2, UI_TITLE, hdr);
  ui_hline(0, 11, UI_SCR_W, UI_BORDER);

  if (g_count == 0) {
    ui_text(6, 40, UI_WARN, "(no folders or .sav files here)");
    ui_text(6, 52, UI_DIM,  at_root() ? "Open a folder where your saves live."
                                      : "B = go up a folder.");
  } else {
    char row[LIST_COLS * 4 + 1];
    char tmp[NAME_MAX + 2];
    for (int i = 0; i < VIS_ROWS && top + i < g_count; i++) {
      int idx = top + i;
      int y = 14 + i * UI_ROW_H;
      const BrowseEntry* e = &g_entries[idx];
      if (e->is_dir) {
        siprintf(tmp, "%s/", e->name);
        ui_truncate(row, tmp, LIST_COLS);
        ui_text_sel(4, y, UI_SCR_W - 8, idx == sel, UI_DIRCLR, row);
      } else {
        ui_truncate(row, e->name, LIST_COLS);
        ui_text_sel(4, y, UI_SCR_W - 8, idx == sel, UI_SAVECLR, row);
      }
    }
  }

  ui_hline(0, 147, UI_SCR_W, UI_BORDER);
  ui_text(4, 150, UI_DIM,
          at_root() ? "A=open  U/D=scroll  L/R=top/bot"
                    : "A=open  B=up  U/D=scroll  L/R=top/bot");
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

/* Load the picked save, decode the party, and show it as a list; A opens the
 * full 6-card summary for the highlighted mon. */
static void party_screen(const char* path) {
  uint32_t sz = 0;
  SfStatus st = sf_read_full(path, g_save, G3_SAVE_FILE_SIZE, &sz);
  Gen3SaveInfo info;
  if (st != SF_OK || sz < (uint32_t)G3_SLOT_BYTES ||
      !gen3_parse(g_save, sz, &info) || !info.valid || !info.sb1_ok ||
      gen3_read_saveblock1(g_save, info.slot, g_sb1) != G3_SAVEBLOCK1_BYTES) {
    ui_clear();
    ui_text(6, 60, UI_WARN, "Cannot read this save.");
    ui_text(6, 76, UI_DIM, st != SF_OK ? sf_status_str(st) : "not a valid Gen-3 .sav");
    ui_text(4, 150, UI_DIM, "B=back");
    wait_keys(KEY_B);
    return;
  }

  bool frlg = false;
  int n = pk_read_party_auto(g_sb1, g_party, &frlg);
  for (int i = 0; i < n; i++) pk_resolve(&g_party[i]);   /* fill gender (party stats are plaintext) */
  bool have_pc = (gen3_read_pc_storage(g_save, info.slot, g_pc) == G3_PC_BYTES);

  int sel = 0;
  for (;;) {
    ui_clear();
    char line[48];
    siprintf(line, "%s  -  %s", info.trainer_name, ver_label(info.version_guess, frlg));
    ui_text(4, 2, UI_TITLE, line);
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);

    if (n == 0) ui_text(6, 40, UI_WARN, "No Pokemon in party.");
    for (int i = 0; i < n; i++) {
      int y = 16 + i * 22;
      PkMon* p = &g_party[i];
      if (i == sel) ui_panel(2, y - 2, 236, 20, UI_SEL, UI_TITLE);
      ui_icon16(6, y, mon_icon_for(p->species));
      char nm[16];
      ui_truncate(nm, p->nickname[0] ? p->nickname : pk_species_name(p->species), 11);
      siprintf(line, "%-11s Lv%u", nm, (unsigned)p->level);
      ui_text(26, y, i == sel ? UI_SELTEXT : UI_TEXT, line);
      siprintf(line, "%s%s%s", pk_species_name(p->species),
               p->isShiny ? "  *" : "", p->isEgg ? "  EGG" : "");
      ui_text(26, y + 9, UI_DIM, line);
    }

    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, have_pc ? "A view  SEL boxes  B back" : "A view  U/D move  B back");

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_SELECT);
    if      (k & KEY_UP)   { if (sel > 0) sel--; }
    else if (k & KEY_DOWN) { if (sel < n - 1) sel++; }
    else if (k & KEY_B)    return;
    else if ((k & KEY_SELECT) && have_pc) pkview_box(g_pc);
    else if ((k & KEY_A) && n > 0) sel = pkview_summary(g_party, n, sel);
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
    if (browse_pick(path, sizeof(path))) party_screen(path);
  }
  return 0;
}
