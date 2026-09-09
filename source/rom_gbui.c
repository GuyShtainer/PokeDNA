/* Gen-1 / Gen-2 trainer-card and bag UI graphics, located BY SHAPE in the
 * user's own Game Boy cartridge -- see rom_gbui.h for the design, the
 * per-signature citations and the fail-closed rules. Pure C: no tonc, no
 * FatFs, no GBA headers. */
#include "rom_gbui.h"

#include <string.h>

#define GB_BANK   0x4000u
#define GB_WIN_LO 0x4000u
#define GB_WIN_HI 0x8000u

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

static uint32_t fileoff(uint8_t bank, uint16_t addr) {
  if (addr < GB_WIN_LO) return addr;
  return (uint32_t)bank * GB_BANK + (uint32_t)(addr - GB_WIN_LO);
}

/* ------------------------------------------------------------- scan context */

/* Everything a locator needs during open()/open_loc() -- deliberately NOT
 * part of RomGbUi (which retains no scratch pointer at all, see the header's
 * memory note). Local to this file, lives on locate()'s own stack frame. */
typedef struct {
  GbReadFn read;
  void*    ctx;
  uint32_t size;
  uint8_t  banks;         /* size / 16 KiB, needed by the card-frame bank sweep */
  uint8_t* scratch;
  uint32_t scratch_len;
} Scan;

static int rd(const Scan* s, uint32_t off, void* dst, uint32_t len) {
  if (!s->read || len == 0) return 0;
  if (off >= s->size || len > s->size - off) return 0;
  return s->read(s->ctx, off, dst, len) ? 1 : 0;
}

/* ------------------------------------------------------------- header/id */

#define GB_LOGO_OFF  0x104u
#define GB_LOGO_LEN  0x30u
#define GB_LOGO_FNV  0x016BAD3Fu   /* measured identically on Red, Yellow, Gold, Crystal */

static uint32_t fnv1a(const uint8_t* p, uint32_t n, uint32_t h) {
  for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
  return h;
}

static int parse_header(const Scan* s, uint8_t* out_banks, uint32_t* out_id_hash) {
  uint8_t h[0x50];   /* 0x100..0x14F */
  if (!rd(s, 0x100, h, sizeof h)) return 0;
  if (fnv1a(h + (GB_LOGO_OFF - 0x100), GB_LOGO_LEN, 0x811C9DC5u) != GB_LOGO_FNV) return 0;

  uint8_t x = 0;
  for (uint32_t i = 0x134; i <= 0x14C; i++) x = (uint8_t)(x - h[i - 0x100] - 1u);
  if (x != h[0x14D - 0x100]) return 0;

  uint8_t code = h[0x148 - 0x100];
  if (code > 8) return 0;
  if (s->size != (0x8000u << code)) return 0;
  if (s->size % GB_BANK) return 0;
  uint32_t banks = s->size / GB_BANK;
  if (banks > 255u || banks == 0) return 0;

  *out_banks = (uint8_t)banks;
  *out_id_hash = fnv1a(h, sizeof h, 0x811C9DC5u);
  return 1;
}

/* -------------------------------------------------------------- verifiers */

/* All fixed-size structural checks re-read through a bounded local buffer
 * rather than the scan window, which is only valid during the single
 * scan_multi pass. */

static uint32_t distinct_tiles(const Scan* s, uint32_t off, uint8_t bpp, uint32_t ntiles) {
  uint32_t stride = (bpp == 2) ? 16u : 8u;
  uint8_t tiles[96 * 16];  /* worst case here is 86 (leaders); 96 is headroom */
  if (ntiles > 96) return 0;
  if (off + ntiles * stride > s->size) return 0;
  if (!rd(s, off, tiles, ntiles * stride)) return 0;   /* ONE read, not ntiles */
  uint32_t distinct = 0;
  for (uint32_t i = 0; i < ntiles; i++) {
    int dup = 0;
    for (uint32_t j = 0; j < i; j++)
      if (memcmp(tiles + i * stride, tiles + j * stride, stride) == 0) { dup = 1; break; }
    if (!dup) distinct++;
  }
  return distinct;
}

static int all_blank(const Scan* s, uint32_t off, uint32_t nbytes) {
  uint8_t buf[64];
  uint32_t done = 0;
  while (done < nbytes) {
    uint32_t chunk = nbytes - done; if (chunk > sizeof buf) chunk = sizeof buf;
    if (!rd(s, off + done, buf, chunk)) return 0;
    for (uint32_t i = 0; i < chunk; i++) if (buf[i]) return 0;
    done += chunk;
  }
  return 1;
}

static int any_nonzero(const Scan* s, uint32_t off, uint32_t nbytes) {
  uint8_t buf[64];
  uint32_t done = 0;
  while (done < nbytes) {
    uint32_t chunk = nbytes - done; if (chunk > sizeof buf) chunk = sizeof buf;
    if (!rd(s, off + done, buf, chunk)) return 0;
    for (uint32_t i = 0; i < chunk; i++) if (buf[i]) return 1;
    done += chunk;
  }
  return 0;
}

static int block_eq(const Scan* s, uint32_t a, uint32_t b, uint32_t n) {
  uint8_t ba[64], bb[64];
  uint32_t done = 0;
  while (done < n) {
    uint32_t chunk = n - done; if (chunk > sizeof ba) chunk = sizeof ba;
    if (!rd(s, a + done, ba, chunk)) return 0;
    if (!rd(s, b + done, bb, chunk)) return 0;
    if (memcmp(ba, bb, chunk) != 0) return 0;
    done += chunk;
  }
  return 1;
}

/* 1024 B = 128 tiles, 1bpp (Gen 1 Font / Gen 2 Font, same charmap layout). */
static int font_verify(const Scan* s, uint32_t off) {
  if (off + 1024 > s->size) return 0;
  uint32_t solid = 0, blank = 0;
  uint8_t p[128];                                /* 16 tiles per read: 8 reads, not 128 */
  for (uint32_t c = 0; c < 8; c++) {
    if (!rd(s, off + c * 128u, p, 128u)) return 0;
    for (uint32_t t = 0; t < 16; t++) {
      int all_ff = 1, all_00 = 1;
      for (int i = 0; i < 8; i++) {
        uint8_t v = p[t * 8 + i];
        if (v != 0xFF) all_ff = 0;
        if (v != 0x00) all_00 = 0;
      }
      if (all_ff) solid++;
      if (all_00) blank++;
    }
  }
  return solid == 0 && blank <= 40;   /* measured 26-32 blank across the corpus */
}

/* -------------------------------------------------------- pattern matchers */
/* Each cb reads only the scan window (no ROM re-read); `look` is how many
 * window bytes it inspects. Struct-address relations that need only the
 * window (e.g. G1-C's C-A==144) are checked inline here; everything that
 * needs `banks` or a fresh read happens in the try_*() functions below,
 * against the RAW hit offset. */

typedef int (*ScanCb)(const uint8_t* w);

static int g1_font_cb(const uint8_t* w) {
  return w[0]==0xF0 && w[1]==0x40 && w[2]==0xCB && w[3]==0x7F && w[4]==0x20 &&
         w[6]==0x21 && w[9]==0x11 && w[12]==0x01 && w[13]==0x00 && w[14]==0x04 &&
         w[15]==0x3E && w[17]==0xC3;
}
static int g1_textbox_cb(const uint8_t* w) {
  return w[0]==0xF0 && w[1]==0x40 && w[2]==0xCB && w[3]==0x7F && w[4]==0x20 &&
         w[6]==0x21 && w[9]==0x11 && w[10]==0x00 && w[11]==0x96 &&
         w[12]==0x01 && w[13]==0x00 && w[14]==0x02 && w[15]==0x3E && w[17]==0xC3;
}
static int g1_cardframe_cb(const uint8_t* w) {
  if (w[0]!=0x21 || w[3]!=0x11 || w[4]!=0x70 || w[5]!=0x97 || w[6]!=0x01 || w[7]!=0x80 ||
      w[8]!=0x00 || w[9]!=0xC5 || w[10]!=0xCD) return 0;
  if (w[13]!=0x21 || w[16]!=0x11 || w[17]!=0x00 || w[18]!=0x96 || w[19]!=0x01 ||
      w[20]!=0x70 || w[21]!=0x01 || w[22]!=0xCD) return 0;
  if (w[25]!=0xC1 || w[26]!=0x21 || w[29]!=0x11 || w[30]!=0x80 || w[31]!=0x8D || w[32]!=0xCD) return 0;
  uint16_t a = rd16(w+1), c = rd16(w+14), e = rd16(w+27);
  return (uint16_t)(c - a) == 144 && (uint16_t)(e - a) == 512;
}
static int g1_badges_cb(const uint8_t* w) {
  static const uint8_t sig[8] = {0x20,0x28,0x30,0x38,0x40,0x48,0x50,0x58};
  return memcmp(w, sig, 8) == 0;
}
static int g1_playerpic_cb(const uint8_t* w) {
  return w[0]==0x11 && w[3]==0x01 && w[4]==0x01;
}
static int g2_frame_cb(const uint8_t* w) {
  return w[0]==0xFA && w[3]==0xE6 && w[4]==0x07 && w[5]==0x01 && w[6]==0x30 && w[7]==0x00 &&
         w[8]==0x21 && w[11]==0xCD && w[14]==0x54 && w[15]==0x5D && w[16]==0x21 &&
         w[17]==0x90 && w[18]==0x97 && w[19]==0x01 && w[20]==0x06;
}
static int g2_badgeleader_cb(const uint8_t* w) {
  return w[0]==0x11 && w[3]==0x21 && w[4]==0x90 && w[5]==0x92 && w[6]==0x01 && w[7]==0x56 &&
         w[9]==0xCD && w[12]==0x11 && w[15]==0x21 && w[16]==0x00 && w[17]==0x80 &&
         w[18]==0x01 && w[19]==0x2C && w[21]==0xCD;
}
static int g2_cardpic_cm_cb(const uint8_t* w) {
  return w[0]==0x21 && w[3]==0xFA && w[6]==0xCB && w[8]==0x28 && w[10]==0x21 &&
         w[13]==0x11 && w[14]==0x00 && w[15]==0x90 && w[16]==0x01 && w[17]==0x30 &&
         w[18]==0x02 && w[19]==0x3E && w[21]==0xCD && w[24]==0x21 &&
         w[27]==0x11 && w[28]==0x30 && w[29]==0x92 && w[30]==0x01 && w[31]==0x60 && w[32]==0x00;
}
static int g2_cardpic_gold_cb(const uint8_t* w) {
  return w[0]==0x21 && w[3]==0x11 && w[4]==0x00 && w[5]==0x90 && w[6]==0x01 && w[7]==0x90 &&
         w[8]==0x02 && w[9]==0x3E && w[11]==0xCD && w[14]==0x21 && w[17]==0x11 &&
         w[18]==0x90 && w[19]==0x92 && w[20]==0x01 && w[21]==0x60 && w[22]==0x05 &&
         w[23]==0x3E && w[25]==0xCD;
}
static int g2_pack_cb(const uint8_t* w) {
  uint16_t p0 = rd16(w+0), p1 = rd16(w+2), p2 = rd16(w+4), p3 = rd16(w+6);
  if (p0 < GB_WIN_LO || p0 >= GB_WIN_HI || p1 < GB_WIN_LO || p1 >= GB_WIN_HI ||
      p2 < GB_WIN_LO || p2 >= GB_WIN_HI || p3 < GB_WIN_LO || p3 >= GB_WIN_HI) return 0;
  uint16_t base = p2;
  return p0 == (uint16_t)(base + 240) && p1 == (uint16_t)(base + 720) &&
         p3 == (uint16_t)(base + 480);
}

enum {
  J_G1_FONT = 0, J_G1_TEXTBOX, J_G1_CARDFRAME, J_G1_BADGES, J_G1_PLAYERPIC,
  J_G2_FRAME, J_G2_BADGELEADER, J_G2_CARDPIC_CM, J_G2_CARDPIC_GOLD, J_G2_PACK,
  J_COUNT
};

/* Only G1-P's raw anchor hits this many times before the 0x77 filter (see
 * rom_gbui.h) -- MEASURED across the corpus: Red 71, Yellow 74, Gold 96,
 * Crystal 97 (the g1_playerpic pattern still fires spuriously in the Gen-2
 * ROMs since scan_multi runs every job against every ROM regardless of which
 * gen it turns out to be; those hits are simply never inspected because the
 * Gen-1 try-block bails out on J_G1_FONT.n before it ever looks at
 * J_G1_PLAYERPIC, so 96 slots is plenty even though Crystal's raw count is
 * 97). Every other signature here is unique or near-unique in a real ROM. A
 * single SCAN_MAX_HITS sized for the worst job and applied to ALL 10 jobs
 * cost 5,120 B of locate()'s own stack frame for hit storage NONE of the
 * other 9 jobs ever use (measured with -fstack-usage, 2026-09-09, before
 * this fix: locate() was 5,376 B own frame). So each job now owns a
 * CALLER-SIZED hit array via a pointer+cap instead of a fixed inline one --
 * 9 small jobs at 8 slots (288 B total) plus the one job that needs more
 * (96 slots, 384 B) is 672 B, a >4 KB reduction with identical behaviour. */
#define SCAN_SMALL_CAP  8u
#define SCAN_PLAYERPIC_CAP 96u

typedef struct {
  ScanCb    cb;
  uint32_t  look;
  uint32_t* off;               /* caller-owned, size == cap                 */
  uint32_t  cap;
  uint32_t  n;                 /* > cap means "too many, fail closed"       */
} ScanJob;

static int scan_multi(const Scan* s, ScanJob* jobs, uint32_t njobs) {
  uint8_t* w = s->scratch;
  uint32_t cap = s->scratch_len;
  uint32_t look = 0, minlook = 0xFFFFFFFFu;
  for (uint32_t j = 0; j < njobs; j++) {
    jobs[j].n = 0;
    if (jobs[j].look > look) look = jobs[j].look;
    if (jobs[j].look < minlook) minlook = jobs[j].look;
  }
  if (!w || cap < look + 16u || s->size < look) return 0;
  uint32_t step = cap - look + 1u;
  for (uint32_t base = 0; base + look <= s->size; base += step) {
    uint32_t n = s->size - base; if (n > cap) n = cap;
    if (!rd(s, base, w, n)) return 0;
    uint32_t lim = n - look;
    for (uint32_t i = 0; i <= lim; i++) {
      const uint8_t* p = w + i;
      for (uint32_t j = 0; j < njobs; j++) {
        if (!jobs[j].cb(p)) continue;
        if (jobs[j].n < jobs[j].cap) jobs[j].off[jobs[j].n] = base + i;
        jobs[j].n++;
      }
    }
    /* This window is bounded by the LONGEST pattern (`look`), so a shorter
     * signature duplicated in the final `look - minlook` bytes of the ROM is
     * never tested by the loop above. On the last window only, sweep those
     * remaining starts with each job bounded by its OWN pattern length. */
    if (base + n >= s->size) {
      for (uint32_t i = lim + 1; i + minlook <= n; i++) {
        const uint8_t* p = w + i;
        for (uint32_t j = 0; j < njobs; j++) {
          if (i + jobs[j].look > n) continue;
          if (!jobs[j].cb(p)) continue;
          if (jobs[j].n < jobs[j].cap) jobs[j].off[jobs[j].n] = base + i;
          jobs[j].n++;
        }
      }
    }
  }
  return 1;
}

/* --------------------------------------------------------- try_* (Gen 1) */

static int try_font(const Scan* s, uint32_t hit, uint32_t* out) {
  uint8_t buf[18];
  if (!rd(s, hit, buf, sizeof buf)) return 0;
  uint16_t addr = rd16(buf + 7);
  uint8_t bank = buf[16];
  uint32_t off = fileoff(bank, addr);
  if (!font_verify(s, off)) return 0;
  *out = off;
  return 1;
}
static int try_textbox(const Scan* s, uint32_t hit, uint32_t* out) {
  uint8_t buf[18];
  if (!rd(s, hit, buf, sizeof buf)) return 0;
  uint16_t addr = rd16(buf + 7);
  uint8_t bank = buf[16];
  uint32_t off = fileoff(bank, addr);
  if (off + 512 > s->size) return 0;
  if (distinct_tiles(s, off, 2, 32) != 32) return 0;
  *out = off;
  return 1;
}
static int try_cardframe(const Scan* s, uint32_t hit, uint32_t* out) {
  uint8_t buf[33];
  if (!rd(s, hit, buf, sizeof buf)) return 0;
  uint16_t addr = rd16(buf + 1);
  uint32_t good = 0, good_off = 0;
  for (uint32_t bank = 0; bank < (uint32_t)s->banks; bank++) {
    uint32_t off = fileoff((uint8_t)bank, addr);
    if (off + 640 > s->size) continue;
    if (distinct_tiles(s, off, 2, 9) != 9) continue;
    if (!all_blank(s, off + 144, 352)) continue;
    if (!any_nonzero(s, off + 496, 16)) continue;
    if (distinct_tiles(s, off + 512, 2, 8) < 7) continue;
    good++; good_off = off;
    if (good > 1) return 0;
  }
  if (good != 1) return 0;
  *out = good_off;
  return 1;
}
static int try_badges(const Scan* s, uint32_t hit, uint32_t* out) {
  uint32_t off = hit + 8;
  if (off + 1024 > s->size) return 0;
  if (distinct_tiles(s, off, 2, 64) != 64) return 0;
  *out = off;
  return 1;
}
static int try_playerpic(const Scan* s, uint32_t hit, uint32_t* out, uint8_t* out_bank) {
  uint8_t buf[6];
  if (!rd(s, hit, buf, sizeof buf)) return 0;
  uint16_t addr = rd16(buf + 1);
  uint8_t bank = buf[5];
  if (addr < GB_WIN_LO || addr >= GB_WIN_HI) return 0;
  if (bank == 0 || (uint32_t)bank * GB_BANK >= s->size) return 0;
  uint32_t off = fileoff(bank, addr);
  uint8_t first;
  if (!rd(s, off, &first, 1)) return 0;
  if (first != 0x77) return 0;
  *out = off; *out_bank = bank;
  return 1;
}

/* --------------------------------------------------------- try_* (Gen 2) */

static int try_frame(const Scan* s, uint32_t hit, uint32_t* out) {
  uint8_t buf[22];
  if (!rd(s, hit, buf, sizeof buf)) return 0;
  uint16_t addr = rd16(buf + 9);
  uint8_t bank = buf[21];
  uint32_t off = fileoff(bank, addr);
  if (off + 9 * 48 > s->size) return 0;
  if (distinct_tiles(s, off, 1, 54) < 46) return 0;
  *out = off;
  return 1;
}

typedef struct { uint32_t leaders, badges; } BadgeLeaderHit;

static int try_badgeleader_hit(const Scan* s, uint32_t hit, BadgeLeaderHit* out) {
  (void)s;
  uint8_t buf[22];
  if (!rd(s, hit, buf, sizeof buf)) return 0;
  uint16_t laddr = rd16(buf + 1); uint8_t lbank = buf[8];
  uint16_t baddr = rd16(buf + 13); uint8_t bbank = buf[20];
  out->leaders = fileoff(lbank, laddr);
  out->badges  = fileoff(bbank, baddr);
  return 1;
}

static int try_cardpic_crystal(const Scan* s, uint32_t hit,
                               uint32_t* chris, uint32_t* kris, uint32_t* tcgfx) {
  uint8_t buf[33];
  if (!rd(s, hit, buf, sizeof buf)) return 0;
  uint8_t bank = buf[20];
  uint32_t co = fileoff(bank, rd16(buf + 1));
  uint32_t ko = fileoff(bank, rd16(buf + 11));
  uint32_t to = fileoff(bank, rd16(buf + 25));
  if (ko < co || ko - co != 0x230u) return 0;
  if (co + 35 * 16 > s->size || to + 6 * 16 > s->size) return 0;
  *chris = co; *kris = ko; *tcgfx = to;
  return 1;
}
static int try_cardpic_gold(const Scan* s, uint32_t hit, uint32_t* chris) {
  uint8_t buf[26];
  if (!rd(s, hit, buf, sizeof buf)) return 0;
  uint8_t bank = buf[10];
  uint32_t co = fileoff(bank, rd16(buf + 1));
  if (co + 41 * 16 > s->size) return 0;   /* 35 pic + 6 TrainerCardGFX tiles */
  *chris = co;
  return 1;
}

/* PackGFXPointers is a pure numeric fingerprint: the hit offset IS the
 * pointer-table's own file offset, its bank is hit/GB_BANK, and PackGFX
 * itself is the table's OWN third entry (p2 == base, see g2_pack_cb). */
static int try_pack(const Scan* s, uint32_t hit, uint32_t* out) {
  uint8_t buf[8];
  if (!rd(s, hit, buf, sizeof buf)) return 0;
  uint8_t bank = (uint8_t)(hit / GB_BANK);
  uint16_t base = rd16(buf + 4);
  uint32_t off = fileoff(bank, base);
  if (off + 60 * 16 > s->size) return 0;
  *out = off;
  return 1;
}

/* --------------------------------------------------------------- locate() */

typedef struct {
  uint32_t off[ROM_GBUI_OFF_COUNT];
  uint32_t anchor[ROM_GBUI_ANCH_COUNT];
  uint8_t  gen, playerpic_bank, cardpic_colmajor;
} LocResult;

static int locate(const Scan* s, LocResult* r) {
  memset(r, 0, sizeof *r);

  ScanJob jobs[J_COUNT];
  memset(jobs, 0, sizeof jobs);

  uint32_t hits_font[SCAN_SMALL_CAP], hits_textbox[SCAN_SMALL_CAP],
           hits_cardframe[SCAN_SMALL_CAP], hits_badges[SCAN_SMALL_CAP],
           hits_frame[SCAN_SMALL_CAP], hits_badgeleader[SCAN_SMALL_CAP],
           hits_cardpic_cm[SCAN_SMALL_CAP], hits_cardpic_gold[SCAN_SMALL_CAP],
           hits_pack[SCAN_SMALL_CAP];
  uint32_t hits_playerpic[SCAN_PLAYERPIC_CAP];

  jobs[J_G1_FONT        ].cb = g1_font_cb;         jobs[J_G1_FONT        ].look = 18;
  jobs[J_G1_FONT        ].off = hits_font;         jobs[J_G1_FONT        ].cap = SCAN_SMALL_CAP;
  jobs[J_G1_TEXTBOX     ].cb = g1_textbox_cb;      jobs[J_G1_TEXTBOX     ].look = 18;
  jobs[J_G1_TEXTBOX     ].off = hits_textbox;      jobs[J_G1_TEXTBOX     ].cap = SCAN_SMALL_CAP;
  jobs[J_G1_CARDFRAME   ].cb = g1_cardframe_cb;    jobs[J_G1_CARDFRAME   ].look = 33;
  jobs[J_G1_CARDFRAME   ].off = hits_cardframe;    jobs[J_G1_CARDFRAME   ].cap = SCAN_SMALL_CAP;
  jobs[J_G1_BADGES      ].cb = g1_badges_cb;       jobs[J_G1_BADGES      ].look = 8;
  jobs[J_G1_BADGES      ].off = hits_badges;       jobs[J_G1_BADGES      ].cap = SCAN_SMALL_CAP;
  jobs[J_G1_PLAYERPIC   ].cb = g1_playerpic_cb;    jobs[J_G1_PLAYERPIC   ].look = 6;
  jobs[J_G1_PLAYERPIC   ].off = hits_playerpic;    jobs[J_G1_PLAYERPIC   ].cap = SCAN_PLAYERPIC_CAP;
  jobs[J_G2_FRAME       ].cb = g2_frame_cb;        jobs[J_G2_FRAME       ].look = 22;
  jobs[J_G2_FRAME       ].off = hits_frame;        jobs[J_G2_FRAME       ].cap = SCAN_SMALL_CAP;
  jobs[J_G2_BADGELEADER ].cb = g2_badgeleader_cb;  jobs[J_G2_BADGELEADER ].look = 22;
  jobs[J_G2_BADGELEADER ].off = hits_badgeleader;  jobs[J_G2_BADGELEADER ].cap = SCAN_SMALL_CAP;
  jobs[J_G2_CARDPIC_CM  ].cb = g2_cardpic_cm_cb;   jobs[J_G2_CARDPIC_CM  ].look = 33;
  jobs[J_G2_CARDPIC_CM  ].off = hits_cardpic_cm;   jobs[J_G2_CARDPIC_CM  ].cap = SCAN_SMALL_CAP;
  jobs[J_G2_CARDPIC_GOLD].cb = g2_cardpic_gold_cb; jobs[J_G2_CARDPIC_GOLD].look = 26;
  jobs[J_G2_CARDPIC_GOLD].off = hits_cardpic_gold; jobs[J_G2_CARDPIC_GOLD].cap = SCAN_SMALL_CAP;
  jobs[J_G2_PACK        ].cb = g2_pack_cb;         jobs[J_G2_PACK        ].look = 8;
  jobs[J_G2_PACK        ].off = hits_pack;         jobs[J_G2_PACK        ].cap = SCAN_SMALL_CAP;
  if (!scan_multi(s, jobs, J_COUNT)) return 0;

  /* ---------------------------------------------------------- Gen 1 ---- */
  do {
    if (jobs[J_G1_FONT].n != 1) break;
    uint32_t font;
    if (!try_font(s, jobs[J_G1_FONT].off[0], &font)) break;

    if (jobs[J_G1_TEXTBOX].n != 1) break;
    uint32_t textbox;
    if (!try_textbox(s, jobs[J_G1_TEXTBOX].off[0], &textbox)) break;

    if (jobs[J_G1_CARDFRAME].n != 1) break;
    uint32_t cardframe;
    if (!try_cardframe(s, jobs[J_G1_CARDFRAME].off[0], &cardframe)) break;

    if (jobs[J_G1_BADGES].n != 1) break;
    uint32_t badges;
    if (!try_badges(s, jobs[J_G1_BADGES].off[0], &badges)) break;

    if (jobs[J_G1_PLAYERPIC].n == 0 || jobs[J_G1_PLAYERPIC].n > SCAN_PLAYERPIC_CAP) break;
    uint32_t pp = 0, pp_found = 0, pp_anchor = 0; uint8_t pp_bank = 0;
    for (uint32_t i = 0; i < jobs[J_G1_PLAYERPIC].n; i++) {
      uint32_t cand; uint8_t bk;
      if (try_playerpic(s, jobs[J_G1_PLAYERPIC].off[i], &cand, &bk)) {
        pp_found++; pp = cand; pp_bank = bk; pp_anchor = jobs[J_G1_PLAYERPIC].off[i];
      }
    }
    if (pp_found != 1) break;

    r->off[ROM_GBUI_OFF_FONT] = font;
    r->off[ROM_GBUI_OFF_TEXTBOX] = textbox;
    r->off[ROM_GBUI_OFF_CARDFRAME] = cardframe;
    r->off[ROM_GBUI_OFF_BADGES] = badges;
    r->off[ROM_GBUI_OFF_PLAYERPIC] = pp;
    r->playerpic_bank = pp_bank;
    r->anchor[ROM_GBUI_ANCH_FONT] = jobs[J_G1_FONT].off[0];
    r->anchor[ROM_GBUI_ANCH_TEXTBOX] = jobs[J_G1_TEXTBOX].off[0];
    r->anchor[ROM_GBUI_ANCH_CARDFRAME] = jobs[J_G1_CARDFRAME].off[0];
    r->anchor[ROM_GBUI_ANCH_BADGES] = jobs[J_G1_BADGES].off[0];
    r->anchor[ROM_GBUI_ANCH_PLAYERPIC] = pp_anchor;
    r->gen = ROM_GBUI_GEN1;
    return 1;
  } while (0);

  /* ---------------------------------------------------------- Gen 2 ---- */
  memset(r, 0, sizeof *r);

  if (jobs[J_G2_FRAME].n != 1) return 0;
  uint32_t frames;
  if (!try_frame(s, jobs[J_G2_FRAME].off[0], &frames)) return 0;
  if (frames < 1536u + 512u) return 0;          /* font, then fontextra, must exist below it */
  uint32_t font_off = frames - 1536u;
  if (!font_verify(s, font_off)) return 0;
  uint32_t fontextra_off = font_off - 512u;
  if (distinct_tiles(s, fontextra_off, 2, 32) != 32) return 0;

  if (jobs[J_G2_BADGELEADER].n != 2) return 0;
  BadgeLeaderHit h0, h1;
  if (!try_badgeleader_hit(s, jobs[J_G2_BADGELEADER].off[0], &h0)) return 0;
  if (!try_badgeleader_hit(s, jobs[J_G2_BADGELEADER].off[1], &h1)) return 0;
  if (h0.badges + 44 * 16 > s->size || h0.leaders + 86 * 16 > s->size) return 0;
  if (!block_eq(s, h0.badges, h1.badges, 44 * 16)) return 0;
  if (!block_eq(s, h0.leaders, h1.leaders, 86 * 16)) return 0;
  if (distinct_tiles(s, h0.badges, 2, 44) != 44) return 0;
  if (distinct_tiles(s, h0.leaders, 2, 86) < 63) return 0;

  uint32_t cardpic_m = 0, cardpic_f = 0, cardgfx = 0, cardpic_anchor = 0;
  int is_crystal_shaped = 0;
  if (jobs[J_G2_CARDPIC_CM].n == 1 &&
      try_cardpic_crystal(s, jobs[J_G2_CARDPIC_CM].off[0], &cardpic_m, &cardpic_f, &cardgfx)) {
    is_crystal_shaped = 1;
    cardpic_anchor = jobs[J_G2_CARDPIC_CM].off[0];
  } else if (jobs[J_G2_CARDPIC_GOLD].n == 1 &&
             try_cardpic_gold(s, jobs[J_G2_CARDPIC_GOLD].off[0], &cardpic_m)) {
    cardgfx = cardpic_m + 35 * 16;
    cardpic_f = 0;
    cardpic_anchor = jobs[J_G2_CARDPIC_GOLD].off[0];
  } else {
    return 0;
  }

  uint32_t pack_m = 0, pack_f = 0;
  /* R3: Crystal has the gender branch and exactly 2 tables; Gold has
   * neither. Tie the count to the shape the card-pic signature already
   * proved, so a ROM with a stray 2nd table can't invent a pack_f for Gold. */
  if (jobs[J_G2_PACK].n != (is_crystal_shaped ? 2u : 1u)) return 0;
  if (!try_pack(s, jobs[J_G2_PACK].off[0], &pack_m)) return 0;
  if (is_crystal_shaped && !try_pack(s, jobs[J_G2_PACK].off[1], &pack_f)) return 0;

  if (is_crystal_shaped && (cardpic_f == 0 || pack_f == 0)) return 0;

  r->off[ROM_GBUI_OFF_FRAMES] = frames;
  r->off[ROM_GBUI_OFF_FONT] = font_off;
  r->off[ROM_GBUI_OFF_FONTEXTRA] = fontextra_off;
  r->off[ROM_GBUI_OFF_BADGES] = h0.badges;
  r->off[ROM_GBUI_OFF_LEADERS] = h0.leaders;
  r->off[ROM_GBUI_OFF_CARDPIC_M] = cardpic_m;
  r->off[ROM_GBUI_OFF_CARDPIC_F] = cardpic_f;
  r->off[ROM_GBUI_OFF_CARDGFX] = cardgfx;
  r->off[ROM_GBUI_OFF_PACK_M] = pack_m;
  r->off[ROM_GBUI_OFF_PACK_F] = pack_f;
  r->cardpic_colmajor = (uint8_t)is_crystal_shaped;
  r->anchor[ROM_GBUI_ANCH_FRAMES] = jobs[J_G2_FRAME].off[0];
  r->anchor[ROM_GBUI_ANCH_BADGELEADER] = jobs[J_G2_BADGELEADER].off[0];
  r->anchor[ROM_GBUI_ANCH_CARDPIC] = cardpic_anchor;
  r->anchor[ROM_GBUI_ANCH_PACK_M] = jobs[J_G2_PACK].off[0];
  r->anchor[ROM_GBUI_ANCH_PACK_F] = is_crystal_shaped ? jobs[J_G2_PACK].off[1] : 0u;
  r->gen = ROM_GBUI_GEN2;
  return 1;
}

/* ------------------------------------------------------------------- open */

static void fill_from_result(RomGbUi* gu, const LocResult* r) {
  gu->gen = r->gen;
  gu->font        = r->off[ROM_GBUI_OFF_FONT];
  gu->textbox     = r->off[ROM_GBUI_OFF_TEXTBOX];
  gu->cardframe   = r->off[ROM_GBUI_OFF_CARDFRAME];
  gu->badges      = r->off[ROM_GBUI_OFF_BADGES];
  gu->leaders     = r->off[ROM_GBUI_OFF_LEADERS];
  gu->playerpic   = r->off[ROM_GBUI_OFF_PLAYERPIC];
  gu->frames      = r->off[ROM_GBUI_OFF_FRAMES];
  gu->fontextra   = r->off[ROM_GBUI_OFF_FONTEXTRA];
  gu->cardpic_m   = r->off[ROM_GBUI_OFF_CARDPIC_M];
  gu->cardpic_f   = r->off[ROM_GBUI_OFF_CARDPIC_F];
  gu->cardgfx     = r->off[ROM_GBUI_OFF_CARDGFX];
  gu->pack_m      = r->off[ROM_GBUI_OFF_PACK_M];
  gu->pack_f      = r->off[ROM_GBUI_OFF_PACK_F];
  gu->playerpic_bank = r->playerpic_bank;
  gu->cardpic_colmajor = r->cardpic_colmajor;
  memcpy(gu->anchor, r->anchor, sizeof gu->anchor);
}

int rom_gbui_open(RomGbUi* gu, GbReadFn read, void* ctx, uint32_t size,
                  uint8_t* scratch, uint32_t scratch_len) {
  if (!gu) return 0;
  memset(gu, 0, sizeof *gu);
  gu->read = read; gu->ctx = ctx; gu->size = size;
  if (!read || !scratch || scratch_len < ROM_GBUI_SCRATCH_MIN) return 0;

  Scan s; memset(&s, 0, sizeof s);
  s.read = read; s.ctx = ctx; s.size = size; s.scratch = scratch; s.scratch_len = scratch_len;

  uint8_t banks; uint32_t id_hash;
  if (!parse_header(&s, &banks, &id_hash)) return 0;
  s.banks = banks;
  gu->banks = banks; gu->id_hash = id_hash;

  LocResult r;
  if (!locate(&s, &r)) { gu->gen = ROM_GBUI_NONE; return 0; }
  fill_from_result(gu, &r);
  gu->ok = 1;
  return 1;
}

/* FNV-1a over a RomGbUiLoc's off[] array THEN its anchor[] array (little-
 * endian byte order), so a single corrupted cached offset OR a single
 * corrupted cached anchor -- even one that still slides past its own field's
 * structural re-verification (font+16 still looks like a font) -- is caught
 * before any offset from the cache is trusted. anchor[] alone is what makes
 * revalidate_loc() EXACT rather than statistical (BACKLOG #71): see the
 * struct's own comment in rom_gbui.h. */
static uint32_t loc_check(const uint32_t off[13], const uint32_t anchor[ROM_GBUI_ANCH_COUNT]) {
  uint32_t h = 0x811C9DC5u;
  for (uint32_t i = 0; i < 13; i++) {
    uint8_t b[4] = { (uint8_t)off[i], (uint8_t)(off[i] >> 8),
                      (uint8_t)(off[i] >> 16), (uint8_t)(off[i] >> 24) };
    h = fnv1a(b, 4, h);
  }
  for (uint32_t i = 0; i < ROM_GBUI_ANCH_COUNT; i++) {
    uint8_t b[4] = { (uint8_t)anchor[i], (uint8_t)(anchor[i] >> 8),
                      (uint8_t)(anchor[i] >> 16), (uint8_t)(anchor[i] >> 24) };
    h = fnv1a(b, 4, h);
  }
  return h;
}

void rom_gbui_save_loc(const RomGbUi* gu, RomGbUiLoc* out) {
  if (!gu || !out) return;
  memset(out, 0, sizeof *out);
  out->id_hash = gu->id_hash;
  out->size    = gu->size;
  out->gen     = gu->gen;
  out->off[ROM_GBUI_OFF_FONT]      = gu->font;
  out->off[ROM_GBUI_OFF_TEXTBOX]   = gu->textbox;
  out->off[ROM_GBUI_OFF_CARDFRAME] = gu->cardframe;
  out->off[ROM_GBUI_OFF_BADGES]    = gu->badges;
  out->off[ROM_GBUI_OFF_LEADERS]   = gu->leaders;
  out->off[ROM_GBUI_OFF_PLAYERPIC] = gu->playerpic;
  out->off[ROM_GBUI_OFF_FRAMES]    = gu->frames;
  out->off[ROM_GBUI_OFF_FONTEXTRA] = gu->fontextra;
  out->off[ROM_GBUI_OFF_CARDPIC_M] = gu->cardpic_m;
  out->off[ROM_GBUI_OFF_CARDPIC_F] = gu->cardpic_f;
  out->off[ROM_GBUI_OFF_CARDGFX]   = gu->cardgfx;
  out->off[ROM_GBUI_OFF_PACK_M]    = gu->pack_m;
  out->off[ROM_GBUI_OFF_PACK_F]    = gu->pack_f;
  memcpy(out->anchor, gu->anchor, sizeof out->anchor);
  out->check = loc_check(out->off, out->anchor);
}

/* ------------------------------------------------- anchor re-derivation */
/* BACKLOG #71. Each of these re-reads a SHORT, FIXED-SIZE run at the cached
 * ANCHOR file offset (never the block itself), confirms it is still
 * byte-for-byte the same ScanCb signature locate() originally matched there,
 * decodes the exact same address/bank operand(s) the matching try_*()
 * function decodes, and rejects unless that recomputation lands EXACTLY on
 * the cached block offset -- never a plausibility check on the block's own
 * bytes (font_verify/distinct_tiles below still do that separately, as a
 * second, independent layer). A corrupted off[] with a recomputed `check`
 * cannot pass these: the ROM's own code bytes would have to independently
 * agree with the corruption, which means forging the cartridge, not the
 * cache record. One short read per call (the signature's own `look` bytes,
 * <= 33 B) -- see the header's cost note for the per-open() total. */

static int anchor_addr_bank(const Scan* s, ScanCb cb, uint32_t look,
                            uint32_t anchor, uint32_t addr_off, uint32_t bank_off,
                            uint32_t expect) {
  uint8_t w[40];
  if (look > sizeof w) return 0;
  if (anchor + look > s->size) return 0;
  if (!rd(s, anchor, w, look)) return 0;
  if (!cb(w)) return 0;
  uint16_t addr = rd16(w + addr_off);
  uint8_t bank = w[bank_off];
  return fileoff(bank, addr) == expect;
}

/* G1-C: the card-frame's bank is chosen at SCAN time by a uniqueness sweep
 * across every bank, not encoded in the opcode -- but off = bank*0x4000 +
 * (addr-0x4000) is a bijection for a fixed addr, so the bank is exactly
 * RECOVERABLE from the cached off and the freshly-read addr, then
 * re-verified with the identical fileoff() math locate() used. No sweep
 * needed at revalidation time at all (cheaper than the original scan). */
static int anchor_cardframe(const Scan* s, uint32_t anchor, uint32_t expect) {
  uint8_t w[33];
  if (anchor + 33 > s->size) return 0;
  if (!rd(s, anchor, w, 33)) return 0;
  if (!g1_cardframe_cb(w)) return 0;
  uint16_t addr = rd16(w + 1);
  if (addr < GB_WIN_LO) return 0;
  uint32_t rel = expect + GB_WIN_LO;
  if (rel < addr) return 0;
  rel -= addr;
  if (rel % GB_BANK) return 0;
  uint32_t bank = rel / GB_BANK;
  if (bank >= 256u) return 0;
  return fileoff((uint8_t)bank, addr) == expect;
}

/* G1-B: the anchor IS the 8-byte .FaceBadgeTiles data table itself (a data
 * signature, not a code one -- see g1_badges_cb); badges = anchor+8 exactly. */
static int anchor_g1_badges(const Scan* s, uint32_t anchor, uint32_t expect) {
  uint8_t w[8];
  if (anchor + 8 > s->size) return 0;
  if (!rd(s, anchor, w, 8)) return 0;
  if (!g1_badges_cb(w)) return 0;
  return anchor + 8 == expect;
}

/* G2-B/L: TrainerCard_Page2_LoadGFX's own (page-2) call site derives BOTH
 * leaders and badges from the ONE stored anchor -- the page-3 INCBIN's
 * byte-identity was a scan-time disambiguation rule (locate() needed it to
 * pick the canonical hit out of two candidates); once a specific offset is
 * cached, re-deriving it exactly from its own anchor is the strictly
 * stronger check, so no second anchor is stored for the page-3 copy. */
static int anchor_badgeleader(const Scan* s, uint32_t anchor,
                              uint32_t expect_leaders, uint32_t expect_badges) {
  uint8_t w[22];
  if (anchor + 22 > s->size) return 0;
  if (!rd(s, anchor, w, 22)) return 0;
  if (!g2_badgeleader_cb(w)) return 0;
  uint16_t laddr = rd16(w + 1); uint8_t lbank = w[8];
  uint16_t baddr = rd16(w + 13); uint8_t bbank = w[20];
  if (fileoff(lbank, laddr) != expect_leaders) return 0;
  if (fileoff(bbank, baddr) != expect_badges) return 0;
  return 1;
}

/* G2-P: GetCardPic (Crystal, colmajor) or Gold's own loader -- `crystal`
 * picks which signature/layout to re-check, exactly as locate() picked
 * which job matched (RomGbUiLoc has no separate flag for this: cardpic_f!=0
 * iff Crystal-shaped, the same rule rom_gbui_open_loc() already uses).
 * cardgfx and (Crystal only) cardpic_f are re-derived from the SAME anchor,
 * never a second one -- their addresses live in the same opcode run. */
static int anchor_cardpic(const Scan* s, uint32_t anchor, int crystal,
                          uint32_t expect_m, uint32_t expect_f, uint32_t expect_gfx) {
  if (crystal) {
    uint8_t w[33];
    if (anchor + 33 > s->size) return 0;
    if (!rd(s, anchor, w, 33)) return 0;
    if (!g2_cardpic_cm_cb(w)) return 0;
    uint8_t bank = w[20];
    uint32_t co = fileoff(bank, rd16(w + 1));
    uint32_t ko = fileoff(bank, rd16(w + 11));
    uint32_t to = fileoff(bank, rd16(w + 25));
    return co == expect_m && ko == expect_f && to == expect_gfx;
  }
  uint8_t w[26];
  if (anchor + 26 > s->size) return 0;
  if (!rd(s, anchor, w, 26)) return 0;
  if (!g2_cardpic_gold_cb(w)) return 0;
  uint8_t bank = w[10];
  uint32_t co = fileoff(bank, rd16(w + 1));
  return co == expect_m && expect_f == 0 && (co + 35u * 16u) == expect_gfx;
}

/* G2-K: the anchor IS PackGFXPointers' own file offset -- its bank is
 * anchor/GB_BANK, the identical relation try_pack() uses at scan time
 * (never a sweep, never stored separately). */
static int anchor_pack(const Scan* s, uint32_t anchor, uint32_t expect) {
  uint8_t w[8];
  if (anchor + 8 > s->size) return 0;
  if (!rd(s, anchor, w, 8)) return 0;
  if (!g2_pack_cb(w)) return 0;
  uint8_t bank = (uint8_t)(anchor / GB_BANK);
  uint16_t base = rd16(w + 4);
  return fileoff(bank, base) == expect;
}

/* Re-validate a cached loc against THIS ROM without a full rescan. Two
 * independent layers, BOTH required: (1) anchor re-derivation above --
 * exact, closes BACKLOG #71's MUT-D5b hole; (2) the same structural
 * verifier locate() itself used wherever one exists (font_verify/
 * distinct_tiles/all_blank/any_nonzero), kept as defense in depth against a
 * signature collision this module hasn't anticipated. Derived fields
 * (font/fontextra in Gen 2, cardgfx, cardpic_f) are checked for being
 * EXACTLY what their anchor derives, not merely in-bounds. Anything that
 * fails falls back to rom_gbui_open()'s full scan -- never a partial
 * accept. */
static int revalidate_loc(const Scan* s, const RomGbUiLoc* loc) {
  if (loc->gen == ROM_GBUI_GEN1) {
    uint32_t font = loc->off[ROM_GBUI_OFF_FONT];
    uint32_t textbox = loc->off[ROM_GBUI_OFF_TEXTBOX];
    uint32_t cardframe = loc->off[ROM_GBUI_OFF_CARDFRAME];
    uint32_t badges = loc->off[ROM_GBUI_OFF_BADGES];
    uint32_t pp = loc->off[ROM_GBUI_OFF_PLAYERPIC];

    if (!anchor_addr_bank(s, g1_font_cb, 18, loc->anchor[ROM_GBUI_ANCH_FONT], 7, 16, font))
      return 0;
    if (!anchor_addr_bank(s, g1_textbox_cb, 18, loc->anchor[ROM_GBUI_ANCH_TEXTBOX], 7, 16, textbox))
      return 0;
    if (!anchor_cardframe(s, loc->anchor[ROM_GBUI_ANCH_CARDFRAME], cardframe)) return 0;
    if (!anchor_g1_badges(s, loc->anchor[ROM_GBUI_ANCH_BADGES], badges)) return 0;
    if (!anchor_addr_bank(s, g1_playerpic_cb, 6, loc->anchor[ROM_GBUI_ANCH_PLAYERPIC], 1, 5, pp))
      return 0;

    if (!font_verify(s, font)) return 0;
    if (textbox + 512 > s->size || distinct_tiles(s, textbox, 2, 32) != 32) return 0;
    if (cardframe + 640 > s->size) return 0;
    if (distinct_tiles(s, cardframe, 2, 9) != 9) return 0;
    if (!all_blank(s, cardframe + 144, 352)) return 0;
    if (!any_nonzero(s, cardframe + 496, 16)) return 0;
    if (distinct_tiles(s, cardframe + 512, 2, 8) < 7) return 0;
    if (badges + 1024 > s->size || distinct_tiles(s, badges, 2, 64) != 64) return 0;
    uint8_t first;
    if (!rd(s, pp, &first, 1) || first != 0x77) return 0;
    return 1;
  }
  if (loc->gen == ROM_GBUI_GEN2) {
    uint32_t frames = loc->off[ROM_GBUI_OFF_FRAMES];
    uint32_t font = loc->off[ROM_GBUI_OFF_FONT];
    uint32_t fontextra = loc->off[ROM_GBUI_OFF_FONTEXTRA];
    uint32_t badges = loc->off[ROM_GBUI_OFF_BADGES];
    uint32_t leaders = loc->off[ROM_GBUI_OFF_LEADERS];
    uint32_t cardpic_m = loc->off[ROM_GBUI_OFF_CARDPIC_M];
    uint32_t cardpic_f = loc->off[ROM_GBUI_OFF_CARDPIC_F];
    uint32_t cardgfx = loc->off[ROM_GBUI_OFF_CARDGFX];
    uint32_t pack_m = loc->off[ROM_GBUI_OFF_PACK_M];
    uint32_t pack_f = loc->off[ROM_GBUI_OFF_PACK_F];
    int crystal = (cardpic_f != 0);   /* Crystal-shaped iff cardpic_f present */

    if (!anchor_addr_bank(s, g2_frame_cb, 22, loc->anchor[ROM_GBUI_ANCH_FRAMES], 9, 21, frames))
      return 0;
    if (frames < 1536u + 512u || frames - 1536u != font) return 0;
    if (font < 512u || font - 512u != fontextra) return 0;
    if (!anchor_badgeleader(s, loc->anchor[ROM_GBUI_ANCH_BADGELEADER], leaders, badges))
      return 0;
    if (!anchor_cardpic(s, loc->anchor[ROM_GBUI_ANCH_CARDPIC], crystal,
                        cardpic_m, cardpic_f, cardgfx))
      return 0;
    if (!anchor_pack(s, loc->anchor[ROM_GBUI_ANCH_PACK_M], pack_m)) return 0;
    if (crystal) {
      /* review D2: both pack anchors are genuine tables, so anchor_pack() alone
       * would accept pack_f := pack_m or the pair swapped; locate() emits them in
       * ascending scan order, so a genuine record always has PACK_M < PACK_F. */
      if (loc->anchor[ROM_GBUI_ANCH_PACK_F] <= loc->anchor[ROM_GBUI_ANCH_PACK_M]) return 0;
      if (!anchor_pack(s, loc->anchor[ROM_GBUI_ANCH_PACK_F], pack_f)) return 0;
    } else if (pack_f != 0 || loc->anchor[ROM_GBUI_ANCH_PACK_F] != 0) {
      return 0;
    }

    if (frames + 9 * 48 > s->size || distinct_tiles(s, frames, 1, 54) < 46) return 0;
    if (!font_verify(s, font)) return 0;
    if (fontextra + 512 > s->size || distinct_tiles(s, fontextra, 2, 32) != 32) return 0;
    if (badges + 44 * 16 > s->size || distinct_tiles(s, badges, 2, 44) != 44) return 0;
    if (leaders + 86 * 16 > s->size || distinct_tiles(s, leaders, 2, 86) < 63) return 0;
    if (cardpic_m + 35 * 16 > s->size) return 0;
    if (cardpic_f != 0 && (cardpic_f < cardpic_m || cardpic_f - cardpic_m != 0x230u)) return 0;
    if (cardgfx + 6 * 16 > s->size) return 0;
    if (pack_m == 0 || pack_m + 60 * 16 > s->size) return 0;
    if (pack_f != 0 && pack_f + 60 * 16 > s->size) return 0;
    if (crystal && pack_f == 0) return 0;   /* Crystal-shaped needs both */
    return 1;
  }
  return 0;
}

int rom_gbui_open_loc(RomGbUi* gu, GbReadFn read, void* ctx, uint32_t size,
                      uint8_t* scratch, uint32_t scratch_len,
                      const RomGbUiLoc* loc) {
  if (!gu) return 0;
  memset(gu, 0, sizeof *gu);
  gu->read = read; gu->ctx = ctx; gu->size = size;
  if (!read || !scratch || scratch_len < ROM_GBUI_SCRATCH_MIN) return 0;

  Scan s; memset(&s, 0, sizeof s);
  s.read = read; s.ctx = ctx; s.size = size; s.scratch = scratch; s.scratch_len = scratch_len;

  uint8_t banks; uint32_t id_hash;
  if (!parse_header(&s, &banks, &id_hash)) return 0;
  s.banks = banks;

  if (loc && loc->id_hash == id_hash && loc->size == size &&
      loc->check == loc_check(loc->off, loc->anchor) &&
      (loc->gen == ROM_GBUI_GEN1 || loc->gen == ROM_GBUI_GEN2) &&
      revalidate_loc(&s, loc)) {
    gu->banks = banks; gu->id_hash = id_hash; gu->gen = loc->gen;
    gu->font        = loc->off[ROM_GBUI_OFF_FONT];
    gu->textbox     = loc->off[ROM_GBUI_OFF_TEXTBOX];
    gu->cardframe   = loc->off[ROM_GBUI_OFF_CARDFRAME];
    gu->badges      = loc->off[ROM_GBUI_OFF_BADGES];
    gu->leaders     = loc->off[ROM_GBUI_OFF_LEADERS];
    gu->playerpic   = loc->off[ROM_GBUI_OFF_PLAYERPIC];
    gu->frames      = loc->off[ROM_GBUI_OFF_FRAMES];
    gu->fontextra   = loc->off[ROM_GBUI_OFF_FONTEXTRA];
    gu->cardpic_m   = loc->off[ROM_GBUI_OFF_CARDPIC_M];
    gu->cardpic_f   = loc->off[ROM_GBUI_OFF_CARDPIC_F];
    gu->cardgfx     = loc->off[ROM_GBUI_OFF_CARDGFX];
    gu->pack_m      = loc->off[ROM_GBUI_OFF_PACK_M];
    gu->pack_f      = loc->off[ROM_GBUI_OFF_PACK_F];
    if (gu->gen == ROM_GBUI_GEN1) gu->playerpic_bank = (uint8_t)(gu->playerpic / GB_BANK);
    gu->cardpic_colmajor = (gu->cardpic_f != 0);   /* Crystal-shaped iff cardpic_f present */
    memcpy(gu->anchor, loc->anchor, sizeof gu->anchor);
    gu->ok = 1;
    return 1;
  }

  return rom_gbui_open(gu, read, ctx, size, scratch, scratch_len);
}

/* ------------------------------------------------------------ tile/glyph */

#define GB8_TO_RGB15(v) ((uint16_t)((((v) >> 3) & 0x1Fu) | \
                          ((((v) >> 3) & 0x1Fu) << 5) | ((((v) >> 3) & 0x1Fu) << 10)))

static const uint16_t DMG_SHADE[4] = {
  GB8_TO_RGB15(0xF8), GB8_TO_RGB15(0xA8), GB8_TO_RGB15(0x58), GB8_TO_RGB15(0x10)
};

int rom_gbui_tile(RomGbUi* gu, uint32_t off, uint32_t index, uint8_t bpp,
                  uint32_t grid_w, uint32_t grid_h, int colmajor, uint16_t out[64]) {
  if (!gu || !gu->ok || !out) return 0;
  if (bpp != 1 && bpp != 2) return 0;
  if (grid_w || grid_h) {
    if (!grid_w || !grid_h) return 0;
    if (index >= grid_w * grid_h) return 0;
    if (colmajor) index = (index % grid_w) * grid_h + (index / grid_w);
  } else if (colmajor) {
    return 0;                            /* colmajor needs the grid          */
  }
  uint32_t stride = (bpp == 2) ? 16u : 8u;
  uint32_t toff = off + index * stride;
  if (toff < off) return 0;             /* overflow guard                   */
  if (toff >= gu->size || stride > gu->size - toff) return 0;

  uint8_t buf[16];
  if (!gu->read(gu->ctx, toff, buf, stride)) return 0;

  for (uint32_t y = 0; y < 8; y++) {
    uint8_t lo = (bpp == 2) ? buf[y * 2] : buf[y];
    uint8_t hi = (bpp == 2) ? buf[y * 2 + 1] : lo;  /* 1bpp: both planes, like the games' FarCopy*Double */
    for (uint32_t x = 0; x < 8; x++) {
      uint32_t b = 7u - x;
      uint8_t idx = (uint8_t)(((lo >> b) & 1u) | (((hi >> b) & 1u) << 1));
      out[y * 8 + x] = DMG_SHADE[idx];
    }
  }
  return 1;
}

int rom_gbui_glyph(RomGbUi* gu, uint8_t ch, uint16_t out[64]) {
  if (!gu || !gu->ok || !gu->font) return 0;
  if (ch < 0x80u) return 0;
  uint32_t tile = (uint32_t)(ch - 0x80u);
  return rom_gbui_tile(gu, gu->font, tile, 1, 16u, 8u, 0, out);
}
