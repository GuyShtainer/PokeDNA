#include "xfer_rec.h"

/* Same constants gb_sidecar.c's gbsc_key() uses (source/gb_sidecar.h:68-70). */
uint64_t xr_key_g3(const uint8_t rec80[80]) {
  uint64_t h = 14695981039346656037ULL;   /* FNV-1a-64 offset basis */
  for (int i = 0; i < 8; i++) {
    h ^= (uint64_t)rec80[i];
    h *= 1099511628211ULL;                /* FNV-1a-64 prime */
  }
  return h;
}
