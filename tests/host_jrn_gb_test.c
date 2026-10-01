/* Host test for BACKLOG #234 slice 4: a Game Boy image on the SAME journal core (source/jrn_app.c's flat accessor, the
 * img_rec_flat recorder in source/img_stage.c, the key/step names in source/gb_jkey.c) over the real FatFs on a RAM disk.
 * What pdna_gen12.c's gb_hold_commit / gb_persist / the exit confirm do on the console is exactly
 * img_rec_flat(&R, pristine, img, 8, 4096, name) + jrnapp_flush(); this proves the recorder side on the PC:
 *
 *   Covers: key (stable/moved/Gen-1 != Gen-2/0 without image); Everdrive never opens; exact recorded runs, byte-exact
 *   undo/redo; crossed floors + power-cut offer + re-apply; resync hashes the BASELINE (GAP); cap -> ring size; a Gen-3-layout
 *   open over a GB journal is FOREIGN; Clear history (files+dir gone, reopens EMPTY, .sav untouched).
 *
 *   cc -std=c11 -Wall -Wextra -Wno-unused-function -DFF_USE_MKFS=1 -Dsiprintf=sprintf -Dsniprintf=snprintf \
 *      -Dvsniprintf=vsnprintf -I tests/hostfat -I lib/fatfs -I source -I tests tests/host_jrn_gb_test.c tests/gen12_fixture.c \
 *      source/img_stage.c source/gb_jkey.c source/gb_trainer.c source/gb_fields.c source/gb_session.c source/gb_edit.c \
 *      source/gen1_save.c source/gen1_write.c source/gen2_save.c source/gen2_write.c source/data_tables.c \
 *      source/item_map_g2g3.c source/item_map_g1g2.c source/gb_item_names.c source/gb_bag.c source/gen3_to_gb.c \
 *      source/gb_sidecar.c source/bank_cell.c source/gen3_edit.c source/gen3_mon.c source/gen3_box.c source/gen3_save.c \
 *      source/gen3_daycare.c source/journal.c source/journal_undo.c source/journal_fs.c source/jrn_app.c \
 *      lib/fatfs/ff.c lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c -o /tmp/hjgb && /tmp/hjgb
 *
 * tests/host_y19_s4_sites_test.py recompiles this against mutated copies of the real sources and requires each mutant to
 * fail (G1..G7). */
#include "jrn_harness.h"
#include "gen12_fixture.h"
#include "gb_jkey.h"
#include "gb_session.h"
#include "gb_fields.h"
#include "gb_trainer.h"
#include "img_stage.h"
#include "jrn_app.h"

/* jrn_app.c is compiled in (its log/rumble deps stubbed). */
void log_line(const char* fmt, ...) { (void)fmt; }
void rmbl_pause(void) {}
void rmbl_resume(void) {}

#define GB_LEN 0x8000u
static uint8_t img[GB_LEN + GBF_RTC_TAIL_64], orig[GB_LEN + GBF_RTC_TAIL_64], base[GB_LEN + GBF_RTC_TAIL_64];
static uint32_t imglen;
static uint8_t scratch[GBS_SCRATCH_BYTES];
static GbSession S;
static ImgRec R;
static ImgFlags F;

static int build(GbfGame g) {
  memset(img, 0, sizeof img);
  imglen = gbf_build(g, img, 0);
  if (!imglen) return 0;
  memcpy(orig, img, sizeof img); memcpy(base, img, sizeof img);
  return gbs_open(&S, img, imglen, scratch, sizeof scratch) == GBS_OK;
}

/* A fresh card + a GB image + an opened, prepared journal + the recorder (what gb_journal_session_open does). */
static int world(GbfGame g, uint8_t cap) {
  if (!build(g)) return 0;
  card_fresh(FM_FAT);
  rd_fattime_hook = jrn_fattime_filter;
  imgf_clear(&F);
  if (jrnapp_open_gb(&R, img, gb_journal_key(&S), cap, true) != JA_OK) return 0;
  return jrnapp_prepare_key(&R, gb_journal_key(&S)) == JRN_OK;
}

/* The same commit the app would hold: change `n` bytes at `off`, record img vs the baseline, re-baseline. */
static void edit(uint32_t off, uint32_t n, uint8_t x, const char* name, int crossed) {
  uint32_t i;
  for (i = 0; i < n; i++) img[off + i] = (uint8_t)(img[off + i] ^ x ^ (uint8_t)i);
  if (crossed) img_rec_cross(&R);
  (void)img_rec_flat(&R, base, img, 8, 4096, name);
  memcpy(base, img, sizeof base);
}

static int count_slot_files(void) {
  char hex[17], p[96];
  DIR d; FILINFO fi; int n = 0;
  jrn_key_hex(gb_journal_key(&S), hex);
  snprintf(p, sizeof p, ROOT "/%s", hex);
  if (f_opendir(&d, p) != FR_OK) return -1;
  while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) n++;
  f_closedir(&d);
  return n;
}

static void t_key(void) {
  uint64_t k1, k2;
  uint32_t noff, ioff;
  CHECK(build(GBF_RBY), "Gen-1 fixture");
  k1 = gb_journal_key(&S);
  CHECK(k1 != 0, "a resident Gen-1 session has a key");
  img[0x1000] ^= 0x5A;                                                     /* a mon-area byte is not the identity */
  CHECK(gb_journal_key(&S) == k1, "G1 the key is stable across an edit that is not the identity");
  noff = gbf_off(gbt_game(&S), GBF_PLAYER_NAME); ioff = gbf_off(gbt_game(&S), GBF_TRAINER_ID);
  img[noff] ^= 0x01; k2 = gb_journal_key(&S);
  CHECK(k2 != k1, "G1 a trainer NAME edit moves the key");
  img[noff] ^= 0x01;
  img[ioff + 1] ^= 0x01; k2 = gb_journal_key(&S);
  CHECK(k2 != k1, "G1 a trainer ID edit moves the key");
  img[ioff + 1] ^= 0x01;
  CHECK(gb_journal_key(&S) == k1, "G1 the key comes back with the identity");
  { GbSession st2 = S; st2.img = 0; CHECK(gb_journal_key(&st2) == 0, "a streamed session (no resident image) has no key"); }
  CHECK(gb_journal_key(0) == 0, "NULL session");
  CHECK(build(GBF_GS), "Gen-2 fixture");
  k2 = gb_journal_key(&S);
  CHECK(k2 != 0 && k2 != k1, "G2 a Gen-2 save of the same trainer name + ID never shares a Gen-1 journal");
  CHECK(strcmp(gb_step_name("move"), "Box move") == 0 && strcmp(gb_step_name("nonsense"), "Edit") == 0 &&
        strcmp(gb_step_name(0), "Edit") == 0, "step names map, unknown = Edit");
  /* #310: GB crossings read "Transfer up/down"; the plain Gen-3 Bank->GB drop records under "bank-down". */
  CHECK(strcmp(gb_step_name("xferup"), "Transfer up") == 0, "#310 xferup -> Transfer up");
  CHECK(strcmp(gb_step_name("xferdown"), "Transfer down") == 0, "#310 xferdown -> Transfer down");
  CHECK(strcmp(gb_step_name("bank-down"), "Transfer down") == 0, "#310 bank-down -> Transfer down");
  {
    /* text pin (cwd = repo root): the sidecar paste core, the plain Gen-3 DOWN drop's one card write, tags "bank-down" */
    static char src[1 << 20];
    size_t n = 0;
    FILE* sf = fopen("source/pdna_gen12.c", "rb");
    if (sf) { n = fread(src, 1, sizeof src - 1, sf); fclose(sf); }
    src[n] = 0;
    {
      const char* def = strstr(src, "gb_paste_write(const GbEditMon* mon");
      const char* call = def ? strstr(def, "gb_persist(\"") : 0;
      CHECK(n > 0 && def && call, "#310 pin: source/pdna_gen12.c readable, paste core + its gb_persist found");
      CHECK(call && strncmp(call, "gb_persist(\"bank-down\")", 23) == 0,
            "#310 pin: the g3 DOWN paste core persists under \"bank-down\" (Transfer down), not \"paste\"");
    }
  }
}

static void t_everdrive_never_opens(void) {
  FILINFO fi;
  CHECK(build(GBF_RBY), "fixture");
  card_fresh(FM_FAT);
  CHECK(jrnapp_open_gb(&R, img, gb_journal_key(&S), 0, false) == JA_OFF, "G3 write_ok false: the GB journal never opens");
  CHECK(jrnapp_state(&R) == JA_OFF, "state OFF (never says recorded)");
  CHECK(f_stat(ROOT, &fi) != FR_OK, "and it created no journal directory");
  CHECK(jrnapp_prepare_key(&R, 1) == 0, "prepare on a closed journal does nothing");
}

static void t_hold_record_undo_redo(void) {
  JrnRec rec;
  char nm[25];
  CHECK(world(GBF_RBY, 0), "world");
  edit(0x0100, 4, 0x5A, "Box move", 0);                                    /* region 0 */
  edit(0x2100 + 7, 10, 0xA5, "Box move", 0);                               /* region 2 */
  CHECK(jrnapp_state(&R) == JA_OK && R.lost == 0, "recording is honest: state %d lost %u", jrnapp_state(&R), (unsigned)R.lost);
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_tip() == 2, "two steps, tip %u", (unsigned)jrnapp_tip());
  CHECK(memcmp(img, orig, GB_LEN) != 0, "the image carries the edits");
  {
    Jrn* dummy = 0; (void)dummy;
  }
  CHECK(jrnapp_step(-1, nm) == JRN_OK && strcmp(nm, "Box move") == 0, "undo #1 names the step, got '%s'", nm);
  CHECK(jrnapp_step(-1, nm) == JRN_OK, "undo #2");
  CHECK(memcmp(img, orig, GB_LEN) == 0, "G4 undo restores the GB image byte for byte (no checksum fixup needed)");
  CHECK(jrnapp_step(-1, nm) != JRN_OK, "nothing further to undo");
  CHECK(jrnapp_step(1, nm) == JRN_OK && jrnapp_step(1, nm) == JRN_OK, "redo x2");
  memcpy(base, img, sizeof base);
  CHECK(memcmp(img, orig, GB_LEN) != 0, "redo brings the edits back");
  CHECK(jrnapp_flush() == JRN_OK, "flush the cursor markers");
  (void)rec;
}

static void t_offer_after_power_cut(void) {
  uint32_t av = 99, total;
  char stop[25];
  static uint8_t edited[GB_LEN + GBF_RTC_TAIL_64];
  CHECK(world(GBF_RBY, 0), "world");
  edit(0x0100, 4, 0x5A, "Box move", 0);
  edit(0x0900, 6, 0x11, "Bag", 0);
  CHECK(jrnapp_flush() == JRN_OK, "flush (the exit's rest point)");
  memcpy(edited, img, sizeof edited);
  memcpy(img, orig, sizeof img);                                           /* the power cut: the card kept the ORIGINAL image */
  CHECK(jrnapp_open_gb(&R, img, gb_journal_key(&S), 0, true) == JA_OK, "reopen on the original image");
  total = jrnapp_offer(&av, stop);
  CHECK(total == 2 && av == 2, "G5 both steps are offered: total %u avail %u", (unsigned)total, (unsigned)av);
  CHECK(jrnapp_reapply() == 2, "re-apply applies both");
  CHECK(memcmp(img, edited, GB_LEN) == 0, "G5 the re-applied image is the edited one, byte for byte");
  /* and a crossed step floors the offer */
  CHECK(world(GBF_RBY, 0), "world");
  edit(0x0100, 4, 0x5A, "Box move", 0);
  edit(0x0900, 6, 0x11, "Transfer up", 1);                                 /* crossed */
  edit(0x1100, 6, 0x22, "Box move", 0);
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  memcpy(img, orig, sizeof img);
  CHECK(jrnapp_open_gb(&R, img, gb_journal_key(&S), 0, true) == JA_OK, "reopen");
  total = jrnapp_offer(&av, stop);
  CHECK(total == 3 && av == 1 && stop[0] != 0, "G5 the crossed step floors the offer: total %u avail %u stop '%s'",
        (unsigned)total, (unsigned)av, stop);
}

static void t_resync_hashes_the_baseline(void) {
  Jrn* none = 0; (void)none;
  CHECK(world(GBF_RBY, 0), "world");
  edit(0x0100, 4, 0x5A, "Box move", 0);
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  img[0x0500] ^= 0x33; base[0x0500] ^= 0x33;                               /* a write OUTSIDE the recorder (both copies move) */
  edit(0x0900, 6, 0x11, "Bag", 0);
  CHECK(R.lost >= 1 && R.state == IREC_GAP, "G6 the divergence is counted honestly: lost %u state %d", (unsigned)R.lost, R.state);
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  {
    JaHist rows[4]; int more = 0, fl = 0, n;
    n = jrnapp_history(rows, 4, &more, &fl);
    CHECK(n == 2 && rows[0].crossed == 1, "G6 the resynced step is a FLOOR (crossed): rows %d crossed %d", n, n ? rows[0].crossed : -1);
  }
}

static void t_cap_and_foreign_layout(void) {
  Jrn j3;
  JrnCfg c;
  JrnImage im;
  CHECK(world(GBF_RBY, 4), "world at cap 4");
  CHECK(count_slot_files() == 5, "G7 a cap of 4 makes a ring of 5 slot files, got %d", count_slot_files());
  memset(&c, 0, sizeof c);
  c.fs = &jrn_fatfs; c.root = ROOT; c.key = gb_journal_key(&S); c.nreg = NREG; c.reg_size = RSZ;   /* the GEN-3 layout over a GB journal */
  im.ctx = 0; im.get = img_get; im.set = img_set;
  CHECK(jrn_open(&j3, &c, &im) == JRN_E_VERSION, "a Gen-3-layout open of a GB journal is FOREIGN (layout recorded in the header)");
}

static void t_clear_history(void) {
  uint32_t av = 99;
  char stop[25];
  FILINFO fi;
  char hex[17], p[96];
  CHECK(world(GBF_RBY, 0), "world");
  edit(0x0100, 4, 0x5A, "Box move", 0);
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(count_slot_files() == 17, "a default ring is 17 slot files, got %d", count_slot_files());
  CHECK(jrnapp_clear(&R, 4) >= 17, "G8 Clear history removes the slot files");
  jrn_key_hex(gb_journal_key(&S), hex);
  snprintf(p, sizeof p, ROOT "/%s", hex);
  CHECK(f_stat(p, &fi) != FR_OK, "G8 the key directory is gone");
  CHECK(jrnapp_state(&R) == JA_OK && jrnapp_tip() == 0 && jrnapp_cursor() == 0, "G8 reopened EMPTY and still recording");
  CHECK(jrnapp_offer(&av, stop) == 0, "no offer from an empty history");
  CHECK(jrnapp_prepare_key(&R, gb_journal_key(&S)) == JRN_OK, "the new ring is made");
  CHECK(count_slot_files() == 5, "G8 the new ring honours the NEW cap (4 -> 5 slot files), got %d", count_slot_files());
  edit(0x0900, 6, 0x11, "Bag", 0);
  CHECK(jrnapp_flush() == JRN_OK && jrnapp_tip() == 1, "a fresh history records from the image as it is now");
  CHECK(sav_intact(), "the .sav was never touched");
  {
    ImgRec dead; memset(&dead, 0, sizeof dead);
    CHECK(jrnapp_clear(&dead, 0) == JRN_E_ARG, "clearing a journal that is not open is refused");
  }
}

/* D5 (#234 s4 review): a card error part-way through Clear history may have deleted SOME slot files. The journal must then
 * read ERROR and record nothing (never a half ring misread as a history); a clean clear keeps recording. Sweeps the failing
 * write across the whole delete + reopen. At least one K must hit the delete itself (n < 0), or the sweep proves nothing. */
static void t_clear_card_error(void) {
  long K;
  int hit_delete = 0, bad = 0;
  for (K = 0; K <= 60; K++) {
    int n, st;
    if (!world(GBF_RBY, 0)) { CHECK(0, "world K=%ld", K); return; }
    edit(0x0100, 4, 0x5A, "Box move", 0);
    (void)jrnapp_flush();
    rd_fail_at = K;
    n = jrnapp_clear(&R, 0);
    rd_fail_at = -1;
    st = jrnapp_state(&R);
    if (n < 0) { hit_delete++; if (st != JA_ERROR) bad++; }
    else if (st != JA_OK) bad++;
  }
  CHECK(hit_delete > 0, "D5 the sweep reached a failed delete (%d hits)", hit_delete);
  CHECK(bad == 0, "D5 a failed Clear history leaves the journal ERROR (off), never a half ring; %d bad", bad);
}

/* D8 (#234 s4 review): the B-at-exit discard. The card image (saved after step 1) is put back in the buffer, the thrown-away
 * step 2 stays in the history MARKED, and the journal must RE-ANCHOR on the restored image: a new edit after it undoes
 * byte-exactly to the restored image, never to a state the buffer does not hold. */
static void t_discard_reanchor(void) {
  static uint8_t saved[sizeof img];
  char nm[25];
  CHECK(world(GBF_RBY, 0), "world");
  edit(0x0100, 4, 0x5A, "Box move", 0);
  CHECK(jrnapp_flush() == JRN_OK, "flush 1");
  jrnapp_mark_saved();
  memcpy(saved, img, sizeof img);                                          /* what the card holds */
  edit(0x2100 + 7, 10, 0xA5, "Box move", 0);                               /* the edit the user throws away */
  CHECK(jrnapp_flush() == JRN_OK && jrnapp_tip() == 2, "two steps recorded, tip %u", (unsigned)jrnapp_tip());
  memcpy(img, saved, sizeof img); memcpy(base, saved, sizeof base);        /* B at the exit confirm: the card image is back */
  jrnapp_after_discard(&R);
  CHECK(jrnapp_state(&R) == JA_OK, "D8 still recording after the discard, state %d", jrnapp_state(&R));
  CHECK(jrnapp_cursor() == jrnapp_tip(), "D8 the cursor sits on the tip after the re-anchor (cursor %u tip %u)", (unsigned)jrnapp_cursor(), (unsigned)jrnapp_tip());
  edit(0x0900, 6, 0x11, "Bag", 0);
  CHECK(jrnapp_flush() == JRN_OK, "flush after the discard");
  CHECK(jrnapp_step(-1, nm) == JRN_OK, "undo the new edit");
  CHECK(memcmp(img, saved, GB_LEN) == 0, "D8 undo lands on the restored card image byte-exactly (the journal re-anchored)");
}

/* #308: the GB journal key stopped at the first 0xFF ('9' in the GB charset). Fix = escape in gb_jkey.c (names WITH a 0xFF only;
 * every other name keeps its key) + jrnapp_open_gb_compat continuing an OLD-keyed journal through a redirect (jrn_app.c). */
static void set_name(const uint8_t* nm, unsigned n) {
  uint32_t noff = gbf_off(gbt_game(&S), GBF_PLAYER_NAME);
  memcpy(img + noff, nm, n);
  img[noff + n] = 0x50;
  memcpy(orig, img, sizeof orig); memcpy(base, img, sizeof base);
}
static uint64_t newk, legk;
static void keys_now(void) { newk = gb_journal_key(&S); legk = gb_journal_key_legacy(&S); }

static void t_key_entropy(void) {
  static const uint8_t n1[] = { 0x80, 0xFF, 0x81 }, n2[] = { 0x80, 0xFF, 0x82 }, plain[] = { 0x80, 0x81 }, fe[] = { 0x80, 0xFE },
                       ff[] = { 0x80, 0xFF }, fe01[] = { 0x80, 0xFE, 0x01 };
  uint64_t a_new, a_leg, b_new, b_leg;
  CHECK(build(GBF_RBY), "fixture");
  set_name(n1, 3); keys_now(); a_new = newk; a_leg = legk;
  set_name(n2, 3); keys_now(); b_new = newk; b_leg = legk;
  CHECK(a_leg == b_leg, "#308 (documents the bug) the LEGACY key of two names that differ only after a '9' is the same");
  CHECK(a_new != b_new && a_new != 0 && b_new != 0, "#308 (a) the NEW keys of those two names are DISTINCT");
  set_name(plain, 2); keys_now();
  CHECK(newk == legk, "#308 a name without a 0xFF byte keeps its key EXACTLY (nothing existing moves)");
  set_name(fe, 2); keys_now();
  CHECK(newk == legk, "#308 a name with a 0xFE (the escape byte) but no 0xFF keeps its key too");
  set_name(ff, 2); keys_now(); a_new = newk;
  set_name(fe01, 3); keys_now(); b_new = newk;
  CHECK(a_new != b_new, "#308 the escape is injective: '9' alone vs the literal bytes of its escape never share a key");
  { GbSession st2 = S; st2.img = 0; CHECK(gb_journal_key_legacy(&st2) == 0 && gb_journal_key_legacy(0) == 0, "no image / NULL: no legacy key either"); }
}

static int world9(const uint8_t* nm, unsigned n, uint64_t* legacy_out, uint64_t* new_out) {
  if (!build(GBF_RBY)) return 0;
  set_name(nm, n);
  keys_now();
  *legacy_out = legk; *new_out = newk;
  card_fresh(FM_FAT);
  rd_fattime_hook = jrn_fattime_filter;
  imgf_clear(&F);
  return 1;
}

static void t_old_key_compat(void) {
  static const uint8_t n1[] = { 0x80, 0xFF, 0x81 }, n2[] = { 0x80, 0xFF, 0x82 };
  static uint8_t edited[sizeof img];
  uint64_t leg, nw, leg2, nw2, out = 0;
  char nm[25];
  CHECK(world9(n1, 3, &leg, &nw), "world (a name with a '9')");
  CHECK(leg != nw, "the legacy and new keys differ for this name");
  /* an OLD build created this journal: open under the legacy key, record, flush */
  CHECK(jrnapp_open_gb(&R, img, leg, 0, true) == JA_OK && jrnapp_prepare_key(&R, leg) == JRN_OK, "an old-style journal is created under the legacy key");
  edit(0x0100, 4, 0x5A, "Box move", 0);
  edit(0x0900, 6, 0x11, "Bag", 0);
  CHECK(jrnapp_flush() == JRN_OK && jrnapp_tip() == 2, "two steps on disk under the legacy key");
  memcpy(edited, img, sizeof edited);
  jrnapp_close(&R);
  /* (b) the new build opens the SAME save: finds the old-keyed journal and continues it */
  CHECK(jrnapp_open_gb_compat(&R, img, nw, leg, 0, true) == JA_OK, "#308 (b) compat open");
  CHECK(jrnapp_tip() == 2 && jrnapp_cursor() == 2, "#308 (b) the OLD journal is continued (cursor %u tip %u), not a fresh empty one", (unsigned)jrnapp_cursor(), (unsigned)jrnapp_tip());
  CHECK(jrnapp_prepare_key(&R, nw) == JRN_OK, "prepare at the safe moment");
  CHECK(jrn_key_resolve(&jrn_fatfs, ROOT, nw, &out) == JRN_OK && out == leg, "#308 (b) the REDIRECT new -> old now exists (the engine's own .pdr)");
  CHECK(jrnapp_step(-1, nm) == JRN_OK && jrnapp_step(-1, nm) == JRN_OK && memcmp(img, orig, GB_LEN) == 0, "#308 (b) undo through the old-keyed journal is byte-exact");
  CHECK(jrnapp_step(1, nm) == JRN_OK && jrnapp_step(1, nm) == JRN_OK && memcmp(img, edited, GB_LEN) == 0, "#308 (b) and redo");
  CHECK(jrnapp_flush() == JRN_OK, "flush the cursor markers");
  memcpy(base, img, sizeof base);
  edit(0x1100, 6, 0x22, "Box move", 0);
  CHECK(jrnapp_flush() == JRN_OK && jrnapp_tip() == 5, "recording continues in the adopted journal (tip %u)", (unsigned)jrnapp_tip());
  jrnapp_close(&R);
  CHECK(jrnapp_open_gb_compat(&R, img, nw, leg, 0, true) == JA_OK && jrnapp_tip() == 5, "#308 (b) a later session follows the redirect (tip %u)", (unsigned)jrnapp_tip());
  jrnapp_close(&R);
  CHECK(jrnapp_open_gb(&R, img, nw, 0, true) == JA_OK && jrnapp_tip() == 5, "#308 (b) even the plain open resolves the redirect (tip %u)", (unsigned)jrnapp_tip());
  jrnapp_close(&R);

  /* (c) ANOTHER save that shares the truncated legacy key (differs only after the '9'): the old journal does NOT anchor to
   * its image, so it is never adopted -- a fresh journal under the new key, no redirect, the old one untouched */
  CHECK(world9(n1, 3, &leg, &nw), "fresh card (the old-keyed journal again)");
  CHECK(jrnapp_open_gb(&R, img, leg, 0, true) == JA_OK && jrnapp_prepare_key(&R, leg) == JRN_OK, "old-style journal for save 1");
  edit(0x0100, 4, 0x5A, "Box move", 0);
  CHECK(jrnapp_flush() == JRN_OK && jrnapp_tip() == 1, "one step");
  jrnapp_close(&R);
  CHECK(build(GBF_RBY), "save 2's image");
  set_name(n2, 3); keys_now(); leg2 = legk; nw2 = newk;
  CHECK(leg2 == leg && nw2 != nw, "save 2 shares the legacy key but has its own new key");
  CHECK(jrnapp_open_gb_compat(&R, img, nw2, leg2, 0, true) == JA_OK, "compat open of save 2");
  CHECK(jrnapp_tip() == 0 && jrnapp_cursor() == 0, "#308 (c) the wrong save's old-keyed journal is NOT adopted (cursor %u tip %u)", (unsigned)jrnapp_cursor(), (unsigned)jrnapp_tip());
  CHECK(jrnapp_prepare_key(&R, nw2) == JRN_OK, "prepare");
  out = 0;
  CHECK(jrn_key_resolve(&jrn_fatfs, ROOT, nw2, &out) == JRN_OK && out == nw2, "#308 (c) no redirect to the old journal was written");
  jrnapp_close(&R);
  CHECK(build(GBF_RBY), "save 1's image again");
  set_name(n1, 3);
  CHECK(jrnapp_open_gb_compat(&R, img, nw, leg, 0, true) == JA_OK, "save 1 compat open");
  CHECK(jrnapp_state(&R) == JA_OK, "save 1 still opens (its old journal is intact)");
  jrnapp_close(&R);
}

int main(void) {
  t_key();
  t_everdrive_never_opens();
  t_hold_record_undo_redo();
  t_offer_after_power_cut();
  t_resync_hashes_the_baseline();
  t_cap_and_foreign_layout();
  t_clear_history();
  t_clear_card_error();
  t_discard_reanchor();
  t_key_entropy();
  t_old_key_compat();
  printf("%lu checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
