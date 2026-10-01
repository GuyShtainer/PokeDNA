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
/* Slice 4: a flat (Game Boy) image pair -- region id = the id-th `regsz` window of both buffers. */
typedef struct { const uint8_t* old_img; const uint8_t* new_img; uint16_t regsz; uint8_t nreg; } FlatPair;

/* A read-only accessor over a FlatPair's OLD image: a resync must re-hash the baseline, not the image that already
 * holds the edit (the Game Boy session edits in place, so the accessor bound at open sees the NEW bytes). */
static int fp_get_old(void* ctx, uint8_t region, uint16_t off, uint8_t* dst, uint16_t n) {
  const FlatPair* fp = (const FlatPair*)ctx;
  if (!fp || !dst || region >= fp->nreg || (uint32_t)off + n > fp->regsz) return -1;
  memcpy(dst, fp->old_img + (uint32_t)region * fp->regsz + off, n);
  return 0;
}

static int rec_try(ImgRec* r, const char* name, int crossed, const uint8_t* save, int slot, uint16_t mask,
                   const uint8_t* const* blk, const FlatPair* fp) {
  int rc = jrn_step_begin(r->j, name, crossed), id;
  if (rc) return rc;
  if (fp) {
    for (id = 0; id < (int)fp->nreg; id++) {
      rc = jrn_step_region(r->j, (uint8_t)id, fp->old_img + (uint32_t)id * fp->regsz,
                           fp->new_img + (uint32_t)id * fp->regsz);
      if (rc) return rc;
    }
    return jrn_step_end(r->j);
  }
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

/* z9 (D10): the diff did not fit ONE record: record it as a CHAIN of up to JRN_CHAIN_MAX records (journal.h). Same blocks, same name, same crossed
 * flag as the one-record attempt that returned JRN_E_TOOBIG. The old/new pointers are only read for the duration of the call. Two noinline bodies, each with a
 * table sized to ITS image (14 sections / 8 flat regions), so neither adds the other's frame to the staging funnel's stack chain. */
static __attribute__((noinline)) int rec_chain_g3(ImgRec* r, const char* name, int crossed, const uint8_t* save, int slot, uint16_t mask,
                                                    const uint8_t* const* blk) {
  JrnBlk jb[IMG_NSEC];
  uint8_t id;
  memset(jb, 0, sizeof jb);
  for (id = 0; id < IMG_NSEC; id++) {
    const uint8_t* old_blk;
    if (!((mask >> id) & 1u)) continue;
    old_blk = sec_data(save, slot, id);
    if (!old_blk) continue;
    jb[id].old_blk = old_blk; jb[id].new_blk = blk[id];
  }
  return r->chain ? r->chain(name, crossed, jb, (uint8_t)IMG_NSEC) : jrn_chain_record(r->j, name, crossed, jb, (uint8_t)IMG_NSEC);
}

static __attribute__((noinline)) int rec_chain_flat(ImgRec* r, const char* name, int crossed, const FlatPair* fp) {
  JrnBlk jb[8];
  uint8_t id;
  if (fp->nreg > 8u) return JRN_E_TOOBIG;                                      /* a flat image is the 8-region Game Boy one */
  memset(jb, 0, sizeof jb);
  for (id = 0; id < fp->nreg; id++) {
    jb[id].old_blk = fp->old_img + (uint32_t)id * fp->regsz;
    jb[id].new_blk = fp->new_img + (uint32_t)id * fp->regsz;
  }
  return r->chain ? r->chain(name, crossed, jb, fp->nreg) : jrn_chain_record(r->j, name, crossed, jb, fp->nreg);
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
static void rec_step(ImgRec* r, const uint8_t* save, int slot, uint16_t mask, const uint8_t* const* blk,
                     const FlatPair* fp) {
  const char* name;
  int crossed, rc = JRN_E_STATE, attempt;
  if (!r || !r->j || r->state == IREC_OFF) return;
  name = r->name ? r->name : (fp ? "Edit" : default_name(mask));
  crossed = r->epoch != r->epoch_seen;
  for (attempt = 0; attempt < 3; attempt++) {
    rc = rec_try(r, name, crossed, save, slot, mask, blk, fp);
    if (rc == JRN_E_TOOBIG) rc = fp ? rec_chain_flat(r, name, crossed, fp) : rec_chain_g3(r, name, crossed, save, slot, mask, blk);   /* > one record: a chain, or (beyond JRN_CHAIN_MAX) the honest gap below */
    if (rc == JRN_OK) { r->epoch_seen = r->epoch; return; }
    if (rc == JRN_NOOP) return;                                  /* nothing changed: nothing to record */
    if (rc == JRN_E_FULL) { if (r->flush) (void)r->flush(); continue; }   /* flush, then re-stage in a clear buffer */
    if (rc == JRN_E_DIVERGED) {                                  /* a write outside the funnel moved g_save */
      if (fp) {
        JrnImage oi;
        oi.ctx = (void*)fp; oi.get = fp_get_old; oi.set = 0;
        if (jrn_recompute(r->j, &oi) != 0) break;
      } else if (jrn_recompute(r->j, &r->img) != 0) break;
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
  rec_step(r, save, slot, mask, blk, 0);
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
  return true;
}

void img_rec_cross(ImgRec* r) { if (r) r->epoch++; }
void img_rec_name(ImgRec* r, const char* name) { if (r) r->name = name; }

/* ---- slice 4: the Game Boy image -------------------------------------------------------------------------------- */
bool img_rec_flat(ImgRec* r, const uint8_t* old_img, const uint8_t* new_img, uint8_t nreg, uint16_t regsz,
                  const char* name) {
  FlatPair fp;
  if (!r || !r->j || r->state == IREC_OFF || !old_img || !new_img || !nreg || nreg > JRN_NREG_MAX || !regsz) return false;
  fp.old_img = old_img; fp.new_img = new_img; fp.regsz = regsz; fp.nreg = nreg;
  r->name = name;
  rec_step(r, 0, 0, 0, 0, &fp);
  r->name = 0;
  return true;
}
