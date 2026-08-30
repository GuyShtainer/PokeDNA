/*
 * Game-faithful paged Pokémon summary (6 cards) for PokeDNA — now an
 * inline VIEW + EDIT screen. Each editable field registers its on-screen box
 * during render; in edit mode a cursor highlights one and A / LEFT-RIGHT edit it
 * in place (reusing the editor's field dispatchers). No separate "edit" mode.
 *   cards: 0 INFO  1 SKILLS  2 IVs  3 EVs  4 BATTLE MOVES  5 CONTEST MOVES
 *          6 ORIGIN  7 CONDITION (cool/beauty/cute/smart/tough/sheen, editable)
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "sys.h"          /* EWRAM_BSS: the selection outline's save-under buffer */
#include "pdna_summary.h"
#include "perf.h"        /* the summary-open rollup (telemetry) */
#include "pdna_app.h"     /* app_anim_enabled (portrait animation) */
#include "ui.h"
#include "gen3_mon.h"
#include "gen3_edit.h"
#include "gen3_places.h"   /* g3_region_of / g3_region_name for the Region row */
#include "gen3_box.h"      /* pk_resolve */
#include "data_tables.h"
#include "pdna_edit.h"   /* F_*, em_field_press / em_field_adjust */
#include "mon_front.h"
#include "mon_back.h"
#include "mon_icons.h"
#include "pdna_origin_art.h"   /* draw the mon in the art of the generation it came FROM */
#include "mon_anim.h"     /* per-species Emerald front-animation family */
#include "type_icons.h"
#include "snd.h"
#include "rumble.h"       /* rumble_io_suspend/resume around the ROM-read portrait LZ77 */
#include "gen3_ivroll.h"  /* the IV reroll: one shared roller, an undo/redo list */
#include "pdna_layout.h"  /* the reroll row's geometry + every string it draws */

#define NCARDS 8

/* Summary portrait animation (the Emerald entrance/idle wiggle). Compiled in, but
 * gated at runtime on the ANIM_SUMMARY toggle (Settings > Animations > Summary),
 * which defaults OFF — the static portrait from draw_left stands in until the user
 * opts in. Set to 0 to compile it out entirely. */
#define SUMMARY_ANIM 1

/* real Gen-3 type badge (32x14); ui_sprite honours the 0x8000 opacity bit.
 * Art-free build: the original coloured type chip stands in. */
static void type_badge(int x, int y, uint8_t t) {
  if (t >= 18) return;
  /* app_type_badge adds the ROM rung on top of the compiled type_icon_for() (Phase 1,
   * docs/analysis-2026-08-19-rom-art/DESIGN.md Sec 4.7). Decodes into the SAME
   * mon_decomp scratch the portrait below uses, but strictly BEFORE this card's own
   * portrait fetch runs (draw_left calls this after it has already blitted the
   * portrait -- see the "Fetch the portrait ONCE per repaint" comment further down),
   * so there is no ordering hazard here; a ROM badge is not the compiled 14 px crop
   * (16 RSE / 12 FRLG), so draw the height it actually reports. */
  uint8_t h = TYPE_ICON_H;
  const uint16_t* ic = app_type_badge(t, &h);
  if (ic) ui_sprite(x, y, TYPE_ICON_W, h, ic);
  else    ui_type_chip(x, y, TYPE_ICON_W, TYPE_ICON_H, t);
}

static const int   DISP[6]   = { PK_HP, PK_ATK, PK_DEF, PK_SPA, PK_SPD, PK_SPE };
static const char* DSHORT[6] = { "HP", "Atk", "Def", "SpA", "SpD", "Spd" };

#define C_HDR  UI_TITLE
#define C_KEY  UI_DIRCLR
#define C_VAL  UI_TEXT
#define C_HOT  UI_WARN

/* The reroll's entropy: this counts vblanks, so the seed depends on WHEN A was pressed —
 * the only unpredictable thing a GBA has. gba_rtc_get is second-resolution and simply
 * absent with the RTC switched off, so it cannot be the source. */
static uint32_t s_ticks = 0;
static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); s_ticks++; }

/* SELECT flips the portrait between front and back sprite (issue #12); persists
 * across mon-scroll within a summary session. */
static bool g_back = false;

/* ---- editable-field slot registry (filled during render in edit mode) ---- */
static int g_edit = 0, g_nslot = 0;
static int g_create = 0;          /* CREATE mode: render the NEW chip, not VIEW/EDIT */
static struct { int field, x, y, w; } g_slot[24];
static void reg(int field, int x, int y, int w) {
  if (g_edit && g_nslot < 24) { g_slot[g_nslot].field = field; g_slot[g_nslot].x = x;
                                g_slot[g_nslot].y = y; g_slot[g_nslot].w = w; g_nslot++; }
}

/* F_SUM_REROLL is a SUMMARY-LOCAL pseudo-field, one past pdna_edit.h's F_NUM and
 * deliberately NOT an entry in that enum: the field-list editor walks 0..F_NUM-1 and indexes
 * FLABEL[] by the same id (pdna_edit.c:117-126), so a real field id would put a dead
 * "Reroll" row in a screen with no history to step and no confirm panel. It rides the slot
 * registry purely to inherit focus, the highlight frame and U/D navigation; summary_run
 * intercepts it before em_field_press/em_field_adjust ever see it.
 * LIFETIME = ONE POKEMON: ivh_reset runs at the top of summary_run, and every mon change
 * RETURNS from summary_run for the caller to call again (pdna_main.c:944, :968, :3238,
 * :3346), so a redo entry can never be applied to a different mon. ivh_sync, once per
 * repaint, is the other half of that promise. */
#define F_SUM_REROLL (F_NUM + 0)
_Static_assert(F_SUM_REROLL >= F_NUM, "summary pseudo-fields must sit past the real enum");
static IvHistory g_ivh;
static IvRollState g_roll_st;      /* seed survives across mons: never reset it */
static bool      g_last_ivonly = false;   /* the newest roll kept the PID */
static bool      g_have_roll   = false;

static void draw_dots(int x, int y, int n, int active) {
  for (int i = 0; i < n; i++) {
    int cx = x + i * 7;
    if (i == active) ui_fill_rect(cx, y, 5, 5, C_HDR);
    else { ui_fill_rect(cx, y, 5, 5, UI_PANEL); m3_frame(cx, y, cx + 4, y + 4, UI_BORDER); }
  }
}

static int text_wrap(int x, int y, int cols, u16 ink, const char* s) {
  char line[64];
  int li = 0;
  while (*s) {
    const char* w = s; int wl = 0;
    while (s[wl] && s[wl] != ' ') wl++;
    int need = (li ? li + 1 : 0) + wl;
    if (need > cols && li) { line[li] = 0; ui_text(x, y, ink, line); y += UI_ROW_H; li = 0; }
    if (li) line[li++] = ' ';
    for (int i = 0; i < wl && li < (int)sizeof(line) - 1; i++) line[li++] = w[i];
    s += wl;
    while (*s == ' ') s++;
  }
  if (li) { line[li] = 0; ui_text(x, y, ink, line); y += UI_ROW_H; }
  return y;
}

static u16  gender_col(uint8_t g) { return g == 0 ? RGB15(12, 18, 31) : g == 1 ? RGB15(31, 13, 19) : UI_DIM; }
static const char* gender_sym(uint8_t g) { return g == 0 ? "M" : g == 1 ? "F" : "-"; }

/* ---- Emerald-style blue gradient backdrops ---- */
/* The sprite "screen" behind the portrait (yy in 14..77): a light-blue vertical
 * gradient, like the real summary. Used by draw_left AND portrait_redraw so the
 * static and animated sprite sit on the same backdrop. */
static u16 portrait_bg(int yy) {
  int t = yy - 14; if (t < 0) t = 0; if (t > 63) t = 63;
  return RGB15(16 - t * 6 / 63, 22 - t * 6 / 63, 30 - t * 4 / 63);
}
/* Whole-screen blue gradient (medium-dark so the light card text stays readable).
 *
 * Deliberately never paints (0,11)-(92,150): draw_left's own ui_panel() unconditionally
 * fills that exact rect the moment it runs, so painting it here was always overwritten
 * and wasted -- and now that draw_left can be SKIPPED on an unchanged mon+pose
 * (draw_left_conditional below, the fix for the "renders from scratch" regression),
 * painting it here would wipe the preserved portrait/name/type-badge pixels with plain
 * gradient and nothing would ever put them back. Band 1 (rows 10-19) straddles the
 * rect's top edge (row 11) and is split; every other band is either fully inside or
 * fully outside it. */
/* One band's colour. Factored out of summary_bg's loop so the footer strip below can
 * restore EXACTLY the pixels summary_bg would have put there, from the same expression,
 * rather than a second copy of the formula that could drift from this one. */
static u16 summary_bg_col(int b) { return RGB15(6 - b / 5, 11 - b / 3, 22 - b / 2); }
#define SUMMARY_BG_FOOT_BAND 15        /* the band rows 150..159 (and so the footer) live in */

static void summary_bg(void) {
  for (int b = 0; b < 16; b++) {
    int y = b * 10;
    u16 col = summary_bg_col(b);
    if (b == 1) {                                              /* rows 10..19: split at row 11 */
      ui_fill_rect(0, y, UI_SCR_W, 1, col);                    /* row 10: outside the rect      */
      ui_fill_rect(92, y + 1, UI_SCR_W - 92, 9, col);          /* rows 11..19: rect's left 92px owned by draw_left */
    } else if (y >= 11 && y + 10 <= 150) {
      ui_fill_rect(92, y, UI_SCR_W - 92, 10, col);             /* fully inside the rect's row range */
    } else {
      ui_fill_rect(0, y, UI_SCR_W, 10, col);                   /* fully outside (header row / footer rows) */
    }
  }
}

/* Shared portrait column (all 7 cards): framed sprite, dex no, name, Lv + colored
 * sex, species, type badges, and an egg/shiny tag. No editable fields here. */
static void draw_left(const PkMon* p) {
  ui_panel(0, 11, 92, 139, RGB15(4, 7, 16), UI_BORDER);    /* dark-blue info column */
  m3_frame(11, 13, 80, 78, UI_BORDER);                     /* sprite sub-frame */
  for (int yy = 14; yy <= 77; yy++) ui_fill_rect(12, yy, 68, 1, portrait_bg(yy));   /* blue "screen" */
  rumble_io_suspend();   /* portrait fetch decompresses from ROM (Gen-3 LZ77 or GB pic) */
  /* One accessor decides the era: a mon converted from a Game Boy save wears its OWN
   * generation's art when that cartridge is registered, and everything else takes the
   * front/back ladder this used to open-code. With no GB ROM registered the result is
   * the same pointer and the same (14,14) 64x64 blit as before. */
  PdnaArt art; PdnaOrigin org;
  pdna_origin_art_portrait(p, g_back, &art, &org);
  rumble_io_resume();
  if (art.px) {
    int ax, ay;
    pdna_origin_art_place(&art, 12, 14, 68, 64, &ax, &ay);   /* 64x64 -> (14,14) exactly */
    ui_sprite(ax, ay, art.w, art.h, art.px);
  } else if (art.egg) {
    ui_sprite(30, 30, MON_ICON_W, MON_ICON_H, mon_icon_egg());
  } else {
    ui_sprite(30, 30, MON_ICON_W, MON_ICON_H, mon_icon_for_form(p->species, p->form));
  }
  if (p->isShiny) ui_text(70, 16, C_HOT, "*");             /* gold shiny mark on the portrait */
  /* Provenance stamp — drawn ONLY for a GB import, so a native Gen-3 mon's portrait is
   * pixel-identical to before. Opaque chip rather than bare text so it stays readable
   * over whatever the sprite puts behind it. "GB1?" = Gen 1 by elimination, i.e. the
   * record cannot prove it is not a Gen-2 Kanto mon (pdna_origin_art.h). */
  if (org.verdict == PDNA_ORIGIN_GB)
    ui_name_chip(13, 15, 30, 11, pdna_origin_color(&org), 0x7FFF, pdna_origin_tag(&org));

  char buf[40];
  ui_hline(4, 81, 84, UI_BORDER);
  siprintf(buf, "#%03u", (unsigned)pk_national_no(p->species));
  ui_text(6, 84, C_KEY, buf);
  if (p->species == 410) {                                 /* Deoxys (internal 410): show the current forme (SELECT cycles it) */
    static const char* const DF[4] = { "Normal", "Attack", "Defense", "Speed" };
    ui_text(40, 84, C_HOT, DF[p->form < 4 ? p->form : 0]);
  } else if (g_back) ui_text(44, 84, UI_DIM, "back");      /* current portrait side */

  ui_ptext_fit(6, 94, 86, C_VAL, p->nickname[0] ? p->nickname : pk_species_name(p->species));

  siprintf(buf, "Lv%u", (unsigned)p->level);
  ui_text(6, 104, C_VAL, buf);
  ui_text(6 + (int)strlen(buf) * 8 + 4, 104, gender_col(p->gender), gender_sym(p->gender));

  ui_ptext_fit(6, 114, 86, C_KEY, pk_species_name(p->species));

  uint8_t t1 = pk_species_type1(p->species), t2 = pk_species_type2(p->species);
  type_badge(6, 126, t1);
  if (t2 != t1) type_badge(42, 126, t2);

  if (p->isBadEgg)   ui_text(6, 142, UI_WARN, "BAD EGG");
  else if (p->isEgg) ui_text(6, 142, C_HOT, "EGG");
  else if (p->pokerus) ui_text(6, 142, UI_WARN, "Pokerus");
}

/* ---- draw_left, made conditional on the mon+pose actually having changed ----------
 *
 * THE BUG (Guy's report, 2026-08-23): "editing a pokemon or simply scrolling through
 * its stats, the whole screen renders from scratch". render_card() runs unconditionally
 * on EVERY keypress in summary_run's loop -- always did, since the inline-edit rewrite
 * (a30647c) -- and that was cheap and invisible right up until this month's ROM-art
 * work gave draw_left() real I/O: the portrait fetch (pdna_origin_art_portrait ->
 * rom_portrait, DELIBERATELY not memoised, up to 3 verified SD reads + an 8 KB LZ77
 * decode) and up to two type_badge() calls (app_type_badge, "no cache and no memo,
 * matching Phase 1's pure wiring scope" -- reloads a 5,888 B ROM sheet from SD EVERY
 * call, commit 63c0b45, 2026-08-19). On Guy's actual configuration -- the artless
 * build with his ROM registered on SD, where the compiled art rungs are absent and
 * EVERY portrait/badge falls through to the SD rung -- that is real flashcart I/O
 * firing on a UP/DOWN that only moves the field cursor one row, something that changes
 * NOTHING about the mon's portrait, name, level, gender, type or egg/Pokerus tag. It
 * will not show up in a fused capture (a cart memcpy, not the SD path) or in mGBA
 * (which has no EZ-Flash SD interface to exercise at all) -- see
 * docs/analysis-2026-08-23/MEASUREMENTS.md.
 *
 * draw_left()'s entire output is a pure function of (*p, g_back): nothing about it
 * depends on `card`, `fsel` or `editing`. So a byte-for-byte-identical (*p, g_back) as
 * last time means draw_left would paint EXACTLY the same pixels it already painted --
 * skip it and the correct pixels are already on screen. memcmp over the whole PkMon
 * (not a hand-picked subset of "the fields that affect art") is deliberate: PkMon also
 * drives pdna_origin_of()'s GB-import verdict inside pdna_origin_art_portrait (it reads
 * otId/metLocation/pokeball/language/ivs/evs/contest/ribbons/nature/experience/moves/
 * friendship), which the "GB1?"/"GB2?" chip depends on -- a subset keyed only on the
 * fields that affect the SPRITE would go stale the moment an EV/IV edit flips that
 * verdict without changing species/form/shiny. A false MISS here (the snapshot differs
 * only in struct padding, or after gen3_edit_load() lands the mon at a different stack
 * address) just falls back to the always-correct full draw -- there is no way for this
 * comparison to produce a false HIT that hides a real change. The snapshot is one
 * PkMon's worth of plain .bss (no arrays added, no EWRAM) -- a few dozen bytes against
 * the 6,108 B IWRAM-stack floor CLAUDE.md warns is nearly spent, nowhere near the
 * 3,360 B commit that got reverted for costing every call chain 3,424 B.
 *
 * MUST-FIX (2026-08-23 review, reproduced not just reasoned): the "byte-identical
 * mon" memo above is necessary but NOT sufficient -- it says nothing about what else
 * painted the screen in between. em_field_press() (pdna_edit.c, called from EDIT
 * mode's KEY_A handler below) routes F_SPECIES/F_NICK/F_OT/F_ITEM/F_MV0-3/F_NATURE/
 * F_ABILITY/F_BALL/F_METLOC/F_METREGION into a picker (pdna_pick.c) or osk_input()
 * (osk.c) -- full-screen overlays, reached through OTHER FILES, that ui_clear() and
 * draw full-width rows straight through this rect. A field's CANCEL path (B in the
 * picker) returns the mon byte-for-byte unchanged, so the old code here saw an
 * identical PkMon and skipped draw_left -- leaving the picker's own pixels (its list,
 * its selection box) sitting in the info column forever, healed by nothing (not even
 * a further keypress, since the mon still doesn't change). A first version of this
 * fix enumerated the field ids above by hand; that list already left out F_NATURE
 * (also routes through pick_nature -- see em_field_press) on its first pass, which is
 * exactly the failure mode of a hand-audited list: it silently stops being true the
 * next time a field or a picker is added. So invalidation is NOT keyed to field ids
 * or to "which dialogs live in this file" -- it is keyed to ui_clear_gen() (ui.h),
 * one counter every full-screen overlay in the codebase already bumps for free by
 * calling ui_clear() before it draws. draw_left_conditional remembers the generation
 * it last painted in; ANY ui_clear() since then -- a picker, the OSK, confirm_q()'s
 * own ui_clear() right before its `continue` back into the SAME session (CREATE
 * mode's confirm_keep()==false "B = carry on editing" branch), or the top-of-
 * summary_run() reset for a fresh session -- forces a real repaint even when the mon
 * snapshot still matches. pd_summary_left_dirty() (the explicit call at the top of
 * summary_run and inside confirm_q) stays as an extra, redundant belt: it forces the
 * same outcome without relying on the generation counter, so the two mechanisms fail
 * independently rather than sharing one blind spot. */
static PkMon s_dl_snap;
static bool  s_dl_snap_valid = false;
static bool  s_dl_snap_back  = false;
static uint32_t s_dl_snap_gen = 0;   /* ui_clear_gen() as of the last real paint */

static void pd_summary_left_dirty(void) { s_dl_snap_valid = false; }

/* Returns true iff draw_left() actually ran (false on a skip). summary_run's caller
 * MUST use this, not assume it: the "render_card drew the rest pose" lastkey reset a
 * few lines below it is only true when draw_left really ran. Skip that reset on a
 * false return and the idle-bob animation's diff check (portrait_redraw's own lastkey,
 * pdna_summary.c) keeps comparing against what is ACTUALLY on screen -- the pose the
 * previous outer-loop iteration's animation left it at, not an assumed rest pose that
 * was never drawn. Getting this wrong doesn't corrupt anything (portrait_redraw's own
 * redraw is self-contained and always paints a complete, correct frame WHEN it fires),
 * it just means an occasional skipped animation frame -- caught empirically by diffing
 * mGBA screenshots against the pre-fix build frame-for-frame
 * (docs/analysis-2026-08-23/MEASUREMENTS.md) before this return value was wired up. */
static bool draw_left_conditional(const PkMon* p) {
  uint32_t gen = ui_clear_gen();
  /* gen == s_dl_snap_gen is the fix: a picker/OSK cancel leaves the mon byte-identical
   * but has ui_clear()-ed the screen since -- see the header comment above for the
   * repro this closes. */
  if (s_dl_snap_valid && gen == s_dl_snap_gen && g_back == s_dl_snap_back &&
      memcmp(p, &s_dl_snap, sizeof *p) == 0)
    return false;                               /* identical to last time: already on screen */
  draw_left(p);
  s_dl_snap = *p;
  s_dl_snap_back = g_back;
  s_dl_snap_gen = gen;
  s_dl_snap_valid = true;
  return true;
}

/* Right-hand card column: x=98, so 138 px to the screen edge with a 4 px margin.
 * Everything here is drawn with the proportional face — at 8 px/glyph the column was
 * 17 characters, which cut "Hatch ~10240 ste~" and "40 egg cycles le~" mid-word. */
#define INFO_W 138

static void card_info(const PkMon* p) {
  int x = 98, y = 14; char b[48];
  ui_text(x, y, C_HDR, "POKEMON INFO"); y += 12;

  if (p->isEgg && !p->isBadEgg) {        /* an egg stores its hatch counter in friendship */
    siprintf(b, "~%u steps to hatch", (unsigned)(p->friendship * 256));
    ui_ptext_fit(x, y, INFO_W, C_HOT, b); y += 10;
    siprintf(b, "%u egg cycle%s left", (unsigned)p->friendship, p->friendship == 1 ? "" : "s");
    ui_ptext_fit(x, y, INFO_W, UI_DIM, b); y += 11;
  }

  ui_text(x, y, C_KEY, "Spec."); reg(F_SPECIES, x + 48, y, 88);
  ui_text(x + 48, y, C_VAL, pk_species_name(p->species)); y += 9;

  ui_text(x, y, C_KEY, "Name"); reg(F_NICK, x + 48, y, 88);
  ui_ptext_fit(x + 48, y, INFO_W - 48, C_VAL, p->nickname[0] ? p->nickname : "-"); y += 9;

  ui_text(x, y, C_KEY, "OT"); reg(F_OT, x + 48, y, 88);
  ui_text(x + 48, y, C_VAL, p->otName); y += 9;               /* TID on its own row (was off-screen) */
  siprintf(b, "TID %05u", (unsigned)(p->otId & 0xFFFF)); ui_text(x, y, UI_DIM, b); y += 9;

  uint8_t t1 = pk_species_type1(p->species), t2 = pk_species_type2(p->species);
  ui_text(x, y, C_KEY, "Type");
  if (t1 == t2) type_badge(x + 46, y - 2, t1);
  else { type_badge(x + 46, y - 2, t1); type_badge(x + 80, y - 2, t2); }
  y += 16;

  uint16_t ab = pk_species_ability(p->species, p->abilityNum);
  ui_text(x, y, C_KEY, "Abil."); reg(F_ABILITY, x + 48, y, 88);
  ui_ptext_fit(x + 48, y, INFO_W - 48, C_VAL, pk_ability_name(ab)); y += 9;
  y += UI_ROW_H * ui_ptext_wrap(x + 4, y, INFO_W - 4, UI_ROW_H, 0, UI_DIM, app_ability_desc(ab));
  y += 1;

  ui_text(x, y, C_KEY, "Nat."); reg(F_NATURE, x + 48, y, 60);
  ui_text(x + 48, y, C_HOT, pk_nature_name(p->nature)); y += 9;

  ui_text(x, y, C_KEY, "Shiny"); reg(F_SHINY, x + 48, y, 26);
  ui_text(x + 48, y, p->isShiny ? UI_OK : C_VAL, p->isShiny ? "Yes" : "No");
  ui_text(x + 80, y, C_KEY, "Sex"); reg(F_GENDER, x + 108, y, 22);
  ui_text(x + 108, y, gender_col(p->gender), p->gender == 0 ? "M" : p->gender == 1 ? "F" : "-"); y += 10;

  /* The footer lives at y=152 and `y` is a running cursor: the egg rows above push it down
   * 21 px, which is how this row once landed ON TOP of the footer. It was then dropped
   * entirely when it collided — leaving an obviously empty line, which Guy asked to use.
   * So clamp the POSITION instead of skipping the row: at y=142 it ends at 150, two pixels
   * clear of the footer, and the proportional face fits the whole location name. */
  if (y > 142) y = 142;
  /* Met level 0 is not a missing value: it is how Gen 3 records "this came out of an egg",
   * and retail prints a hatch sentence rather than a level — the decomp's own comment at
   * src/egg_hatch.c:384-386 reads "A met level of 0 is interpreted on the summary screen as
   * 'hatched at'". Rendering the raw 0 is what made a created L32 Venusaur look broken to
   * Guy ("met at lvl 0"); the byte was right, the word was missing. An unhatched egg keeps
   * the numeric row — it has not been met yet at all.
   * "Hatched " is 44 px against INFO_W = 138; "Hatched at " is 58 and would push
   * LITTLEROOT TOWN to 143 and PETALBURG WOODS to 145, i.e. it would newly truncate names
   * that fit today. Drop the "at". */
  if (p->metLevel == 0 && !p->isEgg)
    siprintf(b, "Hatched %s", pk_location_name(p->metLocation));
  else
    siprintf(b, "Met Lv%u %s", (unsigned)p->metLevel, pk_location_name(p->metLocation));
  ui_ptext_fit(x, y, INFO_W, UI_DIM, b);
}

static void card_skills(const PkMon* p) {
  int x = 98, y = 14; char b[48];
  ui_text(x, y, C_HDR, "SKILLS"); y += 11;
  ui_text(x, y, C_KEY, "Level"); reg(F_LEVEL, x + 60, y, 40);
  siprintf(b, "%u", (unsigned)p->level); ui_text(x + 60, y, C_VAL, b); y += 9;
  ui_text(x, y, C_KEY, "Item"); reg(F_ITEM, x + 60, y, 76);
  { char it_[48];
    if (p->heldItem) pk_item_label(p->heldItem, it_, sizeof it_); else strcpy(it_, "none");
    ui_ptext_fit(x + 60, y, 238 - (x + 60), C_VAL, it_); } y += 9;
  ui_text(x, y, C_KEY, "Friend"); reg(F_FRIEND, x + 60, y, 40);
  siprintf(b, "%u", (unsigned)p->friendship); ui_text(x + 60, y, C_VAL, b); y += 10;
  /* Each stat row edits that stat's EV — the only persistent, lossless stat lever
   * in Gen-3 (final stats are derived from base+IV+EV+level+nature). The number
   * updates live; full IV/EV grids remain on the IV/EV cards. */
  for (int i = 0; i < 6; i++) {
    int s = DISP[i], bo = pk_nature_boost(p->nature), h = pk_nature_hinder(p->nature);
    u16 col = (s == bo) ? UI_OK : (s == h) ? UI_WARN : C_VAL;
    reg(F_EV0 + s, x, y, 138);
    siprintf(b, "%-3s", DSHORT[i]); ui_text(x, y, C_KEY, b);   /* 3-char label clears the value column */
    siprintf(b, "%4u", (unsigned)p->stats[s]); ui_text(x + 30, y, col, b);
    siprintf(b, "EV%u", (unsigned)p->evs[s]);  ui_text(x + 72, y, UI_DIM, b);
    y += 10;                                                   /* 10px stride: edit-frames don't share a scanline */
  }
  ui_text(x, y, UI_DIM, "<>: train EVs"); y += 9;

  /* EXP: current total + how much remains for the next level and for Lv100.
   * Box mons store EXP (Growth +4) even though level/stats are computed; the
   * curve comes from the species' growth-rate table (pk_exp_for_level). */
  ui_hline(x, y - 2, 138, UI_BORDER);
  uint8_t gr = pk_species_growth(p->species);
  uint32_t exp = p->experience;
  siprintf(b, "EXP %lu", (unsigned long)exp); ui_text(x, y, C_KEY, b); y += 9;
  if (p->level >= 100) {
    ui_text(x, y, C_VAL, "At Lv100 (max)");
  } else {
    uint32_t nxt = pk_exp_for_level(gr, (uint8_t)(p->level + 1));
    uint32_t t100 = pk_exp_for_level(gr, 100);
    siprintf(b, "Lv%u +%lu", (unsigned)(p->level + 1), (unsigned long)(nxt > exp ? nxt - exp : 0));
    ui_text(x, y, C_VAL, b); y += 9;
    siprintf(b, "Lv100 +%lu", (unsigned long)(t100 > exp ? t100 - exp : 0));
    ui_text(x, y, UI_DIM, b);
  }
}

/* The reroll button, the two history arrows Guy asked for, and a note row saying where in
 * the history you are. Drawn dimmed in VIEW mode too, so the feature is discoverable. */
static void draw_reroll_row(const PkMon* p) {
  const int x = PDNA_SUM_CARD_X, y = PDNA_SUM_ROLL_Y, w = PDNA_SUM_CARD_W;
  bool armed = g_edit && !p->isBadEgg;
  if (armed) reg(F_SUM_REROLL, x, y, w);
  ui_panel(x - 2, y + PDNA_SUM_ROLL_BOX_DY, w + 2, PDNA_SUM_ROLL_BOX_H, UI_PANEL, UI_BORDER);
  int lw = (int)strlen(PDNA_SUM_ROLL_LBL) * UI_SYS8_W;
  ui_text(x + (w - lw) / 2, y, armed ? C_VAL : UI_DIM, PDNA_SUM_ROLL_LBL);
  /* Lit only when there is somewhere to step, so the arrows tell the truth about the ends
   * of the list rather than inviting a press that does nothing. */
  ui_text(x, y, (armed && g_ivh.cur > 0) ? C_HOT : UI_DIM, PDNA_SUM_ARROW_L);
  ui_text(x + w - UI_SYS8_W, y,
          (armed && g_ivh.cur + 1 < g_ivh.n) ? C_HOT : UI_DIM, PDNA_SUM_ARROW_R);
  char b[40];
  if (p->isBadEgg)       strcpy(b, PDNA_SUM_NOTE_BADEGG);
  else if (!g_edit)      strcpy(b, PDNA_SUM_NOTE_VIEW);
  else if (g_ivh.n <= 1) strcpy(b, PDNA_SUM_NOTE_IDLE);
  else {
    const char* tail = (g_ivh.cur == 0) ? PDNA_SUM_TAIL_ORIG
                     : (g_have_roll && g_ivh.cur + 1 == g_ivh.n && g_last_ivonly)
                       ? PDNA_SUM_TAIL_IVONLY : PDNA_SUM_TAIL_PIDIV;
    siprintf(b, PDNA_SUM_NOTE_FMT, g_ivh.cur + 1, g_ivh.n, tail);
  }
  ui_ptext_fit(x, PDNA_SUM_NOTE_Y, w, UI_DIM, b);
}

static void card_spread(const PkMon* p, bool ev) {
  int x = 98, y = 14; char b[48];
  ui_text(x, y, C_HDR, ev ? "EVs" : "IVs"); y += 12;
  int total = 0, maxv = ev ? 255 : 31;
  for (int i = 0; i < 6; i++) {
    int s = DISP[i], v = ev ? p->evs[s] : p->ivs[s];
    total += v;
    reg(ev ? F_EV0 + s : F_IV0 + s, x, y, 138);
    ui_text(x, y, C_KEY, DSHORT[i]);
    siprintf(b, "%3d", v); ui_text(x + 26, y, C_VAL, b);
    int fill = v * 84 / maxv;
    u16 col = (!ev && v == 31) ? UI_OK : (ev && v == 252) ? UI_OK : C_HDR;
    ui_progress(x + 54, y + 1, 84, 5, fill, col, UI_PANEL, UI_BORDER);
    y += 12;
  }
  y += 2;
  ui_text(x, y, C_KEY, "TOTAL");
  siprintf(b, "%d / %d", total, ev ? 510 : 186);
  ui_text(x + 44, y, total > (ev ? 510 : 186) ? UI_WARN : C_HOT, b);
  if (!ev) draw_reroll_row(p);   /* the IV spread gets the reroll; the EV card has no RNG
                                  * story to tell */
}

static void card_moves(const PkMon* p, bool contest) {
  int x = 98, y = 14; char b[48];
  int step = contest ? 18 : 22;          /* battle rows are taller to fit the type badge */
  ui_text(x, y, C_HDR, contest ? "CONTEST MOVES" : "BATTLE MOVES"); y += 12;
  for (int i = 0; i < 4; i++) {
    uint16_t mv = p->moves[i];
    reg(F_MV0 + i, x, y, contest ? 132 : 74);
    if (mv == 0) { ui_text(x, y, UI_DIM, "-"); y += step; continue; }
    ui_ptext_fit(x, y, contest ? 132 : 74, C_VAL, pk_move_name(mv));
    if (!contest) {
      /* The MAXIMUM is what a player wants to change, and the only thing that can change
       * it is the slot's PP Ups — so this cell edits F_PPU (max PP) and shows the Up
       * count that produced the maximum. Current PP stays editable on the field list's
       * own "PP n" row. */
      uint8_t maxpp = em_pp_max(mv, p->ppBonuses, i);
      uint8_t ups   = (uint8_t)((p->ppBonuses >> (i * 2)) & 3);
      reg(F_PPU0 + i, x + 76, y, 60);
      if (ups) siprintf(b, PDNA_SUM_PP_UPS_FMT, (unsigned)p->pp[i], (unsigned)maxpp, (unsigned)ups);
      else     siprintf(b, PDNA_SUM_PP_FMT,     (unsigned)p->pp[i], (unsigned)maxpp);
      ui_ptext_fit(x + PDNA_SUM_PP_X_DX, y, PDNA_SUM_PP_W, UI_DIM, b);
      type_badge(x + 4, y + 8, pk_move_type(mv));            /* real type badge under the name */
    } else {
      ui_text(x + 8, y + UI_ROW_H, C_HOT, pk_contest_name(pk_move_contest(mv)));
    }
    y += step;
  }
}

/* ORIGIN / MET card — the caught info: Poké Ball, met level, region + location, origin
 * game (all editable), plus read-only OT / TID / SID and a Pokérus tag.
 *
 * Region sits ABOVE Loc because that is the direction the two work in: picking a region
 * scopes the place list, and both open real lists (pick_region / pick_metloc) rather than
 * stepping through 256 ids. Its geometry is in pdna_layout.h — adding the row pushed the
 * OT/TID/SID tail down 11 px, and nothing was watching that tail. */
static void card_origin(const PkMon* p) {
  const int x = PDNA_SUM_CARD_X, vx = x + PDNA_SUM_ORG_VAL_DX;
  const int vw = PDNA_SUM_ORG_RIGHT - vx;
  int y = PDNA_SUM_ORG_Y0; char b[48];
  ui_text(x, y, C_HDR, "ORIGIN / MET"); y += PDNA_SUM_ORG_HDR_DY;

  ui_text(x, y, C_KEY, "Ball"); reg(F_BALL, vx, y, 76);
  ui_ptext_fit(vx, y, vw, C_VAL, pk_item_name(p->pokeball)); y += PDNA_SUM_ORG_ROW_H;

  ui_text(x, y, C_KEY, "Met Lv"); reg(F_METLEVEL, vx, y, 30);            /* shortened: was "Met at Lv" (overlapped value) */
  siprintf(b, "%u", (unsigned)p->metLevel); ui_text(vx, y, C_VAL, b);
  /* The number stays (F_METLEVEL edits it), but 0 gets its meaning next to it. 41 px of
   * proportional text at x+92 ends at 231, inside the 238 margin, and 2 px clear of the
   * F_METLEVEL selection frame which ends at 188. */
  if (p->metLevel == 0 && !p->isEgg) ui_ptext(x + 92, y, UI_OK, "hatched");
  y += PDNA_SUM_ORG_ROW_H;

  ui_text(x, y, C_KEY, PDNA_SUM_ORG_REGION_LBL); reg(F_METREGION, vx, y, 76);
  { int r = g3_region_of(p->metLocation);
    ui_ptext_fit(vx, y, vw, r < 0 ? UI_WARN : C_VAL, r < 0 ? "?" : g3_region_name(r)); }
  y += PDNA_SUM_ORG_ROW_H;

  ui_text(x, y, C_KEY, "Loc"); reg(F_METLOC, vx, y, 76);                 /* shortened: was "Location" (overlapped value) */
  ui_ptext_fit(vx, y, vw, C_VAL, pk_location_name(p->metLocation)); y += PDNA_SUM_ORG_ROW_H;

  ui_text(x, y, C_KEY, "Origin"); reg(F_METGAME, vx, y, 76);
  ui_text(vx, y, C_HOT, pk_metgame_name(p->metGame)); y += PDNA_SUM_ORG_GAP;

  siprintf(b, "OT %s", p->otName); ui_ptext_fit(x, y, 238 - x, UI_DIM, b); y += PDNA_SUM_ORG_TAIL_H;
  /* One row cannot hold both: "TID 38800 SID 15243" is 19 glyphs and the column is 17, so
   * truncating it dropped three of the five SID digits without saying so. */
  siprintf(b, "TID %05u", (unsigned)(p->otId & 0xFFFF));
  ui_text(x, y, UI_DIM, b); y += PDNA_SUM_ORG_TAIL_H;
  siprintf(b, "SID %05u", (unsigned)(p->otId >> 16));
  ui_text(x, y, UI_DIM, b); y += PDNA_SUM_ORG_TAIL_H;
  if (p->pokerus) ui_text(x, y, UI_WARN, "Pokerus");
}

/* CONTEST CONDITION card — cool/beauty/cute/smart/tough (raised by Pokéblocks) + sheen
 * (how "full" the mon is = how many Pokéblocks it's been fed). All editable (0..255). */
static void card_condition(const PkMon* p) {
  static const char* const CTL[6] = { "Cool", "Beauty", "Cute", "Smart", "Tough", "Sheen" };
  int x = 98, y = 14; char b[16];
  ui_text(x, y, C_HDR, "CONDITION"); y += 12;
  for (int i = 0; i < 6; i++) {
    int v = p->contest[i];
    reg(F_CT0 + i, x, y, 138);
    ui_text(x, y, C_KEY, CTL[i]);
    siprintf(b, "%3d", v); ui_text(x + 52, y, C_VAL, b);   /* +44 collided with "Beauty" */
    int fill = v * 64 / 255;
    ui_progress(x + 80, y + 1, 58, 5, fill, i == 5 ? C_HOT : C_HDR, UI_PANEL, UI_BORDER);
    y += 12;
  }
  y += 2;
  ui_text(x, y, UI_DIM, "Sheen: blocks fed");   /* 17 cols; the full phrase wrapped onto the portrait */
}

/* ---- the WHOLE CARD, made conditional -- and its selection frame, made movable -----
 *
 * THE BUG (Guy, 2026-08-23): "the scroll is still very slow, and when editing the numbers
 * it still rerenders the whole screen and not only the IV i edit for example which is very
 * slow, so i cant quickly go from 0 to 30, it takes patience" -- plus the move list on the
 * BATTLE MOVES card scrolling slowly. draw_left_conditional above fixed the info COLUMN
 * (the portrait fetch); the rest of summary_run's loop still repainted the card side from
 * scratch on every single keypress, and that side is where the remaining cost is:
 *
 *   summary_bg()      25,612 px = 51,224 B of framebuffer, every press.
 *   type_badge() x4   BATTLE MOVES draws one badge per move; in Guy's configuration
 *                     (artless build + his ROM registered on SD) each one is an
 *                     app_type_badge -> rom_type_sheet_load, i.e. a 5,888 B / 12-sector
 *                     read off the card, with no cache (pdna_main.c says why). Four of
 *                     them, per D-pad press, to move a cursor between two move slots that
 *                     did not change. The INFO card pays the same shape through
 *                     app_ability_desc (ROM text) plus two badges.
 *   portrait_sprite() once per iteration UNCONDITIONALLY -- another rom_portrait fetch
 *                     (up to 3 verified SD reads + an 8 KB LZ77 decode) -- even with the
 *                     summary animation switched OFF, in which case its result is never
 *                     read at all. Commit fcb8300 hoisted it out of the per-FRAME loop;
 *                     it stayed in the per-KEYPRESS one.
 *
 * WHAT THIS DOES. render_card()'s entire output is a pure function of the fields snapped
 * in CardPaint below -- the record itself (memcmp, for the same reason draw_left_conditional
 * memcmps: an EV edit can flip a GB-import verdict, so a hand-picked subset would go
 * stale), which card is up, the VIEW/EDIT/NEW mode, the portrait side, and the four
 * IV-history scalars draw_reroll_row prints. Identical snapshot => render_card would paint
 * exactly the pixels already on screen => skip it, and skip the portrait fetch with it
 * (nothing else in the loop decodes into mon_decomp, so the pointer stays live).
 *
 * THE CURSOR IS THE HARD PART, and it is the user's actual complaint. Moving the field
 * cursor changes ONE thing on screen: a 1 px UI_SELTEXT outline. Folding fsel into the
 * snapshot would be correct but would repaint the whole card -- badges and all -- on every
 * U/D press, which is exactly the slow move list. So the outline is SAVED AND RESTORED
 * instead: s_self_px keeps the 294 pixels that were underneath it, and moving it puts them
 * back before drawing the outline in its new place. Save-under is used rather than
 * "restore the gradient there", because the outline genuinely crosses card content -- on
 * BATTLE MOVES the next row's outline runs through the bottom scanlines of the previous
 * move's type badge, and on the IV/EV cards its right column crosses the stat bar -- and
 * repainting that content is precisely the SD traffic being avoided. 588 B of EWRAM buys
 * every cursor move for 1,176 B of framebuffer writes and ZERO card reads.
 *
 * COUNTED (from the geometry, not timed on hardware -- the SD path cannot be emulated,
 * so the clock on this belongs to Guy's log), per keypress, moving the field cursor one
 * slot on BATTLE MOVES with the mon unchanged:
 *   before   51,224 B (summary_bg) + 4,096 B (4 badge blits) + 588 B (outline)
 *            + ~118 glyph cells, AND ~51 SD sectors -- 4 type sheets at 12 plus the
 *            portrait fetch -- every press, held-key repeat included
 *   after     1,176 B: 294 px restored, 294 px drawn. 0 glyph cells. 0 SD sectors.
 *            (+ 1,920 B and <=30 cells on the presses where the footer HINT changes)
 *   = ~48x fewer framebuffer bytes and, the part that matters on the real card,
 *     51 -> 0 sectors of SD traffic per press.
 * Editing a value (LEFT/RIGHT on an IV) still repaints the card -- it must, the numbers
 * and their bars really changed -- but it no longer also re-fetches the portrait, and the
 * presses that change NOTHING (LEFT on an IV already at 0, a cancelled picker) now cost
 * nothing at all instead of a full repaint. */
typedef struct {
  PkMon    mon;
  uint32_t gen;                    /* ui_clear_gen() as of the paint                  */
  int16_t  card, ivh_cur, ivh_n;
  uint8_t  edit, create, back, have_roll, ivonly;
  bool     valid;
} CardPaint;

/* The selection outline's own pixels. m3_frame(x, y, x+w, y+h) paints four 1 px strips
 * with its right column at x+w-1 and its bottom row at y+h-1 -- (right,bottom) EXCLUSIVE,
 * the same convention m3_rect uses; that is not a guess, it was measured pixel-by-pixel
 * off a live capture (see ui_progress's comment in ui.c). self_strips() walks exactly
 * those pixels in a fixed order, so the set that is saved and the set that is painted are
 * the same set by construction -- there is no way for a frame pixel to be drawn but not
 * recorded, which is the only way this could leave ghost ink behind. */
#define SELF_MAX_W  (PDNA_SUM_CARD_W + 2)         /* the widest slot any card registers */
#define SELF_H      (UI_ROW_H + 1)
#define SELF_PX     (2 * SELF_MAX_W + 2 * (SELF_H - 2))     /* 294 px = 588 B */
static uint16_t EWRAM_BSS s_self_px[SELF_PX];
static int  s_self_x, s_self_y, s_self_w;
static bool s_self_on = false;      /* an outline is drawn AND s_self_px holds its under */
static bool s_self_toobig = false;  /* a slot too wide to save: fall back to full repaints */

#define SELF_SAVE  0
#define SELF_PUT   1
#define SELF_PAINT 2
static void self_strips(int x, int y, int w, int h, int mode, u16 col) {
  int side = h - 2, n = 2 * w + 2 * side;
  for (int i = 0; i < n; i++) {
    int px, py;
    if      (i < w)            { px = x + i;             py = y;         }
    else if (i < 2 * w)        { px = x + i - w;         py = y + h - 1; }
    else if (i < 2 * w + side) { px = x;                 py = y + 1 + (i - 2 * w); }
    else                       { px = x + w - 1;         py = y + 1 + (i - 2 * w - side); }
    if ((unsigned)px >= (unsigned)UI_SCR_W || (unsigned)py >= 160u) continue;
    u16* v = &vid_mem[py * 240 + px];        /* halfword writes only into VRAM */
    if      (mode == SELF_SAVE) s_self_px[i] = *v;
    else if (mode == SELF_PUT)  *v = s_self_px[i];
    else                        *v = col;
  }
}

/* The card was repainted over the outline AND over the pixels s_self_px was holding:
 * both are now meaningless. Never restore after this without a fresh save. */
static void sel_frame_drop(void) { s_self_on = false; }

/* Take the outline off the screen (leaving edit mode). */
static void sel_frame_hide(void) {
  if (!s_self_on) return;
  self_strips(s_self_x, s_self_y, s_self_w, SELF_H, SELF_PUT, 0);
  s_self_on = false;
}

/* Put the outline around the slot registered at (sx, sy, sw); a no-op if it is already
 * exactly there. */
static void sel_frame_set(int sx, int sy, int sw) {
  int x = sx - 2, y = sy - 1, w = sw + 2;
  if (w > SELF_MAX_W) {
    /* Unreachable today (no reg() in this file passes more than PDNA_SUM_CARD_W). If
     * someone adds a wider slot, degrade to the OLD behaviour -- a full card repaint every
     * iteration, which erases the outline for us -- rather than drawing an outline whose
     * under-pixels we cannot restore. */
    s_self_toobig = true;
    sel_frame_hide();
    m3_frame(sx - 2, sy - 1, sx + sw, sy + UI_ROW_H, UI_SELTEXT);
    return;
  }
  s_self_toobig = false;
  if (s_self_on && s_self_x == x && s_self_y == y && s_self_w == w) return;
  sel_frame_hide();
  self_strips(x, y, w, SELF_H, SELF_SAVE, 0);
  self_strips(x, y, w, SELF_H, SELF_PAINT, UI_SELTEXT);
  s_self_x = x; s_self_y = y; s_self_w = w; s_self_on = true;
}

static bool card_paint_needed(const CardPaint* v, const PkMon* p, int card) {
  return !v->valid || s_self_toobig
      || v->gen  != ui_clear_gen()          /* a picker/OSK/confirm painted over us */
      || v->card != (int16_t)card
      || v->edit != (uint8_t)g_edit || v->create != (uint8_t)g_create
      || v->back != (uint8_t)g_back
      || v->ivh_cur   != (int16_t)g_ivh.cur || v->ivh_n != (int16_t)g_ivh.n
      || v->have_roll != (uint8_t)g_have_roll
      || v->ivonly    != (uint8_t)g_last_ivonly
      || memcmp(p, &v->mon, sizeof *p) != 0;
}

static void card_paint_store(CardPaint* v, const PkMon* p, int card) {
  v->mon = *p;
  v->gen = ui_clear_gen();
  v->card = (int16_t)card;
  v->ivh_cur = (int16_t)g_ivh.cur; v->ivh_n = (int16_t)g_ivh.n;
  v->edit = (uint8_t)g_edit; v->create = (uint8_t)g_create; v->back = (uint8_t)g_back;
  v->have_roll = (uint8_t)g_have_roll; v->ivonly = (uint8_t)g_last_ivonly;
  v->valid = true;
}

static bool render_card(const PkMon* p, int card) {
  summary_bg();                             /* Emerald-style blue gradient backdrop */
  g_nslot = 0;
  if (g_edit)        { ui_fill_rect(0, 0, 50, 9, UI_WARN); ui_text(8, 1, UI_PANEL, "EDIT"); } /* unmissable */
  else if (g_create) { ui_fill_rect(0, 0, 50, 9, UI_OK);   ui_text(12, 1, UI_PANEL, "NEW"); } /* not saved yet */
  else               ui_text(4, 2, UI_DIM, "VIEW");
  draw_dots(150, 2, NCARDS, card);
  ui_hline(0, 10, UI_SCR_W, UI_BORDER);
  bool dl_ran = draw_left_conditional(p);
  ui_hline(98, 24, 100, UI_TITLE);          /* header accent rule under each card title */
  switch (card) {
    case 0: card_info(p);          break;
    case 1: card_skills(p);        break;
    case 2: card_spread(p, false); break;
    case 3: card_spread(p, true);  break;
    case 4: card_moves(p, false);  break;
    case 5: card_moves(p, true);   break;
    case 6: card_origin(p);        break;
    case 7: card_condition(p);     break;
  }
  return dl_ran;
}

static bool confirm_q(const char* title, const char* a_line, const char* b_line) {
  ui_clear();
  /* CREATE mode's confirm_keep()==false path `continue`s back into summary_run's loop
   * without changing mon or pose (pdna_summary.c's "B = carry on editing"), so the
   * draw_left_conditional snapshot above would otherwise think nothing changed and skip
   * redrawing the panel this ui_clear() just wiped. Every other caller of confirm_q()
   * unconditionally returns out of summary_run afterward, so this is a no-op there. */
  pd_summary_left_dirty();
  ui_panel(16, 44, 208, 60, UI_PANEL, UI_WARN);
  ui_text(28, 52, UI_TITLE, title);
  ui_text(28, 72, UI_TEXT, a_line);
  ui_text(28, 86, UI_DIM,  b_line);
  u16 k; do { s_vsync(); k = key_hit(KEY_A | KEY_B); } while (!k);
  bool yes = (k & KEY_A) != 0;
  if (yes) snd_ok(); else snd_back();
  return yes;
}

/* NOTE the A-line length. It used to read "A = write  (backup made first)": 30 glyphs
 * at sys8's fixed 8 px = 240 px drawn at x=28, i.e. 44 px past the panel's right edge
 * (16+208=224) and 28 px off the 240 px screen, so the tail wrapped to the far left as
 * loose "rst)". tests/host_textfit_test.c now pins every string on this panel. */
static bool confirm(void) {
  return confirm_q("Save changes?", "A = write (backup first)", "B = discard");
}

/* CREATE mode's keep/discard prompt. Deliberately worded so "discard" reads as
 * "the slot stays empty", not "your edits are lost". */
static bool confirm_keep(void) {
  return confirm_q("Keep this Pokemon?", "A = write (backup first)", "B = discard it");
}

static const char UNOWN_CH[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!?";

/* THE CONFIRM PANEL. For a caught Pokemon the IVs are not free: new IVs mean a new PID, and
 * the PID IS the nature, sex, ability slot, shininess and Unown letter. The search keeps
 * every one of them where it can (measured: never had to drop one over a 2,000-Pokemon
 * sweep); this panel is for the case it could not — a SHINY, where keeping the sparkle costs
 * ~8,192x more seeds than the budget can also spend on the nature — and for the one case
 * NOTHING can keep, an NPC trade, whose PID is a constant in the trade template rather than
 * RNG output. B cancels with NOTHING written and the history untouched. Never shown on the
 * PID-exempt path. Drawn at x=96 so the portrait at x=12..79 stays visible, which is what
 * its old comment claimed and its old geometry (x=16, w=208) did not do. */
static bool reroll_confirm(const IvRoll* r) {
  char ln[PDNA_SUM_RC_LINES][32];
  int n = 0;
  /* FIRST, because it is the only line that is about legality rather than looks — and the
   * only one no search could have avoided. */
  if (r->chg_trade && n < PDNA_SUM_RC_LINES)
    strcpy(ln[n++], PDNA_SUM_RC_TRADE);
  if (r->chg_shiny && n < PDNA_SUM_RC_LINES)
    strcpy(ln[n++], r->new_shiny ? PDNA_SUM_RC_SHINY_ON : PDNA_SUM_RC_SHINY_OFF);
  if (r->chg_nature && n < PDNA_SUM_RC_LINES)
    siprintf(ln[n++], PDNA_SUM_RC_NAT_FMT, pk_nature_name(r->old_nature),
                                           pk_nature_name(r->new_nature));
  if (r->chg_gender && n < PDNA_SUM_RC_LINES)
    siprintf(ln[n++], PDNA_SUM_RC_SEX_FMT, gender_sym(r->old_gender), gender_sym(r->new_gender));
  if (r->chg_ability && n < PDNA_SUM_RC_LINES)
    siprintf(ln[n++], PDNA_SUM_RC_ABI_FMT, (unsigned)(r->old_ability + 1),
                                           (unsigned)(r->new_ability + 1));
  if (r->chg_form && n < PDNA_SUM_RC_LINES)
    siprintf(ln[n++], PDNA_SUM_RC_UNO_FMT, UNOWN_CH[r->old_form % 28], UNOWN_CH[r->new_form % 28]);
  if (n == 0) return true;                        /* everything survived: no question */
  int h  = PDNA_SUM_RC_HEAD + n * PDNA_SUM_RC_ROW_H + PDNA_SUM_RC_FOOT;
  int tx = PDNA_SUM_RC_X + PDNA_SUM_RC_PAD;
  ui_panel(PDNA_SUM_RC_X, PDNA_SUM_RC_Y, PDNA_SUM_RC_W, h, UI_PANEL, UI_WARN);
  ui_text(tx, PDNA_SUM_RC_Y + 6, UI_TITLE, PDNA_SUM_RC_TITLE);
  /* PROPORTIONAL, not sys8: "Nat ADAMANT>ADAMANT" is 19 glyphs, and at sys8's fixed 8 px
   * cell that is 152 px against a 120 px text column — it would have lost the second
   * nature entirely, on the one line whose whole job is to name what changed.
   * tests/host_textfit_test.c pins every line on this panel at its worst case. */
  for (int i = 0; i < n; i++)
    ui_ptext_fit(tx, PDNA_SUM_RC_Y + PDNA_SUM_RC_HEAD + i * PDNA_SUM_RC_ROW_H,
                 PDNA_SUM_RC_TEXT_W, UI_WARN, ln[i]);
  ui_text(tx, PDNA_SUM_RC_Y + h - 14, UI_DIM, PDNA_SUM_RC_HINT);
  u16 k; do { s_vsync(); k = key_hit(KEY_A | KEY_B); } while (!k);
  if (k & KEY_A) { snd_ok(); return true; }
  snd_back();
  return false;
}

/* A on the reroll row. CHUNKED: 8,000 candidates a frame with an s_vsync() between slices,
 * so sound, rumble and the frame counter keep running. A monolithic search would block
 * s_vsync for ~0.5-1.0 s on a shiny — and s_vsync is the sole caller of snd_vblank(), which
 * is the sole caller of rmbl_vblank(), which is the only thing that ENDS a rumble cue, so
 * the motor would stay energised for the whole search on Guy's Omega. */
static bool do_reroll(EditMon* e, PkMon* cur) {
  if (cur->isBadEgg) { snd_back(); return false; }
  if (!g_roll_st.seed)
    g_roll_st.seed = ((uint32_t)s_ticks << 11) ^ ((uint32_t)REG_VCOUNT << 3)
                   ^ e->personality ^ e->otId ^ 0x9E3779B9u;
  g_roll_st.rung = 0; g_roll_st.started = 0;
  IvRoll r; int rc;
  ui_name_chip(PDNA_SUM_CARD_X, PDNA_SUM_NOTE_Y - 1, 60, 10, UI_WARN, 0x7FFF, PDNA_SUM_ROLLING);
  while ((rc = iv_roll_step(e, cur, &g_roll_st, 8000u, &r)) == 0) s_vsync();
  if (rc < 0) { snd_back(); return false; }
  if (r.pid_locked && !reroll_confirm(&r)) return false;   /* cancelled: nothing applied */
  iv_roll_apply(e, &r);
  em_preview(e, cur); pk_resolve(cur);
  ivh_push(&g_ivh, e);
  g_last_ivonly = !r.pid_locked; g_have_roll = true;
  return true;                     /* the A-press earcon at line 602 already fired */
}

static bool do_history(EditMon* e, PkMon* cur, int dir) {
  if (!ivh_step(&g_ivh, e, dir)) { snd_back(); return false; }
  em_preview(e, cur); pk_resolve(cur);
  g_have_roll = false;             /* the note now describes a roll we have stepped off */
  snd_edit();
  return true;
}

/* ---- portrait animation (Emerald-style entrance bounce + gentle idle bob) ----
 * A single 64x64 frame re-blitted at a vertical offset — no second asset, no extra
 * EWRAM. Plays a one-shot rise+overshoot on open / mon-change, then a slow idle
 * bob. Gated on app_anim_enabled() by the caller. */

/* The portrait sprite draw_left would pick, through the SAME era router — so the
 * animated portrait and the static one can never disagree about which generation's
 * art a mon wears. Also reports the source pixel size (*sw x *sh): Gen-3 art is 64x64,
 * a Game Boy pic is not, and the pose math below scales relative to it. */
static const uint16_t* portrait_sprite(const PkMon* p, bool* is_icon, int* sw, int* sh) {
  rumble_io_suspend();   /* portrait fetch decompresses from ROM */
  PdnaArt art;
  pdna_origin_art_portrait(p, g_back, &art, 0);
  rumble_io_resume();
  if (art.px) { *is_icon = false; *sw = art.w; *sh = art.h; return art.px; }
  *is_icon = true; *sw = MON_ICON_W; *sh = MON_ICON_H;
  return art.egg ? mon_icon_egg() : mon_icon_for_form(p->species, p->form);
}

/* Emerald-style intro: a squish-and-bounce of the single frame (vertical squash +
 * horizontal widen, anchored at the feet) that plays once, THEN a gentle idle
 * float. wx/sy are the drawn width/height in px (64 = natural); dy floats the rest
 * pose. This is the procedural affine intro the real Gen-3 summary uses (the front
 * "frames" are transforms of one sprite, not a flipbook). */
/* sine LUT scaled to +/-64 over a 16-step period */
static const signed char SIN16[16] = { 0, 24, 45, 59, 64, 59, 45, 24, 0, -24, -45, -59, -64, -59, -45, -24 };
static int isin(int i) { return SIN16[i & 15]; }

#define ENT 24                          /* entrance length (frames); after it, a gentle float */
/* The species' Emerald front-animation family as a procedural pose over frame t:
 * wx/sy = drawn width/height (64 = natural), dx/dy = pixel offset. Plays once, then
 * settles to a uniform idle float — matching the real game's intro-then-static. */
static void portrait_params(int fam, int t, int* wx, int* sy, int* dx, int* dy) {
  *wx = 64; *sy = 64; *dx = 0; *dy = 0;
  if (t >= ENT) { *dy = isin((t - ENT) >> 1) / 32; return; }      /* idle float ~ +/-2 px */
  int dk = (ENT - t) * 64 / ENT;                                   /* decay 64..~2 */
  switch (fam) {
    case 0: { int s = (isin(t * 2 + 12) * 12 * dk) >> 12; *sy = 64 + s; *wx = 64 - s / 2; } break;   /* squish & bounce */
    case 1: { int s = (isin(t * 2 + 12) * 14 * dk) >> 12; *sy = 64 - s; *wx = 64 + s / 2; } break;   /* stretch (tall first) */
    case 2: { *dy = (isin(t * 3) * 5 * dk) >> 12; } break;                                            /* v-shake */
    case 3: { *dx = (isin(t * 3) * 6 * dk) >> 12; } break;                                            /* h-shake */
    case 4: { int g = (dk * 18) >> 6; *wx = 64 - g; *sy = 64 - g; *dx = (isin(t * 3) * 3) >> 6; } break; /* grow + vibrate */
    case 5: { int s = (isin(t * 2) * 14 * dk) >> 12; *wx = 64 - s; *sy = 64 - s; } break;             /* shrink-grow pulse */
    case 6: { *dy = -((dk * 14) >> 6); } break;                                                       /* v-slide (down into place) */
    case 7: { *dx = -((dk * 16) >> 6); } break;                                                       /* h-slide in */
    case 8: { int h = (isin(t * 4) * 10 * dk) >> 12; *dy = -(h < 0 ? -h : h); } break;                /* jumps (hops up) */
    default:{ *dx = (isin(t * 2) * 8 * dk) >> 12; int s = (isin(t * 2 + 4) * 5 * dk) >> 12; *sy = 64 + s; } break; /* wobble (rotate approx) */
  }
}

/* Redraw the portrait at pose (wx,sy,dy) iff it changed. Composes each scanline of
 * the sprite sub-frame interior (x 12..79, y 14..77) — panel background plus the
 * 64x64 frame squashed to `sy` / widened to `wx`, anchored at the feet (y 78),
 * centred at x 46 — into a line buffer and writes it to VRAM in one pass (memcpy32).
 * No separate erase, so the animation never flickers.
 *
 * ALWAYS CPU transport (2026-08-29, PokeDNA B3 hardening pass -- corrected from a
 * first-pass `tick` parameter that mis-split the two call sites). This function has
 * TWO callers below: summary_run's first paint, and its idle wiggle loop. The first
 * paint was assumed one-shot/load-time and kept on dma3_cpy, but that is wrong: in
 * EDIT mode, holding LEFT/RIGHT to nudge a numeric field re-enters summary_run's outer
 * `for (;;)` at key_repeat rate (LEFT/RIGHT are in the repeat mask set at this
 * function's key_repeat_mask() call, `em_field_adjust` runs on `k`, not just `fresh`),
 * and each re-entry that changes `cur` makes card_paint_needed() true, which calls the
 * "first paint" site again -- so that call site can ALSO fire at key-repeat rate (~20
 * Hz), each call issuing 64 dma3_cpy (one per scanline, y 14..77), i.e. up to ~1,280
 * DMA3/s from the same fast-repeating-loop shape as the wiggle tick, not a rare
 * one-shot. Splitting on a "tick" flag was solving the wrong axis: the real fix is
 * simply to drop DMA from this function entirely. All four of this file's/the other 3
 * converted line buffers (s_pcol, s_dcline, s_ovl_line, and this one, s_pline) live in
 * IWRAM, so memcpy32 runs at DMA parity here -- there is no perf argument for keeping
 * either call site on DMA. See box_oam.c's swap_cache_slot / upload_tiles_cpu for the
 * same hardware finding this follows (2026-08-23 A/B, fd205bb): a per-vblank-or-faster
 * dma3_cpy is the proven risk, mechanism not pinned. */
static u16 __attribute__((aligned(4))) s_pline[68];
static void portrait_redraw(const PkMon* p, const uint16_t* spr, bool icon, int sw, int sh,
                            int wx, int sy, int dx, int dy, int* lastkey) {
  int key = (wx & 0xFF) | ((sy & 0xFF) << 8) | (((dx + 64) & 0xFF) << 16) | (((dy + 64) & 0xFF) << 24);
  if (key == *lastkey) return;
  *lastkey = key;
  /* spr/icon/sw/sh are supplied by the caller (fetched once per repaint) — see the
   * hoist comment in summary_run. This function must not fetch: it runs per frame. */
  /* Art-free build: no sprite exists at all. Without this check the loop below
   * sampled address 0 (open bus) and painted garbage stripes into the portrait —
   * paint the plain gradient instead (which also erases any previous pose). */
  int x0, baseline, top, iw, ih;
  if (!spr) { x0 = 0; baseline = -1; top = -1; iw = 0; ih = 1; }
  /* wx/sy come out of portrait_params in units where 64 == natural, so scaling them
   * by the source size makes a 56x56 Game Boy pic play the same pose at ITS natural
   * size instead of being stretched to 64. For 64x64 art this is the identity and the
   * arithmetic is literally what it was before. */
  else if (!icon) { iw = wx * sw / MON_FRONT_W; ih = sy * sh / MON_FRONT_H;
                    if (iw < 1) iw = 1;         /* never divide by zero at an extreme pose */
                    if (ih < 1) ih = 1;
                    x0 = 46 - iw / 2 + dx; baseline = 78 + dy; top = baseline - ih; }
  else       { x0 = 30 + dx; top = 30 + dy; if (top < 14) top = 14; if (top > 45) top = 45;
               baseline = top + 32; iw = 32; ih = 32; }
  /* The source column for a destination x depends only on x, x0, sw and iw — none of
   * which change down the frame — yet it used to be recomputed inside the innermost
   * loop. That is two software divisions per destination pixel (the ARM7TDMI has no
   * divide instruction, so each is a ~20-40 cycle __aeabi_idiv call) across 68x64
   * pixels: ~8,000 calls per pose change, and the pose changes nearly every frame
   * while the portrait is animating. Hoisting is enough — build the column table once
   * per redraw and lift the row term out of the x loop, and the count drops to 68 + 64.
   *
   * Deliberately NOT a fixed-point DDA: the expression below is character-for-character
   * the one that shipped, so the rendering cannot shift by a pixel at a rounding
   * boundary. Same pixels, ~60x fewer divisions. */
  int16_t sicol[68];
  if (spr && iw > 0) {
    for (int dx = 0; dx < 68; dx++) {
      int x = 12 + dx;
      int k = x - x0;
      sicol[dx] = (int16_t)(icon ? k : (k * sw / iw));
    }
  }
  for (int yy = 14; yy <= 77; yy++) {
    u16 bg = portrait_bg(yy);
    const uint16_t* srow = 0;
    if (spr && yy >= top && yy < baseline) {
      int sj = icon ? (yy - top) : ((yy - top) * sh / ih);
      srow = spr + sj * sw;
    }
    for (int dx = 0; dx < 68; dx++) {
      int x = 12 + dx; u16 c = bg;
      if (srow && x >= x0 && x < x0 + iw) {
        u16 px = srow[sicol[dx]];   /* icon: sw == 32, exactly the old expression */
        if (px & 0x8000) c = (u16)(px & 0x7FFF);
      }
      s_pline[dx] = c;
    }
    memcpy32(&vid_mem[yy * 240 + 12], s_pline, 68 * 2 / 4);
  }
  if (p->isShiny) ui_text(70, 16, C_HOT, "*");
  /* The loop above repaints the WHOLE frame interior, which erases anything draw_left
   * put there — the shiny star (restored just above) and the provenance stamp. Redraw
   * it here or the tag blinks out the moment the portrait animation starts. */
  PdnaOrigin org;
  pdna_origin_of(p, &org);
  if (org.verdict == PDNA_ORIGIN_GB)
    ui_name_chip(13, 15, 30, 11, pdna_origin_color(&org), 0x7FFF, pdna_origin_tag(&org));
}

/* Inline summary with two sub-modes:
 *   VIEW  (default): A enters EDIT; U/D scroll to the prev/next mon (real-PC style);
 *                    L/R flip card; B leaves. The save prompt appears HERE — only
 *                    when you try to leave or change mon with unsaved edits.
 *   EDIT  (after A): U/D pick a field, A opens its picker, <> nudge, L/R flip card,
 *                    B returns to VIEW (edits stay pending). The EDIT banner shows.
 * Returns 0 to exit, +1 for "next mon", -1 for "prev mon" (the caller loads it and
 * calls again). *saved is set true (and out_rec filled) if the user kept the edits. */
static int summary_run(uint8_t* rec, bool is_party, bool can_edit, uint8_t* out_rec,
                       bool* saved, int* card_io, bool create) {
  if (saved) *saved = false;
  EditMon e;
  gen3_edit_load(rec, is_party, &e);
  PkMon cur;
  em_preview(&e, &cur); pk_resolve(&cur);
  ivh_reset(&g_ivh, &e); g_have_roll = false;   /* the history belongs to THIS Pokemon */

  /* A ROLLUP, NOT A SPAN -- and the one screen in the app where that distinction is
   * load-bearing. Every other span is opened by a deliberate screen transition; this one
   * is driven by HELD INPUT. app_box_browse/party_browse (pdna_main.c) re-invoke
   * pdna_inspect per mon, and the +/-1 nav below fires on key_repeat, so holding DOWN
   * walks mon -> mon at repeat rate. One ~80 B span line per step against log.c's 192 KiB
   * per-run budget (LOG_RUN_MAX) is ~2,400 steps to the ceiling -- and that ceiling does
   * not drop the oldest half the way the 8 KiB ring does, it LATCHES: commit_bytes writes
   * "[log size cap reached - logging stopped]", sets s_capped, and every later flush
   * returns -2 forever, so the whole run's evidence after that instant -- later spans,
   * every rollup, any hang breadcrumb -- is gone. Rolled up, a whole hold-DOWN sweep is
   * ONE line that also reports the worst step, which is strictly more useful anyway.
   * Its own slot, not PERF_REP_PAGE: the box owns that slot ("box.load") while the
   * summary is up, and a differently-named begin would flush the box's rollup mid-visit.
   * Flushed at every return-0 exit below; a +/-1 nav return deliberately does NOT flush,
   * which is what makes the sweep accumulate into one line. */
  perf_rep_begin(PERF_REP_MON, "summary.open");
  bool perf_first_paint = true;
  int card = (card_io && *card_io >= 0 && *card_io < NCARDS) ? *card_io : 0;
  int fsel = 0;
  bool dirty = false, editing = false;
  int anim_t = 0, lastkey = -1;                /* portrait pose key (entrance anim + idle float) */
  /* What is on screen. A LOCAL, not a static: summary_run returns to its caller on every
   * mon step (the +/-1 nav below), so a per-call snapshot is exactly "one Pokemon's
   * worth" and a fresh visit always paints in full. 128 B, inside the ~256 B ceiling the
   * C guideline (Sec 0.2) sets for a stack local. */
  CardPaint pv;
  memset(&pv, 0, sizeof pv);
  const char* foot_drawn = 0;                  /* the footer hint currently printed */
  /* The portrait, fetched at most ONCE per real card repaint instead of once per keypress
   * -- see the CardPaint comment. p_spr points into mon_decomp, so it stays valid exactly
   * as long as nothing decodes into that buffer, and inside this loop the only thing that
   * does is render_card (draw_left's portrait + type_badge). */
  const uint16_t* p_spr = 0;
  bool p_icon = false, p_spr_ok = false;
  int  p_sw = MON_FRONT_W, p_sh = MON_FRONT_H;
  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
  /* g_back is file-static and survives between calls: a create opened after someone
   * flipped a previous mon to its back sprite would otherwise open on the back. */
  if (create) g_back = false;
  /* A fresh session must never trust draw_left_conditional's snapshot from whatever
   * screen (box, another mon's summary) was on-screen before this call. */
  pd_summary_left_dirty();
  sel_frame_drop();          /* likewise: whatever s_self_px held belonged to that screen */

  for (;;) {
    g_edit = editing;
    g_create = create && !editing;
    /* Keep the history honest: anything that moved the PID or the IV word behind its back —
     * a hand-edited IV row, a nature change on the INFO card (which re-rolls the PID), a
     * species change — rebases it, so the arrows and the "Roll k/n" counter can never
     * describe a state the record is not in. */
    ivh_sync(&g_ivh, &e);
    /* THE REPAINT GATE. Everything render_card draws is a pure function of the snapshot
     * card_paint_needed() compares; identical snapshot means identical pixels, which are
     * already on screen. g_nslot/g_slot survive a skip untouched, which is what lets the
     * cursor below still find its slots. */
    bool dl_ran = false, painted = false;
    if (card_paint_needed(&pv, &cur, card)) {
      dl_ran  = render_card(&cur, card);
      painted = true;
      card_paint_store(&pv, &cur, card);
      sel_frame_drop();     /* summary_bg just painted over the outline AND its under-pixels */
      p_spr_ok = false;     /* ...and type_badge/draw_left decoded into mon_decomp */
      ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    }
    if (editing && g_nslot) {
      if (fsel >= g_nslot) fsel = g_nslot - 1;
      sel_frame_set(g_slot[fsel].x, g_slot[fsel].y, g_slot[fsel].w);
    } else {
      sel_frame_hide();
    }
    const char* foot =
        (editing && g_nslot && g_slot[fsel].field == F_SUM_REROLL) ? PDNA_SUM_FOOT_REROLL
      : editing  ? (create ? PDNA_SUM_FOOT_CREATE_EDIT : PDNA_SUM_FOOT_EDIT)
      : create   ? PDNA_SUM_FOOT_CREATE
      : can_edit ? PDNA_SUM_FOOT_VIEW
                 : PDNA_SUM_FOOT_RO;
    /* The hints are string literals, so comparing the POINTER is comparing the text: two
     * that merged are the same text, and skipping a repaint of the same text is right.
     * The strip goes back to the gradient first -- the hints differ in length, and a
     * shorter one must not leave the tail of a longer one behind. */
    if (painted || foot != foot_drawn) {
      if (!painted)
        ui_fill_rect(0, PDNA_SUM_FOOTER_Y, UI_SCR_W, UI_ROW_H,
                     summary_bg_col(SUMMARY_BG_FOOT_BAND));
      ui_text(4, PDNA_SUM_FOOTER_Y, UI_DIM, foot);
      foot_drawn = foot;
    }
    /* Only true when render_card's draw_left_conditional actually redrew the rest pose
     * (64,64,0,0) -- draw_left_conditional's fix for Guy's "renders from scratch"
     * report (2026-08-23) skips draw_left when the mon+pose are unchanged, and on a
     * skip the screen is NOT at rest: it is still showing whatever pose the PREVIOUS
     * outer-loop iteration's animation left on screen. Resetting lastkey to the rest
     * sentinel on a skip made portrait_redraw's own diff check compare against a pose
     * that was never actually drawn -- caught by diffing mGBA screenshots against the
     * pre-fix build frame-for-frame (docs/analysis-2026-08-23/MEASUREMENTS.md), where
     * card flips and field-cursor moves occasionally froze the idle bob on a stale
     * frame instead of continuing it. */
    if (dl_ran) lastkey = 64 | (64 << 8) | (64 << 16) | (64 << 24);
    /* Fetch the portrait ONCE per repaint, not once per animation frame.
     *
     * portrait_redraw used to call portrait_sprite() itself, and portrait_sprite goes
     * through the era router into mon_front_for_form -> LZ77UnCompWram of a 64x64 RGB15
     * sprite (8 KB) into mon_decomp. The pose changes on almost every frame while the
     * portrait animates, so that 8 KB BIOS decompress — plus, in the Game Boy case, a
     * cartridge read — was running at up to 60 Hz to redraw pixels that had not changed.
     *
     * Safe to hoist here specifically: the only thing that runs between this point and
     * the redraws is portrait_redraw, and the pointer is mon_decomp, a buffer shared
     * with mon_back. Anything decoding into mon_decomp between the fetch and the last
     * redraw would hand out the wrong mon's pixels — render_card/draw_left do exactly
     * that, which is why the fetch sits AFTER them and inside the same repaint. If a
     * future caller (rom_sprite.h:78, rom_gbsprite.h:134 and rom_itemart.h:145 all
     * nominate mon_decomp as their staging buffer) starts decoding inside this loop, it
     * must re-fetch here. */
    bool anim = SUMMARY_ANIM && app_anim_enabled(ANIM_SUMMARY) && !cur.isEgg;
    /* Two conditions, both new. `anim` because with the animation off the fetched sprite
     * was never read -- the loop paid a rom_portrait every keypress to throw it away.
     * !p_spr_ok because a keypress that did not repaint the card did not decode anything
     * into mon_decomp either, so last iteration's pointer still addresses this mon's
     * pixels. Every path that CAN clobber the buffer clears p_spr_ok: render_card above,
     * and a picker/OSK (which ui_clear()s, so the next iteration repaints anyway). */
    if (anim && !p_spr_ok) { p_spr = portrait_sprite(&cur, &p_icon, &p_sw, &p_sh); p_spr_ok = true; }
    if (anim) { int fam = mon_anim_family(cur.species), wx, sy, dx, dy;
                              portrait_params(fam, anim_t, &wx, &sy, &dx, &dy); portrait_redraw(&cur, p_spr, p_icon, p_sw, p_sh, wx, sy, dx, dy, &lastkey); }  /* first paint -- memcpy32, see portrait_redraw's header comment */

    /* The card and the portrait are on screen; everything past here is the idle
     * portrait wiggle (pure CPU -- the sprite fetch is hoisted above on purpose) and
     * input. That is where "open the summary" ends. */
    if (perf_first_paint) { perf_first_paint = false; perf_rep_end(PERF_REP_MON); }
    u16 k, fresh;
    do { s_vsync();
         if (anim) { int fam = mon_anim_family(cur.species), wx, sy, dx, dy;
                                   portrait_params(fam, ++anim_t, &wx, &sy, &dx, &dy); portrait_redraw(&cur, p_spr, p_icon, p_sw, p_sh, wx, sy, dx, dy, &lastkey); }  /* idle wiggle tick -- memcpy32 */
         fresh = key_hit(KEY_FULL);
         k = fresh | key_repeat(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT); } while (!k);
    if      (fresh & (KEY_UP | KEY_DOWN)) snd_move();
    else if (fresh & (KEY_L | KEY_R | KEY_SELECT)) snd_tab();
    else if (fresh & (KEY_A | KEY_START)) snd_ok();
    else if (fresh & (KEY_LEFT | KEY_RIGHT)) { if (editing) snd_edit(); else snd_tab(); }
    else if (fresh & KEY_B)               snd_back();

    if (fresh & KEY_SELECT) {                                 /* SELECT changes the portrait */
      if (cur.species == 410) {                               /* Deoxys (internal 410): cycle the forme (view-only; version-baked in Gen 3) */
        pk_set_deoxys_form((pk_get_deoxys_form() + 1) % 4);
        cur.form = (uint8_t)pk_get_deoxys_form();
      } else g_back = !g_back;                                /* every other mon: flip front/back */
      continue;
    }

    /* ---- CREATE: START keeps the new mon, from EITHER sub-mode. Never gated on
     * `dirty` — the record IS the new thing, so a create with zero edits must still
     * be writable (gen3_edit_commit is lossless, so it reproduces gen3_build_mon). */
    if (create && (fresh & KEY_START)) {
      if (confirm_keep()) {
        gen3_edit_commit(&e, out_rec); if (saved) *saved = true;
        key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
        if (card_io) *card_io = card;
        g_create = 0;
        perf_rep_flush(PERF_REP_MON);
        return 0;
      }
      continue;                                    /* B in the prompt = carry on editing */
    }

    if (editing) {
      /* ---- EDIT MODE ---- */
      if (k & KEY_B) editing = false;              /* back to VIEW, keep pending edits */
      else if (k & (KEY_L | KEY_R)) { card = (card + (k & KEY_R ? 1 : NCARDS - 1)) % NCARDS; fsel = 0; }
      /* NOTE the LEFT/RIGHT branches test `fresh`, NOT `k`, on the reroll row: LEFT/RIGHT
       * are in key_repeat_mask above, so a held d-pad would otherwise scrub fifteen history
       * entries in a quarter of a second and machine-gun snd_back() at the end of them. */
      else if (g_nslot && (k & KEY_A)) {
        /* The reroll is the ONE action in this loop that paints without either changing the
         * record or calling ui_clear(): do_reroll drops a "ROLLING" chip on the card, and
         * reroll_confirm() opens a bordered panel over it. On the paths where nothing is
         * applied -- a bad egg, a failed search, B in the confirm -- the record comes back
         * byte-identical and ui_clear_gen() has not moved, so the gate above would leave
         * both sitting on screen with nothing able to heal them. Invalidate by hand. */
        if (g_slot[fsel].field == F_SUM_REROLL) { if (do_reroll(&e, &cur)) dirty = true;
                                                  pv.valid = false; }
        else { em_field_press(g_slot[fsel].field, &e, &cur); em_preview(&e, &cur); pk_resolve(&cur); dirty = true; }
      }
      else if (g_nslot && (k & KEY_LEFT)) {
        if (g_slot[fsel].field == F_SUM_REROLL) {
          if ((fresh & KEY_LEFT) && do_history(&e, &cur, -1)) dirty = true;
        } else { em_field_adjust(g_slot[fsel].field, -1, false, &e, &cur); em_preview(&e, &cur); pk_resolve(&cur); dirty = true; }
      }
      else if (g_nslot && (k & KEY_RIGHT)) {
        if (g_slot[fsel].field == F_SUM_REROLL) {
          if ((fresh & KEY_RIGHT) && do_history(&e, &cur, +1)) dirty = true;
        } else { em_field_adjust(g_slot[fsel].field, +1, false, &e, &cur); em_preview(&e, &cur); pk_resolve(&cur); dirty = true; }
      }
      else if (k & KEY_UP)   { if (g_nslot) fsel = (fsel > 0) ? fsel - 1 : g_nslot - 1; }
      else if (k & KEY_DOWN) { if (g_nslot) fsel = (fsel + 1) % g_nslot; }
    } else {
      /* ---- VIEW MODE ---- */
      if (k & KEY_A) { if (can_edit) editing = true; }
      else if ((k & (KEY_L | KEY_R)) || (fresh & (KEY_LEFT | KEY_RIGHT))) {   /* L/R shoulder OR d-pad LEFT/RIGHT flip cards */
        int fwd = (k & KEY_R) || (fresh & KEY_RIGHT);
        card = (card + (fwd ? 1 : NCARDS - 1)) % NCARDS; fsel = 0;
      }
      else if (create) {
        /* No prev/next mon exists to scroll to, so U/D do nothing; B is the other way
         * out and asks the same keep/discard question START does. */
        if (k & KEY_B) {
          bool keep = confirm_keep();
          if (keep) { gen3_edit_commit(&e, out_rec); if (saved) *saved = true; }
          key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
          if (card_io) *card_io = card;
          g_create = 0;
          perf_rep_flush(PERF_REP_MON);
          return 0;                                  /* discarded -> *saved stays false */
        }
      }
      else if (k & (KEY_UP | KEY_DOWN | KEY_B)) {    /* leaving this mon: prompt-save if dirty */
        if (dirty && confirm()) { gen3_edit_commit(&e, out_rec); if (saved) *saved = true; }
        key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
        if (card_io) *card_io = card;                /* keep the card sticky across mon-scroll */
        /* B ends the visit -> emit. UP/DOWN is a nav step: the caller re-enters this
         * function immediately, so the rollup stays open and the whole sweep lands as
         * one line when the user finally backs out. */
        if (k & KEY_B) { perf_rep_flush(PERF_REP_MON); return 0; }
        return (k & KEY_DOWN) ? +1 : -1;
      }
    }
  }
}

/* ---- public entry points -------------------------------------------------
 * Two names over one body, so the five existing call sites keep their exact
 * signature and CREATE mode is a flag rather than a forked screen. */
int pdna_inspect(uint8_t* rec, bool is_party, bool can_edit, uint8_t* out_rec,
                 bool* saved, int* card_io) {
  return summary_run(rec, is_party, can_edit, out_rec, saved, card_io, false);
}

int pdna_inspect_create(uint8_t* rec, uint8_t* out_rec, bool* saved, int* card) {
  /* A brand-new record is never a party mon and editing is always on: the caller
   * (app_create_mon) is already Omega-gated. */
  return summary_run(rec, false, true, out_rec, saved, card, true);
}
