/*
 * pdna_romcheck.c — cartridge glue for the sampled ROM self-check: run it, log it, show
 * it, and latch the tool read-only when the image is stably not the one we shipped.
 *
 * The measurement and the sampling rationale live in source/pdna_romver.{h,c} (pure C,
 * host-tested). This file is the only GBA-aware part, and it owns three policy decisions
 * that are deliberate and were argued rather than defaulted:
 *
 * 1. THE MESSAGE DOES NOT CLAIM TO KNOW THE CAUSE. A mismatch cannot distinguish "the
 *    copy on the card is bad" from "the loader patched this image at the PREVIOUS
 *    build's offsets". The second is real and cheap to hit: the Omega DE caches its
 *    patch scan in /PATCH/<name>.pat keyed on the ROM FILENAME, and Check_pat
 *    (reference/omega-de-kernel/source/GBApatch.c:720-767) re-validates that cache
 *    against only gl_reset_on / gl_rts_on / gl_sleep_on / gl_cheat_on — no size, no
 *    mtime, no hash. Rebuild PokeDNA.gba, keep the name, and the kernel happily applies
 *    the old iPatchInfo2[] and iTrimSize to the new image. So the panel says MODIFIED OR
 *    INCOMPLETE and offers BOTH remedies, in the order that costs least.
 *
 * 2. ONLY A STABLE MISMATCH LATCHES THE TOOL READ-ONLY. An unstable bus gets a loud log
 *    line and a warning panel, but keeps write mode: the answer to a lying bus is the
 *    timing ladder, not permanently amputating Guy's write features on a cart that may
 *    be fine. Same reasoning as icopy_verified (source/box_oam.c:110-121), which retries
 *    4x and fails soft.
 *
 * 3. THE VERDICT IS ONLY ACTED ON WHERE ITS REMEDY APPLIES. The EverDrive GBA X5 loader
 *    has not been analysed for image patching the way the Omega kernel has, so a
 *    mismatch there might be the loader, and "delete the /PATCH cache file" is meaningless on it.
 *    The measurement still runs and is still logged on every cart — only the panel and
 *    the read-only latch are gated. Note that pdna_romcheck_boot() runs BEFORE
 *    flashcartio_activate(), so the cart identity is not known until report time; that
 *    is why the gate lives here and not around the measurement.
 */

#include <tonc.h>
#include <stdio.h>

#include "pdna_romcheck.h"
#include "pdna_romver.h"
#include "flashcartio.h"   /* active_flashcart */
#include "log.h"
#include "snd.h"
#include "ui.h"

extern const PdnaRomVerify g_pdna_romver;

/* EIGHT BYTES of IWRAM .bss, and that is the whole RAM cost of this feature. This build
 * has a reproducible boot crash once ~1,232 bytes of new IWRAM .bss appear, and EWRAM has
 * ~1.5 KB left, so the full 56-byte PdnaRvResult stays a STACK local in pdna_romcheck_boot
 * and only what the panel and the write gate actually need survives the call. */
static struct {
  uint8_t ran;
  uint8_t verdict;      /* PdnaRvVerdict */
  uint8_t n_checked;
  uint8_t n_bad;
  char    tag[4];       /* the offending window's tag, or spaces */
} s_st;

/* True where the verdict's remedies make sense. See policy note 3 above. Written as
 * "not the EverDrive" rather than "is the Omega" on purpose: NO_FLASHCART is the
 * emulator/PDNA_DELTA case, where the panel is exactly what a tester needs to see when
 * they deliberately corrupt a window to prove the failure path works. */
static bool verdict_applies(void) { return active_flashcart != EVERDRIVE_GBA_X5; }

void pdna_romcheck_boot(void) {
  /* 64 bytes of CRC nibble table + a 56-byte result, both on the IWRAM stack: no .bss in
   * either RAM. A 256-entry byte table would be 1 KiB of frame sitting live underneath
   * the log_line() -> vsnprintf -> newlib _svfprintf_r calls below — which is precisely
   * the stack path the IWRAM boot cliff crashes in. 64 bytes is not. */
  uint32_t     tab[16];
  PdnaRvResult r;

  /* Launder the pointer so no optimiser — present or future, LTO or not — can pair the
   * verifier with the near-empty initialiser in pdna_romver_data.c and fold the reads
   * into compile-time zeroes. Zero instructions. */
  const PdnaRomVerify* d = &g_pdna_romver;
  __asm__ volatile("" : "+r"(d));

  pdna_rv_check(d, (const unsigned char*)PDNA_RV_ROM_BASE, PDNA_RV_ROM_BASE, tab, &r);

  s_st.ran       = 1u;
  s_st.verdict   = (uint8_t)r.verdict;
  s_st.n_checked = (uint8_t)(r.n_checked > 255 ? 255 : r.n_checked);
  s_st.n_bad     = (uint8_t)((r.n_bad + r.n_unstable) > 255 ? 255 : (r.n_bad + r.n_unstable));
  s_st.tag[0] = r.bad_tag[0]; s_st.tag[1] = r.bad_tag[1];
  s_st.tag[2] = r.bad_tag[2]; s_st.tag[3] = r.bad_tag[3];

  switch (r.verdict) {
    case PDNA_RV_OK:
      if (r.n_recovered)
        log_line("rom self-check: OK (%d windows, %d needed a RETRY - unstable rom reads)",
                 (int)r.n_checked, (int)r.n_recovered);
      else
        log_line("rom self-check: OK (%d windows)", (int)r.n_checked);
      break;
    case PDNA_RV_UNSTAMPED:
      log_line("rom self-check: UNSTAMPED - tools/stamp_rom_windows.py did not run");
      break;
    case PDNA_RV_NOWINDOWS:
      log_line("rom self-check: no windows in this build (artless?)");
      break;
    case PDNA_RV_BAD_DESC:
      log_line("rom self-check: DESCRIPTOR CORRUPT - image_bytes=%08lx - measured nothing",
               (unsigned long)d->image_bytes);
      break;
    default:
      log_line("rom self-check: %d BAD + %d UNSTABLE of %d windows -> %s",
               (int)r.n_bad, (int)r.n_unstable, (int)r.n_checked,
               r.verdict == PDNA_RV_BAD_BUS ? "CART BUS is lying" : "IMAGE differs from the file");
      log_line("  first %.4s [%s] @%08lx exp=%08lx got=%08lx last=%08lx tries=%d",
               r.bad_tag,
               (r.bad_flags & PDNA_RVW_K_TEXT)  ? "CODE"  :
               (r.bad_flags & PDNA_RVW_K_TABLE) ? "TABLE" : "art",
               (unsigned long)r.bad_addr,   (unsigned long)r.bad_expect,
               (unsigned long)r.bad_got,    (unsigned long)r.bad_got2, (int)r.bad_tries);
      log_line("  remedy: delete /PATCH/*.pat on the card, then re-copy PokeDNA.gba; or run from NOR");
      break;
  }
  if (r.n_skipped)
    log_line("rom self-check: %d window(s) failed a sanity clamp (descriptor damage?)",
             (int)r.n_skipped);
}

bool pdna_romcheck_bad(void) {
  return s_st.ran && verdict_applies() && s_st.verdict == PDNA_RV_BAD_IMAGE;
}

void pdna_romcheck_report(void) {
  bool bad_image, bad_bus, bad_desc;
  char det[48];

  if (!s_st.ran) return;
  bad_image = (s_st.verdict == PDNA_RV_BAD_IMAGE);
  bad_bus   = (s_st.verdict == PDNA_RV_BAD_BUS);
  bad_desc  = (s_st.verdict == PDNA_RV_BAD_DESC);
  if (!(bad_image || bad_bus || bad_desc)) return;

  if (!verdict_applies()) {
    log_line("rom self-check: verdict NOT applied - EverDrive loader is not analysed");
    return;
  }

  ui_clear();
  ui_panel(10, 20, 220, 126, UI_PANEL, UI_WARN);

  if (bad_bus) {
    ui_ptext_fit(18, 26, 204, UI_WARN, "CART READS UNSTABLE");
    ui_ptext_wrap(18, 42, 204, 11, 3, UI_TEXT,
                  "The cart gave different bytes for the same address. Reseat it, or run from NOR.");
    ui_ptext_wrap(18, 78, 204, 11, 3, UI_DIM,
                  "Editing stays ON: this is a bus fault, not a bad file.");
  } else if (bad_desc) {
    ui_ptext_fit(18, 26, 204, UI_WARN, "SELF-CHECK RECORD UNREADABLE");
    ui_ptext_wrap(18, 42, 204, 11, 3, UI_TEXT,
                  "This ROM's own self-check record is damaged. Re-copy PokeDNA.gba, or run from NOR.");
    ui_ptext_wrap(18, 78, 204, 11, 3, UI_DIM,
                  "Nothing was measured, so editing stays ON.");
  } else {
    ui_ptext_fit(18, 26, 204, UI_WARN, "ROM IMAGE MODIFIED OR INCOMPLETE");
    ui_ptext_wrap(18, 42, 204, 11, 3, UI_TEXT,
                  "Delete /PATCH/*.pat on the card, then re-copy PokeDNA.gba to it. Or run from NOR.");
    ui_ptext_wrap(18, 78, 204, 11, 3, UI_DIM,
                  "A stale /PATCH entry makes the loader patch this build at the last build's offsets.");
  }

  siprintf(det, "%d of %d windows bad, first %.4s",
           (int)s_st.n_bad, (int)s_st.n_checked, s_st.tag);
  ui_ptext_fit(18, 112, 204, UI_DIM, det);
  ui_ptext_fit(18, 123, 204, UI_DIM,
               bad_image ? "Editing OFF. See /PokeDNA/log.txt"
                         : "See /PokeDNA/log.txt");
  ui_text(18, 134, UI_DIM, "Press A");

  snd_error();
  while (1) {
    VBlankIntrWait();
    snd_vblank();
    key_poll();
    if (key_hit(KEY_A)) break;
  }
}
