#ifndef CARD_BG_INCLUDED
#define CARD_BG_INCLUDED
#include <stdint.h>

/* Real Gen-3 TRAINER CARD fronts: one full 240x160 RGB15 frame per game
 * (PkGame-indexed: RS / Emerald / FRLG), star tier 0..4 and player gender —
 * the tier is the card's palette (RS 0..4star, Emerald green..gold, FRLG
 * blue..gold), the gender swaps BG palette bank 1 (the surround) plus the
 * photo (Brendan/May, Red/Leaf), and the tier's star row + the trainer pic
 * are pre-composited at build time. The pixels come from the GENERATED
 * source/card_bg.c + card_bg_data.s (git-ignored ripped art — regenerate with
 * python3 tools/gen_card_bg.py); an art-free clone links the weak NULL
 * fallbacks in pdna_trainer.c instead and keeps the plain trainer screen. */

#define CARD_BG_W 240
#define CARD_BG_H 160

/* The cursor-editable fields ON the card, in U/D order (top-to-bottom). */
enum { CARDF_ID, CARDF_NAME, CARDF_STARS, CARDF_MONEY, CARDF_TIME,
       CARDF_SEX, CARDF_BADGES, CARDF_NUM };

/* Per-game overlay geometry in screen px, from each game's src/trainer_card.c
 * print/draw functions — pdna_trainer.c has zero magic pixels. Text Y is the
 * game's print top adjusted so the 8-px sys8 glyphs sit centered in the game
 * font's ink band (the +4 rule measured off real Emerald screenshots; RS adds
 * its BG VOFS=-4 shift first, FRLG prints at window (1,1) = +8 px).
 * labels==0 (RS) means NAME/IDNo./MONEY/POKeDEX/PLAY TIME labels are BAKED
 * into the card art and the runtime draws bare values only. */
typedef struct {
  uint8_t labels;                   /* 1 = draw NAME:/IDNo./MONEY/... text   */
  uint8_t name_x, name_y;           /* trainer-name row                      */
  uint8_t id_x, id_y, id_w;         /* IDNo: centered in [id_x..id_x+id_w],  */
                                    /* or left-aligned at id_x when id_w==0  */
  uint8_t lbl_x, val_xr;            /* label column / value right edge       */
  uint8_t money_y, dex_y, time_y;
  uint8_t badge_x, badge_y;         /* badge i 16x16 at (badge_x + 24*i)     */
  uint16_t dex_flag;                /* FLAG_SYS_POKEDEX_GET (this game)      */
  struct { uint8_t x, y, w, h; } rect[CARDF_NUM];   /* selection-frame rects */
} CardLayout;

/* RS (pokeruby): values only (labels baked); name (7,5)*8, id digits (20,2)*8,
 * money/dex right edge tile 16 = 128 rows 8/10, time (10,12) in a 48-px field
 * -> right edge 128 — all on BGs shifted down 4 px (VOFS -4), ink +2. Stars
 * map row 6 -> y 52, badges rows 15..16 -> y 124, photo (152,44), dex flag
 * SYSTEM_FLAGS(0x800)+1.
 * Emerald (pokeemerald): unchanged from the first version (window (1,1) = +8,
 * +4 ink; IDNo centered in [128..224]; stars y 56; badges y 120; flag 0x861).
 * FRLG (pokefirered): window (1,1) = +8: name "NAME: x" (20,29), id
 * "IDNo.x" (142,10), labels x 20, money right edge 134/dex 136 -> 144, rows
 * 56/72/88, +4 ink; stars (15,7) -> y 56; badges row 16..17 -> y 128; photo
 * window (19,5) + Kanto offset {13,4} -> (165,44); flag SYS_FLAGS(0x800)+0x29. */
static const CardLayout CARD_LAYOUTS[3] = {
  /* PK_RS */ { 0,  56, 46,  160, 22, 0,   0, 128,  70,  86, 102,  32, 124, 0x801,
    { [CARDF_ID]     = { 156,  16,  48, 14 },
      [CARDF_NAME]   = {  54,  40,  64, 14 },
      [CARDF_STARS]  = { 118,  50,  36, 12 },
      [CARDF_MONEY]  = {  14,  64, 118, 14 },
      [CARDF_TIME]   = {  14,  96, 118, 14 },
      [CARDF_SEX]    = { 150,  42,  68, 68 },
      [CARDF_BADGES] = {  30, 122, 188, 20 } } },
  /* PK_EMERALD */ { 1,  24, 45,  128, 21, 96,  24, 136,  69,  85, 101,  32, 120, 0x861,
    { [CARDF_ID]     = { 126,  17, 100, 14 },
      [CARDF_NAME]   = {  22,  41, 110, 14 },
      [CARDF_STARS]  = { 118,  54,  36, 12 },
      [CARDF_MONEY]  = {  22,  65, 118, 14 },
      [CARDF_TIME]   = {  22,  97, 118, 14 },
      [CARDF_SEX]    = { 151,  38,  68, 68 },
      [CARDF_BADGES] = {  30, 118, 188, 20 } } },
  /* PK_FRLG */ { 1,  28, 41,  150, 22, 0,   28, 144,  68,  84, 100,  32, 128, 0x829,
    { [CARDF_ID]     = { 148,  18,  84, 14 },
      [CARDF_NAME]   = {  26,  37, 110, 14 },
      [CARDF_STARS]  = { 118,  54,  36, 12 },
      [CARDF_MONEY]  = {  26,  64, 122, 14 },
      [CARDF_TIME]   = {  26,  96, 122, 14 },
      [CARDF_SEX]    = { 163,  42,  68, 68 },
      [CARDF_BADGES] = {  30, 126, 188, 20 } } },
};

/* 240x160 RGB15 frame for (game, star tier 0..4, gender), or NULL in an
 * art-free build. Lives in ROM — blit ROM->VRAM, never buffer it in EWRAM. */
const uint16_t* card_bg(int game, int tier, int female);

/* 16x16 gym badge i (0..7) of `game` in ui_sprite format (0 = transparent),
 * or NULL. RS badges come through badges_map.bin, Emerald/FRLG are plain. */
const uint16_t* card_badge16(int game, int i);

/* Hoenn dex 1..200 -> National dex number (Jirachi/Deoxys excluded, exactly
 * the HasAllHoennMons set), or NULL in an art-free build (the RS/Emerald
 * "complete dex" star achievement is then read-only n/a; FRLG's dex stars
 * need no table). Feeds gen3_stars' hoenn200 arg. */
const uint16_t* card_hoenn_dex(void);

#endif /* CARD_BG_INCLUDED */
