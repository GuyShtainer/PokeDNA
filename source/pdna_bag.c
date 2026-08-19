/*
 * Real Gen-3 bag screen — the data editor's bag tab wearing the in-game bag
 * chrome of the LOADED game (RS / Emerald / FRLG, per-gender art). One
 * ROM->VRAM blit of the pre-composited 240x160 background (see
 * tools/gen_bag_bg.py) with the pocket's item list in the game's list pane,
 * the selected item's 24x24 icon in the chrome's own icon square (Emerald /
 * FRLG; RS's bag has none), its description in the desc region, and the
 * pocket name in its banner — all geometry from BAG_LAYOUTS[game] in
 * bag_bg.h, zero magic pixels here. FRLG keeps its 3-pocket chrome but
 * browses all 5 PokeDNA pockets (its TM Case / Berry Pouch have no bag
 * frame — they reuse the Items open frame).
 * Controls follow the game: L/R switch POCKET, U/D scroll, A edits the slot
 * (the exact pick_item -> quantity -> pk_bag_set powers of the plain tab,
 * wrong-pocket auto-routing included), B exits. Edits stay in RAM; the CALLER
 * (data_editor) owns the confirm + verified SB1 commit — no new SD write paths.
 * Partial redraws restore rects from the ROM bg (bg_restore): 0 new EWRAM.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_bag.h"
#include "pdna_app.h"   /* app_item_desc: the user's ROM first, embedded table second */
#include "bag_bg.h"
#include "ui.h"
#include "snd.h"
#include "osk.h"
#include "rumble.h"
#include "gen3_items.h"
#include "data_tables.h"     /* pk_item_name / pk_item_desc */
#include "item_icons.h"
#include "pdna_pick.h"       /* pick_item */

/* Strong bag_bg()/bag_anim() come from the GENERATED bag_bg.c (git-ignored
 * ripped art); these weak NULLs keep an art-free clone building — data_editor
 * then keeps the plain bag tab (same pattern as the wallpaper fallbacks in
 * pdna_box.c), and a NULL bag_anim() just skips the pocket animation. */
__attribute__((weak)) BgFrame bag_bg(int game, int female) {
  (void)game; (void)female; BgFrame f = { 0, 0, BAG_BG_W }; return f;
}
__attribute__((weak)) BgFrame bag_anim(int game, int female, int step) {
  (void)game; (void)female; (void)step; BgFrame f = { 0, 0, BAG_ANIM_W }; return f;
}

/* Inks tuned on the generated bg: dark text on the cream list / white desc
 * panes and the yellow banner; the game-style red cursor; white on the stripes. */
#define BINK   RGB15( 5,  5,  7)   /* primary text on the light panes  */
#define BDIM   RGB15(18, 18, 16)   /* empty-slot rows                  */
#define BCUR   RGB15(26,  4,  3)   /* selection '>' + current-pocket dot */
#define BFOOT  RGB15(31, 31, 31)   /* footer hints on the striped wall */

/* 9-px text rows that fit the game's list pane (14 on RS/Emerald, 10 on FRLG) */
#define BROWS(L)  (((L)->list_y1 - (L)->list_y0 - 2) / 9)

/* ---- input (the wait_keys idiom, local like pdna_trainer's s_wait) ------ */
static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 hit, fresh;
  do {
    s_vsync();
    fresh = key_hit(mask);
    hit = fresh | key_repeat(mask & (KEY_UP | KEY_DOWN));
  } while (!hit);
  if (fresh & (KEY_UP | KEY_DOWN)) snd_move();
  if (fresh & KEY_A) snd_ok();
  else if (fresh & KEY_B) snd_back();
  return hit;
}

/* numeric entry via the on-screen keyboard; returns `cur` on cancel. */
static uint32_t bag_number(const char* prompt, uint32_t cur, uint32_t maxv) {
  char init[12], out[12];
  siprintf(init, "%lu", (unsigned long)cur);
  if (!osk_search(prompt, init, out, sizeof(out))) return cur;
  uint32_t v = 0;
  for (const char* p = out; *p >= '0' && *p <= '9'; p++) v = v * 10 + (uint32_t)(*p - '0');
  return v > maxv ? maxv : v;
}

/* framed dialog (msg_wait clone); the caller full-redraws the bag after it. */
static void bag_msg(const char* title, u16 col, const char* l1, const char* l2) {
  ui_clear();
  ui_panel(16, 48, 208, 70, UI_PANEL, col);
  ui_text(28, 58, col, title);
  if (l1) ui_text(28, 80, UI_TEXT, l1);
  if (l2) ui_text(28, 92, UI_DIM, l2);
  ui_text(28, 104, UI_DIM, "Press A");
  s_wait(KEY_A);
}

/* ---- bag sprite: rest frame + the game's pocket-switch animation --------- */

/* blit one pre-composited 64x(64+rise) anim rect (ROM) at the bag anchor */
static void bag_rect(const BagLayout* L, BgFrame r) {
  bg_blit_rect(r, L->anim_x, L->anim_y, BAG_ANIM_W, 64 + L->rise);
}

/* Resting bag = the current pocket's OPEN frame — the games never show the
 * closed bag while browsing. Art-free fallback: the bg's baked closed bag. */
static void bag_rest(const BagLayout* L, PkGame g, int female, int pocket) {
  BgFrame r = bag_anim(g, female, L->rise + pocket);
  if (r.blob) bag_rect(L, r);
}

/* The game's own pocket-switch animation: the CLOSED bag pops up `rise` px and
 * falls back 1 px per fall_wait frames — Emerald/FRLG 5 px at 1 px/frame
 * (SetBagVisualPocketId), RS 4 px at 1 px per 2 frames (sub_80A79EC) — then
 * the new pocket's open frame appears. One ~8.8 KiB ROM->VRAM blit per step.
 * No art -> no anim (the screen still works). */
static void pocket_anim(BgFrame bg, const BagLayout* L,
                        PkGame g, int female, int pocket) {
  for (int i = 0; i < L->rise; i++) {
    BgFrame r = bag_anim(g, female, i);
    if (!r.blob) return;
    bag_rect(L, r);
    for (int w = 0; w < L->fall_wait; w++) s_vsync();
  }
  bg_restore(bg, L->anim_x, L->anim_y, BAG_ANIM_W, 64 + L->rise);  /* closed, landed */
  s_vsync();
  bag_rest(L, g, female, pocket);             /* the new pocket springs open */
}

/* pocket-name banner + (Emerald) the 5 switch dots, current pocket in red.
 * The restore rect is the layout's pkt_r* box — wider than the text window
 * where the banner graphic needs it ("Poke Balls" = 10 cols) and tall enough
 * to erase the dot markers. */
static void draw_header(BgFrame bg, const BagLayout* L, int pocket) {
  bg_restore(bg, L->pkt_rx, L->pkt_ry, L->pkt_rw, L->pkt_rh);
  ui_text(L->pkt_tx, L->pkt_ty, BINK, pk_pocket_name(pocket));
  if (L->dot_x)
    ui_fill_rect(L->dot_x - 1 + pocket * 8, L->dot_y - 1, 4, 4, BCUR);
}

/* the '>' cursor column only (selection moved within the visible page) */
static void draw_cursor(BgFrame bg, const BagLayout* L, int top, int sel) {
  bg_restore(bg, L->list_x0 + 2, L->list_y0, 8, L->list_y1 - L->list_y0);
  ui_text(L->list_x0 + 2, L->list_y0 + 1 + (sel - top) * 9, BCUR, ">");
}

/* whole list pane: name + right-aligned quantity per slot (name columns fill
 * whatever the pane leaves after the "x999" gutter). The Key Items pocket
 * shows no quantity, matching the games. */
static void draw_list(BgFrame bg, const BagLayout* L, const uint8_t* sb1,
                      const uint8_t* sb2, PkGame g, int pocket, int top, int sel) {
  bg_restore(bg, L->list_x0, L->list_y0,
                L->list_x1 - L->list_x0, L->list_y1 - L->list_y0);
  int cap = pk_pocket_cap(g, pocket);
  int nx = L->list_x0 + 9;                       /* name column, just past the '>' */
  for (int i = 0; i < BROWS(L) && top + i < cap; i++) {
    int sl = top + i, y = L->list_y0 + 1 + i * 9;
    uint16_t id = pk_bag_item(sb1, g, pocket, sl);
    if (id) {
      /* The quantity is right-aligned and measured FIRST, so the name gets every
       * pixel the number does not need. Retail does the same, which is how a row
       * fits "No01 FOCUS PUNCH x 1". A fixed "x999" gutter would have cost 12 px
       * on every row for a width almost nothing uses. */
      int right = L->list_x1 - 3;
      if (pocket != POCKET_KEY) {
        char q[8]; siprintf(q, "x%u", (unsigned)pk_bag_qty(sb1, sb2, g, pocket, sl));
        right = ui_ptext_right(right, y, BINK, q) - 4;
      }
      char nm[48];
      pk_item_label(id, nm, sizeof nm);          /* TMs/HMs carry their move here */
      ui_ptext_fit(nx, y, right - nx, BINK, nm);
    } else ui_ptext(nx, y, BDIM, "-");
  }
  ui_text(L->list_x0 + 2, L->list_y0 + 1 + (sel - top) * 9, BCUR, ">");
}

/* ---- description pane: word-wrap + AUTO-PAGING (BACKLOG 12d) --------------
 * Laid out in PIXELS with the proportional face (ui_ptext), the same way the
 * games do it — which is why retail fits "Powerful, but makes the user flinch if
 * hit by the foe." in four short lines and our 8 px font could not. Most
 * descriptions now land on a single page; the pager stays for the few that don't
 * and flips every BAG_DESC_FLIP idle frames. The pane is PURE TEXT (full width,
 * no icon flow-around): the 24x24 item icon lives in the game's own icon slot
 * (L->icon_xy — the baked white square each game reserves for it, outside this
 * region; see bag_bg.h). */
#define BAG_DESC_FLIP 90                         /* ~1.5 s per page */
static int s_desc_pages = 1;                     /* pages of the CURRENT desc (set by draw_desc) */

/* Lay out ONE page starting at text offset *pi, advancing *pi past what it
 * consumed; draws only when `draw`. Every page uses the same row slots, measured
 * in pixels. When `multi`, the LAST row gives up ~22 px for the "n/m" page chip.
 * Returns true while text remains after this page. */
static bool desc_flow(const BagLayout* L, const char* s, int* pi, bool draw, bool multi) {
  int n = (int)strlen(s), i = *pi;
  for (int y = L->desc_y0 + 2; y + 8 <= L->desc_y1 - 2 && i < n; y += 9) {
    int x = L->desc_x0 + 6;
    int maxw = L->desc_x1 - x - 2;
    if (multi && y + 9 + 8 > L->desc_y1 - 2) maxw -= 22;   /* last row: room for "n/m" */
    if (maxw < 8) continue;
    int skip, take = ui_ptext_break(s + i, maxw, &skip);
    if (draw) {
      char line[48];
      if (take > (int)sizeof line - 1) take = (int)sizeof line - 1;
      memcpy(line, s + i, take); line[take] = 0;
      ui_ptext(x, y, L->desc_ink, line);
    }
    i += skip;
    while (i < n && s[i] == ' ') i++;
  }
  *pi = i;
  return i < n;
}

/* Repaints ONLY the desc pane rect + the game's icon slot (bg_restore) —
 * no flicker anywhere else. The icon goes in the chrome's own baked square
 * (L->icon_xy, outside the pane; (0,0) = this game shows no icon — RS), the
 * pane itself is pure text. Draws page `pg` (0-based, clamped) and the "n/m"
 * chip when it overflows; stores the page count in s_desc_pages for the idle
 * auto-flip. */
static void draw_desc(BgFrame bg, const BagLayout* L, const uint8_t* sb1,
                      PkGame g, int pocket, int sel, int pg) {
  bg_restore(bg, L->desc_x0, L->desc_y0,
                L->desc_x1 - L->desc_x0, L->desc_y1 - L->desc_y0);
  if (L->icon_x | L->icon_y)                     /* the game's icon slot */
    bg_restore(bg, L->icon_x, L->icon_y, ITEM_ICON_W, ITEM_ICON_H);
  s_desc_pages = 1;
  uint16_t id = pk_bag_item(sb1, g, pocket, sel);
  if (!id) { ui_text(L->desc_x0 + 6, L->desc_y0 + 18, BDIM, "(empty slot)"); return; }
  if (L->icon_x | L->icon_y)                     /* NULL icon data = no-op */
    ui_sprite(L->icon_x, L->icon_y, ITEM_ICON_W, ITEM_ICON_H, app_item_icon(id));
  const char* s = app_item_desc(id);
  int i = 0;
  bool multi = desc_flow(L, s, &i, false, false); /* single page? then the plain layout */
  if (multi) {                                    /* count pages with the shortened last row */
    int total = 1; i = 0;
    while (desc_flow(L, s, &i, false, true)) total++;
    s_desc_pages = total;
  }
  if (pg >= s_desc_pages) pg = 0;
  i = 0;                                          /* skip to the page, then draw it */
  for (int p = 0; p < pg; p++) desc_flow(L, s, &i, false, multi);
  desc_flow(L, s, &i, true, multi);
  if (multi) {                                    /* "n/m" chip, bottom-right of the pane */
    int ylast = L->desc_y0 + 2;
    while (ylast + 9 + 8 <= L->desc_y1 - 2) ylast += 9;
    char b[8]; siprintf(b, "%d/%d", pg + 1, s_desc_pages);
    ui_ptext_right(L->desc_x1 - 2, ylast, L->desc_ink, b);
  }
}

bool bag_screen(uint8_t* sb1, const uint8_t* sb2, PkGame game, int female) {
  BgFrame bg = bag_bg(game, female);
  if (!bg.blob || game < 0 || game > 2) return false; /* defensive: callers gate on bag_bg() */
  const BagLayout* L = &BAG_LAYOUTS[game];
  int pocket = 0, sel = 0, top = 0, prev_sel = -1, desc_pg = 0;
  bool dirty = false, full = true, list = true, desc = true;

  for (;;) {
    int cap = pk_pocket_cap(game, pocket);
    if (sel >= cap) sel = cap - 1;
    if (sel < top)             { top = sel; list = true; }
    if (sel >= top + BROWS(L)) { top = sel - (BROWS(L) - 1); list = true; }

    if (full) {                                  /* whole chrome (entry / after picker+dialogs) */
      bg_restore(bg, 0, 0, BAG_BG_W, BAG_BG_H);   /* 20 LZ77 pages ROM->VRAM */
      bag_rest(L, game, female, pocket);         /* bg bakes the CLOSED bag; show the pocket open */
      if (L->foot_y)                             /* only where the chrome leaves a free strip */
        ui_text(L->foot_x, L->foot_y, BFOOT, "A edit  L/R pocket  B done");
      draw_header(bg, L, pocket);
      list = true; full = false;
    }
    if (list) { draw_list(bg, L, sb1, sb2, game, pocket, top, sel); list = false; desc = true; }
    else if (sel != prev_sel) { draw_cursor(bg, L, top, sel); desc = true; }
    if (desc) { desc_pg = 0; draw_desc(bg, L, sb1, game, pocket, sel, desc_pg); desc = false; }
    prev_sel = sel;

    /* s_wait, inlined so idle frames can AUTO-PAGE an overflowing description
     * (12d): every BAG_DESC_FLIP frames without input, show the next page —
     * only the desc pane rect repaints, nothing else flickers. */
    u16 k, fresh;
    {
      const u16 mask = KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R | KEY_A | KEY_B;
      int idle = 0;
      do {
        s_vsync();
        fresh = key_hit(mask);
        k = fresh | key_repeat(mask & (KEY_UP | KEY_DOWN));
        if (!k && s_desc_pages > 1 && ++idle >= BAG_DESC_FLIP) {
          idle = 0;
          desc_pg = (desc_pg + 1) % s_desc_pages;
          draw_desc(bg, L, sb1, game, pocket, sel, desc_pg);
        }
      } while (!k);
      if (fresh & (KEY_UP | KEY_DOWN)) snd_move();
      if (fresh & KEY_A) snd_ok();
      else if (fresh & KEY_B) snd_back();
    }
    if (k & KEY_B) return dirty;
    else if (k & KEY_UP)   { if (sel > 0) sel--; }
    else if (k & KEY_DOWN) sel++;                /* clamped to cap-1 above */
    else if (k & (KEY_L | KEY_R | KEY_LEFT | KEY_RIGHT)) {   /* switch POCKET (game: d-pad too) */
      snd_tab();
      pocket = (pocket + ((k & (KEY_R | KEY_RIGHT)) ? 1 : POCKET_COUNT - 1)) % POCKET_COUNT;
      sel = top = 0; prev_sel = -1; list = true;
      pocket_anim(bg, L, game, female, pocket);  /* closed bag pops + the pocket opens */
      draw_header(bg, L, pocket);
    } else if (k & KEY_A) {                      /* the plain tab's edit powers, verbatim */
      uint16_t cur = pk_bag_item(sb1, game, pocket, sel);
      uint16_t id = pick_item(cur);
      full = true;                               /* the picker/OSK/dialogs took the screen */
      if (id == 0) { pk_bag_set(sb1, sb2, game, pocket, sel, 0, 0); dirty = true; }   /* clear slot */
      else if (id != 0xFFFF) {
        uint16_t q = (uint16_t)bag_number("QUANTITY", pk_bag_qty(sb1, sb2, game, pocket, sel) ? pk_bag_qty(sb1, sb2, game, pocket, sel) : 1, 999);
        int dp = pk_item_pocket(id);
        if (dp == pocket) { pk_bag_set(sb1, sb2, game, pocket, sel, id, q); dirty = true; }
        else {                                   /* wrong pocket: route to the item's real pocket */
          int dcap = pk_pocket_cap(game, dp), dest = -1;
          for (int s = 0; s < dcap; s++) { uint16_t it = pk_bag_item(sb1, game, dp, s);
                                           if (it == id) { dest = s; break; } if (it == 0 && dest < 0) dest = s; }
          if (dest < 0) { snd_deny(); bag_msg("POCKET FULL", UI_WARN, pk_pocket_name(dp), "is full."); }
          else { pk_bag_set(sb1, sb2, game, dp, dest, id, q); dirty = true;
                 char m[40]; siprintf(m, "Put in %s.", pk_pocket_name(dp)); bag_msg("RIGHT POCKET", UI_OK, m, 0); }
        }
      }
    }
  }
}
