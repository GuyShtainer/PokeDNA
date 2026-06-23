/* Host test for the RTC clock check & fix math in gen3_save.c (pure C, no cart).
 *   cc -std=c11 -I source tests/host_clock_test.c source/gen3_save.c -o /tmp/hc && /tmp/hc
 *
 * Validates the #1 corruption risk flagged in research: the day-count convention
 * (game ConvertDateToDayCount == gen3_rtc_days + 1) and the offset/anchor round-trip,
 * so a fixed save's derived in-game time matches what was requested. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_save.h"

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* Reference replica of the game's ConvertDateToDayCount (2-digit year, +1 style). */
static int ref_game_daycount(int y4, int mo, int d) {
  static const int md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  int y2 = y4 - 2000, days = 0;
  for (int i = y2 - 1; i >= 0; i--)
    days += 365 + ((i % 4 == 0 && (i % 100 != 0 || i % 400 == 0)) ? 1 : 0);
  for (int m = 1; m < mo; m++) days += md[m - 1];
  if (mo > 2 && (y2 % 4 == 0 && (y2 % 100 != 0 || y2 % 400 == 0))) days += 1;
  return days + d;   /* 1-based day, no -1 */
}

static long long total_secs(int days, int h, int m, int s) {
  return (long long)days * 86400 + (long long)h * 3600 + m * 60 + s;
}

int main(void) {
  /* 1) date <-> day-count round-trip across the whole supported range. */
  for (int y = 2000; y <= 2099; y++)
    for (int mo = 1; mo <= 12; mo++)
      for (int d = 1; d <= 28; d++) {            /* 28 keeps every month valid */
        int days = gen3_rtc_days(y, mo, d);
        int ry, rm, rd; gen3_days_to_date(days, &ry, &rm, &rd);
        CHECK(ry == y && rm == mo && rd == d, "roundtrip %04d-%02d-%02d -> %04d-%02d-%02d", y, mo, d, ry, rm, rd);
        /* game convention is exactly +1 vs the repo epoch-0 count */
        CHECK(ref_game_daycount(y, mo, d) == days + 1, "convention %04d-%02d-%02d: game=%d repo+1=%d",
              y, mo, d, ref_game_daycount(y, mo, d), days + 1);
      }
  /* leap-day edge: 2024-02-29 must round-trip */
  { int days = gen3_rtc_days(2024, 2, 29); int ry, rm, rd; gen3_days_to_date(days, &ry, &rm, &rd);
    CHECK(ry == 2024 && rm == 2 && rd == 29, "leap 2024-02-29 -> %04d-%02d-%02d", ry, rm, rd); }

  static uint8_t sb2[G3_SECTOR_DATA_SIZE];

  /* 2) AUTO-SYNC: in-game clock should equal the live cart RTC; delta 0, verdict green. */
  {
    memset(sb2, 0, sizeof sb2);
    int Y=2026, Mo=6, D=22, H=15, Mi=30, S=10;
    gen3_clock_autosync(sb2, Y, Mo, D, H, Mi, S);
    Gen3ClockInfo ci; gen3_clock_read(sb2, gen3_rtc_days(Y, Mo, D), H*3600+Mi*60+S, 1, &ci);
    CHECK(ci.off_days == 0 && ci.off_h == 0 && ci.off_m == 0 && ci.off_s == 0, "autosync offset not zero: %dd %d:%d:%d", ci.off_days, ci.off_h, ci.off_m, ci.off_s);
    CHECK(ci.ly == Y && ci.lm == Mo && ci.ld == D, "autosync derived date %04d-%02d-%02d", ci.ly, ci.lm, ci.ld);
    CHECK(ci.delta == 0, "autosync delta %d (want 0)", ci.delta);
    CHECK(ci.verdict == 0, "autosync verdict %d (want 0=green)", ci.verdict);
    /* game's view: gLocalTime.days = gameDayCount(live) - off.days must equal stored berry_days */
    CHECK(ref_game_daycount(Y, Mo, D) - ci.off_days == ci.berry_days, "autosync berry mismatch: %d vs %d",
          ref_game_daycount(Y, Mo, D) - ci.off_days, ci.berry_days);
  }

  /* 3) MANUAL: make the game believe `desired` is now, given a (wrong) live RTC. */
  {
    memset(sb2, 0, sizeof sb2);
    int lY=2000, lMo=1, lD=1, lH=0, lMi=0, lS=0;        /* battery-reset cart clock */
    int dY=2026, dMo=6, dD=22, dH=8, dMi=45, dS=0;       /* what the player wants "now" to be */
    gen3_clock_manual(sb2, lY,lMo,lD,lH,lMi,lS, dY,dMo,dD,dH,dMi,dS);
    Gen3ClockInfo ci; gen3_clock_read(sb2, gen3_rtc_days(lY, lMo, lD), lH*3600+lMi*60+lS, 1, &ci);
    CHECK(ci.ly == dY && ci.lm == dMo && ci.ld == dD, "manual derived date %04d-%02d-%02d (want %04d-%02d-%02d)",
          ci.ly, ci.lm, ci.ld, dY, dMo, dD);
    CHECK(ci.delta == 0, "manual delta %d (want 0)", ci.delta);
    /* time-of-day round-trip: liveTotal - offsetTotal == desiredTotal */
    long long off_total = total_secs(ci.off_days, ci.off_h, ci.off_m, ci.off_s);
    long long live_total = total_secs(gen3_rtc_days(lY,lMo,lD), lH, lMi, lS);
    long long des_total  = total_secs(gen3_rtc_days(dY,dMo,dD), dH, dMi, dS);
    CHECK(live_total - off_total == des_total, "manual time round-trip off by %lld s", (live_total - off_total) - des_total);
  }

  /* 4) FROZEN detection: cart RTC behind the save's last event -> verdict backwards(2). */
  {
    memset(sb2, 0, sizeof sb2);
    /* save last updated at game-day for 2026-06-22; cart RTC now reads 2026-06-10 (12 days behind) */
    gen3_clock_autosync(sb2, 2026, 6, 22, 12, 0, 0);    /* anchors lastBerry to 2026-06-22 */
    Gen3ClockInfo ci; gen3_clock_read(sb2, gen3_rtc_days(2026, 6, 10), 8*3600, 1, &ci);
    CHECK(ci.delta < 0, "frozen delta %d (want <0)", ci.delta);
    CHECK(ci.verdict == 2, "frozen verdict %d (want 2)", ci.verdict);
  }

  /* 4b) out-of-range refusal: a battery-dead cart reporting an implausible far-future
   * year must be refused (s16 day-count would wrap), writing nothing. */
  {
    memset(sb2, 0, sizeof sb2);
    CHECK(gen3_clock_autosync(sb2, 2026, 6, 22, 12, 0, 0) == 1, "valid autosync should succeed");
    memset(sb2, 0, sizeof sb2);
    CHECK(gen3_clock_autosync(sb2, 2099, 12, 31, 23, 59, 59) == 0, "far-future autosync should be refused");
    /* and nothing was written */
    CHECK(sb2[SB2_OFF_LAST_BERRY_UPDATE] == 0 && sb2[SB2_OFF_LAST_BERRY_UPDATE + 1] == 0, "refused write must not touch sb2");
  }

  /* 5) no-RTC -> verdict 4 */
  {
    memset(sb2, 0, sizeof sb2);
    Gen3ClockInfo ci; gen3_clock_read(sb2, 0, 0, 0, &ci);
    CHECK(ci.verdict == 4, "no-rtc verdict %d (want 4)", ci.verdict);
  }

  if (fails == 0) printf("host_clock_test: ALL PASS\n");
  else            printf("host_clock_test: %d FAILURE(S)\n", fails);
  return fails ? 1 : 0;
}
