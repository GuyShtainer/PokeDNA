/* Host (PC) test for gen3_frontier — the Emerald Battle Frontier streak tables and
 * their safe writers, plus the Ruby/Sapphire Battle Tower record pair.
 *
 * What this pins down (each of these was a real bug risk found in research):
 *   1) lvlMode is the INNER array index — getting it backwards silently swaps Lv50
 *      with Open on every multi-mode facility;
 *   2) each facility's RECORD array starts exactly where its CURRENT array ends,
 *      and the Factory record array ends on 0xDF2 (factoryRentsCount) — the offset
 *      pret's own decomp comment gets wrong (it says 0xDF6);
 *   3) winStreakActiveFlags bits are NOT sequential by facility, and a CURRENT
 *      streak written without its bit is wiped by the game's challenge-init;
 *   4) Frontier Brain symbols use `==` with a per-facility modifier, so the value
 *      to STORE for Tower Silver is 34, not 35;
 *   5) nothing is ever written past SB2 0xF2B (the section tail must stay zero);
 *   6) RS 0x572 is a derived cache — writing a record must refresh it.
 *
 * Build + run (from the repo root):
 *   cc -I source tests/host_frontier_test.c source/gen3_frontier.c source/gen3_save.c \
 *      -o /tmp/hf && /tmp/hf [path/to/emerald.sav]
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_frontier.h"
#include "gen3_save.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } } while (0)

static uint8_t g_sb2[G3_SECTOR_DATA_SIZE];
static uint8_t g_buf[G3_SAVE_FILE_SIZE];

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void     wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

int main(int argc, char** argv) {
  /* ---- 1. lane offsets: the exact decomp table, and lvlMode is the inner index ---- */
  {
    static const struct { int fac, modes; uint16_t cur, rec; } K[G3F_FACILITIES] = {
      { G3F_TOWER,   4, 0xCE0, 0xCF0 }, { G3F_DOME,    2, 0xD0C, 0xD14 },
      { G3F_PALACE,  2, 0xDC8, 0xDD0 }, { G3F_ARENA,   1, 0xDDA, 0xDDE },
      { G3F_FACTORY, 2, 0xDE2, 0xDEA }, { G3F_PIKE,    1, 0xE04, 0xE08 },
      { G3F_PYRAMID, 1, 0xE1A, 0xE1E },
    };
    for (int i = 0; i < G3F_FACILITIES; i++) {
      CHECK(g3f_modes(K[i].fac) == K[i].modes, "mode count matches the facility");
      CHECK(g3f_lane_off(K[i].fac, 0, G3F_LVL_50, G3F_CURRENT) == K[i].cur, "CURRENT base offset");
      CHECK(g3f_lane_off(K[i].fac, 0, G3F_LVL_50, G3F_RECORD)  == K[i].rec, "RECORD base offset");
      /* lvlMode inner => Open is +2 from Lv50, and mode stride is 4 bytes. */
      CHECK(g3f_lane_off(K[i].fac, 0, G3F_LVL_OPEN, G3F_CURRENT) == K[i].cur + 2,
            "Open is +2 (lvlMode is the INNER index)");
      if (K[i].modes > 1)
        CHECK(g3f_lane_off(K[i].fac, 1, G3F_LVL_50, G3F_CURRENT) == K[i].cur + 4,
              "battle-mode stride is 4 bytes");
      /* RECORD array begins exactly where CURRENT ends. */
      CHECK(K[i].cur + K[i].modes * 4 == K[i].rec, "RECORD starts where CURRENT ends");
      /* out-of-range modes rejected */
      CHECK(g3f_lane_off(K[i].fac, K[i].modes, 0, G3F_CURRENT) == -1, "mode past the facility's count rejected");
    }
    /* The Factory RECORD array ends on factoryRentsCount — pret's comment says 0xDF6. */
    CHECK(0xDEA + 2 * 4 == G3F_FACTORY_RENTS_OFF, "factoryRentsCount is 0xDF2, not 0xDF6");
    CHECK(G3F_FACTORY_RENTS_OFF + 2 * 4 == G3F_FACTORY_REC_RENTS_OFF, "record rents follow at 0xDFA");
    CHECK(0xD14 + 2 * 4 == G3F_DOME_CHAMPS_OFF, "domeTotalChampionships follows the Dome record array");
    CHECK(0xE08 + 1 * 4 == G3F_PIKE_TOTALS_OFF, "pikeTotalStreaks follows the Pike record array");
    /* bad args */
    CHECK(g3f_lane_off(-1, 0, 0, G3F_CURRENT) == -1, "negative facility rejected");
    CHECK(g3f_lane_off(G3F_FACILITIES, 0, 0, G3F_CURRENT) == -1, "facility past the end rejected");
    CHECK(g3f_lane_off(G3F_TOWER, 0, 2, G3F_CURRENT) == -1, "lvl > 1 rejected");
    CHECK(g3f_lane_off(G3F_TOWER, 0, 0, 7) == -1, "bad kind rejected");
  }

  /* ---- 2. winStreakActiveFlags: every real lane owns a UNIQUE bit, 26 in total ---- */
  {
    int seen[32]; memset(seen, 0, sizeof seen);
    int n = 0;
    for (int f = 0; f < G3F_FACILITIES; f++)
      for (int m = 0; m < g3f_modes(f); m++)
        for (int l = 0; l < 2; l++) {
          int b = g3f_active_bit(f, m, l);
          CHECK(b >= 0 && b < 32, "every real lane has a bit in the u32");
          if (b >= 0 && b < 32) { CHECK(!seen[b], "no two lanes share an active bit"); seen[b] = 1; }
          n++;
        }
    CHECK(n == 26, "26 real streak lanes across the 7 facilities");
    /* Spot-check the non-obvious assignments: the doubles/multis lanes live at 14+. */
    CHECK(g3f_active_bit(G3F_TOWER,   G3F_SINGLES, G3F_LVL_50)   == 0,  "Tower singles Lv50 = bit 0");
    CHECK(g3f_active_bit(G3F_TOWER,   G3F_DOUBLES, G3F_LVL_50)   == 14, "Tower doubles Lv50 = bit 14 (NOT sequential)");
    CHECK(g3f_active_bit(G3F_TOWER,   G3F_LINK_MULTIS, G3F_LVL_OPEN) == 19, "Tower link-multis Open = bit 19");
    CHECK(g3f_active_bit(G3F_PYRAMID, G3F_SINGLES, G3F_LVL_OPEN) == 13, "Pyramid Open = bit 13");
    CHECK(g3f_active_bit(G3F_FACTORY, G3F_DOUBLES, G3F_LVL_OPEN) == 25, "Factory doubles Open = bit 25 (highest)");
    CHECK(g3f_active_bit(G3F_ARENA,   G3F_DOUBLES, 0) == -1, "Arena has no doubles lane");
  }

  /* ---- 3. the safe writer keeps streak and active bit in lockstep ---- */
  {
    memset(g_sb2, 0, sizeof g_sb2);
    int w = g3f_set_current(g_sb2, G3F_TOWER, G3F_SINGLES, G3F_LVL_50, 34, true);
    CHECK(w == 34, "set_current returns the written value");
    CHECK(rd16(g_sb2 + 0xCE0) == 34, "current written at the right offset");
    CHECK(g3f_active_get(g_sb2, G3F_TOWER, G3F_SINGLES, G3F_LVL_50), "active bit set for a nonzero streak");
    CHECK(rd16(g_sb2 + 0xCF0) == 34, "record raised to match");
    /* a lane's write must not touch its neighbours */
    CHECK(rd16(g_sb2 + 0xCE2) == 0 && rd16(g_sb2 + 0xCE4) == 0, "neighbouring lanes untouched");
    /* lowering must NOT lower the record */
    g3f_set_current(g_sb2, G3F_TOWER, G3F_SINGLES, G3F_LVL_50, 5, true);
    CHECK(rd16(g_sb2 + 0xCF0) == 34, "record is never lowered by a smaller current");
    /* zero clears the bit */
    g3f_set_current(g_sb2, G3F_TOWER, G3F_SINGLES, G3F_LVL_50, 0, true);
    CHECK(!g3f_active_get(g_sb2, G3F_TOWER, G3F_SINGLES, G3F_LVL_50), "active bit cleared at 0");
    /* raise_record = false leaves the record alone */
    g3f_set_current(g_sb2, G3F_DOME, G3F_SINGLES, G3F_LVL_50, 9, false);
    CHECK(rd16(g_sb2 + 0xD14) == 0, "raise_record=false leaves the record");
    /* caps */
    CHECK(g3f_streak_cap(G3F_PYRAMID) == 999, "Pyramid caps at 999");
    CHECK(g3f_streak_cap(G3F_TOWER) == 9999, "Tower caps at 9999");
    CHECK(g3f_set_current(g_sb2, G3F_PYRAMID, G3F_SINGLES, G3F_LVL_50, 5000, false) == 999, "Pyramid clamped");
    CHECK(g3f_set_current(g_sb2, G3F_TOWER, G3F_SINGLES, G3F_LVL_50, 99999, false) == 9999, "Tower clamped");
    CHECK(g3f_set_current(g_sb2, G3F_TOWER, G3F_SINGLES, G3F_LVL_50, -7, false) == 0, "negative clamped to 0");
  }

  /* ---- 4. Frontier Brain targets: threshold - modifier, and the TIER IS CHOSEN BY
   * THE SAVE'S SYMBOL COUNT, not by the caller. GetFrontierBrainStatus indexes
   * sFrontierBrainStreakAppearances[facility][symbolsCount] and compares with ==,
   * so asking for Gold while holding 0 symbols writes a number the game never
   * tests (and jumps clean over the Silver threshold). ---- */
  {
    /* 0 symbols -> the game only ever tests the Silver row */
    CHECK(g3f_brain_tier(G3F_TOWER, 0) == 0, "0 symbols -> Silver tier");
    CHECK(g3f_brain_tier(G3F_TOWER, 1) == 1, "1 symbol  -> Gold tier");
    CHECK(g3f_brain_tier(G3F_TOWER, 2) == 0, "2 symbols -> the repeat path accepts Silver's row");
    CHECK(g3f_brain_target(G3F_TOWER,   0) == 34, "Tower Silver: store 34 (35 - 1)");
    CHECK(g3f_brain_target(G3F_TOWER,   1) == 69, "Tower Gold: store 69 (70 - 1)");
    CHECK(g3f_brain_target(G3F_DOME,    0) == 4,  "Dome Silver: store 4 (modifier 0)");
    CHECK(g3f_brain_target(G3F_DOME,    1) == 9,  "Dome Gold: store 9");
    CHECK(g3f_brain_target(G3F_PALACE,  0) == 20, "Palace Silver: store 20");
    CHECK(g3f_brain_target(G3F_ARENA,   0) == 27, "Arena Silver: store 27");
    CHECK(g3f_brain_target(G3F_FACTORY, 0) == 20, "Factory Silver: store 20");
    CHECK(g3f_brain_target(G3F_PIKE,    1) == 139, "Pike Gold: store 139 (140 - 1)");
    CHECK(g3f_brain_target(G3F_PYRAMID, 0) == 21, "Pyramid Silver: store 21 (modifier 0)");
    CHECK(g3f_brain_target(G3F_PYRAMID, 1) == 70, "Pyramid Gold: store 70");
    CHECK(g3f_brain_target(-1, 0) == -1, "bad facility rejected");
    CHECK(g3f_brain_tier(-1, 0) == -1, "bad facility rejected (tier)");
    /* every Brain target must be storable in its facility's cap — Pike Gold is 139
     * and Pyramid caps at 999, so this is not vacuous */
    for (int f = 0; f < G3F_FACILITIES; f++)
      for (int sym = 0; sym <= 2; sym++)
        CHECK(g3f_brain_target(f, sym) <= g3f_streak_cap(f), "Brain target fits the facility cap");
  }

  /* ---- 5. nothing writes past the SaveBlock2 tail ---- */
  {
    memset(g_sb2, 0, sizeof g_sb2);
    for (int f = 0; f < G3F_FACILITIES; f++)
      for (int m = 0; m < g3f_modes(f); m++)
        for (int l = 0; l < 2; l++)
          for (int k = 0; k <= 1; k++) {
            int off = g3f_lane_off(f, m, l, k);
            CHECK(off >= 0 && off + 2 <= G3F_SB2_LIMIT, "every lane lies inside the SB2 section");
          }
    CHECK(G3F_BATTLE_POINTS_OFF + 2 <= G3F_SB2_LIMIT, "BP offset inside the section");
    CHECK(G3F_CARD_BP_OFF + 2 <= G3F_SB2_LIMIT, "card BP offset inside the section");
    /* an out-of-range write is refused rather than smearing the tail */
    g3f_u16_set(g_sb2, G3F_SB2_LIMIT - 1, 0x1234, 0);
    CHECK(g_sb2[G3F_SB2_LIMIT - 1] == 0, "a u16 straddling the limit is refused");
    CHECK(g3f_streak_set(g_sb2, G3F_TOWER, 0, 0, 7, 1) == -1, "bad kind refuses to write");
  }

  /* ---- 6. challenge state ---- */
  {
    memset(g_sb2, 0, sizeof g_sb2);
    CHECK(!g3f_challenge_active(g_sb2), "idle save: no challenge active");
    g_sb2[G3F_CHALLENGE_STATUS_OFF] = 2;                 /* PAUSED */
    CHECK(g3f_challenge_active(g_sb2), "challengeStatus != 0 counts as active");
    g_sb2[G3F_CHALLENGE_STATUS_OFF] = 0;
    g_sb2[G3F_BITFIELD_OFF] = 0x04;                      /* challengePaused bit */
    CHECK(g3f_challenge_active(g_sb2), "challengePaused bit counts as active");
    g_sb2[G3F_BITFIELD_OFF] = 0x01;                      /* lvlMode = 1, not paused */
    CHECK(!g3f_challenge_active(g_sb2), "lvlMode bits alone are not 'active'");
    CHECK(g3f_lvl_mode(g_sb2) == 1, "lvlMode read from bits 0-1");
    wr16(g_sb2 + G3F_CUR_BATTLE_NUM_OFF, 3);
    CHECK(g3f_cur_battle_num(g_sb2) == 3, "curChallengeBattleNum read");
  }

  /* ---- 7. Ruby/Sapphire: 0x572 is derived and must be refreshed ---- */
  {
    memset(g_sb2, 0, sizeof g_sb2);
    CHECK(g3f_rs_set_record(g_sb2, 0, 7) == 7, "RS: record[Lv50] written");
    CHECK(rd16(g_sb2 + 0x560) == 7, "RS: recordWinStreaks[0] at 0x560");
    CHECK(rd16(g_sb2 + 0x572) == 7, "RS: derived best refreshed to 7");
    CHECK(g3f_rs_set_record(g_sb2, 1, 42) == 42, "RS: record[Open] written");
    CHECK(rd16(g_sb2 + 0x562) == 42, "RS: recordWinStreaks[1] at 0x562");
    CHECK(rd16(g_sb2 + 0x572) == 42, "RS: derived best = max(record[0], record[1])");
    /* lowering the higher lane must drop the derived cache too */
    g3f_rs_set_record(g_sb2, 1, 3);
    CHECK(rd16(g_sb2 + 0x572) == 7, "RS: derived best recomputed downward");
    CHECK(g3f_rs_record(g_sb2, 0) == 7 && g3f_rs_record(g_sb2, 1) == 3, "RS: both lanes read back");
    CHECK(g3f_rs_record(g_sb2, 2) == -1, "RS: bad lvl rejected");
  }

  /* ---- 8. per-game support ---- */
  CHECK(g3f_supported(PK_EMERALD), "Emerald supported");
  CHECK(g3f_supported(PK_RS), "Ruby/Sapphire supported");
  CHECK(!g3f_supported(PK_FRLG), "FRLG has no streak block");

  /* ---- 9. a REAL save, if one is available ---- */
  {
    const char* path = (argc > 1) ? argv[1] : "tests/fixtures/POKEMON_EMER_BPEE00.sav";
    FILE* f = fopen(path, "rb");
    if (!f) {
      printf("(skipped real-save checks: %s not found)\n", path);
    } else {
      uint32_t n = (uint32_t)fread(g_buf, 1, sizeof g_buf, f);
      fclose(f);
      Gen3SaveInfo info;
      int s0 = -1;
      if (n >= G3_SLOT_BYTES && gen3_parse(g_buf, n, &info) && info.valid &&
          (s0 = gen3_find_section(g_buf, info.slot, G3_SID_SAVEBLOCK2)) >= 0) {
        const uint8_t* sb2 =
            g_buf + (uint32_t)info.slot * G3_SLOT_BYTES + (uint32_t)s0 * G3_SECTOR_SIZE;
        {
          printf("save \"%s\": frontier BP=%d cardBP=%d challengeStatus=%d\n",
                 info.trainer_name, g3f_u16_get(sb2, G3F_BATTLE_POINTS_OFF),
                 g3f_u16_get(sb2, G3F_CARD_BP_OFF), g3f_challenge_status(sb2));
          for (int fc = 0; fc < G3F_FACILITIES; fc++) {
            printf("  %-15s", g3f_facility_name(fc));
            for (int m = 0; m < g3f_modes(fc); m++)
              for (int l = 0; l < 2; l++) {
                int cur = g3f_streak_get(sb2, fc, m, l, G3F_CURRENT);
                int rec = g3f_streak_get(sb2, fc, m, l, G3F_RECORD);
                printf(" [%s%s cur=%d rec=%d%s]", g3f_mode_name(fc, m),
                       l ? "/O" : "/50", cur, rec,
                       g3f_active_get(sb2, fc, m, l) ? " ACT" : "");
                CHECK(cur >= 0 && cur <= 0xFFFF, "current streak reads back");
                CHECK(rec >= 0 && rec <= 0xFFFF, "record streak reads back");
              }
            printf("\n");
          }
          /* The fixture is known to hold Factory singles Lv50 = 7 with its active bit
           * set — the ground truth that proved the offset table and the bit map. */
          if (g3f_streak_get(sb2, G3F_FACTORY, G3F_SINGLES, G3F_LVL_50, G3F_CURRENT) == 7) {
            CHECK(g3f_active_get(sb2, G3F_FACTORY, G3F_SINGLES, G3F_LVL_50),
                  "fixture: Factory singles Lv50 streak of 7 has its active bit set");
            CHECK(g3f_streak_get(sb2, G3F_FACTORY, G3F_SINGLES, G3F_LVL_50, G3F_RECORD) >= 7,
                  "fixture: Factory record >= current");
          }
        }
      } else {
        printf("(skipped real-save checks: %s did not parse)\n", path);
      }
    }
  }

  if (g_fail == 0) printf("OK: host_frontier_test (0 failures)\n");
  else             printf("host_frontier_test: %d FAILURE(S)\n", g_fail);
  return g_fail ? 1 : 0;
}
