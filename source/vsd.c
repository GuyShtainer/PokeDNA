/*
 * vsd -- the harness-hosted "virtual SD" mailbox. See vsd.h for the contract and
 * docs/briefs/s179-design.md S4.1-4.4 for the full protocol. BACKLOG #179 Phase A
 * step A1 (the spike): does the GBA-side spin/ack loop survive mGBA at all? Whole body
 * sits under #ifdef PDNA_DELTA (precedent: source/bank_plant.c, source/xfer_plant.c) --
 * no symbol from this file exists in either shipped gate build.
 */
#include "vsd.h"

#ifdef PDNA_DELTA

#include <tonc.h>          /* REG_VCOUNT */
#include "sys.h"           /* EWRAM_BSS */
#include "flashcartio.h"   /* active_flashcart, EZ_FLASH_OMEGA */

/* 32 B, exactly VsdBox -- the wire protocol the host reads/writes directly through
 * core.memory while the emulated CPU is stopped (S4.4: no barriers needed for that
 * reason). Lives in .sbss, not .bss, matching every other PDNA_DELTA-only static in
 * this tree (fused_gb.c's s_entry, bank_plant.c's s_xfer_buf). */
static VsdBox EWRAM_BSS s_vsd;

/* Latched false forever after a timeout (S4.3); vsd_xfer() checks this before ever
 * touching s_vsd so a dead harness costs one instruction, not a spin. */
static bool s_attached;

/* Step A1's own mutation target (S7.10): the design's claim is that INCREMENTING a
 * volatile counter inside the spin body is what stops the compiler from proving the
 * loop's exit condition (s_vsd.ack) can be hoisted/cached -- s_vsd.ack is ALREADY
 * volatile (VsdBox's field), so this counter is not what makes correctness true; it is
 * the thing step A1 removes (drop `volatile`, or drop the increment entirely) to watch
 * for a *different* failure mode: mGBA's own idle-loop fast-forward treating a loop
 * that touches no other memory as idle and skipping emulated time past the frame
 * boundary the host script needs to observe. Kept outside VsdBox on purpose: VsdBox is
 * the wire protocol the host parses by field offset; this has no meaning to the host. */
static volatile uint32_t s_spin_count;

/* Locator record -- same convention as fused_gb.c's g_pdna_gbd / fused_rom.c's
 * g_pdna_fuse: `const volatile` + `used` so it survives --gc-sections and a byte
 * scanner (no ELF, no symbol table) can find the mailbox's runtime address in the
 * raw .gba image. */
const volatile VsdRec g_pdna_vsd __attribute__((used, aligned(4))) = {
  { 'P', 'D', 'N', 'A', 'V', 'S', 'D', '1' }, (uint32_t)&s_vsd, (uint32_t)sizeof(VsdBox)
};

/* One VCOUNT wrap (a large value dropping to a small one, e.g. 227->0) counts as "one
 * frame elapsed" -- deterministic regardless of build speed (S4.3), unlike an
 * iteration-count bound which would depend on how fast the spin body runs. */
static int vsd_wrap_frame(uint16_t* last_vc) {
  uint16_t vc = REG_VCOUNT;
  int wrapped = (vc < *last_vc) ? 1 : 0;
  *last_vc = vc;
  return wrapped;
}

bool vsd_attach(void) {
  s_vsd.op     = VSD_OP_NONE;
  s_vsd.sector = 0;
  s_vsd.count  = 0;
  s_vsd.addr   = 0;
  s_vsd.ack    = 0;
  s_vsd.status = VSD_ST_BUSY;
  s_vsd.seq    = 1;         /* the doorbell: written after every other field above */
  s_vsd.magic  = VSD_MAGIC; /* written LAST of the handshake fields, first thing the
                              * host looks for -- the harness must not act on a
                              * mailbox whose seq/status it saw before magic landed */

  uint16_t last_vc = REG_VCOUNT;
  uint32_t frames = 0;
  while (s_vsd.ack != s_vsd.seq) {
    s_spin_count++;   /* the mutation target -- see the comment above */
    if (vsd_wrap_frame(&last_vc)) {
      frames++;
      if (frames >= 4) {
        s_attached = false;
        return false;
      }
    }
  }
  s_attached = true;
  active_flashcart = EZ_FLASH_OMEGA;
  return true;
}

bool vsd_attached(void) { return s_attached; }

bool vsd_xfer(uint32_t op, uint32_t sector, uint32_t addr, uint32_t count) {
  if (!s_attached) return false;
  if (op != VSD_OP_READ && op != VSD_OP_WRITE) return false;

  s_vsd.op     = op;
  s_vsd.sector = sector;
  s_vsd.count  = count;
  s_vsd.addr   = addr;
  s_vsd.status = VSD_ST_BUSY;
  s_vsd.seq    = s_vsd.seq + 1;   /* doorbell last (S4.4 step 1) */

  uint16_t last_vc = REG_VCOUNT;
  uint32_t frames = 0;
  while (s_vsd.ack != s_vsd.seq) {
    s_spin_count++;
    if (vsd_wrap_frame(&last_vc)) {
      frames++;
      if (frames >= 16) {
        s_attached = false;
        return false;
      }
    }
  }
  bool ok = (s_vsd.status == VSD_ST_OK);
  /* Mailbox goes idle once served -- also step A1's own proof-of-return signal: this
   * write happens ONLY on the loop's normal-exit path (the 16-frame timeout above
   * returns straight out without reaching here), so a harness watching `op` flip back
   * to VSD_OP_NONE knows the CPU-side spin loop actually noticed the ack and resumed,
   * independent of re-reading ack/seq (which stay stable either way). Costs no new
   * storage: reuses a field already inside the 32-byte mailbox. */
  s_vsd.op = VSD_OP_NONE;
  return ok;
}

#endif /* PDNA_DELTA */
