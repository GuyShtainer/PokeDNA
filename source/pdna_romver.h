/*
 * pdna_romver.h — the description this ROM carries of the bytes it expects to find
 * inside itself, and the pure-C core that checks it.
 *
 * PURE C (stdint only). tests/host_romver_test.c dual-compiles the core on the Mac and
 * tools/stamp_rom_windows.py hard-codes this exact layout. Change a constant here and
 * you MUST change it there; PDNA_RV_SIZE's compile-time assert plus the host test are
 * what catch you when you forget.
 *
 * WHY THIS EXISTS
 * ---------------
 * PokeDNA.gba is ~12.5 MB and this flashcart has a documented history of loading a big
 * image incompletely (projects/rom-load-lab), a failure that today is indistinguishable
 * from every other hang. So the image carries CRC32s of a handful of sampled windows —
 * a low-ROM control, three slabs of .text, four generated lookup tables and nine art
 * probes spread across the image — and re-reads them at boot over the cart bus.
 *
 * WHAT AN "OK" ACTUALLY PROVES — READ THIS BEFORE QUOTING IT
 * ---------------------------------------------------------
 * ~64 KiB of a ~12.5 MB image is sampled: 0.53%. For a single contiguous corrupt run
 * placed uniformly at random the chance at least one window intersects it is roughly
 * sum((L + len_i)) / image_size, so:
 *     one lost 32 KiB cluster ..... ~5%      (a coin flip it is NOT seen)
 *     a 512 KiB run ............... ~50%
 *     >= 2 MB run ................. ~100%
 *     whole file wrong/zero-filled  100%
 * Therefore "rom self-check: OK" rules out GROSS corruption, and — because windows sit
 * inside .text and inside the parser's own offset tables — it rules out a corrupt code
 * or table region, which is the only kind of corruption that can produce a HANG rather
 * than a garbled screen. It does NOT rule out a small mid-file hole in art. The
 * exhaustive check is the host-side one: tools/stamp_rom_windows.py --verify byte-checks
 * the card's own copy in a second, with no console involved.
 *
 * TWO RULES INHERITED FROM rom-load-lab, BOTH LOAD-BEARING
 * --------------------------------------------------------
 * 1. The descriptor is located BY SYMBOL (nm), never by scanning for the magic:
 *    this file's own literal pool holds byte-identical copies of both magic words.
 *    (rom-load-lab/source/diag_data.c:6-10)
 * 2. The hole rule: the descriptor's own bytes are excluded from everything it
 *    checksums, so re-stamping the same image is idempotent. Here the hole is enforced
 *    by exclusion — the stamper refuses to build if any window overlaps
 *    [hole_off, hole_off+hole_len).  (rom-load-lab/source/diag.h:96-111)
 *
 * A THIRD RULE, FROM THE OMEGA DE KERNEL ITSELF (verified in the vendored source)
 * ------------------------------------------------------------------------------
 * The kernel MODIFIES the image after loading it, so the running image is legitimately
 * NOT the file. Two facts that are commonly misstated, both checked here against
 * reference/omega-de-kernel/source:
 *
 *   * The DE has TWO SD load paths, not one. ezkernelnew.c:2915 calls Check_pat(); if a
 *     valid /PATCH/<name>.pat exists the FPGA streams the file from a cluster run-list
 *     (Send_FATbuffer), and if it does not, ezkernelnew.c:2934 calls Loadfile2PSRAM()
 *     (ezkernelnew.c:1245-1296) which is a plain CPU copy: f_read 128 KiB into
 *     pReadCache, PatchInternal() over that block, dmaCopy into PSRAM, repeat.
 *   * GBApatch.c PatchInternal (line 241) rewrites every aligned word equal to
 *     0x03007FFC or 0x03FFFFFC to 0x03007FF4, and Patch_B_address rewrites ROM word 0.
 *     Results are cached in /PATCH/<name>.pat, and Check_pat (GBApatch.c:720-767)
 *     validates that cache against ONLY gl_reset_on/gl_rts_on/gl_sleep_on/gl_cheat_on —
 *     no size, no mtime, no hash, keyed on the ROM FILENAME. So a rebuild silently
 *     reuses the PREVIOUS build's patch offsets. That is why the failure message says
 *     "MODIFIED OR INCOMPLETE" and lists deleting the /PATCH cache file as the first remedy: a
 *     stale .pat and a bad SD copy are not distinguishable from inside the cartridge.
 *   * Patch_Reset_Sleep drops a sleep/RTS blob at iTrimSize. SetTrimSize
 *     (GBApatch.c:268-320) walks back only PATCH_LENGTH+16 bytes — PATCH_LENGTH is
 *     0x300, 0x1000 or 0x2000 — so at most ~0x2010 bytes below EOF, NOT 128 KiB.
 *
 * Consequences, all enforced at build time by tools/stamp_rom_windows.py:
 *   - no window may cover byte 0 (HEAD_GUARD);
 *   - no window may contain an aligned 0x03007FFC / 0x03FFFFFC word;
 *   - no window may sit in the last 64 KiB (TAIL_GUARD).
 *
 * TAIL_GUARD IS AN ACCEPTED BLIND SPOT — SAY SO OUT LOUD.
 * The 64 KiB guard is ~30x larger than the measured 0x2010 walk-back, and it blinds the
 * check to the last 64 KiB of the image, which is exactly where the crt0 LMA copies live
 * (__iwram_lma .. __rom_end__, ~8.8 KB: the IWRAM .data image and the EWRAM-resident
 * flashcart driver). Corruption there is code corruption and could absolutely hang the
 * tool, and this check will not see it. It is accepted deliberately, because a false
 * alarm on EVERY SD boot (which is what a window in the kernel's drop zone would
 * produce) is worse than a blind spot: it would train the user to ignore the alarm. The
 * host-side --verify covers the tail exhaustively (size compare + whole-image CRC), so
 * the blind spot is on the cartridge only.
 *
 * >>> Do NOT add -flto to this project. <<<  It would let the verifier see the
 * near-empty initialiser in pdna_romver_data.c and fold every read into a compile-time
 * constant, silently turning this into something that always reports success.
 */
#ifndef PDNA_ROMVER_H
#define PDNA_ROMVER_H

#include <stdint.h>

/* Magic words, chosen so the bytes read as text in a hex dump of the .gba
 * (little-endian: the LSB is the first byte on disk). */
#define PDNA_RV_MAGIC0  0x414E4450u /* 'P','D','N','A' */
#define PDNA_RV_MAGIC1  0x31305652u /* 'R','V','0','1' */
#define PDNA_RV_STAMPED 0x504D5453u /* 'S','T','M','P' — written post-link only */
#define PDNA_RV_VERSION 1u

#define PDNA_RV_MAX_WINDOWS 24u
#define PDNA_RV_ROM_BASE    0x08000000u

/* Defensive clamps. The descriptor lives in the very ROM whose corruption is being
 * hunted, so NOTHING it says may be trusted enough to drive a read. A flipped bit that
 * made a len 0xFFFFFFFF would otherwise walk 4 GiB over the cart bus at boot — a
 * multi-minute black screen produced by the anti-hang instrument itself. */
#define PDNA_RV_MAX_WINLEN 65536u   /* biggest window we will ever read */
#define PDNA_RV_MAX_IMAGE  0x02000000u /* 32 MiB: the whole cartridge address window */

/* Retries per failing window. 4 is the house number — icopy_verified
 * (source/box_oam.c:110-121), rom_icon_read_verified, upload_tiles_verified and
 * wp_copy_verified all retry 4x and fail soft. A single transient bit-flip on a
 * lying cart bus must not permanently strip Guy's write features. */
#define PDNA_RV_TRIES 4

/* window flags */
#define PDNA_RVW_CONTROL 0x0001u /* low-ROM control: it arrived if we are executing   */
#define PDNA_RVW_EXACT   0x0002u /* window must stay inside its anchor symbol's extent
                                  * (art blobs). Without it the window is a REGION
                                  * PROBE: the anchor is only a nearby global symbol
                                  * and the window may run past it into its neighbours,
                                  * which is the point — it samples a slab of code or
                                  * of generated tables, not one named object.        */
/* region kind — for the log line, so "the bad window is CODE" reads differently from
 * "the bad window is ART". Exactly one of these per window. */
#define PDNA_RVW_K_TEXT  0x0010u
#define PDNA_RVW_K_TABLE 0x0020u
#define PDNA_RVW_K_ART   0x0040u
#define PDNA_RVW_K_MASK  0x0070u

typedef struct {
  uint32_t base_addr; /* LINKER-stamped address of the anchor symbol.
                       * 0 == the symbol does not exist in this build (artless, or
                       * `make sd` for the shiny/back blobs). NEVER fold the offset into
                       * this relocation: a weak-undefined &sym[0x200000] resolves to
                       * 0x00200000, not to 0, and the absence test would break. */
  uint32_t off;       /* compile-time byte offset of the window inside/after the anchor */
  uint32_t len;       /* POST-LINK: bytes actually checksummed; 0 == window unused      */
  uint32_t crc;       /* POST-LINK: CRC32 of the window as it is in the .gba file       */
  uint32_t flags;     /* PDNA_RVW_*                                                     */
  char     tag[4];    /* 4 chars for the log line; NOT NUL-terminated (print with %.4s) */
} PdnaRvWindow;       /* 24 bytes */

typedef struct {
  uint32_t magic0, magic1;
  uint32_t version;
  uint32_t stamped;     /* PDNA_RV_STAMPED once tools/stamp_rom_windows.py ran   */
  uint32_t image_bytes; /* size of the .gba it was stamped from                  */
  uint32_t n_windows;   /* declared entries (compile time)                       */
  uint32_t n_present;   /* entries whose anchor symbol exists (post-link)        */
  uint32_t hole_off;    /* this struct's own file offset — the hole              */
  uint32_t hole_len;    /* == sizeof(PdnaRomVerify)                              */
  uint32_t crc_whole;   /* whole file, hole read as zeroes. HOST VERIFIER ONLY:
                         * the cartridge must never check it, because the Omega DE
                         * kernel legitimately rewrites part of the image in PSRAM
                         * (see the header comment).                             */
  uint32_t reserved[2];
  PdnaRvWindow w[PDNA_RV_MAX_WINDOWS];
} PdnaRomVerify;

#define PDNA_RV_HDR_BYTES 48u
#define PDNA_RV_SIZE      (PDNA_RV_HDR_BYTES + PDNA_RV_MAX_WINDOWS * 24u) /* 624 */
/* The stamper hard-codes 48/24/624. This is what stops the two sides drifting. */
typedef char pdna_rv_size_assert[(sizeof(PdnaRomVerify) == PDNA_RV_SIZE) ? 1 : -1];

typedef enum {
  PDNA_RV_OK = 0,    /* every present window matched (possibly after a retry)     */
  PDNA_RV_UNSTAMPED, /* the post-link tool never ran — we have no opinion          */
  PDNA_RV_NOWINDOWS, /* stamped, but this build has nothing to check               */
  PDNA_RV_BAD_DESC,  /* the descriptor itself failed a sanity clamp — no opinion   */
  PDNA_RV_BAD_IMAGE, /* stable mismatch: the image in the cart is not the file     */
  PDNA_RV_BAD_BUS    /* re-reads of one window disagreed and none ever matched     */
} PdnaRvVerdict;

/* Per-window outcome. Pure and public so tests/host_romver_test.c can exercise the
 * bus-vs-image discrimination without having to simulate an unstable bus. */
typedef enum {
  PDNA_RVC_MATCH      = 0, /* first read matched the stamp                          */
  PDNA_RVC_RECOVERED  = 1, /* a RETRY matched: the bus lied once, then told the truth
                            * — same outcome icopy_verified treats as success       */
  PDNA_RVC_UNSTABLE   = 2, /* attempts disagreed and none ever matched: bus is lying */
  PDNA_RVC_STABLE_BAD = 3  /* every attempt agreed, every one differs: IMAGE is wrong*/
} PdnaRvClass;

typedef struct {
  int32_t  verdict;     /* PdnaRvVerdict                                            */
  int32_t  n_checked;   /* windows actually read                                    */
  int32_t  n_bad;       /* PDNA_RVC_STABLE_BAD                                      */
  int32_t  n_unstable;  /* PDNA_RVC_UNSTABLE                                        */
  int32_t  n_recovered; /* PDNA_RVC_RECOVERED (still counted as a pass)             */
  int32_t  n_skipped;   /* declared windows rejected by a sanity clamp              */
  int32_t  first_bad;   /* index into w[] of the first non-clean window, -1 if none  */
  uint32_t bad_addr;
  uint32_t bad_flags;   /* that window's flags — so the log can say CODE vs ART      */
  uint32_t bad_expect, bad_got, bad_got2;
  int32_t  bad_tries;
  char     bad_tag[4];
} PdnaRvResult;

/* Fill a caller-supplied 16-entry CRC32 nibble table (64 bytes).
 *
 * SIXTEEN entries, not 256, and that is not a micro-decision. This build has a
 * reproducible boot crash once >= ~1,232 bytes of new IWRAM .bss appear, and the crash
 * site is newlib's _svfprintf_r — which is exactly what log_line() calls, on top of its
 * own char tmp[256], while this table's frame is still live. A 1 KiB table on the IWRAM
 * stack would sit directly under that path. 64 bytes does not. The cost is ~2 table
 * lookups per byte instead of 1; the whole scan is still under a tenth of a second and
 * invisible next to the 8-retry SD mount loop. */
void     pdna_rv_crc32_table(uint32_t* tab16);

/* Standard CRC32 (poly 0xEDB88320, init/final 0xFFFFFFFF) — bit-for-bit what
 * zlib.crc32 computes, which is the contract with the stamper. CRC32("123456789")
 * must be 0xCBF43926; the host test asserts it. */
uint32_t pdna_rv_crc32(const uint32_t* tab16, const void* data, uint32_t len);

/* Classify one window from up to `n` CRC attempts (attempt 0 first). */
int pdna_rv_classify(uint32_t expect, const uint32_t* got, int n);

/* Read every present window and fill *out.
 *
 * `rom` points at the image base and `rom_base` is the address that pointer stands for
 * (0x08000000 on the cartridge; a malloc'd buffer plus a synthetic base in the host
 * test). `tab16` is caller-supplied scratch — see pdna_rv_crc32_table. */
void pdna_rv_check(const PdnaRomVerify* d, const unsigned char* rom,
                   uint32_t rom_base, uint32_t* tab16, PdnaRvResult* out);

#endif /* PDNA_ROMVER_H */
