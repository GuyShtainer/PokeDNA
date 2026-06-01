/*
 * gba-pokeviewer — entry point (milestone M0).
 *
 * Scaffold: bring up tonc + the bitmap UI, detect the flashcart, mount the SD,
 * list the .sav files in the card root, and let the user pick one. On select we
 * parse the save (existing gen3_save.c) and show a basic read-only summary
 * (game, trainer, TID, play time, party preview). This proves the whole pipeline
 * — flashcart -> FatFs -> parse -> UI — end to end before M1 adds the full
 * hidden-data (IV/EV/stat) viewer.
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
#include "savefile.h"
#include "log.h"
#include "ui.h"

#define LOG_PATH    "/pokeviewer_log.txt"
#define PATH_MAX    256
#define MAX_SAVES   128
#define NAME_MAX    64
#define LIST_COLS   28          /* display columns for a list row                 */
#define VIS_ROWS    16          /* visible rows in the picker                      */

/* Big buffers live in EWRAM (.bss), never on the IWRAM stack. */
static u8   EWRAM_BSS g_save[G3_SAVE_FILE_SIZE];        /* 128 KiB raw image       */
static u8   EWRAM_BSS g_sb1[G3_SAVEBLOCK1_BYTES];       /* reassembled SaveBlock1  */
static char EWRAM_BSS g_names[MAX_SAVES][NAME_MAX];     /* .sav filenames in root  */
static int  g_count = 0;

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

/* Writes (edit mode, later) are Omega-only; surface it from M0 so the user is
 * never surprised. */
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

/* Scan the SD root for *.sav files into g_names. */
static void scan_saves(void) {
  g_count = 0;
  DIR dir;
  FILINFO fno;
  if (f_opendir(&dir, "/") != FR_OK) { log_line("opendir / failed"); return; }
  while (g_count < MAX_SAVES && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
    if (fno.fattrib & AM_DIR) continue;
    if (!has_sav_ext(fno.fname)) continue;
    strncpy(g_names[g_count], fno.fname, NAME_MAX - 1);
    g_names[g_count][NAME_MAX - 1] = 0;
    g_count++;
  }
  f_closedir(&dir);
  log_line("scan: %d .sav file(s) in root", g_count);
}

/* Scrolling list picker. Returns the chosen index, or -1 if cancelled (B). */
static int pick_save(void) {
  int sel = 0, top = 0;
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + VIS_ROWS) top = sel - VIS_ROWS + 1;

    ui_clear();
    char hdr[48];
    siprintf(hdr, "POKEVIEWER  [%s]", flashcart_name());
    ui_text(4, 2, UI_TITLE, hdr);
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);

    if (g_count == 0) {
      ui_text(6, 40, UI_WARN, "No .sav files in the SD root.");
      ui_text(6, 52, UI_DIM,  "Copy a Gen-3 save there and press A.");
    } else {
      char row[LIST_COLS * 4 + 1];
      for (int i = 0; i < VIS_ROWS && top + i < g_count; i++) {
        int idx = top + i;
        int y = 14 + i * UI_ROW_H;
        ui_truncate(row, g_names[idx], LIST_COLS);
        ui_text_sel(4, y, UI_SCR_W - 8, idx == sel, UI_SAVECLR, row);
      }
    }

    ui_hline(0, 147, UI_SCR_W, UI_BORDER);
    ui_text(4, 150, UI_DIM,
            cart_writable() ? "A=open  U/D=move  R=rescan  (editable cart)"
                            : "A=open  U/D=move  R=rescan  (read-only cart)");

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_R);
    if      (k & KEY_UP)    { if (sel > 0) sel--; }
    else if (k & KEY_DOWN)  { if (sel < g_count - 1) sel++; }
    else if (k & KEY_R)     return -1;                 /* caller rescans */
    else if (k & KEY_B)     return -1;
    else if ((k & KEY_A) && g_count > 0) return sel;
  }
}

static const char* ver_label(Gen3Version v) {
  switch (v) {
    case G3_VER_EMERALD: return "Emerald";
    case G3_VER_RS:      return "Ruby/Sapphire";
    default:             return "FireRed/LeafGreen or unknown";
  }
}

/* Read-only summary of the picked save (M0 depth; full hidden-data viewer = M1+). */
static void show_detail(int idx) {
  char path[PATH_MAX];
  siprintf(path, "/%s", g_names[idx]);

  ui_clear();
  ui_text(4, 2, UI_TITLE, "SAVE DETAIL");
  ui_hline(0, 11, UI_SCR_W, UI_BORDER);

  char line[80];
  char namebuf[LIST_COLS * 4 + 1];
  ui_truncate(namebuf, g_names[idx], LIST_COLS);
  ui_text(4, 14, UI_TEXT, namebuf);

  uint32_t sz = 0;
  SfStatus st = sf_read_full(path, g_save, G3_SAVE_FILE_SIZE, &sz);
  siprintf(line, "size: %u bytes", (unsigned)sz);
  ui_text(4, 24, UI_DIM, line);

  if (st != SF_OK) {
    ui_text(4, 42, UI_WARN, sf_status_str(st));
    ui_text(4, 150, UI_DIM, "B=back");
    wait_keys(KEY_B);
    return;
  }

  Gen3SaveInfo info;
  if (sz < (uint32_t)G3_SLOT_BYTES || !gen3_parse(g_save, sz, &info) || !info.valid) {
    ui_text(4, 42, UI_WARN, "Not a valid Gen-3 save.");
    ui_text(4, 150, UI_DIM, "B=back");
    wait_keys(KEY_B);
    return;
  }

  siprintf(line, "game: %s", ver_label(info.version_guess));
  ui_text(4, 38, UI_OK, line);
  siprintf(line, "trainer: %s  (%c)", info.trainer_name, info.gender ? 'F' : 'M');
  ui_text(4, 48, UI_TEXT, line);
  siprintf(line, "TID: %05u   slot: %d", (unsigned)info.tid_public, info.slot);
  ui_text(4, 58, UI_TEXT, line);
  siprintf(line, "playtime: %uh %02um", (unsigned)info.play_h, (unsigned)info.play_m);
  ui_text(4, 68, UI_TEXT, line);

  if (info.sb1_ok &&
      gen3_read_saveblock1(g_save, info.slot, g_sb1) == G3_SAVEBLOCK1_BYTES) {
    Gen3DisplayParty dp;
    gen3_read_live_party_display(g_sb1, &dp);
    siprintf(line, "party: %d", dp.count);
    ui_text(4, 84, UI_TITLE, line);
    for (int i = 0; i < dp.count; i++) {
      char nm[16];
      ui_truncate(nm, dp.mon[i].nickname, 10);
      siprintf(line, "%-10s  Lv%u", nm, (unsigned)dp.mon[i].level);
      ui_text(10, 94 + i * UI_ROW_H, UI_TEXT, line);
    }
  } else {
    ui_text(4, 84, UI_DIM, "(SaveBlock1 incomplete)");
  }

  ui_hline(0, 147, UI_SCR_W, UI_BORDER);
  ui_text(4, 150, UI_DIM, "B=back   (read-only viewer)");
  wait_keys(KEY_B);
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

  for (;;) {
    scan_saves();
    int idx = pick_save();
    if (idx >= 0) show_detail(idx);
  }
  return 0;
}
