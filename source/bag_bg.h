#ifndef BAG_BG_INCLUDED
#define BAG_BG_INCLUDED
#include <stdint.h>

/* Real Gen-3 bag-screen backgrounds: one full 240x160 RGB15 frame per game
 * (PkGame-indexed: RS / Emerald / FRLG) and player gender, pre-composited at
 * build time from each game's own decomp art (palette swap + gendered bag
 * sprite). The pixels come from the GENERATED source/bag_bg.c + bag_bg_data.s
 * (git-ignored ripped art — regenerate with python3 tools/gen_bag_bg.py); an
 * art-free clone links the weak NULL fallbacks in pdna_bag.c instead, and
 * callers keep the plain data-editor bag tab for that game. */

#define BAG_BG_W   240
#define BAG_BG_H   160
#define BAG_ANIM_W  64         /* anim rect width = the 64x64 bag sprite */

/* Per-game screen layout in px, from each game's src/item_menu.c windows —
 * pdna_bag.c has zero magic pixels. (0,0) coordinates mean "this game has no
 * such element". anim geometry MUST match tools/gen_bag_bg.py GAMES. */
typedef struct {
  uint8_t list_x0, list_y0, list_x1, list_y1;   /* item-list pane            */
  uint8_t desc_x0, desc_y0, desc_x1, desc_y1;   /* description text region   */
  uint8_t icon_x, icon_y;                       /* the game's 24x24 item-icon
                                                 * slot, or (0,0) = no icon  */
  uint8_t pkt_rx, pkt_ry, pkt_rw, pkt_rh;       /* pocket banner restore rect*/
  uint8_t pkt_tx, pkt_ty;                       /* pocket-name text corner   */
  uint8_t dot_x, dot_y;                         /* 5 switch dots, or (0,0)   */
  uint8_t foot_x, foot_y;                       /* footer hints, or (0,0)    */
  uint8_t anim_x, anim_y;                       /* anim rect = bag TL - rise */
  uint8_t rise;                                 /* pop-up px (rect h = 64+rise) */
  uint8_t fall_wait;                            /* vsyncs per 1-px fall step */
  uint16_t desc_ink;                            /* RGB15 text ink in the desc pane */
} BagLayout;

/* RS: list panel + desc box measured off the rendered bag_screen.bin (list
 * inner matches Emerald exactly); pocket pill baked at (8,80)-(108,96), no
 * dots (the game uses a spinner sprite instead — not reproduced), no room for
 * a footer line; fall = -4 px at 1 px per 2 frames (pokeruby sub_80A79EC).
 * Emerald: sDefaultBagWindows values (unchanged from the phase-1 rects).
 * FRLG: list.bin panel at tile (11,1) 18x12; description = white text on the
 * blue bottom bar (window (5,14)); pocket name in the tan header (window
 * (1,1)); no dots/footer.
 *
 * icon_xy = each game's OWN 24x24 item-icon slot, always OUTSIDE the desc
 * text region (the desc pane is pure text; the wrap never flows around it):
 *   RS      (0,0) = NO icon — pokeruby's bag draws no item icon at all
 *           (src/item_menu.c has no icon path; item icons are FRLG+ art).
 *   Emerald (8,72) = the baked white square left of the bag: sprite center
 *           (24,88) [pokeemerald src/item_menu_icons.c AddBagItemIconSprite
 *           x2=24/y2=88] - centerToCornerVec(16,16) = canvas TL (8,72); the
 *           24x24 pic sits at the 32x32 canvas's TL (item_icon.c
 *           CopyItemIconPicTo4x4Buffer). Square inner = (5,71)-(34,96).
 *   FRLG    (8,124) = the baked white square bottom-left: center (24,140)
 *           [pokefirered src/item_menu_icons.c CreateItemMenuIcon
 *           x2=24/y2=140] - (16,16) = (8,124). Square inner = (7,123)-(33,149). */
static const BagLayout BAG_LAYOUTS[3] = {
  /* PK_RS      */ { 112, 16, 232, 144,   4, 102, 106, 152,   0,   0,
                       8, 80, 104, 16,   26, 84,   0, 0,   0, 0,
                      26,  4, 4, 2, 0x1CA5 },
  /* PK_EMERALD */ { 112, 16, 232, 144,   0, 104, 112, 152,   8,  72,
                      32,  8, 120, 24,   36, 12,  43, 28,   4, 152,
                      36, 29, 5, 1, 0x1CA5 },
  /* PK_FRLG    */ {  88,  8, 232, 104,  40, 115, 236, 157,   8, 124,
                       4,  4,  80, 24,   10, 11,   0, 0,   0, 0,
                       8, 31, 5, 1, 0x7FFF },
};

/* 240x160 RGB15 frame for (game, gender 0=male/1=female), or NULL when that
 * game's art is absent (art-free build). Lives in ROM — blit ROM->VRAM,
 * never buffer it in EWRAM. */
const uint16_t* bag_bg(int game, int female);

/* 64x(64+rise) pocket-switch anim rect at (anim_x, anim_y), or NULL when
 * absent (art-free build / bad step): step 0..rise-1 = closed bag falling
 * (y2 = step-rise), rise+p = pocket p (PkPocket order) OPEN at rest — the
 * resting look while browsing. FRLG's TMs&HMs / Berries pockets reuse the
 * Items open frame (the real game keeps those in the TM Case / Berry Pouch,
 * which have no bag frame). */
const uint16_t* bag_anim(int game, int female, int step);

#endif /* BAG_BG_INCLUDED */
