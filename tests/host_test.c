/*
 * Host test for the toolchain-independent logic in gen3_save.c / record_mix.c.
 * Runs on your PC (no devkitARM / no emulator), so you can sanity-check the
 * save-format math before ever touching the cartridge.
 *
 *   cc -I../source host_test.c ../source/gen3_save.c ../source/record_mix.c -o host_test && ./host_test
 *
 * (record_mix.c carries a _Static_assert that sizeof(SecretBase)==160, so a
 *  layout mistake fails at COMPILE time too.)
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gen3_save.h"
#include "record_mix.h"

/* ===================== record-mix merge tests ============================ */

/* Build one secret base. name is raw bytes (test charset doesn't matter, only
 * byte-equality), EOS-padded. loc==0 means an empty/unused slot. */
static SecretBase mk(uint8_t loc, uint8_t gender, uint16_t tid,
                     const char* name, uint16_t numRecv,
                     uint8_t battled, uint8_t regStatus) {
  SecretBase b;
  memset(&b, 0, sizeof(b));
  for (int i = 0; i < 7; i++) b.trainerName[i] = SB_EOS;
  b.secretBaseId = loc;
  b.trainerId[0] = (uint8_t)(tid & 0xFF);
  b.trainerId[1] = (uint8_t)(tid >> 8);
  if (name) {
    int i = 0;
    for (; name[i] && i < 7; i++) b.trainerName[i] = (uint8_t)name[i];
    for (; i < 7; i++) b.trainerName[i] = SB_EOS;
  }
  b.numSecretBasesReceived = numRecv;
  if (gender) b.flags |= SB_GENDER_BIT;
  if (battled) b.flags |= SB_BATTLED_BIT;
  sb_set_registryStatus(&b, regStatus);
  return b;
}

static int count_used(const SecretBase* arr) {
  int n = 0;
  for (int i = 0; i < G3_SECRET_BASES_COUNT; i++) if (arr[i].secretBaseId) n++;
  return n;
}

/* Find a host slot holding a base from trainer (tid, name); -1 if absent. */
static int find_by_trainer(const SecretBase* arr, uint16_t tid, const char* name) {
  SecretBase key = mk(1, 0, tid, name, 0, 0, 0);
  for (int i = 0; i < G3_SECRET_BASES_COUNT; i++) {
    if (!arr[i].secretBaseId) continue;
    SecretBase cur = arr[i];
    /* compare trainerId + name only */
    int same = 1;
    for (int k = 0; k < 4; k++) if (cur.trainerId[k] != key.trainerId[k]) same = 0;
    for (int k = 0; k < 7; k++) if (cur.trainerName[k] != key.trainerName[k]) same = 0;
    if (same) return i;
  }
  return -1;
}

static PlayerIdentity host_identity(uint8_t gender, uint16_t tid, const char* name) {
  PlayerIdentity id;
  memset(&id, 0, sizeof(id));
  id.gender = gender;
  id.trainerId[0] = (uint8_t)(tid & 0xFF);
  id.trainerId[1] = (uint8_t)(tid >> 8);
  for (int i = 0; i < 7; i++) id.trainerName[i] = SB_EOS;
  if (name) {
    int i = 0;
    for (; name[i] && i < 7; i++) id.trainerName[i] = (uint8_t)name[i];
  }
  return id;
}

static void test_merge(void) {
  /* ---- flag-byte accessors round-trip on the documented bit layout ---- */
  {
    SecretBase b;
    memset(&b, 0, sizeof(b));
    sb_set_toRegister(&b, 1);   assert(sb_toRegister(&b) == 1);
    b.flags = 0;
    b.flags |= SB_GENDER_BIT;     assert(sb_gender(&b) == 1);
    b.flags = 0;
    sb_set_battledToday(&b, 1);  assert(sb_battledToday(&b) == 1);
    sb_set_battledToday(&b, 0);  assert(sb_battledToday(&b) == 0);
    sb_set_registryStatus(&b, SB_REG_NEW);
    assert(sb_registryStatus(&b) == SB_REG_NEW);
    /* setters must not disturb neighbouring fields */
    b.flags = 0;
    b.flags |= SB_GENDER_BIT;
    sb_set_registryStatus(&b, SB_REG_REGISTERED);
    sb_set_toRegister(&b, 1);
    assert(sb_gender(&b) == 1);
    assert(sb_registryStatus(&b) == SB_REG_REGISTERED);
    assert(sb_toRegister(&b) == 1);
    assert(sb_battledToday(&b) == 0);
  }

  PlayerIdentity host = host_identity(0, 1111, "HOST");

  /* ---- basic import + battle refresh + counter bump ---- */
  {
    SecretBase h[20], f[20];
    memset(h, 0, sizeof(h));
    memset(f, 0, sizeof(f));
    /* host's own base at slot 0, location 10, 3 received so far */
    h[0] = mk(10, 0, 1111, "HOST", 3, 0, SB_REG_UNREGISTERED);
    /* friend's own base at slot 0, location 20, already battled today */
    f[0] = mk(20, 1, 2222, "FRND", 0, 1, SB_REG_UNREGISTERED);

    MixStats st;
    assert(recordmix_run(h, &host, G3_VER_EMERALD, f, G3_VER_EMERALD, &st));

    int fi = find_by_trainer(h, 2222, "FRND");
    assert(fi > 0);                                  /* imported into a non-0 slot */
    assert(h[fi].secretBaseId == 20);
    assert(sb_battledToday(&h[fi]) == 0);            /* refreshed: battleable now  */
    assert(h[0].numSecretBasesReceived == 4);        /* counter bumped             */
    assert(st.imported == 1);
    assert(st.refreshed == 1);
    assert(count_used(h) == 2);
  }

  /* ---- the host's own base is never re-imported from the friend's list ---- */
  {
    SecretBase h[20], f[20];
    memset(h, 0, sizeof(h));
    memset(f, 0, sizeof(f));
    h[0] = mk(10, 0, 1111, "HOST", 5, 0, SB_REG_UNREGISTERED);
    f[0] = mk(20, 1, 2222, "FRND", 0, 0, SB_REG_UNREGISTERED);
    /* friend collected a copy of the host's base */
    f[1] = mk(10, 0, 1111, "HOST", 4, 1, SB_REG_UNREGISTERED);

    MixStats st;
    recordmix_run(h, &host, G3_VER_EMERALD, f, G3_VER_EMERALD, &st);

    assert(st.host_base_evicted == true);
    /* host still has exactly one base from itself (slot 0) + friend's own */
    assert(count_used(h) == 2);
    assert(find_by_trainer(h, 2222, "FRND") > 0);
  }

  /* ---- dedup keeps the most-recently-updated copy ---- */
  {
    /* host already has trainer X's base (numRecv=9); friend's copy is older */
    SecretBase h[20], f[20];
    memset(h, 0, sizeof(h));
    memset(f, 0, sizeof(f));
    h[0] = mk(10, 0, 1111, "HOST", 1, 0, SB_REG_UNREGISTERED);
    h[1] = mk(30, 1, 3333, "XTRN", 9, 0, SB_REG_UNREGISTERED);
    f[0] = mk(20, 1, 2222, "FRND", 0, 0, SB_REG_UNREGISTERED);
    f[1] = mk(31, 1, 3333, "XTRN", 4, 1, SB_REG_UNREGISTERED); /* older copy */

    recordmix_run(h, &host, G3_VER_EMERALD, f, G3_VER_EMERALD, NULL);
    int xi = find_by_trainer(h, 3333, "XTRN");
    assert(xi >= 0);
    assert(h[xi].numSecretBasesReceived == 9);  /* kept host's newer copy        */
    assert(h[xi].secretBaseId == 30);           /* friend's older copy discarded */
  }
  {
    /* reverse: friend's copy is newer -> it replaces the host's */
    SecretBase h[20], f[20];
    memset(h, 0, sizeof(h));
    memset(f, 0, sizeof(f));
    h[0] = mk(10, 0, 1111, "HOST", 1, 0, SB_REG_UNREGISTERED);
    h[1] = mk(30, 1, 3333, "XTRN", 4, 0, SB_REG_UNREGISTERED);
    f[0] = mk(20, 1, 2222, "FRND", 0, 0, SB_REG_UNREGISTERED);
    f[1] = mk(31, 1, 3333, "XTRN", 9, 1, SB_REG_UNREGISTERED); /* newer copy */

    recordmix_run(h, &host, G3_VER_EMERALD, f, G3_VER_EMERALD, NULL);
    int xi = find_by_trainer(h, 3333, "XTRN");
    assert(xi >= 0);
    assert(h[xi].numSecretBasesReceived == 9);  /* friend's newer copy won  */
    assert(sb_battledToday(&h[xi]) == 0);        /* and it's battleable      */
  }

  /* ---- registered bases survive; overflow is reported, never overwrites ---- */
  {
    SecretBase h[20], f[20];
    memset(h, 0, sizeof(h));
    memset(f, 0, sizeof(f));
    h[0] = mk(10, 0, 1111, "HOST", 1, 0, SB_REG_UNREGISTERED);
    /* fill slots 1..19 with REGISTERED bases (cannot be evicted) */
    for (int i = 1; i < 20; i++)
      h[i] = mk((uint8_t)(100 + i), 1, (uint16_t)(4000 + i), "REG", 1, 0, SB_REG_REGISTERED);
    f[0] = mk(60, 1, 2222, "FRND", 0, 0, SB_REG_UNREGISTERED);

    MixStats st;
    recordmix_run(h, &host, G3_VER_EMERALD, f, G3_VER_EMERALD, &st);
    assert(count_used(h) == 20);                 /* still full          */
    assert(find_by_trainer(h, 2222, "FRND") == -1); /* friend didn't fit */
    assert(st.overflow == true);
  }

  /* ---- a friend base at the host's OWN location is not imported ---- */
  {
    SecretBase h[20], f[20];
    memset(h, 0, sizeof(h));
    memset(f, 0, sizeof(f));
    h[0] = mk(10, 0, 1111, "HOST", 1, 0, SB_REG_UNREGISTERED);
    /* friend's own base sits at location 10 (same map spot as host's) */
    f[0] = mk(10, 1, 2222, "FRND", 0, 0, SB_REG_UNREGISTERED);

    recordmix_run(h, &host, G3_VER_EMERALD, f, G3_VER_EMERALD, NULL);
    assert(count_used(h) == 1);                  /* only host's own base remains */
    assert(find_by_trainer(h, 2222, "FRND") == -1);
  }

  printf("MERGE TESTS PASSED\n");
}

/* ===================== party-decryption tests =========================== */

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static const char* const SUB_PERMS[24] = {
  "GAEM","GAME","GEAM","GEMA","GMAE","GMEA","AGEM","AGME","AEGM","AEMG","AMGE","AMEG",
  "EGAM","EGMA","EAGM","EAMG","EMGA","EMAG","MGAE","MGEA","MAGE","MAEG","MEGA","MEAG"};

/* Build a 100-byte struct Pokemon at `mon` by the INVERSE of gen3 decryption,
 * so the decoder is checked against an independently-encoded mon. */
static void build_mon(uint8_t* mon, uint32_t pers, uint32_t otid,
                      uint16_t species, uint16_t held, const uint16_t moves[4],
                      const uint8_t evs[6], uint8_t level, int is_egg) {
  memset(mon, 0, 100);
  put32(mon + 0x00, pers);
  put32(mon + 0x04, otid);
  mon[0x13] = is_egg ? 0x04 : 0x00;
  mon[0x54] = level;

  uint8_t plain[48];
  memset(plain, 0, sizeof(plain));
  const char* ord = SUB_PERMS[pers % 24];
  int pg = 0, pa = 0, pe = 0;
  for (int s = 0; s < 4; s++) {
    if (ord[s] == 'G') pg = s;
    if (ord[s] == 'A') pa = s;
    if (ord[s] == 'E') pe = s;
  }
  uint8_t* g = plain + pg * 12;
  uint8_t* a = plain + pa * 12;
  uint8_t* e = plain + pe * 12;
  put16(g + 0, species);
  put16(g + 2, held);
  for (int m = 0; m < 4; m++) put16(a + m * 2, moves[m]);
  for (int k = 0; k < 6; k++) e[k] = evs[k];

  uint16_t sum = 0;
  for (int h = 0; h < 24; h++) sum = (uint16_t)(sum + (plain[h*2] | (plain[h*2+1] << 8)));
  put16(mon + 0x1C, sum);

  uint32_t key = pers ^ otid;
  for (int w = 0; w < 12; w++) {
    uint32_t word = (uint32_t)plain[w*4] | ((uint32_t)plain[w*4+1] << 8) |
                    ((uint32_t)plain[w*4+2] << 16) | ((uint32_t)plain[w*4+3] << 24);
    put32(mon + 0x20 + w * 4, word ^ key);
  }
}

static void test_party(void) {
  static uint8_t sb1[G3_SAVEBLOCK1_BYTES];

  /* --- decode round-trip across several substruct orderings + skip rules --- */
  memset(sb1, 0, sizeof(sb1));
  uint32_t pv[4] = {0, 9, 18, 23};          /* GAEM, AEMG, MGAE, MEAG */
  uint16_t sp[4] = {149, 113, 282, 359};
  uint8_t  lv[4] = {100, 50, 62, 46};
  uint8_t  evs[6] = {252, 0, 4, 252, 0, 0}; /* avg = 508/6 = 84 */
  sb1[SB1_OFF_PARTY_COUNT] = 6;
  for (int i = 0; i < 4; i++) {
    uint16_t mv[4] = {(uint16_t)(57 + i), (uint16_t)(19 + i), 200, 63};
    build_mon(sb1 + SB1_OFF_PARTY + i * 100, pv[i], 0x12345678u,
              sp[i], (uint16_t)(100 + i), mv, evs, lv[i], 0);
  }
  /* slot 4: an egg -> skipped; slot 5: species 0 -> skipped */
  uint16_t mvx[4] = {1, 0, 0, 0};
  uint8_t  evx[6] = {0, 0, 0, 0, 0, 0};
  build_mon(sb1 + SB1_OFF_PARTY + 4 * 100, 7, 0xAAAA5555u, 25, 0, mvx, evx, 5, 1);
  build_mon(sb1 + SB1_OFF_PARTY + 5 * 100, 11, 0xBBBB4444u, 0, 0, mvx, evx, 0, 0);

  Gen3LiveParty live;
  assert(gen3_read_live_party(sb1, 0, &live) == 4);
  for (int i = 0; i < 4; i++) {
    assert(live.mon[i].species == sp[i]);
    assert(live.mon[i].level == lv[i]);
    assert(live.mon[i].heldItem == (uint16_t)(100 + i));
    assert(live.mon[i].moves[0] == (uint16_t)(57 + i));
    assert(live.mon[i].moves[3] == 63);
    assert(live.mon[i].avgEV == 84);
    assert(live.mon[i].personality == pv[i]);
  }

  /* --- compaction: an egg in the MIDDLE collapses the gap --- */
  memset(sb1, 0, sizeof(sb1));
  sb1[SB1_OFF_PARTY_COUNT] = 3;
  build_mon(sb1 + SB1_OFF_PARTY + 0 * 100, 0, 1u, 6,   0, mvx, evx, 36, 0); /* keep */
  build_mon(sb1 + SB1_OFF_PARTY + 1 * 100, 5, 2u, 200, 0, mvx, evx, 40, 1); /* egg  */
  build_mon(sb1 + SB1_OFF_PARTY + 2 * 100, 9, 3u, 380, 0, mvx, evx, 44, 0); /* keep */
  assert(gen3_read_live_party(sb1, 0, &live) == 2);
  assert(live.mon[0].species == 6 && live.mon[1].species == 380);

  /* --- omit mask drops the right RAW slots (mask is in slot space) --- */
  /* slots 0,1,2 kept (no egg); omit slots 0 and 2 -> only slot 1 (species 200) */
  memset(sb1, 0, sizeof(sb1));
  sb1[SB1_OFF_PARTY_COUNT] = 3;
  build_mon(sb1 + SB1_OFF_PARTY + 0 * 100, 0, 1u, 6,   0, mvx, evx, 36, 0);
  build_mon(sb1 + SB1_OFF_PARTY + 1 * 100, 5, 2u, 200, 0, mvx, evx, 40, 0);
  build_mon(sb1 + SB1_OFF_PARTY + 2 * 100, 9, 3u, 380, 0, mvx, evx, 44, 0);
  assert(gen3_read_live_party(sb1, (1u << 0) | (1u << 2), &live) == 1);
  assert(live.mon[0].species == 200);
  assert(gen3_read_live_party(sb1, 0x3F, &live) == 0);   /* omit-all -> empty */

  /* --- display read: nickname + stats + original_slot, egg shifts the slot --- */
  memset(sb1, 0, sizeof(sb1));
  sb1[SB1_OFF_PARTY_COUNT] = 3;
  build_mon(sb1 + SB1_OFF_PARTY + 0 * 100, 0, 1u, 6, 0, mvx, evx, 36, 0);
  build_mon(sb1 + SB1_OFF_PARTY + 1 * 100, 5, 2u, 25, 0, mvx, evx, 9, 1); /* egg */
  build_mon(sb1 + SB1_OFF_PARTY + 2 * 100, 9, 3u, 380, 0, mvx, evx, 44, 0);
  /* nickname "ZARD" on slot 2 (A=0xBB) + plaintext maxHP @0x58 */
  {
    uint8_t* m2 = sb1 + SB1_OFF_PARTY + 2 * 100;
    const char* nm = "ZARD";
    for (int k = 0; k < 4; k++) m2[0x08 + k] = (uint8_t)(0xBB + (nm[k] - 'A'));
    m2[0x08 + 4] = 0xFF;
    put16(m2 + 0x58, 281);  /* maxHP */
  }
  Gen3DisplayParty dp;
  assert(gen3_read_live_party_display(sb1, &dp) == 2);
  assert(dp.mon[0].original_slot == 0 && dp.mon[0].species == 6);
  assert(dp.mon[1].original_slot == 2 && dp.mon[1].species == 380);
  assert(dp.mon[1].level == 44 && dp.mon[1].maxHP == 281);
  assert(strcmp(dp.mon[1].nickname, "ZARD") == 0);

  /* --- gen3_rtc_days: date -> days-since-2000 (leap years) --- */
  assert(gen3_rtc_days(2000, 1, 1) == 0);
  assert(gen3_rtc_days(2000, 3, 1) == 60);     /* 2000 is a leap year */
  assert(gen3_rtc_days(2001, 1, 1) == 366);
  assert(gen3_rtc_days(2001, 3, 1) == 366 + 59);   /* 2000 leap; 2001 not */
  assert(gen3_rtc_days(2024, 1, 1) == gen3_rtc_days(2023, 1, 1) + 365);

  /* --- gen3_count_battleable: friend bases (skip own #0), battledOwnerToday --- */
  {
    memset(sb1, 0, sizeof(sb1));
    uint32_t bo = gen3_secret_base_offset(G3_VER_EMERALD);
    sb1[bo + 0 * 160 + 0] = 5;                          /* own base (skipped) */
    sb1[bo + 1 * 160 + 0] = 10; sb1[bo + 1 * 160 + 1] = (1u << 5); /* battled */
    sb1[bo + 2 * 160 + 0] = 11; sb1[bo + 2 * 160 + 1] = 0;         /* battleable */
    sb1[bo + 3 * 160 + 0] = 12; sb1[bo + 3 * 160 + 1] = 0;         /* battleable */
    int total = 0;
    int btl = gen3_count_battleable(sb1, G3_VER_EMERALD, &total);
    assert(total == 3 && btl == 2);
  }

  /* --- sb_set_party_from_live writes compacted + zeroes the tail --- */
  /* rebuild a 2-mon live party (slot 1 egg) for the checks below */
  memset(sb1, 0, sizeof(sb1));
  sb1[SB1_OFF_PARTY_COUNT] = 3;
  build_mon(sb1 + SB1_OFF_PARTY + 0 * 100, 0, 1u, 6,   0, mvx, evx, 36, 0);
  build_mon(sb1 + SB1_OFF_PARTY + 1 * 100, 5, 2u, 200, 0, mvx, evx, 40, 1); /* egg */
  build_mon(sb1 + SB1_OFF_PARTY + 2 * 100, 9, 3u, 380, 0, mvx, evx, 44, 0);
  gen3_read_live_party(sb1, 0, &live);

  SecretBase b;
  memset(&b, 0, sizeof(b));
  for (int i = 0; i < 7; i++) b.trainerName[i] = SB_EOS;
  b.secretBaseId = 5;
  sb_set_party_from_live(&b, &live);
  assert(b.species[0] == 6 && b.levels[0] == 36);
  assert(b.species[1] == 380 && b.levels[1] == 44);
  for (int i = 2; i < 6; i++) { assert(b.species[i] == 0); assert(b.levels[i] == 0); }

  /* --- no-op on an empty base (secretBaseId == 0) --- */
  SecretBase e0;
  memset(&e0, 0, sizeof(e0));
  sb_set_party_from_live(&e0, &live);
  assert(e0.species[0] == 0);

  printf("PARTY DECRYPT TESTS PASSED\n");
}

/* Fixture-guarded: only runs if the user's real saves are in tests/fixtures/.
 * Proves the bug + fix on real data (run from the tests/ directory). */
static void test_fixtures(void) {
  static uint8_t buf[G3_SAVE_FILE_SIZE];
  static uint8_t sb1[G3_SAVEBLOCK1_BYTES];
  struct { const char* path; Gen3Version ver; int expect_sp0; } fx[] = {
    {"fixtures/POKEMON_RUBY_AXVE02.sav", G3_VER_RS,      149}, /* Dragonite */
    {"fixtures/POKEMON_EMER_BPEE00.sav", G3_VER_EMERALD, -1},
  };
  int ran = 0;
  for (unsigned k = 0; k < sizeof(fx) / sizeof(fx[0]); k++) {
    FILE* f = fopen(fx[k].path, "rb");
    if (!f) { printf("  (fixture %s absent, skipping)\n", fx[k].path); continue; }
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n < (size_t)G3_SLOT_BYTES) { printf("  (fixture %s too small)\n", fx[k].path); continue; }
    ran++;

    Gen3SaveInfo info;
    assert(gen3_parse(buf, (uint32_t)n, &info));
    assert(info.sb1_ok);
    assert(info.version_guess == fx[k].ver);
    assert(gen3_read_saveblock1(buf, info.slot, sb1) == G3_SAVEBLOCK1_BYTES);
    assert(gen3_detect_game(sb1) == fx[k].ver);   /* secret-base presence id */

    uint32_t off = gen3_secret_base_offset(fx[k].ver);
    SecretBase* b0 = (SecretBase*)(sb1 + off);
    uint16_t before0 = b0->species[0];

    Gen3LiveParty live;
    int kept = gen3_read_live_party(sb1, 0, &live);
    printf("  %s [%s]: live kept=%d, live[0]=species %u lvl %u | stale base[0].species[0]=%u\n",
           fx[k].path, (fx[k].ver == G3_VER_RS) ? "RS" : "Emerald",
           kept, live.mon[0].species, live.mon[0].level, before0);

    sb_set_party_from_live(b0, &live);
    for (int i = 0; i < kept && i < 6; i++) {
      assert(b0->species[i] == live.mon[i].species);
      assert(b0->levels[i]  == live.mon[i].level);
    }
    if (fx[k].expect_sp0 >= 0) {
      assert(live.mon[0].species == (uint16_t)fx[k].expect_sp0); /* live has it */
      assert(before0 != (uint16_t)fx[k].expect_sp0);             /* stale lacked it */
      assert(b0->species[0] == (uint16_t)fx[k].expect_sp0);      /* fix restores it */
      printf("  -> PROVEN: stale base lacked species %d; regen restored it.\n", fx[k].expect_sp0);
    }
  }
  /* FireRed/LeafGreen have no secret bases -> must be EXCLUDED (detected UNKNOWN). */
  {
    FILE* f = fopen("fixtures/POKEMON_FIRE_BPRE01.sav", "rb");
    if (f) {
      size_t n = fread(buf, 1, sizeof(buf), f);
      fclose(f);
      Gen3SaveInfo info;
      if (gen3_parse(buf, (uint32_t)n, &info) && info.sb1_ok &&
          gen3_read_saveblock1(buf, info.slot, sb1) == G3_SAVEBLOCK1_BYTES) {
        assert(gen3_detect_game(sb1) == G3_VER_UNKNOWN);
        printf("  FireRed fixture correctly EXCLUDED (no secret bases).\n");
        ran++;
      }
    }
  }

  if (ran) printf("FIXTURE TESTS PASSED (%d save(s))\n", ran);
  else     printf("(no fixtures present; fixture tests skipped)\n");
}

int main(void) {
  /* --- struct layout --- */
  assert(sizeof(SecretBase) == 160);

  /* --- per-version secret-base offsets (from the decomps) --- */
  assert(gen3_secret_base_offset(G3_VER_EMERALD) == 0x1A9C);
  assert(gen3_secret_base_offset(G3_VER_RS)      == 0x1A08);
  assert(gen3_secret_base_offset(G3_VER_UNKNOWN) == 0);

  /* --- which SaveBlock1 sections the 3200-byte array touches --- */
  int f, l;
  gen3_sb1_touch_sections(G3_VER_EMERALD, &f, &l); assert(f == 2 && l == 3);
  gen3_sb1_touch_sections(G3_VER_RS,      &f, &l); assert(f == 2 && l == 3);

  /* --- checksum matches pokeemerald CalculateChecksum on hand-computed cases --- */
  /* one word = 1 -> (0>>16)+1 = 1 */
  uint8_t d1[4] = {1, 0, 0, 0};
  assert(gen3_checksum(d1, 4) == 1);
  /* 0xFFFFFFFF + 1 = 0 (mod 2^32); fold -> 0 */
  uint8_t d2[8] = {0xFF, 0xFF, 0xFF, 0xFF, 1, 0, 0, 0};
  assert(gen3_checksum(d2, 8) == 0);
  /* 0x0000FFFF + 0x00010000 = 0x0001FFFF; (u16)(1 + 0x1FFFF) = 0 */
  uint8_t d3[8] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
  assert(gen3_checksum(d3, 8) == 0x0000);
  /* four words of 0x100 -> 0x400 */
  uint8_t d4[16] = {0x00, 0x01, 0, 0, 0x00, 0x01, 0, 0,
                    0x00, 0x01, 0, 0, 0x00, 0x01, 0, 0};
  assert(gen3_checksum(d4, 16) == 0x0400);
  /* checksum only covers (size/4)*4 bytes; trailing 3 bytes ignored */
  uint8_t d5[7] = {1, 0, 0, 0, 0xAA, 0xBB, 0xCC};
  assert(gen3_checksum(d5, 7) == 1);

  /* --- charset decode spot checks --- */
  assert(gen3_decode_char(0xFF) == 0);    /* terminator */
  assert(gen3_decode_char(0x00) == ' ');
  assert(gen3_decode_char(0xBB) == 'A');
  assert(gen3_decode_char(0xD4) == 'Z');
  assert(gen3_decode_char(0xD5) == 'a');
  assert(gen3_decode_char(0xEE) == 'z');
  assert(gen3_decode_char(0xA1) == '0');
  assert(gen3_decode_char(0xAA) == '9');

  /* --- end-to-end parse on a synthesized minimal save --- */
  static uint8_t save[G3_SAVE_FILE_SIZE];
  memset(save, 0, sizeof(save));
  /* lay out slot 0 sectors 0..13 in order, valid signatures, counter=5 */
  for (int s = 0; s < G3_SECTORS_PER_SLOT; s++) {
    uint8_t* sec = save + (uint32_t)s * G3_SECTOR_SIZE;
    sec[G3_OFF_ID]     = (uint8_t)s;
    sec[G3_OFF_ID + 1] = 0;
    /* signature 0x08012025 little-endian */
    sec[G3_OFF_SIGNATURE + 0] = 0x25;
    sec[G3_OFF_SIGNATURE + 1] = 0x20;
    sec[G3_OFF_SIGNATURE + 2] = 0x01;
    sec[G3_OFF_SIGNATURE + 3] = 0x08;
    /* counter = 5 */
    sec[G3_OFF_COUNTER] = 5;
  }
  /* trainer name "RED" (R=0xCC? no: A=0xBB so R = 0xBB+17 = 0xCC) in section 0 */
  uint8_t* sb2 = save; /* section id 0 sits at sector 0 here */
  sb2[SB2_OFF_PLAYER_NAME + 0] = 0xBB + ('R' - 'A');
  sb2[SB2_OFF_PLAYER_NAME + 1] = 0xD5 + ('e' - 'a');
  sb2[SB2_OFF_PLAYER_NAME + 2] = 0xD5 + ('d' - 'a');
  sb2[SB2_OFF_PLAYER_NAME + 3] = 0xFF;
  sb2[SB2_OFF_GENDER] = 0;
  sb2[SB2_OFF_TRAINER_ID + 0] = 0x39; /* 12345 = 0x3039 */
  sb2[SB2_OFF_TRAINER_ID + 1] = 0x30;

  Gen3SaveInfo info;
  assert(gen3_parse(save, sizeof(save), &info));
  assert(info.valid);
  assert(info.slot == 0);
  assert(info.slot_valid[0] == true);
  assert(strcmp(info.trainer_name, "Red") == 0);
  assert(info.tid_public == 12345);
  assert(info.sections_found == 14);
  assert(info.sb1_ok == true);

  test_merge();
  test_party();
  test_fixtures();

  printf("ALL HOST ASSERTS PASSED\n");
  return 0;
}
