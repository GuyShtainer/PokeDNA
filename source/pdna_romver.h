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
 * TWO INSTRUMENTS LIVE HERE, AND THEY ANSWER DIFFERENT QUESTIONS
 * --------------------------------------------------------------
 *   THE WINDOWS (v1, boot, ~64 KiB, free)  — 17 sampled 4 KiB windows checked at every
 *     boot. Cheap enough to be unconditional; blind to 99.47% of the image. Read the
 *     next section for exactly what it can and cannot see.
 *   THE GRID (v2, on demand, the WHOLE image, seconds) — every byte from 0x08000000 to
 *     the end of the stamped image, CRC32'd in fixed-size regions with a per-region
 *     stamp, so a hole's ADDRESS shows up on screen instead of merely its existence.
 *     Run from the boot hold (R+SELECT) or the browser's FILE MENU; see
 *     source/pdna_romfull.h. A green grid is proof the load is intact; the windows can
 *     only ever be evidence.
 *
 * WHAT AN "OK" FROM THE WINDOWS ACTUALLY PROVES — READ THIS BEFORE QUOTING IT
 * --------------------------------------------------------------------------
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
 * exhaustive checks are the host-side one (tools/stamp_rom_windows.py --verify byte-checks
 * the card's own copy in a second, with no console involved) and, ON THE CONSOLE WITH NO
 * PC AT ALL, the region grid below.
 *
 * THE REGION GRID (v2)
 * --------------------
 * The image is cut into fixed regions (64 KiB unless the image needs coarser ones to fit
 * PDNA_RV_MAX_REGIONS) and each region's CRC32 is stamped post-link. The cartridge
 * re-reads every region and paints one cell per region, so the answer is not "something
 * is wrong" but "region 74, at 0x004A0000, is wrong". Coverage is 100% of the image minus
 * the ZONES below — measured and printed, never assumed.
 *
 * The grid has to survive the fact that THE RUNNING IMAGE IS LEGITIMATELY NOT THE FILE
 * (see the kernel notes further down). Two mechanisms, and both sides — this core and
 * tools/stamp_rom_windows.py — implement them identically or every boot is a false alarm:
 *
 *   ZONES.  Byte ranges the loader or a post-link tool of ours rewrites are read as
 *           ZEROES by both sides instead of being compared: the first PDNA_RV_HEAD_GUARD
 *           bytes (Patch_B_address rewrites ROM word 0), the descriptor's own bytes (the
 *           hole rule — this is what keeps stamping idempotent), the 16-byte g_pdna_fuse
 *           / g_pdna_sav locator records that tools/fuse_*.py patch, and the last
 *           PDNA_RV_TAIL_GUARD bytes (the kernel's sleep/RTS blob lands at iTrimSize).
 *           All zones are 4-aligned so no canonical word straddles one.
 *   CANONICAL IRQ WORDS.  PatchInternal (GBApatch.c:241, verified in the vendored
 *           source) rewrites every aligned word equal to 0x03007FFC or 0x03FFFFFC to
 *           0x03007FF4 — anywhere in the image, up to EMax=32 sites. A whole-image CRC
 *           cannot guard against that by moving, the way a 4 KiB window can, so both
 *           sides map all three values to one before feeding the CRC. The check is then
 *           invariant to whether the kernel patched, did not patch, or patched only the
 *           first 32 sites.
 *
 * WHAT THE GRID STILL CANNOT DISTINGUISH: a bad SD copy from a STALE /PATCH/<name>.pat.
 * Check_pat (GBApatch.c:720-767) validates its cache against only the four gl_*_on flags
 * — no size, no mtime, no hash, keyed on the ROM FILENAME — so a rebuild under the same
 * name gets patched at the PREVIOUS build's offsets, writing 0x03007FF4 over words that
 * were never IRQ words. Those are real differences from the shipped file and the grid
 * will show them. That is why the remedy line names deleting the /PATCH cache entry first: it is
 * the cheaper of the two fixes, and the two causes are not distinguishable from inside
 * the cartridge.
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
#define PDNA_RV_VERSION 2u          /* 1 = windows only; 2 = windows + region grid */

#define PDNA_RV_MAX_WINDOWS 24u
#define PDNA_RV_ROM_BASE    0x08000000u

/* ---- the region grid ----------------------------------------------------------
 * 256 regions is the on-screen budget as much as the RAM one: 16 columns x 16 rows of
 * cells is the most a 240x160 screen can show and still be readable in a phone photo,
 * which is the only way this verdict ever leaves the console. The stamper picks the
 * SMALLEST region size >= 64 KiB that fits the image into that many cells, so a 12.5 MB
 * image gets 64 KiB granularity and a hypothetical 20 MB one gets 128 KiB. */
#define PDNA_RV_MAX_REGIONS  256u
#define PDNA_RV_MIN_RSHIFT    16u   /* 64 KiB  */
#define PDNA_RV_MAX_RSHIFT    24u   /* 16 MiB  */
#define PDNA_RV_MAX_ZONES      6u   /* head, descriptor, 2 locator records, tail (+1) */

/* Guards, shared with tools/stamp_rom_windows.py. Both are ZONES for the grid and
 * refusal rules for a window. */
#define PDNA_RV_HEAD_GUARD 0x100u    /* the kernel rewrites ROM word 0                */
#define PDNA_RV_TAIL_GUARD 0x10000u  /* the kernel's sleep/RTS blob lands near EOF    */

/* The three values PatchInternal collapses into one. Canonicalised on both sides so a
 * kernel-patched image and an unpatched one produce the SAME region CRC. */
#define PDNA_RV_PW0 0x03007FFCu
#define PDNA_RV_PW1 0x03FFFFFCu
#define PDNA_RV_PWC 0x03007FF4u

/* Attempts per mismatching region. Three, not the windows' four: a region is 16x the
 * size of a window, the whole scan is already seconds long, and three reads are enough
 * to separate "the bus lied once" from "these bytes are consistently wrong". */
#define PDNA_RV_GRID_TRIES 3

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

/* A byte range read as ZEROES instead of compared, by both this core and the stamper.
 * Sorted by `off`, non-overlapping, 4-aligned — the stamper merges and asserts all
 * three, because the region walk below relies on it. */
typedef struct {
  uint32_t off;
  uint32_t len;
} PdnaRvZone;           /* 8 bytes */

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
  uint32_t region_shift;/* POST-LINK: log2(region bytes); 0 == no grid in this image */
  uint32_t n_regions;   /* POST-LINK: ceil(image_bytes / region bytes)               */
  PdnaRvWindow w[PDNA_RV_MAX_WINDOWS];
  uint32_t   n_zones;                          /* POST-LINK                        */
  PdnaRvZone z[PDNA_RV_MAX_ZONES];             /* POST-LINK                        */
  uint32_t   excl[PDNA_RV_MAX_REGIONS / 32u];  /* POST-LINK: 1 = region has no
                                                * verifiable byte left (it is all
                                                * zone) — shown as SKIP, never as a
                                                * pass. bit i of excl[i>>5].        */
  uint32_t   rcrc[PDNA_RV_MAX_REGIONS];        /* POST-LINK: per-region CRC32       */
} PdnaRomVerify;

#define PDNA_RV_HDR_BYTES  48u
#define PDNA_RV_ZONES_OFF  (PDNA_RV_HDR_BYTES + PDNA_RV_MAX_WINDOWS * 24u)  /*  624 */
#define PDNA_RV_EXCL_OFF   (PDNA_RV_ZONES_OFF + 4u + PDNA_RV_MAX_ZONES * 8u) /*  676 */
#define PDNA_RV_RCRC_OFF   (PDNA_RV_EXCL_OFF + PDNA_RV_MAX_REGIONS / 8u)     /*  708 */
#define PDNA_RV_SIZE       (PDNA_RV_RCRC_OFF + PDNA_RV_MAX_REGIONS * 4u)     /* 1732 */
/* The stamper hard-codes 48/24/624/676/708/1732. This is what stops the two sides
 * drifting — and the host test asserts it too, so a struct edit that forgets the tool
 * fails on the Mac instead of on Guy's cartridge. */
typedef char pdna_rv_size_assert[(sizeof(PdnaRomVerify) == PDNA_RV_SIZE) ? 1 : -1];
typedef char pdna_rv_zone_assert[(sizeof(PdnaRvZone) == 8u) ? 1 : -1];

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

/* ================= the full-image region grid ==================================
 *
 * Streaming CRC32, so a 12.5 MB scan can be cut into pieces without buffering any of
 * it. `crc` is the RAW running state: start from PDNA_RV_CRC_INIT, feed it, finish with
 * pdna_rv_crc32_fin(). pdna_rv_crc32() is exactly begin/upd/fin with canon off.
 *
 * `canon` != 0 applies the PDNA_RV_PW* collapse, and then `data` and `len` MUST be
 * 4-aligned — a canonical word must never be split across two calls, and the grid walk
 * guarantees that by only ever cutting on zone boundaries, which the stamper aligns. */
#define PDNA_RV_CRC_INIT 0xFFFFFFFFu
#define pdna_rv_crc32_fin(c) ((c) ^ 0xFFFFFFFFu)

uint32_t pdna_rv_crc32_upd(const uint32_t* tab16, uint32_t crc,
                           const void* data, uint32_t len, int canon);
/* Feed `n` zero bytes — how a ZONE is accounted for without needing a zero buffer. */
uint32_t pdna_rv_crc32_zeros(const uint32_t* tab16, uint32_t crc, uint32_t n);

typedef enum {
  PDNA_RVG_PENDING  = 0,
  PDNA_RVG_OK       = 1,
  PDNA_RVG_RECOVER  = 2,   /* a re-read matched: the bus lied once                */
  PDNA_RVG_UNSTABLE = 3,   /* attempts disagreed, none matched: the BUS is lying  */
  PDNA_RVG_BAD      = 4,   /* every attempt agreed and differs: the IMAGE is wrong*/
  PDNA_RVG_SKIP     = 5    /* nothing verifiable in this region (all zone)        */
} PdnaRvRegionState;

typedef struct {
  uint32_t region_bytes;   /* 1u << region_shift                                  */
  int32_t  n_regions;
  int32_t  n_done;         /* regions visited so far                              */
  int32_t  n_ok, n_recovered, n_unstable, n_bad, n_skipped;
  int32_t  first_bad;      /* region index of the first BAD/UNSTABLE, else -1      */
  int32_t  first_bad_state;/* which of the two it was (so a stable BAD can displace
                            * a merely-flaky UNSTABLE as the reported one)         */
  uint32_t first_bad_off;  /* its file offset                                     */
  uint32_t first_bad_exp, first_bad_got;
  uint32_t bytes_verified; /* real image bytes actually fed to a CRC               */
  uint32_t bytes_zoned;    /* bytes deliberately read as zeroes (the blind spot)   */
} PdnaRvGrid;

/* Validate the grid half of the descriptor and zero *g. Returns 1 when a grid is
 * present AND believable, 0 otherwise (unstamped, v1, artless-with-no-grid, or a
 * descriptor whose own numbers do not agree — in which case NOTHING is measured, on
 * purpose: the descriptor lives in the image under suspicion). */
int pdna_rv_grid_init(const PdnaRomVerify* d, PdnaRvGrid* g);

/* 1 if region `i` has no verifiable bytes (stamped excl bit). */
int pdna_rv_region_excluded(const PdnaRomVerify* d, int32_t i);

/* CRC32 one region exactly the way the stamper did: zones as zeroes, IRQ words
 * canonicalised. *out_bytes (optional) receives the count of real bytes read. */
uint32_t pdna_rv_region_crc(const PdnaRomVerify* d, const unsigned char* rom,
                            int32_t i, const uint32_t* tab16, uint32_t* out_bytes);

/* CRC region `i` up to `tries` times, classify it, and fold the outcome into *g.
 * Returns a PdnaRvRegionState. `tries` is clamped to [1, PDNA_RV_GRID_TRIES]. */
int pdna_rv_region_check(const PdnaRomVerify* d, const unsigned char* rom,
                         int32_t i, const uint32_t* tab16, int tries, PdnaRvGrid* g);

#endif /* PDNA_ROMVER_H */
