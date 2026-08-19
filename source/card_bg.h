#ifndef CARD_BG_INCLUDED
#define CARD_BG_INCLUDED
#include <stdint.h>
#include "lzblob.h"   /* the art ships LZ77-paged; accessors hand out a BgFrame */

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
 * window (19,5) + Kanto offset {13,4} -> (165,44); flag SYS_FLAGS(0x800)+0x29.
 *
 * Selection rects derive from the drawn ink with one rule so the 2 px cursor
 * frame HUGS every field the same way: rect = ink box + 3 px on each side
 * (border pixels at -3/-2, a 1 px gap at -1, then the ink; text rows h = 14,
 * photo h/w = 64+6, stars 32x8 -> 38x14, one 16x16 badge cell -> 22x22 via
 * CARD_BADGE_RECT). Ink boxes: ID = the fixed-width digits ("IDNo."+5 or 5
 * digits; Emerald centered "%05u" in [128..224] is always 80 px at x 136),
 * NAME = label + 7-char name from name_x, MONEY/TIME = label column..val_xr,
 * SEX = the 64x64 photo, STARS = 4 stars from star_xy. */
static const CardLayout CARD_LAYOUTS[3] = {
  /* PK_RS */ { 0,  56, 46,  160, 22, 0,   0, 128,  70,  86, 102,  32, 124, 0x801,
    { [CARDF_ID]     = { 157,  19,  46, 14 },   /* "NNNNN" at 160,22          */
      [CARDF_NAME]   = {  53,  43,  62, 14 },   /* 7 chars at 56,46           */
      [CARDF_STARS]  = { 117,  49,  38, 14 },   /* 4 stars from (120,52)      */
      [CARDF_MONEY]  = {  13,  67, 121, 17 },   /* baked label x17..y80 (11 px
                                                 * game font) .. value edge 128 */
      [CARDF_TIME]   = {  13,  99, 121, 17 },
      [CARDF_SEX]    = { 149,  41,  70, 70 },   /* photo (152,44) 64x64       */
      [CARDF_BADGES] = {  29, 121, 190, 22 } } },
  /* PK_EMERALD */ { 1,  24, 45,  128, 21, 96,  24, 136,  69,  85, 101,  32, 120, 0x861,
    { [CARDF_ID]     = { 133,  18,  86, 14 },   /* "IDNo.NNNNN" centered: 136 */
      [CARDF_NAME]   = {  21,  42, 110, 14 },   /* "NAME: "+7 at 24,45        */
      [CARDF_STARS]  = { 117,  53,  38, 14 },   /* 4 stars from (120,56)      */
      [CARDF_MONEY]  = {  21,  66, 118, 14 },   /* label x24 .. edge 136      */
      [CARDF_TIME]   = {  21,  98, 118, 14 },
      [CARDF_SEX]    = { 150,  37,  70, 70 },   /* photo (153,40) 64x64       */
      [CARDF_BADGES] = {  29, 117, 190, 22 } } },
  /* PK_FRLG */ { 1,  28, 41,  150, 22, 0,   28, 144,  68,  84, 100,  32, 128, 0x829,
    { [CARDF_ID]     = { 147,  19,  86, 14 },   /* "IDNo.NNNNN" at 150,22     */
      [CARDF_NAME]   = {  25,  38, 110, 14 },   /* "NAME: "+7 at 28,41        */
      [CARDF_STARS]  = { 117,  53,  38, 14 },   /* 4 stars from (120,56)      */
      [CARDF_MONEY]  = {  25,  65, 122, 14 },   /* label x28 .. edge 144      */
      [CARDF_TIME]   = {  25,  97, 122, 14 },
      [CARDF_SEX]    = { 162,  41,  70, 70 },   /* photo (165,44) 64x64       */
      [CARDF_BADGES] = {  29, 125, 190, 22 } } },
};

/* One badge cell's cursor rect on the badge row (A toggles that badge). */
#define CARD_BADGE_RECT(L, i, X, Y, W_, H_) \
  do { (X) = (L)->badge_x + 24 * (i) - 3; (Y) = (L)->badge_y - 3; \
       (W_) = 22; (H_) = 22; } while (0)

/* ---- card BACK (L/R flips): per-game stat rows over the back frame ----
 * Rows + data sources, each verified in that game's own decomp:
 *  RS    (pokeruby src/trainer_card.c:1233-1341 TrainerCard_Back_Print*):
 *        rows at menu tile rows 5/7/9/11/13/15, labels col 3, values
 *        right-aligned col 28 = x224, text BG VOFS -4 -> +4 px (+2 ink);
 *        HoF time (gameStats[1] gated on [10]), link W/L (stats 23/24),
 *        trades (21), pokeblocks-mixing (34), contests (35), Battle Tower
 *        totalWins/bestStreak = SB2 u16 0x0570/0x0572 (include/global.h:834-835,
 *        trainer_card.c:408-409).
 *  E     (pokeemerald src/trainer_card.c:951-989 PrintAllOnCardBack +
 *        :1196-1203 PrintStatOnBackOfCard: label x16, y = top*16+33, value
 *        right edge 216, window (1,1) -> +8 px (+4 ink); name right edge 216
 *        y9; HoF (stats 1/10), link W/L (23/24), trades (21), pokeblocks
 *        w/friends (34), contests w/friends (35), BATTLE POINTS WON =
 *        frontier.cardBattlePoints SB2 u16 0xEBA (include/global.h:450,541).
 *  FRLG  (pokefirered src/trainer_card.c:1296-1390: label x10, ys
 *        35/51/67/83/99, window (1,1) -> +8 px (+4 ink); name printed after
 *        the baked TRAINER: at x138 y11; HoF (stats 1/10), link W/L (23/24),
 *        trades (21), union trades&battles (stat 50), berry crush points
 *        (stat 51) — include/constants/game_stat.h:25-28,54-55.
 * Stat-id constants are identical across all three games (each game's
 * include/constants/game_stat.h). Caps = the games' GetCappedGameStat args. */
enum { CBK_HOF,       /* first HoF time: stats 1 (packed h:m:s) + 10 (gate) */
       CBK_WL,        /* two stats: wins = .stat, losses = .stat + 1        */
       CBK_STAT,      /* one game-stat counter (.stat), capped at .cap      */
       CBK_TOWER_RS,  /* RS Battle Tower: SB2 0x0570 wins / 0x0572 streak   */
       CBK_BP_E };    /* Emerald card Battle Points: SB2 0xEBA              */

typedef struct {
  const char* label;                /* shortened to fit the 8-px font       */
  uint8_t kind, stat, y;            /* y = ink top (screen px)              */
  uint16_t cap;
} CardBackRow;

typedef struct {
  uint8_t name_x, name_y, name_right;   /* name line; right-align at name_x */
  uint8_t lbl_x, val_xr;                /* label column / value right edge  */
  uint8_t nrows;
  CardBackRow rows[6];
} CardBackLayout;

static const CardBackLayout CARD_BACK_LAYOUTS[3] = {
  /* PK_RS */ { 224, 28, 1,  28, 224, 6, {
      { "HALL OF FAME",   CBK_HOF,       0,  46, 0     },
      { "LINK BATTLES",   CBK_WL,       23,  62, 9999  },
      { "TRADE RECORD",   CBK_STAT,     21,  78, 65535 },
      { "MIXING RECORD",  CBK_STAT,     34,  94, 65535 },
      { "CONTEST RECORD", CBK_STAT,     35, 110, 999   },
      { "BATTLE TOWER",   CBK_TOWER_RS,  0, 126, 9999  } } },
  /* PK_EMERALD */ { 224, 22, 1,  24, 224, 6, {
      { "HALL OF FAME",    CBK_HOF,    0,  45, 0     },
      { "LINK BATTLES",    CBK_WL,    23,  61, 9999  },
      { "POKeMON TRADES",  CBK_STAT,  21,  77, 65535 },
      { "POKeBLOCKS W/FR.",CBK_STAT,  34,  93, 65535 },
      { "CONTESTS W/FR.",  CBK_STAT,  35, 109, 999   },
      { "BATTLE POINTS",   CBK_BP_E,   0, 125, 65535 } } },
  /* PK_FRLG */ { 146, 24, 0,  18, 224, 5, {   /* row ink 46..53 = the real
      * card's 45..53 (measured off an authentic back screenshot; separators
      * at 55/71/87/103 stay clear) */
      { "HALL OF FAME",     CBK_HOF,   0,  46, 0     },
      { "LINK BATTLES",     CBK_WL,   23,  62, 9999  },
      { "POKeMON TRADES",   CBK_STAT, 21,  78, 65535 },
      { "UNION TRADES&BTL", CBK_STAT, 50,  94, 65535 },
      { "BERRY CRUSH",      CBK_STAT, 51, 110, 65535 } } },
};

/* 240x160 RGB15 frame for (game, star tier 0..4, gender), or NULL in an
 * art-free build. Lives in ROM — blit ROM->VRAM, never buffer it in EWRAM. */
BgFrame card_bg(int game, int tier, int female);

/* The card BACK frame of (game, tier, gender) — same surround, the game's
 * back.bin card face, no photo/stars/badges — or NULL in an art-free build. */
BgFrame card_bg_back(int game, int tier, int female);

/* 16x16 gym badge i (0..7) of `game` in ui_sprite format (0 = transparent),
 * or NULL. RS badges come through badges_map.bin, Emerald/FRLG are plain. */
const uint16_t* card_badge16(int game, int i);

/* Hoenn dex 1..200 -> National dex number (Jirachi/Deoxys excluded, exactly
 * the HasAllHoennMons set), or NULL in an art-free build (the RS/Emerald
 * "complete dex" star achievement is then read-only n/a; FRLG's dex stars
 * need no table). Feeds gen3_stars' hoenn200 arg. */
const uint16_t* card_hoenn_dex(void);

#endif /* CARD_BG_INCLUDED */
