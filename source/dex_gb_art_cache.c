/*
 * dex_gb_art_cache.c -- see the header for the full contract. Deliberately tiny:
 * this is the whole cache policy, not a summary of a bigger one hiding in the
 * caller.
 */
#include <string.h>

#include "dex_gb_art_cache.h"

void dex_gb_art_cache_open(DexGbArtCache* c, DexGbArtSlot* slot, int cap) {
  if (!c) return;
  c->slot = (slot && cap > 0) ? slot : 0;
  c->cap  = c->slot ? cap : 0;
  dex_gb_art_cache_clear(c);
}

void dex_gb_art_cache_clear(DexGbArtCache* c) {
  if (!c) return;
  for (int i = 0; i < c->cap; i++) c->slot[i].dex = 0;
  c->next = 0;
}

const uint16_t* dex_gb_art_cache_find(const DexGbArtCache* c, uint16_t dex) {
  if (!c || !c->slot || dex == 0) return 0;
  for (int i = 0; i < c->cap; i++)
    if (c->slot[i].dex == dex) return c->slot[i].px;
  return 0;
}

void dex_gb_art_cache_put(DexGbArtCache* c, uint16_t dex, const uint16_t px[DEX_GB_ART_CELL_PX]) {
  if (!c || !c->slot || !px || dex == 0 || c->cap <= 0) return;
  if (c->next < 0 || c->next >= c->cap) c->next = 0;   /* belt: never trust a caller-
                                                        * corruptible cursor blindly */
  DexGbArtSlot* s = &c->slot[c->next];
  c->next = (c->next + 1) % c->cap;
  s->dex = dex;
  memcpy(s->px, px, sizeof s->px);
}
