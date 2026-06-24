/* Gen-3 Pokéblock case read/edit (pure C). See gen3_pokeblock.h for the layout. */
#include "gen3_pokeblock.h"
#include <string.h>

uint32_t pk_pokeblock_offset(PkGame g) {
  switch (g) {
    case PK_EMERALD: return 0x848;
    case PK_RS:      return 0x7F8;
    default:         return 0;        /* FRLG: no contests -> no pokéblock case */
  }
}

static uint8_t* slot_ptr(uint8_t* sb1, PkGame g, int i) {
  uint32_t base = pk_pokeblock_offset(g);
  if (!base || i < 0 || i >= PK_POKEBLOCK_COUNT) return 0;
  return sb1 + base + (uint32_t)i * PK_POKEBLOCK_STRIDE;
}

bool pk_pokeblock_get(const uint8_t* sb1, PkGame g, int i, PkPokeblock* out) {
  const uint8_t* p = slot_ptr((uint8_t*)sb1, g, i);
  if (!p) return false;
  out->color = p[0]; out->spicy = p[1]; out->dry = p[2]; out->sweet = p[3];
  out->bitter = p[4]; out->sour = p[5]; out->feel = p[6];
  return true;
}

void pk_pokeblock_set(uint8_t* sb1, PkGame g, int i, const PkPokeblock* in) {
  uint8_t* p = slot_ptr(sb1, g, i);
  if (!p) return;
  p[0] = in->color; p[1] = in->spicy; p[2] = in->dry; p[3] = in->sweet;
  p[4] = in->bitter; p[5] = in->sour; p[6] = in->feel;   /* p[7] (pad) untouched */
}

void pk_pokeblock_clear(uint8_t* sb1, PkGame g, int i) {
  uint8_t* p = slot_ptr(sb1, g, i);
  if (p) memset(p, 0, PK_POKEBLOCK_STRIDE);
}

bool pk_pokeblock_occupied(const PkPokeblock* p) { return p->color != 0; }

const char* pk_pokeblock_color_name(uint8_t c) {
  static const char* const N[] = {
    "None", "Red", "Blue", "Pink", "Green", "Yellow", "Purple", "Indigo",
    "Brown", "LiteBlue", "Olive", "Gray", "Black", "White", "Gold",
  };
  return (c < (sizeof N / sizeof N[0])) ? N[c] : "?";
}
