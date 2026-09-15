/*
 * Gen-2's OWN Pack (item bag) / PC item store -- U5, BACKLOG #67. See
 * pdna_gbpack.h for the full ground-truth comment (box geometry, tile ids,
 * pocket order, the CANCEL row, the description-box/item-names/per-item-
 * pocket-membership deviations). Sibling of pdna_gbbag.c (Gen 1's own ITEM
 * screen, U4) -- same shell, same commit contract, different real-cartridge
 * geometry and pocket set.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_gbpack.h"
#include "gb_bag.h"
#include "gb_edit.h"        /* GB_GEN1/GB_GEN2                                        */
#include "gen2_save.h"      /* G2_VER_CRYSTAL                                          */
#include "pdna_gen12.h"     /* gb_rollback / gb_persist / gb12_arena_tail(_release)   */
#include "pdna_gbscreen.h"  /* the shared GB-screen shell                             */
#include "pdna_origin_art.h" /* PDNA_GEN2                                             */
#include "pdna_layout.h"    /* PDNA_GBSCR_ACT_*, PDNA_GBTR_ACT_*, GBTR_HEADER2_MAXW   */
#include "pdna_trainer.h"   /* num_entry / num_entry_opt                              */
#include "ui.h"
#include "snd.h"
#include "pdna_app.h"       /* msg_wait / app_confirm                                 */

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R)) snd_move();
  else if (k & KEY_A) snd_ok();
  else if (k & KEY_B) snd_back();
  return k;
}

/* Which real GbGame to pass gbb_insert()/gbb_remove()/gbb_set_qty()/gbb_tmhm_set() --
 * SAME resolution gb_bag.c's own gbb_read()/gbb_write() already do internally for a
 * Gen-2 session (s->g2w.sv.version), duplicated here because those four editing
 * calls take an explicit GbGame, not a GbSession*. Identical for GS/Crystal on every
 * fact this screen touches (gbb_max_item_id, pocket presence) -- resolved properly
 * anyway, not hardcoded, matching gb_bag.c's own comment posture. */
static GbGame g2pack_game(const GbSession* s) {
  if (s->gen == GB_GEN1) return GBF_G_RED;   /* never reached (Gen-2 only screen) */
  return (s->g2w.sv.version == G2_VER_CRYSTAL) ? GBF_G_CRYSTAL : GBF_G_GS;
}

/* D-Kris fix (review-opus ac9ffc0): a female Crystal save (Kris) still showed the
 * boy's (Chris's) pack picture -- 13/15 pic tiles differ on the real cartridge.
 * The gender byte g2_offsets() locates (G/S: 0, not stored, always false;
 * Crystal: file offset 0x3E3D, bit 0) is the SAME field the trainer card already
 * reads via gbt_read()'s own has_gender/gender pair, just fetched here without
 * pulling in the whole GbTrainer struct for one bit. BACKLOG #64 review Finding
 * (HIGH): read through gbs_read_field(), not s->img directly -- a streamed
 * read-only session (gb_session.h's own `img == NULL` invariant) has no resident
 * image to index into at all; gbs_read_field() already does the bounds check and
 * dispatches through the session's own `rd` callback when streamed. */
static bool g2pack_is_female(const GbSession* s) {
  G2Offsets o;
  if (!g2_offsets(s->g2w.sv.version, &o) || !o.player_gender) return false;
  uint8_t g = 0;
  if (gbs_read_field((GbSession*)s, o.player_gender, &g, 1) != GBS_OK) return false;
  return (g & 1u) != 0;
}

/* N3 (BACKLOG #111): the description box was always drawn with frame 0, ignoring
 * the player's own text-box-frame choice from the OPTIONS menu. wTextboxFrame is
 * wOptions + 2 on BOTH Gold/Silver and Crystal -- verified against the .sym files
 * (pokegold.sym: wOptions d199, wTextboxFrame d19b; pokecrystal.sym: wOptions cfcc,
 * wTextboxFrame cfce -- both +2) -- and gb_fields.c's own GBF_OPTIONS entry for
 * GBF_G_GS/GBF_G_CRYSTAL is `{0x2000, 8, GBFK_BYTES}`, so the frame byte is file
 * offset 0x2002 on both. engine/gfx/load_font.asm's LoadFrame does
 * `ld a, [wTextboxFrame] / maskbits NUM_FRAMES` (NUM_FRAMES = 8, so `and $07`) --
 * low 3 bits, values 0..7. The frames block itself is 9 frames x 6 tiles
 * (rom_gbui.h:73), so a masked value can never reach the block's own end, but the
 * clamp is kept anyway (frame > 8 -> 0) per the brief's own instruction, in case a
 * corrupt/foreign save has bits above the mask this reader is not applying. */
static int g2pack_frame(const GbSession* s, GbGame game) {
  uint32_t off = gbf_off(game, GBF_OPTIONS);
  if (!off) return 0;
  uint8_t byte = 0;
  GbSession* ncs = (GbSession*)(const void*)s;
  if (gbs_read_field(ncs, off + 2, &byte, 1) != GBS_OK) return 0;
  int frame = (int)(byte & 0x07u);
  if (frame > 8) frame = 0;
  return frame;
}

/* ============================================================================
 * ---- U5: Gold/Silver/Crystal's OWN Pack, on the shared shell (see
 * pdna_gbpack.h for the full ground-truth comment this layout is built from).
 * The pure painter (g2_tm_rebuild..gbpack_clamp_scroll, the desc box/pic
 * column/list painters) is pulled in as a shared body BEFORE its first use
 * below (gbpack_row_paint, part of the plain fallback, needs it too) -- see
 * pdna_gbpack_body.inc's own header for why this is a shared file, not a copy.
 * ============================================================================ */

#include "pdna_gbpack_body.inc"

/* ============================================================================
 * ---- the plain row-list fallback (BACKLOG #67's own "no dead end" rule) --
 * same shape as pdna_gbbag.c's own pdna_gbbag_plain(), over the four real
 * pockets (cycled LEFT/RIGHT) plus the PC store.
 * ============================================================================ */

static const char* const kPocketNames[5] = { "ITEMS", "BALLS", "KEY ITEMS", "TM/HM", "PC ITEM STORE" };
static const GbBagPocket kUiPocket[4] = { GBB_POCKET_ITEMS, GBB_POCKET_BALLS,
                                          GBB_POCKET_KEY, GBB_POCKET_TMHM };

static void gbpack_row_paint(const GbBag* bag, GbBagPocket pocket, int row, int y, bool sel) {
  int total = g2pack_row_total(bag, pocket);
  char lbl[16], val[16];
  if (row >= total - 1) {
    if (row == total - 1) siprintf(lbl, "CANCEL"); else lbl[0] = 0;
    val[0] = 0;
  } else {
    g2pack_row_label(bag, lbl, sizeof lbl, pocket, row);
    if (pocket == GBB_POCKET_KEY) siprintf(val, "-");
    else if (pocket == GBB_POCKET_TMHM) {
      int real = g2_tm_owned[row];
      if (g2pack_is_hm(real)) val[0] = 0;
      else { uint8_t c = 0; gbb_tmhm_get(bag, real, &c); siprintf(val, "x%u", (unsigned)c); }
    } else siprintf(val, "x%u", (unsigned)bag->pockets[pocket].entries[row].qty);
  }
  if (sel) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
  else     ui_fill_rect(2, y - 1, 236, 9, UI_BG);
  ui_text(6, y, sel ? UI_SELTEXT : UI_TEXT, lbl);
  char valp[24]; siprintf(valp, "%-8s", val);
  ui_text(6 + 12 * 8, y, sel ? UI_SELTEXT : UI_TEXT, valp);
}

__attribute__((noinline))
static bool pdna_gbpack_plain(GbBag* bag, GbGame game, bool can_edit, const char* header,
                              const char* header2) {
  int pcyc = 0;         /* 0..3 = kUiPocket cycle position; PC is a 5th state */
  bool in_pc = false;
  int sel = 0, top = 0;
  const int vis = 12;

  for (;;) {
    GbBagPocket pocket = in_pc ? GBB_POCKET_PC : kUiPocket[pcyc];
    int total = g2pack_row_total(bag, pocket);
    if (sel >= total) sel = total > 0 ? total - 1 : 0;
    if (sel < top) top = sel;
    if (sel >= top + vis) top = sel - vis + 1;
    if (top < 0) top = 0;

    ui_clear();
    int hline_y = header2 ? 20 : 11;
    int row_y0  = header2 ? 23 : 14;
    char title[24]; siprintf(title, "%s", header ? header : kPocketNames[in_pc ? 4 : pcyc]);
    ui_text(4, 2, UI_TITLE, title);
    if (header2) ui_ptext_fit(4, 11, GBTR_HEADER2_MAXW, UI_DIM, header2);
    ui_hline(0, hline_y, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < vis && top + i < total; i++)
      gbpack_row_paint(bag, pocket, top + i, row_y0 + i * 9, top + i == sel);
    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    trainer_key_legend(can_edit ? "A edit  L/R pocket  B save" : "L/R pocket  B back");

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_START);
    if (k & KEY_B) return true;
    if (k & KEY_START) { in_pc = !in_pc; sel = 0; top = 0; continue; }
    if (k & (KEY_LEFT | KEY_RIGHT)) {
      if (!in_pc) pcyc = (k & KEY_RIGHT) ? (pcyc + 1) % 4 : (pcyc + 3) % 4;
      sel = 0; top = 0; continue;
    }
    if (total > 0 && (k & KEY_UP))        sel = (sel > 0) ? sel - 1 : total - 1;
    else if (total > 0 && (k & KEY_DOWN)) sel = (sel + 1) % total;
    else if (can_edit && sel < total - 1 && (k & KEY_A)) {
      if (pocket == GBB_POCKET_KEY) { /* no qty to edit */ }
      else if (pocket == GBB_POCKET_TMHM) {
        /* D4/D5: `sel` is an owned-list POSITION here (row_total() just above
         * rebuilt g2_tm_owned for this pocket) -- map to the real tmhm_index.
         * HMs (D5) have no count to edit at all -- matches the real cartridge's
         * own "no x column" posture; A on an HM row is a no-op here. */
        int real = g2_tm_owned[sel];
        if (!g2pack_is_hm(real)) {
          uint8_t cur = 0; gbb_tmhm_get(bag, real, &cur);
          uint32_t q = num_entry("COUNT", cur, GBB_TMHM_CAP);
          gbb_tmhm_set(game, bag, real, (uint8_t)q);
        }
      } else {
        uint32_t q = num_entry("QUANTITY", bag->pockets[pocket].entries[sel].qty, GBB_QTY_CAP);
        if (q < 1) q = 1;
        gbb_set_qty(game, bag, pocket, sel, (uint8_t)q);
      }
    }
  }
}

/* START menu: ADD ITEM / REMOVE / SWAP / PC STORE toggle / CANCEL -- not
 * offered for the TM/HM pocket (a fixed 57-slot count array has no insert/
 * remove/swap notion, gb_bag.h's own contract); that pocket's START press
 * only offers the PC-store toggle. Same "wide OSK cap, clamp to 0xFF, let
 * gbb_insert's own refusal answer" posture as pdna_gbbag.c's own U4 fix (R1). */
__attribute__((noinline))
static int gbpack_start_menu(GbBag* bag, GbBagPocket pocket, GbGame game, int* sel, int* top,
                             int* swap_src_out, bool in_pc) {
  bool has_edit_ops = (pocket != GBB_POCKET_TMHM);
  const char* opts[5];
  int n = 0;
  if (has_edit_ops) { opts[n++] = "ADD ITEM"; opts[n++] = "REMOVE"; opts[n++] = "SWAP"; }
  opts[n++] = in_pc ? "PACK" : "PC STORE";
  opts[n++] = "CANCEL";
  int csel = 0;
  for (;;) {
    ui_clear();
    ui_text(4, 2, UI_TITLE, "PACK MENU");
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < n; i++) {
      if (i == csel) ui_panel(2, 14 + i * 9 - 1, 236, 9, UI_SEL, UI_TITLE);
      ui_text(6, 14 + i * 9, i == csel ? UI_SELTEXT : UI_TEXT, opts[i]);
    }
    trainer_key_legend("A choose  U/D  B cancel");
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return 0;
    if (k & KEY_UP)   csel = (csel > 0) ? csel - 1 : n - 1;
    if (k & KEY_DOWN) csel = (csel + 1) % n;
    if (!(k & KEY_A)) continue;

    if (csel == n - 1) return 0;                 /* CANCEL */
    if (csel == n - 2) return 2;                  /* PC/PACK toggle -- caller's job */

    const GbBagList* l = &bag->pockets[pocket];
    if (csel == 0) {   /* ADD ITEM */
      /* Per-item pocket membership (which pocket a given item id legally
       * belongs to) was NOT located this slice (time-boxed, per the brief's
       * own permission) -- fall back to the brief's own sanctioned rule:
       * ADD ITEM only works from the Items pocket; Balls/Key items refuse
       * outright (a real id typed here might genuinely belong to a DIFFERENT
       * pocket than the one open, and this core has no table to tell that
       * apart -- refusing is honest, silently accepting into the wrong
       * pocket would not be).
       *
       * D7 fix (review-opus ac9ffc0): the PC ITEM STORE is its own separate,
       * UNDIFFERENTIATED list, not one of the four real bag pockets with its
       * own membership rule -- the real cartridge lets you deposit ANY item
       * id into it (that is the store's whole job; gbb_insert()'s own id8
       * range check is what actually bounds a bad id, same as every other
       * pocket). Refusing ADD ITEM here was over-applying the Items-only
       * fallback to a pocket the fallback's own reasoning never covered. */
      if (pocket != GBB_POCKET_ITEMS && pocket != GBB_POCKET_PC) {
        /* D9 fix (review-opus ac9ffc0): this refusal is a known gap in THIS
         * core (no per-item pocket-membership table located yet), not a rule
         * of the real game -- say so, rather than let the message read as if
         * the cartridge itself refused. */
        msg_wait("WRONG POCKET", UI_WARN, "Add items from the Items pocket.",
                "No per-item pocket table yet.");
        continue;
      }
      uint32_t id, qty;
      if (!num_entry_opt("ITEM ID", 1, 999, &id)) continue;
      uint8_t id8 = (uint8_t)(id > 0xFFu ? 0xFFu : id);
      if (!num_entry_opt("QUANTITY", 1, 999, &qty)) continue;
      bool qty_in_range = qty >= 1u && qty <= GBB_QTY_CAP;
      uint8_t qty8 = (uint8_t)(qty > 0xFFu ? 0xFFu : qty);
      GbBagOpStatus st = gbb_insert(game, bag, pocket, id8, qty8);
      if (st == GBB_ERR_FULL)
        msg_wait("BAG FULL", UI_WARN, "This pocket has no free slot.", 0);
      else if (st == GBB_ERR_BADID)
        msg_wait("BAD ID", UI_WARN, "That item id does not exist.", 0);
      else if (st == GBB_ERR_QTY) {
        if (qty_in_range)
          msg_wait("SATURATED", UI_WARN, "Quantity clamped to the cap.", 0);
        else
          msg_wait("BAD QUANTITY", UI_WARN, "Quantity must be 1-99.", 0);
      }
      *sel = l->count > 0 ? l->count - 1 : 0;
    } else if (csel == 1) {   /* REMOVE */
      if (l->count > 0) gbb_remove(game, bag, pocket, *sel);
    } else if (csel == 2) {   /* SWAP -- arm, same pick-source-then-destination
                               * semantic as pdna_gbbag.c's own U4 D10. */
      if (l->count > 1 && *sel < l->count) {
        *swap_src_out = *sel;
        return 1;
      }
      return 0;
    }
    gbpack_clamp_scroll(g2pack_row_total(bag, pocket), sel, top);
    return 0;
  }
}

__attribute__((noinline))
static bool pdna_gbpack_gen2_screen(GbScreen* gs, GbBag* bag, GbGame game, bool can_edit,
                                    bool female, int frame) {
  static const char* const kLegendEdit[4] = {
    PDNA_GBTR_ACT_EDIT, PDNA_GBTR_ACT_SAVE, PDNA_GBSCR_ACT_SIZE, PDNA_GBTR_ACT_MORE
  };
  static const char* const kLegendView[4] = {
    0, PDNA_GBSCR_ACT_BACK, PDNA_GBSCR_ACT_SIZE, 0
  };
  gbscr_set_legend(gs, can_edit ? kLegendEdit : kLegendView);

  int cyc = 0;
  bool in_pc = false;
  int sel = 0, top = 0;
  bool want_commit = false;
  g2_pack_swap_active = false;

  GbBagPocket pocket = in_pc ? GBB_POCKET_PC : kUiPocket[cyc];
  g2pack_pic_column(gs, cyc, in_pc, female);
  g2pack_desc_box(gs, frame);
  g2pack_desc_label(gs, in_pc);
  g2pack_paint_list(gs, bag, pocket, top, sel);
  for (;;) {
    gbscr_flush(gs, 0);

    u16 k = 0;
    for (;;) {
      s_vsync();
      k = key_hit(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B |
                  KEY_SELECT | KEY_START);
      if (k) break;
    }
    if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) snd_move();
    else if (k & KEY_A) snd_ok();
    else if (k & KEY_B) snd_back();

    if (k & KEY_SELECT) { gbscr_toggle_scale(gs); continue; }
    if (k & KEY_B) {
      if (g2_pack_swap_active) {
        g2_pack_swap_active = false;
        g2pack_paint_list(gs, bag, pocket, top, sel);
        gbscr_mark_all_dirty(gs);
        continue;
      }
      want_commit = true; break;
    }

    int total = g2pack_row_total(bag, pocket);
    if (k & (KEY_LEFT | KEY_RIGHT)) {
      if (!in_pc) {
        cyc = (k & KEY_RIGHT) ? (cyc + 1) % 4 : (cyc + 3) % 4;
        pocket = kUiPocket[cyc];
        sel = 0; top = 0;
        g2_pack_swap_active = false;
        g2pack_pic_column(gs, cyc, in_pc, female);
        g2pack_paint_list(gs, bag, pocket, top, sel);
        gbscr_mark_all_dirty(gs);
      }
      continue;
    }
    if (k & KEY_START) {
      if (can_edit) {
        int swap_src = 0;
        g2_pack_swap_active = false;
        int r = gbpack_start_menu(bag, pocket, game, &sel, &top, &swap_src, in_pc);
        if (r == 1) { g2_pack_swap_active = true; g2_pack_swap_src = swap_src; }
        else if (r == 2) {
          in_pc = !in_pc;
          pocket = in_pc ? GBB_POCKET_PC : kUiPocket[cyc];
          sel = 0; top = 0;
          g2pack_pic_column(gs, cyc, in_pc, female);
          g2pack_desc_label(gs, in_pc);
        }
        g2pack_paint_list(gs, bag, pocket, top, sel);
        gbscr_mark_all_dirty(gs);
      }
      continue;
    }
    if (k & KEY_UP) {
      if (sel > 0) sel--;
      gbpack_clamp_scroll(total, &sel, &top);
      g2pack_paint_list(gs, bag, pocket, top, sel);
      gbscr_mark_all_dirty(gs);
    } else if (k & KEY_DOWN) {
      if (sel < total - 1) sel++;
      gbpack_clamp_scroll(total, &sel, &top);
      g2pack_paint_list(gs, bag, pocket, top, sel);
      gbscr_mark_all_dirty(gs);
    } else if (k & KEY_A) {
      if (g2_pack_swap_active) {
        int cnt = bag->pockets[pocket].count;
        if (sel < cnt && g2_pack_swap_src < cnt && sel != g2_pack_swap_src) {
          GbBagEntry tmp = bag->pockets[pocket].entries[sel];
          bag->pockets[pocket].entries[sel] = bag->pockets[pocket].entries[g2_pack_swap_src];
          bag->pockets[pocket].entries[g2_pack_swap_src] = tmp;
        }
        g2_pack_swap_active = false;
        g2pack_paint_list(gs, bag, pocket, top, sel);
        gbscr_mark_all_dirty(gs);
        continue;
      }
      if (sel == total - 1) { want_commit = true; break; }   /* CANCEL row */
      if (can_edit) {
        if (pocket == GBB_POCKET_KEY) {
          /* no quantity field to edit -- matches the real cartridge's own
           * "key items have no ×N" posture. */
        } else if (pocket == GBB_POCKET_TMHM) {
          /* D4/D5: `sel` is an owned-list POSITION (the `total` computed just
           * above this switch already rebuilt g2_tm_owned for this pocket).
           * HMs have no count to edit (D5) -- A is a no-op on an HM row. A
           * count set to 0 REMOVES the row (the real cartridge's own rule),
           * so the list can shrink under the cursor -- re-clamp afterward,
           * same as UP/DOWN already does, rather than leaving `sel`/`top`
           * pointing past the new (shorter) total. */
          int real = g2_tm_owned[sel];
          if (!g2pack_is_hm(real)) {
            uint8_t cur = 0; gbb_tmhm_get(bag, real, &cur);
            uint32_t q = num_entry("COUNT", cur, GBB_TMHM_CAP);
            gbb_tmhm_set(game, bag, real, (uint8_t)q);
            gbpack_clamp_scroll(g2pack_row_total(bag, pocket), &sel, &top);
          }
          g2pack_paint_list(gs, bag, pocket, top, sel);
          gbscr_mark_all_dirty(gs);
        } else {
          uint32_t q = num_entry("QUANTITY", bag->pockets[pocket].entries[sel].qty, GBB_QTY_CAP);
          if (q < 1) q = 1;
          gbb_set_qty(game, bag, pocket, sel, (uint8_t)q);
          g2pack_paint_list(gs, bag, pocket, top, sel);
          gbscr_mark_all_dirty(gs);
        }
      }
    }
  }

  return want_commit;
}

void pdna_gbpack(GbSession* s, bool can_edit) {
  if (!s || s->gen != GB_GEN2) {
    msg_wait("PACK", UI_WARN, "This screen is Gen-2 only.", 0);
    return;
  }

  uint32_t shell_need = gbscr_tail_need(PDNA_GEN2,
      GBSCR_NEED_TEXTBOX | GBSCR_NEED_PACKMENU | GBSCR_NEED_PACK, 0);
  uint32_t need = shell_need + 2u * (uint32_t)sizeof(GbBag);
  uint8_t* tail = gb12_arena_tail(need);
  if (!tail) {
    msg_wait("PACK", UI_WARN, "Not enough memory right now.", 0);
    return;
  }
  GbBag* bag = (GbBag*)(tail + shell_need);
  GbBag* t0  = (GbBag*)(tail + shell_need + sizeof(GbBag));
  if (!gbb_read(s, bag)) {
    gb12_arena_tail_release();
    msg_wait("PACK", UI_WARN, "Could not read this save.", 0);
    return;
  }
  memcpy(t0, bag, sizeof *t0);

  GbGame game = g2pack_game(s);
  bool female = g2pack_is_female(s);
  int frame = g2pack_frame(s, game);
  uint16_t pic_need = female ? GBSCR_NEED_PACK_F : GBSCR_NEED_PACK;
  /* BACKLOG #125 review: uint32_t for consistency with every other need_mask
   * local now that gbscr_open()/gbscr_tail_need()/gbscr_cache_plan() take
   * uint32_t -- this call site's own bits (PACKMENU/PACK/PACK_F/TEXTBOX) are
   * all < 16 today, but a uint16_t local here would silently re-truncate a
   * FUTURE need bit >= 16 the same way GBSCR_NEED_CARDCORNER just did. */
  uint32_t need_mask = GBSCR_NEED_TEXTBOX | GBSCR_NEED_PACKMENU | pic_need;
  GbScreen gs;
  const char* reason = 0;
  bool ok = gbscr_open(PDNA_GEN2, &gs, tail, shell_need, need_mask, 0, &reason);
  if (!ok && female) {
    /* Same fail-safe pdna_gbtrainer.c's own gen2 card uses: a save claims
     * female on a ROM whose pack_f rom_gbui somehow failed to locate should
     * not happen (rom_gbui.c ties it to the same anchor as pack_m) -- retry
     * once as Chris rather than refuse the whole screen over one picture.
     * BACKLOG #128 note: pdna_gbtrainer_gen2_card() folded its own equivalent
     * retry into a single opt_mask open; this screen's own retry is left
     * as-is here (out of #128's scope, argument-list-only touch) -- a future
     * lane may fold this one the same way. */
    female = false;
    need_mask = (need_mask & ~(uint16_t)GBSCR_NEED_PACK_F) | GBSCR_NEED_PACK;
    ok = gbscr_open(PDNA_GEN2, &gs, tail, shell_need, need_mask, 0, &reason);
  }
  bool want_commit;
  if (ok) {
    want_commit = pdna_gbpack_gen2_screen(&gs, bag, game, can_edit, female, frame);
    gbscr_close(&gs);
  } else {
    want_commit = pdna_gbpack_plain(bag, game, can_edit, PDNA_GBTR_FALLBACK_TITLE,
                                    reason ? reason : PDNA_GBSCR_REASON_UNAVAILABLE);
  }

  bool commit_ok = true;
  if (want_commit && memcmp(bag, t0, sizeof *t0) != 0) {
    if (app_confirm("Save pack changes?", "Writes the item edits now.")) {
      GbsStatus st = gbb_write(s, bag);
      if (st != GBS_OK) {
        gb_rollback();
        snd_error();
        msg_wait("EDIT REFUSED", UI_WARN, gbs_status_text(st), "Nothing was changed.");
        commit_ok = false;
      }
    } else {
      commit_ok = false;
    }
  } else if (want_commit) {
    snd_back();
    commit_ok = false;
  }
  gb12_arena_tail_release();

  if (want_commit && commit_ok) gb_persist("bag");
}
