/* Gen-1 / Gen-2 sprites out of the user's own Game Boy cartridge — see
 * rom_gbsprite.h for the design, the decomp citations and the addresses this
 * lands on. Pure C: no tonc, no FatFs, no GBA headers. */
#include "rom_gbsprite.h"
#include "gb_scanwin.h"

#include <string.h>

/* ------------------------------------------------------------------ basics */

#define GB_BANK        0x4000u
#define GB_WIN_LO      0x4000u          /* ROMX window: pointers live in       */
#define GB_WIN_HI      0x8000u          /* [0x4000,0x8000)                     */

#define G1_ROW         28u              /* pokered BASE_DATA_SIZE              */
#define G1_ROWS        150u             /* rows every Gen-1 ROM has            */
#define G1_PIC_SIZE    10u              /* BASE_PIC_SIZE                       */
#define G1_FRONT       11u              /* BASE_FRONTPIC                       */
#define G1_BACK        13u              /* BASE_BACKPIC                        */
#define G1_DEXORDER    190u             /* PokedexOrder length                 */
#define G1_SPECIES     151u

#define G2_ROW         32u              /* pokecrystal BASE_DATA_SIZE          */
#define G2_ROWS        251u
#define G2_PIC_SIZE    17u              /* BASE_PIC_SIZE                       */
#define G2_BETA_PICS   18u              /* the two unused NULL words at +18    */
#define G2_ENTRY       6u               /* dba_pics: 3 B front + 3 B back      */
#define G2_UNOWN_IDX   200u             /* dex 201 - 1: the FF FF FF hole      */
#define G2_BACK_TILE   6u               /* Gen-2 backs are always 6x6          */
#define G2_PAL_ENTRIES 252u             /* species 0..251                      */
#define G2_PAL_BYTES   (G2_PAL_ENTRIES * 8u)

/* The four DMG greys, as RGB15. A Red/Blue mon SHOULD look like a Red/Blue mon;
 * that is the provenance cue, not a missing feature. */
#define G1_GREY0       0x7FFFu          /* white  — background, transparent    */
#define G1_GREY1       0x56B5u          /* light  (21,21,21)                   */
#define G1_GREY2       0x294Au          /* dark   (10,10,10)                   */
#define G1_GREY3       0x0000u          /* black                               */

/* pokecrystal engine/gfx/color.asm LoadPalette_White_Col1_Col2_Black */
#define G2_PAL_WHITE   0x7FFFu          /* PALRGB_WHITE                        */
#define G2_PAL_BLACK   0x0000u

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

static int rd(const RomGbSprite* gs, uint32_t off, void* dst, uint32_t len) {
  if (!gs->read || len == 0) return 0;
  if (off >= gs->size || len > gs->size - off) return 0;
  return gs->read(gs->ctx, off, dst, len) ? 1 : 0;
}

/* file offset of a bank-relative pointer, or 0 if it leaves the image. Bank 0
 * never holds pics (it is the fixed ROM0 window), so 0 doubles as "no". */
static uint32_t bank_off(const RomGbSprite* gs, uint32_t bank, uint16_t addr) {
  if (bank == 0 || bank >= gs->banks) return 0;
  if (addr < GB_WIN_LO || addr >= GB_WIN_HI) return 0;
  uint32_t off = bank * GB_BANK + (uint32_t)(addr - GB_WIN_LO);
  return (off < gs->size) ? off : 0;
}

static uint32_t bank_end(uint32_t off) { return (off / GB_BANK + 1u) * GB_BANK; }

/* ------------------------------------------------------- header / identity */

/*
 * Every licensed Game Boy cartridge carries the same 48-byte boot logo at
 * 0x104..0x133 — the DMG boot ROM refuses to start otherwise. We do not embed
 * those bytes (they are Nintendo's); we embed a 32-bit FNV-1a of them, which is
 * a fingerprint rather than the data, and is enough to say "this really is a GB
 * ROM". A Gen-3 .gba, a .sav and a truncated image all fail it.
 */
#define GB_LOGO_OFF    0x104u
#define GB_LOGO_LEN    0x30u
#define GB_LOGO_FNV    0x016BAD3Fu     /* measured identically on Red, Yellow,
                                        * Gold and Crystal */

static uint32_t fnv1a(const uint8_t* p, uint32_t n, uint32_t h) {
  for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
  return h;
}

static int parse_header(RomGbSprite* gs) {
  uint8_t h[0x50];                                  /* 0x100..0x14F */
  if (!rd(gs, 0x100, h, sizeof h)) return 0;

  if (fnv1a(h + (GB_LOGO_OFF - 0x100), GB_LOGO_LEN, 0x811C9DC5u) != GB_LOGO_FNV) return 0;

  /* header checksum: x = x - byte - 1 over 0x134..0x14C (Pan Docs). The boot ROM
   * enforces it, so every real cartridge passes and nothing else has to. */
  uint8_t x = 0;
  for (uint32_t i = 0x134; i <= 0x14C; i++) x = (uint8_t)(x - h[i - 0x100] - 1u);
  if (x != h[0x14D - 0x100]) return 0;

  /* the size code must agree with the file we were handed: 32 KiB << code */
  uint8_t code = h[0x148 - 0x100];
  if (code > 8) return 0;
  if (gs->size != (0x8000u << code)) return 0;
  if (gs->size % GB_BANK) return 0;
  if (gs->size / GB_BANK > 255u) return 0;          /* banks are indexed in u8 */
  gs->banks = (uint8_t)(gs->size / GB_BANK);

  memcpy(gs->title, h + (0x134 - 0x100), 15);
  gs->title[15] = 0;
  for (int i = 0; i < 15; i++) {
    uint8_t c = (uint8_t)gs->title[i];
    if (c && (c < 0x20 || c > 0x7E)) { gs->title[i] = 0; break; }
  }
  gs->cgb             = h[0x143 - 0x100];
  gs->cart_type       = h[0x147 - 0x100];
  gs->rom_size_code   = code;
  gs->version         = h[0x14C - 0x100];
  gs->global_checksum = (uint16_t)((h[0x14E - 0x100] << 8) | h[0x14F - 0x100]);
  gs->id_hash         = fnv1a(h, sizeof h, 0x811C9DC5u);
  return 1;
}

/* -------------------------------------------------------- the window walker */

/*
 * One pass over the whole file offers every byte position to every pattern.
 * Candidates are only COLLECTED here — verifying them re-reads, which would
 * clobber the window — and the whole-ROM read is the expensive part, so all six
 * patterns (three Gen-1, three Gen-2) ride the same pass. That is the difference
 * between one 2 MB read and six of them on a card.
 */
#define SCAN_MAX_HITS 16

typedef int (*ScanCb)(const uint8_t* w);

/* BACKLOG #185 F2: an inline prefilter, tested BEFORE the indirect callback call --
 * scan_multi's inner loop runs once per byte position per job over a whole ROM (up
 * to 2M positions x 3-6 jobs), so a cheap `p[a_off] == a_val` byte compare that
 * rejects the overwhelming majority of positions is worth far more there than
 * inside the callback itself (which the compiler cannot inline across a function
 * pointer). Each job's gate is the CHEAPEST byte its own pattern already requires
 * at a fixed offset -- picked by reading the callback, not guessed -- so the gate
 * can never reject a position the real callback would have accepted: it is a
 * strict pre-check of one of the callback's own early conditions, not a new,
 * independent one.
 *
 * `two_step` is g1_dex_cb's own special case, TIGHTENED (coordinator direction,
 * BACKLOG #185 second pass): the original two_step gate was a permissive RANGE
 * check (both of the first two bytes merely "a valid dex number, 1..151"),
 * which the coordinator measured still let ~30% of positions through to the
 * 190-byte walk. PokedexOrder's own first two entries are fixed game-data
 * facts, not per-ROM addresses: VERIFIED (not trusted) against this exact
 * scanner's own located table on all four corpus ROMs (dump below) --
 * index 0 = 112 (Rhydon), index 1 = 115 (Kangaskhan), identical in Red AND
 * Yellow (a shared data table, unrelated to which cart it is):
 *
 *   Red.gb/Yellow.gb dex_order[0..2] = { 112, 115, 32, ... } (Nidoran-M, 32,
 *   confirms the coordinator's own claim too, though only the first two bytes
 *   are gated here -- see G1_DEXORDER_B0/B1 below).
 *
 * Trade-off, stated plainly: a hack that hand-edits BOTH of PokedexOrder's
 * first two entries to different values would no longer be found by this
 * job's gate (the shape-scan's whole point was surviving exactly that kind
 * of edit) -- accepted deliberately for the CPU win on every hack/unknown-
 * revision scan, which is what tightening this gate is FOR. g1_dex_cb itself
 * (the full 190-byte bijection check) is UNCHANGED and remains the real
 * verifier for whatever the gate lets through. */
typedef struct {
  ScanCb   cb;
  uint32_t look;                        /* bytes of context the cb reads      */
  uint32_t a_off;                       /* gate byte offset                   */
  uint8_t  a_val;                       /* gate byte value                    */
  uint8_t  a_val2;                      /* two_step: p[a_off+1] must equal this too */
  uint8_t  two_step;                    /* 1 = also check a_val2 at a_off+1   */
  uint8_t  job_id;                      /* J_G1_BS.. below, for the host-only
                                         * ROM_GBSPRITE_JOB_COUNTERS instrumentation */
  uint32_t off[SCAN_MAX_HITS];
  uint32_t n;                           /* > SCAN_MAX_HITS means "too many"   */
} ScanJob;

#define G1_DEXORDER_B0 112u   /* PokedexOrder[0]: Rhydon (national dex 112)      */
#define G1_DEXORDER_B1 115u   /* PokedexOrder[1]: Kangaskhan (national dex 115)  */

#ifdef ROM_GBSPRITE_JOB_COUNTERS
/* BACKLOG #185 Step 1: per-job CALLBACK invocation counts (not gate tests), so a
 * host benchmark can report exactly how much less work the F2 gate leaves for the
 * expensive indirect callback to do. Never defined by the GBA build (the Makefile
 * never passes this macro) -- six uint32_t only exist in a host test binary. */
uint32_t g_rgs_cb_calls[6];
#endif

/* Forward-only, sector-aligned reads through gb_scanwin.h -- see that header for
 * the measured reason (the old backward-hopping window cost 8,592 FatFs
 * transactions per 2 MB scan on the cart; this costs one per chunk). */
static int scan_multi(RomGbSprite* gs, ScanJob* jobs, uint32_t njobs) {
  uint8_t* w = gs->scratch;
  uint32_t look = 0;
  for (uint32_t j = 0; j < njobs; j++) { jobs[j].n = 0; if (jobs[j].look > look) look = jobs[j].look; }
  GbScanWin sw;
  if (!w || !gb_scanwin_init(&sw, gs->size, gs->scratch_len, look)) return 0;
  while (gb_scanwin_plan(&sw, w)) {
    if (!rd(gs, sw.rd_off, w + sw.rd_dst, sw.rd_len)) return 0;
    uint32_t cnt = gb_scanwin_filled(&sw);
    for (uint32_t i = sw.first; i < sw.first + cnt; i++) {
      const uint8_t* p = w + i;
      for (uint32_t j = 0; j < njobs; j++) {
        if (jobs[j].two_step) {
          if (p[jobs[j].a_off] != jobs[j].a_val || p[jobs[j].a_off + 1] != jobs[j].a_val2) continue;
        } else if (p[jobs[j].a_off] != jobs[j].a_val) continue;
#ifdef ROM_GBSPRITE_JOB_COUNTERS
        g_rgs_cb_calls[jobs[j].job_id]++;
#endif
        if (!jobs[j].cb(p)) continue;
        if (jobs[j].n < SCAN_MAX_HITS) jobs[j].off[jobs[j].n] = sw.base + i;
        jobs[j].n++;
      }
    }
  }
  return 1;
}

/* ------------------------------------------------------------ Gen-1 tables */

/* Prefilter: 10 rows of 28 whose first byte counts 1,2,3,… and whose pic-size
 * byte is one of the three legal square dimensions. */
static int g1_bs_cb(const uint8_t* w) {
  for (uint32_t i = 0; i < 10; i++) {
    const uint8_t* r = w + i * G1_ROW;
    if (r[0] != i + 1) return 0;
    uint8_t d = r[G1_PIC_SIZE];
    if (d != 0x55 && d != 0x66 && d != 0x77) return 0;
    uint16_t f = rd16(r + G1_FRONT), b = rd16(r + G1_BACK);
    if (f < GB_WIN_LO || f >= GB_WIN_HI || b < GB_WIN_LO || b >= GB_WIN_HI) return 0;
  }
  return 1;
}

static int g1_bs_verify(RomGbSprite* gs, uint32_t off) {
  uint8_t buf[G1_ROW];
  for (uint32_t i = 0; i < G1_ROWS; i++) {
    if (!rd(gs, off + i * G1_ROW, buf, G1_ROW)) return 0;
    if (buf[0] != i + 1) return 0;
    uint8_t d = buf[G1_PIC_SIZE];
    if (d != 0x55 && d != 0x66 && d != 0x77) return 0;
    uint16_t f = rd16(buf + G1_FRONT), b = rd16(buf + G1_BACK);
    if (f < GB_WIN_LO || f >= GB_WIN_HI || b < GB_WIN_LO || b >= GB_WIN_HI) return 0;
  }
  return 1;
}

/* PokedexOrder: 190 bytes, 151 non-zero forming a bijection onto 1..151, and 39
 * MissingNo holes. BACKLOG #185 F2: scan_multi's own two-byte gate (p[0] and
 * p[1] both in [1,151]) already rejects almost every position before this is
 * even called, so the ONE remaining pass below does the range check, the
 * bijection check and the hole count together instead of three separate passes
 * over the same 190 bytes -- `seen` is memset exactly once, and a running zero
 * count lets a position that can no longer possibly reach 39 holes (or 151
 * distinct species) bail before the full 190-byte walk finishes. Bailing on the
 * first out-of-range byte or the first duplicate is what keeps a per-byte scan
 * of a 1 MB ROM cheap on whatever positions the gate still lets through. */
static int g1_dex_cb(const uint8_t* w) {
  uint8_t seen[G1_SPECIES + 1];
  memset(seen, 0, sizeof seen);
  uint32_t nz = 0, zeros = 0;
  const uint32_t max_zeros = G1_DEXORDER - G1_SPECIES;      /* 39 */
  for (uint32_t i = 0; i < G1_DEXORDER; i++) {
    uint8_t v = w[i];
    if (v > G1_SPECIES) return 0;
    if (!v) { if (++zeros > max_zeros) return 0; continue; }
    if (seen[v]) return 0;
    seen[v] = 1; nz++;
  }
  return nz == G1_SPECIES && zeros == max_zeros;
}

/* Red/Blue keep Mew's record alone in the bank that holds its pics
 * (data/pokemon/mew.asm — "a kind of prank"). Accept a 28-byte row for dex 151
 * only when its own front pointer resolves, inside that same bank, to a byte
 * equal to its dimension field. */
static int g1_mew_cb(const uint8_t* w) {
  if (w[0] != G1_SPECIES) return 0;
  uint8_t d = w[G1_PIC_SIZE];
  if (d != 0x55 && d != 0x66 && d != 0x77) return 0;
  uint16_t f = rd16(w + G1_FRONT), b = rd16(w + G1_BACK);
  if (f < GB_WIN_LO || f >= GB_WIN_HI || b < GB_WIN_LO || b >= GB_WIN_HI) return 0;
  for (uint32_t i = 1; i <= 5; i++) if (!w[i]) return 0;   /* real base stats  */
  if (!w[8] || !w[9]) return 0;                            /* catch rate / exp */
  return 1;
}

static int g1_mew_verify(RomGbSprite* gs, uint32_t off, uint8_t* out_bank) {
  uint8_t row[G1_ROW], probe;
  if (!rd(gs, off, row, G1_ROW)) return 0;
  uint32_t bank = off / GB_BANK;
  uint32_t p = bank_off(gs, bank, rd16(row + G1_FRONT));
  if (!p || !rd(gs, p, &probe, 1) || probe != row[G1_PIC_SIZE]) return 0;
  *out_bank = (uint8_t)bank;
  return 1;
}

/* ------------------------------------------------------------ Gen-2 tables */

static int g2_bd_cb(const uint8_t* w) {
  for (uint32_t i = 0; i < 10; i++) {
    const uint8_t* r = w + i * G2_ROW;
    if (r[0] != i + 1) return 0;
    /* the two unused "beta pic" words are NULL in every row of every Gen-2 ROM */
    if (r[G2_BETA_PICS] || r[G2_BETA_PICS + 1] || r[G2_BETA_PICS + 2] || r[G2_BETA_PICS + 3])
      return 0;
    uint8_t sz = (uint8_t)(r[G2_PIC_SIZE] & 0x0Fu);
    if (sz < 4 || sz > 7) return 0;
  }
  return 1;
}

static int g2_bd_verify(RomGbSprite* gs, uint32_t off) {
  uint8_t r[G2_ROW];
  for (uint32_t i = 0; i < G2_ROWS; i++) {
    if (!rd(gs, off + i * G2_ROW, r, G2_ROW)) return 0;
    if (r[0] != i + 1) return 0;
    if (r[G2_BETA_PICS] || r[G2_BETA_PICS + 1] || r[G2_BETA_PICS + 2] || r[G2_BETA_PICS + 3])
      return 0;
    uint8_t sz = (uint8_t)(r[G2_PIC_SIZE] & 0x0Fu);
    if (sz < 4 || sz > 7) return 0;
  }
  return 1;
}

/*
 * A 3-byte "bank + pointer" record is a common shape — a first cut that only
 * asked for sixteen consecutive pointer-shaped entries found 1 643 candidates in
 * Crystal, all of them in one unrelated dba table. What makes THIS table
 * unmistakable is its hole: index 200 is FF FF FF FF FF FF because Unown's pics
 * live in a separate table. Requiring the hole in exactly that place, with real
 * entries either side of it, cuts it to one.
 */
#define G2_PP_LOOK  (201u * G2_ENTRY)          /* through the Unown hole        */
/* ROM_GBSPRITE_SCRATCH_MIN must hold the carry for the longest pattern (this one)
 * plus one aligned chunk -- gb_scanwin.h's rule. Pinned so a longer pattern fails
 * the build instead of making every open() silently refuse at run time. */
_Static_assert(((G2_PP_LOOK - 1u) + GB_SCANWIN_ALIGN - 1u) / GB_SCANWIN_ALIGN * GB_SCANWIN_ALIGN
               + GB_SCANWIN_ALIGN <= ROM_GBSPRITE_SCRATCH_MIN,
               "ROM_GBSPRITE_SCRATCH_MIN too small for the scan window");

static int g2_pp_cb(const uint8_t* w) {
  const uint8_t* hole = w + G2_UNOWN_IDX * G2_ENTRY;
  for (uint32_t k = 0; k < G2_ENTRY; k++) if (hole[k] != 0xFF) return 0;
  for (uint32_t i = 0; i < 16; i++) {
    uint16_t f = rd16(w + i * G2_ENTRY + 1), b = rd16(w + i * G2_ENTRY + 4);
    if (f < GB_WIN_LO || f >= GB_WIN_HI || b < GB_WIN_LO || b >= GB_WIN_HI) return 0;
  }
  for (uint32_t i = 195; i < 200; i++) {       /* real entries before the hole  */
    uint16_t f = rd16(w + i * G2_ENTRY + 1), b = rd16(w + i * G2_ENTRY + 4);
    if (f < GB_WIN_LO || f >= GB_WIN_HI || b < GB_WIN_LO || b >= GB_WIN_HI) return 0;
  }
  return 1;
}

/* The table must be pointer-shaped everywhere EXCEPT index 200, which is the
 * FF FF FF FF FF FF hole where Unown would be (data/pokemon/pic_pointers.asm:
 * `dba_pics ; Unown pics have their own table`). One hole, in that one place. */
static int g2_pp_verify(RomGbSprite* gs, uint32_t off, uint8_t* lo, uint8_t* hi) {
  uint8_t e[G2_ENTRY];
  uint8_t mn = 0xFF, mx = 0;
  for (uint32_t i = 0; i < G2_ROWS; i++) {
    if (!rd(gs, off + i * G2_ENTRY, e, G2_ENTRY)) return 0;
    if (i == G2_UNOWN_IDX) {
      for (uint32_t k = 0; k < G2_ENTRY; k++) if (e[k] != 0xFF) return 0;
      continue;
    }
    uint16_t f = rd16(e + 1), b = rd16(e + 4);
    if (f < GB_WIN_LO || f >= GB_WIN_HI || b < GB_WIN_LO || b >= GB_WIN_HI) return 0;
    if (e[0] < mn) mn = e[0];
    if (e[3] < mn) mn = e[3];
    if (e[0] > mx) mx = e[0];
    if (e[3] > mx) mx = e[3];
  }
  if (mx < mn || (uint32_t)(mx - mn) >= ROM_GBSPRITE_BANKMAP) return 0;
  *lo = mn; *hi = mx;
  return 1;
}

/* _GetMonPalettePointer: ld l,a / ld h,0 / add hl,hl x3 / ld bc,nn / add hl,bc / ret */
static const uint8_t G2_PAL_SIG[] = { 0x6F, 0x26, 0x00, 0x29, 0x29, 0x29, 0x01 };
#define G2_PAL_SIG_LEN 11u                          /* + lo hi + 09 C9         */

static int g2_pal_cb(const uint8_t* w) {
  if (memcmp(w, G2_PAL_SIG, sizeof G2_PAL_SIG) != 0) return 0;
  if (w[9] != 0x09 || w[10] != 0xC9) return 0;          /* add hl,bc / ret     */
  uint16_t a = rd16(w + 7);
  return (a >= GB_WIN_LO && a < GB_WIN_HI);
}

/* 252 entries x 4 GB colours: bit 15 clear, never pure white or black (the table
 * stores only the two MIDDLE colours of each palette), and entry 0's normal pair
 * equals its shiny pair. That is what kills the OTHER signature hit — there are
 * two in each of Gold and Crystal, and exactly one survives this. */
static int g2_pal_verify(RomGbSprite* gs, uint32_t off) {
  if (gs->scratch_len < G2_PAL_BYTES) return 0;
  if (!rd(gs, off, gs->scratch, G2_PAL_BYTES)) return 0;
  const uint8_t* p = gs->scratch;
  if (rd16(p) != rd16(p + 4) || rd16(p + 2) != rd16(p + 6)) return 0;
  for (uint32_t i = 0; i < G2_PAL_BYTES; i += 2) {
    uint16_t c = rd16(p + i);
    if (c & 0x8000u) return 0;
    if (c == 0 || c == 0x7FFFu) return 0;
  }
  return 1;
}

/* ------------------------------------------------------------------- open */

enum { J_G1_BS = 0, J_G1_DEX, J_G1_MEW, J_G2_BD, J_G2_PP, J_G2_PAL, J_COUNT };

static void job_g1_bs(ScanJob* j)  { memset(j, 0, sizeof *j); j->cb = g1_bs_cb;  j->look = 10 * G1_ROW;
                                     j->a_off = 0; j->a_val = 1; j->job_id = J_G1_BS; }
static void job_g1_dex(ScanJob* j) { memset(j, 0, sizeof *j); j->cb = g1_dex_cb; j->look = G1_DEXORDER;
                                     j->a_off = 0; j->a_val = (uint8_t)G1_DEXORDER_B0;
                                     j->a_val2 = (uint8_t)G1_DEXORDER_B1; j->two_step = 1;
                                     j->job_id = J_G1_DEX; }
static void job_g1_mew(ScanJob* j) { memset(j, 0, sizeof *j); j->cb = g1_mew_cb; j->look = G1_ROW;
                                     j->a_off = 0; j->a_val = (uint8_t)G1_SPECIES; j->job_id = J_G1_MEW; }
static void job_g2_bd(ScanJob* j)  { memset(j, 0, sizeof *j); j->cb = g2_bd_cb;  j->look = 10 * G2_ROW;
                                     j->a_off = 0; j->a_val = 1; j->job_id = J_G2_BD; }
static void job_g2_pp(ScanJob* j)  { memset(j, 0, sizeof *j); j->cb = g2_pp_cb;  j->look = G2_PP_LOOK;
                                     /* the Unown hole's own first byte -- the cheapest byte g2_pp_cb
                                      * itself demands first (its very first check). */
                                     j->a_off = G2_UNOWN_IDX * G2_ENTRY; j->a_val = 0xFF; j->job_id = J_G2_PP; }
static void job_g2_pal(ScanJob* j) { memset(j, 0, sizeof *j); j->cb = g2_pal_cb; j->look = G2_PAL_SIG_LEN;
                                     j->a_off = 0; j->a_val = G2_PAL_SIG[0]; j->job_id = J_G2_PAL; }

/* Gen-1 ROMs are 1 MiB (32 KiB << size-code 5); Gen-2 ROMs are 2 MiB (size-code
 * 6). Size ALONE, not cgb: the brief this fix comes from (BACKLOG #185) assumed
 * cgb 0x00 for every Gen-1 ROM, but the real corpus disagrees -- measured
 * directly off Guy's own dumps (xxd -s 0x143 -l 1), Yellow.gb (Gen 1) is cgb=0x80,
 * the SAME "GBC-enhanced" value Gold.gbc (Gen 2) uses:
 *
 *     Red.gb      1,048,576 B  cgb=0x00
 *     Yellow.gb   1,048,576 B  cgb=0x80   <- would have been wrongly REFUSED by
 *                                              a cgb==0x00 check
 *     Gold.gbc    2,097,152 B  cgb=0x80
 *     Crystal.gbc 2,097,152 B  cgb=0xC0
 *
 * cgb cannot discriminate Gen 1 from Gen 2 (0x80 appears on both sides), so this
 * checks size only -- verified sufficient across all four corpus ROMs (1 MiB vs
 * 2 MiB, no overlap) and structurally sound: Pan Docs' size-code byte 0x148 is
 * exactly the same "how many banks does the boot ROM expect" byte parse_header()
 * already turned into gs->size, so a wrong-generation ROM of the SAME size as
 * the requested generation still fails the real scan/verify a moment later --
 * this check's job is only to reject the CHEAP, common case (a 2 MB ROM handed
 * to the Gen-1 slot) before spending a single scan byte on it, not to be the
 * only line of defense. Checked against the already-parsed header BEFORE a
 * restricted scan runs -- BACKLOG #185 F1's "must not silently succeed". */
static int header_matches_gen(const RomGbSprite* gs, uint8_t gen_hint) {
  if (gen_hint == GB_ROM_GEN1) return gs->size == 0x100000u;
  if (gen_hint == GB_ROM_GEN2) return gs->size == 0x200000u;
  return 1;                                        /* GB_ROM_NONE: no constraint */
}

/* Gen-1 identification from three already-scanned jobs (base stats, dex order,
 * Mew). Returns 1 and fills gs (gen = GB_ROM_GEN1) on success, 0 otherwise --
 * gs->base_stats is left at 0 on failure so a caller that falls through to
 * Gen-2 never sees a stale Gen-1 offset. */
static int identify_g1(RomGbSprite* gs, const ScanJob* bs, const ScanJob* dex, const ScanJob* mew) {
  uint32_t found = 0, off = 0;
  if (bs->n && bs->n <= SCAN_MAX_HITS) {
    for (uint32_t i = 0; i < bs->n; i++)
      if (g1_bs_verify(gs, bs->off[i])) { found++; off = bs->off[i]; }
  }
  if (found != 1 || dex->n != 1) { gs->base_stats = 0; return 0; }
  gs->base_stats = off;
  if (!rd(gs, dex->off[0], gs->dex_order, G1_DEXORDER)) { gs->base_stats = 0; return 0; }

  /* Is dex 151 an ordinary 151st row (Yellow) or a lone record (Red/Blue)? */
  uint8_t row[G1_ROW];
  if (rd(gs, gs->base_stats + G1_ROWS * G1_ROW, row, G1_ROW) && row[0] == G1_SPECIES) {
    gs->mew_stats = 0; gs->mew_bank = 0;          /* the ladder covers it     */
  } else {
    if (mew->n > SCAN_MAX_HITS) { gs->base_stats = 0; return 0; }
    uint32_t mfound = 0;
    for (uint32_t i = 0; i < mew->n; i++) {
      uint8_t bank;
      if (g1_mew_verify(gs, mew->off[i], &bank)) {
        mfound++; gs->mew_stats = mew->off[i]; gs->mew_bank = bank;
      }
    }
    if (mfound != 1) { gs->base_stats = 0; return 0; }
  }
  gs->gen = GB_ROM_GEN1;
  return 1;
}

/* Gen-2 identification from three already-scanned jobs (base data, pic
 * pointers, palettes). Same success/failure contract as identify_g1(). */
static int identify_g2(RomGbSprite* gs, const ScanJob* bd, const ScanJob* pp, const ScanJob* pal) {
  uint32_t found = 0, off = 0;
  if (bd->n > SCAN_MAX_HITS) return 0;
  for (uint32_t i = 0; i < bd->n; i++)
    if (g2_bd_verify(gs, bd->off[i])) { found++; off = bd->off[i]; }
  if (found != 1) return 0;
  gs->base_data = off;

  found = 0;
  uint8_t lo = 0, hi = 0;
  if (pp->n > SCAN_MAX_HITS) return 0;
  for (uint32_t i = 0; i < pp->n; i++) {
    uint8_t a, b;
    if (g2_pp_verify(gs, pp->off[i], &a, &b)) { found++; off = pp->off[i]; lo = a; hi = b; }
  }
  if (found != 1) return 0;
  gs->pic_ptrs = off;
  gs->stored_lo = lo;

  /* PicsBanks[0] is the bank the pointer table itself lives in: "Pic Pointers"
   * and "Pics 1" share a bank (pokecrystal layout.link). Everything else starts
   * from that constant offset and is CONFIRMED, or repaired, on first use. */
  uint32_t tbl_bank = gs->pic_ptrs / GB_BANK;
  memset(gs->bank_map, 0, sizeof gs->bank_map);
  gs->bank_ok = 0;
  for (uint32_t i = 0; (uint32_t)lo + i <= (uint32_t)hi; i++) {
    uint32_t b = tbl_bank + i;
    gs->bank_map[i] = (b < gs->banks) ? (uint8_t)b : 0;
  }

  found = 0;
  if (pal->n > SCAN_MAX_HITS) return 0;
  for (uint32_t i = 0; i < pal->n; i++) {
    uint8_t sig[G2_PAL_SIG_LEN];
    if (!rd(gs, pal->off[i], sig, sizeof sig)) continue;
    uint32_t p = bank_off(gs, pal->off[i] / GB_BANK, rd16(sig + 7));
    if (!p || !g2_pal_verify(gs, p)) continue;
    found++; off = p;
  }
  if (found != 1) return 0;
  gs->palettes = off;

  gs->unown_ptrs = 0;              /* located on the first Unown fetch */
  gs->gen = GB_ROM_GEN2;
  return 1;
}

/* BACKLOG #185 F1: `gen_hint` narrows the scan to ONE generation's three jobs
 * (half the per-position callback/gate work of the original six-job pass) when
 * the caller already knows which generation it is registering/fetching.
 * GB_ROM_NONE keeps the original "try Gen 1, then Gen 2" behaviour every
 * gen-less caller relies on. */
static int locate(RomGbSprite* gs, uint8_t gen_hint) {
  if (gen_hint != GB_ROM_NONE && !header_matches_gen(gs, gen_hint)) {
    /* BACKLOG #185 D1 (review fix): boot logo + header checksum already
     * passed (parse_header() refused otherwise) -- this really is a Game Boy
     * ROM, just of the OTHER size than the hint asked for. Name it, don't
     * just say "not a Game Boy image": a Gen-2 ROM registered under the
     * Gen-1 hint (Gold in the Gen-1 slot, the most common user mistake) must
     * come back as "that's a Gen 2 ROM", not the generic BAD_ROM refusal a
     * genuinely unlocatable image gets. */
    gs->gen = (gs->size == 0x200000u) ? GB_ROM_GEN2 : (gs->size == 0x100000u) ? GB_ROM_GEN1 : GB_ROM_NONE;
    return 0;
  }

  if (gen_hint == GB_ROM_GEN1) {
    ScanJob jobs[3];
    job_g1_bs(&jobs[0]); job_g1_dex(&jobs[1]); job_g1_mew(&jobs[2]);
    if (!scan_multi(gs, jobs, 3)) return 0;
    return identify_g1(gs, &jobs[0], &jobs[1], &jobs[2]);
  }
  if (gen_hint == GB_ROM_GEN2) {
    ScanJob jobs[3];
    job_g2_bd(&jobs[0]); job_g2_pp(&jobs[1]); job_g2_pal(&jobs[2]);
    if (!scan_multi(gs, jobs, 3)) return 0;
    return identify_g2(gs, &jobs[0], &jobs[1], &jobs[2]);
  }

  ScanJob jobs[J_COUNT];
  job_g1_bs(&jobs[J_G1_BS]);   job_g1_dex(&jobs[J_G1_DEX]); job_g1_mew(&jobs[J_G1_MEW]);
  job_g2_bd(&jobs[J_G2_BD]);   job_g2_pp(&jobs[J_G2_PP]);   job_g2_pal(&jobs[J_G2_PAL]);
  if (!scan_multi(gs, jobs, J_COUNT)) return 0;
  if (identify_g1(gs, &jobs[J_G1_BS], &jobs[J_G1_DEX], &jobs[J_G1_MEW])) return 1;
  return identify_g2(gs, &jobs[J_G2_BD], &jobs[J_G2_PP], &jobs[J_G2_PAL]);
}

/* BACKLOG #185 F5 / open_loc()'s own file-cache validation: attempt to trust a
 * CANDIDATE RomGbSpriteLoc -- from the .loc cache file (open_loc()) or the
 * known-ROM fast-path table below (open()) -- by re-running the SAME
 * independent *_verify reads a fresh scan's own candidates get. A wrong,
 * stale, or foreign candidate is REJECTED here, not used, so neither source
 * can ever silently hand back the wrong offsets: a hand-typed or
 * scanner-drifted known-ROM entry, or a .loc file swapped onto a different
 * ROM, both degrade to "fall through to a full scan", never a wrong picture.
 * Returns 1 (gs fully populated, gs->ok=1, gs->gen set) or 0 (gs is
 * UNCHANGED beyond whatever a rejected candidate's own verify reads already
 * touched -- the caller must fall through to a full scan). */
static int try_loc(RomGbSprite* gs, const RomGbSpriteLoc* loc, uint8_t gen_hint) {
  if (!loc) return 0;
  if (gen_hint != GB_ROM_NONE && loc->gen != gen_hint) return 0;
  if (loc->gen == GB_ROM_GEN1 && g1_bs_verify(gs, loc->base_stats)) {
    gs->base_stats = loc->base_stats;
    memcpy(gs->dex_order, loc->dex_order, sizeof gs->dex_order);
    uint8_t seen[G1_SPECIES + 1];
    memset(seen, 0, sizeof seen);
    uint32_t nz = 0, dup = 0;
    for (uint32_t i = 0; i < G1_DEXORDER; i++) {
      uint8_t v = gs->dex_order[i];
      if (!v) continue;
      if (v > G1_SPECIES || seen[v]) { dup = 1; break; }
      seen[v] = 1; nz++;
    }
    if (!dup && nz == G1_SPECIES) {
      gs->mew_stats = loc->mew_stats; gs->mew_bank = loc->mew_bank;
      int mew_ok = 1;
      if (gs->mew_stats) {
        uint8_t bank;
        mew_ok = g1_mew_verify(gs, gs->mew_stats, &bank) && bank == gs->mew_bank;
      }
      if (mew_ok) { gs->gen = GB_ROM_GEN1; gs->ok = 1; return 1; }
    }
  } else if (loc->gen == GB_ROM_GEN2) {
    uint8_t lo, hi;
    if (g2_bd_verify(gs, loc->base_data) && g2_pp_verify(gs, loc->pic_ptrs, &lo, &hi) &&
        lo == loc->stored_lo && g2_pal_verify(gs, loc->palettes)) {
      gs->base_data  = loc->base_data;
      gs->pic_ptrs   = loc->pic_ptrs;
      gs->palettes   = loc->palettes;
      gs->stored_lo  = loc->stored_lo;
      gs->unown_ptrs = 0;                 /* re-earned, like the bank map    */
      memcpy(gs->bank_map, loc->bank_map, sizeof gs->bank_map);
      gs->bank_ok = 0;
      gs->gen = GB_ROM_GEN2; gs->ok = 1; return 1;
    }
  }
  return 0;
}

/* BACKLOG #185 F5: a static const table of (title, version, global_checksum)
 * -> the located offsets, populated ONLY from what THIS scanner finds on the
 * four ROMs Guy actually owns (tests/host_gbscan_test.c's part_b_f5_one(),
 * BACKLOG #185 D2, re-derives every entry from the live scanner on every run
 * and asserts byte-equality, so the table can never drift from it -- see
 * that function for the generator this table was pasted from). Blue/Silver
 * (no corpus ROM) simply have no
 * entry -- they fall straight through to the full scan below, same as any
 * hack or unknown revision. A HIT here still runs the exact same try_loc()
 * verify-before-use gate as a .loc cache hit -- this table is a candidate,
 * never a trusted source. */
typedef struct {
  char           title[16];
  uint8_t        version;
  uint16_t       global_checksum;
  RomGbSpriteLoc loc;
} RomGbSpriteKnown;

#include "rom_gbsprite_known.h"

static const RomGbSpriteLoc* known_rom_lookup(const char* title, uint8_t version,
                                              uint16_t global_checksum) {
  for (uint32_t i = 0; i < sizeof k_known_gbsprite / sizeof k_known_gbsprite[0]; i++) {
    const RomGbSpriteKnown* k = &k_known_gbsprite[i];
    if (k->version == version && k->global_checksum == global_checksum &&
        memcmp(k->title, title, sizeof k->title) == 0)
      return &k->loc;
  }
  return 0;
}

int rom_gbsprite_open(RomGbSprite* gs, GbReadFn read, void* ctx, uint32_t size,
                      uint8_t* scratch, uint32_t scratch_len, uint8_t gen_hint) {
  if (!gs) return 0;
  memset(gs, 0, sizeof *gs);
  gs->read = read; gs->ctx = ctx; gs->size = size;
  gs->scratch = scratch; gs->scratch_len = scratch_len;
  if (!read || !scratch || scratch_len < ROM_GBSPRITE_SCRATCH_MIN) return 0;
  if (!parse_header(gs)) return 0;
  /* BACKLOG #185 F5: a known ROM's own table entry, verified before use, skips
   * the whole-ROM scan entirely -- checked before locate() so a hit costs
   * only the handful of *_verify reads, not one scan byte. */
  if (try_loc(gs, known_rom_lookup(gs->title, gs->version, gs->global_checksum), gen_hint))
    return 1;
  /* Which generation is decided by what is IN the ROM, never by the title, so
   * Blue, Silver and localised builds work the same way -- unless the caller
   * already knows (gen_hint != GB_ROM_NONE), in which case locate() restricts
   * itself to that generation's jobs and fails closed on a header mismatch
   * (BACKLOG #185 F1). */
  if (!locate(gs, gen_hint)) return 0;   /* gs->gen: NONE, or the header-implied gen on a hint mismatch */
  gs->ok = 1;
  return 1;
}

void rom_gbsprite_save_loc(const RomGbSprite* gs, RomGbSpriteLoc* out) {
  if (!gs || !out) return;
  memset(out, 0, sizeof *out);
  out->id_hash    = gs->id_hash;
  out->size       = gs->size;
  out->gen        = (uint8_t)gs->gen;
  out->mew_bank   = gs->mew_bank;
  out->stored_lo  = gs->stored_lo;
  out->base_stats = gs->base_stats;
  out->mew_stats  = gs->mew_stats;
  out->base_data  = gs->base_data;
  out->pic_ptrs   = gs->pic_ptrs;
  out->unown_ptrs = gs->unown_ptrs;
  out->palettes   = gs->palettes;
  memcpy(out->dex_order, gs->dex_order, sizeof out->dex_order);
  memcpy(out->bank_map,  gs->bank_map,  sizeof out->bank_map);
}

int rom_gbsprite_open_loc(RomGbSprite* gs, GbReadFn read, void* ctx, uint32_t size,
                          uint8_t* scratch, uint32_t scratch_len,
                          const RomGbSpriteLoc* loc, uint8_t gen_hint) {
  if (!gs) return 0;
  memset(gs, 0, sizeof *gs);
  gs->read = read; gs->ctx = ctx; gs->size = size;
  gs->scratch = scratch; gs->scratch_len = scratch_len;
  if (!read || !scratch || scratch_len < ROM_GBSPRITE_SCRATCH_MIN) return 0;
  if (!parse_header(gs)) return 0;

  /* A cache is a hint, never a source of truth: it has to survive the same
   * verification the scan's own candidates do (try_loc(), shared with F5's
   * known-ROM table below -- same gate, two candidate sources). bank_ok is
   * deliberately NOT restored — every stored bank byte re-earns its mapping
   * on first use. BACKLOG #185 F1: a cache whose OWN recorded gen disagrees
   * with a non-NONE gen_hint is not even consulted -- it falls straight to
   * the gen-restricted rom_gbsprite_open() below (which itself now also
   * tries the F5 known-ROM table before a full scan), exactly like a
   * wrong-gen scan. Without this a foreign-gen cache could validate (its
   * fields are genuinely self-consistent) and silently hand back the OTHER
   * generation's tables under a hint that promised otherwise. */
  if (loc && loc->id_hash == gs->id_hash && loc->size == size &&
      try_loc(gs, loc, gen_hint))
    return 1;
  return rom_gbsprite_open(gs, read, ctx, size, scratch, scratch_len, gen_hint);
}

/* ------------------------------------------------------------------ Gen 1 */

/* dex -> internal index, from the ROM's own PokedexOrder. 0 = not a species. */
static uint8_t g1_internal(const RomGbSprite* gs, uint16_t dex) {
  for (uint32_t i = 0; i < G1_DEXORDER; i++)
    if (gs->dex_order[i] == dex) return (uint8_t)(i + 1);
  return 0;
}

/* pokered home/pics.asm UncompressMonSprite. Pinned because it is CODE — and
 * checked at every use against the pic's own dimension byte. */
static uint32_t g1_bank(const RomGbSprite* gs, uint8_t idx) {
  if (gs->mew_stats && idx == 0x15) return gs->mew_bank;   /* Red/Blue only    */
  if (idx < 0x1F) return 0x09;
  if (idx < 0x4A) return 0x0A;
  if (idx < 0x74) return 0x0B;
  if (idx < 0x99) return 0x0C;
  return 0x0D;
}

static int g1_row(const RomGbSprite* gs, uint16_t dex, uint8_t* row) {
  if (dex < 1 || dex > G1_SPECIES) return 0;
  uint32_t off = (dex == G1_SPECIES && gs->mew_stats)
                   ? gs->mew_stats
                   : gs->base_stats + (uint32_t)(dex - 1) * G1_ROW;
  if (!rd(gs, off, row, G1_ROW)) return 0;
  return row[0] == (uint8_t)dex;
}

static int g1_pic(RomGbSprite* gs, RomGbSide side, uint16_t dex,
                  uint8_t* px, uint8_t* work, RomGbPic* info) {
  uint8_t row[G1_ROW];
  if (!g1_row(gs, dex, row)) return 0;
  uint8_t idx = g1_internal(gs, dex);
  if (!idx) return 0;
  uint16_t ptr = rd16(row + (side == ROM_GBSPRITE_FRONT ? G1_FRONT : G1_BACK));
  uint32_t off = bank_off(gs, g1_bank(gs, idx), ptr);
  if (!off) return 0;

  GbSpriteInfo gi;
  if (gb_sprite_gen1_buf(px, work, gs->read, gs->ctx, off, &gi) != GB_SPRITE_OK) return 0;
  if (!gi.wt || !gi.ht || gi.wt > 7 || gi.ht > 7) return 0;

  /* THE ASSERT THAT MATTERS. A Gen-1 pic carries its own geometry byte, and for
   * a FRONT pic it must equal the base-stats one. A wrong sprite bank hands back
   * a perfectly well-formed picture of the WRONG POKEMON — this is what turns
   * that into an error instead. Backs are not in the base stats (the game reads
   * their geometry from the stream too), so they only get the bank-bounds check. */
  if (side == ROM_GBSPRITE_FRONT) {
    uint8_t dim = row[G1_PIC_SIZE];
    if (gi.wt != (uint8_t)(dim >> 4) || gi.ht != (uint8_t)(dim & 0x0Fu)) return 0;
  }
  /* and the stream must have ended inside its own 16 KiB bank */
  if (gi.consumed == 0 || gi.consumed > bank_end(off) - off) return 0;

  info->gen = 1; info->wt = gi.wt; info->ht = gi.ht;
  info->w = gi.w; info->h = gi.h;
  info->pixels = (uint32_t)gi.w * gi.h;
  info->consumed = gi.consumed;
  return 1;
}

/* ------------------------------------------------------------------ Gen 2 */

static int g2_size(const RomGbSprite* gs, uint16_t dex, uint8_t* out) {
  uint8_t r[G2_ROW];
  if (dex < 1 || dex > G2_ROWS) return 0;
  if (!rd(gs, gs->base_data + (uint32_t)(dex - 1) * G2_ROW, r, G2_ROW)) return 0;
  if (r[0] != (uint8_t)dex) return 0;
  uint8_t sz = (uint8_t)(r[G2_PIC_SIZE] & 0x0Fu);
  if (sz < 4 || sz > 7) return 0;
  *out = sz;
  return 1;
}

/*
 * "Is this actually a Pokemon?"
 *
 * The obvious test — did it decode, did it stay in its bank, is it more than one
 * colour — is nowhere near enough, and that is measured, not assumed. Gold's bank
 * 0x2C is 16 KiB of 0x00 and EVERY address in it decompresses happily; banks 0x2A
 * and 0x2B are low-entropy data where all 26 of a stored value's pictures decode.
 * Three different banks passed the obvious test 26/26 for Gold's stored 0x13.
 *
 * What separates a real picture from plausible rubbish is its FRAME. A Gen-2
 * front pic is a mon standing in a box of background. Measured over all 552 real
 * fronts in Gold and Crystal (250 species + 26 Unown letters each): every one has
 * all four corners on background except Golbat and Zapdos, and the worst edge is
 * 73% background (95% for those two). Backs are cropped and routinely touch the
 * frame, so they only get the edge bound; the worst real back is 67%.
 *
 * Thresholds are therefore 55% with corners / 85% without — 12 to 18 points of
 * margin on every real picture in both carts, which matters because the TRUE bank
 * has to pass EVERY peer. Rubbish clears that bar about 39% of the time at worst
 * (Gold bank 0x2A), so twelve pictures in a row is ~1e-5 per bank. And if it ever
 * did happen twice the resolve reports AMBIGUOUS and shows nothing, rather than
 * showing the wrong Pokemon.
 */
static int g2_plausible(const uint8_t* px, uint32_t w, uint32_t h, int is_front) {
  uint32_t n = w * h;
  if (n < 4) return 0;
  uint32_t varied = 0;
  for (uint32_t i = 1; i < n; i++) if (px[i] != px[0]) { varied = 1; break; }
  if (!varied) return 0;
  uint32_t bt = 0, bz = 0;
  for (uint32_t x = 0; x < w; x++) {
    bt += 2; bz += (px[x] == 0) + (px[(h - 1) * w + x] == 0);
  }
  for (uint32_t y = 1; y + 1 < h; y++) {
    bt += 2; bz += (px[y * w] == 0) + (px[y * w + w - 1] == 0);
  }
  if (!is_front) return bz * 100u >= bt * 55u;
  int corners = (px[0] == 0 && px[w - 1] == 0 &&
                 px[(h - 1) * w] == 0 && px[n - 1] == 0);
  return corners ? (bz * 100u >= bt * 55u) : (bz * 100u >= bt * 85u);
}

static int g2_try(RomGbSprite* gs, uint32_t bank, uint16_t addr,
                  uint8_t wt, uint8_t ht, int is_front, uint8_t* px, uint8_t* work) {
  uint32_t off = bank_off(gs, bank, addr);
  if (!off) return 0;
  GbSpriteInfo gi;
  if (gb_sprite_gen2_buf(px, work, gs->read, gs->ctx, off, wt, ht, &gi) != GB_SPRITE_OK) return 0;
  if (gi.consumed == 0 || gi.consumed > bank_end(off) - off) return 0;
  return g2_plausible(px, gi.w, gi.h, is_front);
}

/* Other entries in PokemonPicPointers that share a stored bank byte, so a
 * candidate bank is confirmed against a dozen pictures instead of one. Fronts
 * come first because their frame test is the sharper one. */
#define G2_PEERS 12

typedef struct { uint16_t addr; uint8_t wt, ht, front; } G2Peer;

static uint32_t g2_peers(RomGbSprite* gs, uint8_t stored, uint16_t skip_dex,
                         G2Peer* peers, uint32_t max) {
  uint32_t n = 0;
  uint8_t chunk[30 * G2_ENTRY];
  for (int want_front = 1; want_front >= 0; want_front--) {
    for (uint32_t i = 0; i < G2_ROWS && n < max; i += 30) {
      uint32_t rows = G2_ROWS - i; if (rows > 30) rows = 30;
      if (!rd(gs, gs->pic_ptrs + i * G2_ENTRY, chunk, rows * G2_ENTRY)) break;
      for (uint32_t k = 0; k < rows && n < max; k++) {
        uint32_t idx = i + k;
        if (idx == G2_UNOWN_IDX) continue;
        if ((uint16_t)(idx + 1) == skip_dex) continue;
        const uint8_t* e = chunk + k * G2_ENTRY;
        if (want_front && e[0] == stored) {
          uint8_t sz;
          if (!g2_size(gs, (uint16_t)(idx + 1), &sz)) continue;
          peers[n].addr = rd16(e + 1);
          peers[n].wt = sz; peers[n].ht = sz; peers[n].front = 1;
          n++;
        } else if (!want_front && e[3] == stored) {
          peers[n].addr = rd16(e + 4);
          peers[n].wt = G2_BACK_TILE; peers[n].ht = G2_BACK_TILE; peers[n].front = 0;
          n++;
        }
      }
    }
  }
  return n;
}

/*
 * Confirm (or find) the real bank for one stored bank byte.
 *
 * The stored byte is NOT the bank: engine/gfx/load_pics.asm FixPicBank indexes a
 * PicsBanks[] table. Crystal's happens to be contiguous, so the default guess is
 * always right there; GOLD'S IS NOT — 0x13 -> 0x1F, 0x14 -> 0x20 and 0x1F -> 0x2E
 * while everything else is identity. So the default is CHECKED against three
 * different pictures, and if it fails every bank is tried and the answer must be
 * UNIQUE. "The nearest bank that happens to work" would be a guess, and a guess
 * here renders the wrong Pokemon in a screen whose whole point is provenance.
 */
static int g2_resolve(RomGbSprite* gs, uint8_t stored, uint16_t dex, uint16_t addr,
                      uint8_t wt, uint8_t ht, int is_front, uint8_t* px, uint8_t* work) {
  if (stored < gs->stored_lo) return 0;
  uint32_t i = (uint32_t)stored - gs->stored_lo;
  if (i >= ROM_GBSPRITE_BANKMAP) return 0;
  if (gs->bank_ok & (1u << i)) return gs->bank_map[i] != 0;

  G2Peer peers[G2_PEERS];
  uint32_t np = g2_peers(gs, stored, dex, peers, G2_PEERS);

  uint32_t win = 0, wins = 0;
  uint32_t first = gs->bank_map[i];
  for (uint32_t pass = 0; pass < 2; pass++) {
    for (uint32_t b = 1; b < gs->banks; b++) {
      if (pass == 0 && b != first) continue;          /* try the default first */
      if (pass == 1 && b == first) continue;
      if (!g2_try(gs, b, addr, wt, ht, is_front, px, work)) continue;
      uint32_t ok = 1;
      for (uint32_t k = 0; k < np; k++)
        if (!g2_try(gs, b, peers[k].addr, peers[k].wt, peers[k].ht, peers[k].front, px, work)) {
          ok = 0; break;
        }
      if (!ok) continue;
      wins++; win = b;
      if (wins > 1) return 0;                         /* ambiguous: fail closed */
    }
    /* The default surviving a dozen pictures is proof; skip the sweep. */
    if (pass == 0 && wins == 1) break;
  }
  if (wins != 1) return 0;
  gs->bank_map[i] = (uint8_t)win;
  gs->bank_ok |= (1u << i);
  return 1;
}

static int g2_fetch(RomGbSprite* gs, uint8_t stored, uint16_t dex, uint16_t addr,
                    uint8_t wt, uint8_t ht, int is_front, uint8_t* px, uint8_t* work,
                    GbSpriteInfo* info) {
  if (!g2_resolve(gs, stored, dex, addr, wt, ht, is_front, px, work)) return 0;
  uint32_t bank = gs->bank_map[stored - gs->stored_lo];
  uint32_t off = bank_off(gs, bank, addr);
  if (!off) return 0;
  if (gb_sprite_gen2_buf(px, work, gs->read, gs->ctx, off, wt, ht, info) != GB_SPRITE_OK) return 0;
  return info->consumed != 0 && info->consumed <= bank_end(off) - off;
}

/* UnownPicPointers sits at the SAME bank-relative address as PokemonPicPointers,
 * in the bank of "Pics 2" (gfx/pics.asm asserts the first half — "These are
 * assumed to be at the same address in their respective banks" — and layout.link
 * the second). Resolve that bank first, then prove all 26 letters decode. */
static int g2_unown_table(RomGbSprite* gs, uint8_t* px, uint8_t* work) {
  if (gs->unown_ptrs) return 1;
  uint8_t sz;
  if (!g2_size(gs, ROM_GBSPRITE_UNOWN_DEX, &sz)) return 0;

  /* force bank_map[1] ("Pics 2") to be resolved, using a picture that uses it */
  uint8_t stored1 = (uint8_t)(gs->stored_lo + 1);
  G2Peer p[1];
  if (g2_peers(gs, stored1, 0, p, 1) != 1) return 0;
  if (!g2_resolve(gs, stored1, 0, p[0].addr, p[0].wt, p[0].ht, p[0].front, px, work)) return 0;
  uint32_t bank = gs->bank_map[1];
  if (!bank) return 0;

  uint32_t off = bank * GB_BANK + (gs->pic_ptrs % GB_BANK);
  if (off >= gs->size || ROM_GBSPRITE_UNOWN_FORMS * G2_ENTRY > gs->size - off) return 0;

  uint8_t e[G2_ENTRY];
  GbSpriteInfo gi;
  for (uint32_t i = 0; i < ROM_GBSPRITE_UNOWN_FORMS; i++) {
    if (!rd(gs, off + i * G2_ENTRY, e, G2_ENTRY)) return 0;
    if (!g2_fetch(gs, e[0], 0, rd16(e + 1), sz, sz, 1, px, work, &gi)) return 0;
  }
  gs->unown_ptrs = off;
  return 1;
}

static int g2_pic(RomGbSprite* gs, RomGbSide side, uint16_t dex, uint8_t form,
                  uint8_t* px, uint8_t* work, RomGbPic* info) {
  uint8_t sz;
  if (!g2_size(gs, dex, &sz)) return 0;
  uint8_t tw = sz, th = sz;
  /* every Gen-2 back pic is 6x6; the base stats only describe the front */
  if (side == ROM_GBSPRITE_BACK) { tw = G2_BACK_TILE; th = G2_BACK_TILE; }

  uint32_t entry;
  if (dex == ROM_GBSPRITE_UNOWN_DEX) {
    if (form >= ROM_GBSPRITE_UNOWN_FORMS) return 0;
    if (!g2_unown_table(gs, px, work)) return 0;
    entry = gs->unown_ptrs + (uint32_t)form * G2_ENTRY;
  } else {
    if (form) return 0;
    entry = gs->pic_ptrs + (uint32_t)(dex - 1) * G2_ENTRY;
  }
  uint8_t e[G2_ENTRY];
  if (!rd(gs, entry, e, G2_ENTRY)) return 0;
  uint32_t k = (side == ROM_GBSPRITE_FRONT) ? 0 : 3;
  GbSpriteInfo gi;
  if (!g2_fetch(gs, e[k], dex, rd16(e + k + 1), tw, th,
                side == ROM_GBSPRITE_FRONT, px, work, &gi)) return 0;
  if (gi.wt != tw || gi.ht != th) return 0;

  info->gen = 2; info->wt = tw; info->ht = th;
  info->w = gi.w; info->h = gi.h;
  info->pixels = (uint32_t)gi.w * gi.h;
  info->consumed = gi.consumed;   /* to the end of frame 0, not of the blob */
  return 1;
}

/* ------------------------------------------------------------------- API */

int rom_gbsprite_pic_buf(RomGbSprite* gs, RomGbSide side, uint16_t dex, uint8_t form,
                         uint8_t* px, uint8_t* work, RomGbPic* info) {
  RomGbPic tmp;
  if (!info) info = &tmp;
  memset(info, 0, sizeof *info);
  if (!gs || !gs->ok || !px || !work) return 0;
  if (side != ROM_GBSPRITE_FRONT && side != ROM_GBSPRITE_BACK) return 0;
  int r = 0;
  if (gs->gen == GB_ROM_GEN1)      r = (form == 0) && g1_pic(gs, side, dex, px, work, info);
  else if (gs->gen == GB_ROM_GEN2) r = g2_pic(gs, side, dex, form, px, work, info);
  if (!r) memset(info, 0, sizeof *info);
  return r;
}

int rom_gbsprite_pic(RomGbSprite* gs, RomGbSide side, uint16_t dex, uint8_t form,
                     GbSprite* out, RomGbPic* info) {
  RomGbPic tmp;
  if (!info) info = &tmp;
  if (!out) { memset(info, 0, sizeof *info); return 0; }
  int r = rom_gbsprite_pic_buf(gs, side, dex, form, out->px, out->work, info);
  /* rom_gbsprite_pic_buf() only fills `info` -- callers of THIS entry point (the
   * host test's hash_px, e.g.) read out->wt/ht/w/h/consumed too, so mirror them
   * back for exact behavioural equivalence with the pre-refactor function. */
  if (r) {
    out->wt = info->wt; out->ht = info->ht;
    out->w  = info->w;  out->h  = info->h;
    out->consumed = info->consumed;
  }
  return r;
}

int rom_gbsprite_pal(const RomGbSprite* gs, uint16_t dex, int shiny, uint16_t dst[4]) {
  if (!gs || !gs->ok || !dst) return 0;
  if (gs->gen == GB_ROM_GEN1) {
    if (dex < 1 || dex > G1_SPECIES) return 0;
    dst[0] = G1_GREY0; dst[1] = G1_GREY1; dst[2] = G1_GREY2; dst[3] = G1_GREY3;
    return 1;
  }
  if (gs->gen != GB_ROM_GEN2) return 0;
  if (dex < 1 || dex > G2_ROWS) return 0;
  uint8_t p[8];
  /* index = species; entry 0 is a dummy (engine/gfx/color.asm
   * _GetMonPalettePointer: hl = a*8 + PokemonPalettes) */
  if (!rd(gs, gs->palettes + (uint32_t)dex * 8u, p, 8)) return 0;
  const uint8_t* q = shiny ? p + 4 : p;
  uint16_t c1 = rd16(q), c2 = rd16(q + 2);
  if ((c1 | c2) & 0x8000u) return 0;
  dst[0] = G2_PAL_WHITE; dst[1] = c1; dst[2] = c2; dst[3] = G2_PAL_BLACK;
  return 1;
}

int rom_gbsprite_to_rgb15(const GbSprite* s, const uint16_t pal[4],
                          uint16_t* dst, uint32_t dst_pixels) {
  if (!s || !pal || !dst) return 0;
  if (!s->w || !s->h || s->w > GB_SPRITE_MAX_W || s->h > GB_SPRITE_MAX_H) return 0;
  uint32_t n = (uint32_t)s->w * s->h;
  if (dst_pixels < n) return 0;                 /* refused, never truncated    */
  /* index 0 is the picture's background and renders TRANSPARENT, exactly as
   * rom_sprite_to_rgb15() does for Gen 3 — that is what lets one blitter draw
   * all three generations side by side in the bank. */
  for (uint32_t i = 0; i < n; i++) {
    uint8_t v = s->px[i];
    if (v > 3) return 0;
    dst[i] = v ? (uint16_t)(0x8000u | (pal[v] & 0x7FFFu)) : 0u;
  }
  return 1;
}

int rom_gbsprite_to_rgb15_inplace(uint8_t* buf, uint32_t px_off, uint8_t w, uint8_t h,
                                  const uint16_t pal[4]) {
  if (!buf || !pal) return 0;
  if (!w || !h || w > GB_SPRITE_MAX_W || h > GB_SPRITE_MAX_H) return 0;
  if (px_off < GB_SPRITE_MAX_PX) return 0;   /* the safety margin the header proves */
  uint32_t n = (uint32_t)w * h;
  const uint8_t* px = buf + px_off;
  uint16_t* dst = (uint16_t*)(void*)buf;
  /* ASCENDING i: dst[i]'s write never reaches an unread px[j] (j > i) -- see
   * rom_gbsprite.h's proof. Reading px[i] into `v` before writing dst[i] is what
   * keeps the i == w*h-1 coincidence (px_off == GB_SPRITE_MAX_PX exactly) correct;
   * every other i is safe regardless of order. */
  for (uint32_t i = 0; i < n; i++) {
    uint8_t v = px[i];
    if (v > 3) return 0;
    dst[i] = v ? (uint16_t)(0x8000u | (pal[v] & 0x7FFFu)) : 0u;
  }
  return 1;
}
