/* SPDX-License-Identifier: GPL-3.0-or-later
 * img_stage.c -- see img_stage.h. Pure C. */
#include "img_stage.h"

#include <string.h>

#include "gen3_save.h"

#define IMG_NSEC 14u   /* the logical image: 14 sections, addressed by section ID (the journal's regions) */
#define IMG_PC_MASK ((uint16_t)(((1u << (G3_SID_PKMN_STORAGE_END + 1)) - 1u) & ~((1u << G3_SID_PKMN_STORAGE_START) - 1u)))

_Static_assert(IMG_NSEC <= JRN_NREG_MAX, "every section is a journal region");
_Static_assert(G3_SECTOR_DATA_SIZE <= 0xFFFFu, "a region size fits the journal's u16");

/* ---- the recorder ---------------------------------------------------------------------------------- */
static const uint8_t* sec_data(const uint8_t* save, int slot, int id) {
  int s = gen3_find_section(save, slot, id);
  return s < 0 ? 0 : save + (uint32_t)slot * G3_SLOT_BYTES + (uint32_t)s * G3_SECTOR_SIZE;
}

static const char* default_name(uint16_t mask) {
  if (mask == 1u) return "Trainer";
  if ((mask & ~IMG_PC_MASK) == 0) return "Box move";
  if ((mask & ~(uint16_t)0x1Eu) == 0) return "Save block 1";
  return "Edit";
}

/* One attempt: begin, one region per staged section (old = what g_save holds NOW, new = the block), end.
 * The engine aborts a half-built step on any region error, so a failure leaves nothing pending. */
static int rec_try(ImgRec* r, const char* name, int crossed, const uint8_t* save, int slot, uint16_t mask,
                   const uint8_t* const* blk) {
  int rc = jrn_step_begin(r->j, name, crossed), id;
  if (rc) return rc;
  for (id = 0; id < (int)IMG_NSEC; id++) {
    const uint8_t* old_blk;
    if (!((mask >> id) & 1u)) continue;
    old_blk = sec_data(save, slot, id);
    if (!old_blk) continue;                       /* a section this image lacks: nothing to record */
    rc = jrn_step_region(r->j, (uint8_t)id, old_blk, blk[id]);
    if (rc) return rc;
  }
  return jrn_step_end(r->j);
}

static void rec_lost(ImgRec* r, const char* what, int rc) {
  r->last_what = what; r->last_rc = rc;
  if (rc == JRN_E_STOPPED) { r->state = IREC_STOPPED; return; }
  r->lost++;
  r->epoch++;                       /* a lost step is a floor: the next record is crossed (#301) */
  if (r->state == IREC_OK) r->state = IREC_GAP;
}

/* Journal the difference as ONE step. Never fails the staging that follows: a step that cannot be
 * recorded is counted (state GAP) and the image is staged regardless -- the .sav write at exit is
 * the product, the journal is the safety net, and the UI must never claim what was not recorded. */
static void rec_step(ImgRec* r, const uint8_t* save, int slot, uint16_t mask, const uint8_t* const* blk) {
  const char* name;
  int crossed, rc = JRN_E_STATE, attempt;
  if (!r || !r->j || r->state == IREC_OFF) return;
  name = r->name ? r->name : default_name(mask);
  crossed = r->epoch != r->epoch_seen;
  for (attempt = 0; attempt < 3; attempt++) {
    rc = rec_try(r, name, crossed, save, slot, mask, blk);
    if (rc == JRN_OK) { r->epoch_seen = r->epoch; return; }
    if (rc == JRN_NOOP) return;                                  /* nothing changed: nothing to record */
    if (rc == JRN_E_FULL) { if (r->flush) (void)r->flush(); continue; }   /* flush, then re-stage in a clear buffer */
    if (rc == JRN_E_DIVERGED) {                                  /* a write outside the funnel moved g_save */
      if (jrn_recompute(r->j, &r->img) != 0) break;
      r->last_what = "resync"; r->last_rc = rc; r->lost++;
      crossed = 1;                  /* the resynced base is not the recorded chain's base: floor here too */
      if (r->state == IREC_OK) r->state = IREC_GAP;
      continue;
    }
    break;
  }
  rec_lost(r, "step", rc);
}

/* ---- staging ----------------------------------------------------------------------------------------- */
static void stage_now(ImgFlags* f, ImgRec* r, uint8_t* save, int slot, uint16_t mask, const uint8_t* const* blk) {
  int id;
  rec_step(r, save, slot, mask, blk);
  for (id = 0; id < (int)IMG_NSEC; id++) {
    const uint8_t* cur;
    if (!((mask >> id) & 1u)) continue;
    /* #300: a drop changes one or two of the nine PC sections; the rest are byte-identical, and rewriting them
     * (3,968 B copy + checksum each) was most of the mid-drop freeze. An identical section is left as it is. */
    cur = sec_data(save, slot, id);
    if (cur && memcmp(cur, blk[id], G3_SECTOR_DATA_SIZE) == 0 &&
        gen3_section_checksum_ok(save, slot, id, G3_SECTOR_DATA_SIZE)) continue;   /* a stale checksum is still healed */
    (void)gen3_write_full_section(save, slot, id, blk[id]);
  }
  if (r) r->name = 0;
  imgf_staged(f);
}

static bool stage_range(ImgFlags* f, ImgRec* r, uint8_t* save, int slot, int lo, int hi, const uint8_t* block,
                        bool allow_defer) {
  const uint8_t* blk[JRN_NREG_MAX];
  uint16_t mask = 0;
  int id;
  if (!f || !save || !block || lo < 0 || hi < lo || hi > 13) return false;
  for (id = lo; id <= hi; id++) {
    blk[id] = block + (uint32_t)(id - lo) * G3_SECTOR_DATA_SIZE;
    mask = (uint16_t)(mask | (1u << id));
  }
  if (allow_defer && r && r->depth) {                    /* inside a scope: the close applies it (one diff, one write) */
    for (id = lo; id <= hi; id++) r->blk[id] = blk[id];
    r->mask = (uint16_t)(r->mask | mask);
    if (f) imgf_staged(f);
    return true;
  }
  stage_now(f, r, save, slot, mask, blk);
  return true;
}

bool img_stage_sections(ImgFlags* f, ImgRec* r, uint8_t* save, int slot, int sect_lo, int sect_hi,
                        const uint8_t* block) {
  return stage_range(f, r, save, slot, sect_lo, sect_hi, block, true);
}

void img_pc_edited(ImgFlags* f, ImgRec* r, uint8_t* save, int slot, const uint8_t* pc, bool can_stage) {
  bool was_unstaged = f ? f->pc_unstaged : false;
  bool staged = can_stage &&
                img_stage_sections(f, r, save, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc);
  if (staged && r && r->depth) {                          /* deferred: g_pc is ahead of g_save until the scope closes */
    staged = false;
    if (!was_unstaged) r->pc_deferred = 1;                /* this scope raised the flag, so its close clears it */
  }
  imgf_pc_edited(f, staged);
}

bool img_fold_pc(ImgFlags* f, ImgRec* r, uint8_t* save, int slot, const uint8_t* pc) {
  if (!imgf_fold_needed(f)) return false;
  if (!stage_range(f, r, save, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc, false))
    return false;
  imgf_pc_folded(f);
  return true;
}

/* ---- scopes ------------------------------------------------------------------------------------------- */
void img_scope_open(ImgRec* r, const char* name) {
  if (!r) return;
  if (!r->depth && name) r->name = name;
  if (r->depth < 255u) r->depth++;
}

bool img_scope_flush(ImgFlags* f, ImgRec* r, uint8_t* save, int slot) {
  if (!r || !f || !save || !r->mask) return false;
  {
    uint16_t mask = r->mask;
    const uint8_t* blk[JRN_NREG_MAX];
    memcpy(blk, r->blk, sizeof blk);
    r->mask = 0;
    memset(r->blk, 0, sizeof r->blk);
    stage_now(f, r, save, slot, mask, blk);
  }
  if (r->pc_deferred) { imgf_pc_folded(f); r->pc_deferred = 0; }
  return true;
}

bool img_scope_close(ImgFlags* f, ImgRec* r, uint8_t* save, int slot) {
  if (!r || !r->depth) return true;
  r->depth--;
  if (r->depth) return true;
  (void)img_scope_flush(f, r, save, slot);
  r->name = 0;
  /* Rest-point high-water flush (#234 ruling): the screen is static here, so a nearly-full pend is written NOW
   * rather than by the next step's JRN_E_FULL fallback (rec_step) in the middle of its animation. */
  if (r->j && r->flush && jrn_pending(r->j) && jrn_pend_free(r->j) < JRN_PEND_HIWATER) (void)r->flush();
  return true;
}

void img_rec_cross(ImgRec* r) { if (r) r->epoch++; }
void img_rec_name(ImgRec* r, const char* name) { if (r) r->name = name; }
