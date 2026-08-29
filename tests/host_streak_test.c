/* Host (PC) test for g3_record_win_streak — the SaveBlock2 BattleFrontier
 * current-win-streak reader that the battle-record export bakes into the .rec
 * filename. Three parts:
 *   1) synthetic SB2 image: every facility/battle-mode/level-mode lane resolves
 *      to the exact decomp offset, and every reject path returns -1;
 *   1b) sidecar streak block on a second synthetic SB2: current AND best are
 *      distinct numbers, correctly paired per level mode — regression cover
 *      for the current/current mislabel bug (see gen3_record.h);
 *   2) a real Emerald save (the fixture by default, or argv[1], e.g. a genuine
 *      post-game cart dump): gen3_parse picks the current slot, section 0 is
 *      located, and every facility's stored streaks read back in 0..9999.
 * Build + run (from the repo root):
 *   cc -std=c11 -I source tests/host_streak_test.c source/gen3_record.c \
 *      source/gen3_frontier.c source/gen3_dex.c source/gen3_flags.c source/gen3_save.c \
 *      source/gen3_mon.c -o /tmp/hs && /tmp/hs [path/to/emerald.sav]
 * (gen3_record now delegates its lane offsets to gen3_frontier — one table in the tree.
 * gen3_dex/gen3_flags are pulled in by g3_record_sidecar's SAVE STATE block.)
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "gen3_trainer.h"   /* PkGame */
#include "gen3_record.h"
#include "gen3_frontier.h"  /* G3F_ARENA / G3F_LVL_50 / G3F_LVL_OPEN / G3F_CURRENT / G3F_RECORD, 1b block */
#include "gen3_save.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } } while (0)

static uint8_t g_buf[G3_SAVE_FILE_SIZE];

static uint32_t load(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("cannot open %s\n", path); return 0; }
  uint32_t n = (uint32_t)fread(g_buf, 1, sizeof g_buf, f);
  fclose(f);
  return n;
}

static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* Decomp-cited absolute SB2 offsets (pokeemerald include/global.h:393-437). */
static const struct { const char* name; uint16_t off; uint8_t modes; } k_fac[7] = {
  { "Tower",   0xCE0, 4 }, { "Dome", 0xD0C, 2 }, { "Palace", 0xDC8, 2 },
  { "Arena",   0xDDA, 1 }, { "Factory", 0xDE2, 2 }, { "Pike", 0xE04, 1 },
  { "Pyramid", 0xE1A, 1 },
};

static G3RecordInfo mk(int fac, int lvl, uint32_t flags) {
  G3RecordInfo ri;
  memset(&ri, 0, sizeof ri);
  ri.facility = (uint8_t)fac;
  ri.lvl_mode = (uint8_t)lvl;
  ri.battle_flags = flags;
  return ri;
}

int main(int argc, char** argv) {
  /* ---- 1) synthetic SB2: exact lane selection --------------------------- */
  static uint8_t sb2[G3_SECTOR_DATA_SIZE];
  memset(sb2, 0xEE, sizeof sb2);            /* poison: a wrong offset reads 0xEEEE (> 9999) -> -1 */
  for (int f = 0; f < 7; f++)
    for (int m = 0; m < k_fac[f].modes; m++)
      for (int l = 0; l < 2; l++)           /* unique, plausible value per lane */
        wr16(sb2 + k_fac[f].off + (m * 2 + l) * 2, (uint16_t)(1000 * f + 10 * m + l + 1));

  static const uint32_t k_modeflags[4] = {  /* battleFlags per FRONTIER_MODE_* */
    0,                                      /* singles                          */
    1u << 0,                                /* BATTLE_TYPE_DOUBLE               */
    1u << 6,                                /* BATTLE_TYPE_MULTI                */
    (1u << 6) | (1u << 23),                 /* MULTI | TOWER_LINK_MULTI         */
  };
  for (int f = 0; f < 7; f++)
    for (int m = 0; m < k_fac[f].modes; m++)
      for (int l = 0; l < 2; l++) {
        G3RecordInfo ri = mk(f, l, k_modeflags[m]);
        int got = g3_record_win_streak(sb2, &ri);
        char msg[80];
        snprintf(msg, sizeof msg, "%s mode %d lvl %d reads its own lane", k_fac[f].name, m, l);
        CHECK(got == 1000 * f + 10 * m + l + 1, msg);
      }

  /* reject paths */
  { G3RecordInfo ri = mk(7, 0, 0);  CHECK(g3_record_win_streak(sb2, &ri) == -1, "facility 7 -> -1"); }
  { G3RecordInfo ri = mk(0, 2, 0);  CHECK(g3_record_win_streak(sb2, &ri) == -1, "lvlMode 2 -> -1"); }
  { G3RecordInfo ri = mk(1, 0, 1u << 6);   /* multis at the Dome: no lane */
    CHECK(g3_record_win_streak(sb2, &ri) == -1, "Dome multis -> -1"); }
  { G3RecordInfo ri = mk(3, 0, 1u << 0);   /* doubles at the Arena: no lane */
    CHECK(g3_record_win_streak(sb2, &ri) == -1, "Arena doubles -> -1"); }
  { G3RecordInfo ri = mk(0, 0, 0);
    wr16(sb2 + 0xCE0, 10000);              /* > MAX_STREAK 9999 -> implausible */
    CHECK(g3_record_win_streak(sb2, &ri) == -1, "streak 10000 -> -1");
    wr16(sb2 + 0xCE0, 9999);
    CHECK(g3_record_win_streak(sb2, &ri) == 9999, "streak 9999 (MAX_STREAK) is accepted");
    CHECK(g3_record_win_streak(NULL, &ri) == -1, "NULL sb2 -> -1");
    CHECK(g3_record_win_streak(sb2, NULL) == -1, "NULL ri -> -1"); }

  /* short facility names used by the export filename */
  CHECK(strcmp(g3_record_facility_short(0), "Tower") == 0 &&
        strcmp(g3_record_facility_short(6), "Pyramid") == 0 &&
        strcmp(g3_record_facility_short(7), "?") == 0, "facility short names");

  /* ---- 1b) sidecar streak block: current AND best, unambiguously paired ---
   * Ground truth: Guy's real export once showed "Arena singles 0/42" where 0
   * was Lv50-CURRENT and 42 was Open-CURRENT — both current, never a
   * current/best pair. This reproduces that exact lane (Arena is singles-only,
   * so mode 0 is its only lane) with a genuine current < best gap on both
   * level modes, written through g3f_streak_set (the real production writer,
   * not a hand-rolled offset) so the test can't silently desync from a future
   * lane-offset change. `save` is the still-zeroed g_buf (real content loads
   * in part 2, below): g3_record_party/pk_decode_mon read it safely either
   * way, since an all-zero party just decodes to "no species" and the team
   * loop skips it — the streak block does not depend on the team data. */
  {
    static uint8_t sb2b[G3_SECTOR_DATA_SIZE];
    memset(sb2b, 0, sizeof sb2b);
    CHECK(g3f_streak_set(sb2b, G3F_ARENA, 0, G3F_LVL_50,   G3F_CURRENT, 0)  == 0,  "set Arena Lv50 current");
    CHECK(g3f_streak_set(sb2b, G3F_ARENA, 0, G3F_LVL_50,   G3F_RECORD,  17) == 17, "set Arena Lv50 best");
    CHECK(g3f_streak_set(sb2b, G3F_ARENA, 0, G3F_LVL_OPEN, G3F_CURRENT, 42) == 42, "set Arena Open current (Guy's real number)");
    CHECK(g3f_streak_set(sb2b, G3F_ARENA, 0, G3F_LVL_OPEN, G3F_RECORD,  99) == 99, "set Arena Open best");

    G3RecordInfo ri2; memset(&ri2, 0, sizeof ri2);
    static char sc2[2048];
    int sn2 = g3_record_sidecar(sc2, sizeof sc2, &ri2, g_buf, sb2b, NULL, PK_EMERALD, 1, NULL);
    CHECK(sn2 > 0 && sc2[sn2] == 0, "1b sidecar built");
    CHECK(strstr(sc2, "streaks (current/best)") != NULL, "1b header names both current and best");
    CHECK(strstr(sc2, "Arena Lv50 0/17") != NULL, "1b Lv50 line: current 0, best 17, correctly paired");
    CHECK(strstr(sc2, "Arena Open 42/99") != NULL, "1b Open line: current 42, best 99 (best > current)");
    /* The old bug printed Lv50-current/Open-current as if it were a cur/best pair,
     * i.e. "0/42" — that exact substring must never appear once best is tracked. */
    CHECK(strstr(sc2, "0/42") == NULL, "1b old current/current mislabel (0/42) is gone");
  }

  /* ---- 1c) honest degradation when there is no Frontier struct to read ---
   * RS has no stored CURRENT streak at all (it's derived, gen3_frontier.h) and
   * never reaches this export in practice (RS predates the Frontier), but the
   * function is pure C and callable with any game/sb2 — a caller passing RS,
   * or Emerald with a NULL sb2, must get an honest "not available" line, never
   * a fabricated 0. */
  {
    static uint8_t sb2c[G3_SECTOR_DATA_SIZE];
    memset(sb2c, 0, sizeof sb2c);
    G3RecordInfo ri3; memset(&ri3, 0, sizeof ri3);
    static char sc3[2048];
    int sn3 = g3_record_sidecar(sc3, sizeof sc3, &ri3, g_buf, sb2c, NULL, PK_RS, 1, NULL);
    CHECK(strstr(sc3, "not available") != NULL, "1c RS: streaks degrade honestly");
    CHECK(strstr(sc3, "current/best") == NULL, "1c RS: no per-lane table is printed at all");

    static char sc4[2048];
    int sn4 = g3_record_sidecar(sc4, sizeof sc4, &ri3, g_buf, NULL, NULL, PK_EMERALD, 1, NULL);
    CHECK(strstr(sc4, "not available") != NULL, "1c Emerald + NULL sb2: streaks degrade honestly too");
    CHECK(sn3 > 0 && sn4 > 0, "1c both degraded sidecars still produced output");
  }

  /* ---- 2) a real Emerald save ------------------------------------------- */
  const char* path = (argc > 1) ? argv[1] : "tests/fixtures/POKEMON_EMER_BPEE00.sav";
  uint32_t sz = load(path);
  CHECK(sz == G3_SAVE_FILE_SIZE, "save is 128 KiB");
  if (sz == G3_SAVE_FILE_SIZE) {
    Gen3SaveInfo info;
    CHECK(gen3_parse(g_buf, sz, &info) && info.valid, "gen3_parse: save valid");
    CHECK(info.slot == 0 || info.slot == 1, "current slot selected");
    printf("save: \"%s\" slot=%d counters=%u/%u\n",
           info.trainer_name, info.slot,
           (unsigned)info.counter[0], (unsigned)info.counter[1]);

    int s0 = gen3_find_section(g_buf, info.slot, G3_SID_SAVEBLOCK2);
    CHECK(s0 >= 0, "SaveBlock2 (section 0) found in the current slot");
    if (s0 >= 0) {
      const uint8_t* save_sb2 =
          g_buf + (uint32_t)info.slot * G3_SLOT_BYTES + (uint32_t)s0 * G3_SECTOR_SIZE;
      /* SaveBlock1 too: the sidecar's symbol field reads event flags, which live there. */
      static uint8_t g_sb1[G3_SAVEBLOCK1_BYTES];
      CHECK(gen3_read_saveblock1(g_buf, info.slot, g_sb1), "SaveBlock1 reassembled");

      /* every facility's stored streaks must be plausible on a genuine save */
      for (int f = 0; f < 7; f++) {
        printf("  %-8s", k_fac[f].name);
        for (int m = 0; m < k_fac[f].modes; m++)
          for (int l = 0; l < 2; l++) {
            uint16_t v = (uint16_t)(save_sb2[k_fac[f].off + (m * 2 + l) * 2] |
                                    (save_sb2[k_fac[f].off + (m * 2 + l) * 2 + 1] << 8));
            printf(" m%d/%s=%u", m, l ? "O" : "50", v);
            CHECK(v <= G3_REC_MAX_STREAK, "stored streak in 0..9999");
          }
        printf("\n");
      }

      /* and the export's own lookup: the streak for THIS save's record */
      G3RecordInfo ri;
      if (g3_record_scan(g_buf, sz, &ri) && ri.checksum_ok) {
        int streak = g3_record_win_streak(save_sb2, &ri);
        printf("record: %s %s -> filename tag \"_%s-%s-%d\"\n",
               g3_record_facility_name(ri.facility), ri.lvl_mode ? "Open" : "Lv50",
               g3_record_facility_short(ri.facility), ri.lvl_mode ? "O" : "50", streak);
        CHECK(streak >= 0 && streak <= G3_REC_MAX_STREAK,
              "record's own streak reads back plausible");

        /* ---- 3) the export sidecar builder on the same real record ---- */
        static char sc[2048];
        int sn = g3_record_sidecar(sc, sizeof sc, &ri, g_buf, save_sb2, g_sb1,
                                   PK_EMERALD, 12345, "27-07-2026 12:00");
        CHECK(sn > 200 && sn < (int)sizeof sc, "sidecar length sane");
        CHECK(sc[sn] == 0 && (int)strlen(sc) == sn, "sidecar NUL-terminated, length == strlen");
        CHECK(strstr(sc, "PokeDNA battle record export") == sc, "sidecar header first");
        CHECK(strstr(sc, "27-07-2026 12:00") != NULL, "sidecar carries the stamp");
        CHECK(strstr(sc, "IDNo 12345") != NULL, "sidecar carries the TID");
        CHECK(strstr(sc, g3_record_facility_name(ri.facility)) != NULL, "sidecar names the facility");
        CHECK(strstr(sc, "player team:") && strstr(sc, "opponent team:"), "sidecar lists both teams");
        CHECK(strstr(sc, "streaks (current/best)") != NULL, "sidecar carries the current/best streak table");
        CHECK(strstr(sc, "Pyramid") != NULL, "streak table covers all facilities");
        /* ---- the save-state block rec2mp4 consumes (docs/REC-SIDECAR.md) ---- */
        CHECK(strstr(sc, "state.playtime: ")     != NULL, "sidecar carries play time");
        CHECK(strstr(sc, "state.dex_seen: ")     != NULL, "sidecar carries dex seen");
        CHECK(strstr(sc, "state.dex_caught: ")   != NULL, "sidecar carries dex caught");
        CHECK(strstr(sc, "state.bp: ")           != NULL, "sidecar carries BP");
        CHECK(strstr(sc, "state.symbols: ")      != NULL, "sidecar carries the symbol string");
        { const char* sy = strstr(sc, "state.symbols: ");
          int ok = 1;
          for (int i = 0; i < 7; i++) {
            char ch = sy[15 + i];
            if (ch != '-' && ch != 's' && ch != 'G') ok = 0;
          }
          CHECK(ok && sy[22] == '\n', "symbol field is exactly 7 chars of -/s/G");
          /* seen >= caught is an invariant of the two bit arrays, not a coincidence */
          int seen = atoi(strstr(sc, "state.dex_seen: ") + 16);
          int caught = atoi(strstr(sc, "state.dex_caught: ") + 18);
          CHECK(seen >= caught, "dex seen >= caught"); }
        /* truncation safety: a tiny cap must not overflow or lose the NUL */
        char tiny[64];
        int tn = g3_record_sidecar(tiny, sizeof tiny, &ri, g_buf, save_sb2, g_sb1,
                                   PK_EMERALD, 1, 0);
        CHECK(tn < (int)sizeof tiny && tiny[tn] == 0, "sidecar truncates safely");
        printf("sidecar: %d bytes\n%s\n", sn, strstr(sc, "state.playtime") ? strstr(sc, "state.playtime") : "(no state block)");
      } else {
        printf("record: none stored in this save (streak lookup not exercised)\n");
      }
    }
  }

  if (g_fail) { printf("FAILURES: %d\n", g_fail); return 1; }
  printf("OK: host_streak_test (0 failures)\n");
  return 0;
}
