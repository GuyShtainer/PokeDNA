#ifndef ART_CACHE_H
#define ART_CACHE_H

#include <stdint.h>
#include <stdbool.h>

#include "rom_map.h" /* RomCtx, RomReadFn, RomKind */

/*
 * The ROM-art cache manifest: /PokeDNA/art/art.idx — see
 * docs/analysis-2026-08-19-rom-art/DESIGN.md Sec 2.2 for the design. This file is the
 * PURE-C core (no tonc, no FatFs): every multi-byte field is packed/unpacked
 * explicitly, little-endian, so the on-disk layout is identical whether it was written
 * by the GBA build or a host test, and is independent of any C struct's compiler-
 * chosen padding (rule 5's "explicit little-endian helpers", the same posture
 * rom_mon.c's rd32le takes).
 *
 * THE INVARIANT THIS FILE EXISTS TO HOLD: an interrupted extraction must never look
 * complete. art.idx is the ONLY thing the loader trusts, it is written LAST (after
 * every kind file it describes already exists, complete and verified on the card),
 * and art_idx_parse() below refuses anything shorter than the header+rows it itself
 * promises, so a cut-off write reads as ABSENT, never as a damaged-but-present cache.
 * There is no code path here that returns "ok" for a truncated or half-written
 * manifest — see tests/host_artcache_test.c (this file's own parse/pack proof) and
 * tests/host_artsession_test.c (drives art_session_kind_ready against the REAL
 * FatFs, including a truncated/absent/wrong-size/payload-disagrees-with-index
 * art.idx) for the proof.
 *
 * A NOTE ON A NUMBER IN THE DESIGN DOC. DESIGN.md Sec 2.2 lists the ArtIdxHead fields
 * as (in order) magic[8], format(u16), builder(u16), rom_code[4], rom_rev(u8),
 * rom_kind(u8), kinds(u8), pad(u8), rom_bytes(u32), rom_fnv(u32) — which sums to 28
 * bytes — but its own comment labels the struct "32 B", and the Sec 2.3 size table
 * computes art.idx's total as "32 + 8*16 = 160 B". The two numbers in the same
 * document disagree. Rather than silently pick one, this format reserves 4 trailing bytes
 * (`reserved`, always written 0, ignored on read) so the ON-DISK header is exactly the
 * 32 B the doc's own size table depends on, while every named field keeps the exact
 * semantics Sec 2.2 describes. This is flagged in the phase report, not just here.
 */

#define ART_IDX_HEAD_BYTES 32u
#define ART_IDX_KIND_BYTES 16u

#define ART_IDX_FORMAT_V1 1u

/* Hard rule 9 (one folder per tool): every ROM-art file lives under /PokeDNA/art/. */
#define ART_DIR "/PokeDNA/art"
#define ART_IDX_PATH ART_DIR "/art.idx"

/* Every kind the FINISHED design names (DESIGN.md Sec 2.1). Phase 2 extracts and
 * serves ONLY ART_KIND_ICONS — the rest exist here so `kinds` bitmask, the filename
 * table, and art.idx's row order are all settled once, without a format bump when a
 * later phase adds a row. A future phase's row simply starts appearing in `kinds`. */
typedef enum {
  ART_KIND_ICONS = 0,
  ART_KIND_WALLPAPER,
  ART_KIND_GLOVE,
  ART_KIND_CARD,
  ART_KIND_BAG,
  ART_KIND_POKEBLOCK,
  ART_KIND_ITEMS,
  ART_KIND_TYPES,
  ART_KIND_COUNT
} ArtKind;

/* "/PokeDNA/art/<name>" for kind k, or NULL for an out-of-range k. */
const char* art_kind_filename(ArtKind k);

typedef struct {
  uint16_t format; /* cache layout version — bump on ANY layout change              */
  uint16_t builder; /* PokeDNA build id that wrote it (0 if the build has none)      */
  char rom_code[4]; /* RomCtx.code, e.g. "BPEE" — 4 raw chars, NOT NUL-terminated    */
  uint8_t rom_rev; /* RomCtx.version, header byte 0xBC                              */
  uint8_t rom_kind; /* RomKind, stored as a byte                                    */
  uint8_t kinds; /* bitmask of ArtKind bits actually present                        */
  uint32_t rom_bytes; /* the source ROM's file size                                 */
  uint32_t rom_fnv; /* FNV-1a over 16 sampled 4 KiB ROM windows                     */
} ArtIdxHead;

typedef struct {
  uint8_t kind; /* ArtKind                                                         */
  uint32_t bytes; /* the kind file's exact length                                  */
  uint32_t fnv; /* FNV-1a over every byte of it                                    */
  uint32_t entries; /* rows/species/wallpapers — whatever indexes it                */
} ArtIdxKindRow;

/* ---- FNV-1a (32-bit, the standard offset basis / prime) ------------------------- */
#define ART_FNV1A_INIT 0x811c9dc5u
uint32_t art_fnv1a(uint32_t seed, const void* data, uint32_t len);

/* FNV-1a over 16 sampled 4 KiB windows spread evenly across the ROM — the same idea
 * tools/stamp_rom_windows.py + pdna_romver.c use for sampled-window stamping (a
 * fraction of a second to compute, catches "wrong game" AND "same game, different
 * dump" without ever hashing the full 16 MB image). Pure C; `read` is a RomReadFn so
 * this runs identically on the GBA and in a host test, and the extractor and the
 * loader call the SAME function, so they can never disagree about what a ROM hashes
 * to. Returns false (leaving *out unset) if any window's read fails or rom_size is
 * too small to hold even one 4 KiB window — callers must treat that as "cannot
 * stamp/verify this ROM", never as a hash of zero. */
bool art_rom_fnv(RomReadFn read, void* ctx, uint32_t rom_size, uint32_t* out);

/* Pack/unpack exactly ART_IDX_HEAD_BYTES / ART_IDX_KIND_BYTES bytes, little-endian. */
void art_idx_head_write(const ArtIdxHead* h, uint8_t out[ART_IDX_HEAD_BYTES]);
/* Unpacks unconditionally (no magic check — see art_idx_parse for the checked path). */
void art_idx_head_read(const uint8_t buf[ART_IDX_HEAD_BYTES], ArtIdxHead* out);
void art_idx_kind_write(const ArtIdxKindRow* r, uint8_t out[ART_IDX_KIND_BYTES]);
void art_idx_kind_read(const uint8_t buf[ART_IDX_KIND_BYTES], ArtIdxKindRow* out);

typedef enum {
  ART_IDX_OK = 0,
  ART_IDX_ABSENT,    /* caller passed len==0 / no bytes at all — cache does not exist */
  ART_IDX_TRUNCATED, /* shorter than header + kinds*16 rows it itself promises        */
  ART_IDX_BAD_MAGIC, /* not a PokeDNA art.idx                                        */
  ART_IDX_BAD_FORMAT /* format version this build does not understand                */
} ArtIdxParseStatus;

/* Parse a whole art.idx buffer already in RAM (header immediately followed by
 * popcount(kinds) kind rows, in ascending ArtKind order) and validate its own internal
 * shape — magic, format, and that `len` actually holds every row `kinds` promises.
 * Does NOT check it against a RomCtx (see art_idx_matches_rom for that). `len` MUST be
 * the file's real size on disk (f_size), not a buffer capacity: a file cut off mid-
 * write is exactly `len` short of its promise and this is the check that catches it
 * BEFORE a single kind row is trusted. out_rows must hold ART_KIND_COUNT entries;
 * *out_nrows is how many were actually parsed (0 on any non-OK status). */
ArtIdxParseStatus art_idx_parse(const uint8_t* buf, uint32_t len, ArtIdxHead* out_head,
                                 ArtIdxKindRow out_rows[ART_KIND_COUNT], int* out_nrows);

/* Does a parsed header match THIS rom (rule: a cache from the wrong ROM is never
 * served)? code + rev + kind + bytes + fnv must all agree with rc / rom_fnv. */
bool art_idx_matches_rom(const ArtIdxHead* h, const RomCtx* rc, uint32_t rom_fnv);

/* Find kind k's row, or NULL if its `kinds` bit was clear. */
const ArtIdxKindRow* art_idx_find(const ArtIdxKindRow rows[ART_KIND_COUNT], int nrows,
                                   ArtKind k);

/* Streaming FNV-1a over a whole on-card file, for the "verify a kind file's stored
 * FNV once per session, not per draw" check (DESIGN.md Sec 2.2 point 4). Generic over
 * any (ctx, off, dst, len)->bool reader so it is the same code whether the reader is
 * FatFs (GBA / tests/hostfat) or fread (a plain host test). scratch/chunk let the
 * caller bound the RAM this uses — pass a slice of an already-existing buffer (e.g.
 * mon_decomp), never a new static one. Returns false on any read failure. */
typedef bool (*ArtReadFn)(void* ctx, uint32_t off, void* dst, uint32_t len);
bool art_fnv_of_stream(ArtReadFn read, void* ctx, uint32_t len, uint8_t* scratch,
                        uint32_t chunk, uint32_t* out);

#endif /* ART_CACHE_H */
