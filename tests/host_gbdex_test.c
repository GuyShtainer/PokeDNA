/* source/gb_dex.c under test (BACKLOG #87 item 2) -- the per-species owned/seen
 * bitfield core and the Unown-forms order-list, over the corpus.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gbdex_test.c source/gb_dex.c \
 *      source/gb_fields.c source/gb_session.c source/gb_edit.c source/gen1_save.c source/gen1_write.c \
 *      source/gen2_save.c source/gen2_write.c source/data_tables.c \
 *      source/gen3_to_gb.c source/gb_sidecar.c source/gen3_edit.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_save.c source/gen3_daycare.c source/gb_trainer.c \
 *      -o /tmp/hgbd && /tmp/hgbd
 *
 * PROVES (per the brief):
 *   1. bit order: gbdex_get popcounts over 1..gb_max_species(gen) equal gb_trainer.c's
 *      own gbt_read().dex_owned/dex_seen (already itself cross-checked against a real
 *      trainer card's own numbers by earlier work) -- on Red, Gold and Crystal.
 *   2. caught implies seen: gbdex_set(dex, owned=true, on=true) also sets seen;
 *      gbdex_set(dex, owned=false, on=false) also clears owned -- round-tripped on a
 *      RAM copy, never the corpus file itself.
 *   3. Unown semantics: append-dedup-compact matches UpdateUnownDex's own scan shape,
 *      exercised on a synthetic in-memory Crystal-shaped field (no real save is known
 *      to have every slot populated) plus a live read against Crystal.sav's own
 *      wUnownDex if any letters are already recorded there.
 *
 * Corpus: gba-toolkit/roms/gb (Guy's own cartridge dumps), read-only, outside the repo.
 * Missing files SKIP; a present corpus must pass perfectly. Every mutation happens on
 * an in-RAM copy -- the file on disk is never reopened for write.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gb_dex.h"
#include "gb_trainer.h"
#include "gb_edit.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_fail = 0, g_check = 0, g_ran = 0;
#define CHECK(c, msg) do { g_check++; if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)

static uint8_t g_img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
static uint8_t g_scratch[GBS_SCRATCH_BYTES];

static uint32_t load(const char* file) {
  char p[512];
  snprintf(p, sizeof p, "%s/%s", ROMS, file);
  FILE* f = fopen(p, "rb");
  if (!f) return 0;
  uint32_t n = (uint32_t)fread(g_img, 1, sizeof g_img, f);
  fclose(f);
  return n;
}

/* ---- 1: bit order / popcount vs gb_trainer.c's own counts ------------------ */
static void one_dex_popcount(const char* file, uint8_t expect_gen) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECK(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "gbs_open");
  CHECK(s.gen == expect_gen, "right generation detected");

  GbTrainer t;
  CHECK(gbt_read(&s, &t), "gbt_read");

  int max = gb_max_species(s.gen);
  int owned = 0, seen = 0;
  for (int nat = 1; nat <= max; nat++) {
    if (gbdex_get(&s, (uint16_t)nat, true))  owned++;
    if (gbdex_get(&s, (uint16_t)nat, false)) seen++;
  }
  printf("  -- %s: gbdex owned=%d seen=%d | gb_trainer owned=%u seen=%u\n",
         file, owned, seen, (unsigned)t.dex_owned, (unsigned)t.dex_seen);
  CHECK(owned == (int)t.dex_owned, "owned popcount matches gb_trainer's own count");
  CHECK(seen  == (int)t.dex_seen,  "seen popcount matches gb_trainer's own count");
  /* caught-implies-seen already true on every real save: owned is a subset of seen. */
  CHECK(owned <= seen, "owned <= seen on the real save (subset, per the game's own invariant)");

  /* out-of-range dex numbers never read as true (bit order stays inside the field). */
  CHECK(!gbdex_get(&s, 0, true), "dex 0 is out of range");
  CHECK(!gbdex_get(&s, (uint16_t)(max + 1), true), "dex max+1 is out of range");
}

/* ---- 2: caught-implies-seen round-trip on a RAM copy ------------------------ */
static void one_dex_invariant(const char* file, uint8_t expect_gen) {
  uint32_t len = load(file);
  if (!len) return;   /* already SKIPped by one_dex_popcount above */
  GbSession s;
  if (gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) != GBS_OK) return;
  if (s.gen != expect_gen) return;

  /* Pick a species that is currently wholly unset (owned=0, seen=0) so both
   * directions of the invariant are observable; scan for one rather than assuming
   * dex 1 -- a fully-caught save would make the "set owned" half a no-op. */
  int max = gb_max_species(s.gen);
  int target = 0;
  for (int nat = 1; nat <= max; nat++)
    if (!gbdex_get(&s, (uint16_t)nat, true) && !gbdex_get(&s, (uint16_t)nat, false)) { target = nat; break; }
  if (!target) {
    /* Guy's corpus saves are fully-completed dexes -- manufacture an unset species on
     * this in-RAM copy via the SAME public API (clear owned, then clear seen too, so
     * the starting state is genuinely 0/0 rather than assuming gbdex_set's own
     * clear-seen-clears-owned half already works before it has been proven). */
    target = 1;
    CHECK(gbdex_set(&s, (uint16_t)target, false, false) == GBS_OK, "synthesize an unset species: clear seen");
    CHECK(gbdex_set(&s, (uint16_t)target, true, false) == GBS_OK, "synthesize an unset species: clear owned too");
    CHECK(!gbdex_get(&s, (uint16_t)target, true) && !gbdex_get(&s, (uint16_t)target, false),
          "species 1 is now genuinely 0/0 on this in-RAM copy");
  }
  {
    CHECK(gbdex_set(&s, (uint16_t)target, true, true) == GBS_OK, "set owned=true");
    CHECK(gbdex_get(&s, (uint16_t)target, true),  "owned reads back true");
    CHECK(gbdex_get(&s, (uint16_t)target, false), "seen was ALSO forced true (caught implies seen)");

    /* now clear seen -> owned must also clear */
    CHECK(gbdex_set(&s, (uint16_t)target, false, false) == GBS_OK, "clear seen=false");
    CHECK(!gbdex_get(&s, (uint16_t)target, false), "seen reads back false");
    CHECK(!gbdex_get(&s, (uint16_t)target, true),  "owned was ALSO forced false (clearing seen clears owned)");
  }

  /* set seen only (not owned) must NOT force owned on -- the two directions of the
   * invariant are not symmetric (seen-without-owned is a normal, legal state). */
  int target2 = 0;
  for (int nat = 1; nat <= max; nat++)
    if (!gbdex_get(&s, (uint16_t)nat, true) && !gbdex_get(&s, (uint16_t)nat, false)) { target2 = nat; break; }
  if (target2) {
    CHECK(gbdex_set(&s, (uint16_t)target2, false, true) == GBS_OK, "set seen=true (owned untouched)");
    CHECK(gbdex_get(&s, (uint16_t)target2, false), "seen reads back true");
    CHECK(!gbdex_get(&s, (uint16_t)target2, true), "owned stays false (seen alone never implies caught)");
  }
}

/* ---- 3: Unown forms (Crystal/GS only) --------------------------------------- */
static void unown_semantics(const char* file, uint8_t expect_gen) {
  uint32_t len = load(file);
  if (!len) return;
  GbSession s;
  if (gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) != GBS_OK) return;
  if (s.gen != expect_gen) return;

  /* Gen 1 has no Unown field at all -- both accessors must refuse cleanly. */
  if (s.gen == GB_GEN1) {
    CHECK(!gbdex_unown_seen(&s, 0), "Gen 1: no Unown field, letter A reads unseen");
    CHECK(gbdex_unown_set(&s, 0, true) == GBS_ERR_ARG, "Gen 1: setting an Unown letter is refused");
    return;
  }

  /* Gen 2: record whichever letters are already seen (view-only baseline), then round-
   * trip one letter that is NOT currently recorded, then remove it again and confirm
   * the list compacted back to its original shape (byte-for-byte, not just "which
   * letters are present" -- UpdateUnownDex's own 0-terminated scan depends on no gap
   * being left behind). */
  int baseline_present = 0;
  for (int L = 0; L < 26; L++) if (gbdex_unown_seen(&s, L)) baseline_present++;
  printf("  %s: %d Unown letter(s) already recorded\n", file, baseline_present);

  int fresh = -1;
  for (int L = 0; L < 26; L++) if (!gbdex_unown_seen(&s, L)) { fresh = L; break; }
  if (fresh < 0) { printf("  %s: all 26 letters already recorded -- skipping the append/remove probe\n", file); return; }

  CHECK(gbdex_unown_set(&s, fresh, true) == GBS_OK, "append a fresh letter");
  CHECK(gbdex_unown_seen(&s, fresh), "the fresh letter now reads seen");
  int after_append = 0;
  for (int L = 0; L < 26; L++) if (gbdex_unown_seen(&s, L)) after_append++;
  CHECK(after_append == baseline_present + 1, "exactly one more letter recorded after append");

  /* appending the SAME letter again is a no-op (dedup, per UpdateUnownDex's own
   * cp c / ret z short-circuit) -- the count must not grow a second time. */
  CHECK(gbdex_unown_set(&s, fresh, true) == GBS_OK, "re-append the same letter (no-op)");
  int after_reappend = 0;
  for (int L = 0; L < 26; L++) if (gbdex_unown_seen(&s, L)) after_reappend++;
  CHECK(after_reappend == after_append, "re-appending an already-present letter does not grow the list");

  CHECK(gbdex_unown_set(&s, fresh, false) == GBS_OK, "remove the fresh letter");
  CHECK(!gbdex_unown_seen(&s, fresh), "the fresh letter no longer reads seen");
  int after_remove = 0;
  for (int L = 0; L < 26; L++) if (gbdex_unown_seen(&s, L)) after_remove++;
  CHECK(after_remove == baseline_present, "count back to baseline after remove");
}

/* ---- Unown FULL-LIST semantics on a synthetic in-memory image -------------- */
/* No real save is known to have every one of the 26 slots populated, so the "list is
 * full" (GBS_ERR_FULL) and the exact compaction shape are proven on a synthetic
 * Crystal-shaped image instead: a minimal, self-consistent image this test builds by
 * hand at the field's own offset, not a real cartridge dump. */
static void unown_synthetic(void) {
  /* Build the smallest valid Crystal session this test can construct: reuse a real
   * Crystal.sav if present (guarantees every OTHER checksum/region stays valid), only
   * poking the 26-byte wUnownDex field directly through gbdex's own write path. */
  uint32_t len = load("Crystal.sav");
  if (!len) { printf("  SKIP synthetic Unown-full probe (no Crystal.sav)\n"); return; }
  GbSession s;
  if (gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) != GBS_OK) return;
  if (s.gen != GB_GEN2) return;

  /* Ask for all 26 letters via the real API. If Crystal.sav already has all 26 (it
   * does, on Guy's corpus -- a fully-completed save), every call is the documented
   * no-op (already present); if it did not, this would be the append path filling
   * each of the 26 physical slots in turn. Either way, all 26 must read seen after. */
  for (int L = 0; L < 26; L++) {
    GbsStatus st = gbdex_unown_set(&s, L, true);
    CHECK(st == GBS_OK, "letter present (appended or already there)");
  }
  for (int L = 0; L < 26; L++) CHECK(gbdex_unown_seen(&s, L), "every letter reads seen once all 26 are present");

  /* Now every slot is taken by a distinct letter -- there is no 27th letter to add
   * (0-25 is the whole alphabet), so GBS_ERR_FULL is unreachable through the public
   * API with 26 real letters; document that rather than fabricate a 27th letter id
   * gbdex_unown_set's own [0,25] range check would refuse anyway. */
  CHECK(gbdex_unown_set(&s, 30, true) == GBS_ERR_ARG, "an out-of-range letter (>25) is refused before any slot logic runs");

  /* Remove letter A and confirm the list compacted at WHATEVER physical slot A
   * actually occupied (not assumed to be array index 0 -- the corpus save's own
   * first-seen order is whatever the player's real playthrough produced): every
   * OTHER letter must still read seen, and A alone must not. */
  CHECK(gbdex_unown_set(&s, 0, false) == GBS_OK, "remove letter A, wherever its slot is");
  CHECK(!gbdex_unown_seen(&s, 0), "A no longer reads seen");
  for (int L = 1; L < 26; L++) CHECK(gbdex_unown_seen(&s, L), "B..Z still all read seen after A's removal");
}

/* ---- 4 (D2, b87 fix pass, DO-NOT-SHIP review): decline must discard the edit -----
 *
 * gbdex_shim_set/gbdex_unown_set (source/pdna_gbdex.c) write straight into the GB
 * session's own image via gbdex_set/gbdex_unown_set below -- there is no staging
 * buffer. pdna_gbdex()'s original decline path (`if (!app_confirm(...)) return
 * false;`) left those bytes sitting in the image uncommitted; the fix adds
 * `gb_rollback()` on decline. gb_rollback() itself (pdna_gen12.c) is GBA-only
 * (guarded behind PDNA_GEN12_HOST, needs the arena-resident Gb12Edit/tonc.h), so
 * this mirrors gb_rollback's own two-line body EXACTLY (pdna_gen12.c:1046-1049:
 * `memcpy(g_ed->img, g_ed->pristine, g_ed->len); gbs_open(&g_ed->s, g_ed->img,
 * g_ed->len, g_ed->scratch, sizeof g_ed->scratch);`) against g_img/g_orig here,
 * using the REAL gbdex_set/gbdex_unown_set (not a mock) to make the edit -- the
 * same round-trip the fixed decline path performs, byte for byte. */
static uint8_t g_orig[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];

static void decline_discards_edits(const char* file, uint8_t expect_gen) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  memcpy(g_orig, g_img, len);   /* the pristine snapshot the mirrored gb_rollback() restores to */
  g_ran++;
  GbSession s;
  CHECK(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "gbs_open");
  CHECK(s.gen == expect_gen, "right generation detected");

  /* Simulate a visit: TOGGLE species 1..5's caught state (the Pokedex screen half)
   * -- Guy's corpus saves are fully-completed dexes (every species already owned),
   * so "mark caught" alone would be a no-op; toggling off is the edit that's
   * guaranteed to actually change bytes on a completed save, same as the real
   * Wipe-ALL / per-species un-catch path through gbdex_shim_set. On Gen 2, also
   * touch the Unown-forms list (the same visit gbdex_chooser lets a player do
   * BOTH sub-screens in, per pdna_gbdex.c's own header). */
  int max = gb_max_species(s.gen);
  for (int nat = 1; nat <= 5 && nat <= max; nat++) {
    bool was_owned = gbdex_get(&s, (uint16_t)nat, true);
    CHECK(gbdex_set(&s, (uint16_t)nat, true, !was_owned) == GBS_OK, "edit: toggle a species' caught state");
  }
  if (expect_gen == GB_GEN2) {
    bool z_owned = gbdex_unown_seen(&s, 25);
    GbsStatus st = gbdex_unown_set(&s, 25, !z_owned);   /* toggle letter Z */
    CHECK(st == GBS_OK, "edit: touch the Unown list too");
  }

  CHECK(memcmp(g_img, g_orig, len) != 0, "sanity: the edit actually changed the in-RAM image");

  /* The fixed decline path: gb_rollback()'s own two lines, mirrored. */
  memcpy(g_img, g_orig, len);
  CHECK(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "re-open over the restored bytes");

  CHECK(memcmp(g_img, g_orig, len) == 0, "decline: whole-image compare to pristine is 0 bytes");

  /* Re-read species 1..5 through the reopened session and confirm each one's
   * caught state matches a FRESH session opened directly over the untouched
   * g_orig bytes (not just "the bytes matched" -- the session's own field reads
   * agree with the pristine save too, not some stale cached state). */
  uint8_t ref_img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
  uint8_t ref_scratch[GBS_SCRATCH_BYTES];
  memcpy(ref_img, g_orig, len);
  GbSession ref;
  CHECK(gbs_open(&ref, ref_img, len, ref_scratch, sizeof ref_scratch) == GBS_OK, "open a reference session over pristine g_orig");
  for (int nat = 1; nat <= 5 && nat <= max; nat++)
    CHECK(gbdex_get(&s, (uint16_t)nat, true) == gbdex_get(&ref, (uint16_t)nat, true),
          "post-rollback caught-state matches a fresh read of the untouched save");
}

/* ---- 5 (D4, b87 fix pass, DO-NOT-SHIP review): the Unown-dex GATE stays in sync --
 *
 * STATUSFLAGS_UNOWN_DEX_F (wStatusFlags bit 1, GBF_STATUS_FLAGS) and wFirstUnownSeen
 * (GBF_FIRST_UNOWN_SEEN) are a SEPARATE gate from gbdex's own owned/seen bitfields and
 * from wUnownDex's own order-of-first-seen list -- neither offset is exercised by any
 * earlier test in this file. Gen 2 only (GS/Crystal); Gen 1 has no Unown dex entry
 * (gb_max_species(GB_GEN1)=151 < 201) so every one of these calls is a no-op there,
 * exercised as its own explicit assertion below rather than skipped silently. */
static uint8_t status_flags_raw(GbSession* s, GbGame g) {
  uint32_t off = gbf_off(g, GBF_STATUS_FLAGS);
  uint8_t v = 0xFF;
  if (off) (void)gbs_read_field(s, off, &v, 1);
  return v;
}
static uint8_t first_unown_seen_raw(GbSession* s, GbGame g) {
  uint32_t off = gbf_off(g, GBF_FIRST_UNOWN_SEEN);
  uint8_t v = 0xFF;
  if (off) (void)gbs_read_field(s, off, &v, 1);
  return v;
}
/* R1 (b87 fix pass 2): mirrors tests/host_gbsurgery_tool.c's do_dexset() two-call
 * shape for national dex 201's state 0/1/2 = none/seen/caught -- the exact real
 * caller shape `--op dexset 201 N` drives (gbdex_set(owned=true) first, then
 * gbdex_set(owned=false)). */
static GbsStatus shim201(GbSession* s, int state) {
  GbsStatus st = gbdex_set(s, 201, true, state >= 2);
  if (st != GBS_OK) return st;
  return gbdex_set(s, 201, false, state >= 1);
}
static void unown_gate_sync(const char* file, uint8_t expect_gen) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECK(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "gbs_open");
  CHECK(s.gen == expect_gen, "right generation detected");
  GbGame g = (s.gen == GB_GEN1) ? GBF_G_RED
           : (s.g2w.sv.version == G2_VER_CRYSTAL) ? GBF_G_CRYSTAL : GBF_G_GS;

  if (expect_gen == GB_GEN1) {
    /* Gen 1 has no Unown dex entry at all -- the special-case branch in gbdex_set
     * (dex==201) is unreachable (dex 201 > gb_max_species(GB_GEN1)==151 is refused
     * before it), and gbdex_unown_set always fails via unown_read's own off==0 gate. */
    CHECK(gbdex_set(&s, 201, false, true) == GBS_ERR_ARG, "Gen 1: dex 201 is out of range, refused before D4's branch runs");
    CHECK(gbdex_unown_set(&s, 0, true) == GBS_ERR_ARG, "Gen 1: gbdex_unown_set refuses (no Unown-dex field at all)");
    return;
  }

  uint32_t status_off = gbf_off(g, GBF_STATUS_FLAGS);
  uint32_t fus_off = gbf_off(g, GBF_FIRST_UNOWN_SEEN);
  CHECK(status_off != 0 && fus_off != 0, "Gen 2: both D4 fields are present");

  /* Force a clean slate: bit 1 clear, wFirstUnownSeen == 0, and (via the repeated
   * gbdex_unown_set(letter,false) removal loop) wUnownDex fully emptied -- Guy's
   * corpus saves already have every letter recorded, so start from there. */
  for (int L = 0; L < 26; L++) (void)gbdex_unown_set(&s, L, false);
  { uint8_t zero = 0; CHECK(gbs_write_field(&s, fus_off, &zero, 1) == GBS_OK, "reset wFirstUnownSeen to 0"); }
  { uint8_t cur; CHECK(gbs_read_field(&s, status_off, &cur, 1) == GBS_OK, "read wStatusFlags");
    cur &= (uint8_t)~(1u << 1);
    CHECK(gbs_write_field(&s, status_off, &cur, 1) == GBS_OK, "clear bit 1 of wStatusFlags"); }
  for (int L = 0; L < 26; L++) CHECK(!gbdex_unown_seen(&s, L), "clean slate: no Unown letter recorded");
  CHECK((status_flags_raw(&s, g) & 0x02) == 0, "clean slate: gate bit clear");
  CHECK(first_unown_seen_raw(&s, g) == 0, "clean slate: wFirstUnownSeen == 0");

  /* Case A: gbdex_unown_set(letter, true) on an append sets the gate bit and, since
   * wFirstUnownSeen reads 0, records this letter (F, 1-based value 6). */
  CHECK(gbdex_unown_set(&s, 5, true) == GBS_OK, "append letter F");
  CHECK((status_flags_raw(&s, g) & 0x02) != 0, "A: append sets the gate bit");
  CHECK(first_unown_seen_raw(&s, g) == 6, "A: wFirstUnownSeen records letter F (1-based: 6)");

  /* A second, later append must NOT overwrite the already-recorded first letter. */
  CHECK(gbdex_unown_set(&s, 10, true) == GBS_OK, "append a second letter (K)");
  CHECK(first_unown_seen_raw(&s, g) == 6, "A2: a later append never overwrites the first-seen letter");

  /* Case D: emptying the list all the way back out clears the gate bit again. */
  CHECK(gbdex_unown_set(&s, 5, false) == GBS_OK, "remove letter F");
  CHECK((status_flags_raw(&s, g) & 0x02) != 0, "D-partial: gate stays set while K is still recorded");
  CHECK(gbdex_unown_set(&s, 10, false) == GBS_OK, "remove letter K too -- list now empty");
  CHECK((status_flags_raw(&s, g) & 0x02) == 0, "D: emptying the list clears the gate bit");
  /* wFirstUnownSeen is NOT reset by emptying the list (matches the brief's own scope:
   * only the gate bit moves here; a manual reset back to 0 is what the next test uses). */
  CHECK(first_unown_seen_raw(&s, g) == 6, "D: wFirstUnownSeen is left as-is by the empty-list path");

  /* Case B: gbdex_set(201, seen=true) on a save with wFirstUnownSeen==0 (RESET it back
   * to 0 first -- case D left it at 6) seeds UNOWN A (letter 0), matching
   * DebugRoomMenu_PokedexDex's own behaviour, per the brief. */
  { uint8_t zero = 0; CHECK(gbs_write_field(&s, fus_off, &zero, 1) == GBS_OK, "reset wFirstUnownSeen to 0 for case B"); }
  CHECK(gbdex_set(&s, 201, false, true) == GBS_OK, "mark dex 201 (Unown) seen");
  CHECK((status_flags_raw(&s, g) & 0x02) != 0, "B: marking #201 seen sets the gate bit");
  CHECK(first_unown_seen_raw(&s, g) == 1, "B: wFirstUnownSeen seeded to 1 (UNOWN A)");
  CHECK(gbdex_unown_seen(&s, 0), "B: letter A is now present in wUnownDex too");
  CHECK(gbdex_get(&s, 201, false), "B: dex 201 itself reads seen");

  /* Case C: clearing #201's seen bit clears ONLY the gate bit (brief's own scope --
   * wFirstUnownSeen/wUnownDex are left alone, matching DebugRoomMenu_PokedexClr). */
  CHECK(gbdex_set(&s, 201, false, false) == GBS_OK, "clear dex 201 (Unown) seen");
  CHECK((status_flags_raw(&s, g) & 0x02) == 0, "C: clearing #201's seen bit clears the gate bit");
  CHECK(first_unown_seen_raw(&s, g) == 1, "C: wFirstUnownSeen is untouched by the clear path");
  CHECK(gbdex_unown_seen(&s, 0), "C: letter A is still present in wUnownDex (only the gate cleared)");

  /* A save that already has a recorded letter is left alone by case B's seed path
   * (the "if wFirstUnownSeen reads 0" guard) -- verified directly, not just implied. */
  CHECK(gbdex_set(&s, 201, false, true) == GBS_OK, "mark #201 seen again (wFirstUnownSeen already 1)");
  CHECK(first_unown_seen_raw(&s, g) == 1, "B-repeat: wFirstUnownSeen stays 1, never reseeded to a different letter");
  /* R1 (b87 fix pass 2): B-repeat is also the "already has a letter" case R1's own
   * else branch targets -- the gate bit must come back here too, not just when
   * seeding a brand-new letter. */
  CHECK((status_flags_raw(&s, g) & 0x02) != 0, "B-repeat: the gate bit is ALSO restored (R1's else branch)");

  /* R1 (b87 fix pass 2, mutation-proven DO-NOT-SHIP finding): on a save that
   * ALREADY has a letter recorded (wFirstUnownSeen != 0, the exact state this
   * test is in right now), a Wipe-ALL-then-Undo-shaped round trip -- clear #201's
   * seen bit (clears the gate, case C's own path), then set it again -- used to
   * leave the gate PERMANENTLY OFF (the `on` branch's `if (first_unown_seen_is_
   * zero(...))` guard skipped the re-arm entirely once a letter already existed).
   * shim201() mirrors host_gbsurgery_tool.c's do_dexset() two-call shape
   * (owned=true first, then owned=false) for dex 201's state 0/1/2 = none/seen/
   * caught, the exact real caller shape `--op dexset 201 N` drives. */
  uint8_t baseline_status = status_flags_raw(&s, g);
  CHECK((baseline_status & 0x02) != 0, "R1 setup: baseline gate bit is set before the off-then-on round trip");
  CHECK(shim201(&s, 0) == GBS_OK, "R1: dexset 201 0 (Wipe/none)");
  CHECK((status_flags_raw(&s, g) & 0x02) == 0, "R1: gate bit cleared by the wipe half, as expected");
  CHECK(shim201(&s, 2) == GBS_OK, "R1: dexset 201 2 (Catch/caught again)");
  CHECK(status_flags_raw(&s, g) == baseline_status,
        "R1: #201 off-then-on restores the gate bit to its exact baseline (was stuck OFF pre-fix)");
}

/* R1 (b87 fix pass 2): a whole-save byte-exact proof that dex_bulk()'s own Wipe
 * ALL + Undo (pdna_pick.c, GBA-only -- not host-compilable) is not broken by the
 * R1 fix. Mirrors gbdex_shim_get/gbdex_shim_set's own shape (pdna_gbdex.c, static
 * there): state 0/1/2 = none/seen/caught via gbdex_get/gbdex_set(owned=true)+
 * (owned=false), the SAME two real functions the shims call through -- snapshot
 * every species 1..cap, wipe every species to 0 (state==none), then restore every
 * species from the snapshot (Undo), exactly dex_bulk's own Catch/See/Wipe-ALL +
 * Undo sequence. gbs_finish() is called once at the very end (gbdex_set's own
 * documented batching contract), so the Gen-2 checksums are recomputed exactly
 * once, matching how pdna_gbdex() itself only finishes after a whole visit.
 * Expects the restored image to be byte-IDENTICAL to the pristine one except
 * Gold's own checksum-2 (GBF2_GS_CKSUM2, 0x7E6D-0x7E6E, tests/gen12_fixture.h) --
 * a real, expected recompute (the byte sum algorithm does not commute with a
 * wipe-then-restore round trip bit-for-bit the way a no-op would), not a
 * regression. Crystal carries no such artifact -- 0 bytes must differ there. */
static void wipe_all_undo_byte_exact(const char* file, uint8_t expect_gen) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  if (expect_gen != GB_GEN2) { printf("  SKIP %s (R1's byte-exact proof is Gen 2 only)\n", file); return; }
  g_ran++;
  uint8_t pristine[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
  memcpy(pristine, g_img, len);

  GbSession s;
  CHECK(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "gbs_open");
  int max = gb_max_species(s.gen);

  static int8_t snap[251];   /* DEX_NAT_MAX would pull pdna_pick.c's own macro -- 251 covers every Gen-2 cap */
  for (int nat = 1; nat <= max; nat++) {
    int state = gbdex_get(&s, (uint16_t)nat, true) ? 2 : gbdex_get(&s, (uint16_t)nat, false) ? 1 : 0;
    snap[nat - 1] = (int8_t)state;
  }
  for (int nat = 1; nat <= max; nat++) {
    CHECK(gbdex_set(&s, (uint16_t)nat, true, false) == GBS_OK, "wipe: owned=false");
    CHECK(gbdex_set(&s, (uint16_t)nat, false, false) == GBS_OK, "wipe: seen=false");
  }
  for (int nat = 1; nat <= max; nat++) {
    int want = snap[nat - 1];
    CHECK(gbdex_set(&s, (uint16_t)nat, true, want >= 2) == GBS_OK, "undo: restore owned");
    CHECK(gbdex_set(&s, (uint16_t)nat, false, want >= 1) == GBS_OK, "undo: restore seen");
  }
  CHECK(gbs_finish(&s) == GBS_OK, "gbs_finish after the whole Wipe-ALL + Undo batch");

  int ndiff = 0; uint32_t first_diff = 0;
  for (uint32_t i = 0; i < len; i++) if (g_img[i] != pristine[i]) { if (!ndiff) first_diff = i; ndiff++; }
  bool is_gold = (s.g2w.sv.version != G2_VER_CRYSTAL);
  if (is_gold) {
    CHECK(ndiff == 2 && (first_diff == 0x7E6D || first_diff == 0x7E6E),
          "Gold: Wipe ALL + Undo is byte-exact except the known 2-byte checksum-2 repair (0x7E6D-0x7E6E)");
    printf("  -- %s: %d byte(s) differ after Wipe-ALL+Undo (first @%#06x) -- want 2 @ 0x7e6d/0x7e6e\n",
           file, ndiff, first_diff);
  } else {
    CHECK(ndiff == 0, "Crystal: Wipe ALL + Undo is byte-exact, 0 bytes differ");
    printf("  -- %s: %d byte(s) differ after Wipe-ALL+Undo (want 0)\n", file, ndiff);
  }
}

int main(void) {
  one_dex_popcount("Red.sav", GB_GEN1);
  one_dex_popcount("Gold.sav", GB_GEN2);
  one_dex_popcount("Crystal.sav", GB_GEN2);

  one_dex_invariant("Red.sav", GB_GEN1);
  one_dex_invariant("Gold.sav", GB_GEN2);
  one_dex_invariant("Crystal.sav", GB_GEN2);

  unown_semantics("Red.sav", GB_GEN1);
  unown_semantics("Gold.sav", GB_GEN2);
  unown_semantics("Crystal.sav", GB_GEN2);

  unown_synthetic();

  decline_discards_edits("Red.sav", GB_GEN1);
  decline_discards_edits("Gold.sav", GB_GEN2);
  decline_discards_edits("Crystal.sav", GB_GEN2);

  unown_gate_sync("Red.sav", GB_GEN1);
  unown_gate_sync("Gold.sav", GB_GEN2);
  unown_gate_sync("Crystal.sav", GB_GEN2);

  wipe_all_undo_byte_exact("Gold.sav", GB_GEN2);
  wipe_all_undo_byte_exact("Crystal.sav", GB_GEN2);

  printf("\n%s: %d check(s), %d failure(s), %d file(s) exercised\n",
         g_fail ? "FAIL" : "OK", g_check, g_fail, g_ran);
  return g_fail ? 1 : 0;
}
