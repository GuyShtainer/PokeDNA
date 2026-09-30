/* SPDX-License-Identifier: GPL-3.0-or-later
 * The golden pin for jrn_crc32_update (BACKLOG #302). The journal's STORED CRC32 values (segment headers,
 * record trailers, region hashes) are a frozen on-card format: old journals must keep verifying, so every
 * rewrite of the CRC loop must reproduce the EXACT values the original byte-at-a-time nibble-table code
 * produced. The constants below were computed with that original code BEFORE the loop was touched, over a
 * fixed xorshift32 buffer, at four alignments, at lengths that straddle every unrolling boundary, chained
 * with uneven splits, with a non-zero seed, and over the all-zero / all-FF region shapes. A second,
 * independent check compares against a plain bitwise reflected CRC-32 (0xEDB88320) on many lengths.
 *
 *   cc -std=c11 -I source tests/host_jrn_crc_pin_test.c source/journal.c -o /tmp/hjcrc
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "journal.h"

static uint32_t st = 0x2545F491u;
static uint8_t rnd(void) { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return (uint8_t)(st >> 11); }

static uint32_t ref_crc(uint32_t crc, const uint8_t* p, uint32_t n) {
  uint32_t i, k;
  crc = ~crc;
  for (i = 0; i < n; i++) {
    crc ^= p[i];
    for (k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

typedef struct { unsigned off, len; uint32_t crc; } Pin;
static const Pin k_pins[] = {
  { 0, 0, 0x00000000u },
  { 0, 1, 0xCB6ED9FCu },
  { 0, 2, 0xF7A653F4u },
  { 0, 3, 0xA79FEA42u },
  { 0, 4, 0xE4A63A91u },
  { 0, 5, 0x00EBCC4Au },
  { 0, 7, 0x70614A58u },
  { 0, 8, 0xF81DA553u },
  { 0, 9, 0x1EF685BDu },
  { 0, 15, 0x396027FCu },
  { 0, 16, 0xA7512C36u },
  { 0, 17, 0x84167A82u },
  { 0, 31, 0xB41C49C5u },
  { 0, 32, 0xED03F519u },
  { 0, 33, 0xCA58AB0Fu },
  { 0, 63, 0xC91C63F9u },
  { 0, 64, 0x09703A38u },
  { 0, 65, 0x5CB5704Eu },
  { 0, 127, 0x5EB5C875u },
  { 0, 128, 0x8534EF60u },
  { 0, 129, 0xB3EC24B1u },
  { 0, 255, 0x442F4561u },
  { 0, 256, 0xF9F29A5Au },
  { 0, 257, 0xA5FC2D81u },
  { 0, 1000, 0x53A6BC8Bu },
  { 0, 3964, 0x075F2F51u },
  { 0, 3968, 0x6C6BA8B4u },
  { 1, 0, 0x00000000u },
  { 1, 1, 0x4366831Au },
  { 1, 2, 0xE099E5EDu },
  { 1, 3, 0xA28882E7u },
  { 1, 4, 0xB9CB36FBu },
  { 1, 5, 0xB80B0449u },
  { 1, 7, 0xE9BC1832u },
  { 1, 8, 0x245275CEu },
  { 1, 9, 0x3F9E3ED8u },
  { 1, 15, 0x7572DE0Au },
  { 1, 16, 0xABAB25F7u },
  { 1, 17, 0x0612744Au },
  { 1, 31, 0x23DD029Au },
  { 1, 32, 0xBE27A762u },
  { 1, 33, 0x08DC70FAu },
  { 1, 63, 0xB63D2365u },
  { 1, 64, 0x49D0101Eu },
  { 1, 65, 0xDEFD24A5u },
  { 1, 127, 0x8522D669u },
  { 1, 128, 0xCA308A2Cu },
  { 1, 129, 0xCA7F9870u },
  { 1, 255, 0x58072AB7u },
  { 1, 256, 0x7BE646F4u },
  { 1, 257, 0x6C7219DBu },
  { 1, 1000, 0x23ABBEA5u },
  { 1, 3964, 0xB96520F5u },
  { 1, 3968, 0x8B4BCEA9u },
  { 2, 0, 0x00000000u },
  { 2, 1, 0x1DB87A14u },
  { 2, 2, 0x6614E9C0u },
  { 2, 3, 0x1C051FFBu },
  { 2, 4, 0xB8AECA60u },
  { 2, 5, 0x92654A21u },
  { 2, 7, 0xE00BA506u },
  { 2, 8, 0xAAE52D8Au },
  { 2, 9, 0xCA1F4DD7u },
  { 2, 15, 0x9E3F6D14u },
  { 2, 16, 0x3F2453C0u },
  { 2, 17, 0x10EA3479u },
  { 2, 31, 0x111AC6B0u },
  { 2, 32, 0x60AEFE63u },
  { 2, 33, 0x03D92682u },
  { 2, 63, 0xF608EBA9u },
  { 2, 64, 0x8B47DA71u },
  { 2, 65, 0xE18AB5B2u },
  { 2, 127, 0xDD9FDF53u },
  { 2, 128, 0x0AD25B88u },
  { 2, 129, 0xA6B9EF4Cu },
  { 2, 255, 0x0322FDC9u },
  { 2, 256, 0x34629171u },
  { 2, 257, 0x2E8F0560u },
  { 2, 1000, 0xF23677A9u },
  { 2, 3964, 0xCFF014BAu },
  { 2, 3968, 0x779E8FC5u },
  { 3, 0, 0x00000000u },
  { 3, 1, 0x7CD385C7u },
  { 3, 2, 0x827B4D34u },
  { 3, 3, 0xB3EB6B13u },
  { 3, 4, 0x5B622F06u },
  { 3, 5, 0x74E5DAC5u },
  { 3, 7, 0x11097767u },
  { 3, 8, 0x141F3F48u },
  { 3, 9, 0x23C212F0u },
  { 3, 15, 0xA6C53775u },
  { 3, 16, 0xAB78921Eu },
  { 3, 17, 0x047F34F7u },
  { 3, 31, 0x7E09A37Au },
  { 3, 32, 0x67AC291Fu },
  { 3, 33, 0x97BA1FD1u },
  { 3, 63, 0xFBFD9839u },
  { 3, 64, 0x99FDC652u },
  { 3, 65, 0x5925AAA1u },
  { 3, 127, 0x504333B2u },
  { 3, 128, 0x60EFA796u },
  { 3, 129, 0xFD0D7CADu },
  { 3, 255, 0xFB3D4071u },
  { 3, 256, 0x2E405AB1u },
  { 3, 257, 0x754B89B6u },
  { 3, 1000, 0xD827C09Bu },
  { 3, 3964, 0xDFFB8DAAu },
  { 3, 3968, 0x78B7E65Au },
};
static const uint32_t k_chain[8] = { 0x4366831AU, 0xB9CB36FBU, 0x245275CEU, 0xAF32E9F2U, 0x522F3394U, 0xBA39ACB0U, 0xA6DAC435U, 0x8B4BCEA9U };

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
  static uint8_t buf[4096 + 8], z[3968];
  unsigned i, p = 0, n;
  uint32_t c = 0;
  static const unsigned cuts[8] = { 1, 3, 4, 5, 60, 129, 700, 3968u - (1 + 3 + 4 + 5 + 60 + 129 + 700) };
  for (i = 0; i < sizeof buf; i++) buf[i] = rnd();
  CHECK(jrn_crc32_update(0, "123456789", 9) == 0xCBF43926u, "check value");
  for (i = 0; i < sizeof k_pins / sizeof k_pins[0]; i++) {
    uint32_t got = jrn_crc32_update(0, buf + k_pins[i].off, k_pins[i].len);
    CHECK(got == k_pins[i].crc, "pin off=%u len=%u: got %08X want %08X", k_pins[i].off, k_pins[i].len, got, k_pins[i].crc);
  }
  for (i = 0; i < 8; i++) {
    c = jrn_crc32_update(c, buf + 1 + p, cuts[i]);
    p += cuts[i];
    CHECK(c == k_chain[i], "chain %u: got %08X want %08X", i, c, k_chain[i]);
  }
  CHECK(jrn_crc32_update(0xDEADBEEFu, buf, 3968) == 0x690B5E41u, "seeded 3968");
  CHECK(jrn_crc32_update(0, z, 3968) == 0xF12D3CD7u, "all-zero region");
  memset(z, 0xFF, sizeof z);
  CHECK(jrn_crc32_update(0, z, 3968) == 0x7EAC710Au, "all-FF region");
  CHECK(jrn_crc32_update(0x12345678u, 0, 0) == 0x12345678u, "empty update is the identity");
  for (n = 0; n < 300; n++) {                              /* every short length, every alignment, vs the bitwise oracle */
    unsigned off;
    for (off = 0; off < 4; off++)
      CHECK(jrn_crc32_update(0xA5A5A5A5u, buf + off, n) == ref_crc(0xA5A5A5A5u, buf + off, n), "oracle off=%u len=%u", off, n);
  }
  CHECK(jrn_crc32_update(0, buf, 4096) == ref_crc(0, buf, 4096), "oracle 4096");
  if (fails) { printf("host_jrn_crc_pin_test: %d FAILED\n", fails); return 1; }
  printf("host_jrn_crc_pin_test: all checks passed (%u pins + chain + oracle)\n", (unsigned)(sizeof k_pins / sizeof k_pins[0]));
  return 0;
}
