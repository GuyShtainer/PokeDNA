/*
  io_ezfo.c
  Hardware Routines for reading the EZ Flash Omega filesystem
*/

#include "io_ezfo.h"
#include "../flashcartio.h"   /* flashcartio_read_retries/_failures: counted here, logged by the caller */

// SOURCE:
// https://github.com/ez-flash/omega-de-kernel/blob/main/source/Ezcard_OP.c

static void EWRAM_CODE delay(u32 R0) {
  int volatile i;

  for (i = R0; i; --i)
    ;
  return;
}
// --------------------------------------------------------------------
static void EWRAM_CODE SetSDControl(u16 control) {
  *(vu16*)0x9fe0000 = 0xd200;
  *(vu16*)0x8000000 = 0x1500;
  *(vu16*)0x8020000 = 0xd200;
  *(vu16*)0x8040000 = 0x1500;
  *(vu16*)0x9400000 = control;
  *(vu16*)0x9fc0000 = 0x1500;
}
// --------------------------------------------------------------------
static void EWRAM_CODE SD_Enable(void) {
  SetSDControl(1);
}
// --------------------------------------------------------------------
static void EWRAM_CODE SD_Read_state(void) {
  SetSDControl(3);
}
// --------------------------------------------------------------------
static void EWRAM_CODE SD_Disable(void) {
  SetSDControl(0);
}
// --------------------------------------------------------------------
static u16 EWRAM_CODE SD_Response(void) {
  return *(vu16*)0x9E00000;
}
// --------------------------------------------------------------------
static u32 EWRAM_CODE Wait_SD_Response() {
  vu16 res;
  u32 count = 0;
  while (1) {
    res = SD_Response();
    if (res != 0xEEE1) {
      return 0;
    }

    count++;
    if (count > 0x100000) {
      // DEBUG_printf("time out %x",res);
      // wait_btn();
      return 1;
    }
  }
}
// --------------------------------------------------------------------
static u32 EWRAM_CODE Read_SD_sectors(u32 address, u16 count, u8* SDbuffer) {
  SD_Enable();

  u16 i;
  u16 blocks;
  u32 res;
  for (i = 0; i < count; i += 4) {
    blocks = (count - i > 4) ? 4 : (count - i);

    /* The retry budget is PER CHUNK. Upstream declared it once outside this loop, so a
     * multi-chunk transfer shared two attempts across every chunk: one timeout early on
     * left the rest of the transfer with no retries at all. */
    u32 times = 3;

  read_again:
    *(vu16*)0x9fe0000 = 0xd200;
    *(vu16*)0x8000000 = 0x1500;
    *(vu16*)0x8020000 = 0xd200;
    *(vu16*)0x8040000 = 0x1500;
    *(vu16*)0x9600000 = ((address + i) & 0x0000FFFF);
    *(vu16*)0x9620000 = ((address + i) & 0xFFFF0000) >> 16;
    *(vu16*)0x9640000 = blocks;
    *(vu16*)0x9fc0000 = 0x1500;
    SD_Read_state();
    res = Wait_SD_Response();
    SD_Enable();
    if (res == 1) {
      flashcartio_read_retries++;    /* counted, not logged: log.c lives in unmapped ROM */
      if (--times) {
        delay(5000);
        goto read_again;
      }
      flashcartio_read_failures++;
      /* Retries exhausted. Upstream FELL THROUGH to the dmaCopy and returned 0 = success,
       * so the caller got the FPGA window's stale contents and FatFs reported FR_OK over
       * them: ff.c's ABORT(fs, FR_DISK_ERR) was dead code on this cart, and every layer
       * above — including the verified-write compare — was reasoning about bytes the card
       * never delivered. Fail instead: a loud error beats a silent wrong render, and on
       * the write path it beats silent data loss. */
      SD_Disable();
      return 1;
    }

    dmaCopy((void*)0x9E00000, SDbuffer + i * 512, blocks * 512);
  }
  SD_Disable();
  return 0;
}
// --------------------------------------------------------------------
static u32 EWRAM_CODE Write_SD_sectors(u32 address,
                                       u16 count,
                                       const u8* SDbuffer) {
  SD_Enable();
  SD_Read_state();
  u16 i;
  u16 blocks;
  u32 res;
  for (i = 0; i < count; i += 4) {
    blocks = (count - i > 4) ? 4 : (count - i);

    dmaCopy(SDbuffer + i * 512, (void*)0x9E00000, blocks * 512);
    *(vu16*)0x9fe0000 = 0xd200;
    *(vu16*)0x8000000 = 0x1500;
    *(vu16*)0x8020000 = 0xd200;
    *(vu16*)0x8040000 = 0x1500;
    *(vu16*)0x9600000 = ((address + i) & 0x0000FFFF);
    *(vu16*)0x9620000 = ((address + i) & 0xFFFF0000) >> 16;
    *(vu16*)0x9640000 = 0x8000 + blocks;
    *(vu16*)0x9fc0000 = 0x1500;

    res = Wait_SD_Response();
    if (res == 1)
      return 1;
  }
  delay(3000);
  SD_Disable();
  return 0;
}
// --------------------------------------------------------------------
static void EWRAM_CODE SetRompage(u16 page) {
  *(vu16*)0x9fe0000 = 0xd200;
  *(vu16*)0x8000000 = 0x1500;
  *(vu16*)0x8020000 = 0xd200;
  *(vu16*)0x8040000 = 0x1500;
  *(vu16*)0x9880000 = page;  // C4
  *(vu16*)0x9fc0000 = 0x1500;
}
// --------------------------------------------------------------------

#define ROMPAGE_BOOTLOADER 0x8000
#define ROMPAGE_PSRAM 0x200
#define S98WS512PE0_FLASH_PAGE_MAX 0x200
#define ROM_HEADER_CHECKSUM *(vu16*)(0x8000000 + 188)

static u16 EWRAM_BSS ROMPAGE_ROM;

/* Detection diagnostics (io_ezfo.h). EWRAM: written while the ROM is unmapped. */
static u8  EWRAM_BSS s_det_result;        /* EZFO_DET_*                                  */
static u8  EWRAM_BSS s_lookalikes;        /* pages that passed the header word, not the  */
static u16 EWRAM_BSS s_lookalike_first;   /* fingerprint; the first of them (0xFFFF=none) */

/* ---- identifying OUR page ----------------------------------------------------
 * The rompage register is write-only, so the driver has to find the page the CPU is
 * running from by comparing what each candidate page shows at 0x08000000 against what
 * the running image looked like BEFORE the first switch. Upstream compared one 16-bit
 * header word (version | complement, at 0xBC). That word is identical for every build
 * that shares a title, so a stale same-title image in PSRAM (probed first) or at a lower
 * NOR page (scanned upward) was taken for the running one, and the driver returned into
 * ROM .text with the WRONG image mapped -- a silent hang on the first screen (Guy's
 * 2026-09-12 cart run: PokeDNA.gba booted from NOR with an earlier PokeDNA.gba still in
 * PSRAM/NOR). The header word is now only a pre-filter: a page is accepted when
 * EZFO_FP_WINS windows spread over the whole image, tail included, checksum the same as
 * the running image did. A different build differs at the first window it reaches; a
 * shorter or partial copy differs at the tail.
 *
 * Everything here runs from EWRAM with the ROM possibly unmapped: no array initialisers
 * (memset lives in ROM) and no division (so does __aeabi_uidiv) -- the walk itself is
 * ezfo_fp.h, one definition shared with tests/host_fpwalk_test.c, forced inline below so
 * its code lands in THIS function's EWRAM bytes and not in .text. The reference lives
 * on the IWRAM stack. */
#define EZFO_FP_FN   static inline __attribute__((always_inline))
#define EZFO_FP_WORD u32
#include "ezfo_fp.h"

extern char __text_start[];                         /* devkitARM ld scripts: image   */
extern char __rom_end__[];                          /* bounds, every build, any size */

/* noinline: -O2 would otherwise plant a copy at each of the three call sites, and this
 * whole file is paid for in EWRAM bytes. */
static int EWRAM_CODE __attribute__((noinline)) fp_walk(u32* fp, int take) {
  return ezfo_fp_walk(fp, take, (unsigned long)__text_start, (unsigned long)__rom_end__);
}

/* Map `page`, then: 0 = header word differs, 1 = header word matches but the windows do
 * not (a look-alike), 2 = this page shows the running image. With use_fp == 0 the header
 * word alone decides (the upstream rule), for a bus that could not hold the reference
 * still. Leaves `page` mapped either way. noinline for the same EWRAM-bytes reason. */
static int EWRAM_CODE __attribute__((noinline)) _EZFO_TestRompage(u32* fp, u16 hdr, u16 page, int use_fp) {
  SetRompage(page);
  if (hdr != ROM_HEADER_CHECKSUM) return 0;
  /* TWO consecutive walks must disagree before a header match is demoted to a
   * look-alike: one transient bad word on the TRUE page is otherwise fatal. */
  if (use_fp && !fp_walk(fp, 0) && !fp_walk(fp, 0)) return 1;
  return 2;
}

static void EWRAM_CODE note_lookalike(u16 page) {
  if (s_lookalikes < 255) s_lookalikes++;
  if (s_lookalike_first == 0xFFFF) s_lookalike_first = page;
}

bool EWRAM_CODE _EZFO_startUp(void) {
  u32 fp[EZFO_FP_WINS];            /* IWRAM stack: readable while the ROM is unmapped */
  int i, r, use_fp;
  bool ok = false;
#if FLASHCARTIO_EZFO_DISABLE_IRQ != 0
  u16 ime = REG_IME;
  REG_IME = 0;
#endif
  const u16 hdr = ROM_HEADER_CHECKSUM;
  /* The reference, taken BEFORE the first switch: this is the running image, whatever
   * page it is on. Taken twice; if the two disagree the bus cannot be trusted to read
   * the same bytes twice and the fingerprint would reject the true page, which is fatal
   * (the return lands in unmapped ROM). Fall back to the header-only rule and say so. */
  fp_walk(fp, 1);
  use_fp = fp_walk(fp, 0);
  s_lookalikes = 0;
  s_lookalike_first = 0xFFFF;

  // unmap rom; if the running image is STILL there, the register did nothing: not an ezflash
  if (_EZFO_TestRompage(fp, hdr, ROMPAGE_BOOTLOADER, use_fp) == 2) {
    s_det_result = EZFO_DET_NOT_EZFO;
    goto done;
  }

  // find where the rom is mapped, try psram first
  r = _EZFO_TestRompage(fp, hdr, ROMPAGE_PSRAM, use_fp);
  if (r == 1) note_lookalike(ROMPAGE_PSRAM);
  if (r == 2) { ROMPAGE_ROM = ROMPAGE_PSRAM; ok = true; }

  // try and find it within norflash, test each 128 KiB page (512 pages; the kernel
  // launches a NOR game with rompage = NORaddress >> 17, omega-de-kernel NORflash_OP.c:249)
  for (i = 0; i < S98WS512PE0_FLASH_PAGE_MAX && !ok; i++) {
    r = _EZFO_TestRompage(fp, hdr, (u16)i, use_fp);
    if (r == 1) note_lookalike((u16)i);
    if (r == 2) { ROMPAGE_ROM = (u16)i; ok = true; }
  }
  /* Not found. Before returning into ROM, put SOMETHING plausible under the return
   * address: leaving NOR page 0x1FF mapped (the loop's last probe) is a certain crash
   * with no screen left to say so. If a page matched the header word, map THAT --
   * upstream's rule is a bad answer, page 0x1FF is no answer at all -- and report it
   * header-only. Without this, ONE transient bad word inside one fp_walk demotes the
   * TRUE page to a look-alike and nothing else can ever match: a boot that works on
   * main hangs here. */
  if (!ok && s_lookalike_first != 0xFFFF) {
    ROMPAGE_ROM = s_lookalike_first;
    SetRompage(ROMPAGE_ROM);
    ok = true;
    s_det_result = EZFO_DET_OK_HDRONLY;
  } else if (!ok) {
    /* Nothing matched at all -- not even a look-alike header word. The loop above leaves
     * NOR page 0x1FF mapped (its last probe): returning into ROM with that under the
     * return address is the same certain crash as above, only now with no plausible page
     * to fall back on. No image of ours can start above 128 KiB in, so 0x1FF can never be
     * right; map PSRAM instead -- it is the kernel's own SD-launch page, so it is at least
     * POSSIBLY right, and it makes the caller's "no flashcart" diagnostic reachable
     * instead of hanging before it can run. _EZFO_startUp() still returns false, and
     * ROMPAGE_ROM is never read in that case (_EZFO_rompage() returns 0xFFFF unless
     * s_det_result is OK/OK_HDRONLY): flashcartio_activate() keys off the false return,
     * not off what is mapped, so active_flashcart stays NO_FLASHCART. */
    ROMPAGE_ROM = ROMPAGE_PSRAM;
    SetRompage(ROMPAGE_PSRAM);
    s_det_result = EZFO_DET_NO_PAGE;
  } else {
    s_det_result = use_fp ? EZFO_DET_OK : EZFO_DET_OK_HDRONLY;
  }

done:
#if FLASHCARTIO_EZFO_DISABLE_IRQ != 0
  REG_IME = ime;
#endif
  return ok;
}

/* ROM-resident getters: read EWRAM state, never called with a transfer in flight. */
int _EZFO_detect_result(void) { return s_det_result; }

u16 _EZFO_rompage(void) {
  return (s_det_result == EZFO_DET_OK || s_det_result == EZFO_DET_OK_HDRONLY) ? ROMPAGE_ROM
                                                                              : 0xFFFF;
}

unsigned _EZFO_lookalikes(unsigned* first) {
  if (first) *first = s_lookalike_first;
  return s_lookalikes;
}

bool EWRAM_CODE _EZFO_readSectors(u32 address, u32 count, void* buffer) {
#if FLASHCARTIO_EZFO_DISABLE_IRQ != 0
  u16 ime = REG_IME;
  REG_IME = 0;
#endif
  SetRompage(ROMPAGE_BOOTLOADER);
  const u32 result = Read_SD_sectors(address, count, buffer);
  SetRompage(ROMPAGE_ROM);
#if FLASHCARTIO_EZFO_DISABLE_IRQ != 0
  REG_IME = ime;
#endif
  return result == 0;
}

bool EWRAM_CODE _EZFO_writeSectors(u32 address, u32 count, const void* buffer) {
#if FLASHCARTIO_EZFO_DISABLE_IRQ != 0
  u16 ime = REG_IME;
  REG_IME = 0;
#endif
  SetRompage(ROMPAGE_BOOTLOADER);
  const u32 result = Write_SD_sectors(address, count, buffer);
  SetRompage(ROMPAGE_ROM);
#if FLASHCARTIO_EZFO_DISABLE_IRQ != 0
  REG_IME = ime;
#endif
  return result == 0;
}

// Quiesce the system (IRQs/DMA/timers off), map the EZ-Flash kernel at the ROM
// window, then SoftReset into it. Runs from EWRAM (the gamepak window is unmapped
// once BOOTLOADER is paged in). No return.
void EWRAM_CODE _EZFO_reboot(void) {
  REG_IME = 0;
  *(vu16*)(REG_BASE_ + 0x0200) = 0;       // REG_IE  : disable all IRQ sources
  *(vu16*)(REG_BASE_ + 0x0202) = 0xFFFF;  // REG_IF  : acknowledge any pending
  REG_DMA0CNT = 0; REG_DMA1CNT = 0;       // stop all DMA channels
  REG_DMA2CNT = 0; REG_DMA3CNT = 0;
  *(vu16*)(REG_BASE_ + 0x0102) = 0;       // TM0..TM3 control: disable timers
  *(vu16*)(REG_BASE_ + 0x0106) = 0;
  *(vu16*)(REG_BASE_ + 0x010A) = 0;
  *(vu16*)(REG_BASE_ + 0x010E) = 0;
  SetRompage(ROMPAGE_BOOTLOADER);             // map the kernel at 0x08000000
  asm volatile("swi 0x00" ::: "memory");      // SoftReset -> 0x08000000 (kernel/menu)
  for (;;) {}                                 // unreachable
}
