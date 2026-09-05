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

/* ---- the release plan (S5-C review) -- see gb_reconcile.h for the full design
 * note, including the exact data-loss scenario this function exists to prevent. */
int gb_reconcile_plan(GbReconHit* hits, int n, uint8_t* sb1, bool frlg, uint8_t* pc) {
  if (!hits || !sb1 || n <= 0) return 0;
  if (n > GB_RECON_MAX_HITS) n = GB_RECON_MAX_HITS;    /* defensive clamp, golden rule 2 */

  for (int i = 0; i < n; i++) { hits[i].released = false; hits[i].done = false; }

  /* Dedupe: any hit sharing an EXACT (box, slot) with an earlier (lower-index) hit
   * is a duplicate of it, independent of whatever the caller may already have
   * marked (`!hits[k].duplicate` below always resolves a chain back to the FIRST
   * hit in a group, never a middle one). A duplicate is claimed but never touches
   * `sb1`/`pc` -- the earlier hit already released "the" mon. */
  for (int i = 1; i < n; i++)
    for (int k = 0; k < i; k++)
      if (!hits[k].duplicate && hits[k].box == hits[i].box && hits[k].slot == hits[i].slot) {
        hits[i].duplicate = true;
        break;
      }
  for (int i = 0; i < n; i++)
    if (hits[i].duplicate) { hits[i].released = true; hits[i].done = true; }

  /* PC hits: fixed addresses, array order is fine (no shift between releases).
   * Bounds-checked (S5-C 2nd review #3) BEFORE ever indexing `pc` -- a hit is
   * caller-supplied data (ultimately decoded off the SD card via a sidecar's
   * original80/gb_reconcile_match()), so an out-of-range box/slot must be refused
   * exactly like a live mismatch, not trusted into pk_box_slot()'s own pointer
   * arithmetic. Live-reverified immediately before touching anything; a mismatch
   * (or an out-of-range hit) means it is left unreleased and unclaimed (never a
   * bystander touched, and never a phantom "release" claimed either).
   *
   * `pc == NULL` (no PC storage this save): every box-hit is marked `done` here
   * too (S5-C 2nd review #4) -- nothing else in this function will ever visit a
   * box hit (the party loop below only considers box == -1), so leaving `done`
   * false for it would break the "always true when this function returns"
   * invariant gb_reconcile.h documents for every hit. */
  for (int i = 0; i < n; i++) {
    if (hits[i].done || hits[i].box < 0) continue;
    if (!pc || hits[i].box >= G3_TOTAL_BOXES || hits[i].slot < 0 || hits[i].slot >= G3_IN_BOX) {
      hits[i].done = true;
      continue;
    }
    if (memcmp(pk_box_slot(pc, hits[i].box, hits[i].slot), hits[i].id8, 8) != 0) {
      hits[i].done = true;
      continue;
    }
    clip_clear_box_slot(pc, hits[i].box, hits[i].slot);
    hits[i].released = true;
    hits[i].done = true;
  }

  /* Party hits: HIGHEST SLOT FIRST -- party_release() shifts every later index
   * down by one, so releasing low-to-high would silently release the WRONG
   * (shifted) mon at a later hit's recorded slot. Bounded by n+1 (golden rule 2):
   * each pass marks exactly one not-yet-done hit `done`, or finds none left. */
  for (int guard = 0; guard <= n; guard++) {
    int best = -1;
    for (int i = 0; i < n; i++) {
      if (hits[i].done || hits[i].box != -1) continue;
      if (best < 0 || hits[i].slot > hits[best].slot) best = i;
    }
    if (best < 0) break;

    GbReconHit* h = &hits[best];
    int pcnt = party_count(sb1, frlg);
    const uint8_t* cur = (h->slot >= 0 && h->slot < pcnt) ? pk_party_slot(sb1, frlg, h->slot) : NULL;
    if (!cur || memcmp(cur, h->id8, 8) != 0) {
      /* By construction the only way a party slot's content can change between
       * the walk and this call is an EARLIER hit in this SAME pass releasing it
       * (every other cause is excluded by the ordering above) -- so a mismatch
       * here means "already released, by a different entry", not "something
       * unexpected". Claim it: the mon really is gone from the save. */
      h->released = true;
      h->done = true;
      continue;
    }
    if (pcnt <= 1) { h->done = true; continue; }   /* never release the last party mon */
    party_release(sb1, frlg, h->slot);
    h->released = true;
    h->done = true;
  }

  int released = 0;
  for (int i = 0; i < n; i++) if (hits[i].released) released++;
  return released;
}
