/*
 * Fused-GB-corpus reader. See fused_gb.h for the format and why a locator record is
 * used instead of scanning for the trailer at end-of-cartridge. Dependency-free like
 * fused_rom.c/fused_sav.c: no tonc, no FatFs, just bounds-checked reads out of the
 * cartridge address window — pure-C core per the toolkit's golden rules.
 *
 * The parsing/lookup surface is real ONLY under PDNA_DELTA (BACKLOG #62's own delta-gb
 * build is the only place a GB directory is ever fused); everywhere else it is a
 * permanent "not fused" stand-in, same split gb_art_source.c already uses for its own
 * PDNA_DELTA/non-PDNA_DELTA halves. The locator record itself (g_pdna_gbd) is compiled
 * unconditionally so tools/fuse_gb.py can always find and patch it, mirroring
 * fused_rom.c/fused_sav.c's own g_pdna_fuse/g_pdna_sav.
 */
#include <string.h>
#include "fused_gb.h"
#include "pdna_romver.h"      /* pdna_rv_crc32* -- #62 D9's per-entry CRC verification;
                                * pure-C core, same as this file (no tonc/FatFs) */

#define CART_BASE 0x08000000u
#define CART_SPAN 0x02000000u          /* 32 MiB addressable cartridge window */

#define DIR_MAGIC_LEN 8
#define ENTRY_SIZE    52u             /* BACKLOG #98 format v2: type(4) + name(32) + offset(4)
                                       * + size(4) + crc32(4) + pair(4) */
#define TRAILER_SIZE  16u

/* `used` survives --gc-sections; `aligned(4)` so tools/fuse_gb.py can patch the two u32s
 * in place. Magic built element-by-element on purpose (see fused_rom.c's own comment):
 * a string literal could be pooled/duplicated and the tool requires it exactly once.
 *
 * #62 review D9: under FUSED_GB_TEST (never defined outside the host test's own cc
 * line) this is a plain, writable, non-`used`/non-`aligned` global instead -- the host
 * test builds its own directory and points this record at it directly; `const volatile`
 * would make that impossible, and neither `used` (no --gc-sections on a host test
 * binary) nor a patchable-in-the-ELF alignment (nothing patches a host binary) buys
 * anything here. See fused_gb.h's matching #ifdef on the extern declaration. */
#ifdef FUSED_GB_TEST
PdnaGbdRec g_pdna_gbd = { { 'P', 'D', 'N', 'A', 'G', 'B', 'D', '1' }, 0u, 0u };
#else
const volatile PdnaGbdRec __attribute__((used, aligned(4))) g_pdna_gbd = {
  { 'P', 'D', 'N', 'A', 'G', 'B', 'D', '1' }, 0u, 0u
};
#endif

#ifndef PDNA_DELTA

bool fused_gb_present(void) { return false; }
int  fused_gb_entry_count(void) { return 0; }
bool fused_gb_entry(int i, uint32_t* type, const char** name, uint32_t* size) {
  (void)i; (void)type; (void)name; (void)size;
  return false;
}
bool fused_gb_rom(uint8_t gen, const uint8_t** base, uint32_t* size) {
  (void)gen; (void)base; (void)size;
  return false;
}
int  fused_gb_save_count(void) { return 0; }
bool fused_gb_save(int i, const char** name, const uint8_t** base, uint32_t* size) {
  (void)i; (void)name; (void)base; (void)size;
  return false;
}
bool fused_gb_loc(uint8_t kind, uint8_t gen, const uint8_t** rec, uint32_t* rec_len,
                  uint32_t* id_hash, uint32_t* rom_size) {
  (void)kind; (void)gen; (void)rec; (void)rec_len; (void)id_hash; (void)rom_size;
  return false;
}
/* BACKLOG #98: harmless no-ops outside PDNA_DELTA -- nothing above ever answers
 * fused_gb_present()==true here, so no caller's lookup outcome can depend on these,
 * but every call site (the delta-gb save-pick code in pdna_main.c) compiles
 * unconditionally and must still link. */
void fused_gb_set_active_save(int idx) { (void)idx; }
int  fused_gb_get_active_save(void) { return -1; }
bool fused_gb_lookup_was_ambiguous(void) { return false; }
FusedGbFailReason fused_gb_lookup_failed_reason(void) { return FUSED_GB_FAIL_NONE; }

#else /* PDNA_DELTA */

/* #62 review D9: the one chokepoint every "byte at cartridge offset X" read below goes
 * through, so a host test can point the parser at a plain heap buffer instead of real
 * (unmapped, on a host) cartridge address space. uintptr_t-typed deliberately:
 * CART_BASE + off alone is 32-bit arithmetic, and a real host pointer (64-bit on this
 * Mac) would be silently truncated by a cast through it. Scoped to the PDNA_DELTA half
 * (its only callers, below) so a non-delta build never defines an unused static. On the
 * GBA build (32-bit pointers, FUSED_GB_TEST never defined) this compiles to the exact
 * same (CART_BASE + off) arithmetic as before -- zero behaviour change there. */
#ifdef FUSED_GB_TEST
const uint8_t* g_fused_gb_test_base;   /* host test sets this to a real buffer's start */
#endif

static const uint8_t* cart_ptr(uint32_t off) {
#ifdef FUSED_GB_TEST
  if (g_fused_gb_test_base) return g_fused_gb_test_base + off;
#endif
  return (const uint8_t*)((uintptr_t)CART_BASE + off);
}

/* #62 review D1: moved to EWRAM (528 B) -- the delta build's IWRAM/stack headroom is
 * the tight budget (10,208 B of a 10,920-B stack deep-chain), not EWRAM (1,748 B free
 * here); EWRAM_BSS is tonc/sys.h's name for this GCC section attribute, spelled out
 * directly so this pure-C core stays free of tonc/sys.h per the toolkit's golden
 * rules (no u8/u16 macro leakage). A handful of small fixed-size structs; parsed once,
 * on first use. s_parsed replaces the old "s_count == -1" sentinel: EWRAM_BSS is
 * zero-cleared at startup like any .bss, with NO initializer copy, so a nonzero initial
 * value (-1) would silently read back as 0 -- s_parsed/s_count are both correct at their
 * zero-init value (false/0) with no special-cased startup constant needed. */
#ifdef FUSED_GB_TEST
/* #62 review D9: ".sbss" is a devkitARM/gba.specs section name -- the host's mach-o
 * (or ELF-but-not-GBA) toolchain rejects it outright, and a host test has no EWRAM
 * budget to protect anyway. Plain statics here. */
#define GBD_EWRAM_BSS
#else
#define GBD_EWRAM_BSS __attribute__((section(".sbss")))
#endif

/* #68b review D1: `name` is a raw pointer into cartridge address space (see fused_gb.h's
 * caching comment), not a 32-byte copy -- the directory block the pointer targets stays
 * resident for the program's whole life, so nothing needs copying. Shrinks one cached
 * entry from 44 bytes to a (type,offset,size,name-pointer) tuple, making room for
 * FUSED_GB_MAX_ENTRIES 12->24 at a net DECREASE in total cache footprint. */
typedef struct {
  uint32_t    type;
  uint32_t    offset;
  uint32_t    size;
  const char* name;
} GbdEntry;

static GBD_EWRAM_BSS GbdEntry s_entry[FUSED_GB_MAX_ENTRIES];
static GBD_EWRAM_BSS bool     s_parsed;   /* false = not parsed yet */
static GBD_EWRAM_BSS int      s_count;    /* valid once s_parsed is true, 0..N (N clamped) */

/* BACKLOG #98: `pair` cached as its own tightly-packed uint8_t[] parallel to s_entry[],
 * NOT a 5th GbdEntry field -- a uint32_t `pair` member would cost a full 4 bytes per
 * entry (96 B over FUSED_GB_MAX_ENTRIES=24) AND, because GbdEntry's trailing pointer
 * already forces 4-byte struct alignment, even a uint8_t member embedded in the struct
 * would be padded right back up to the same 4-byte cost -- only pulling it OUT into its
 * own byte array actually avoids the padding tax (24 B total, no padding between
 * elements). CACHE_NO_PAIR (0xFF) is this array's OWN "no pairing" sentinel -- distinct
 * from the on-disk FUSED_GB_NO_PAIR (0xFFFFFFFF): parse_once() below is the one place
 * that translates between them, collapsing an out-of-range on-disk `pair` (either the
 * real NO_PAIR, or a corrupt/foreign value past FUSED_GB_MAX_ENTRIES-1 that could never
 * fit a byte anyway) to CACHE_NO_PAIR rather than truncating it into looking like some
 * OTHER, wrong, in-range index. FUSED_GB_MAX_ENTRIES (24) is nowhere near 0xFF (255),
 * so every real index has room. */
#define CACHE_NO_PAIR 0xFFu
static GBD_EWRAM_BSS uint8_t s_pair[FUSED_GB_MAX_ENTRIES];

/* BACKLOG #98: which fused_gb_save() index is "active" (-1 = none) -- set by
 * fused_gb_set_active_save(), read by fused_gb_rom()/fused_gb_loc() to resolve
 * through the active save's own `pair` field instead of guessing by generation
 * alone. Independent of s_parsed/s_count: a caller may set this before parse_once()
 * has ever run (e.g. at boot-picker time, before any other fused_gb_* call this
 * session), so it is never cleared by parse_once() itself. */
static GBD_EWRAM_BSS int  s_active_save;      /* 0-init means "none set" is WRONG here --
                                               * see fused_gb_set_active_save()'s own
                                               * comment for how the true default (-1)
                                               * is established despite EWRAM_BSS's
                                               * zero-init, same s_parsed sentinel
                                               * problem parse_once()'s own comment
                                               * documents. */
static GBD_EWRAM_BSS bool s_active_save_set;  /* true once fused_gb_set_active_save() has
                                               * actually been called -- the zero-init
                                               * sentinel s_active_save itself cannot
                                               * carry (0 is a valid save index). */
static GBD_EWRAM_BSS FusedGbFailReason s_lookup_fail_reason; /* fused_gb_lookup_failed_
                                               * reason()'s backing store -- reset to
                                               * FUSED_GB_FAIL_NONE (== 0, so EWRAM_BSS's
                                               * own zero-init already starts it right)
                                               * at the top of every fused_gb_rom()/
                                               * fused_gb_loc() call. fused_gb_lookup_
                                               * was_ambiguous() is a thin wrapper over
                                               * this == FUSED_GB_FAIL_AMBIGUOUS. */

#ifdef FUSED_GB_TEST
/* #62 review D9: the real app never re-fuses mid-session (the corpus is immutable for
 * a whole boot), so there is deliberately no public "reparse" entry point -- but a host
 * test that wants a FRESH parse_once() per case (rather than one process per case) needs
 * exactly that. Test-only, never declared outside FUSED_GB_TEST. */
void fused_gb_test_reset(void) {
  s_parsed = false;
  s_count = 0;
  memset(s_entry, 0, sizeof s_entry);
  memset(s_pair, CACHE_NO_PAIR, sizeof s_pair);
  s_active_save = 0;
  s_active_save_set = false;
  s_lookup_fail_reason = FUSED_GB_FAIL_NONE;
}
#endif

static bool dir_present_raw(uint32_t* dir_off, uint32_t* dir_size) {
  uint32_t off = g_pdna_gbd.offset;
  uint32_t sz  = g_pdna_gbd.size;
  if (!sz || !off) return false;                                /* unfused state */
  if (off >= CART_SPAN || sz > CART_SPAN - off) return false;    /* corrupt/hand-edited */
  if (sz < 12u + TRAILER_SIZE) return false;                     /* too small to hold anything */
  if (dir_off) *dir_off = off;
  if (dir_size) *dir_size = sz;
  return true;
}

/* Parses the directory into s_entry[]/s_count exactly once. Never trusts a single field
 * in isolation: the entry count implies an exact directory size, cross-checked against
 * the record's own `size` AND the trailer's restated size/magic, same defense-in-depth
 * tools/fuse_gb.py's own verify pass uses. A malformed directory degrades to
 * s_count == 0 (acts unfused) rather than reading out of bounds. */
static void parse_once(void) {
  if (s_parsed) return;
  s_parsed = true;
  s_count = 0;

  uint32_t dir_off, dir_size;
  if (!dir_present_raw(&dir_off, &dir_size)) return;

  const uint8_t* blk = cart_ptr(dir_off);
  /* BACKLOG #98: "PDNAGBD2" -- the DIRECTORY BLOCK's own magic, deliberately distinct
   * from g_pdna_gbd's "PDNAGBD1" locator magic above (which tools/fuse_gb.py's
   * locate_record() still searches the UN-fused binary for, unchanged): this is the
   * entry-FORMAT version signal. A v1 directory (48-byte entries, no `pair` field)
   * fails this check and parses as unfused rather than being misread through the v2
   * field layout below. */
  if (memcmp(blk, "PDNAGBD2", DIR_MAGIC_LEN) != 0) return;

  uint32_t count;
  memcpy(&count, blk + 8, 4);
  uint64_t expect = 12u + (uint64_t)ENTRY_SIZE * count + TRAILER_SIZE;
  if (expect != dir_size) return;

  uint32_t trailer_off = 12u + ENTRY_SIZE * count;
  uint32_t t_size;
  memcpy(&t_size, blk + trailer_off, 4);
  if (t_size != dir_size) return;
  if (memcmp(blk + trailer_off + 4, "PDNAGBD2", DIR_MAGIC_LEN) != 0) return;

  int n = (int)count;
  if (n > FUSED_GB_MAX_ENTRIES) n = FUSED_GB_MAX_ENTRIES;   /* cache the first N; see .h */

  /* #62 review D9: verify each entry's own crc32 field (tools/fuse_gb.py writes
   * zlib.crc32(payload) at byte 44 of every 48-byte record) against the payload it
   * actually points at, same standard CRC-32 pdna_romver.c already uses for the whole-
   * ROM self-check (same polynomial zlib uses -- pdna_rv_crc32() IS zlib.crc32, just a
   * from-scratch pure-C implementation). One-time cost at boot (parse_once() runs once,
   * cached): the fused corpus is at most 32 MiB and this only ever runs under
   * PDNA_DELTA, which never runs on real hardware -- an mGBA/Delta boot pays this once,
   * not per fetch. Table is local/stack (64 B), not persisted. */
  uint32_t crc_tab[16];
  pdna_rv_crc32_table(crc_tab);

  for (int i = 0; i < n; i++) {
    const uint8_t* e = blk + 12u + (uint32_t)i * ENTRY_SIZE;
    uint32_t type, off, size, crc, pair;
    memcpy(&type, e + 0, 4);
    memcpy(&off,  e + 36, 4);
    memcpy(&size, e + 40, 4);
    memcpy(&crc,  e + 44, 4);
    memcpy(&pair, e + 48, 4);   /* BACKLOG #98: format v2's new field -- NOT part of the
                                 * crc32 (crc32 still covers only the PAYLOAD bytes, same
                                 * as v1), so a corrupt `pair` alone cannot be caught by
                                 * the CRC check below; it is bounds-checked on its own
                                 * (against `n`, the clamped entry count) wherever it is
                                 * actually resolved, never trusted blindly. */
    /* #62 review D6: bounded against dir_off, not the whole 32-MiB CART_SPAN --
     * every fused payload precedes the directory (tools/fuse_gb.py's own layout), so
     * a corrupt/adversarial entry that claims to run PAST the directory (into the
     * directory's own bytes, the trailer, or unmapped cartridge space beyond the
     * fused image) is dropped here instead of being handed to a caller as real. */
    if (off >= dir_off || size > dir_off - off) continue;
    if (pdna_rv_crc32(crc_tab, cart_ptr(off), size) != crc) continue;   /* #62 D9 */
    /* #68b review D1: name is cached as a pointer straight at e+4 (still inside the
     * resident directory block cart_ptr() already resolved `e` from) instead of a
     * copy. A well-formed entry (tools/fuse_gb.py always NUL-pads) has a NUL within
     * its 32 bytes; a hand-edited/corrupt one that doesn't gets the shared empty
     * string instead of an unbounded read past the field. */
    const uint8_t* name_field = e + 4;
    bool name_nul = false;
    for (uint32_t k = 0; k < FUSED_GB_NAME_MAX; k++) {
      if (name_field[k] == 0) { name_nul = true; break; }
    }
    s_entry[i].type = type;
    s_entry[i].offset = off;
    s_entry[i].size = size;
    s_entry[i].name = name_nul ? (const char*)name_field : "";
    /* BACKLOG #98: collapse the on-disk pair to the cache's own byte-sized sentinel
     * -- see s_pair[]'s own comment for why this lives in a parallel array instead
     * of a 5th GbdEntry field. Any value that cannot possibly be a real cached
     * index (the real NO_PAIR, or anything >= FUSED_GB_MAX_ENTRIES -- including a
     * corrupt/foreign directory's `pair` that happens to collide with the byte
     * 0xFF by chance) becomes CACHE_NO_PAIR, never silently truncated into looking
     * like some OTHER in-range index. */
    s_pair[i] = (pair < (uint32_t)FUSED_GB_MAX_ENTRIES) ? (uint8_t)pair : CACHE_NO_PAIR;
  }
  s_count = n;
}

/* BACKLOG #98: true iff a cached s_pair[] value points at an entry actually within the
 * (clamped) parsed range -- a directory built by a tool other than tools/fuse_gb.py (or
 * corrupted) could claim a pair index the cache never even held (parse_once() already
 * collapses anything >= FUSED_GB_MAX_ENTRIES to CACHE_NO_PAIR, but s_count can be
 * SMALLER than FUSED_GB_MAX_ENTRIES too -- e.g. a 6-entry directory with a stray pair
 * byte of 20). Every resolver below calls this before trusting a cached pair value, so
 * an out-of-range claim degrades to "no pairing" (falls through to the single-entry
 * fallback / ambiguous-fail path) instead of an out-of-bounds s_entry[] read. */
static bool pair_in_range(uint8_t pair) {
  return pair != CACHE_NO_PAIR && (int)pair < s_count;
}

/* BACKLOG #98 D1 review fix: the old bool-returning version collapsed THREE distinct
 * cases into one "did not resolve" and let the caller treat all of them the same --
 * fall back to resolve_single_idx(), the "exactly one ROM of this generation" rule.
 * That fallback is only ever correct for the first case below: it is the ONE-ROM,
 * no-picker-needed scenario the pre-#98 code always handled. The other two cases mean
 * an active save genuinely WAS chosen and its own pairing is broken -- falling back
 * there is exactly the #98 bug (an orphaned Gold.sav silently borrowing a lone
 * Crystal.gbc that has nothing to do with it). So this now returns a tri-state:
 *
 *   ACTIVE_RESOLVE_NONE      no active save is set, OR the active save's own index
 *                            doesn't match any SAV entry actually present (defensive;
 *                            should not happen from a real picker, but degrades the
 *                            same as "no active save" rather than a hard failure) --
 *                            the single-entry fallback is the right next step.
 *   ACTIVE_RESOLVE_OK        the active save resolved to *out_idx, an entry of exactly
 *                            `want_type`.
 *   ACTIVE_RESOLVE_ORPHANED  the active save's `pair` is FUSED_GB_NO_PAIR or out of
 *                            range -- it was never fused with any ROM.
 *   ACTIVE_RESOLVE_MISMATCH  the active save's `pair` resolves to a real entry, but
 *                            that entry's own type isn't `want_type` (e.g. the active
 *                            save is Gen-1 but the caller asked for Gen-2 art).
 *
 * Callers MUST NOT run the single-entry fallback for ORPHANED/MISMATCH -- only NONE. */
typedef enum {
  ACTIVE_RESOLVE_NONE = 0,
  ACTIVE_RESOLVE_OK,
  ACTIVE_RESOLVE_ORPHANED,
  ACTIVE_RESOLVE_MISMATCH,
} ActiveResolveResult;

static ActiveResolveResult resolve_active_rom_idx(uint32_t want_type, int* out_idx) {
  if (!s_active_save_set || s_active_save < 0) return ACTIVE_RESOLVE_NONE;
  int seen = -1;
  for (int i = 0; i < s_count; i++) {
    if (s_entry[i].type != FUSED_GB_SAV) continue;
    seen++;
    if (seen != s_active_save) continue;
    uint8_t pair = s_pair[i];
    if (!pair_in_range(pair)) return ACTIVE_RESOLVE_ORPHANED;
    if (s_entry[pair].type != want_type) return ACTIVE_RESOLVE_MISMATCH;
    *out_idx = (int)pair;
    return ACTIVE_RESOLVE_OK;
  }
  return ACTIVE_RESOLVE_NONE;   /* s_active_save is past the last SAV entry actually
                                 * present -- same posture as "no active save". */
}

/* BACKLOG #98: the pre-#98 fallback -- when EXACTLY ONE entry of `want_type` is fused,
 * that one is unambiguous regardless of any active-save pairing (this is what keeps
 * every existing single-ROM-per-generation image working unchanged). Returns false
 * (leaving *out_idx untouched) if there are zero or more than one; `*out_count` (if
 * non-NULL) always gets the real count either way, so a caller can tell "nothing of
 * this generation exists" (0) apart from "genuinely ambiguous" (>1) -- only the
 * latter is a fail-loudly case. */
static bool resolve_single_idx(uint32_t want_type, int* out_idx, int* out_count) {
  int idx = -1, count = 0;
  for (int i = 0; i < s_count; i++) {
    if (s_entry[i].type == want_type) { idx = i; count++; }
  }
  if (out_count) *out_count = count;
  if (count != 1) return false;
  *out_idx = idx;
  return true;
}

void fused_gb_set_active_save(int idx) {
  s_active_save = idx;
  s_active_save_set = true;
}

int fused_gb_get_active_save(void) {
  return s_active_save_set ? s_active_save : -1;
}

FusedGbFailReason fused_gb_lookup_failed_reason(void) {
  return s_lookup_fail_reason;
}

bool fused_gb_lookup_was_ambiguous(void) {
  return s_lookup_fail_reason == FUSED_GB_FAIL_AMBIGUOUS;
}

bool fused_gb_present(void) {
  parse_once();
  return s_count > 0;
}

int fused_gb_entry_count(void) {
  parse_once();
  return s_count > 0 ? s_count : 0;
}

bool fused_gb_entry(int i, uint32_t* type, const char** name, uint32_t* size) {
  parse_once();
  if (i < 0 || i >= s_count) return false;
  if (type) *type = s_entry[i].type;
  if (name) *name = s_entry[i].name;
  if (size) *size = s_entry[i].size;
  return true;
}

bool fused_gb_rom(uint8_t gen, const uint8_t** base, uint32_t* size) {
  parse_once();
  s_lookup_fail_reason = FUSED_GB_FAIL_NONE;
  uint32_t want = (gen == 1) ? FUSED_GB_ROM_GEN1 : (gen == 2) ? FUSED_GB_ROM_GEN2 : 0u;
  if (!want) return false;

  int idx;
  /* BACKLOG #98 D1 review fix: the active save's own pairing wins when it applies. An
   * ORPHANED/MISMATCH result means an active save genuinely WAS chosen and its own
   * pairing is broken -- that must fail loudly, NOT fall back to the single-entry
   * heuristic (that heuristic answering "the only ROM of this generation" would be
   * some OTHER, unrelated ROM in exactly the case this review caught). The fallback
   * only ever runs for ACTIVE_RESOLVE_NONE -- no active save to resolve at all. */
  ActiveResolveResult ar = resolve_active_rom_idx(want, &idx);
  if (ar == ACTIVE_RESOLVE_ORPHANED) { s_lookup_fail_reason = FUSED_GB_FAIL_ORPHANED; return false; }
  if (ar == ACTIVE_RESOLVE_MISMATCH) { s_lookup_fail_reason = FUSED_GB_FAIL_GEN_MISMATCH; return false; }
  if (ar != ACTIVE_RESOLVE_OK) {
    int count = 0;
    if (!resolve_single_idx(want, &idx, &count)) {
      if (count > 1) s_lookup_fail_reason = FUSED_GB_FAIL_AMBIGUOUS;
      return false;
    }
  }
  if (base) *base = cart_ptr(s_entry[idx].offset);
  if (size) *size = s_entry[idx].size;
  return true;
}

int fused_gb_save_count(void) {
  parse_once();
  int n = 0;
  for (int i = 0; i < s_count; i++) if (s_entry[i].type == FUSED_GB_SAV) n++;
  return n;
}

bool fused_gb_save(int i, const char** name, const uint8_t** base, uint32_t* size) {
  parse_once();
  if (i < 0) return false;
  for (int k = 0; k < s_count; k++) {
    if (s_entry[k].type != FUSED_GB_SAV) continue;
    if (i-- != 0) continue;
    if (name) *name = s_entry[k].name;
    if (base) *base = cart_ptr(s_entry[k].offset);
    if (size) *size = s_entry[k].size;
    return true;
  }
  return false;
}

/* BACKLOG #68b: the [header+record] framing tools/gbloc_driver.c writes for each LOC
 * payload -- magic(8) + kind(1) + gen(1) + rec_size(2, LE) + id_hash(4, LE) +
 * rom_size(4, LE) = 20 bytes, followed by rec_size bytes of raw struct. Parsed by hand
 * (memcpy at fixed byte offsets) rather than cast through a C struct, same posture the
 * directory entries themselves already use above -- no alignment assumption on the
 * cartridge byte stream either way. */
#define GB_LOC_HDR_SIZE 20u

/* BACKLOG #98: scans for the (kind,gen) LOC entry paired (via its own `pair` field)
 * with directory index `rom_idx`. Shared by both branches of fused_gb_loc() below --
 * once a ROM's directory index is known (however it was resolved), finding ITS LOC
 * entry is identical either way. Returns false (no ambiguity implied -- a ROM the
 * shipped locators could not place this table in just has no such entry, see
 * tools/fuse_gb.py's gbloc_payloads() comment) if none matches. */
static bool find_loc_for_rom(int rom_idx, uint8_t kind, uint8_t gen,
                             const uint8_t** rec, uint32_t* rec_len,
                             uint32_t* id_hash, uint32_t* rom_size) {
  for (int i = 0; i < s_count; i++) {
    if (s_entry[i].type != FUSED_GB_LOC) continue;
    if (s_pair[i] != (uint8_t)rom_idx) continue;
    if (s_entry[i].size < GB_LOC_HDR_SIZE) continue;   /* too small to hold the header */
    const uint8_t* p = cart_ptr(s_entry[i].offset);
    if (memcmp(p, "PDNALOC1", 8) != 0) continue;
    if (p[8] != kind || p[9] != gen) continue;
    uint16_t rsize; memcpy(&rsize, p + 10, 2);
    if ((uint32_t)GB_LOC_HDR_SIZE + rsize != s_entry[i].size) continue;  /* framing check */
    if (rec) *rec = p + GB_LOC_HDR_SIZE;
    if (rec_len) *rec_len = rsize;
    if (id_hash) { uint32_t v; memcpy(&v, p + 12, 4); *id_hash = v; }
    if (rom_size) { uint32_t v; memcpy(&v, p + 16, 4); *rom_size = v; }
    return true;
  }
  return false;
}

bool fused_gb_loc(uint8_t kind, uint8_t gen, const uint8_t** rec, uint32_t* rec_len,
                  uint32_t* id_hash, uint32_t* rom_size) {
  parse_once();
  s_lookup_fail_reason = FUSED_GB_FAIL_NONE;
  uint32_t want = (gen == 1) ? FUSED_GB_ROM_GEN1 : (gen == 2) ? FUSED_GB_ROM_GEN2 : 0u;
  if (!want) return false;

  int rom_idx;
  /* BACKLOG #98 D1 review fix: resolve WHICH ROM's LOC entry we want, same tri-state
   * rule fused_gb_rom() uses (active-save pairing wins; ORPHANED/MISMATCH fails loudly
   * instead of falling back; the single-entry fallback only for ACTIVE_RESOLVE_NONE)
   * -- see that function's own comment. Once rom_idx is known, find_loc_for_rom() does
   * the actual (kind,gen) search; "no LOC entry for this ROM" is a plain false, not
   * an ambiguity -- only "could not even tell which ROM" is. */
  ActiveResolveResult ar = resolve_active_rom_idx(want, &rom_idx);
  if (ar == ACTIVE_RESOLVE_ORPHANED) { s_lookup_fail_reason = FUSED_GB_FAIL_ORPHANED; return false; }
  if (ar == ACTIVE_RESOLVE_MISMATCH) { s_lookup_fail_reason = FUSED_GB_FAIL_GEN_MISMATCH; return false; }
  if (ar != ACTIVE_RESOLVE_OK) {
    int count = 0;
    if (!resolve_single_idx(want, &rom_idx, &count)) {
      if (count > 1) s_lookup_fail_reason = FUSED_GB_FAIL_AMBIGUOUS;
      return false;
    }
  }
  return find_loc_for_rom(rom_idx, kind, gen, rec, rec_len, id_hash, rom_size);
}

#endif /* PDNA_DELTA */

/* Compiled unconditionally: outside PDNA_DELTA it is simply never reached (nothing
 * calls fused_gb_rom() to obtain a slice to read), but keeping it out of the #ifdef
 * avoids a fifth near-duplicate stub. */
#ifdef PDNA_DELTA
/* BACKLOG #263: the emulator has no SD card, so its `sd Nr/Ns` counter would read 0 for
 * the Game Boy art path. Every GbReadFn call over a fused ROM is the stand-in for one
 * f_lseek+f_read on the SD build, so this MODELS what FatFs's f_read would have asked the
 * card for, and gb_art_source.c folds the totals into perf_sd:
 *   - a request that starts or ends mid-sector goes through the FIL's one-sector buffer:
 *     it costs one disk_read the first time that sector is touched and nothing while the
 *     next reads land in the same sector (the buffer already holds it);
 *   - whole sectors go straight to the caller's buffer: one disk_read per contiguous run.
 * fused_gb_read_reopen() drops the modelled buffer, like a fresh f_open() does.
 * NOT modelled (the emulator cannot see them): f_open's directory walk, the .loc file's
 * open+read, FAT-chain lookups on f_lseek. EWRAM, 12 bytes, delta only. */
static GBD_EWRAM_BSS uint32_t s_rd_calls, s_rd_sects;
static GBD_EWRAM_BSS uint32_t s_rd_cached;        /* sector index + 1 held by the buffer; 0 = none */
uint32_t fused_gb_read_calls(void) { return s_rd_calls; }
uint32_t fused_gb_read_sects(void) { return s_rd_sects; }
void fused_gb_read_reopen(void) { s_rd_cached = 0; }

static void count_read(uint32_t off, uint32_t len) {
  if (!len) return;
  uint32_t s0 = off >> 9, s1 = (off + len - 1u) >> 9;
  bool in_run = false;                            /* previous sector was part of a direct run */
  for (uint32_t s = s0; s <= s1; s++) {           /* bounded: len <= slice size */
    bool partial = (s == s0 && (off & 511u)) || (s == s1 && ((off + len) & 511u));
    if (partial) {
      in_run = false;
      if (s_rd_cached != s + 1u) { s_rd_calls++; s_rd_sects++; s_rd_cached = s + 1u; }
    } else {
      if (!in_run) s_rd_calls++;
      s_rd_sects++;
      in_run = true;
    }
  }
}
#endif

bool fused_gb_slice_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  const FusedGbSlice* s = (const FusedGbSlice*)ctx;
  if (!s || !dst) return false;
#ifdef PDNA_DELTA
  count_read(off, len);
#endif
  if (off > s->size || len > s->size - off) return false;   /* short read == failure, as FatFs */
  if (!len) return true;
  memcpy(dst, s->base + off, len);
  return true;
}
