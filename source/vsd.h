#ifndef VSD_H
#define VSD_H

#include <stdint.h>
#include <stdbool.h>

/*
 * vsd -- the harness-hosted "virtual SD" mailbox, BACKLOG #179 Phase A step A1. Whole
 * body sits under #ifdef PDNA_DELTA, exactly like bank_plant.h/xfer_plant.h before it:
 * no symbol from this module exists in either shipped gate build (PDNA_DELTA is never
 * defined there -- `nm PokeDNA*.elf | grep -c vsd_` must read 0 in both).
 *
 * See docs/briefs/s179-design.md S4.1-4.4 for the full protocol. This lane (A1) is the
 * spike that proves the spin/ack loop survives mGBA at all -- it does NOT wire this
 * into flashcartio's real dispatch beyond the two documented hunks, and ships no image
 * factory, no burst mode, no Session hook. Those are later lane steps (A2+).
 */

#ifdef PDNA_DELTA

#define VSD_MAGIC    0x31445356u   /* 'VSD1', little-endian in memory */
#define VSD_OP_NONE  0u
#define VSD_OP_READ  1u
#define VSD_OP_WRITE 2u
#define VSD_ST_BUSY  0u
#define VSD_ST_OK    1u
#define VSD_ST_ERR   2u

/* 32 bytes, one field per line, `status` written before `ack` (S4.4 step by the host),
 * `seq` written LAST by the GBA side so the host never observes a half-formed request. */
typedef struct {
  volatile uint32_t magic;   /* +0  VSD_MAGIC, written once by vsd_attach()      */
  volatile uint32_t seq;     /* +4  ++ on every request; the doorbell            */
  volatile uint32_t op;      /* +8  VSD_OP_READ | VSD_OP_WRITE                   */
  volatile uint32_t sector;  /* +12 LBA                                         */
  volatile uint32_t count;   /* +16 sectors                                     */
  volatile uint32_t addr;    /* +20 GBA address of the caller's buffer          */
  volatile uint32_t status;  /* +24 host writes LAST of its own two             */
  volatile uint32_t ack;     /* +28 host echoes seq                             */
} VsdBox;

/* The locator record the harness finds without an ELF or a symbol table, same
 * convention as fused_gb.h's PdnaGbdRec / fused_rom.h's own record: a `const volatile`
 * global carrying a unique magic + the mailbox's address, scanned for in the raw
 * `.gba` bytes. 16 bytes, `used` survives --gc-sections (gba.specs enables it --
 * source/flashsave.c:56-62). */
typedef struct {
  char     magic[8];   /* "PDNAVSD1", no NUL padding needed (fixed-width) */
  uint32_t addr;        /* &s_vsd */
  uint32_t size;         /* sizeof(VsdBox), 32 -- lets a scanner sanity-check the hit */
} VsdRec;

extern const volatile VsdRec g_pdna_vsd;

/* Attach handshake (S4.3): writes magic/op/seq/status, then spins a BOUNDED 4 frames
 * (counted by REG_VCOUNT wraps, not iteration count) waiting for ack == seq. Returns
 * true and sets active_flashcart = EZ_FLASH_OMEGA on success; false (and latches
 * "never attached" forever) on timeout -- every later vsd_xfer() then returns false in
 * one instruction, so an unattended delta boots exactly as it does today. Call at most
 * once, from the PDNA_DELTA boot branch. */
bool vsd_attach(void);

/* True once vsd_attach() has succeeded; false forever after a timeout or before the
 * first call. Cheap to call from the flashcartio hunks on every transfer. */
bool vsd_attached(void);

/* One transaction: publish the request, ring the doorbell (seq last), spin on ack,
 * bail with false after 16 frames (S4.4 step 3) and latch vsd_attached() false so a
 * dead harness degrades rather than hangs. `op` is VSD_OP_READ/VSD_OP_WRITE, `addr` is
 * the GBA address of the caller's buffer (NOT copied into the mailbox itself -- the
 * host reads/writes it directly through core.memory while the CPU is stopped). */
bool vsd_xfer(uint32_t op, uint32_t sector, uint32_t addr, uint32_t count);

#else
typedef int vsd_no_empty_tu;
#endif /* PDNA_DELTA */

#endif /* VSD_H */
