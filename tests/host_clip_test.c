/* Host test for the clipboard + slot-ops foundation (gen3_clip.c).
 *   cc -std=c11 -I source tests/host_clip_test.c source/gen3_save.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_edit.c source/gen3_daycare.c source/gen3_clip.c source/data_tables.c -o /tmp/hc
 *   /tmp/hc tests/fixtures/POKEMON_EMER_BPEE00.sav
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "gen3_edit.h"
#include "gen3_clip.h"
#include "data_tables.h"

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)

int main(int argc, char** argv) {
  const char* path = (argc > 1) ? argv[1] : "tests/fixtures/POKEMON_EMER_BPEE00.sav";
  FILE* f = fopen(path, "rb");
  if (!f) { printf("cannot open %s\n", path); return 2; }
  static uint8_t save[G3_SAVE_FILE_SIZE];
  size_t n = fread(save, 1, sizeof(save), f);
  fclose(f);
  Gen3SaveInfo info;
  if (!gen3_parse(save, (uint32_t)n, &info)) { printf("parse FAILED\n"); return 1; }

  static uint8_t sb1[G3_SAVEBLOCK1_BYTES];
  static uint8_t pc[G3_PC_BYTES];
  gen3_read_saveblock1(save, info.slot, sb1);
  gen3_read_pc_storage(save, info.slot, pc);

  bool frlg = false;
  PkMon party[6];
  int pcount = pk_read_party_auto(sb1, party, &frlg);
  printf("== %s == (party=%d, frlg=%d)\n", path, pcount, frlg);

  /* find a real box mon to play with */
  const uint8_t* box_rec = NULL;
  for (int b = 0; b < G3_TOTAL_BOXES && !box_rec; b++)
    for (int s = 0; s < 30; s++) {
      const uint8_t* r = pc + 0x0004 + ((uint32_t)b * 30 + s) * 80;
      PkMon m;
      if (pk_decode_mon(r, false, &m) && !m.isBadEgg) { box_rec = r; break; }
    }
  CHECK(box_rec != NULL, "found a box mon to test");
  CHECK(pcount > 0, "save has a party mon");
  if (!box_rec || pcount == 0) { printf("\nFAIL: missing fixtures\n"); return 1; }
  const uint8_t* party_rec = sb1 + (frlg ? 0x0038 : 0x0238);

  /* (1) box -> party -> box preserves the 80 box bytes exactly */
  {
    ClipMon c; uint8_t p100[100], b80[100];
    clip_copy_from(&c, box_rec, false);
    CHECK(clip_to_record(&c, true, p100), "box->party produced a record");
    clip_copy_from(&c, p100, true);
    CHECK(clip_to_record(&c, false, b80), "party->box produced a record");
    CHECK(memcmp(b80, box_rec, 80) == 0, "box->party->box preserves the 80 box bytes");
    /* the derived party form must decode with valid stats (level>0) */
    PkMon m; pk_decode_mon(p100, true, &m);
    printf("(1) box->party->box ok; derived party Lv=%u\n", m.level);
    CHECK(!m.isBadEgg && m.level >= 1, "box->party derives a valid level/stats");
    /* BACKLOG #225: retail's "no mail" is MAIL_NONE (0xFF), not 0 -- 0 is a
     * real mail-slot index. Every box->party expansion in the tree routes
     * through em_set_party_flag(e, true) (gen3_edit.c), so pinning it here
     * (via clip_to_record, the clipboard's own box->party path) covers
     * box_to_party/party_append/the Day-Care return/party_place_held too --
     * they all call the same function. */
    CHECK(p100[G3_PARTY_MAIL_OFF] == G3_MAIL_NONE,
          "box->party writes MAIL_NONE (0xFF) at 0x55, not 0x00");
    printf("(1b) party mail byte 0x55 = 0x%02X\n", p100[G3_PARTY_MAIL_OFF]);

    /* (1c) b225 review D1: CalculateMonStats forces Shedinja's Max HP to 1 in all
     * three families; gen3_legality2.c already grades anything else INVALID, so the
     * box->party expansion has to agree with it. */
    {
      uint8_t shed80[80], shed100[100];
      gen3_build_mon(SPECIES_SHEDINJA, 20, 0x12345678u, 0x0000BEEFu, "GUY", 3, shed80);
      ClipMon sc;
      clip_copy_from(&sc, shed80, false);
      CHECK(clip_to_record(&sc, true, shed100), "shedinja box->party produced a record");
      uint16_t shed_max = (uint16_t)(shed100[0x58] | (shed100[0x59] << 8));
      uint16_t shed_cur = (uint16_t)(shed100[0x56] | (shed100[0x57] << 8));
      CHECK(shed_max == 1 && shed_cur == 1,
            "box->party gives Shedinja Max HP 1, not the stat formula");
      printf("(1c) shedinja party HP %u/%u\n", shed_cur, shed_max);
    }
  }

  /* (2) party -> box == the party record's first 80 bytes */
  {
    ClipMon c; uint8_t b80[100];
    clip_copy_from(&c, party_rec, true);
    clip_to_record(&c, false, b80);
    CHECK(memcmp(b80, party_rec, 80) == 0, "party->box == first 80 bytes of the party record");
  }

  /* (3) same-kind paste is byte-identical */
  {
    ClipMon c; uint8_t out[100];
    clip_copy_from(&c, box_rec, false);
    clip_to_record(&c, false, out);
    CHECK(memcmp(out, box_rec, 80) == 0, "box->box is byte-identical");
    clip_copy_from(&c, party_rec, true);
    clip_to_record(&c, true, out);
    CHECK(memcmp(out, party_rec, 100) == 0, "party->party is byte-identical");
  }

  /* (4) party_append fills to 6 then refuses; (5) party_release compacts + decrements */
  {
    static uint8_t w[G3_SAVEBLOCK1_BYTES];
    memcpy(w, sb1, sizeof(w));
    uint8_t rec[100]; memcpy(rec, party_rec, 100);     /* clone an existing party mon */
    int appended = 0;
    while (party_count(w, frlg) < 6) { CHECK(party_append(w, frlg, rec), "append succeeds while <6"); appended++; }
    CHECK(party_count(w, frlg) == 6, "party filled to 6");
    CHECK(!party_append(w, frlg, rec), "append refuses at 6");
    printf("(4) party_append: filled to 6 (+%d), refused at 6\n", appended);

    /* mark slot 1 distinctly, release slot 0, assert old slot 1 is now slot 0 */
    uint8_t* s1 = pk_party_slot(w, frlg, 1);
    s1[0x08] = 0x42;                                    /* a recognizable nickname byte */
    uint8_t saved1[100]; memcpy(saved1, s1, 100);
    party_release(w, frlg, 0);
    CHECK(party_count(w, frlg) == 5, "release decremented the count");
    CHECK(memcmp(pk_party_slot(w, frlg, 0), saved1, 100) == 0, "release shifted slot 1 down to slot 0");
    /* the now-vacant last slot (index 5) is zeroed */
    uint8_t zero[100] = {0};
    CHECK(memcmp(pk_party_slot(w, frlg, 5), zero, 100) == 0, "release zeroed the vacated last slot");
    printf("(5) party_release: compacted to %d, slot1->slot0, tail cleared\n", party_count(w, frlg));
  }

  /* (6) BACKLOG #227 review D1: g3_item_is_mail's own boundary, value-checked here
   * (not just "some call to it exists before a write", the way the pdna_main.c-side
   * structural pins can only prove). ORANGE MAIL..RETRO MAIL is 121..132. */
  {
    CHECK(!g3_item_is_mail(0), "item 0 (none) is not mail");
    CHECK(!g3_item_is_mail(120), "item 120 (one below the range) is not mail");
    CHECK(g3_item_is_mail(121), "item 121 (ORANGE MAIL, the low edge) is mail");
    CHECK(g3_item_is_mail(132), "item 132 (RETRO MAIL, the high edge) is mail");
    CHECK(!g3_item_is_mail(133), "item 133 (one above the range) is not mail");
    CHECK(!g3_item_is_mail(0xFFFF), "item 0xFFFF (out of range) is not mail");
    printf("(6) g3_item_is_mail boundary: 0/120/121/132/133/0xFFFF -> "
           "%d/%d/%d/%d/%d/%d\n",
           g3_item_is_mail(0), g3_item_is_mail(120), g3_item_is_mail(121),
           g3_item_is_mail(132), g3_item_is_mail(133), g3_item_is_mail(0xFFFF));
  }

  printf("\n%s: %d failure(s)\n", g_fail ? "FAIL" : "OK", g_fail);
  return g_fail ? 1 : 0;
}
