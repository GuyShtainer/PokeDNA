#include "gb_reconcile.h"
#include "gen3_clip.h"     /* pk_box_slot, pk_party_slot, party_count -- pure C too */
#include "gen3_box.h"      /* G3_TOTAL_BOXES, G3_IN_BOX                             */
#include <string.h>

static bool id8_is_zero(const uint8_t id8[8]) {
  for (int i = 0; i < 8; i++)
    if (id8[i]) return false;
  return true;
}

int gb_reconcile_match(const uint8_t* sb1, bool frlg, const uint8_t* pc,
                       const uint8_t id8[8], int* where_box, int* where_slot) {
  if (where_box) *where_box = -1;
  if (where_slot) *where_slot = -1;
  if (!sb1 || !id8 || id8_is_zero(id8)) return 0;

  int count = 0, fbox = -1, fslot = -1;

  int nparty = party_count(sb1, frlg);
  for (int i = 0; i < nparty; i++) {
    const uint8_t* rec = pk_party_slot((uint8_t*)sb1, frlg, i);
    if (memcmp(rec, id8, 8) == 0) {
      count++;
      if (count == 1) { fbox = -1; fslot = i; }
    }
  }

  if (pc) {
    for (int b = 0; b < G3_TOTAL_BOXES; b++) {
      for (int s = 0; s < G3_IN_BOX; s++) {
        const uint8_t* rec = pk_box_slot((uint8_t*)pc, b, s);
        if (memcmp(rec, id8, 8) == 0) {
          count++;
          if (count == 1) { fbox = b; fslot = s; }
        }
      }
    }
  }

  if (count == 1) {
    if (where_box) *where_box = fbox;
    if (where_slot) *where_slot = fslot;
  }
  return count;
}
