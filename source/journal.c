/* SPDX-License-Identifier: GPL-3.0-or-later */
/* journal.c -- the #234 undo journal: format, open, append, keys. PURE C (see journal.h).
 * Undo/redo live in journal_undo.c. Design authority: docs/briefs/234-UNDO-DESIGN.md
 * D3/D4/D5/D6 + the v2-verify fix annotations. */
#include "journal.h"

#include <string.h>

#include "journal_int.h"

#define SEG_VER        JRN_SEG_VER
#define SLOT_FOREIGN   (-1)     /* slot_hdr / hdr_parse: valid magic + crc, but not a header this build understands */
#define ZCHUNK         256u    /* zero-in-place granularity                               */
#define ZWINDOW        1024u   /* the most a cut can leave past the tail; the most we zero */
#define RCHUNK         128u    /* streaming chunk for CRC / verify (stack)                */
#define SEG_MAX        9999u   /* logical segment index cap (the slot files are a fixed ring) */
#define RING_MAX       JRN_RING_MAX   /* slot files in the ring (max_segs + 1, max_segs <= JRN_MAX_SEGS) */
#define ZBODY          512u    /* recycle zero-fill granularity (stack)                   */
#define SCAN_MAX       0x40000u
#define RESOLVE_MAX    8u
#define PDR_LEN        16u
#define DEFAULT_SEGS   16u

_Static_assert(JRN_PEND_MAXN * JRN_REC_MIN <= JRN_PEND_CAP, "pending record count bound");
_Static_assert(JRN_REC_MAX <= JRN_PEND_CAP, "a record must fit the pending buffer");
_Static_assert(JRN_SEG_SIZE % ZCHUNK == 0, "zeroing chunks tile a segment");
_Static_assert(JRN_SEG_SIZE % ZBODY == 0 && ZBODY >= JRN_SEG_HDR, "recycle chunks tile a segment");
_Static_assert(JRN_REC_BASE % 512u == 0 && JRN_REC_BASE >= JRN_SEG_HDR && JRN_REC_BASE + JRN_REC_MAX < JRN_SEG_SIZE, "records start on a sector boundary, after the header sector");

/* ---- the frozen-timestamp hook -------------------------------------------------------------- */
static uint32_t s_hold_stamp;   /* the held FAT stamp (only meaningful while s_hold_on)   */
static uint8_t  s_hold_on;

uint32_t jrn_fattime_filter(uint32_t live) { return s_hold_on ? s_hold_stamp : live; }
int      jrn_stamp_held(void) { return s_hold_on; }

/* ---- pure helpers --------------------------------------------------------------------------- */
static const uint32_t k_crc_tab[16] = {
  0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
  0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
  0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu };

uint32_t jrn_crc32_update(uint32_t crc, const void* data, uint32_t n) {
  const uint8_t* p = (const uint8_t*)data;
  uint32_t i;
  if (!data && n) return crc;
  crc = ~crc;
  for (i = 0; i < n; i++) {
    crc ^= p[i];
    crc = (crc >> 4) ^ k_crc_tab[crc & 15u];
    crc = (crc >> 4) ^ k_crc_tab[crc & 15u];
  }
  return ~crc;
}

static uint64_t fnv_byte(uint64_t h, uint8_t b) { return (h ^ b) * 0x100000001B3ull; }

uint64_t jrn_key64(const uint8_t* name, uint8_t name_len, uint16_t tid, uint16_t sid,
                   uint8_t gender, uint8_t frlg) {
  uint64_t h = 0xCBF29CE484222325ull;
  uint8_t i;
  for (i = 0; name && i < name_len; i++) h = fnv_byte(h, name[i]);
  h = fnv_byte(h, (uint8_t)tid); h = fnv_byte(h, (uint8_t)(tid >> 8));
  h = fnv_byte(h, (uint8_t)sid); h = fnv_byte(h, (uint8_t)(sid >> 8));
  h = fnv_byte(h, gender);
  h = fnv_byte(h, frlg);
  return h;
}

void jrn_key_hex(uint64_t key, char out[17]) {
  static const char hx[] = "0123456789abcdef";
  int i;
  for (i = 0; i < 16; i++) out[i] = hx[(key >> (60 - 4 * i)) & 15u];
  out[16] = 0;
}

static uint32_t hash_of(const uint32_t* crc, uint8_t nreg) {
  uint8_t b[4u * JRN_NREG_MAX];
  uint8_t i;
  for (i = 0; i < nreg && i < JRN_NREG_MAX; i++) jrn_wr32(b + 4u * i, crc[i]);
  return jrn_crc32_update(0, b, 4u * nreg);
}

uint32_t jrn_hash(const Jrn* j) { return j ? hash_of(j->crc, j->nreg) : 0; }
uint32_t jrn_cursor(const Jrn* j) { return j ? j->cursor : 0; }
uint32_t jrn_tip(const Jrn* j) { return j ? j->tip : 0; }
int      jrn_pending(const Jrn* j) { return j ? j->pend_n : 0; }
int      jrn_offer(const Jrn* j) { return j ? j->offer : 0; }
int      jrn_flush_wanted(const Jrn* j) { return j ? j->flush_wanted : 0; }

/* ---- paths ------------------------------------------------------------------------------------ */
static int pput(char* b, uint32_t* n, const char* s) {
  while (*s) {
    if (*n + 1u >= JRN_PATH_MAX) return -1;
    b[(*n)++] = *s++;
  }
  b[*n] = 0;
  return 0;
}

static int p_key(const char* root, uint64_t key, const char* tail, char* out) {
  uint32_t n = 0;
  char hex[17];
  out[0] = 0;
  jrn_key_hex(key, hex);
  if (pput(out, &n, root) || pput(out, &n, "/") || pput(out, &n, hex)) return -1;
  return pput(out, &n, tail);
}

/* A slot file is NNNN.pdj, NNNN = the ring slot (1..ring). The LOGICAL segment index lives in
 * the slot's header; logical L sits in slot ((L-1) mod ring)+1 (deterministic, so a wrapped
 * ring reuses exactly the slot compaction just retired). */
static int p_slot(const char* root, uint64_t key, uint16_t slot, char* out) {
  uint32_t n;
  char d[6];
  if (!slot || slot > RING_MAX || p_key(root, key, "/", out)) return -1;
  n = 0; while (out[n]) n++;
  d[0] = (char)('0' + (slot / 1000u) % 10u); d[1] = (char)('0' + (slot / 100u) % 10u);
  d[2] = (char)('0' + (slot / 10u) % 10u);   d[3] = (char)('0' + slot % 10u); d[4] = 0;
  return pput(out, &n, d) || pput(out, &n, ".pdj");
}

static uint16_t slot_of(uint32_t idx, uint16_t ring) { return (uint16_t)((idx - 1u) % ring + 1u); }

static int seg_path(const Jrn* j, uint16_t idx, char* out) {
  if (!idx || idx > SEG_MAX || j->ring < 2u) return -1;
  return p_slot(j->root, j->key, slot_of(idx, j->ring), out);
}

/* ---- segment I/O ------------------------------------------------------------------------------- */
static int seg_read(const Jrn* j, uint16_t idx, uint32_t off, void* buf, uint32_t n) {
  char p[JRN_PATH_MAX];
  if (seg_path(j, idx, p) || off + n > JRN_SEG_SIZE) return JRN_E_ARG;
  return j->fs->read(j->fs->ctx, p, off, buf, n) == 0 ? 0 : JRN_E_IO;
}

/* In-place write with the file's OWN timestamp held, so f_sync rewrites the directory
 * entry byte-for-byte (D3 fix 6). */
static int seg_write(const Jrn* j, uint16_t idx, uint32_t off, const void* buf, uint32_t n) {
  char p[JRN_PATH_MAX];
  int rc;
  if (seg_path(j, idx, p) || off + n > JRN_SEG_SIZE) return JRN_E_ARG;
  s_hold_stamp = j->fs->stamp(j->fs->ctx, p);
  s_hold_on = 1;
  rc = j->fs->write(j->fs->ctx, p, off, buf, n);
  s_hold_on = 0;
  return rc == 0 ? 0 : JRN_E_IO;
}

/* The bytes on the card must equal `buf` (the card ACKs writes it drops: LOG_VERIFY_LOST). */
static int seg_verify(const Jrn* j, uint16_t idx, uint32_t off, const uint8_t* buf, uint32_t n) {
  uint8_t chunk[RCHUNK];
  uint32_t c, m;
  int rc;
  for (c = 0; c < n; c += m) {
    m = n - c < RCHUNK ? n - c : RCHUNK;
    rc = seg_read(j, idx, off + c, chunk, m);
    if (rc) return rc;
    if (memcmp(chunk, buf + c, m) != 0) return JRN_E_VERIFY;
  }
  return 0;
}

/* Header: 'PDJS', ver u16, ring u16, logical index u32, zeros, crc32 of the first 28 bytes.
 * All-zero = a FREE slot (never written, or retired in place). */
static void seg_hdr_build(uint16_t idx, uint16_t ring, uint8_t* h) {
  memset(h, 0, JRN_SEG_HDR);
  h[0] = 'P'; h[1] = 'D'; h[2] = 'J'; h[3] = 'S';
  jrn_wr16(h + 4, SEG_VER);
  jrn_wr16(h + 6, ring);
  jrn_wr32(h + 8, idx);
  jrn_wr32(h + 28, jrn_crc32_update(0, h, 28));
}

/* Tri-state (the version rule in journal.h). 0 = FREE: bad magic or bad crc (never written, retired in
 * place, or torn). 1 = LIVE: magic + crc hold and version, index and ring are ones this build knows.
 * SLOT_FOREIGN: magic + crc hold but the version, index or ring is NOT -- somebody else's data. */
static int hdr_parse(const uint8_t* h, uint32_t* idx, uint16_t* ring) {
  if (h[0] != 'P' || h[1] != 'D' || h[2] != 'J' || h[3] != 'S') return 0;
  if (jrn_crc32_update(0, h, 28) != jrn_rd32(h + 28)) return 0;
  *idx = jrn_rd32(h + 8); *ring = jrn_rd16(h + 6);
  if (jrn_rd16(h + 4) != SEG_VER || *idx < 1u || *idx > SEG_MAX || *ring < 2u || *ring > RING_MAX) return SLOT_FOREIGN;
  return 1;
}

/* The slot file `slot`: 1 = full size and a live header (fills idx + ring), 0 = free / no such file /
 * a partial file, SLOT_FOREIGN = a header this build does not understand, JRN_E_IO = the card failed. */
static int slot_hdr(const Jrn* j, uint16_t slot, uint32_t* idx, uint16_t* ring) {
  char p[JRN_PATH_MAX];
  uint8_t h[JRN_SEG_HDR];
  long sz;
  if (p_slot(j->root, j->key, slot, p)) return 0;
  sz = j->fs->size(j->fs->ctx, p);
  if (sz < -1) return JRN_E_IO;
  if (sz != (long)JRN_SEG_SIZE) return 0;
  if (j->fs->read(j->fs->ctx, p, 0, h, JRN_SEG_HDR) != 0) return JRN_E_IO;
  return hdr_parse(h, idx, ring);
}

static int seg_valid(const Jrn* j, uint16_t idx) {
  uint32_t i;
  uint16_t r;
  if (!idx || idx > SEG_MAX || j->ring < 2u) return 0;
  return slot_hdr(j, slot_of(idx, j->ring), &i, &r) == 1 && i == idx && r == j->ring;
}


int jrn_i_hdr_parse(const uint8_t* b, JrnRec* r) {
  uint16_t len;
  uint8_t flags;
  if (jrn_rd32(b) != JRN_REC_MAGIC) return 1;
  len = jrn_rd16(b + 4);
  flags = b[6];
  if (len < JRN_REC_MIN || len > JRN_REC_MAX || (flags & ~0x31u)) return 2;
  r->kind = (uint8_t)((flags >> 4) & 3u);
  r->crossed = (uint8_t)(flags & 1u);
  r->nspans = b[7];
  if (r->kind > JRN_KIND_DISCARD || (r->kind != JRN_KIND_STEP && r->nspans)) return 2;
  r->len = len;
  r->seq = jrn_rd32(b + 8);   r->parent = jrn_rd32(b + 12); r->aux = jrn_rd32(b + 16);
  r->pre = jrn_rd32(b + 20);  r->post = jrn_rd32(b + 24);
  memcpy(r->name, b + 28, JRN_NAME_LEN);
  r->name[JRN_NAME_LEN] = 0;
  return r->seq == 0 ? 2 : 0;
}

/* Header only (no CRC): 0 ok, 1/2 = no record here, <0 = I/O. */
static int rec_hdr_at(const Jrn* j, uint16_t seg, uint32_t off, JrnRec* r) {
  uint8_t b[JRN_REC_HDR];
  int rc;
  if (off + JRN_REC_MIN > JRN_SEG_SIZE) return 1;
  rc = seg_read(j, seg, off, b, JRN_REC_HDR);
  if (rc) return rc;
  rc = jrn_i_hdr_parse(b, r);
  if (rc) return rc;
  return off + r->len > JRN_SEG_SIZE ? 2 : 0;
}

/* Header + CRC: a torn tail or flipped bit is 2, never a record. One read covers the header
 * and (for a marker or a small step) the whole record; longer ones stream in RCHUNK pieces. */
static int rec_at(const Jrn* j, uint16_t seg, uint32_t off, JrnRec* r) {
  uint8_t chunk[RCHUNK], tail[4];
  uint32_t crc = 0, body, c, m, n0 = JRN_SEG_SIZE - off < RCHUNK ? JRN_SEG_SIZE - off : RCHUNK;
  uint32_t i;
  int rc;
  if (off + JRN_REC_MIN > JRN_SEG_SIZE) return 1;
  rc = seg_read(j, seg, off, chunk, n0);
  if (rc) return rc;
  rc = jrn_i_hdr_parse(chunk, r);
  if (rc) return rc;
  if (off + r->len > JRN_SEG_SIZE) return 2;
  body = (uint32_t)r->len - 4u;
  for (c = 0; c < r->len; c += m) {
    if (c) {
      m = r->len - c < RCHUNK ? r->len - c : RCHUNK;
      rc = seg_read(j, seg, off + c, chunk, m);
      if (rc) return rc;
    } else {
      m = n0 < r->len ? n0 : r->len;
    }
    for (i = 0; i < m; i++)                      /* the last 4 bytes of the record are the stored crc */
      if (c + i >= body) tail[c + i - body] = chunk[i];
    crc = jrn_crc32_update(crc, chunk, c + m <= body ? m : (c < body ? body - c : 0));
  }
  return jrn_rd32(tail) == crc ? 0 : 2;
}

int jrn_i_src_read(const Jrn* j, const JrnSrc* s, uint32_t off, void* buf, uint32_t n) {
  if (!j || !s || !buf) return JRN_E_ARG;
  if (s->ram) { memcpy(buf, s->ram + off, n); return 0; }
  return seg_read(j, s->seg, s->base + off, buf, n);
}

/* ---- pending buffer helpers ------------------------------------------------------------------ */
int jrn_i_pend_last(const Jrn* j, uint16_t* start, JrnRec* r) {
  uint16_t off = 0, last = 0;
  uint8_t i;
  JrnRec t;
  if (!j->pend_n) return JRN_E_NOTHING;
  for (i = 0; i < j->pend_n; i++) {
    if (off + JRN_REC_MIN > j->pend_len || jrn_i_hdr_parse(j->pend + off, &t)) return JRN_E_STATE;
    last = off;
    off = (uint16_t)(off + t.len);
    *r = t;
  }
  if (off != j->pend_len) return JRN_E_STATE;
  *start = last;
  return 0;
}

void jrn_i_pend_pop(Jrn* j, uint16_t start) {
  j->pend_len = start;
  j->pend_n--;
  j->next_seq--;
}

int jrn_i_locate(Jrn* j, uint32_t seq, JrnRec* r, JrnSrc* src) {
  uint16_t off = 0, seg;
  uint8_t i;
  uint32_t o, g;
  int rc;
  for (i = 0; i < j->pend_n; i++) {
    if (jrn_i_hdr_parse(j->pend + off, r)) return JRN_E_STATE;
    if (r->seq == seq && r->kind == JRN_KIND_STEP) { src->ram = j->pend + off; src->seg = 0; src->base = 0; return 0; }
    off = (uint16_t)(off + r->len);
  }
  if (!j->seg_first || !j->tail_seg) return JRN_E_FLOOR;
  for (seg = j->seg_first; seg <= j->tail_seg; seg++) {
    o = JRN_REC_BASE;
    for (g = 0; g < JRN_SEG_SIZE / JRN_REC_MIN + 1u; g++) {
      if (seg == j->tail_seg && o >= j->tail_off) break;
      rc = rec_hdr_at(j, seg, o, r);
      if (rc < 0) return rc;
      if (rc > 0) break;
      if (r->seq > seq) return JRN_E_FLOOR;
      if (r->seq == seq) {
        if (r->kind != JRN_KIND_STEP) return JRN_E_FLOOR;
        src->ram = 0; src->seg = seg; src->base = o;
        return 0;
      }
      o += r->len;
    }
  }
  return JRN_E_FLOOR;
}

int jrn_find(Jrn* j, uint32_t seq, JrnRec* out) {
  JrnSrc s;
  if (!j || !out || !seq) return JRN_E_ARG;
  return jrn_i_locate(j, seq, out, &s);
}

/* ---- redirects (.pdr) ---------------------------------------------------------------------------- */
/* 0 = found and valid, 1 = absent or corrupt (a corrupt redirect is no redirect). */
static int pdr_read(const JrnFs* fs, const char* root, uint64_t key, uint64_t* target) {
  char p[JRN_PATH_MAX];
  uint8_t b[PDR_LEN];
  uint64_t t = 0;
  int i;
  if (p_key(root, key, ".pdr", p)) return 1;
  if (fs->size(fs->ctx, p) != (long)PDR_LEN) return 1;
  if (fs->read(fs->ctx, p, 0, b, PDR_LEN) != 0) return 1;
  if (b[0] != 'P' || b[1] != 'D' || b[2] != 'R' || b[3] != 'D') return 1;
  if (jrn_rd32(b + 12) != jrn_crc32_update(0, b, 12)) return 1;
  for (i = 7; i >= 0; i--) t = (t << 8) | b[4 + i];
  *target = t;
  return 0;
}

int jrn_key_resolve(const JrnFs* fs, const char* root, uint64_t key, uint64_t* out) {
  uint64_t cur = key, next = 0;
  uint32_t hop;
  if (!fs || !fs->size || !fs->read || !root || !out) return JRN_E_ARG;
  for (hop = 0; hop < RESOLVE_MAX; hop++) {
    if (pdr_read(fs, root, cur, &next) != 0) { *out = cur; return JRN_OK; }
    cur = next;
  }
  return JRN_E_LOOP;
}

static int mkdir_prefixes(const JrnFs* fs, const char* root) {
  char p[JRN_PATH_MAX];
  uint32_t i, n = 0;
  if (pput(p, &n, root)) return JRN_E_ARG;
  for (i = 1; p[i]; i++) {
    if (p[i] != '/') continue;
    p[i] = 0;
    if (fs->mkdir(fs->ctx, p) != 0) return JRN_E_IO;
    p[i] = '/';
  }
  return fs->mkdir(fs->ctx, p) == 0 ? 0 : JRN_E_IO;
}

int jrn_redirect_write(const JrnFs* fs, const char* root, uint64_t newkey, uint64_t target) {
  char pdr[JRN_PATH_MAX];
  uint8_t b[PDR_LEN];
  uint64_t t = 0, have = 0;
  int i, rc;
  if (!fs || !fs->size || !fs->create_zero || !fs->unlink || !root) return JRN_E_ARG;
  rc = jrn_key_resolve(fs, root, target, &t);
  if (rc) return rc;
  if (t == newkey) return JRN_OK;                         /* renamed back: nothing to record */
  if (pdr_read(fs, root, newkey, &have) == 0) return have == t ? JRN_OK : JRN_E_EXISTS;
  if (mkdir_prefixes(fs, root)) return JRN_E_IO;
  if (p_key(root, newkey, ".pdr", pdr)) return JRN_E_ARG;
  if (fs->size(fs->ctx, pdr) >= 0 && fs->unlink(fs->ctx, pdr) != 0) return JRN_E_IO;  /* an invalid leftover */
  b[0] = 'P'; b[1] = 'D'; b[2] = 'R'; b[3] = 'D';
  for (i = 0; i < 8; i++) b[4 + i] = (uint8_t)(t >> (8 * i));
  jrn_wr32(b + 12, jrn_crc32_update(0, b, 12));
  /* Written under its FINAL name (no rename: nothing is ever deleted on this path). Valid only
   * when the file is exactly 16 bytes AND the crc holds; a cut leaves an invalid file that
   * pdr_read ignores, so key resolution reads exactly its before- or after-state. */
  return fs->create_zero(fs->ctx, pdr, PDR_LEN, b, PDR_LEN) == 0 ? JRN_OK : JRN_E_IO;
}

/* ---- image CRCs ------------------------------------------------------------------------------------ */
int jrn_i_region_crc(const Jrn* j, const JrnImage* img, uint8_t region, uint32_t* out) {
  uint8_t chunk[128];
  uint32_t c, m, crc = 0;
  if (!j || !img || !img->get || !out || region >= j->nreg) return JRN_E_ARG;
  for (c = 0; c < j->reg_size; c += m) {
    m = j->reg_size - c < sizeof chunk ? (uint32_t)j->reg_size - c : (uint32_t)sizeof chunk;
    if (img->get(img->ctx, region, (uint16_t)c, chunk, (uint16_t)m) != 0) return JRN_E_IO;
    crc = jrn_crc32_update(crc, chunk, m);
  }
  *out = crc;
  return 0;
}

int jrn_recompute(Jrn* j, const JrnImage* img) {
  uint8_t r;
  int rc;
  if (!j || !img) return JRN_E_ARG;
  for (r = 0; r < j->nreg; r++) {
    rc = jrn_i_region_crc(j, img, r, &j->crc[r]);
    if (rc) return rc;
  }
  return 0;
}

/* ---- open: directory scan, prefix validation, repair, anchor ------------------------------------- */
typedef struct DirScan { uint16_t hi; } DirScan;   /* highest slot file name present */

static int name_idx(const char* s, const char* ext, uint16_t* idx) {
  uint32_t v = 0;
  int i;
  for (i = 0; i < 4; i++) { if (s[i] < '0' || s[i] > '9') return 0; v = v * 10u + (uint32_t)(s[i] - '0'); }
  if (s[4] != '.') return 0;
  for (i = 0; i < 3; i++) {
    char c = s[5 + i];
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    if (c != ext[i]) return 0;
  }
  if (s[8] != 0 || v == 0 || v > SEG_MAX) return 0;
  *idx = (uint16_t)v;
  return 1;
}

static void dir_cb(void* arg, const char* name) {
  DirScan* d = (DirScan*)arg;
  uint16_t idx;
  if (name_idx(name, "pdj", &idx) && idx > d->hi) d->hi = idx;
}

typedef struct Scan { uint32_t expect, c_last, tip_aux, latest; uint8_t have, rootpre; } Scan;

static void scan_absorb(Scan* sc, const JrnRec* r, uint32_t hash) {
  if (r->kind == JRN_KIND_STEP) {
    sc->c_last = r->seq; sc->tip_aux = r->seq;
    if (r->post == hash) sc->latest = r->seq;
    if (!r->parent && r->pre == hash) sc->rootpre = 1;   /* the image is a root's before-state */
  } else {
    sc->c_last = r->parent;
    sc->tip_aux = r->kind == JRN_KIND_CURSOR ? r->aux : r->parent;
  }
  sc->expect = r->seq + 1u;
  sc->have = 1;
}

static int scan_next_seg_continues(const Jrn* j, uint16_t seg, const Scan* sc) {
  JrnRec r;
  return rec_at(j, seg, JRN_REC_BASE, &r) == 0 && (!sc->have || r.seq == sc->expect);
}

/* Longest valid prefix. Ends at the first record that is absent, corrupt or out of seq
 * (unless the next segment continues the sequence: the slack a full segment leaves). */
static int scan_prefix(Jrn* j, uint32_t hash, Scan* sc) {
  uint16_t seg = j->seg_first;
  uint32_t off = JRN_REC_BASE, guard;
  JrnRec r;
  int rc;
  for (guard = 0; guard < SCAN_MAX; guard++) {
    rc = rec_at(j, seg, off, &r);
    if (rc < 0) return rc;
    if (rc == 0 && (!sc->have || r.seq == sc->expect)) { scan_absorb(sc, &r, hash); off += r.len; continue; }
    if (seg < j->seg_last && scan_next_seg_continues(j, (uint16_t)(seg + 1u), sc)) { seg++; off = JRN_REC_BASE; continue; }
    break;
  }
  j->tail_seg = seg;
  j->tail_off = off;
  j->next_seq = sc->have ? sc->expect : 1u;
  return 0;
}

/* Zero every non-zero chunk in [from, from + ZWINDOW), IN PLACE (never a truncate: the segment's
 * size and FAT chain are fixed). The window is the WHOLE damage a cut can do: a torn record is
 * <= JRN_REC_MAX and an orphaned back-to-front batch is <= JRN_PEND_CAP, both starting at the
 * tail. Nothing past the window is touched -- a corrupt record in the MIDDLE of a journal must
 * cost the history behind it (the valid prefix ends there), never zero valid bytes far away. */
static int zero_from(const Jrn* j, uint16_t seg, uint32_t from) {
  uint8_t buf[ZCHUNK], z[ZCHUNK];
  uint32_t off, n, i;
  int rc, dirty;
  memset(z, 0, sizeof z);
  for (off = from; off < JRN_SEG_SIZE && off < from + ZWINDOW; off += n) {
    n = JRN_SEG_SIZE - off < ZCHUNK ? JRN_SEG_SIZE - off : ZCHUNK;
    rc = seg_read(j, seg, off, buf, n);
    if (rc) return rc;
    for (dirty = 0, i = 0; i < n; i++) if (buf[i]) { dirty = 1; break; }
    if (dirty) { rc = seg_write(j, seg, off, z, n); if (rc) return rc; }
  }
  return 0;
}

static int repair_tail(const Jrn* j) {
  uint16_t s;
  uint8_t b[16];
  int rc, i, dirty;
  if (j->readonly || !j->tail_seg) return 0;
  rc = zero_from(j, j->tail_seg, j->tail_off);
  if (rc) return rc;
  for (s = (uint16_t)(j->tail_seg + 1u); s <= j->seg_last && s; s++) {   /* orphans past the tail */
    rc = seg_read(j, s, JRN_REC_BASE, b, sizeof b);
    if (rc) return rc;
    for (dirty = 0, i = 0; i < (int)sizeof b; i++) if (b[i]) dirty = 1;
    if (dirty) { rc = zero_from(j, s, JRN_REC_BASE); if (rc) return rc; }
  }
  return 0;
}

/* Where does the loaded image sit in the tree? Walk the last cursor position up until the
 * image hash matches a post (image is there) or a pre (image is at the parent). */
static void anchor_cursor(Jrn* j, const Scan* sc, uint32_t hash) {
  uint32_t cur = sc->c_last, hops;
  int found = 0;
  JrnRec r;
  JrnSrc s;
  j->anchor = JRN_ANCHOR_EMPTY;
  j->cursor = 0; j->tip = 0;
  if (!sc->have) return;
  for (hops = 0; cur && hops < JRN_WALK_MAX && !found; hops++) {
    if (jrn_i_locate(j, cur, &r, &s) != 0) break;
    if (r.post == hash) { j->cursor = cur; found = 1; }
    else if (r.pre == hash) { j->cursor = r.parent; found = 1; }
    else cur = r.parent;
  }
  if (!found && !sc->c_last && sc->rootpre) { j->cursor = 0; found = 1; }   /* cursor rests before every root */
  if (found) {
    j->tip = j->cursor == sc->c_last ? sc->tip_aux : sc->c_last;
    j->anchor = JRN_ANCHOR_MATCH;
    j->offer = j->cursor != sc->c_last;          /* recorded steps the image lacks */
  } else if (sc->latest) {
    j->cursor = sc->latest; j->tip = sc->latest; j->anchor = JRN_ANCHOR_BRANCH;
  } else {
    j->anchor = JRN_ANCHOR_NEWROOT;
  }
}

static int open_validate(const JrnCfg* c, const JrnImage* img) {
  const JrnFs* f;
  if (!c || !img || !img->get || !c->fs || !c->root) return JRN_E_ARG;
  f = c->fs;
  if (!f->mkdir || !f->size || !f->read || !f->write || !f->create_zero ||
      !f->unlink || !f->list || !f->stamp) return JRN_E_ARG;
  if (!c->nreg || c->nreg > JRN_NREG_MAX || !c->reg_size) return JRN_E_ARG;
  return 0;
}

/* The ring, read from the slot headers. The ring size comes from the first live header found;
 * the OLDEST live segment is the slot whose header carries the lowest logical index (NEVER the
 * lowest slot number: after a wrap the two differ). A zeroed / half-zeroed header is a free slot; a
 * FOREIGN header (JRN_SEG_VER rule) makes the whole journal unreadable to this build -> JRN_E_VERSION;
 * a card error -> JRN_E_IO (never a silently shorter ring). The live set extends contiguously
 * (idx+1, idx+2 ...) through the header indices. 1 = a ring, 0 = none, < 0 = error. */
static int ring_scan(Jrn* j, uint16_t hi) {
  uint32_t sidx[RING_MAX], lo = 0, nxt;
  uint16_t srng[RING_MAX], s, lim = hi < RING_MAX ? hi : (uint16_t)RING_MAX, ring = 0;
  int rc;
  for (s = 1; s <= lim; s++) {
    sidx[s - 1u] = 0; srng[s - 1u] = 0;
    rc = slot_hdr(j, s, &sidx[s - 1u], &srng[s - 1u]);
    if (rc < 0) return rc == SLOT_FOREIGN ? JRN_E_VERSION : rc;
    if (rc != 1) sidx[s - 1u] = 0;
    else if (!ring && slot_of(sidx[s - 1u], srng[s - 1u]) == s) ring = srng[s - 1u];
  }
  if (!ring) return 0;
  for (s = 1; s <= ring && s <= lim; s++)
    if (sidx[s - 1u] && srng[s - 1u] == ring && slot_of(sidx[s - 1u], ring) == s && (!lo || sidx[s - 1u] < lo)) lo = sidx[s - 1u];
  if (!lo) return 0;
  j->ring = ring;
  j->seg_first = j->seg_last = (uint16_t)lo;
  for (nxt = lo + 1u; nxt <= SEG_MAX; nxt++) {
    s = slot_of(nxt, ring);
    if (s > lim || sidx[s - 1u] != nxt || srng[s - 1u] != ring) break;
    j->seg_last = (uint16_t)nxt;
  }
  return 1;
}

int jrn_open(Jrn* j, const JrnCfg* cfg, const JrnImage* img) {
  DirScan d;
  Scan sc;
  char dir[JRN_PATH_MAX];
  int rc = open_validate(cfg, img);
  if (rc || !j) return rc ? rc : JRN_E_ARG;
  memset(j, 0, sizeof *j);
  memset(&sc, 0, sizeof sc);
  memset(&d, 0, sizeof d);
  j->fs = cfg->fs; j->root = cfg->root; j->nreg = cfg->nreg; j->reg_size = cfg->reg_size;
  j->max_segs = cfg->max_segs ? (cfg->max_segs < JRN_MAX_SEGS ? cfg->max_segs : (uint8_t)JRN_MAX_SEGS) : (uint8_t)DEFAULT_SEGS;
  j->readonly = cfg->readonly ? 1 : 0;
  j->next_seq = 1;
  rc = jrn_key_resolve(cfg->fs, cfg->root, cfg->key, &j->key);
  if (rc) return rc;
  rc = jrn_recompute(j, img);
  if (rc) return rc;
  if (p_key(j->root, j->key, "", dir)) return JRN_E_ARG;
  if (j->fs->list(j->fs->ctx, dir, dir_cb, &d) != 0 || !d.hi) { j->anchor = JRN_ANCHOR_EMPTY; return JRN_OK; }
  rc = ring_scan(j, d.hi);
  if (rc == JRN_E_VERSION) {                      /* somebody else's journal: read-only, empty, untouched */
    j->foreign = 1; j->readonly = 1; j->ring = 0; j->seg_first = j->seg_last = j->tail_seg = 0;
    j->anchor = JRN_ANCHOR_EMPTY;
    return JRN_E_VERSION;
  }
  if (rc < 0) return rc;
  if (!rc) { j->anchor = JRN_ANCHOR_EMPTY; return JRN_OK; }
  rc = scan_prefix(j, jrn_hash(j), &sc);
  if (rc) return rc;
  rc = repair_tail(j);
  if (rc) return rc;
  anchor_cursor(j, &sc, jrn_hash(j));
  return JRN_OK;
}

/* ---- safe moments: prepare + compact --------------------------------------------------------------- */
/* THE RING (slice-1 ruling, D3). The exFAT sweep proved a torn directory entry set hides every
 * live segment behind it, so after the FIRST FILL the directory never changes: the segment set is
 * a fixed ring of `ring` slot files created once, and everything after is an IN-PLACE write.
 *   activate  = zero the slot's dirty bytes (header first), then write the header LAST: a slot is a
 *               segment only when its header crc holds, so a cut leaves it free (before) or live.
 *   retire    = zero the header in place (compaction): a cut leaves it live (before) or free.
 * No create, delete or rename runs here. */

/* FIRST FILL only: create every slot file, zero-filled (create-or-complete: a cut leftover is
 * finished, never recreated). No header yet, so the journal still reads as empty. */
static int ring_ensure(const Jrn* j) {
  char p[JRN_PATH_MAX];
  const JrnFs* f = j->fs;
  uint16_t s;
  long sz;
  for (s = 1; s <= j->ring; s++) {
    if (p_slot(j->root, j->key, s, p)) return JRN_E_ARG;
    sz = f->size(f->ctx, p);
    if (sz == (long)JRN_SEG_SIZE) continue;
    if (sz > (long)JRN_SEG_SIZE) return JRN_E_IO;
    if (f->create_zero(f->ctx, p, JRN_SEG_SIZE, 0, 0) != 0) return JRN_E_IO;
  }
  return 0;
}

/* Zero every non-zero ZBODY chunk of the slot IN PLACE (header included: a torn remnant of a
 * retire must not survive into the new life). */
static int slot_zero(const Jrn* j, uint16_t idx) {
  uint8_t buf[ZBODY];
  uint32_t off, i;
  int rc, dirty;
  for (off = 0; off < JRN_SEG_SIZE; off += ZBODY) {
    rc = seg_read(j, idx, off, buf, ZBODY);
    if (rc) return rc;
    for (dirty = 0, i = 0; i < ZBODY; i++) if (buf[i]) { dirty = 1; break; }
    if (!dirty) continue;
    memset(buf, 0, sizeof buf);
    rc = seg_write(j, idx, off, buf, ZBODY);
    if (rc) return rc;
  }
  return 0;
}

/* Logical segment `idx` becomes live in its slot: recycle (zero-fill) then header LAST. */
static int seg_activate(Jrn* j, uint16_t idx) {
  char p[JRN_PATH_MAX];
  uint8_t h[JRN_SEG_HDR];
  uint32_t oi;
  uint16_t orng;
  int rc;
  if (seg_path(j, idx, p)) return JRN_E_ARG;
  if (j->fs->size(j->fs->ctx, p) != (long)JRN_SEG_SIZE) return JRN_E_IO;   /* the ring is not whole */
  if (j->foreign) return JRN_E_VERSION;
  rc = slot_hdr(j, slot_of(idx, j->ring), &oi, &orng);
  if (rc == SLOT_FOREIGN) { j->foreign = 1; return JRN_E_VERSION; }   /* a newer build owns it: never zero it */
  if (rc < 0) return rc;
  if (rc == 1) return JRN_E_STATE;                                     /* a live segment owns it */
  rc = slot_zero(j, idx);
  if (rc) return rc;
  seg_hdr_build(idx, j->ring, h);
  rc = seg_write(j, idx, 0, h, JRN_SEG_HDR);
  if (rc) return rc;
  return seg_valid(j, idx) ? 0 : JRN_E_IO;
}

/* Compaction step: the OLDEST segment leaves by zeroing its header, in place. */
static int seg_retire(const Jrn* j, uint16_t idx) {
  uint8_t z[JRN_SEG_HDR], b[JRN_SEG_HDR];
  int rc;
  memset(z, 0, sizeof z);
  rc = seg_write(j, idx, 0, z, JRN_SEG_HDR);
  if (rc) return rc;
  rc = seg_read(j, idx, 0, b, JRN_SEG_HDR);
  if (rc) return rc;
  return memcmp(b, z, sizeof z) == 0 ? 0 : JRN_E_VERIFY;
}

int jrn_prepare_first(Jrn* j) {
  char dir[JRN_PATH_MAX];
  int rc;
  if (!j) return JRN_E_ARG;
  if (j->foreign) return JRN_E_VERSION;
  if (j->readonly) return JRN_E_RDONLY;
  rc = mkdir_prefixes(j->fs, j->root);
  if (rc) return rc;
  if (p_key(j->root, j->key, "", dir)) return JRN_E_ARG;
  if (j->fs->mkdir(j->fs->ctx, dir) != 0) return JRN_E_IO;
  if (j->seg_last) return JRN_OK;
  j->ring = (uint16_t)(j->max_segs + 1u);      /* a fresh key: the ring is sized once, here */
  rc = ring_ensure(j);
  if (rc) return rc;
  rc = seg_activate(j, 1);
  if (rc) return rc;
  j->seg_first = j->seg_last = j->tail_seg = 1;
  j->tail_off = JRN_REC_BASE;
  return JRN_OK;
}

/* The retention cap in segments: max_segs, and never more than the ring can hold beside a spare. */
static uint16_t seg_cap(const Jrn* j) {
  uint16_t room = j->ring ? (uint16_t)(j->ring - 1u) : (uint16_t)j->max_segs;
  return j->max_segs < room ? (uint16_t)j->max_segs : room;
}

int jrn_compact(Jrn* j) {
  uint32_t g;
  int rc;
  if (!j) return JRN_E_ARG;
  if (j->foreign) return JRN_E_VERSION;
  if (j->readonly) return JRN_E_RDONLY;
  for (g = 0; g < RING_MAX && j->seg_first && j->seg_first < j->tail_seg &&
              (uint32_t)(j->seg_last - j->seg_first + 1u) > seg_cap(j); g++) {
    rc = seg_retire(j, j->seg_first);
    if (rc) return rc;
    j->seg_first++;
  }
  return JRN_OK;
}

int jrn_prepare(Jrn* j) {
  int rc = jrn_prepare_first(j);
  if (rc) return rc;
  if (j->seg_last != j->tail_seg || j->seg_last >= SEG_MAX) return JRN_OK;   /* a spare already exists */
  if ((uint32_t)(j->seg_last - j->seg_first + 1u) >= j->ring) {           /* no free slot: retire the oldest */
    rc = jrn_compact(j);
    if (rc) return rc;
    if ((uint32_t)(j->seg_last - j->seg_first + 1u) >= j->ring) return JRN_E_FULL;
  }
  rc = seg_activate(j, (uint16_t)(j->seg_last + 1u));   /* the NEXT segment, never mid-session */
  if (rc) return rc;
  j->seg_last++;
  return JRN_OK;
}

/* ---- recording: building a step in the pending buffer ------------------------------------------------- */
static uint16_t pend_room(const Jrn* j) { return (uint16_t)(JRN_PEND_CAP - j->pend_len - j->bld_len); }

int jrn_step_begin(Jrn* j, const char* name, int crossed) {
  uint8_t* b;
  uint32_t i;
  if (!j || !name) return JRN_E_ARG;
  if (j->readonly) return JRN_E_RDONLY;
  if (j->stopped) return JRN_E_STOPPED;
  if (j->bld_len) return JRN_E_STATE;
  if (j->pend_n >= JRN_PEND_MAXN || pend_room(j) < JRN_REC_MIN) { j->flush_wanted = 1; return JRN_E_FULL; }
  b = j->pend + j->pend_len;
  memset(b, 0, JRN_REC_HDR);
  jrn_wr32(b, JRN_REC_MAGIC);
  b[6] = (uint8_t)((crossed ? 1u : 0u) | (JRN_KIND_STEP << 4));
  jrn_wr32(b + 8, j->next_seq);
  jrn_wr32(b + 12, j->cursor);
  jrn_wr32(b + 20, hash_of(j->crc, j->nreg));
  for (i = 0; i < JRN_NAME_LEN && name[i]; i++) b[28 + i] = (uint8_t)name[i];
  memcpy(j->bcrc, j->crc, sizeof j->bcrc);
  j->bld_len = JRN_REC_HDR;
  j->nspans = 0;
  return JRN_OK;
}

void jrn_step_abort(Jrn* j) { if (j) { j->bld_len = 0; j->nspans = 0; } }

/* Append one span's bytes; the caller checked the room. */
static void span_put(Jrn* j, uint8_t region, uint16_t off, uint16_t len, const uint8_t* old_p, const uint8_t* new_p) {
  uint8_t* b = j->pend + j->pend_len + j->bld_len;
  b[0] = region; b[1] = 0;
  jrn_wr16(b + 2, off); jrn_wr16(b + 4, len);
  memcpy(b + JRN_SPAN_HDR, old_p, len);
  memcpy(b + JRN_SPAN_HDR + len, new_p, len);
  j->bld_len = (uint16_t)(j->bld_len + JRN_SPAN_HDR + 2u * len);
  j->nspans++;
}

/* The next changed run at or after `from`: [*a, *b), tolerating up to JRN_MERGE_GAP equal
 * bytes inside it (a gap costs 2 bytes per byte, a new span costs 6 + 2). */
static int next_run(const uint8_t* o, const uint8_t* n, uint16_t size, uint16_t from, uint16_t* a, uint16_t* b) {
  uint32_t i = from, end, k, lim;
  int more = 1;
  while (i < size && o[i] == n[i]) i++;
  if (i >= size) return 0;
  *a = (uint16_t)i;
  end = i + 1u;
  while (more) {
    more = 0;
    lim = end + JRN_MERGE_GAP < size ? end + JRN_MERGE_GAP : size;
    for (k = end; k < lim; k++)
      if (o[k] != n[k]) { end = k + 1u; more = 1; break; }
  }
  *b = (uint16_t)end;
  return 1;
}

int jrn_step_region(Jrn* j, uint8_t region, const uint8_t* old_blk, const uint8_t* new_blk) {
  uint16_t from = 0, a, b, need;
  if (!j || !old_blk || !new_blk || region >= j->nreg) return JRN_E_ARG;
  if (!j->bld_len) return JRN_E_STATE;
  if (jrn_crc32_update(0, old_blk, j->reg_size) != j->crc[region]) { jrn_step_abort(j); return JRN_E_DIVERGED; }
  while (next_run(old_blk, new_blk, j->reg_size, from, &a, &b)) {
    need = (uint16_t)(JRN_SPAN_HDR + 2u * (uint32_t)(b - a));
    if (j->nspans >= 255u || (uint32_t)j->bld_len + need + 4u > JRN_REC_MAX) { jrn_step_abort(j); return JRN_E_TOOBIG; }
    if ((uint32_t)j->bld_len + need + 4u > (uint32_t)JRN_PEND_CAP - j->pend_len) {
      j->flush_wanted = 1;                  /* LOUD: never a silent overrun or a silent drop */
      jrn_step_abort(j);
      return j->pend_n ? JRN_E_FULL : JRN_E_TOOBIG;
    }
    span_put(j, region, a, (uint16_t)(b - a), old_blk + a, new_blk + a);
    from = b;
  }
  j->bcrc[region] = jrn_crc32_update(0, new_blk, j->reg_size);
  return JRN_OK;
}

int jrn_step_end(Jrn* j) {
  uint8_t* b;
  uint16_t len;
  if (!j) return JRN_E_ARG;
  if (!j->bld_len) return JRN_E_STATE;
  if (!j->nspans) { jrn_step_abort(j); return JRN_NOOP; }
  len = (uint16_t)(j->bld_len + 4u);
  if (len > JRN_REC_MAX || (uint32_t)j->pend_len + len > JRN_PEND_CAP) {   /* defensive: _region already checked */
    j->flush_wanted = 1; jrn_step_abort(j); return JRN_E_FULL;
  }
  b = j->pend + j->pend_len;
  jrn_wr16(b + 4, len);
  b[7] = j->nspans;
  jrn_wr32(b + 24, hash_of(j->bcrc, j->nreg));
  jrn_wr32(b + j->bld_len, jrn_crc32_update(0, b, j->bld_len));
  j->pend_len = (uint16_t)(j->pend_len + len);
  j->pend_n++;
  j->cursor = j->next_seq;
  j->tip = j->next_seq;
  j->offer = 0;
  j->next_seq++;
  memcpy(j->crc, j->bcrc, sizeof j->crc);
  j->bld_len = 0; j->nspans = 0;
  if ((uint32_t)JRN_PEND_CAP - j->pend_len < JRN_FLUSH_HEADROOM) j->flush_wanted = 1;
  return JRN_OK;
}

/* ---- markers ---------------------------------------------------------------------------------------------- */
static int trailing_cursor_marker(const Jrn* j, uint16_t* start) {
  JrnRec r;
  return jrn_i_pend_last(j, start, &r) == 0 && r.kind == JRN_KIND_CURSOR;
}

int jrn_i_marker_room(const Jrn* j) {
  uint16_t s;
  if (j->bld_len) return 0;
  if (trailing_cursor_marker(j, &s)) return 1;
  return j->pend_n < JRN_PEND_MAXN && (uint32_t)JRN_PEND_CAP - j->pend_len >= JRN_REC_MIN;
}

int jrn_i_marker(Jrn* j, uint8_t kind, uint32_t parent, uint32_t aux, uint32_t pre, uint32_t post, const char* name) {
  uint8_t* b;
  uint16_t s;
  uint32_t i;
  if (kind != JRN_KIND_CURSOR && kind != JRN_KIND_DISCARD) return JRN_E_ARG;
  if (kind == JRN_KIND_CURSOR && trailing_cursor_marker(j, &s)) jrn_i_pend_pop(j, s);   /* latest wins */
  if (j->pend_n >= JRN_PEND_MAXN || (uint32_t)JRN_PEND_CAP - j->pend_len < JRN_REC_MIN) { j->flush_wanted = 1; return JRN_E_FULL; }
  b = j->pend + j->pend_len;
  memset(b, 0, JRN_REC_MIN);
  jrn_wr32(b, JRN_REC_MAGIC);
  jrn_wr16(b + 4, JRN_REC_MIN);
  b[6] = (uint8_t)(kind << 4);
  jrn_wr32(b + 8, j->next_seq); jrn_wr32(b + 12, parent); jrn_wr32(b + 16, aux);
  jrn_wr32(b + 20, pre); jrn_wr32(b + 24, post);
  for (i = 0; i < JRN_NAME_LEN && name && name[i]; i++) b[28 + i] = (uint8_t)name[i];
  jrn_wr32(b + JRN_REC_HDR, jrn_crc32_update(0, b, JRN_REC_HDR));
  j->pend_len = (uint16_t)(j->pend_len + JRN_REC_MIN);
  j->pend_n++;
  j->next_seq++;
  if ((uint32_t)JRN_PEND_CAP - j->pend_len < JRN_FLUSH_HEADROOM) j->flush_wanted = 1;
  return JRN_OK;
}

int jrn_mark_discarded(Jrn* j) {
  int rc;
  uint32_t h;
  if (!j) return JRN_E_ARG;
  if (j->readonly) return JRN_E_RDONLY;
  if (j->stopped) return JRN_E_STOPPED;
  if (j->bld_len) return JRN_E_STATE;
  h = hash_of(j->crc, j->nreg);
  rc = jrn_i_marker(j, JRN_KIND_DISCARD, j->cursor, j->tip, h, h, "discarded");
  if (rc == JRN_OK) { j->tip = j->cursor; j->offer = 0; }
  return rc;
}

/* ---- flush: back to front, then verify ----------------------------------------------------------------------- */
typedef struct Plan { uint16_t seg[JRN_PEND_MAXN], start[JRN_PEND_MAXN], len[JRN_PEND_MAXN]; uint32_t off[JRN_PEND_MAXN]; uint16_t end_seg; uint32_t end_off; } Plan;

/* Assign every pending record a (segment, offset); a record never spans segments. */
static int flush_plan(const Jrn* j, Plan* p) {
  uint16_t seg = j->tail_seg, pos = 0;
  uint32_t off = j->tail_off;
  uint8_t i;
  JrnRec r;
  for (i = 0; i < j->pend_n; i++) {
    if (jrn_i_hdr_parse(j->pend + pos, &r)) return JRN_E_STATE;
    if (off + r.len > JRN_SEG_SIZE) {
      if (seg >= j->seg_last) return JRN_E_FULL;   /* the next segment was never pre-created */
      seg++; off = JRN_REC_BASE;
    }
    p->seg[i] = seg; p->off[i] = off; p->start[i] = pos; p->len[i] = r.len;
    off += r.len; pos = (uint16_t)(pos + r.len);
  }
  p->end_seg = seg; p->end_off = off;
  return pos == j->pend_len ? 0 : JRN_E_STATE;
}

int jrn_flush(Jrn* j) {
  Plan p;
  int i, rc;
  if (!j) return JRN_E_ARG;
  if (j->readonly) return JRN_E_RDONLY;
  if (j->stopped) return JRN_E_STOPPED;
  if (j->bld_len) return JRN_E_STATE;
  if (!j->pend_n) return JRN_OK;
  if (!j->tail_seg) return JRN_E_NOSEG;
  rc = flush_plan(j, &p);
  if (rc) return rc;
  for (i = (int)j->pend_n - 1; i >= 0; i--) {     /* the FIRST record is the commit point */
    rc = seg_write(j, p.seg[i], p.off[i], j->pend + p.start[i], p.len[i]);
    if (rc) return rc;
  }
  for (i = 0; i < (int)j->pend_n; i++) {
    rc = seg_verify(j, p.seg[i], p.off[i], j->pend + p.start[i], p.len[i]);
    if (rc == JRN_E_VERIFY) j->stopped = 1;
    if (rc) return rc;
  }
  j->tail_seg = p.end_seg; j->tail_off = p.end_off;
  j->pend_len = 0; j->pend_n = 0; j->flush_wanted = 0;
  return JRN_OK;
}
