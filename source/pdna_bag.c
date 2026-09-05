/*
 * Real Gen-3 bag screen — the data editor's bag tab wearing the in-game bag
 * chrome of the LOADED game (RS / Emerald / FRLG, per-gender art). One
 * ROM->VRAM blit of the background with the pocket's item list in the game's
 * list pane, the selected item's 24x24 icon in the chrome's own icon square
 * (Emerald / FRLG; RS's bag has none), its description in the desc region,
 * and the pocket name in its banner — all geometry from BAG_LAYOUTS[game] in
 * bag_bg.h, zero magic pixels here. FRLG keeps its 3-pocket chrome but
 * browses all 5 PokeDNA pockets (its TM Case / Berry Pouch have no bag
 * frame — they reuse the Items open frame).
 *
 * The background itself is bag_bg(game, female) (bag_bg.h): the STRONG symbol
 * is pre-composited art from tools/gen_bag_bg.py when a full-art build has it
 * staged; otherwise (this file's weak fallback) it streams tileset+tilemap+
 * palette straight out of the user's registered/fused ROM for Emerald/
 * FireRed/LeafGreen (rom_chrome.h's bag section) — RS and every other art-free
 * case fall back to blob==0 and the caller keeps the plain data-editor bag
 * tab. Every call site below fetches bag_bg(game, female) FRESH, immediately
 * before the bg_restore/bg_blit_rect that uses it, rather than reusing one
 * value across the loop — required correctness for the ROM rung, not style;
 * see rom_bag_frame()'s comment below for why.
 *
 * Controls follow the game: L/R switch POCKET, U/D scroll, A edits the slot
 * (the exact pick_item -> quantity -> pk_bag_set powers of the plain tab,
 * wrong-pocket auto-routing included), B exits. Edits stay in RAM; the CALLER
 * (data_editor) owns the confirm + verified SB1 commit — no new SD write paths.
 * Partial redraws restore rects from bg_restore: 0 new EWRAM (the ROM rung
 * decodes into mon_decomp, the existing shared 8 KiB buffer — artbuf.h).
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
#include "rom_chrome_gate.h"
#include "rom_chrome.h"      /* the ROM rung: rom_chrome_bag_have/_load */
#include "artbuf.h"          /* mon_decomp -- the shared 8 KiB decode buffer */

/* Strong bag_bg()/bag_anim() come from the GENERATED bag_bg.c (git-ignored
 * ripped art); these weak NULLs keep an art-free clone building — data_editor
 * then keeps the plain bag tab (same pattern as the wallpaper fallbacks in
 * pdna_box.c), and a NULL bag_anim() just skips the pocket animation.
 * bag_anim() (the pocket-switch mascot animation sheets) has no ROM rung —
 * out of this track's scope (rom_chrome.h: "the bag's own background chrome",
 * matching what rom_chrome.c already does for the card/Pokeblock — neither of
 * which animate either). */
__attribute__((weak)) BgFrame bag_anim(int game, int female, int step) {
  (void)game; (void)female; (void)step; BgFrame f = { 0, 0, BAG_ANIM_W }; return f;
}

/* Set once per ROM (re)registration by pdna_main.c's app_icon_rom_open(), the
 * same pattern pdna_trainer_set_romchrome() already uses for the card. NULL =
 * no ROM open this session, or the open ROM can't serve this game's bag style. */
static const RomChrome* s_bag_romchrome = 0;
void pdna_bag_set_romchrome(const RomChrome* rch) { s_bag_romchrome = rch; }

#if !PDNA_BAG_ART_COMPILED
/* The ROM rung (Emerald/FireRed/LeafGreen -- rom_chrome.h states why Ruby/
 * Sapphire fall through to blob==0). ALWAYS redecodes, on EVERY call -- this
 * is not a style choice, it is required correctness: mon_decomp is the SAME
 * shared 8 KiB buffer app_item_icon() decodes into (pdna_main.c, offset 0,
 * no way to relocate it from here), and draw_desc() below calls
 * app_item_icon() on every selection change. A BgFrame handed out by this
 * function is only valid until the NEXT decode into mon_decomp by ANYTHING --
 * including this function itself. bag_screen() below never stashes the
 * result across a call boundary; every drawing helper fetches its own fresh
 * copy immediately before it touches bg.blob. This is the exact bug class the
 * Pokeblock case shipped once already (a pointer into this same buffer held
 * across a call that let something else decode first) — see
 * pdna_origin_art.c's rom_portrait() and this file's rom_bag_frame() callers
 * for the two other places that learned it. */
static RomChromeBag s_bag_chrome;
static BgFrame rom_bag_frame(int game, int female) {
  BgFrame f = { 0, 0, BAG_BG_W };
  if (!s_bag_romchrome || !rom_chrome_bag_have(s_bag_romchrome, game)) return f;
  if (!rom_chrome_bag_load(s_bag_romchrome, game, female,
                           (uint8_t*)mon_decomp, MON_DECOMP_BYTES, &s_bag_chrome))
    return f;
  f.blob = &s_bag_chrome.as_lzblob; f.off = 0; f.sw = BAG_BG_W;
  return f;
}
#endif /* !PDNA_BAG_ART_COMPILED */

__attribute__((weak)) BgFrame bag_bg(int game, int female) {
#if !PDNA_BAG_ART_COMPILED
  return rom_bag_frame(game, female);
#else
  (void)game; (void)female; BgFrame f = { 0, 0, BAG_BG_W }; return f;
#endif
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

#if !PDNA_BAG_ART_COMPILED
/* The bag SPRITE'S ROM rung -- Emerald/FireRed/LeafGreen's screen chrome, but
 * the DRAWN BAG ITSELF (rom_chrome.h's bag-sprite section) is FireRed/
 * LeafGreen only: both games' whole gendered animation sheet is ONE
 * monolithic LZ10 blob whose declared size (8,192 B) just fits the shared
 * 8,192 B buffer alone; Emerald's is 12,288 B, bigger than the WHOLE buffer,
 * for every frame including the closed one (there is no partial/seek decode
 * -- see rom_chrome.h's header comment for the confirmed reason). So on
 * Emerald this draws nothing and the screen keeps exactly today's look
 * (background chrome only, no bag icon) -- a real, reported gap, not a
 * silently-skipped one.
 *
 * Draws straight to vid_mem via romchrome_blit_tiles(), NOT through
 * bag_anim()'s BgFrame/tilemap contract: the sheet is plain 8x8-tile picture
 * data with ONE flat palette, and by the time this runs the caller has
 * ALREADY bg_restore()'d the whole screen (or at minimum this rect) from the
 * correct background, so there is no "clean patch above the sprite" to
 * reconstruct the way bag_anim()'s taller (64+rise) rect does for the
 * transient pop/fall frames (also out of scope here, for the same budget
 * reason -- see rom_chrome_bag_sprite_load()). Reuses mon_decomp exactly like
 * every other "decode fresh, blit immediately" call in this file: safe
 * because rom_bag_frame()'s chrome decode, called just before this in every
 * caller, has already been fully consumed by its own bg_restore(). */
static void rom_bag_sprite_draw(const BagLayout* L, PkGame g, int female, int pocket) {
  if (!s_bag_romchrome || !rom_chrome_bag_sprite_have(s_bag_romchrome, (int)g)) return;
  /* bag_bg.h's own pocket_frame convention (tools/gen_bag_bg.py GAMES table,
   * "frlg" row): OUR PkPocket order (Items/Key/Balls/TMHM/Berries) -> the
   * sheet's own frame number (sAnims_Bag: closed/PokeBalls/Items/KeyItems).
   * Emerald never reaches here (rom_chrome_bag_sprite_have() is 0 there). */
  static const uint8_t k_pocket_frame_frlg[5] = { 2, 3, 1, 2, 2 };
  int frame = (pocket >= 0 && pocket < 5) ? k_pocket_frame_frlg[pocket] : 0;
  RomChromeBagSprite bs;
  if (!rom_chrome_bag_sprite_load(s_bag_romchrome, (int)g, female,
                                  (uint8_t*)mon_decomp, MON_DECOMP_BYTES, &bs)) return;
  if (frame >= bs.frame_count) frame = 0;
  romchrome_blit_tiles(bs.tiles + (uint32_t)frame * 64u * 32u, bs.pal, 0, 8, 8,
                       L->anim_x, L->anim_y + L->rise);
}
#endif

/* Resting bag = the current pocket's OPEN frame — the games never show the
 * closed bag while browsing. Art-free fallback: the bg's baked closed bag
 * (or, artless with a wired ROM, the streamed sprite via the ROM rung above). */
static void bag_rest(const BagLayout* L, PkGame g, int female, int pocket) {
  BgFrame r = bag_anim(g, female, L->rise + pocket);
  if (r.blob) { bag_rect(L, r); return; }
#if !PDNA_BAG_ART_COMPILED
  rom_bag_sprite_draw(L, g, female, pocket);
#endif
}

/* The game's own pocket-switch animation: the CLOSED bag pops up `rise` px and
 * falls back 1 px per fall_wait frames — Emerald/FRLG 5 px at 1 px/frame
 * (SetBagVisualPocketId), RS 4 px at 1 px per 2 frames (sub_80A79EC) — then
 * the new pocket's open frame appears. One ~8.8 KiB ROM->VRAM blit per step.
 * No transient-animation art (compiled OR ROM -- see rom_bag_sprite_draw()'s
 * comment, the pop/fall frames are out of this track's budget) -> `break`
 * straight to the landed state, which bag_rest() below can still show via the
 * ROM rung even when the pop/fall itself cannot animate. */
static void pocket_anim(BgFrame bg, const BagLayout* L,
                        PkGame g, int female, int pocket) {
  for (int i = 0; i < L->rise; i++) {
    BgFrame r = bag_anim(g, female, i);
    if (!r.blob) break;
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

/* Column geometry shared by draw_cursor()'s erase and draw_list()'s text origin. Must
 * stay in this relationship: BAG_CUR_DX + BAG_CUR_W <= BAG_NAME_DX, i.e. the cursor
 * column's erase rect may never reach the pixel column the item name's first glyph
 * starts at. BACKLOG #42/#43 (Guy, 2026-09-05): it used to be off by one (erase width
 * 8 against a name x of list_x0+9, so the erase's rightmost column WAS the name's
 * first column) -- a cursor-only repaint (any UP/DOWN that does not also scroll, not
 * just a scroll) erased the leftmost ink column of every row's first letter, because
 * bg_restore's rect spans the WHOLE list height, every row, every time the cursor
 * moves. The static assert below pins the fixed relationship so it can't regress
 * silently the next time either offset is tuned. */
#define BAG_CUR_DX   2                            /* cursor '>' x offset from list_x0 */
#define BAG_CUR_W    7                            /* cursor column erase width        */
#define BAG_NAME_DX  9                            /* name column x offset (draw_list's nx) */
_Static_assert(BAG_CUR_DX + BAG_CUR_W <= BAG_NAME_DX,
              "bag cursor erase rect must not reach the item-name column");

/* the '>' cursor column only (selection moved within the visible page) */
static void draw_cursor(BgFrame bg, const BagLayout* L, int top, int sel) {
  bg_restore(bg, L->list_x0 + BAG_CUR_DX, L->list_y0, BAG_CUR_W, L->list_y1 - L->list_y0);
  ui_text(L->list_x0 + BAG_CUR_DX, L->list_y0 + 1 + (sel - top) * 9, BCUR, ">");
}

/* whole list pane: name + right-aligned quantity per slot (name columns fill
 * whatever the pane leaves after the "x999" gutter). The Key Items pocket
 * shows no quantity, matching the games. */
static void draw_list(BgFrame bg, const BagLayout* L, const uint8_t* sb1,
                      const uint8_t* sb2, PkGame g, int pocket, int top, int sel) {
  bg_restore(bg, L->list_x0, L->list_y0,
                L->list_x1 - L->list_x0, L->list_y1 - L->list_y0);
  int cap = pk_pocket_cap(g, pocket);
  int nx = L->list_x0 + BAG_NAME_DX;             /* name column, just past the '>' */
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

/* Every one of these calls fetches its OWN BgFrame right at the call site,
 * immediately before the blit that uses it — bag_bg(game, female), never a
 * `bg` local hoisted out and reused. This is not stylistic: see rom_chrome.h's
 * bag section and pdna_bag.c's rom_bag_frame() comment above. The compiled-
 * art rung is a cheap, stateless lookup, so this costs nothing extra when
 * PDNA_BAG_ART_COMPILED; the ROM rung genuinely redecodes every time, which is
 * the price of correctness against the interleaved item-icon decoder. */
bool bag_screen(uint8_t* sb1, const uint8_t* sb2, PkGame game, int female) {
  if (game < 0 || game > 2 || !bag_bg(game, female).blob) return false; /* defensive: callers gate on bag_bg() */
  const BagLayout* L = &BAG_LAYOUTS[game];
  int pocket = 0, sel = 0, top = 0, prev_sel = -1, desc_pg = 0;
  bool dirty = false, full = true, list = true, desc = true;

  for (;;) {
    int cap = pk_pocket_cap(game, pocket);
    if (sel >= cap) sel = cap - 1;
    if (sel < top)             { top = sel; list = true; }
    if (sel >= top + BROWS(L)) { top = sel - (BROWS(L) - 1); list = true; }

    if (full) {                                  /* whole chrome (entry / after picker+dialogs) */
      bg_restore(bag_bg(game, female), 0, 0, BAG_BG_W, BAG_BG_H);   /* 20 LZ77 pages ROM->VRAM */
      bag_rest(L, game, female, pocket);         /* bg bakes the CLOSED bag; show the pocket open */
      if (L->foot_y)                             /* only where the chrome leaves a free strip */
        ui_text(L->foot_x, L->foot_y, BFOOT, "A edit  L/R pocket  B done");
      draw_header(bag_bg(game, female), L, pocket);
      list = true; full = false;
    }
    if (list) { draw_list(bag_bg(game, female), L, sb1, sb2, game, pocket, top, sel); list = false; desc = true; }
    else if (sel != prev_sel) { draw_cursor(bag_bg(game, female), L, top, sel); desc = true; }
    if (desc) { desc_pg = 0; draw_desc(bag_bg(game, female), L, sb1, game, pocket, sel, desc_pg); desc = false; }
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
          draw_desc(bag_bg(game, female), L, sb1, game, pocket, sel, desc_pg);
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
      pocket_anim(bag_bg(game, female), L, game, female, pocket);  /* closed bag pops + the pocket opens */
      draw_header(bag_bg(game, female), L, pocket);
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
