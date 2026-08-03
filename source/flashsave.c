/*
 * 128 KiB GBA FLASH save I/O for the emulator build. See flashsave.h.
 *
 * Every access to the save window is u8 — the chip is byte-wide and a 16/32-bit
 * access reads garbage on hardware and is rejected by some emulator cores. That is
 * why this file uses explicit volatile u8 pointers everywhere and never memcpy()s
 * directly out of the window (memcpy is free to widen the transfer).
 */
#include <tonc.h>
#include <string.h>
#include "flashsave.h"

#define FLASH_BASE   ((volatile uint8_t*)0x0E000000)
#define FLASH_CMD1   ((volatile uint8_t*)0x0E005555)
#define FLASH_CMD2   ((volatile uint8_t*)0x0E002AAA)

/* Atmel/Sanyo/Macronix command set — the one VBA-M and mGBA implement. */
#define CMD_UNLOCK1  0xAA
#define CMD_UNLOCK2  0x55
#define CMD_ERASE    0x80
#define CMD_ERASE_SECTOR 0x30
#define CMD_WRITE    0xA0
#define CMD_BANK     0xB0
#define CMD_ID_ENTER 0x90
#define CMD_ID_EXIT  0xF0

/* The save window needs the slowest SRAM wait state (8-bit bus). Without this a
 * real cart returns garbage; emulators are lenient but there is no reason to rely
 * on that. */
static void bus_setup(void) { REG_WAITCNT = (REG_WAITCNT & ~3u) | 3u; }

static void cmd(uint8_t c) {
  *FLASH_CMD1 = CMD_UNLOCK1;
  *FLASH_CMD2 = CMD_UNLOCK2;
  *FLASH_CMD1 = c;
}

/* 128 KiB flash presents 64 KiB at a time; everything above 0xFFFF needs bank 1. */
static void set_bank(uint8_t bank) {
  cmd(CMD_BANK);
  *FLASH_BASE = bank;
}

/* Poll until `addr` reads back `want`, or we give up. Flash program/erase completes
 * asynchronously; the canonical ready test is "the value we wrote is readable".
 * The bound keeps a dead chip (or an emulator with no save allocated) from hanging
 * the app forever — this returns false instead. */
static bool wait_for(volatile uint8_t* addr, uint8_t want) {
  for (uint32_t i = 0; i < 0x20000u; i++)
    if (*addr == want) return true;
  return false;
}

/* THE SAVE-TYPE SIGNATURE.
 * VBA-M (Delta's GBA core) and mGBA decide a ROM's save type by scanning the ROM
 * IMAGE for marker strings — "SRAM_V", "FLASH_V", "FLASH512_V", "FLASH1M_V" and so
 * on. Without one, the emulator gives this ROM no save (or a 64 KiB one), and a
 * Gen-3 save is 128 KiB. So the string simply has to EXIST somewhere in the binary.
 *
 * It must survive `--gc-sections`, which gba.specs enables — an unreferenced const
 * would be stripped and the build would silently lose its save. Hence the volatile
 * read below: it is not dead code, it is what keeps the signature in the image. */
const char pdna_flash_save_type[] __attribute__((used)) = "FLASH1M_V103";

bool flashsave_probe(uint16_t* out_id) {
  /* NOTE the placement of `volatile`: it must qualify the POINTER, not the pointee.
   * `volatile const char* keep` would be a pointer-to-volatile-char and the store
   * below would be optimised away — which is exactly what happened the first time,
   * and the signature vanished from the linked image. Verify after every build with:
   *   python3 -c "print(open('pokedna-delta.gba','rb').read().find(b'FLASH1M_V103'))" */
  static const char* volatile keep;
  keep = pdna_flash_save_type;   /* anchors the signature against --gc-sections */
  bus_setup();
  cmd(CMD_ID_ENTER);
  /* a short settle: the id only appears after the command is latched */
  for (volatile int i = 0; i < 20000; i++) { }
  uint8_t man = FLASH_BASE[0];
  uint8_t dev = FLASH_BASE[1];
  cmd(CMD_ID_EXIT);
  for (volatile int i = 0; i < 20000; i++) { }
  if (out_id) *out_id = (uint16_t)((man << 8) | dev);
  /* 0xFFFF (open bus) and 0x0000 both mean "no flash here". */
  uint16_t id = (uint16_t)((man << 8) | dev);
  return id != 0xFFFF && id != 0x0000;
}

bool flashsave_read(uint8_t* dst, uint32_t len) {
  if (!dst || len > FLASHSAVE_SIZE) return false;
  bus_setup();
  for (uint32_t off = 0; off < len; ) {
    uint8_t bank = (uint8_t)(off / FLASHSAVE_BANK_SIZE);
    set_bank(bank);
    uint32_t in_bank = off % FLASHSAVE_BANK_SIZE;
    uint32_t n = FLASHSAVE_BANK_SIZE - in_bank;
    if (n > len - off) n = len - off;
    /* byte-by-byte on purpose: memcpy may widen the access, which this chip and
     * some emulator cores do not accept on the save window */
    for (uint32_t i = 0; i < n; i++) dst[off + i] = FLASH_BASE[in_bank + i];
    off += n;
  }
  return true;
}

/* Erase one 4 KiB sector. `addr` is the offset WITHIN the current bank. */
static bool erase_sector(uint32_t in_bank) {
  volatile uint8_t* p = FLASH_BASE + in_bank;
  cmd(CMD_ERASE);
  *FLASH_CMD1 = CMD_UNLOCK1;
  *FLASH_CMD2 = CMD_UNLOCK2;
  *p = CMD_ERASE_SECTOR;
  return wait_for(p, 0xFF);                 /* erased flash reads 0xFF */
}

static bool write_byte(uint32_t in_bank, uint8_t v) {
  volatile uint8_t* p = FLASH_BASE + in_bank;
  cmd(CMD_WRITE);
  *p = v;
  return wait_for(p, v);
}

bool flashsave_write(const uint8_t* src, uint32_t len) {
  if (!src || len > FLASHSAVE_SIZE) return false;
  bus_setup();

  for (uint32_t off = 0; off < len; off += FLASHSAVE_SECTOR) {
    uint8_t bank = (uint8_t)(off / FLASHSAVE_BANK_SIZE);
    uint32_t in_bank = off % FLASHSAVE_BANK_SIZE;
    set_bank(bank);
    if (!erase_sector(in_bank)) return false;
    uint32_t n = FLASHSAVE_SECTOR;
    if (n > len - off) n = len - off;
    for (uint32_t i = 0; i < n; i++) {
      /* Erased flash is already 0xFF; skipping those bytes is both correct and a
       * large speedup on the mostly-empty tail of a Gen-3 save. */
      if (src[off + i] == 0xFF) continue;
      if (!write_byte(in_bank + i, src[off + i])) return false;
    }
  }

  /* Read back and byte-compare. There is no .bak to fall back on in this build, so
   * the caller MUST surface a false return to the user rather than assume success. */
  for (uint32_t off = 0; off < len; ) {
    uint8_t bank = (uint8_t)(off / FLASHSAVE_BANK_SIZE);
    set_bank(bank);
    uint32_t in_bank = off % FLASHSAVE_BANK_SIZE;
    uint32_t n = FLASHSAVE_BANK_SIZE - in_bank;
    if (n > len - off) n = len - off;
    for (uint32_t i = 0; i < n; i++)
      if (FLASH_BASE[in_bank + i] != src[off + i]) return false;
    off += n;
  }
  return true;
}
