/*
 * Trainer card / stats screen for PokeDNA. With the vendored card art the
 * REAL in-game card of the loaded save's own game (RS / Emerald / FRLG,
 * per-game layout tables in card_bg.h) IS the editor: a red selection frame
 * moves with U/D across the fields drawn on the card front (IDNo, name,
 * stars, money, play time, gender photo, badge row) and A edits the field in
 * place — SEX flips instantly (the card art swap is the feedback) and A on
 * the badge row toggles the badge under the LEFT/RIGHT badge cursor. L or R
 * flips to the real card BACK (HoF debut, link W/L, trades, and the game's
 * own extra rows), all editable even at 0; B on the back returns to the
 * front, B on the front exits with the one save prompt. Art-free builds keep
 * the plain details/edit page (trainer identity, money, play time, Pokédex
 * counts, HoF first-clear time, Game Records, star achievements — the latter
 * toggled honestly via gen3_stars).
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_trainer.h"
#include "ui.h"
#include "snd.h"
#include "osk.h"
#include "pdna_app.h"
#include "gen3_flags.h"   /* badge / frontier flag toggles */
#include "gen3_stars.h"   /* star achievements (the card's tier)  */
#include "card_bg.h"      /* real Emerald card front (weak NULLs) */
#include "rumble.h"       /* io-suspend around the big bg DMA     */
#include "rom_chrome_gate.h"
#if !PDNA_CARD_ART_COMPILED
#include "rom_chrome.h"   /* the ROM rung -- see rom_chrome.h for scope/why       */
#include "artbuf.h"       /* the ONE shared 8 KiB EWRAM decode buffer, no new one */
#endif

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R)) snd_move();
  else if (k & KEY_A) snd_ok();
  else if (k & KEY_B) snd_back();
  return k;
}

/* numeric entry via the on-screen keyboard; returns `cur` on cancel. */
static uint32_t num_entry(const char* prompt, uint32_t cur, uint32_t maxv) {
  char init[12], out[12];
  siprintf(init, "%lu", (unsigned long)cur);
  if (!osk_search(prompt, init, out, sizeof(out))) return cur;
  uint32_t v = 0;
  for (const char* p = out; *p >= '0' && *p <= '9'; p++) v = v * 10 + (uint32_t)(*p - '0');
  return v > maxv ? maxv : v;
}

/* badges (0..7) then the Emerald Battle-Frontier symbols (8..21). */
static const char* const BADGEFRONT_LBL[22] = {
  "Badge 1", "Badge 2", "Badge 3", "Badge 4", "Badge 5", "Badge 6", "Badge 7", "Badge 8",
  "Tower Silver", "Tower Gold", "Dome Silver", "Dome Gold", "Palace Silver", "Palace Gold",
  "Arena Silver", "Arena Gold", "Factory Silver", "Factory Gold", "Pike Silver", "Pike Gold",
  "Pyramid Silver", "Pyramid Gold",
};
static int badge_or_frontier_flag(PkGame g, int i) {
  return i < 8 ? pk_badge_flag(g, i) : pk_frontier_flag(g, i - 8);   /* -1 if absent */
}

/* What is on screen. A standard cursor list with a scroll window (count can be 22 for
 * Emerald's badges+frontier): `top`/`sel` are the whole state a keypress moves on its
 * own, same shape as pdna_legality.c's SweepPaint. No header counter on this screen
 * (unlike stars_editor/pdna_fly.c below, this list prints no derived total). Stack-
 * local, not a static: a fresh call always starts invalid (first pass paints in full). */
typedef struct { uint32_t gen; int top, sel; bool valid; } FlagPaint;

/* One row. Self-contained: the selected row IS ui_panel's own fill+border (236x9, the
 * same rect the old full repaint always drew); the unselected row wipes to UI_BG first
 * -- same ghost-ink guard as every other row painter in this codebase, needed because a
 * row can go from selected (padded string, SELTEXT ink) to unselected (OK/DIM ink)
 * between two draws of the same slot. No pairing trap: the panel height (9) exactly
 * matches the row pitch (9), unlike pdna_edit.c's row_paint, so a row's own repaint can
 * never touch a neighbour. */
static void flag_row_paint(uint8_t* sb1, PkGame game, int (*flagnum)(PkGame, int),
                           const char* const* names, int idx, int y, bool sel) {
  int fn = flagnum(game, idx);
  bool on = fn >= 0 && pk_flag_get(sb1, game, fn);
  char row[40]; siprintf(row, "%-15s %s", names[idx], on ? "ON" : "off");
  if (sel) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
  else     ui_fill_rect(2, y - 1, 236, 9, UI_BG);
  ui_text(8, y, sel ? UI_SELTEXT : (on ? UI_OK : UI_DIM), row);
}

/* On/off toggler for a set of flags (badges / frontier symbols). Returns true if
 * anything changed. Edits SaveBlock1 flags in place; the caller commits SB1. */
static bool flag_set_editor(uint8_t* sb1, PkGame game, const char* title,
                            int (*flagnum)(PkGame, int), const char* const* names, int count) {
  int sel = 0, top = 0; bool changed = false;
  const int vis = 15;
  FlagPaint pv;
  memset(&pv, 0, sizeof pv);          /* .valid = false: the first pass paints in full */
  for (;;) {
    if (sel < top) top = sel; if (sel >= top + vis) top = sel - vis + 1;

    /* `top` unchanged also proves `sel` (old and new) is still inside the visible
     * window -- see pdna_legality.c's sweep_screen for why that makes the row-pair
     * repaint below safe without a bounds check. */
    bool full = !pv.valid || pv.gen != ui_clear_gen() || top != pv.top;

    if (full) {
      ui_clear();
      ui_text(4, 2, UI_TITLE, title);
      ui_hline(0, 11, UI_SCR_W, UI_BORDER);
      for (int i = 0; i < vis && top + i < count; i++)
        flag_row_paint(sb1, game, flagnum, names, top + i, 16 + i * 9, top + i == sel);
      ui_text(4, 152, UI_DIM, "A toggle  U/D  B back");
    } else if (sel != pv.sel) {
      flag_row_paint(sb1, game, flagnum, names, pv.sel, 16 + (pv.sel - top) * 9, false);
      flag_row_paint(sb1, game, flagnum, names, sel,    16 + (sel    - top) * 9, true);
    }

    pv.top = top; pv.sel = sel; pv.gen = ui_clear_gen(); pv.valid = true;

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return changed;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : count - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % count;
    else if (k & KEY_A) {
      int fn = flagnum(game, sel);
      if (fn >= 0) {
        pk_flag_set(sb1, game, fn, !pk_flag_get(sb1, game, fn)); changed = true;
        /* toggled at the cursor, which did not move: the top-of-loop diff above only
         * catches a `sel` change, so repaint this ONE row by hand -- same idiom
         * card_editor's own CARDF_BADGES case uses a few screens down in this file. */
        flag_row_paint(sb1, game, flagnum, names, sel, 16 + (sel - top) * 9, true);
      }
    }
  }
}

/* ---- the real per-game card front (generated art; tools/gen_card_bg.py) ---- */

/* Strong card_bg()/card_badge16()/card_hoenn_dex() come from the GENERATED
 * card_bg.c (git-ignored ripped art); these weak NULLs keep an art-free clone
 * building — the screen then opens straight on the plain page (same pattern
 * as the bag-screen fallback in pdna_bag.c). */
/* Registered by app_icon_rom_open() on every ROM (re)registration, in BOTH
 * builds -- rom_chrome.h's promise that the setter is "safe to call even in a
 * full-art build". Kept OUTSIDE the PDNA_CARD_ART_COMPILED gate (unlike
 * everything else below) purely so that promise holds: a full-art build simply
 * never reads s_romchrome again (card_bg()'s strong symbol wins the link and
 * never calls rom_card_frame at all), so this costs one pointer store. */
static const RomChrome* s_romchrome = 0;

void pdna_trainer_set_romchrome(const RomChrome* rch) {
  s_romchrome = rch;
}

#if !PDNA_CARD_ART_COMPILED
/* The ROM rung (Emerald + Ruby only -- rom_chrome.h states why; Sapphire/FRLG
 * fall through to blob==0 exactly as before). NULL s_romchrome means no ROM open
 * this session, or the open ROM cannot serve `game`'s card style.
 *
 * ALWAYS redecodes -- NOT memoised by (game, back, tier, female), even though a
 * naive key check looks safe here. It is NOT: mon_decomp is the shared 8 KiB
 * EWRAM staging buffer (artbuf.h) that the box wallpaper, mon icons, item icons
 * and the summary portrait ALL overwrite between visits to this screen. A memo
 * that survives across visits to pdna_trainer() would compare today's key
 * against a key computed on a LAST visit, find them equal, and hand back a
 * pointer into whatever those other screens left in mon_decomp since --
 * garbage tiles rendered as the card. This is documented, and already fixed
 * once, for the summary portrait: see pdna_origin_art.c's rom_portrait()
 * ("DELIBERATELY NOT MEMOISED ... mon_decomp is shared ... a STALE pointer --
 * same key"). The same rule applies here.
 *
 * The decode target (s_chrome_card, IWRAM .bss, ~215 B -- nowhere near the
 * ~1,232 B new-IWRAM-.bss crash threshold DESIGN.md Sec 4.2 measured) is still
 * a file-static so the caller's BgFrame stays valid for a screen's whole
 * lifetime -- card_editor() calls this only on `full` repaints (screen entry,
 * an L/R face flip, an editor that used the whole screen), which is
 * user-driven, not per-frame, so re-decoding every time costs nothing a player
 * would notice and buys correctness instead of a plausible-looking corrupted
 * card. */
static RomChromeCard s_chrome_card;

static BgFrame rom_card_frame(int game, int back, int tier, int female) {
  BgFrame f = { 0, 0, CARD_BG_W };
  if (!s_romchrome || !rom_chrome_card_have(s_romchrome, game)) return f;
  artbuf_claim();          /* E3 review BLOCKING 2: about to overwrite mon_decomp */
  if (!rom_chrome_card_load(s_romchrome, game, back, tier, female,
                            (uint8_t*)mon_decomp, MON_DECOMP_BYTES, &s_chrome_card))
    return f;
  f.blob = &s_chrome_card.as_lzblob; f.off = 0; f.sw = CARD_BG_W;
  return f;
}
#endif /* !PDNA_CARD_ART_COMPILED */

__attribute__((weak)) BgFrame card_bg(int game, int tier, int female) {
#if !PDNA_CARD_ART_COMPILED
  return rom_card_frame(game, 0, tier, female);
#else
  (void)game; (void)tier; (void)female; BgFrame f = { 0, 0, CARD_BG_W }; return f;
#endif
}
__attribute__((weak)) BgFrame card_bg_back(int game, int tier, int female) {
#if !PDNA_CARD_ART_COMPILED
  return rom_card_frame(game, 1, tier, female);
#else
  (void)game; (void)tier; (void)female; BgFrame f = { 0, 0, CARD_BG_W }; return f;
#endif
}
/* card_badge16()/card_hoenn_dex() stay compiled-art-only -- small icon sprites and
 * a lookup table, not "a full-screen background plus its tilemap and palette"
 * (the task this ROM rung covers); see rom_chrome.h's scope note. */
__attribute__((weak)) const uint16_t* card_badge16(int game, int i) { (void)game; (void)i; return 0; }
__attribute__((weak)) const uint16_t* card_hoenn_dex(void) { return 0; }

/* ---- ROM-streamed FOREGROUND art: stars / badges / photo ------------------
 *
 * A CORRECTION to what this file used to assume (see the old comment this
 * replaces, still visible in git blame): "STARS and SEX are baked into the
 * frame" is true for SEX (the whole card front/back swaps) but WRONG for
 * STARS. The tier passed to card_bg() only selects the border/surround
 * PALETTE (tools/gen_card_bg.py's own docstring: "the tier is palette-only");
 * the star GLYPHS are a runtime overlay even in the COMPILED build --
 * gen_card_bg.py's draw_star() stamps `tier` copies of tile 143 at build
 * time, using a palette (star.pal) that is never part of the tier's own 3
 * banks. A frame with 0 stars and a frame with 4 stars share the exact same
 * tilemap; only the offline compositor (or, here, the runtime one) puts the
 * stars there. This was invisible in the compiled build (the star pixels are
 * simply always present in whichever pre-baked frame was chosen) and only
 * showed up as a real gap once the ROM rung had to draw them itself.
 *
 * badges.png / the photo were never wired at all -- rom_chrome.h's own scope
 * note said so plainly ("this module deliberately does not wire
 * card_badge16() ... so nothing else touches the buffer"). All three ROM
 * addresses were located 2026-08-22 (rom_chrome.c's CardBadgePins/
 * CardPhotoPins tables document each one).
 *
 * THE EWRAM TRAP THIS ALMOST SHIPPED WITH: a first pass gave badges/photo
 * their OWN new static scratch buffer (2,080 B, the photo's worst case),
 * reasoning that mon_decomp is busy holding the card's own tileset+tilemaps+
 * palette (up to ~7,776 B on Ruby) for card_restore()'s partial re-blits. The
 * artless build's actual EWRAM headroom that day was 748 B, not the "~10 KB"
 * this project's own brief warns is stale and must be measured -- `make
 * rebuild` with the generated art hidden failed outright: "FATAL: EWRAM
 * OVERFLOW ... 1,332 bytes past the end". A NEW static buffer was never
 * affordable here.
 *
 * The fix is not a smaller buffer -- badges (1,024 B tileset alone) and the
 * photo (2,048 B) still do not fit in 748 B. It is to stop assuming
 * mon_decomp's card-background residency ever safely outlives a SINGLE
 * decode-then-consume step, and follow the SAME rule pdna_bag.c's bag chrome
 * already lives by: badges/photo decode INTO mon_decomp (the existing 8,192 B
 * buffer, zero new EWRAM), clobbering whatever the card's tileset/tilemaps
 * were doing there -- so card_restore()/card_field() below no longer trust a
 * stale `bg` handle across a call boundary; every restore re-fetches
 * card_bg()/card_bg_back() FRESH (cheap in the compiled build: a stateless
 * lookup into the pre-baked LZ77-paged blob; a real but small re-decode in
 * the ROM build, same class of cost this file already pays per cursor move
 * on the badge cursor). See card_restore()'s own comment for the sequencing
 * this buys: decode bg -> blit -> [mon_decomp free again] -> decode+draw the
 * field's own overlay (badges/stars/photo), never two decodes resident at
 * once. */

static void card_draw_stars(PkGame game, int tier) {
#if !PDNA_CARD_ART_COMPILED
  if (!s_romchrome || !rom_chrome_card_have(s_romchrome, (int)game)) return;
  static const int16_t k_star_id = ROM_CHROME_STAR_TILE;
  const CardLayout* L = &CARD_LAYOUTS[game];
  int sx = L->rect[CARDF_STARS].x + 3, sy = L->rect[CARDF_STARS].y + 3;   /* ink box
                                                                          * -3 px pad
                                                                          * (card_bg.h) */
  if (tier > 4) tier = 4;
  /* Reads s_chrome_card DIRECTLY (not a fresh decode of its own): correct
   * only because every caller (card_field()'s CARDF_STARS case) runs
   * immediately after a card_bg()/card_bg_back() call that just populated it,
   * with nothing else touching mon_decomp in between -- see card_field()'s
   * own call order. */
  for (int s = 0; s < tier; s++)
    romchrome_blit_tiles(s_chrome_card.src.tiles, s_chrome_card.star_pal,
                         &k_star_id, 1, 1, sx + 8 * s, sy);
#else
  (void)game; (void)tier;
#endif
}

static void card_draw_badge(PkGame game, int i, int x, int y) {
#if !PDNA_CARD_ART_COMPILED
  if (s_romchrome && rom_chrome_card_badges_have(s_romchrome, (int)game)) {
    RomChromeCardBadges b;
    artbuf_claim();          /* E3 review BLOCKING 2: about to overwrite mon_decomp */
    if (rom_chrome_card_badges_load(s_romchrome, (int)game,
                                    (uint8_t*)mon_decomp, MON_DECOMP_BYTES, &b)) {
      int16_t ids[4];
      if (rom_chrome_card_badge_ids(s_romchrome, (int)game, i, ids))
        romchrome_blit_tiles(b.tiles, b.pal, ids, 2, 2, x, y);
      return;
    }
  }
#endif
  ui_sprite(x, y, 16, 16, card_badge16((int)game, i));   /* compiled-art path (or a
                                                          * no-ROM artless no-op) */
}

static void card_draw_photo(PkGame game, int female) {
#if !PDNA_CARD_ART_COMPILED
  if (!s_romchrome || !rom_chrome_card_photo_have(s_romchrome, (int)game)) return;
  RomChromeCardPhoto ph;
  artbuf_claim();          /* E3 review BLOCKING 2: about to overwrite mon_decomp */
  if (!rom_chrome_card_photo_load(s_romchrome, (int)game, female,
                                  (uint8_t*)mon_decomp, MON_DECOMP_BYTES, &ph)) return;
  const CardLayout* L = &CARD_LAYOUTS[game];
  int px = L->rect[CARDF_SEX].x + 3, py = L->rect[CARDF_SEX].y + 3;   /* photo is a
                                                                       * 64x64 ink box,
                                                                       * -3 px pad */
  romchrome_blit_tiles(ph.tiles, ph.pal, 0, 8, 8, px, py);
#else
  (void)game; (void)female;   /* compiled art already baked the photo into card_bg() */
#endif
}

#define CINK   RGB15( 9,  9, 10)   /* card text (the games' dark-gray ink) */
#define CFOOT  RGB15(31, 31, 31)   /* footer hint on the card border       */
#define CSEL   RGB15(26,  4,  3)   /* selection frame (pdna_bag's red)     */

/* Paint the whole card. The frame used to be a raw 240x160 block in ROM,
 * copied row by row with a volatile re-read + byte-compare (wp_copy_verified
 * discipline), because a 75 KB bulk ROM read is the shape this cartridge
 * demonstrably glitches. That guard compared one ROM read against another and
 * cannot survive compression: the frames now ship LZ77-paged (source/lzblob.c),
 * so a bad ROM read corrupts the stream, not one pixel row. What replaces it is
 * the boot-time ROM self-check stamp, which tests the whole image once and says
 * "re-copy me" — and shipping 4.0 MB less ROM is what makes the bad load rare
 * in the first place. Each page is 8 screen rows and lands directly in VRAM. */
static void card_blit(BgFrame bg) {
  bg_restore(bg, 0, 0, CARD_BG_W, CARD_BG_H);
}

/* The cursor-editable fields ON the card (CARDF_* order) own the per-game
 * screen rects in CARD_LAYOUTS[game].rect: the selection frame is drawn on
 * the rect's edge and a move/redraw restores exactly this rect from the ROM
 * bg. All geometry comes from card_bg.h — nothing game-specific here. */

/* Draw one field's runtime overlay at the game's own position (card_bg.h);
 * SEX itself is baked into the frame (the whole card front/back swaps), but
 * unlike RS/E/FRLG's compiled build the ROM rung's card_bg() does NOT bake
 * the trainer photo into that swap (card_draw_photo() draws it as a runtime
 * overlay -- rom_chrome.h's photo section), so CARDF_SEX still has a case
 * below: redraw the photo, purely cosmetic in the compiled build (it is
 * already part of the frame there, so this just repaints the same pixels)
 * but load-bearing in the ROM build. STARS is likewise NOT baked in -- see
 * card_draw_stars()'s comment above for why that used to be assumed and was
 * wrong. RS bakes the NAME/IDNo./MONEY/TIME labels into the art
 * (L->labels == 0), so only the bare values are drawn there. */
static void card_field(int f, PkGame game, int tier, int female, const uint8_t* sb1,
                       const char* name, uint16_t tid, uint32_t money, uint16_t ph, uint8_t pm) {
  const CardLayout* L = &CARD_LAYOUTS[game];
  char line[32];
  switch (f) {
    case CARDF_ID:                           /* E/FRLG write "IDNo." + id; RS
                                              * bakes the label, digits only  */
      siprintf(line, L->labels ? "IDNo.%05u" : "%05u", (unsigned)tid);
      if (L->id_w)                           /* Emerald: centered in the box  */
        ui_text(L->id_x + (L->id_w - 8 * (int)strlen(line)) / 2, L->id_y, CINK, line);
      else
        ui_text(L->id_x, L->id_y, CINK, line);
      break;
    case CARDF_NAME:
      if (L->labels) siprintf(line, "NAME: %s", name);   /* gText_TrainerCardName */
      else           siprintf(line, "%s", name);
      ui_text(L->name_x, L->name_y, CINK, line);
      break;
    case CARDF_MONEY:
      if (L->labels) ui_text(L->lbl_x, L->money_y, CINK, "MONEY");
      siprintf(line, "$%lu", (unsigned long)money);
      ui_text(L->val_xr - 8 * (int)strlen(line), L->money_y, CINK, line);
      break;
    case CARDF_TIME:
      if (L->labels) ui_text(L->lbl_x, L->time_y, CINK, "TIME");
      siprintf(line, "%u:%02u", (unsigned)ph, (unsigned)pm);
      ui_text(L->val_xr - 8 * (int)strlen(line), L->time_y, CINK, line);
      break;
    case CARDF_SEX:                          /* ROM rung only: repaint the photo
                                              * this field's rect covers (see the
                                              * function comment above) */
      card_draw_photo(game, female);
      break;
    case CARDF_STARS:                        /* tier's star row -- NOT baked
                                              * into the frame (that was a
                                              * wrong assumption this file
                                              * carried: the frame only bakes
                                              * the PALETTE tier/border; the
                                              * star GLYPHS themselves are a
                                              * shared tile drawn `tier` times
                                              * at runtime, exactly as
                                              * tools/gen_card_bg.py's own
                                              * draw_star() does at build
                                              * time for the compiled art). */
      card_draw_stars(game, tier);
      break;
    case CARDF_BADGES:                       /* badges overlay only when owned
                                              * (the baked empty slots keep the
                                              * games' own 1..8 digit marks) */
      for (int i = 0; i < 8; i++)
        if (pk_flag_get(sb1, game, pk_badge_flag(game, i)))
          card_draw_badge(game, i, L->badge_x + 24 * i, L->badge_y);
      break;
  }
}

/* A field's cursor rect. On the badge row the cursor owns ONE 16x16 badge
 * cell (bsel 0..7) — A toggles exactly that badge, so the frame hugs it. */
static void card_rect(PkGame game, int f, int bsel, int* x, int* y, int* w, int* h) {
  const CardLayout* L = &CARD_LAYOUTS[game];
  if (f == CARDF_BADGES) { CARD_BADGE_RECT(L, bsel, *x, *y, *w, *h); return; }
  *x = L->rect[f].x; *y = L->rect[f].y; *w = L->rect[f].w; *h = L->rect[f].h;
}

/* 2 px red selection frame on a field's rect edge (over the card art). */
/* ---- identity edits and record mixing -------------------------------------
 * Record mixing does NOT match a secret base by the base's own id. It matches on the
 * owner's GENDER, the 4-byte trainer ID (public + secret) and the trainer NAME. Change
 * any of those and every cart that has already mixed with you keeps a base it can no
 * longer match to you: your base is stranded there under the old identity, and the copy
 * you mixed back stops tracking. Nothing in the save warns about it, so we do — ONCE per
 * visit, before the first such edit. After that the edits stay instant, which is how the
 * card page is meant to feel. */
static bool s_id_warned;

static bool id_edit_ok(void) {
  if (s_id_warned) return true;
  s_id_warned = true;
  return app_confirm("Changes your MIXING identity",
                     "Bases you already mixed get orphaned.");
}

static void card_sel_frame(PkGame game, int f, int bsel) {
  int x, y, w, h; card_rect(game, f, bsel, &x, &y, &w, &h);
  m3_frame(x, y, x + w, y + h, CSEL);
  m3_frame(x + 1, y + 1, x + w - 1, y + h - 1, CSEL);
}

/* restore a field's rect from the ROM bg and repaint its overlay (no smear).
 * +1 px right/bottom so the selection frame is erased whichever edge
 * convention m3_frame uses (all rects stay on-screen with the margin). */
/* Fetches its OWN card_bg() FRESH rather than trusting a `bg` handle the
 * caller already holds -- required correctness in the ROM build, not style:
 * card_draw_badge()/card_draw_photo() (called from card_field() below, for
 * the very field this function is about to restore) decode INTO mon_decomp,
 * the SAME buffer card_bg()'s tileset/tilemaps/palette live in, so a `bg`
 * captured before an earlier badge/photo draw would read clobbered bytes.
 * Sequencing per call: decode bg -> bg_restore() [reads it, done] ->
 * card_field() [may decode a DIFFERENT thing into the same buffer, fine --
 * nothing after this call needs `bg` again until the NEXT card_restore()].
 * Cheap in the compiled build (a stateless lookup); a real but small (a few
 * KB) re-decode in the ROM build, the same class of cost this file already
 * pays on every badge-cursor L/R press. */
static void card_restore(int f, int bsel, PkGame game, int tier, int female,
                         const uint8_t* sb1, const char* name, uint16_t tid,
                         uint32_t money, uint16_t ph, uint8_t pm) {
  int x, y, w, h; card_rect(game, f, bsel, &x, &y, &w, &h);
  BgFrame bg = card_bg(game, tier, female);
  bg_restore(bg, x, y, w + 1, h + 1);
  card_field(f, game, tier, female, sb1, name, tid, money, ph, pm);
}

/* ---- the card BACK (L/R flips; geometry + row model in card_bg.h) ---- */

static uint32_t back_stat(const uint8_t* sb1, const uint8_t* sb2, PkGame game, int stat) {
  return pk_game_stat(sb1, sb2, game, stat);
}

static void back_row_value(const CardBackRow* r, PkGame game, const uint8_t* sb1,
                           const uint8_t* sb2, char* out) {
  switch (r->kind) {
    case CBK_HOF: {                          /* shown 0:00:00 when never entered */
      uint16_t h = 0; uint8_t m = 0, s = 0;
      pk_hof_time(sb1, sb2, game, &h, &m, &s);
      siprintf(out, "%u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
    } break;
    case CBK_WL:
      siprintf(out, "W%lu L%lu",
               (unsigned long)back_stat(sb1, sb2, game, r->stat),
               (unsigned long)back_stat(sb1, sb2, game, r->stat + 1));
      break;
    case CBK_STAT:
      siprintf(out, "%lu", (unsigned long)back_stat(sb1, sb2, game, r->stat));
      break;
    case CBK_TOWER_RS:
      siprintf(out, "W%u S%u", (unsigned)pk_rs_tower(sb2, 0), (unsigned)pk_rs_tower(sb2, 1));
      break;
    case CBK_BP_E:
      siprintf(out, "%u BP", (unsigned)pk_e_card_bp(sb2));
      break;
  }
}

static void back_rect(PkGame game, int row, int* x, int* y, int* w, int* h) {
  const CardBackLayout* B = &CARD_BACK_LAYOUTS[game];
  *x = B->lbl_x - 3; *y = B->rows[row].y - 3;
  *w = B->val_xr - B->lbl_x + 6; *h = 14;
}

static void back_sel_frame(PkGame game, int row) {
  int x, y, w, h; back_rect(game, row, &x, &y, &w, &h);
  m3_frame(x, y, x + w, y + h, CSEL);
  m3_frame(x + 1, y + 1, x + w - 1, y + h - 1, CSEL);
}

static void back_row_draw(int row, PkGame game, const uint8_t* sb1, const uint8_t* sb2) {
  const CardBackLayout* B = &CARD_BACK_LAYOUTS[game];
  char v[24];
  ui_text(B->lbl_x, B->rows[row].y, CINK, B->rows[row].label);
  back_row_value(&B->rows[row], game, sb1, sb2, v);
  ui_text(B->val_xr - 8 * (int)strlen(v), B->rows[row].y, CINK, v);
}

static void back_restore(BgFrame bg, int row, PkGame game,
                         const uint8_t* sb1, const uint8_t* sb2) {
  int x, y, w, h; back_rect(game, row, &x, &y, &w, &h);
  bg_restore(bg, x, y, w + 1, h + 1);
  back_row_draw(row, game, sb1, sb2);
}

/* Edit one back row (the front editors' exact prompt style; RAM-only until
 * the caller's single exit commit). Sets *d1 (gameStats, SB1) / *d2 (SB2). */
static void back_row_edit(const CardBackRow* r, PkGame game, uint8_t* sb1,
                          uint8_t* sb2, bool* d1, bool* d2) {
  switch (r->kind) {
    case CBK_HOF: {
      uint16_t h = 0; uint8_t m = 0, s = 0;
      pk_hof_time(sb1, sb2, game, &h, &m, &s);
      h = (uint16_t)num_entry("HOF DEBUT HOURS", h, 999);
      m = (uint8_t)num_entry("HOF DEBUT MINUTES", m, 59);
      s = (uint8_t)num_entry("HOF DEBUT SECONDS", s, 59);
      uint32_t packed = ((uint32_t)h << 16) | ((uint32_t)m << 8) | s;
      pk_set_game_stat(sb1, sb2, game, PK_STAT_FIRST_HOF_PLAY_TIME, packed);
      /* the games gate the row (and the HoF star) on ENTERED_HOF: a nonzero
       * debut time needs at least one entry, 0:00:00 = honestly never entered */
      uint32_t entered = pk_game_stat(sb1, sb2, game, PK_STAT_ENTERED_HOF);
      if (packed && !entered)      pk_set_game_stat(sb1, sb2, game, PK_STAT_ENTERED_HOF, 1);
      else if (!packed && entered) pk_set_game_stat(sb1, sb2, game, PK_STAT_ENTERED_HOF, 0);
      *d1 = true;
    } break;
    case CBK_WL: {
      uint32_t w = num_entry("LINK WINS", back_stat(sb1, sb2, game, r->stat), r->cap);
      uint32_t l = num_entry("LINK LOSSES", back_stat(sb1, sb2, game, r->stat + 1), r->cap);
      pk_set_game_stat(sb1, sb2, game, r->stat, w);
      pk_set_game_stat(sb1, sb2, game, r->stat + 1, l);
      *d1 = true;
    } break;
    case CBK_STAT:
      pk_set_game_stat(sb1, sb2, game, r->stat,
                       num_entry(r->label, back_stat(sb1, sb2, game, r->stat), r->cap));
      *d1 = true;
      break;
    case CBK_TOWER_RS: {
      uint16_t w = (uint16_t)num_entry("TOWER WINS", pk_rs_tower(sb2, 0), r->cap);
      uint16_t s = (uint16_t)num_entry("TOWER STREAK", pk_rs_tower(sb2, 1), r->cap);
      pk_set_rs_tower(sb2, 0, w);
      pk_set_rs_tower(sb2, 1, s);
      *d2 = true;
    } break;
    case CBK_BP_E:
      pk_set_e_card_bp(sb2, (uint16_t)num_entry("BATTLE POINTS", pk_e_card_bp(sb2), r->cap));
      *d2 = true;
      break;
  }
}

/* What is on screen. At most PK_STAR_ACH_MAX (4) rows, all visible at once -- no
 * scroll window here, unlike flag_set_editor's badge list -- so `sel` is the whole
 * cursor state, plus the header's own star count (derived from the same underlying
 * flags every row reads: turning a dex-based star OFF un-catches species, so a toggle
 * that leaves `sel` in place can still move this number). Stack-local, not a static: a
 * fresh call always starts invalid (first pass paints in full). */
typedef struct { uint32_t gen; int sel, scount; bool valid; } StarsPaint;

/* One row -- same wipe-then-draw shape as flag_row_paint above (panel height 9 on a
 * 9 px pitch, no neighbour bleed). */
static void star_row_paint(uint8_t* sb1, uint8_t* sb2, PkGame game, const uint16_t* dex,
                           int idx, int y, bool sel) {
  bool can = pk_star_ach_can_set(game, idx, dex);
  bool on  = pk_star_ach_done(sb1, sb2, game, idx, dex);
  char row[40]; siprintf(row, "%-16s %s", pk_star_ach_name(game, idx),
                         !can ? "n/a" : on ? "ON" : "off");
  if (sel) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
  else     ui_fill_rect(2, y - 1, 236, 9, UI_BG);
  ui_text(8, y, sel ? UI_SELTEXT : (can && on ? UI_OK : UI_DIM), row);
}

/* The STARS row's sub-editor: this game's 4 star achievements (each ONE card
 * star / palette tier), toggled honestly in the underlying save data via
 * gen3_stars. Returns a dirty mask (1 = SB1, 2 = SB2); the caller commits. */
static int stars_editor(uint8_t* sb1, uint8_t* sb2, PkGame game) {
  const uint16_t* dex = card_hoenn_dex();    /* NULL in an art-free build */
  const int n = pk_star_ach_count(game);
  int sel = 0, dirty = 0;
  StarsPaint pv;
  memset(&pv, 0, sizeof pv);          /* .valid = false: the first pass paints in full */
  for (;;) {
    int scount = pk_star_count(sb1, sb2, game, dex);
    bool full = !pv.valid || pv.gen != ui_clear_gen();

    if (full) {
      ui_clear();
      char t[32]; siprintf(t, "CARD STARS  %d/4", scount);
      ui_text(4, 2, UI_TITLE, t);
      ui_hline(0, 11, UI_SCR_W, UI_BORDER);
      for (int i = 0; i < n; i++)
        star_row_paint(sb1, sb2, game, dex, i, 16 + i * 9, i == sel);
      ui_text(4, 62, UI_DIM, "Each ON = one star (card color).");
      if (!dex) ui_text(4, 72, UI_DIM, "n/a: needs the generated art data.");
      ui_text(4, 152, UI_DIM, "A toggle  U/D  B back");
    } else {
      if (sel != pv.sel) {
        star_row_paint(sb1, sb2, game, dex, pv.sel, 16 + pv.sel * 9, false);
        star_row_paint(sb1, sb2, game, dex, sel,    16 + sel    * 9, true);
      }
      if (scount != pv.scount) {
        char t[32]; siprintf(t, "CARD STARS  %d/4", scount);
        ui_fill_rect(0, 2, UI_SCR_W, UI_ROW_H, UI_BG);
        ui_text(4, 2, UI_TITLE, t);
      }
    }

    pv.sel = sel; pv.scount = scount; pv.gen = ui_clear_gen(); pv.valid = true;

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return dirty;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
    else if (k & KEY_A) {
      if (!pk_star_ach_can_set(game, sel, dex)) continue;
      bool on = pk_star_ach_done(sb1, sb2, game, sel, dex);
      if (on && pk_star_ach_is_dex(game, sel) &&   /* dex stars: OFF un-catches */
          !app_confirm("Clear dex catches?", "Un-catches these species."))
        continue;
      dirty |= pk_star_ach_set(sb1, sb2, game, sel, !on, dex);
      /* NOT a single-row repaint: FRLG's achievements overlap (gen3_stars.c --
       * ACH_NATDEX's dex ranges 0..2 INCLUDE ACH_KANTO's range 0, so turning
       * NatDex on/off can silently flip the Kanto row's on-state too, leaving a
       * hand-repaint of just `sel` stale on a SIBLING row). Only 4 rows on this
       * screen, all CPU-drawn -- repaint every row plus the header on any
       * accepted toggle, which covers every cascade by construction and is
       * still far cheaper than the old unconditional ui_clear(). */
      int scount2 = pk_star_count(sb1, sb2, game, dex);
      for (int i = 0; i < n; i++)
        star_row_paint(sb1, sb2, game, dex, i, 16 + i * 9, i == sel);
      char t2[32]; siprintf(t2, "CARD STARS  %d/4", scount2);
      ui_fill_rect(0, 2, UI_SCR_W, UI_ROW_H, UI_BG);
      ui_text(4, 2, UI_TITLE, t2);
    }
  }
}

/* the one exit-save prompt; commits through the usual verified paths. */
static void trainer_commit(bool d1, bool d2) {
  if ((d1 || d2) && app_confirm("Save trainer changes?", "Writes the card edits now.")) {
    if (d1 && d2)  app_commit_sb12();        /* both blocks: ONE backup + write */
    else if (d2)   app_commit_sb2();         /* trainer block (section 0)     */
    else           app_commit_sb1();         /* money (sections 1..4)         */
  }
}

/* The card editor (any game with art): the selection frame walks the fields
 * ON the card and A edits in place with the plain page's exact editors and
 * validation; every full-screen editor forces a re-blit on return (gender/
 * star edits swap the frame itself). SEX and single badges toggle INSTANTLY
 * (the art swap / badge appearing is the feedback). L/R flips to the card
 * BACK (per-game stat rows, editable even at 0); B there returns to the
 * front, B on the front returns to the caller, which owns the one
 * trainer_commit prompt. */
static void card_editor(uint8_t* sb1, uint8_t* sb2, PkGame game, bool edit,
                        char* name, uint8_t* gender, uint16_t* tid, uint16_t* sid,
                        uint32_t* money, uint16_t* ph, uint8_t* pm,
                        bool* d1, bool* d2) {
  const CardLayout* L = &CARD_LAYOUTS[game];
  const int nback = CARD_BACK_LAYOUTS[game].nrows;
  int sel = CARDF_NAME, bsel = 0, brow = 0, tier = 0;
  bool back = false, full = true;
  BgFrame bg = { 0, 0, CARD_BG_W };
  for (;;) {
    if (full) {                              /* (re)blit + all overlays */
      tier = pk_star_count(sb1, sb2, game, card_hoenn_dex());
      if (!back) {
        bg = card_bg(game, tier, *gender);
        card_blit(bg);
        for (int f = 0; f < CARDF_NUM; f++)
          card_field(f, game, tier, *gender, sb1, name, *tid, *money, *ph, *pm);
        if (pk_flag_get(sb1, game, L->dex_flag)) {   /* dex row: display-only */
          char line[16]; int seen, caught; bool nat; pk_pokedex(sb2, &seen, &caught, &nat);
          if (L->labels) ui_text(L->lbl_x, L->dex_y, CINK, "POKeDEX");
          siprintf(line, "%d", caught);
          ui_text(L->val_xr - 8 * (int)strlen(line), L->dex_y, CINK, line);
        }
        ui_text(4, 152, CFOOT, edit ? "U/D A edit  L/R flip  B save"
                                    : "L/R flip  B back");
        if (edit) card_sel_frame(game, sel, bsel);
      } else {                               /* the card back */
        const CardBackLayout* B = &CARD_BACK_LAYOUTS[game];
        bg = card_bg_back(game, tier, *gender);
        card_blit(bg);
        char line[32];
        /* RS/E append "'s TRAINER CARD" (gText_Var1sTrainerCard); FRLG's own
         * back prints ONLY the name after its baked "TRAINER:" (pokefirered
         * trainer_card.c:1254-1261 BufferNameForCardBack: the suffix is
         * RSE-cards-only). */
        siprintf(line, game == PK_FRLG ? "%s" : "%s's TRAINER CARD", name);
        ui_text(B->name_right ? B->name_x - 8 * (int)strlen(line) : B->name_x,
                B->name_y, CINK, line);
        for (int r = 0; r < nback; r++)
          back_row_draw(r, game, sb1, sb2);
        ui_text(4, 152, CFOOT, edit ? "U/D A edit  B front" : "B front");
        if (edit) back_sel_frame(game, brow);
      }
      full = false;
    }

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT |
                   KEY_A | KEY_B | KEY_L | KEY_R | KEY_SELECT);
    if (k & (KEY_L | KEY_R)) { back = !back; full = true; continue; }
    if (k & KEY_B) {
      if (back) { back = false; full = true; continue; }
      return;                                /* front: exit to the save prompt */
    }
    if (!edit) continue;                     /* read-only: flip/view only */

    if (back) {                              /* ---- back page input ---- */
      if (k & (KEY_UP | KEY_DOWN)) {
        back_restore(bg, brow, game, sb1, sb2);
        brow = (k & KEY_UP) ? (brow > 0 ? brow - 1 : nback - 1) : (brow + 1) % nback;
        back_sel_frame(game, brow);
      } else if (k & KEY_A) {
        back_row_edit(&CARD_BACK_LAYOUTS[game].rows[brow], game, sb1, sb2, d1, d2);
        full = true;                         /* num_entry used the whole screen */
      }
      continue;
    }

    if (k & (KEY_UP | KEY_DOWN)) {           /* move the frame: restore + repaint */
      card_restore(sel, bsel, game, tier, *gender, sb1, name, *tid, *money, *ph, *pm);
      sel = (k & KEY_UP) ? (sel > 0 ? sel - 1 : CARDF_NUM - 1) : (sel + 1) % CARDF_NUM;
      card_sel_frame(game, sel, bsel);
    } else if ((k & (KEY_LEFT | KEY_RIGHT)) && sel == CARDF_BADGES) {
      card_restore(sel, bsel, game, tier, *gender, sb1, name, *tid, *money, *ph, *pm);
      bsel = (bsel + ((k & KEY_RIGHT) ? 1 : 7)) & 7;   /* badge cursor 0..7 */
      card_sel_frame(game, sel, bsel);
    } else if ((k & KEY_SELECT) && sel == CARDF_BADGES && game == PK_EMERALD) {
      /* the Frontier symbols aren't drawn on the card: keep the full list
       * reachable on Emerald behind SELECT (badges themselves = instant A) */
      if (flag_set_editor(sb1, game, "BADGES + FRONTIER",
                          badge_or_frontier_flag, BADGEFRONT_LBL, 22))
        *d1 = true;
      full = true;
    } else if (k & KEY_A) {                  /* edit in place (plain page's editors) */
      switch (sel) {
        case CARDF_NAME: { char b[8];
                          if (id_edit_ok() && osk_input("TRAINER NAME", name, b, 8)) {
                          strcpy(name, b); pk_set_trainer_name(sb2, name); *d2 = true; } } break;
        case CARDF_ID: if (!id_edit_ok()) break;
                       *tid = (uint16_t)num_entry("ID No", *tid, 65535);
                       *sid = (uint16_t)num_entry("SID", *sid, 65535);   /* SID rides the ID field */
                       pk_set_trainer_id(sb2, *tid, *sid); *d2 = true; break;
        case CARDF_MONEY: *money = num_entry("MONEY", *money, 999999);
                       pk_set_money(sb1, sb2, game, *money); *d1 = true; break;
        case CARDF_TIME: *ph = (uint16_t)num_entry("PLAY HOURS", *ph, 999);
                       *pm = (uint8_t)num_entry("PLAY MINUTES", *pm, 59);
                       pk_set_playtime(sb2, *ph, *pm, 0); *d2 = true; break;
        case CARDF_SEX:  /* instant flip — the card art + photo swap IS the
                          * feedback (and it stays RAM-only until B-save) */
                       if (!id_edit_ok()) break;
                       *gender ^= 1; pk_set_gender(sb2, *gender); *d2 = true;
                       break;
        case CARDF_BADGES: {                 /* instant toggle of the badge under
                                              * the cursor; repaint its cell only */
          int fn = pk_badge_flag(game, bsel);
          if (fn >= 0) {
            pk_flag_set(sb1, game, fn, !pk_flag_get(sb1, game, fn)); *d1 = true;
            card_restore(sel, bsel, game, tier, *gender, sb1, name, *tid, *money, *ph, *pm);
            card_sel_frame(game, sel, bsel);
          }
        } break;
        case CARDF_STARS: if (pk_star_ach_count(game)) {
                         int m2 = stars_editor(sb1, sb2, game);
                         if (m2 & 1) *d1 = true;
                         if (m2 & 2) *d2 = true;
                       } break;
      }
      if (sel != CARDF_BADGES) full = true;  /* those editors used the whole
                                              * screen (SEX: art swap redraw) */
    }
  }
}

enum { TF_NAME, TF_SEX, TF_TID, TF_SID, TF_MONEY, TF_TIME, TF_BADGES, TF_STARS, TF_NUM };

/* What is on screen. Every field this loop's rows and the DEX/HOF/GAME RECORDS block
 * below print is either the cursor position (only UP/DOWN move it, with no ui_clear()
 * in between) or a value some editor mutated -- and EVERY editor reachable from the
 * switch below (osk_input/num_entry/app_confirm/flag_set_editor/stars_editor) opens
 * with its own ui_clear() on entry, so ui_clear_gen() alone already proves whether an
 * edit ran, INCLUDING the cascades a hand-list would miss (e.g. turning a dex star OFF
 * in stars_editor un-catches species, moving the DEX line's own caught count). That
 * leaves exactly one case worth special-casing: a plain UP/DOWN cursor move, which
 * touches nothing outside the two affected rows (the DEX/HOF/RECORDS block has no
 * cursor and cannot change without an editor having run first). Stack-local, not a
 * static: a fresh call always starts invalid (first pass paints in full). */
typedef struct { uint32_t gen; int sel; bool valid; } TCardPaint;

/* One field row (TF_* order). Reads live SB1/SB2 -- like pdna_edit.c's field_value(),
 * everything here is a pure function of (idx, the live save buffers), so a fresh call
 * always shows the current value with no separate shadow to keep in sync. Panel height
 * 9 on a 9 px pitch: no neighbour bleed, same shape as flag_row_paint/star_row_paint
 * above. */
static void tcard_row_paint(uint8_t* sb1, uint8_t* sb2, PkGame game, const char* name,
                            uint8_t gender, uint16_t tid, uint16_t sid, uint32_t money,
                            uint16_t ph, uint8_t pm, int idx, int y, bool sel) {
  const char* lbl; char val[24];
  switch (idx) {
    case TF_NAME:  lbl = "NAME";  siprintf(val, "%s", name); break;
    case TF_SEX:   lbl = "SEX";   siprintf(val, "%s", gender ? "Female" : "Male"); break;
    case TF_TID:   lbl = "ID No"; siprintf(val, "%05u", (unsigned)tid); break;
    case TF_SID:   lbl = "SID";   siprintf(val, "%05u", (unsigned)sid); break;
    case TF_MONEY: lbl = "MONEY"; siprintf(val, "$%lu", (unsigned long)money); break;
    case TF_TIME:  lbl = "TIME";  siprintf(val, "%uh %02um", (unsigned)ph, (unsigned)pm); break;
    case TF_BADGES: {
      lbl = "BADGE";
      int nb = 0; for (int i = 0; i < 8; i++) if (pk_flag_get(sb1, game, pk_badge_flag(game, i))) nb++;
      siprintf(val, "%d/8%s (A)", nb, game == PK_EMERALD ? " +front" : "");   /* row budget 29 cols */
    } break;
    case TF_STARS:                     /* card tier (all three games, gen3_stars) */
      lbl = "STARS";
      if (pk_star_ach_count(game))
        siprintf(val, "%d/4 (A)", pk_star_count(sb1, sb2, game, card_hoenn_dex()));
      else siprintf(val, "n/a");
      break;
    default: lbl = ""; val[0] = 0;
  }
  if (sel) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
  else     ui_fill_rect(2, y - 1, 236, 9, UI_BG);
  char line[64]; siprintf(line, "%-6s %s", lbl, val);
  ui_text(6, y, sel ? UI_SELTEXT : (idx == TF_MONEY ? UI_OK : UI_TEXT), line);
}

static void tcard_render(uint8_t* sb1, uint8_t* sb2, PkGame game, bool edit,
                         const char* name, uint8_t gender, uint16_t tid, uint16_t sid,
                         uint32_t money, uint16_t ph, uint8_t pm, int sel, TCardPaint* pv) {
  bool full = !pv->valid || pv->gen != ui_clear_gen();

  if (full) {
    ui_clear();
    ui_text(4, 2, UI_TITLE, "TRAINER CARD");
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < TF_NUM; i++)
      tcard_row_paint(sb1, sb2, game, name, gender, tid, sid, money, ph, pm,
                      i, 14 + i * 9, edit && i == sel);

    char line[64];
    int y = 14 + TF_NUM * 9 + 1;       /* one row denser than before: STARS fits */
    int seen, caught; bool nat; pk_pokedex(sb2, &seen, &caught, &nat);
    siprintf(line, "DEX  seen %d  caught %d%s", seen, caught, nat ? " Nat" : "");
    { char lt[40]; ui_truncate(lt, line, 29); ui_text(6, y, UI_TEXT, lt); } y += 9;

    ui_hline(4, y, 232, UI_BORDER); y += 3;
    ui_text(6, y, UI_TITLE, "ELITE FOUR / HALL OF FAME"); y += 9;
    uint16_t h; uint8_t m, s2;
    if (pk_hof_time(sb1, sb2, game, &h, &m, &s2)) {
      siprintf(line, "First cleared  %uh %02um %02us", (unsigned)h, (unsigned)m, (unsigned)s2);
      ui_text(10, y, UI_OK, line);
    } else ui_text(10, y, UI_DIM, "Not cleared yet");
    y += 9;

    ui_hline(4, y, 232, UI_BORDER); y += 3;
    ui_text(6, y, UI_TITLE, "GAME RECORDS"); y += 9;
    siprintf(line, "Steps %u", (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_STEPS));
    ui_text(10, y, UI_TEXT, line); y += 9;
    siprintf(line, "Battles %u (w%u/t%u)",
             (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_TOTAL_BATTLES),
             (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_WILD_BATTLES),
             (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_TRAINER_BATTLES));
    { char lt[40]; ui_truncate(lt, line, 28); ui_text(10, y, UI_TEXT, lt); } y += 9;
    siprintf(line, "Captures %u  Eggs %u",
             (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_POKEMON_CAPTURES),
             (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_HATCHED_EGGS));
    { char lt[40]; ui_truncate(lt, line, 28); ui_text(10, y, UI_TEXT, lt); }

    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, edit ? "U/D field  A edit  B save" : "B back");
  } else if (sel != pv->sel) {
    /* the only thing a plain UP/DOWN moves: the two affected rows. Reachable only in
     * edit mode -- the read-only page exits after this function's one full paint,
     * before ever reading a key (pdna_trainer()'s own !edit early return below), so
     * `edit` is always true on any call that reaches this branch. */
    tcard_row_paint(sb1, sb2, game, name, gender, tid, sid, money, ph, pm,
                    pv->sel, 14 + pv->sel * 9, false);
    tcard_row_paint(sb1, sb2, game, name, gender, tid, sid, money, ph, pm,
                    sel,     14 + sel     * 9, true);
  }

  pv->sel = sel; pv->gen = ui_clear_gen(); pv->valid = true;
}

void pdna_trainer(uint8_t* sb1, uint8_t* sb2, const Gen3SaveInfo* info, PkGame game) {
  const bool edit = app_can_edit();
  s_id_warned = false;                 /* the mixing-identity warning is once per VISIT */
  /* Seed EVERY identity field from the LIVE SaveBlock2 buffer, never from the
   * `info` snapshot: Gen3SaveInfo is parsed ONCE when the save is opened and
   * never re-parsed after a commit, while sb2 is re-read from the committed
   * image each nav-loop pass. Seeding from the stale snapshot was the "SEX
   * won't save" bug — the flip DID commit (d2 -> app_commit_sb2), but every
   * re-entry re-showed the at-load sex, and A then wrote stale^1 (usually the
   * value already on disk = a no-op), so the card could never visibly change.
   * TID/SID and PLAYTIME were worse: pk_set_trainer_id / pk_set_playtime
   * write BOTH halves, so editing one after a saved edit of the other
   * silently reverted it to the stale co-seed. Offsets: gen3_save.h SB2_OFF_*
   * (same decode as gen3_parse). */
  char name[8]; int ni;
  for (ni = 0; ni < G3_PLAYER_NAME_LEN; ni++) {
    char ch = gen3_decode_char(sb2[SB2_OFF_PLAYER_NAME + ni]);
    if (ch == 0) break;
    name[ni] = ch;
  }
  name[ni] = 0;
  uint8_t  gender = sb2[SB2_OFF_GENDER] ? 1 : 0;
  uint16_t tid = (uint16_t)(sb2[SB2_OFF_TRAINER_ID]     | (sb2[SB2_OFF_TRAINER_ID + 1] << 8));
  uint16_t sid = (uint16_t)(sb2[SB2_OFF_TRAINER_ID + 2] | (sb2[SB2_OFF_TRAINER_ID + 3] << 8));
  uint32_t money = pk_money(sb1, sb2, game);
  uint16_t ph = (uint16_t)(sb2[SB2_OFF_PLAYTIME_H] | (sb2[SB2_OFF_PLAYTIME_H + 1] << 8));
  uint8_t  pm = sb2[SB2_OFF_PLAYTIME_M];
  (void)info;                        /* kept for the call signature only */
  int  sel = 0;
  bool d1 = false, d2 = false;       /* dirty: sb1 (money) / sb2 (identity) */

  /* Any game with vendored art: the real card front IS the whole screen — the
   * cursor edits on the card itself (no plain-page fallback). Art-free builds
   * keep the plain details/edit page below, as before. */
  if (card_bg(game, 0, gender).blob != 0) {
    card_editor(sb1, sb2, game, edit, name, &gender, &tid, &sid,
                &money, &ph, &pm, &d1, &d2);
    trainer_commit(d1, d2);
    return;
  }

  TCardPaint pv;
  memset(&pv, 0, sizeof pv);           /* .valid = false: the first pass paints in full */
  for (;;) {
    tcard_render(sb1, sb2, game, edit, name, gender, tid, sid, money, ph, pm, sel, &pv);

    if (!edit) { do { s_vsync(); } while (!key_hit(KEY_B)); snd_back(); return; }

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) {
      trainer_commit(d1, d2);
      return;
    } else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : TF_NUM - 1;
    else if (k & KEY_DOWN)   sel = (sel + 1) % TF_NUM;
    else if (k & KEY_A) {
      switch (sel) {
        case TF_NAME: { char b[8];
                        if (id_edit_ok() && osk_input("TRAINER NAME", name, b, 8)) {
                          strcpy(name, b); pk_set_trainer_name(sb2, name); d2 = true; } } break;
        case TF_SEX:   /* confirmed toggle: a single stray A here used to flip the
                        * live save's gender silently (male bag/card ever after). */
                       if (id_edit_ok() &&
                           app_confirm("Change trainer SEX?",
                                       gender ? "Female -> Male" : "Male -> Female")) {
                         gender ^= 1; pk_set_gender(sb2, gender); d2 = true;
                       } break;
        case TF_TID:   if (!id_edit_ok()) break;
                       tid = (uint16_t)num_entry("ID No", tid, 65535);
                       pk_set_trainer_id(sb2, tid, sid); d2 = true; break;
        case TF_SID:   if (!id_edit_ok()) break;
                       sid = (uint16_t)num_entry("SID", sid, 65535);
                       pk_set_trainer_id(sb2, tid, sid); d2 = true; break;
        case TF_MONEY: money = num_entry("MONEY", money, 999999);
                       pk_set_money(sb1, sb2, game, money); d1 = true; break;
        case TF_TIME:  ph = (uint16_t)num_entry("PLAY HOURS", ph, 999);
                       pm = (uint8_t)num_entry("PLAY MINUTES", pm, 59);
                       pk_set_playtime(sb2, ph, pm, 0); d2 = true; break;
        case TF_BADGES: if (flag_set_editor(sb1, game,
                              game == PK_EMERALD ? "BADGES + FRONTIER" : "BADGES",
                              badge_or_frontier_flag, BADGEFRONT_LBL,
                              game == PK_EMERALD ? 22 : 8)) d1 = true; break;   /* SB1 flags */
        case TF_STARS: if (pk_star_ach_count(game)) {
                         int m2 = stars_editor(sb1, sb2, game);
                         if (m2 & 1) d1 = true;
                         if (m2 & 2) d2 = true;
                       } break;
      }
    }
  }
}
