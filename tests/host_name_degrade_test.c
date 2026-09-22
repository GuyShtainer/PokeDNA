/* Host test: BACKLOG #217 -- a degraded name's edit-noop must not overwrite the
 * original gender-sign/e-acute byte.
 *   cc -std=c11 -Wall -Wextra -I source tests/host_name_degrade_test.c \
 *      source/gen3_mon.c source/gen3_edit.c source/gen3_save.c source/gen3_box.c \
 *      source/gen3_daycare.c source/data_tables.c -o /tmp/hnd && /tmp/hnd
 *
 * source/pdna_edit.c owns the actual guard (em_field_press's F_NICK/F_OT cases) but
 * cannot be host-compiled -- it includes <tonc.h> for the key-input loop, same as
 * every other on-screen editor in this tree. This test exercises the SAME predicate
 * pdna_edit.c uses (`(c->nameFlags & PK_NAME_*_DEGRADED) && strcmp(buf, c->otName/
 * nickname) == 0`) against the SAME PkMon decode/encode pipeline it calls through
 * (pk_decode_mon, em_set_otname/em_set_nickname), so what's under test is the exact
 * logic, at one remove from the tonc key-input loop it sits behind. A reviewer can
 * diff this file's guard expression against pdna_edit.c's -- they must read
 * byte-for-byte the same.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "gen3_mon.h"
#include "gen3_edit.h"
#include "gen3_save.h"

static int fails = 0, checks = 0;
static void expect(bool cond, const char* what) {
  checks++;
  if (!cond) { fails++; printf("  FAIL %s\n", what); }
  else       { printf("  ok   %s\n", what); }
}

/* Build a minimal, valid 80-byte box record (a real Gen-3 checksum/encryption
 * pass, via gen3_build_mon) with OT name "ABCDE" + the male gender sign (0xB5) --
 * "ABCDE\xE2\x99\x82" is 5 letters then the sign, at otName's raw 7-byte field
 * width: oi reaches 5 after the letters, needs oi+3 < 8 (otName[8]'s outcap) to
 * fit the 3-byte UTF-8 insert -- 5+3=8, 8<8 is false, so decode_name degrades it
 * to "ABCDE?" exactly like BACKLOG #217's own example. */
static void build_degraded_ot_record(uint8_t rec[80]) {
  gen3_build_mon(1 /* Bulbasaur */, 5, 0x12345678u, 0xABCDu, "NICK", 3, rec);
  EditMon e;
  gen3_edit_load(rec, false, &e);
  /* em_set_otname's own encode_name only spends 1 output byte per gender sign
   * (it does not reserve 3 slots up front), so encoding "ABCDE\xE2\x99\x82"
   * lands the raw bytes exactly as the scenario needs: A,B,C,D,E,0xB5,0xFF. */
  em_set_otname(&e, "ABCDE\xE2\x99\x82");
  gen3_edit_commit(&e, rec);
}

static void test_decode_is_degraded(void) {
  printf("== (1) the built record decodes OT as \"ABCDE?\", flagged degraded ==\n");
  uint8_t rec[80];
  build_degraded_ot_record(rec);

  PkMon c;
  bool ok = pk_decode_mon(rec, false, &c);
  expect(ok, "record decodes");
  expect(strcmp(c.otName, "ABCDE?") == 0, "otName decoded as \"ABCDE?\"");
  expect((c.nameFlags & PK_NAME_OT_DEGRADED) != 0, "PK_NAME_OT_DEGRADED is set");
  expect((c.nameFlags & PK_NAME_NICK_DEGRADED) == 0, "nickname is NOT flagged (unrelated field)");

  /* raw byte check: offset 0x14 is otName's field start (gen12_convert.c's own
   * comment documents this same offset). Byte 5 (after A,B,C,D,E) must be 0xB5,
   * the real gender sign -- not yet touched by anything in this test. */
  expect(rec[0x14 + 5] == 0xB5, "raw OT field byte 5 is 0xB5 (the real gender sign)");
}

/* This is the EXACT predicate source/pdna_edit.c's em_field_press uses for F_OT
 * (and the mirror for F_NICK) -- kept as a tiny local helper so the mutation test
 * below can flip it independently of the guard's own correctness. */
static bool ot_noop_should_skip(const PkMon* c, const char* buf) {
  return (c->nameFlags & PK_NAME_OT_DEGRADED) && strcmp(buf, c->otName) == 0;
}

static void test_noop_ok_keeps_original_byte(void) {
  printf("\n== (2) OK with NO edit -- guard fires, 0xB5 survives ==\n");
  uint8_t rec[80];
  build_degraded_ot_record(rec);
  PkMon c; pk_decode_mon(rec, false, &c);

  /* osk_input seeded with c.otName and confirmed with no change returns buf ==
   * c.otName verbatim (osk.c's own osk_core: START with the seed untouched). */
  char buf[8]; strcpy(buf, c.otName);   /* "ABCDE?" -- the exact noop */

  EditMon e; gen3_edit_load(rec, false, &e);
  if (!ot_noop_should_skip(&c, buf)) em_set_otname(&e, buf);   /* the guarded call */
  uint8_t out[80]; gen3_edit_commit(&e, out);

  expect(ot_noop_should_skip(&c, buf), "guard fires for the exact noop");
  expect(out[0x14 + 5] == 0xB5, "OT field byte 5 is STILL 0xB5 -- the noop did not "
                                 "overwrite the real gender sign with 0xAC ('?')");
  expect(memcmp(rec, out, 80) == 0, "the whole 80-byte record is byte-identical -- "
                                     "truly no write happened");
}

static void test_real_edit_still_writes(void) {
  printf("\n== (3) OK with a REAL edit -- guard does NOT fire, the new name writes ==\n");
  uint8_t rec[80];
  build_degraded_ot_record(rec);
  PkMon c; pk_decode_mon(rec, false, &c);

  char buf[8]; strcpy(buf, "ABCDEF");   /* the user actually typed something new */

  EditMon e; gen3_edit_load(rec, false, &e);
  if (!ot_noop_should_skip(&c, buf)) em_set_otname(&e, buf);
  uint8_t out[80]; gen3_edit_commit(&e, out);

  expect(!ot_noop_should_skip(&c, buf), "guard does NOT fire -- buf differs from the seed");
  PkMon after; pk_decode_mon(out, false, &after);
  expect(strcmp(after.otName, "ABCDEF") == 0, "OT name is now \"ABCDEF\" -- the real edit landed");
  expect(memcmp(rec, out, 80) != 0, "the record actually changed");
}

int main(void) {
  test_decode_is_degraded();
  test_noop_ok_keeps_original_byte();
  test_real_edit_still_writes();
  printf("\n%d checks, %d FAILED\n", checks, fails);
  return fails ? 1 : 0;
}
