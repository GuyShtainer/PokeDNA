/*
 * pdna_romfull.c — the full-image verifier screen. See pdna_romfull.h for why it exists
 * and what it is allowed to touch.
 *
 * THE THREE DESIGN DECISIONS WORTH ARGUING ABOUT
 *
 * 1. ONE REGION PER LOOP ITERATION, AND NO VBlankIntrWait. A 64 KiB region CRC costs a
 *    bit over one frame, so the loop repaints roughly 30-50 times a second by itself:
 *    the screen stays alive with no sub-region chunking machinery, and no state to carry
 *    across frames. Waiting for VBlank between regions would add up to a whole frame of
 *    idle per region and inflate the very number this screen exists to measure, so keys
 *    are polled directly (key_poll reads REG_KEYINPUT; it needs no VBlank) and cells are
 *    drawn mid-frame. A 12x6 pixel cell drawn during active display can tear for one
 *    frame. That is the entire cost.
 *
 * 2. THE MOTOR IS FROZEN AND THE MEASUREMENT SAYS WHICH TIMING IT RAN AT. A cart-GPIO
 *    write has been observed to corrupt an in-flight ROM read on this exact cart
 *    (source/rumble.c:59-66), so leaving the 8 kHz PWM ISR running would let this
 *    instrument manufacture its own BAD regions. And because the whole hang report turns
 *    on bus timing, the screen reports a measured rate at the live rung AND at the
 *    loader's own, from a 256 KiB calibration slab — two numbers instead of an argument.
 *
 * 3. A REGION WITH NOTHING VERIFIABLE IS DRAWN GREY, NEVER GREEN, AND WHICH REGIONS THOSE
 *    ARE IS DERIVED HERE, NOT BELIEVED. Regions made entirely of exclusion zone (the tail
 *    drop zone, the descriptor) are skipped — but the "which ones" comes from
 *    pdna_rv_region_excluded, which recomputes it from the zone list. Trusting the stamped
 *    bitmap was a hole big enough to drive the whole feature through: flipping ONE bit in
 *    it over a region with a real 4 KiB hole turned "IMAGE INCOMPLETE - 1 bad" into
 *    "IMAGE OK - 190 regions match, 2 skipped". A cell that passes because it compared
 *    nothing is a lie, and so is a verdict.
 *
 * 4. "NO FAULTS" IS NOT A PASS UNTIL SOMETHING WAS MEASURED. The verdict is gated on
 *    coverage: bytes_verified has to reach the bytes_verifiable that grid_init derives
 *    from the zone list, and at least one region has to have actually matched. Otherwise
 *    an image that skipped all 192 regions printed "IMAGE OK - 0 regions match, 192
 *    skipped" in green, under the sentence "Every region matched its build-time CRC32."
 *
 * 5. THE BLIND RANGE IS ON SCREEN, ALWAYS. This verdict leaves the console as a phone
 *    photo, so the photo has to carry its own caveat: the last 64 KiB is the loader's drop
 *    zone and holds every one of crt0's load images (the IWRAM .data copy, the
 *    EWRAM-resident flashcart driver). A green grid does not clear those 9 KB.
 */

#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_romfull.h"
#include "pdna_romver.h"
#include "flashcartio.h"
#include "fused_rom.h"
#include "log.h"
#include "rmbl.h"
#include "rumble.h"
#include "snd.h"
#include "ui.h"

extern const PdnaRomVerify g_pdna_romver;

/* ---- cell colours. Chosen to survive a phone photo of a GBA screen: the three that
 * matter (pass / image-fault / bus-fault) are green, red and yellow, and "not yet" is
 * dark enough that the scan front is obvious while it moves. ---- */
#define CLR_PEND     RGB15( 4,  6, 11)
#define CLR_CUR      RGB15(31, 31, 31)
#define CLR_OK       RGB15( 4, 26,  8)
/* A region only PARTLY covered by real bytes passes on less evidence than a full one, so
 * it does not get the full-strength green. Teal, because on a phone photo of a GBA screen
 * it is the one hue that cannot be mistaken for either the green or the grey. */
#define CLR_OK_PART  RGB15( 0, 22, 20)
#define CLR_RECOVER  RGB15(31, 20,  0)
#define CLR_UNSTABLE RGB15(31, 31,  0)
#define CLR_BAD      RGB15(31,  3,  3)
#define CLR_SKIP     RGB15(11, 12, 14)

#define GRID_X    8
#define GRID_Y   32
#define GRID_COLS 16
#define GRID_W   14      /* column pitch */
#define CELL_W   12
/* The grid may not draw at or below this row. 124 and not 128 because the verdict band
 * underneath needs FOUR lines, not three: the remedy sentence is the one line a
 * photograph has to carry to be actionable, and at three lines it ran off the right edge
 * (verified by screenshot, docs/analysis-2026-08-18/romfull-bad-verdict.png). */
#define GRID_BOT 124

/* Real bytes in region `i`, and the span it covers — the two numbers that decide whether a
 * passing cell gets the full green or the "partly compared" teal. */
static uint32_t region_span(const PdnaRomVerify* d, const PdnaRvGrid* g, int i) {
  uint32_t r0 = (uint32_t)i * g->region_bytes;
  if (r0 >= d->image_bytes) return 0u;
  return (d->image_bytes - r0 < g->region_bytes) ? (d->image_bytes - r0) : g->region_bytes;
}

static u16 state_color(int st) {
  switch (st) {
    case PDNA_RVG_OK:       return CLR_OK;
    case PDNA_RVG_RECOVER:  return CLR_RECOVER;
    case PDNA_RVG_UNSTABLE: return CLR_UNSTABLE;
    case PDNA_RVG_BAD:      return CLR_BAD;
    case PDNA_RVG_SKIP:     return CLR_SKIP;
    default:                return CLR_PEND;
  }
}

/* ---- the self-measurement ------------------------------------------------------
 * TIMER0 at F/1024 (16384 Hz) with TIMER1 cascaded gives a 32-bit tick counter good for
 * 72 hours, which is 10,000x longer than this screen can possibly run. TIMER2 belongs to
 * the rumble PWM and is deliberately left alone; it is frozen for the duration anyway. */
static void tmr_start(void) {
  REG_TM0CNT = 0; REG_TM1CNT = 0;
  REG_TM0D = 0;   REG_TM1D = 0;
  REG_TM1CNT = TM_ENABLE | TM_CASCADE;
  REG_TM0CNT = TM_ENABLE | TM_FREQ_1024;
}
static void tmr_stop(void) { REG_TM0CNT = 0; REG_TM1CNT = 0; }

static u32 tmr_ticks(void) {
  /* Read hi, lo, hi: if the high half moved between the two reads the low half we got
   * belongs to the wrong epoch, so take a fresh pair. One retry is enough — the halves
   * cannot both roll in the microseconds between two loads. */
  u16 hi = REG_TM1D, lo = REG_TM0D, hi2 = REG_TM1D;
  if (hi2 != hi) { hi = hi2; lo = REG_TM0D; }
  return ((u32)hi << 16) | lo;
}
static u32 tmr_ms(u32 ticks) {
  return (u32)(((unsigned long long)ticks * 1000ull) >> 14);   /* /16384, no division */
}

/* Time a raw CRC32 over `len` bytes at `off`, at the CURRENT bus timing. Canonicalisation
 * off: nothing is compared here, this is a stopwatch. */
static u32 time_slab(const uint32_t* tab, uint32_t off, uint32_t len) {
  u32 t0 = tmr_ticks();
  (void)pdna_rv_crc32(tab, (const unsigned char*)PDNA_RV_ROM_BASE + off, len);
  return tmr_ticks() - t0;
}

/* ---- drawing ------------------------------------------------------------------- */

static void draw_cell(int idx, int rows, int pitch, u16 clr) {
  int cx = GRID_X + (idx % GRID_COLS) * GRID_W;
  int cy = GRID_Y + (idx / GRID_COLS) * pitch;
  (void)rows;
  ui_fill_rect(cx, cy, CELL_W, pitch - 2, clr);
}

/* Byte counts with thousands separators: "12,539,180". A photo of "12539180" is a photo
 * of a number nobody can read back reliably, and this line is the deliverable. */
static void commas(char* out, uint32_t v) {
  char raw[12];
  int n, i, j = 0, k;
  siprintf(raw, "%lu", (unsigned long)v);
  n = (int)strlen(raw);
  for (i = 0; i < n; i++) {
    out[j++] = raw[i];
    k = n - 1 - i;
    if (k > 0 && (k % 3) == 0) out[j++] = ',';
  }
  out[j] = 0;
}

void pdna_romfull_screen(void) {
  /* ~350 bytes of stack, and not one byte of .bss in either RAM: this build crashes at
   * boot once ~1,232 bytes of new IWRAM .bss appear and EWRAM has ~1.5 KB left, so the
   * per-region state HAS to be a frame local. That is the reason this is a modal screen
   * and not a background task. */
  uint32_t     tab[16];
  PdnaRvGrid   g;
  const PdnaRomVerify* d = &g_pdna_romver;
  char  l1[72], l2[72], l3[72], num[16], num2[16];
  int   rows, pitch, i, aborted = 0, have_grid;
  u32   t_scan = 0, tk_fast = 0, tk_slow = 0, cal_bytes = 0;
  bool  paused = false;

  /* Same launder as pdna_romcheck_boot: keep any optimiser from pairing the verifier with
   * the near-empty initialiser in pdna_romver_data.c and folding these reads into
   * compile-time zeroes. Costs zero instructions. */
  __asm__ volatile("" : "+r"(d));

  pdna_rv_crc32_table(tab);
  have_grid = pdna_rv_grid_init(d, &g);

  ui_clear();
  ui_text(4, 2, UI_TITLE, "ROM IMAGE CHECK");
  ui_hline(0, 12, UI_SCR_W, UI_BORDER);

  if (!have_grid) {
    /* Degrade honestly. An artless build stamped by an older tool, an UNSTAMPED image, or
     * a descriptor whose own numbers disagree all land here — and the right answer is "no
     * opinion", never a red screen. */
    ui_ptext_wrap(8, 30, 224, 11, 3, UI_WARN,
                  "This image carries no region grid, so nothing was measured.");
    ui_ptext_wrap(8, 66, 224, 11, 4, UI_DIM,
                  "Either it was built without python3 (see 'rom self-check: UNSTAMPED' "
                  "in the log), or the descriptor is damaged. The 17-window boot check is "
                  "all this build has.");
    ui_ptext(8, 148, UI_DIM, "B  back");
    log_line("rom full check: no grid in this image (unstamped, v1, or bad descriptor)");
    snd_deny();
    while (1) { VBlankIntrWait(); snd_vblank(); key_poll(); if (key_hit(KEY_A | KEY_B)) break; }
    return;
  }

  rows  = (g.n_regions + GRID_COLS - 1) / GRID_COLS;
  pitch = (GRID_BOT - GRID_Y) / (rows > 0 ? rows : 1);
  if (pitch > 8) pitch = 8;
  if (pitch < 3) pitch = 3;

  /* Two lines, not one: the first cut put the legend on the end of the geometry line and
   * a screenshot showed it running off the right edge with "grey=skip" gone — and the
   * legend is the half a photograph needs, because a photo of a coloured grid with no key
   * is not evidence of anything. */
  commas(num, d->image_bytes);
  siprintf(l1, "%s B  %d x %lu KiB  wait %04x rung %d", num, (int)g.n_regions,
           (unsigned long)(g.region_bytes >> 10),
           *(volatile uint16_t*)0x04000204, flashcartio_bus_rung());
  ui_ptext_fit(6, 14, 232, UI_DIM, l1);
  /* The legend is chosen at RUNTIME by measured width, not by hoping: "part" is the entry
   * that made the string too long once, and a legend truncated to "grey=sk~" is worse than
   * a terse one. ui_ptext_w is the same measurement ui_ptext_fit would use to cut. */
  {
    const char* leg = "green=ok  teal=part  red=BAD  yellow=bus  grey=skip";
    if (ui_ptext_w(leg) > 232) leg = "grn=ok teal=part red=BAD ylw=bus gry=skip";
    ui_ptext_fit(6, 22, 232, UI_DIM, leg);
  }
  for (i = 0; i < g.n_regions; i++) draw_cell(i, rows, pitch, CLR_PEND);

  /* Freeze the motor for the whole scan. Gated on rumble_omega() so the boot-hold entry
   * (which runs BEFORE flashcartio_activate and rmbl_init) never writes cart GPIO on a
   * cart nobody has detected yet. */
  if (rumble_omega()) { rmbl_pause(); paused = true; }
  tmr_start();

  /* --- calibration: how fast is this bus, really, at both timings? --------------
   * A 256 KiB slab from the middle of the image, timed at the live rung and again at the
   * loader's own. Two measured numbers cost 0.5 MB of extra reading (~4% of the scan) and
   * settle an argument the whole hang investigation keeps having. */
  cal_bytes = (d->image_bytes > (1u << 20)) ? (256u * 1024u) : (16u * 1024u);
  {
    uint32_t coff = (d->image_bytes / 2u) & ~0xFFFFu;
    if (coff + cal_bytes > d->image_bytes) coff = 0x10000u;
    if (coff + cal_bytes <= d->image_bytes) {
      unsigned short prev;
      tk_fast = time_slab(tab, coff, cal_bytes);
      prev    = flashcartio_bus_transfer_enter();   /* == the loader's own timing */
      tk_slow = time_slab(tab, coff, cal_bytes);
      flashcartio_bus_transfer_leave(prev);
    } else {
      cal_bytes = 0;
    }
  }

  /* --- the scan ---------------------------------------------------------------- */
  {
    u32 t0 = tmr_ticks();
    for (i = 0; i < g.n_regions; i++) {
      int s, tries;
      draw_cell(i, rows, pitch, CLR_CUR);
      /* Full retry budget while the finding is still being established; one attempt once
       * a dozen regions have already come back stably wrong. The diagnosis is made by
       * then, and tripling the read cost of a wholesale-corrupt image would turn a 12
       * second answer into a 36 second one for no new information. */
      tries = (g.n_bad + g.n_unstable < 12) ? PDNA_RV_GRID_TRIES : 1;
      s = pdna_rv_region_check(d, (const unsigned char*)PDNA_RV_ROM_BASE, i, tab, tries, &g);
      {
        u16 clr = state_color(s);
        /* A pass over PART of a region is not the same evidence as a pass over all of it
         * (on this build region 190 is 46% zone), so it does not get the same colour. */
        if (s == PDNA_RVG_OK && pdna_rv_region_real(d, i) < region_span(d, &g, i))
          clr = CLR_OK_PART;
        draw_cell(i, rows, pitch, clr);
      }

      if ((i & 7) == 7 || i == g.n_regions - 1) {
        char hdr[24];
        siprintf(hdr, "%d/%d", (int)g.n_done, (int)g.n_regions);
        ui_fill_rect(160, 2, 76, 9, UI_BG);
        ui_ptext_right(236, 3, UI_TEXT, hdr);
      }
      key_poll();
      if (key_hit(KEY_B)) { aborted = 1; break; }
    }
    t_scan = tmr_ticks() - t0;
  }

  tmr_stop();
  if (paused) rmbl_resume();

  /* --- the verdict, written for a photograph -----------------------------------
   * Everything a hardware report needs is on screen at once, no scrolling and no second
   * page: how much was checked, how many regions failed, WHERE the first one is, how long
   * it took at which bus timing, and what to do about it. */
  {
    /* WHAT COUNTS AS A PASS is decided by pdna_rv_grid_verdict, in the pure core, because
     * the rule "no faults == pass" was wrong and this screen cannot be host-tested. It
     * also demands COVERAGE: at least one region matched, and the scan reached the byte
     * count grid_init derives from the zone list. Without that, an image whose every
     * region was skipped printed "IMAGE OK - 0 regions match, 192 skipped" in green.
     * tests/host_romgrid_test.c pins each branch. */
    int   vd    = pdna_rv_grid_verdict(&g, aborted);
    int   bad   = pdna_rv_grid_is_fault(vd);
    u16   ink   = bad ? CLR_BAD : (vd == PDNA_RVV_STOPPED ? UI_WARN : UI_OK);
    u32   ms    = tmr_ms(t_scan);
    u32   per   = (d->image_bytes >= 1000u)
                    ? g.bytes_verified / (d->image_bytes / 1000u) : 0u;
    /* On a fused image the descriptor's image_bytes is PokeDNA's own image, not the
     * cartridge: the appended game ROM was added after stamping and has no CRCs at all.
     * Say which denominator the percentage is against, or the line overstates the scan by
     * more than half. */
    uint32_t fsz = 0u;
    int   fused = fused_rom_present(&fsz);

    commas(num, g.bytes_verified);
    commas(num2, d->image_bytes);

    /* Header line: coverage, in bytes, with the percentage spelled out. This is the line
     * that makes a green grid quotable — "99.4% of the image" instead of "it said OK". */
    ui_fill_rect(0, 13, UI_SCR_W, 17, UI_BG);
    siprintf(l1, "%s of %s B checked (%lu.%01lu%%)%s", num, num2,
             (unsigned long)(per / 10u), (unsigned long)(per % 10u),
             fused ? " base only" : "");
    ui_ptext_fit(6, 14, 232, UI_TEXT, l1);

    if (cal_bytes)
      siprintf(l2, "scan %lu.%01lus  %luK: now %lums, loader %lums",
               (unsigned long)(ms / 1000u), (unsigned long)((ms % 1000u) / 100u),
               (unsigned long)(cal_bytes >> 10),
               (unsigned long)tmr_ms(tk_fast), (unsigned long)tmr_ms(tk_slow));
    else
      siprintf(l2, "scan %lu.%01lu s at waitcnt %04x", (unsigned long)(ms / 1000u),
               (unsigned long)((ms % 1000u) / 100u), *(volatile uint16_t*)0x04000204);
    ui_ptext_fit(6, 22, 232, UI_DIM, l2);

    /* Four rows at an 8 px pitch (128/136/144/152), the last ending on scanline 158.
     * Every one of them is clipped to the screen width — a truncated remedy is a
     * photograph nobody can act on. */
    ui_fill_rect(0, 124, UI_SCR_W, 36, UI_BG);
    ui_hline(0, 125, UI_SCR_W, UI_BORDER);

    /* One line per verdict, and every one of them says what was MEASURED. */
    switch (vd) {
      case PDNA_RVV_INCOMPLETE:
        siprintf(l1, "IMAGE INCOMPLETE - %d bad, %d unstable, %d skip",
                 (int)g.n_bad, (int)g.n_unstable, (int)g.n_skipped);
        break;
      case PDNA_RVV_DESC:
        siprintf(l1, "DESCRIPTOR DAMAGED - %d skip bit(s) disagree",
                 (int)g.excl_mismatch);
        break;
      case PDNA_RVV_STOPPED:
        siprintf(l1, "STOPPED at region %d of %d", (int)g.n_done, (int)g.n_regions);
        break;
      case PDNA_RVV_NOTHING:
        siprintf(l1, "NOTHING VERIFIED - 0 bytes in %d regions", (int)g.n_regions);
        break;
      case PDNA_RVV_SHORT:
        siprintf(l1, "COVERAGE SHORT - %lu of %lu KiB compared",
                 (unsigned long)(g.bytes_verified >> 10),
                 (unsigned long)(g.bytes_verifiable >> 10));
        break;
      default:
        if (g.n_partial)
          siprintf(l1, "IMAGE OK - %d match (%d partial), %d skipped",
                   (int)g.n_ok, (int)g.n_partial, (int)g.n_skipped);
        else
          siprintf(l1, "IMAGE OK - %d regions match, %d skipped",
                   (int)g.n_ok, (int)g.n_skipped);
        break;
    }
    ui_ptext_fit(6, 128, 232, ink, l1);

    if (g.first_bad >= 0) {
      siprintf(l2, "bad #1: region %d @ 0x%06lx (%lu.%02lu MB) %s",
               (int)g.first_bad, (unsigned long)g.first_bad_off,
               (unsigned long)(g.first_bad_off / 1000000u),
               (unsigned long)((g.first_bad_off / 10000u) % 100u),
               g.first_bad_state == PDNA_RVG_BAD ? "stable" : "unstable");
      ui_ptext_fit(6, 136, 232, UI_TEXT, l2);
    } else if (vd == PDNA_RVV_DESC) {
      ui_ptext_fit(6, 136, 232, UI_TEXT,
                   "Its bytes are suspect - no CRC covers them.");
    } else if (bad) {
      /* The all-skipped / short-coverage case. It used to land in the reassurance branch
       * below and print "Every region matched its build-time CRC32." with zero regions
       * compared, which is the exact lie this screen exists to prevent. */
      ui_ptext_fit(6, 136, 232, UI_TEXT,
                   vd == PDNA_RVV_NOTHING
                     ? "This scan proved nothing - 0 bytes read."
                     : "Fewer bytes compared than this image can prove.");
    } else {
      siprintf(l2, "%d region(s) needed a retry - the cart bus lied once",
               (int)g.n_recovered);
      /* Short enough to RENDER, not just to be true: at "...matched its build CRC32." the
       * proportional font clipped it to "...its build CRC~" (measured, screenshot
       * docs/analysis-2026-08-18/honest-ok.png). A caveat a photograph cuts in half is
       * not a caveat. */
      ui_ptext_fit(6, 136, 232, g.n_recovered ? UI_WARN : UI_DIM,
                   g.n_recovered ? l2 : "Every compared region matched its stamp.");
    }

    if (bad) {
      /* The remedy, in the order that costs least — and only remedies that EXIST on the
       * cart in front of the user: /PATCH and NOR are EZ-Flash Omega things, and printing
       * them to an EverDrive user is a wild goose chase. The boot-hold entry runs before
       * detection, so active_flashcart is NO_FLASHCART there and the generic line is the
       * honest one. A stale /PATCH entry and a bad SD copy are not distinguishable from
       * inside the cartridge (pdna_romver.h), so both are offered and the cheap one goes
       * first. ONE line, abbreviated: the two-line wrapped version was measured off-screen
       * at the bottom edge, and the log carries the full sentence anyway. */
      strcpy(l3, (active_flashcart == EZ_FLASH_OMEGA)
                   ? "Del /PATCH/*.pat, re-copy the .gba, or NOR"
                   : "Re-copy the .gba to the card, then re-check");
      ui_ptext_fit(6, 144, 232, UI_TEXT, l3);
    } else {
      ui_ptext_fit(6, 144, 232, UI_DIM, "B  back");
    }

    /* The caveat, on every verdict including the green ones: the last TAIL_GUARD bytes are
     * the loader's drop zone and are never compared, and on this build that range holds
     * every one of crt0's load images. A photo of a green grid has to carry this or it
     * over-claims. Derived from the zone that ends at EOF, so it stays true if the guard
     * ever changes. */
    {
      uint32_t tail0 = 0u;
      uint32_t k;
      for (k = 0; k < d->n_zones && k < PDNA_RV_MAX_ZONES; k++)
        if (d->z[k].off + d->z[k].len >= d->image_bytes) tail0 = d->z[k].off;
      /* "crt0 load images" is only true of the TAIL_GUARD-sized drop zone the stamper
       * emits. Claiming it for whatever zone happens to end at EOF is the same kind of
       * unearned assertion this whole pass is removing: an all-zone descriptor made the
       * line read "blind: 0x7f72a0..EOF (4078K) = crt0 load images", naming 4 MB of image
       * after 9 KB of crt0. Say the range always; name its contents only when the size
       * says it really is the guard. */
      if (tail0 && d->image_bytes - tail0 == PDNA_RV_TAIL_GUARD)
        siprintf(l3, "blind: 0x%06lx..EOF (%luK) = crt0 load images",
                 (unsigned long)tail0,
                 (unsigned long)((d->image_bytes - tail0) >> 10));
      else if (tail0)
        siprintf(l3, "blind: 0x%06lx..EOF plus %luK of other zones",
                 (unsigned long)tail0,
                 (unsigned long)((g.bytes_zoned - (d->image_bytes - tail0)) >> 10));
      else
        siprintf(l3, "blind: %lu KiB of zones were never compared",
                 (unsigned long)(g.bytes_zoned >> 10));
      ui_ptext_fit(6, 152, 232, UI_DIM, l3);
    }

    if (bad)            snd_error();
    else if (aborted)   snd_back();
    else                snd_save();

    /* Two to four lines on the card, so the verdict outlives the photo. No flush: the
     * caller owns the card, and at the boot-hold entry there is no mounted card yet. */
    log_line("rom full check: %s - %d ok (%d partial), %d bad, %d unstable, %d recovered, "
             "%d skip of %d regions x %lu KiB",
             vd == PDNA_RVV_INCOMPLETE ? "IMAGE INCOMPLETE" :
             vd == PDNA_RVV_DESC       ? "DESCRIPTOR DAMAGED" :
             vd == PDNA_RVV_STOPPED    ? "STOPPED" :
             vd == PDNA_RVV_NOTHING    ? "NOTHING VERIFIED" :
             vd == PDNA_RVV_SHORT      ? "COVERAGE SHORT" : "IMAGE OK",
             (int)g.n_ok, (int)g.n_partial, (int)g.n_bad, (int)g.n_unstable,
             (int)g.n_recovered, (int)g.n_skipped, (int)g.n_regions,
             (unsigned long)(g.region_bytes >> 10));
    log_line("  %lu of %lu verifiable B checked, %lu B blind, %lu.%01lu s at "
             "waitcnt=%04x rung=%d",
             (unsigned long)g.bytes_verified, (unsigned long)g.bytes_verifiable,
             (unsigned long)g.bytes_zoned,
             (unsigned long)(ms / 1000u), (unsigned long)((ms % 1000u) / 100u),
             *(volatile uint16_t*)0x04000204, flashcartio_bus_rung());
    if (g.excl_mismatch)
      log_line("  descriptor skip map disagrees with its own zones at %d region(s) - the "
               "zones win; the descriptor's bytes are themselves suspect",
               (int)g.excl_mismatch);
    if (fused)
      log_line("  fused image: %lu B of appended game ROM are NOT covered by any CRC",
               (unsigned long)fsz);
    if (cal_bytes)
      log_line("  %lu KiB CRC: %lu ms at this rung, %lu ms at the loader's timing",
               (unsigned long)(cal_bytes >> 10), (unsigned long)tmr_ms(tk_fast),
               (unsigned long)tmr_ms(tk_slow));
    if (g.first_bad >= 0)
      log_line("  first bad region %d @ file 0x%08lx: expected %08lx, read %08lx (%s)",
               (int)g.first_bad, (unsigned long)g.first_bad_off,
               (unsigned long)g.first_bad_exp, (unsigned long)g.first_bad_got,
               g.first_bad_state == PDNA_RVG_BAD ? "stable - the IMAGE differs"
                                                : "unstable - the cart BUS is lying");
    if (bad)
      log_line("  remedy: %s re-copy PokeDNA.gba to the card%s",
               active_flashcart == EZ_FLASH_OMEGA ? "delete /PATCH/*.pat, then" : "",
               active_flashcart == EZ_FLASH_OMEGA ? "; or run from NOR" : "");
    /* The caveat goes in the log too, because the log is what gets pasted into a report
     * while the photo is what gets waved around. */
    log_line("  blind by design: the last %lu B (loader drop zone) hold crt0's load "
             "images - a green grid does not clear them",
             (unsigned long)PDNA_RV_TAIL_GUARD);
  }

  while (1) {
    VBlankIntrWait();
    snd_vblank();
    key_poll();
    if (key_hit(KEY_A | KEY_B | KEY_START)) break;
  }
}
