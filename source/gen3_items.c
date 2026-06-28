#include "gen3_items.h"

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static void     wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* per-game {SaveBlock1 offset, capacity} for the 5 pockets */
static const struct { uint16_t off; uint16_t cap; } POCKETS[3][POCKET_COUNT] = {
  /* PK_RS      */ { {0x0560,20}, {0x05B0,20}, {0x0600,16}, {0x0640,64}, {0x0740,46} },
  /* PK_EMERALD */ { {0x0560,30}, {0x05D8,30}, {0x0650,16}, {0x0690,64}, {0x0790,46} },
  /* PK_FRLG    */ { {0x0310,42}, {0x03B8,30}, {0x0430,13}, {0x0464,58}, {0x054C,43} },
};

static uint16_t qty_key(const uint8_t* sb2, PkGame g) {
  if (g == PK_EMERALD) return (uint16_t)rd32(sb2 + 0x00AC);
  if (g == PK_FRLG)    return (uint16_t)rd32(sb2 + 0x0F20);
  return 0;                                            /* Ruby/Sapphire: plaintext */
}

const char* pk_pocket_name(int p) {
  switch (p) {
    case POCKET_ITEMS:   return "Items";
    case POCKET_KEY:     return "Key Items";
    case POCKET_BALLS:   return "Poke Balls";
    case POCKET_TMHM:    return "TMs & HMs";
    case POCKET_BERRIES: return "Berries";
    default:             return "?";
  }
}

/* An item's home pocket, by id range (shared across RS/E/FRLG for ids 1..348; the
 * 349+ range is the later key items — tickets, etc.). Mirrors the decomp item table's
 * .pocket field, which is contiguous in id order. */
int pk_item_pocket(uint16_t id) {
  if (id == 0) return POCKET_ITEMS;
  if (id <= 12) return POCKET_BALLS;                 /* 1..12  Master..Premier Ball     */
  if (id >= 133 && id <= 175) return POCKET_BERRIES; /* 133..175 berries                */
  if (id >= 289 && id <= 346) return POCKET_TMHM;    /* 289..346 TM01..HM08             */
  if ((id >= 259 && id <= 288) || id >= 349) return POCKET_KEY;  /* key items (bikes, tickets, ...) */
  return POCKET_ITEMS;
}

int pk_pocket_cap(PkGame g, int p) {
  if (p < 0 || p >= POCKET_COUNT) return 0;
  return POCKETS[g][p].cap;
}

static const uint8_t* slot_ptr_c(const uint8_t* sb1, PkGame g, int p, int slot) {
  return sb1 + POCKETS[g][p].off + (uint32_t)slot * 4;
}

uint16_t pk_bag_item(const uint8_t* sb1, PkGame g, int p, int slot) {
  if (p < 0 || p >= POCKET_COUNT || slot < 0 || slot >= POCKETS[g][p].cap) return 0;
  return rd16(slot_ptr_c(sb1, g, p, slot));
}

uint16_t pk_bag_qty(const uint8_t* sb1, const uint8_t* sb2, PkGame g, int p, int slot) {
  if (p < 0 || p >= POCKET_COUNT || slot < 0 || slot >= POCKETS[g][p].cap) return 0;
  return (uint16_t)(rd16(slot_ptr_c(sb1, g, p, slot) + 2) ^ qty_key(sb2, g));
}

void pk_bag_set(uint8_t* sb1, const uint8_t* sb2, PkGame g, int p, int slot,
                uint16_t item_id, uint16_t qty) {
  if (p < 0 || p >= POCKET_COUNT || slot < 0 || slot >= POCKETS[g][p].cap) return;
  uint8_t* s = sb1 + POCKETS[g][p].off + (uint32_t)slot * 4;
  wr16(s, item_id);
  wr16(s + 2, (uint16_t)(qty ^ qty_key(sb2, g)));
}
