#include <tonc.h>
#include <string.h>

#include "pdna_yard.h"
#include "mon_icons.h"     /* MON_ICON_W/H */
#include "data_tables.h"   /* pk_species_type1/2 */
#include "gen3_mon.h"      /* PkMon, pk_decode_mon */
#include "gen3_box.h"      /* pk_resolve */
#include "gen3_trainer.h"  /* PkGame, PK_EMERALD, PK_RS */
#include "gba_rtc.h"       /* GbaRtcTime, gba_rtc_get */
#include "rumble.h"        /* rumble_io_suspend/resume */
#include "ui_layout.h"     /* UI_SCR_W */
#include "ui.h"            /* ui_fill_rect, ui_text (needed for #ifndef HAVE_DAYCARE_BG fallback) */
#include "pdna_app.h"      /* app_session_seed decl, app_tid_public */

/* See pdna_yard.h for the shape this file mirrors, the extraction's provenance
 * (pdna_main.c's old static ~5711-6089 region), and why g_sb1/g_game/g_vinfo are
 * NOT reached directly from here. */

#ifdef HAVE_DAYCARE_BG
#include "daycare_bg_data.h"   /* daycare_bg[], DAYCARE_BG_W/H — only when compiled art exists */
#endif

#ifndef HAVE_DAYCARE_BG    /* procedural-scene helpers — only built without a yard bg */
static void dc_tri_roof(int cx, int top, int halfBase, int h, u16 fill, u16 edge) {
  for (int j = 0; j <= h; j++) { int w = halfBase * j / h; m3_line(cx - w, top + j, cx + w, top + j, fill); }
  m3_line(cx, top, cx - halfBase, top + h, edge);
  m3_line(cx, top, cx + halfBase, top + h, edge);
}

static void dc_house(void) {
  const int hx = 166, ww = 60, wallTop = 50, wallBot = 90, cx = hx + ww / 2;
  dc_tri_roof(cx, 34, ww / 2 + 4, 16, RGB15(26, 7, 5), RGB15(16, 3, 2));        /* red roof */
  ui_fill_rect(hx, wallTop, ww, wallBot - wallTop, RGB15(27, 22, 15));           /* tan walls */
  m3_frame(hx, wallTop, hx + ww - 1, wallBot - 1, RGB15(14, 10, 4));
  ui_fill_rect(cx - 8, wallBot - 20, 16, 20, RGB15(15, 9, 3));                   /* door */
  m3_frame(cx - 8, wallBot - 20, cx + 7, wallBot - 1, RGB15(9, 5, 1));
  m3_plot(cx + 4, wallBot - 10, RGB15(31, 28, 8));                               /* knob */
  ui_fill_rect(hx + 6, wallTop + 6, 13, 13, RGB15(16, 26, 31));                  /* window */
  m3_frame(hx + 6, wallTop + 6, hx + 18, wallTop + 18, RGB15(9, 5, 1));
  m3_line(hx + 12, wallTop + 6, hx + 12, wallTop + 18, RGB15(9, 5, 1));
  m3_line(hx + 6, wallTop + 12, hx + 18, wallTop + 12, RGB15(9, 5, 1));
  ui_fill_rect(hx + 6, wallTop + 23, 48, 11, RGB15(30, 27, 14));                 /* sign */
  m3_frame(hx + 6, wallTop + 23, hx + 53, wallTop + 33, RGB15(14, 9, 0));
  ui_text(hx + 9, wallTop + 25, RGB15(8, 5, 0), "DAYCARE");
}

static void dc_fence(int x0, int x1, int y) {
  const u16 w = RGB15(30, 30, 28), d = RGB15(18, 18, 16);
  m3_line(x0, y + 3, x1, y + 3, w);
  m3_line(x0, y + 6, x1, y + 6, d);
  for (int x = x0; x < x1; x += 10) { ui_fill_rect(x, y, 3, 9, w); m3_plot(x, y, d); m3_plot(x + 2, y, d); }
}

/* The TRUE background colour dc_scene()'s procedural branch (above) paints at scene
 * pixel (x,y), computed on demand instead of stored -- dc_icon_over_bg's compositor
 * needs a source that is never itself the result of a previous icon composite (see
 * that function's own comment for why sampling vid_mem broke it), and the artless
 * build has no daycare_bg[] array to look up. This mirrors, in closed form, every
 * shape dc_scene()'s #else branch and dc_fence() draw: sky/grass bands, the sun disc,
 * the three cloud rects, and the fence's posts + two rails (pixel for pixel, incl.
 * which rail survives under a post vs. in a gap). Costs zero EWRAM -- arithmetic
 * only, no buffer.
 * dc_house() is deliberately NOT modelled: DC_SPOT's artless anchors top out at
 * x=126 (SKY slot 2, cx=110 + the 16px icon half-width), well clear of the house's
 * x>=162 (roof apex at cx=196, half-base 34), so no icon footprint ever samples a
 * house pixel; those coordinates just fall through to the plain sky/grass base. */
static u16 dc_bg_px(int x, int y) {
  const u16 SKY = RGB15(16, 25, 31), GRASS = RGB15(13, 22, 9);
  const u16 SUN = RGB15(31, 30, 14), CLOUD = RGB15(30, 31, 31);
  const u16 FW = RGB15(30, 30, 28), FD = RGB15(18, 18, 16);   /* dc_fence's w/d */
  u16 base = (y < 90) ? SKY : GRASS;
  if (y < 90) {
    int dx = x - 22, dy = y - 26;                              /* sun: r*r <= 25 */
    if (dx * dx + dy * dy <= 25) return SUN;
    if (x >= 150 && x < 178 && y >= 20 && y < 26) return CLOUD;  /* cloud 1 */
    if (x >= 158 && x < 172 && y >= 16 && y < 21) return CLOUD;  /* cloud 2 */
    if (x >=  58 && x <  82 && y >= 28 && y < 34) return CLOUD;  /* cloud 3 */
  }
  if (y >= 82 && y <= 90 && x >= 6 && x < 150) {   /* dc_fence(6, 150, 82) */
    int off = (x - 6) % 10;
    if (off >= 0 && off <= 2) return (y == 82 && off != 1) ? FD : FW;  /* post: dark corner dots at its top row, else post-light */
    if (y == 85) return FW;                         /* light rail, full width (gaps only -- posts already returned above) */
    if (y == 88) return FD;                         /* dark rail, gaps only (posts paint over it) */
  }
  return base;
}
#endif /* !HAVE_DAYCARE_BG */

void dc_pointer(int cx, int y) {           /* small downward arrow over the picked mon */
  const u16 c = RGB15(31, 28, 8);
  for (int j = 0; j < 5; j++) { int w = 4 - j; m3_line(cx - w, y + j, cx + w, y + j, c); }
}

/* Session RNG (a counter + the cart RTC when present). Used for created-mon PIDs
 * (app_create_mon) and, when the yard art is compiled in, the Day-Care visitors —
 * so it must live OUTSIDE the HAVE_DAYCARE_BG block: the artless build still
 * creates Pokemon.
 * app_tid_public() replaces the old direct `g_vinfo.tid_public` read (see this
 * file's header comment) — same value, narrow accessor instead of a raw global. */
static uint32_t dc_seed(void) {
  static uint32_t ctr = 0;
  ctr += 0x9E3779B9u;
  uint32_t e = ctr ^ ((uint32_t)app_tid_public() << 13);
  GbaRtcTime t;
  if (gba_rtc_get(&t)) e ^= (uint32_t)(t.second + t.minute * 60 + t.hour * 3600) * 2654435761u;
  return e | 1u;
}

/* pdna_app.h's public wrapper (G1 review BLOCKING-2): pdna_gen12.c's gb_create_hook
 * needs this EXACT entropy source (counter + TID + RTC), not qran() -- see the
 * header's own comment for why. dc_seed() itself stays file-static; every other
 * caller in this file already reaches it directly. */
uint32_t app_session_seed(void) { return dc_seed(); }

/* Areas on the daycare scene (icon-CENTRE SCREEN coords; the scene is drawn/blitted
 * starting at screen y 12), TWO mon slots each so two mons in the same area don't
 * overlap. This — the SIX-AREA TYPE GROUPING BELOW, dc_region_pick/dc_take_slot,
 * and the yard-visitor roll — is placement LOGIC, not art: it used to live entirely
 * inside `#ifdef HAVE_DAYCARE_BG`, so the artless build (no compiled yard bg) lost
 * it outright rather than falling back to it, which is why both were reported
 * missing from the artless Day-Care alongside the (genuinely art-only) background
 * image. It is unconditional now; only the SPOT COORDINATES differ by build, since
 * the compiled art's spots are aligned to painted terrain (tools/gen_daycare_img.py)
 * that the procedural scene has no equivalent of. */
enum { DR_LAVA, DR_WATER, DR_SKY, DR_ELEC, DR_GRASS, DR_EMPTY, DR_COUNT };
#ifdef HAVE_DAYCARE_BG
/* Two slots per area, spaced so the 32x32 icons don't overlap (>=32px apart in one
 * axis) — the lava pit is tall so its two slots stack vertically, the others spread
 * horizontally. cx/cy are CENTERS; the icon is drawn at (cx-16, cy-16). */
static const struct { int cx, cy; } DC_SPOT[DR_COUNT][2] = {
  { { 34, 28}, { 52, 64} },    /* LAVA  - rock/ground/fire (red area; stacked) */
  { {154, 82}, {208, 96} },    /* WATER - water (below the waterfall)      */
  { {156, 28}, {206, 36} },    /* SKY   - flying (the trees, top-right)    */
  { { 88, 34}, {124, 52} },    /* ELEC  - electric (yellow diamond area)   */
  { { 26, 94}, { 66,108} },    /* GRASS - grass/bug (the flower beds)      */
  { { 96, 84}, {134,104} },    /* EMPTY - everyone else (plain grass)      */
};
#else
/* No compiled yard art: the SAME six labelled areas, as generic anchor points spread
 * across the procedural scene (dc_scene()'s #else branch: sky y 12..90, grass y
 * 90..160), clear of the drawn house (x>=166) and mostly clear of the fence line
 * (y~82-91). Not thematically matched to a picture — there is no lava/water for a
 * mon to stand on — only the "sorted by type into one of six areas" BEHAVIOUR is
 * being restored here, same as it always was for the real boarders in the art
 * build; a fire-type still visibly clusters apart from a water-type. */
static const struct { int cx, cy; } DC_SPOT[DR_COUNT][2] = {
  { { 26, 34}, { 26, 66} },    /* LAVA  */
  { {130, 34}, {130, 66} },    /* WATER */
  { { 78, 22}, {110, 22} },    /* SKY   */
  { { 46, 60}, { 96, 66} },    /* ELEC  */
  { { 20,104}, { 56,112} },    /* GRASS */
  { { 96,104}, {132,112} },    /* EMPTY */
};
#endif
int      s_deco_x[PDNA_YARD_MAXDECO], s_deco_y[PDNA_YARD_MAXDECO];
uint16_t s_deco_sp[PDNA_YARD_MAXDECO];
int      s_ndeco = 0;                  /* decoration mons actually placed       */
static uint16_t s_deco_roll[PDNA_YARD_MAXDECO];  /* the 2..5 random species this visit     */
static int      s_ndeco_roll = 0;
static uint32_t s_dc_visit_rng = 1;           /* per-visit stream for dual-type area picks */
/* Areas a species may live in, by the user's type rules: FIRE -> lava (so a rock/
 * ground mon is only in the lava if it's also fire); water -> water; flying -> trees;
 * electric -> yellow; grass/bug -> grass. A dual-type with two qualifying types gets
 * BOTH areas and *rng picks one (so e.g. a Water/Flying mon randomly sits in the water
 * or the trees each visit). No qualifying type (incl. non-fire rock/ground) -> empty. */
static int dc_region_pick(uint16_t species, uint32_t* rng) {
  /* Each of the mon's TWO types can place it: fire->lava, water->water, flying->sky,
   * electric->elec, grass/bug->grass. A type with no dedicated area (dragon, normal,
   * rock/ground when not also fire, ...) instead allows the fallback spots: grass + empty.
   * So e.g. Dragonite (dragon+flying) can be in the sky OR the grass/empty, and a plain
   * rock/ground mon stays in grass/empty (only fire puts a rock/ground mon in the lava). */
  uint8_t t[2] = { pk_species_type1(species), pk_species_type2(species) };
  int cand[DR_COUNT], nc = 0;
  bool fallback = false;
  for (int i = 0; i < 2; i++) {
    if (i == 1 && t[1] == t[0]) break;                          /* mono-type: count once */
    int area;
    switch (t[i]) {
      case 10: area = DR_LAVA;  break;                          /* fire    */
      case 11: area = DR_WATER; break;                          /* water   */
      case 2:  area = DR_SKY;   break;                          /* flying  */
      case 13: area = DR_ELEC;  break;                          /* electric*/
      case 12: case 6: area = DR_GRASS; break;                  /* grass/bug */
      default: fallback = true; continue;                       /* no area -> grass + empty */
    }
    int dup = 0; for (int k = 0; k < nc; k++) if (cand[k] == area) dup = 1;
    if (!dup) cand[nc++] = area;
  }
  if (fallback) {
    int g = 0, e = 0; for (int k = 0; k < nc; k++) { if (cand[k] == DR_GRASS) g = 1; if (cand[k] == DR_EMPTY) e = 1; }
    if (!g) cand[nc++] = DR_GRASS;
    if (!e) cand[nc++] = DR_EMPTY;
  }
  if (nc == 0) return DR_EMPTY;
  if (nc == 1) return cand[0];
  *rng = *rng * 1103515245u + 12345u;
  return cand[(*rng >> 16) % (uint32_t)nc];
}
/* Reserve a slot in area `rg` (2 per area); if full, fall back to the EMPTY area.
 * Returns rg*2+slot, or -1 if even EMPTY is full. */
static int dc_take_slot(int used[DR_COUNT][2], int rg, uint32_t* rng) {
  int s0 = 0;                                  /* when both slots free, pick one at random */
  if (!used[rg][0] && !used[rg][1]) { *rng = *rng * 1103515245u + 12345u; s0 = (int)((*rng >> 16) & 1u); }
  if (!used[rg][s0])     { used[rg][s0] = 1;     return rg * 2 + s0; }
  if (!used[rg][1 - s0]) { used[rg][1 - s0] = 1; return rg * 2 + (1 - s0); }
  if (!used[DR_EMPTY][0]) { used[DR_EMPTY][0] = 1; return DR_EMPTY * 2 + 0; }
  if (!used[DR_EMPTY][1]) { used[DR_EMPTY][1] = 1; return DR_EMPTY * 2 + 1; }
  return -1;
}
/* Roll this visit's YARD VISITORS (called once on entry).
 *
 * READ THIS BEFORE BELIEVING THE YARD. The Gen-3 Day-Care holds EXACTLY TWO
 * Pokemon: dc_rescan reads slots 0 and 1 at `sb1 + base + i*stride` and nothing
 * else. Every other mon on this screen is scenery THIS VIEWER INVENTS for the
 * picture — 2..5 random species, re-rolled on every entry, seeded from a session
 * counter and the cart RTC. They are never read from or written to the save, they
 * occupy no slot, and they can never be selected (`sel` only indexes the boarders,
 * L/R only swaps when n == 2, and the A-menu acts on recs[sel]).
 *
 * They are always internal species 1..251, i.e. Kanto/Johto only — never a Hoenn
 * mon — which is itself the tell that they are not save data. The screen now says
 * so out loud (pk_daycare_yard_note), draws them hazed and behind the real pair,
 * and Settings > Yard visitors turns them off.
 *
 * Unconditional (not `#ifdef HAVE_DAYCARE_BG`): the roll itself is pure RNG/logic
 * and app_yard_visitors_ok() already separately gates on a ROM being registered
 * (so there is real icon art to draw them with) — that gate, not the compiled-art
 * ifdef, is the right reason for them to stay off. */
void pdna_yard_roll(uint16_t max_dex) {
  s_dc_visit_rng = pdna_yard_roll_core(dc_seed(), max_dex, s_deco_roll, &s_ndeco_roll);
}

void dc_visitors_off(void) {
  s_ndeco_roll = 0;
  s_ndeco = 0;
  s_dc_visit_rng = dc_seed();
}
/* Scene bottom bound for icon placement/compositing: the compiled bg's own height,
 * or (procedural build) the usable area above the info panel (PDNA_DCY_PANEL_Y).
 * DAYCARE_BG_W is always UI_SCR_W (240, tools/gen_daycare_bg.py's OUT_W) — see
 * dc_icon_over_bg below, which used to bound against DAYCARE_BG_W directly. */
#ifdef HAVE_DAYCARE_BG
#define DC_SCENE_BOT (12 + DAYCARE_BG_H)
#else
#define DC_SCENE_BOT 122
#endif
/* Compose a 32x32 icon over the TRUE, unpainted-by-icons background at screen (x,y)
 * and write each scanline (no separate erase => no flicker on the single Mode-3
 * buffer) -- DMA at load time, memcpy32 on the idle-bob tick, see `tick` below and
 * this function's own transport comment. x is forced even for the word alignment.
 *
 * The background sample MUST NOT be the framebuffer this function (or the previous
 * tick's call to it) already drew into: a read-back out of vid_mem is self-
 * referential -- an icon's now-transparent pixels stop being erased back to real
 * terrain (they keep whatever the icon painted there last tick, so the bob
 * silhouette only ever grows and freezes solid), and a hazed blend re-blends
 * against its own already-blended output every tick instead of the original
 * terrain colour, so it saturates to fully opaque in about a second instead of
 * staying hazed. (Both were a real regression here once -- see git history for
 * source/pdna_main.c around dc_icon_over_bg if this comment ever needs the receipts.)
 * The source must be a fixed lookup that ignores anything an icon composite wrote:
 * the compiled yard's own pixel data (HAVE_DAYCARE_BG's daycare_bg[], exactly like
 * the original art-only version of this function) or, without compiled art,
 * dc_bg_px()'s closed-form recomputation of what dc_scene()'s procedural branch
 * paints at that pixel -- never vid_mem. */
/* `num` is the icon's weight out of 8 when it is blended with the yard behind it:
 * 8 = fully opaque (the player's own boarders), lower = hazed into the background
 * (the invented yard visitors, so they read as scenery). Same blend as
 * ui_panel_alpha. Deliberately NOT greyscale — grey already means "not seen yet"
 * in the Pokedex and would say the wrong thing here. */
/* `tick` (2026-08-29, PokeDNA B3 audit) selects the transport, NOT the composition: this
 * function is called both from day_care()'s one-shot redraw block (load-time, tick=false,
 * plain dma3_cpy) and from its idle-bob loop (`if (app_anim_enabled(ANIM_DAYCARE) && ...)`,
 * every ~30-vblank tick, tick=true, memcpy32) -- the exact shared-helper shape box_oam.c's
 * upload_tiles/upload_tiles_cpu split addresses (fd205bb): a per-vblank dma3_cpy is the
 * proven-on-hardware crash trigger, mechanism not pinned, and this function was DEAD on
 * that tick until 421cc1f un-gated ANIM_DAYCARE, so it never got a hardware run under the
 * DMA transport. Load-time DMA is untouched -- every shipping build already does that on
 * daycare entry and is fine (fd205bb: "Load-time DMA is NOT implicated"). A parameter
 * rather than a duplicate function: the composited loop (background sample, alpha blend,
 * bounds clamp) is the same ~30 lines either way, and duplicating it would let the two
 * copies drift -- only the final transport line differs. */
static u16 __attribute__((aligned(4))) s_dcline[MON_ICON_W];   /* 32-bit DMA needs word align */
void dc_icon_over_bg(int x, int y, const u16* icon, int num, bool tick) {
  if (!icon) return;
  rumble_io_suspend();   /* reads icon from ROM per pixel (ROM icon rung); mute the motor toggle */
  x &= ~1;
  for (int j = 0; j < MON_ICON_H; j++) {
    int yy = y + j;
    if (yy < 12 || yy >= DC_SCENE_BOT) continue;
    for (int i = 0; i < MON_ICON_W; i++) {
      int xx = x + i;
#ifdef HAVE_DAYCARE_BG
      u16 c = (xx >= 0 && xx < DAYCARE_BG_W) ? daycare_bg[(yy - 12) * DAYCARE_BG_W + xx] : 0;
#else
      u16 c = (xx >= 0 && xx < UI_SCR_W) ? dc_bg_px(xx, yy) : 0;
#endif
      u16 pxl = icon[j * MON_ICON_W + i];
      if (pxl & 0x8000) {
        u16 s = (u16)(pxl & 0x7FFF);
        if (num >= 8) c = s;
        else {
          int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
          r += ((( s        & 31) - r) * num) >> 3;
          g += ((((s >>  5) & 31) - g) * num) >> 3;
          b += ((((s >> 10) & 31) - b) * num) >> 3;
          c = (u16)(r | (g << 5) | (b << 10));
        }
      }
      s_dcline[i] = c;
    }
    if (tick) memcpy32(&vid_mem[yy * 240 + x], s_dcline, MON_ICON_W * 2 / 4);
    else      dma3_cpy(&vid_mem[yy * 240 + x], s_dcline, MON_ICON_W * 2);
  }
  rumble_io_resume();
}

void dc_scene(void) {
#ifdef HAVE_DAYCARE_BG
  rumble_io_suspend();   /* ~37 KB daycare_bg ROM->VRAM DMA; mute the cart-bus motor toggle */
  dma3_cpy(&vid_mem[12 * 240], daycare_bg, DAYCARE_BG_W * DAYCARE_BG_H * 2);  /* yard bg -> scene region */
  rumble_io_resume();
#else
  ui_fill_rect(0, 12, UI_SCR_W, 78, RGB15(16, 25, 31));        /* sky */
  for (int j = -5; j <= 5; j++) for (int i = -5; i <= 5; i++)  /* sun */
    if (i * i + j * j <= 25) m3_plot(22 + i, 26 + j, RGB15(31, 30, 14));
  ui_fill_rect(150, 20, 28, 6, RGB15(30, 31, 31)); ui_fill_rect(158, 16, 14, 5, RGB15(30, 31, 31));
  ui_fill_rect(58, 28, 24, 6, RGB15(30, 31, 31));
  ui_fill_rect(0, 90, UI_SCR_W, 70, RGB15(13, 22, 9));         /* grass (uniform: the bob erases to this) */
  dc_house();
  dc_fence(6, 150, 82);
#endif
}

/* (Re)scan the daycare: fill recs[]/dc[]/phys[] for the up to 2 boarders, place each by
 * terrain, and read the shared egg/step state. Used on entry and after a put/take so the
 * scene always matches `sb1`. Returns the boarder count. phys[k] = physical slot 0/1.
 * `sb1`/`game` replace the old direct `g_sb1`/`g_game` reads -- see this file's header
 * comment; every other line is unchanged from the old static dc_rescan(). */
int dc_rescan(uint8_t* sb1, PkGame game, uint32_t base, uint32_t stride,
             uint8_t* recs[2], PkMon dc[2], int phys[2],
             int dcx[2], int dcy[2], bool* egg, int* to_check) {
  int n = 0;
  for (int i = 0; i < 2; i++) {
    uint8_t* rec = sb1 + base + (uint32_t)i * stride; PkMon m;
    if (pk_decode_mon(rec, false, &m) && m.species >= 1 && m.species <= 411 && !m.isBadEgg) {
      pk_resolve(&m); dc[n] = m; recs[n] = rec; phys[n] = i; n++;
    }
  }
  /* Egg word @ base+280 in ALL games (u32 on E; u16 on FRLG AND RS), counter @282 (E @284).
   * Verified vs pret/pokeruby: DayCareMail.names @ +0x24 => MailStruct=36 => mail=56 B each;
   * sizeof(DayCare)=0x30B8-0x2F9C=284 => RS: mons 160 + mail 112 + steps@272 + egg@280.
   * (The old RS 276/278 came from a wrong 54-byte-mail assumption.) */
  const uint8_t* op = sb1 + base + 280;
  *egg = (game == PK_EMERALD) ? ((op[0] | op[1] | op[2] | op[3]) != 0) : ((op[0] | op[1]) != 0);
  int stepc = (game == PK_EMERALD) ? sb1[base + 284] : sb1[base + 282];
  int tc = (game == PK_RS) ? stepc : (256 - stepc); if (tc < 1 || tc > 256) tc = 256; *to_check = tc;
  /* "Special areas": each boarder -> a slot in its type area (random for dual-type).
   * Unconditional in both builds now — DC_SPOT supplies art-aligned coordinates with
   * the compiled bg, generic ones without it (see DC_SPOT's own comment above). */
  int used[DR_COUNT][2] = {{0}};
  uint32_t arng = s_dc_visit_rng;               /* per-visit stream: stable within a visit, varies across */
  for (int i = 0; i < n; i++) {
    int slot = dc_take_slot(used, dc_region_pick(dc[i].species, &arng), &arng);
    int rg = (slot < 0) ? DR_EMPTY : slot / 2, sp = (slot < 0) ? 0 : slot % 2;
    int cx = DC_SPOT[rg][sp].cx - 16, cy = DC_SPOT[rg][sp].cy - 16;
    cx &= ~1; if (cx < 2) cx = 2; else if (cx > UI_SCR_W - 34) cx = UI_SCR_W - 34;
    if (cy < 12) cy = 12; else if (cy > DC_SCENE_BOT - MON_ICON_H) cy = DC_SCENE_BOT - MON_ICON_H;
    dcx[i] = cx; dcy[i] = cy;
  }
  /* the 2..5 random decoration mons (rolled once per visit, or none — see
   * dc_roll_decos/app_yard_visitors_ok) -> their type areas */
  s_ndeco = 0;
  for (int d = 0; d < s_ndeco_roll && s_ndeco < PDNA_YARD_MAXDECO; d++) {
    int slot = dc_take_slot(used, dc_region_pick(s_deco_roll[d], &arng), &arng);
    if (slot < 0) continue;                      /* every slot full -> drop this deco */
    int rg = slot / 2, sp = slot % 2;
    int cx = DC_SPOT[rg][sp].cx - 16, cy = DC_SPOT[rg][sp].cy - 16;
    cx &= ~1; if (cx < 2) cx = 2; else if (cx > UI_SCR_W - 34) cx = UI_SCR_W - 34;
    if (cy < 12) cy = 12; else if (cy > DC_SCENE_BOT - MON_ICON_H) cy = DC_SCENE_BOT - MON_ICON_H;
    s_deco_sp[s_ndeco] = s_deco_roll[d]; s_deco_x[s_ndeco] = cx; s_deco_y[s_ndeco] = cy; s_ndeco++;
  }
  return n;
}

/* BACKLOG #114: the Gen-1/2 twin of dc_rescan's own two placement loops,
 * decoupled from SaveBlock1 (species in, screen coords out) -- see pdna_yard.h's
 * own comment on why boarders and visitors share ONE `used` matrix here too.
 * Reads from s_ndeco_roll/s_deco_roll (this visit's ROLL, from pdna_yard_roll())
 * and REBUILDS s_ndeco/s_deco_sp/s_deco_x/s_deco_y from scratch -- exactly
 * dc_rescan's own shape, so a visitor dropped for want of a free slot (every
 * area full, the same rare edge case dc_rescan already has) is dropped here
 * too, not left at a stale/garbage coordinate. */
void pdna_yard_place(uint32_t rng, const uint16_t* board_sp, int n_board,
                     int board_x[2], int board_y[2]) {
  int used[DR_COUNT][2] = {{0}};
  uint32_t arng = rng | 1u;
  for (int i = 0; i < n_board && i < 2; i++) {
    int slot = dc_take_slot(used, dc_region_pick(board_sp[i], &arng), &arng);
    int rg = (slot < 0) ? DR_EMPTY : slot / 2, sp = (slot < 0) ? 0 : slot % 2;
    int cx = DC_SPOT[rg][sp].cx - 16, cy = DC_SPOT[rg][sp].cy - 16;
    cx &= ~1; if (cx < 2) cx = 2; else if (cx > UI_SCR_W - 34) cx = UI_SCR_W - 34;
    if (cy < 12) cy = 12; else if (cy > DC_SCENE_BOT - MON_ICON_H) cy = DC_SCENE_BOT - MON_ICON_H;
    board_x[i] = cx; board_y[i] = cy;
  }
  s_ndeco = 0;
  for (int d = 0; d < s_ndeco_roll && s_ndeco < PDNA_YARD_MAXDECO; d++) {
    int slot = dc_take_slot(used, dc_region_pick(s_deco_roll[d], &arng), &arng);
    if (slot < 0) continue;                      /* every slot full -> drop this visitor */
    int rg = slot / 2, sp = slot % 2;
    int cx = DC_SPOT[rg][sp].cx - 16, cy = DC_SPOT[rg][sp].cy - 16;
    cx &= ~1; if (cx < 2) cx = 2; else if (cx > UI_SCR_W - 34) cx = UI_SCR_W - 34;
    if (cy < 12) cy = 12; else if (cy > DC_SCENE_BOT - MON_ICON_H) cy = DC_SCENE_BOT - MON_ICON_H;
    s_deco_sp[s_ndeco] = s_deco_roll[d]; s_deco_x[s_ndeco] = cx; s_deco_y[s_ndeco] = cy; s_ndeco++;
  }
}
