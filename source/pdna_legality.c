/*
 * Legality report screen (Legality V2) — READ-ONLY analysis.
 *
 * V1 rendered a flat "n illegal, n warning(s)" list from gen3_legality.c. This is the
 * V2 report (gen3_legality2.h): a three-state verdict, findings grouped by category
 * with a per-severity colour, an on-demand PID/IV RNG check, and a sweep of all 30
 * cells of the box the Pokemon came from.
 *
 * Three things here are deliberate and are the whole point of the screen:
 *
 *  1. THE MIDDLE VERDICT IS NOT AN ACCUSATION. PokeDNA must never call a legitimate
 *     Pokemon illegal (docs/OVERNIGHT-DECISIONS.md §2). So the banner word for the
 *     middle grade is QUESTIONABLE, its subtitle says "worth a look", and the help
 *     page names the three populations that legitimately land there (events,
 *     Colosseum/XD, in-game trades). "ILLEGAL" is reserved for values the retail code
 *     cannot produce at all.
 *
 *  2. THE RNG CHECK IS ON DEMAND. pk2_hook_pidiv searches 65,536 LCRNG seeds; that is
 *     ~50 ms for ONE mon, so running it inside the 30-cell sweep would stall visibly
 *     (OVERNIGHT-DECISIONS.md §3). A runs it for the mon on screen only, behind a busy
 *     panel, because 50 ms is not instant either.
 *
 *  3. NOTHING HERE WRITES. No commit function is reachable from this file, no save
 *     block is mutated, and the footer says so on every frame. The help page makes
 *     that a promise to the user — keep it true.
 *
 * MEMORY. The screen keeps ONE Pk2Report (~1.4 KiB) on the IWRAM stack, which is the
 * shape gen3_legality2.h designs for ("returned by value on the caller's stack ...
 * same class as V1's PkLegality"). It is rebuilt from (mon, want_rng) whenever either
 * changes, so the sweep can borrow it as scratch instead of a second one existing.
 *
 * WHY NOT app_arena_acquire() FOR THE SWEEP. The arena IS g_pc — the reassembled PC
 * storage (pdna_app.h) — and g_pc is exactly the buffer the sweep is READING box
 * records out of. Borrowing it would hand the sweep its own input as scratch. The
 * sweep's actual working set is 30 x 4 bytes of per-cell summary, so it needs no
 * arena: it keeps 120 bytes on the stack and re-derives everything else on demand.
 */
#include "pdna_legality.h"
#include "gen3_legality2.h"
#include "gen3_pidiv.h"
#include "gen3_box.h"       /* pk_resolve, G3_IN_BOX */
#include "data_tables.h"    /* pk_species_name */

#include <tonc.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "snd.h"
#include "rmbl.h"

/* ui.h has no red: UI_WARN is orange and is already the SUSPECT colour, so INVALID
 * needs its own ink or the two severities would be indistinguishable. */
#define LEG_BAD      RGB15(31,  6,  6)
#define LEG_BG_BAD   RGB15( 9,  1,  1)
#define LEG_BG_SUS   RGB15( 9,  5,  0)
#define LEG_BG_OK    RGB15( 1,  7,  3)

#define BOX_SLOTS    G3_IN_BOX      /* 30 */
#define REC_BYTES    80

/* ---- pure helpers (no GBA state; the scratch host harness drives these) ---- */

/* The banner word. NOT pk2_grade_name(): that returns "LEGAL" for grade 0, which
 * overclaims for a checker whose own report says which check families are missing.
 * "OK" says "nothing I can test came out wrong", which is what we actually know. */
static const char* grade_word(uint8_t g) {
  return g == PK2_ILLEGAL ? "ILLEGAL" : g == PK2_QUESTIONABLE ? "QUESTIONABLE" : "OK";
}
static const char* grade_sub(uint8_t g) {
  return g == PK2_ILLEGAL      ? "Values the games cannot produce."
       : g == PK2_QUESTIONABLE ? "Worth a look, not an accusation."
                               : "Nothing unusual in what we check.";
}

/* Display list: category headers interleaved with their rows, then the notes that
 * are true of the report as a whole. Built fresh on every check run, so a category
 * that came out clean simply never appears. */
enum { D_HDR, D_ROW, D_NOTE };
enum { N_CLEAN, N_TRUNC, N_NOMOVES, N_NOENC, N_RNGHINT, N_COUNT };
typedef struct { uint8_t kind, a; } DispEnt;
#define DISP_MAX (PK2_MAX_ROWS + PK2_NCAT + N_COUNT)

static const char* note_text(int id) {
  switch (id) {
    /* The clean case still has to say what "clean" covers, or OK reads as a
     * guarantee the checker cannot give. */
    case N_CLEAN:   return "No impossible or odd values found.";
    case N_TRUNC:   return "More findings than fit; list cut off.";
    /* gen3_legality2.h: "The UI should say so rather than imply those checks
     * passed." A missing family is a gap in coverage, not a pass. */
    case N_NOMOVES: return "Move-source checks: not in this build.";
    case N_NOENC:   return "Encounter checks: not in this build.";
    case N_RNGHINT: return "Press A for the PID/IV RNG check.";
    default:        return "?";
  }
}

static int build_disp(const Pk2Report* R, bool rng_ran, DispEnt* d, int cap) {
  int n = 0;
  for (int c = 0; c < PK2_NCAT && n < cap; c++) {
    if (!R->cat_n[c]) continue;             /* category came out clean: no header */
    bool hdr = false;
    for (int i = 0; i < R->n && n < cap; i++) {
      if (R->row[i].cat != c) continue;
      if (!hdr) { d[n].kind = D_HDR; d[n].a = (uint8_t)c; n++; hdr = true; if (n >= cap) break; }
      d[n].kind = D_ROW; d[n].a = (uint8_t)i; n++;
    }
    /* cat_n counts rows that pk2_add dropped when the table filled up, so a
     * category can be non-zero with no surviving row. The N_TRUNC note covers it. */
  }
  if (R->n == 0 && n < cap)                          { d[n].kind = D_NOTE; d[n].a = N_CLEAN;   n++; }
  if (R->truncated && n < cap)                       { d[n].kind = D_NOTE; d[n].a = N_TRUNC;   n++; }
  if ((R->hooks_absent & PK2_HOOK_MOVES) && n < cap) { d[n].kind = D_NOTE; d[n].a = N_NOMOVES; n++; }
  if ((R->hooks_absent & PK2_HOOK_ENCOUNTER) && n < cap) { d[n].kind = D_NOTE; d[n].a = N_NOENC; n++; }
  if (!rng_ran && n < cap)                           { d[n].kind = D_NOTE; d[n].a = N_RNGHINT; n++; }
  return n;
}

/* One cell of the box sweep that raised something. 4 bytes; 30 of them = 120 B. */
typedef struct { uint8_t slot, grade, ninv, nsus; } SwHit;

/* Worst first: grade, then how many hard-invalid rows, then how many suspect rows,
 * then slot order so the list is stable and reads left-to-right within a tier. */
static int sweep_before(const SwHit* a, const SwHit* b) {
  if (a->grade != b->grade) return a->grade > b->grade;
  if (a->ninv  != b->ninv)  return a->ninv  > b->ninv;
  if (a->nsus  != b->nsus)  return a->nsus  > b->nsus;
  return a->slot < b->slot;
}
static void sweep_sort(SwHit* h, int n) {
  for (int i = 1; i < n; i++) {            /* insertion sort: n <= 30, always sorted */
    SwHit k = h[i];
    int j = i - 1;
    while (j >= 0 && sweep_before(&k, &h[j])) { h[j + 1] = h[j]; j--; }
    h[j + 1] = k;
  }
}

/* Same arithmetic as pk_box_slot (gen3_clip.h) but const-clean: this screen must not
 * be able to hand a writable pointer into the save to anything. */
static const uint8_t* box_rec(const uint8_t* block, int box, int slot) {
  return block + 0x0004 + ((uint32_t)box * BOX_SLOTS + (uint32_t)slot) * REC_BYTES;
}

/* ---- frame helpers --------------------------------------------------------- */

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16 s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_R | KEY_L)) snd_move();
  else if (k & KEY_A)     snd_ok();
  else if (k & KEY_B)     snd_back();
  else if (k & KEY_START) snd_tab();
  return k;
}

static u16 grade_ink(uint8_t g) {
  return g == PK2_ILLEGAL ? LEG_BAD : g == PK2_QUESTIONABLE ? UI_WARN : UI_OK;
}
static u16 grade_bg(uint8_t g) {
  return g == PK2_ILLEGAL ? LEG_BG_BAD : g == PK2_QUESTIONABLE ? LEG_BG_SUS : LEG_BG_OK;
}
static u16 sev_ink(uint8_t sev) {
  return sev == PK2_INVALID ? LEG_BAD : sev == PK2_SUSPECT ? UI_WARN : UI_DIM;
}

/* An egg's nickname field holds the Japanese egg name, which decodes to Latin
 * garbage — every other screen shows "EGG" for one, so match them. */
static const char* mon_label(const PkMon* m) {
  if (m->isEgg) return "EGG";
  if (m->nickname[0]) return m->nickname;
  return pk_species_name(m->species);
}

/* ---- running the checks ---------------------------------------------------- */

/* Rebuild the report. want_rng adds the PID/IV search.
 *
 * The PIDIV family ships as a weak no-op hook (gen3_legality2.c) that a separate
 * module may define strongly. When it is ABSENT the report would silently come back
 * with no RNG line at all, and the user would have pressed A for nothing — so run
 * gen3_pidiv.c ourselves and add the row. Either way the screen only ever renders
 * rows, so the two paths look identical on screen. */
static void run_checks(const PkMon* m, bool want_rng, Pk2Report* R) {
  pk_check_legality2_ex(m, R, want_rng ? PK2_RUN_PIDIV : 0);
  /* pidiv_ran (not the hooks_absent bit) is the signal, because there are TWO ways
   * to come back without an answer: the hook is a weak no-op, or the core
   * short-circuited before reaching it (bad egg / species out of range). The user
   * pressed a button either way and is owed a line. */
  if (!want_rng || R->pidiv_ran) return;

  /* Re-run the CORE with PK2_RUN_PIDIV rather than reimplementing the check here.
   * This used to be a private copy of the PIDIV logic, and it silently dropped the
   * in-game-trade exemption that gen3_legality_hooks.c added *because it was
   * measured*: four of the five mons across Guy's five saves that fail the RNG
   * search are NPC trades, whose PID is fixed in the trade template and has no
   * relation to any RNG stream. So the same Pokemon was judged differently depending
   * on whether you reached it through this button or through the box sweep. One
   * implementation, one answer — and the hook stays weak-linked, so a build without
   * gen3_legality_hooks.o simply leaves pidiv_ran clear and we say so. */
  pk_check_legality2_ex(m, R, PK2_RUN_PIDIV);
  if (!R->pidiv_ran) {
    pk2_add(R, PK2_CAT_PID, PK2_INFO, "RNG check unavailable in this build");
    R->pidiv_ran = 1;
    R->grade = R->n_invalid ? PK2_ILLEGAL : R->n_suspect ? PK2_QUESTIONABLE : PK2_LEGAL;
  }
}

/* ---- help ------------------------------------------------------------------ */

static int help_para(int y, u16 ink, const char* s) {
  return y + ui_ptext_wrap(6, y, 228, 9, 0, ink, s) * 9 + 5;
}

static void help_screen(void) {
  int page = 0;
  for (;;) {
    ui_clear();
    ui_text(4, 3, UI_TITLE, page ? "WHAT THIS SCREEN DOES" : "WHAT THE VERDICTS MEAN");
    ui_hline(0, 13, UI_SCR_W, UI_BORDER);
    int y = 18;
    if (page == 0) {
      ui_text(6, y, UI_OK, "OK"); y += 10;
      y = help_para(y, UI_TEXT,
          "Every check this build can run passed. It is not proof of anything: "
          "PokeDNA cannot see every rule of the games.");
      ui_text(6, y, UI_WARN, "QUESTIONABLE"); y += 10;
      y = help_para(y, UI_TEXT,
          "A value with no path we know of. Event Pokemon, Colosseum/XD Pokemon and "
          "in-game trades land here too, so read it as worth a look, not as an "
          "accusation.");
      ui_text(6, y, LEG_BAD, "ILLEGAL"); y += 10;
      y = help_para(y, UI_TEXT,
          "A value the retail games cannot produce at all, such as an EV total "
          "above 510.");
    } else {
      y = help_para(y, UI_TEXT,
          "This screen only READS. PokeDNA never edits, fixes or releases anything "
          "because of these checks.");
      y = help_para(y, UI_TEXT,
          "A runs the PID/IV RNG check on this Pokemon: it searches 65,536 seeds for "
          "one that makes both its PID and its IVs. Eggs, event and Colosseum/XD "
          "Pokemon are exempt, because no such link exists for them.");
      y = help_para(y, UI_TEXT,
          "R sweeps all 30 cells of this box with the fast checks only. Run the RNG "
          "check per Pokemon from here.");
    }
    ui_hline(0, 148, UI_SCR_W, UI_BORDER);
    ui_ptext(4, 151, UI_DIM, page ? "B back" : "A more   B back");
    u16 k = s_wait(KEY_A | KEY_B | KEY_LEFT | KEY_RIGHT);
    if (k & KEY_B) return;
    if (k & (KEY_A | KEY_RIGHT)) { if (page == 0) page = 1; else return; }
    if (k & KEY_LEFT) page = 0;
  }
}

/* ---- box sweep ------------------------------------------------------------- */

#define SW_VIS 10

static void sweep_frame(void) {
  ui_panel(24, 58, 192, 46, UI_PANEL, UI_TITLE);
  ui_text(32, 64, UI_TITLE, "BOX SWEEP");
  ui_ptext(32, 76, UI_DIM, "Fast checks, no RNG search.");
}

/* Returns the slot the user picked to open, or -1. `R` is borrowed as scratch — the
 * caller rebuilds it on return (see the memory note at the top of the file). */
static int sweep_screen(const uint8_t* block, int box, int here, Pk2Report* R) {
  SwHit hit[BOX_SLOTS];
  int nhit = 0;

  sweep_frame();
  for (int s = 0; s < BOX_SLOTS; s++) {
    PkMon mm;
    if (pk_decode_mon(box_rec(block, box, s), false, &mm)) {
      pk_resolve(&mm);
      /* Fast set only: no PIDIV in the sweep (OVERNIGHT-DECISIONS.md §3). */
      pk_check_legality2(&mm, R);
      /* INFO-only cells (e.g. "Event/fateful flag is set") are NOT listed: the list
       * exists to point at suspicion, and a box of event mons would bury it. */
      if (R->grade != PK2_LEGAL) {
        hit[nhit].slot  = (uint8_t)s;
        hit[nhit].grade = R->grade;
        hit[nhit].ninv  = R->n_invalid;
        hit[nhit].nsus  = R->n_suspect;
        nhit++;
      }
    }
    /* The whole sweep is a few milliseconds, so the bar would otherwise appear
     * already full. One vsync every 6 cells paces it to ~5 frames: visible progress,
     * no invented delay. */
    char l[32];
    siprintf(l, "Checking %d of %d...", s + 1, BOX_SLOTS);
    ui_fill_rect(32, 86, 176, 8, UI_PANEL);
    ui_ptext(32, 86, UI_TEXT, l);
    ui_progress(32, 96, 176, 5, (s + 1) * 176 / BOX_SLOTS, UI_TITLE, UI_PANEL, UI_BORDER);
    if (s % 6 == 5) s_vsync();
  }
  sweep_sort(hit, nhit);

  int sel = 0, top = 0;
  for (;;) {
    if (sel < top) top = sel;
    else if (sel >= top + SW_VIS) top = sel - SW_VIS + 1;

    ui_clear();
    ui_text(4, 3, UI_TITLE, "BOX SWEEP");
    char hdr[32];
    siprintf(hdr, "%d of %d flagged", nhit, BOX_SLOTS);
    ui_ptext_right(236, 4, nhit ? UI_WARN : UI_OK, hdr);
    ui_hline(0, 13, UI_SCR_W, UI_BORDER);

    if (!nhit) {
      ui_ptext(8, 30, UI_OK, "Nothing questionable in this box.");
      ui_ptext(8, 44, UI_DIM, "All 30 cells passed the fast checks.");
      ui_ptext(8, 58, UI_DIM, "The RNG check is per Pokemon (A).");
    } else {
      for (int i = 0; i < SW_VIS && top + i < nhit; i++) {
        const SwHit* h = &hit[top + i];
        int y = 20 + i * 12;
        bool s = (top + i == sel);
        if (s) ui_fill_rect(2, y - 2, 236, 12, UI_SEL);
        /* Fixed columns, each one width-CLAMPED, so a 10-glyph nickname can never
         * push the verdict word off the right edge:
         *   4 marker | 12 slot no. | 32..132 name | 138..216 verdict | 234 count */
        if (h->slot == here) ui_ptext(4, y, UI_DIM, ">");
        PkMon mm;
        char num[8];
        siprintf(num, "%2d", h->slot + 1);
        ui_text(12, y, s ? UI_SELTEXT : UI_DIM, num);
        if (pk_decode_mon(box_rec(block, box, h->slot), false, &mm)) {
          pk_resolve(&mm);
          ui_ptext_fit(32, y, 100, s ? UI_SELTEXT : UI_TEXT, mon_label(&mm));
        }
        char cnt[8];
        siprintf(cnt, "x%d", h->ninv + h->nsus);
        ui_ptext_right(234, y, grade_ink(h->grade), cnt);
        ui_ptext_fit(138, y, 78, grade_ink(h->grade), grade_word(h->grade));
      }
      if (nhit > SW_VIS) {
        int trk = SW_VIS * 12, bh = trk * SW_VIS / nhit;
        if (bh < 8) bh = 8;
        int by = 18 + (trk - bh) * top / (nhit - SW_VIS);
        ui_fill_rect(236, 18, 3, trk, UI_PANEL);
        ui_fill_rect(236, by, 3, bh,  UI_BORDER);
      }
    }

    /* Second footer row at y=152: the 5x7 cell is 8 rows, so anything below this
     * loses its descenders off the bottom of the 160 px screen. */
    ui_hline(0, 140, UI_SCR_W, UI_BORDER);
    ui_ptext(4, 143, UI_DIM, "Worst first. Only flagged cells listed.");
    ui_ptext(4, 152, UI_DIM, nhit ? "U/D   A open that Pokemon   B back" : "B back");

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return -1;
    if (!nhit) continue;
    if (k & KEY_UP)   sel = sel ? sel - 1 : nhit - 1;
    if (k & KEY_DOWN) sel = (sel + 1) % nhit;
    if (k & KEY_A)    return hit[sel].slot;
  }
}

/* ---- the report screen ----------------------------------------------------- */

#define VIS     11
#define BODY_Y  25
#define ROW_H   10

static void render(const PkMon* m, const Pk2Report* R, const DispEnt* d, int nd,
                   int top, bool has_box) {
  ui_clear();

  /* Banner. The grade word is fixed-width (an aligned label), the name and the
   * subtitle are prose, so they use the proportional face. */
  u16 ink = grade_ink(R->grade);
  ui_panel(0, 0, UI_SCR_W, 22, grade_bg(R->grade), ink);
  ui_text(4, 3, ink, grade_word(R->grade));
  const char* nm = mon_label(m);
  if (ui_ptext_w(nm) > 132) ui_ptext_fit(104, 4, 132, UI_TEXT, nm);
  else                      ui_ptext_right(236, 4, UI_TEXT, nm);
  ui_ptext_fit(4, 12, 232, UI_DIM, grade_sub(R->grade));

  for (int i = 0; i < VIS && top + i < nd; i++) {
    const DispEnt* e = &d[top + i];
    int y = BODY_Y + i * ROW_H;
    if (e->kind == D_HDR) {
      ui_text(4, y, UI_DIRCLR, pk2_cat_name(e->a));
      /* A rule from the end of the header to the right edge keeps the eye on the
       * group without spending a whole row on a divider. */
      int x = 4 + (int)strlen(pk2_cat_name(e->a)) * 8 + 4;
      ui_hline(x, y + 4, 232 - x, UI_BORDER);
    } else if (e->kind == D_ROW) {
      const Pk2Row* r = &R->row[e->a];
      ui_fill_rect(6, y + 1, 3, 6, sev_ink(r->sev));   /* severity chip */
      ui_ptext_fit(14, y, 218, r->sev == PK2_INFO ? UI_DIM : UI_TEXT, r->text);
    } else {
      ui_ptext_fit(14, y, 218, UI_DIM, note_text(e->a));
    }
  }
  if (nd > VIS) {
    int trk = VIS * ROW_H, bh = trk * VIS / nd;
    if (bh < 8) bh = 8;
    int by = BODY_Y + (trk - bh) * top / (nd - VIS);
    ui_fill_rect(236, BODY_Y, 3, trk, UI_PANEL);
    ui_fill_rect(236, by, 3, bh, UI_BORDER);
  }

  ui_hline(0, 138, UI_SCR_W, UI_BORDER);
  ui_ptext(4, 141, UI_DIM, "PokeDNA only reads here. Nothing is changed.");
  ui_ptext(4, 151, UI_DIM, has_box ? "U/D  A RNG  R sweep  START help  B back"
                                   : "U/D  A RNG  START help  B back");
}

int pdna_legality_show_box(const PkMon* m, const uint8_t* block, int box, int slot) {
  if (!m) return -1;
  const bool has_box = (block != NULL && box >= 0);

  PkMon cur = *m;              /* copy: the sweep can re-target this screen. `raw`
                                * keeps pointing into the caller's block, which
                                * outlives us in every call path. */
  int cur_slot = has_box ? slot : -1;
  Pk2Report R;                 /* ~1.4 KiB, one at a time — see the file header */
  DispEnt d[DISP_MAX];
  bool rng_ran = false;
  int top = 0;

  rmbl_fire(RCUE_ROOM);
  run_checks(&cur, false, &R);
  int nd = build_disp(&R, rng_ran, d, DISP_MAX);

  for (;;) {
    if (top > nd - VIS) top = nd - VIS;
    if (top < 0) top = 0;
    render(&cur, &R, d, nd, top, has_box);

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT |
                   KEY_A | KEY_B | KEY_R | KEY_START);
    if (k & KEY_B) break;
    else if (k & KEY_UP)    top--;
    else if (k & KEY_DOWN)  top++;
    else if (k & KEY_LEFT)  top -= VIS;
    else if (k & KEY_RIGHT) top += VIS;
    else if (k & KEY_START) { help_screen(); }
    else if (k & KEY_A) {
      /* ~50 ms of seed search: not a stall, but not instant either — say so. */
      ui_panel(40, 62, 160, 38, UI_PANEL, UI_TITLE);
      ui_text(48, 70, UI_TITLE, "RNG CHECK");
      ui_ptext(48, 84, UI_DIM, "Searching 65,536 seeds...");
      s_vsync();                       /* put the panel on screen before we stall */
      rng_ran = true;
      run_checks(&cur, true, &R);
      nd = build_disp(&R, rng_ran, d, DISP_MAX);
      top = 0;
    }
    else if ((k & KEY_R) && has_box) {
      int pick = sweep_screen(block, box, cur_slot, &R);   /* R is scratch in there */
      rmbl_fire(RCUE_ROOM);
      if (pick >= 0) {
        /* Decode into a TEMPORARY: pk_decode_mon memsets its output before it can
         * return false, so decoding straight into `cur` would blank the Pokemon on
         * screen if the slot ever came back empty. */
        PkMon nm;
        if (pk_decode_mon(box_rec(block, box, pick), false, &nm)) {
          pk_resolve(&nm);
          cur = nm;
          cur_slot = pick;
        }
      }
      rng_ran = false;                 /* a different Pokemon: its RNG is unchecked */
      run_checks(&cur, false, &R);     /* rebuild what the sweep overwrote */
      nd = build_disp(&R, rng_ran, d, DISP_MAX);
      top = 0;
    }
    else if (k & KEY_R) snd_deny();    /* no box context (party / bank single mon) */
  }
  return cur_slot;
}

void pdna_legality_show(const PkMon* m) { (void)pdna_legality_show_box(m, NULL, -1, -1); }
