/*
 * dex_gbart_cache.c -- see dex_gbart_cache.h for the whole rationale. PURE C, no
 * tonc, no FatFs -- tests/host_dexgbartcache_test.c runs this exact code on the PC.
 */
#include "dex_gbart_cache.h"

void dexcache_reset(DexArtCache* c, DexArtSlot* slots, int n_slots) {
  if (!c) return;
  c->slots = (slots && n_slots > 0) ? slots : 0;
  c->n_slots = c->slots ? n_slots : 0;
  c->head = 0;
  c->count = 0;
}

uint16_t dexcache_key(uint8_t gen, uint16_t dex) {
  return (uint16_t)(((uint16_t)(gen & 3u) << 9) | (dex & 0x1FFu));
}

int dexcache_find(const DexArtCache* c, uint16_t key) {
  if (!c || !c->slots) return -1;
  for (int i = 0; i < c->count; i++)
    if (c->slots[i].key == key) return i;
  return -1;
}

int dexcache_claim(DexArtCache* c, uint16_t key) {
  if (!c || !c->slots || c->n_slots <= 0) return -1;
  int slot = dexcache_find(c, key);
  if (slot >= 0) return slot;
  slot = c->head;
  c->head = (c->head + 1) % c->n_slots;
  if (c->count < c->n_slots) c->count++;
  c->slots[slot].key = key;
  return slot;
}
