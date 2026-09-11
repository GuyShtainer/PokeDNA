/* g2card_cells.c -- see g2card_cells.h. Pure C (stdint/stdbool + pdna_gbscreen.h's
 * GbScrSrc enum only), host-compilable, no tonc/FatFs -- BACKLOG #96 D11. */
#include "g2card_cells.h"

const uint8_t kG2BadgeBit[8] = { 0, 1, 2, 3, 5, 4, 6, 7 };

int g2card_build_upper_cells(bool female, bool has_corner, G2CardCell out[G2CARD_UPPER_CELLS]) {
  int n = 0;
  GbScrSrc pic_src = female ? GBSCR_SRC_CARDPIC_F : GBSCR_SRC_CARDPIC_M;

  out[n].x = 2; out[n].y = 4; out[n].src = GBSCR_SRC_CARDGFX; out[n].index = G2G_ID; n++;
  out[n].x = 3; out[n].y = 4; out[n].src = GBSCR_SRC_CARDGFX; out[n].index = G2G_NO; n++;

  /* (18,9) and (18,1) are the card's RIGHT-CORNER chamfer, NOT a repeat of the
   * photo: TrainerCard_InitBorder writes it one row under each box's top row
   * (Gold $04, Crystal $1c). On GOLD (row-major) display index 4 IS that tile
   * and this is pixel-exact -- `has_corner` is false there (RomGbUi.cardcorner
   * == 0 by construction), so both cells keep the plain pic_src/index-4 form.
   * On CRYSTAL the game copies CardRightCornerGFX (pokecrystal.sym 09:65c3 =
   * badges + 88 tiles, gu->cardcorner) over vTiles2 tile $1c after GetCardPic,
   * so `has_corner` is true and BOTH cells resolve through GBSCR_SRC_CARDCORNER
   * (a single 16-B block, index always 0) instead -- BACKLOG #125, fixed. The
   * (18,1) override happens IN the grid loop below (tx==4, ty==0 is exactly
   * that cell): overwriting that one iteration's src/index is what "override,
   * don't add" means here -- the emitted cell count stays G2CARD_UPPER_CELLS
   * either way, no append+dedupe needed. */
  for (int ty = 0; ty < 7; ty++)
    for (int tx = 0; tx < 5; tx++) {
      out[n].x = (uint8_t)(14 + tx); out[n].y = (uint8_t)(1 + ty);
      if (has_corner && tx == 4 && ty == 0) {
        out[n].src = GBSCR_SRC_CARDCORNER; out[n].index = 0;
      } else {
        out[n].src = pic_src; out[n].index = (uint8_t)(ty * 5 + tx);
      }
      n++;
    }
  if (has_corner) {
    out[n].x = 18; out[n].y = 9; out[n].src = GBSCR_SRC_CARDCORNER; out[n].index = 0; n++;
  } else {
    out[n].x = 18; out[n].y = 9; out[n].src = pic_src; out[n].index = 4; n++;
  }

  for (int x = 1; x <= 12; x++) {
    out[n].x = (uint8_t)x; out[n].y = 3; out[n].src = GBSCR_SRC_CARDGFX; out[n].index = G2G_DIVFILL; n++;
  }
  out[n].x = 13; out[n].y = 3; out[n].src = GBSCR_SRC_CARDGFX; out[n].index = G2G_DIVCAP; n++;

  return n;   /* == G2CARD_UPPER_CELLS */
}

int g2card_build_page1_cells(G2CardCell out[G2CARD_PAGE1_CELLS]) {
  int n = 0;
  for (int i = 0; i < 5; i++) {
    out[n].x = (uint8_t)(2 + i); out[n].y = 8;
    out[n].src = GBSCR_SRC_STATUSWORD; out[n].index = (uint8_t)(G2X_STATUS0 + i); n++;
  }
  out[n].x = 18; out[n].y = 15; out[n].src = GBSCR_SRC_FONT; out[n].index = 0xED; n++;
  return n;   /* == G2CARD_PAGE1_CELLS */
}

int g2card_build_page2_cells(const bool badge_owned[8], G2CardCell out[G2CARD_PAGE2_CELLS_MAX]) {
  int n = 0;
  for (int i = 0; i < 5; i++) {
    out[n].x = (uint8_t)(2 + i); out[n].y = 8;
    out[n].src = GBSCR_SRC_LEADERS; out[n].index = (uint8_t)(G2L_BADGES_WORD + i); n++;
  }

  for (int k = 0; k < 8; k++) {
    int row = k / 4, col = k % 4;
    int c0 = 2 + col * 4, y0 = 10 + row * 3;
    int base = 10 * k;
    for (int i = 0; i < 4; i++) {
      out[n].x = (uint8_t)(c0 + i); out[n].y = (uint8_t)y0;
      out[n].src = GBSCR_SRC_LEADERS; out[n].index = (uint8_t)(base + i); n++;
    }
    for (int i = 0; i < 3; i++) {
      out[n].x = (uint8_t)(c0 + 1 + i); out[n].y = (uint8_t)(y0 + 1);
      out[n].src = GBSCR_SRC_LEADERS; out[n].index = (uint8_t)(base + 4 + i); n++;
    }
    for (int i = 0; i < 3; i++) {
      out[n].x = (uint8_t)(c0 + 1 + i); out[n].y = (uint8_t)(y0 + 2);
      out[n].src = GBSCR_SRC_LEADERS; out[n].index = (uint8_t)(base + 7 + i); n++;
    }

    if (badge_owned && badge_owned[k]) {
      int base2 = 4 * kG2BadgeBit[k];
      for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) {
          out[n].x = (uint8_t)(c0 + dx); out[n].y = (uint8_t)(y0 + 1 + dy);
          out[n].src = GBSCR_SRC_BADGES; out[n].index = (uint8_t)(base2 + dy * 2 + dx); n++;
        }
    }
  }

  return n;   /* 85 + 4*owned_count, so 85..117 */
}
