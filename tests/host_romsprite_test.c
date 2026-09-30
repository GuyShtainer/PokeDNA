/* Host (PC) test for rom_sprite — Pokemon front/back battle sprites and their
 * normal/shiny palettes read from REAL retail ROMs. All of rom_sprite's I/O goes
 * through the RomCtx callback, so the code under test is byte-for-byte the code
 * that runs on the GBA (fused today, FatFs later).
 *
 * ROMs are the user's own dumps, never part of the repo; missing ROMs SKIP.
 *
 * Build + run (from the repo root):
 *   cc -std=c11 -I source tests/host_romsprite_test.c source/rom_sprite.c \
 *      source/map_render.c source/rom_map.c -o /tmp/hrms && /tmp/hrms
 *
 * What it proves:
 *   1) the GF header parses on Emerald + FireRed + LeafGreen, Ruby/Sapphire open
 *      through their PINNED rows (#293: no header; the 440-row tag shape is proven at
 *      open and mutants — a wrong tag, a wild pointer, an unpinned revision — fail
 *      CLOSED), and a header whose table pointers do not fit inside the image fails;
 *   2) all 440 front and all 440 back entries decompress to EXACTLY the byte count
 *      the geometry implies, per game: Emerald fronts are 2-frame (4096), FRLG
 *      fronts and all backs are 1-frame (2048), Castform is 4 formes (8192) and
 *      Deoxys is 2 (4096) — and NO read ever touches a byte outside the image;
 *   3) every palette is 16 RGB15 entries with bit 15 clear, and shiny differs from
 *      normal for every real species (and is deliberately identical for the 25
 *      unused internal ids and the Egg);
 *   4) the forme axis: 28 Unown letters, the Egg, Castform's four formes each with
 *      their OWN palette, and Deoxys — frame 0 Normal everywhere, frame 1 this
 *      cart's own forme, form_exact = 0 when the cart cannot show the one asked for;
 *   5) rom_sprite_to_rgb15's in-place expansion matches an independent
 *      out-of-place reference, on real sprite data, for every frame index;
 *   6) a truncated dump is rejected instead of read off the end, and a read that
 *      silently returns valid-looking WRONG bytes is caught by the verification
 *      (and sails through when verification is switched off — proving it is the
 *      verification doing the catching).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "rom_map.h"
#include "rom_sprite.h"

static int fails = 0, checks = 0;
static void chk(const char* rom, const char* what, int cond) {
  checks++;
  if (!cond) { printf("FAIL [%s] %s\n", rom, what); fails++; }
}

/* ---- readers ---------------------------------------------------------------
 * The file reader records the highest byte any caller touched and refuses reads
 * past `limit`, so "no read outside the image" is enforced, not just hoped for. */
typedef struct {
  FILE*    f;
  uint32_t limit;        /* the size rom_open was told about        */
  uint32_t high;         /* highest off+len actually requested      */
  uint32_t over;         /* reads that ran past `limit`             */
  long     calls;
  long     slot_collisions; /* two corruption slots armed on one call (b165 F3) */
  unsigned long bytes;   /* step 1 (BACKLOG #103): total bytes requested */
  /* BACKLOG #103 step 4: corrupt exactly ONE physical read call, by its 1-based
   * call index, XOR-ing one byte of ITS OWN returned buffer. This models the
   * realistic EZ-Flash failure mode ("this ONE read succeeded but the bytes
   * are garbage", icon_store.h's "PAYLOAD VERIFY STAYS") at the granularity
   * the new verify-by-consumed-input-hash design actually operates at: each
   * read() invocation is its own independent opportunity for corruption,
   * whether it happens during the decode or during the raw re-read that
   * verifies it. (The old per-decode-attempt address-redirect trick this
   * replaced relied on every attempt starting with its own fresh 4-byte
   * header peek to "re-roll" a toggle -- true for the old double-full-decode
   * verify, no longer true now that the re-read is a single raw byte fetch,
   * not a second full decode; see the step-4 commit for the analysis. This
   * mock does NOT key on len == 4, i.e. it does not care what shape a call
   * has -- only which ordinal call it is, exactly as review-opus's own
   * attack list demands.) Fires exactly once (hit_call == 0 disarms it). */
  long     hit_call;
  uint32_t hit_pos;
  uint8_t  hit_call_xor;  /* 0 (the memset default) means "use 0xFF", the value
                           * every pre-BACKLOG#144 case relied on implicitly */
  /* BACKLOG #165 cases (e)/(f): decode_verified's own-header retry needs MORE
   * than one physical read corrupted within the SAME rom_sprite_pic() call --
   * attempt 0's header AND attempt 1's header (case e), or all three attempts'
   * headers (case f). Two more independent single-shot slots, same fire-once
   * semantics as hit_call/hit_pos/hit_call_xor above. */
  long     hit_call_b, hit_call_c;
  uint32_t hit_pos_b,  hit_pos_c;
  uint8_t  hit_call_xor_b, hit_call_xor_c;
  /* the same trick one level up: make a TABLE ENTRY read return a different (but
   * perfectly valid) neighbouring row on alternate reads. A garbled pointer that
   * still lands on a real sprite decodes identically every time, so only a
   * verified entry read can catch it. */
  uint32_t flip_off, flip_alt;
  int      flip_on, flip_armed;
} FileCtx;

static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FileCtx* fc = (FileCtx*)ctx;
  fc->calls++;
  fc->bytes += len;
  if (off + len > fc->high) fc->high = off + len;
  if (off > fc->limit || len > fc->limit - off) { fc->over++; return false; }
  if (fc->flip_armed && off == fc->flip_off) {
    fc->flip_on ^= 1;
    if (fc->flip_on) off = fc->flip_alt;
  }
  if (fseek(fc->f, (long)off, SEEK_SET) != 0) return false;
  if (fread(dst, 1, len, fc->f) != len) return false;
  /* A call consumed by one slot is ineligible for the others: two slots armed on
   * the same call would XOR the byte twice (= identity) and pass silently. */
  long fired = -1;
  if (fc->hit_call && fc->calls == fc->hit_call && fc->hit_pos < len) {
    ((uint8_t*)dst)[fc->hit_pos] ^= fc->hit_call_xor ? fc->hit_call_xor : 0xFF;
    fc->hit_call = 0;      /* fires exactly once */
    fc->hit_call_xor = 0;  /* back to the 0xFF default for the next case */
    fired = fc->calls;
  }
  if (fc->hit_call_b && fc->calls == fired && fc->calls == fc->hit_call_b) fc->slot_collisions++;
  if (fc->hit_call_b && fc->calls != fired && fc->calls == fc->hit_call_b && fc->hit_pos_b < len) {
    ((uint8_t*)dst)[fc->hit_pos_b] ^= fc->hit_call_xor_b ? fc->hit_call_xor_b : 0xFF;
    fc->hit_call_b = 0;
    fc->hit_call_xor_b = 0;
    fired = fc->calls;
  }
  if (fc->hit_call_c && fc->calls == fired && fc->calls == fc->hit_call_c) fc->slot_collisions++;
  if (fc->hit_call_c && fc->calls != fired && fc->calls == fc->hit_call_c && fc->hit_pos_c < len) {
    ((uint8_t*)dst)[fc->hit_pos_c] ^= fc->hit_call_xor_c ? fc->hit_call_xor_c : 0xFF;
    fc->hit_call_c = 0;
    fc->hit_call_xor_c = 0;
  }
  return true;
}

/* an in-memory image, for the synthesised bad-header cases */
typedef struct { const uint8_t* p; uint32_t n; } MemCtx;
static bool mem_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  MemCtx* m = (MemCtx*)ctx;
  if (off > m->n || len > m->n - off) return false;
  memcpy(dst, m->p + off, len);
  return true;
}

/* ---- reference 4bpp -> RGB15, deliberately written the naive out-of-place way
 * so it shares no code (and no aliasing assumptions) with the module. -------- */
static void ref_expand(const uint8_t* frame2048, const uint16_t pal[16], uint16_t out[4096]) {
  for (int t = 0; t < 64; t++) {
    int tx = (t % 8) * 8, ty = (t / 8) * 8;
    for (int b = 0; b < 32; b++) {
      uint8_t v = frame2048[t * 32 + b];
      int py = ty + b / 4, px = tx + (b % 4) * 2;
      uint8_t idx[2] = { (uint8_t)(v & 0xF), (uint8_t)(v >> 4) };
      for (int k = 0; k < 2; k++)
        out[py * 64 + px + k] = idx[k] ? (uint16_t)(0x8000u | (pal[idx[k]] & 0x7FFF)) : 0;
    }
  }
}

/* ---- expected decompressed size, per game and side ------------------------- */
static uint32_t expect_bytes(RomKind k, RomSpriteSide side, int ts) {
  if (ts == ROM_SPRITE_CASTFORM) return 8192;                 /* 4 formes, always */
  if (ts == ROM_SPRITE_DEOXYS)   return (k == ROM_RUBY || k == ROM_SAPPHIRE) ? 2048 : 4096; /* R/S: Normal only */
  if (side == ROM_SPRITE_FRONT && k == ROM_EMERALD) return 4096;  /* anim_front    */
  return 2048;
}

static int all_zero(const uint8_t* p, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) if (p[i]) return 0;
  return 1;
}

/* ---------------------------------------------------------------------------- */
static void run_rom(const char* path, const char* name, int expect_header) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("SKIP %s (no %s)\n", name, path); return; }
  fseek(f, 0, SEEK_END); long sz = ftell(f);
  FileCtx fc; memset(&fc, 0, sizeof fc);
  fc.f = f; fc.limit = (uint32_t)sz;

  RomCtx rc;
  if (!rom_open(&rc, file_read, &fc, (uint32_t)sz)) {
    chk(name, "rom_open accepts the retail dump", 0);
    fclose(f); return;
  }

  RomSprite rs;
  int ok = rom_sprite_open(&rs, &rc);
  chk(name, expect_header ? "GF header parses" : "fails CLOSED (no GF header)", ok == expect_header);
  if (!ok) {
    printf("  %s: fails closed as designed (kind=%s rev=%u, no GF header)\n",
           name, rom_kind_name(rc.kind), rc.version);
    fclose(f); return;
  }
  chk(name, "verification defaults ON", rs.verify == 1);

  /* Step 1 (BACKLOG #103): baseline read-callback counts for exactly ONE
   * front decode (Bulbasaur), measured only on Emerald. */
  if (strcmp(name, "Emerald") == 0) {
    static uint8_t mpic[ROM_SPRITE_BUF_BYTES];
    RomSpritePic minfo;
    long calls0 = fc.calls; unsigned long bytes0 = fc.bytes;
    int mok = rom_sprite_pic(&rs, ROM_SPRITE_FRONT, 1, 0, mpic, sizeof mpic, &minfo);
    printf("counts Emerald sprite front: %ld reads / %lu bytes (ok=%d)\n",
           fc.calls - calls0, fc.bytes - bytes0, mok);
  }

  static uint8_t pic[ROM_SPRITE_BUF_BYTES];
  RomSpritePic info;

  /* 2) every entry, both sides, exact byte count, nothing blank */
  int bad = 0, wrong = 0, blanks = 0, first_bad = -1;
  for (int side = 0; side < 2; side++) {
    for (int ts = 0; ts < ROM_SPRITE_ENTRIES; ts++) {
      uint16_t sp = (uint16_t)ts; uint8_t form = 0;
      if (ts >= 413) { sp = ROM_SPRITE_UNOWN; form = (uint8_t)(ts - 413 + 1); }
      if (!rom_sprite_pic(&rs, (RomSpriteSide)side, sp, form, pic, sizeof pic, &info)) {
        bad++; if (first_bad < 0) first_bad = ts; continue;
      }
      if (info.bytes != expect_bytes(rc.kind, (RomSpriteSide)side, ts)) {
        wrong++; if (first_bad < 0) first_bad = ts;
      }
      if (all_zero(pic, info.bytes)) blanks++;
    }
  }
  chk(name, "all 880 front+back entries decode", bad == 0);
  chk(name, "every decode is exactly the expected byte count", wrong == 0);
  chk(name, "no picture is blank", blanks == 0);
  if (bad || wrong || blanks)
    printf("  [%s] bad=%d wrong=%d blank=%d first=%d\n", name, bad, wrong, blanks, first_bad);

  chk(name, "no read touched a byte outside the image", fc.over == 0 && fc.high <= (uint32_t)sz);

  /* the shape claims the header comment makes, spelled out */
  chk(name, "Bulbasaur front frames match the game",
      rom_sprite_pic(&rs, ROM_SPRITE_FRONT, 1, 0, pic, sizeof pic, &info) &&
      info.frames == (rc.kind == ROM_EMERALD ? 2 : 1) &&
      info.kind == (rc.kind == ROM_EMERALD ? ROM_SPRITE_ANIM : ROM_SPRITE_SINGLE));
  if (rc.kind == ROM_EMERALD)
    chk(name, "the two Emerald front frames really differ",
        memcmp(pic, pic + 2048, 2048) != 0);
  chk(name, "Bulbasaur back is a single frame",
      rom_sprite_pic(&rs, ROM_SPRITE_BACK, 1, 0, pic, sizeof pic, &info) &&
      info.frames == 1 && info.kind == ROM_SPRITE_SINGLE);

  /* 3) palettes */
  int palbad = 0, palflat = 0, palbit15 = 0, shinysame = 0, unusedsame = 0;
  for (int ts = 0; ts < ROM_SPRITE_ENTRIES; ts++) {
    uint16_t sp = (uint16_t)ts; uint8_t form = 0;
    if (ts >= 413) { sp = ROM_SPRITE_UNOWN; form = (uint8_t)(ts - 413 + 1); }
    uint16_t np[16], shp[16];
    if (!rom_sprite_pal(&rs, sp, form, 0, np) || !rom_sprite_pal(&rs, sp, form, 1, shp)) { palbad++; continue; }
    for (int i = 0; i < 16; i++) if (np[i] & 0x8000) palbit15++;
    int distinct = 0;
    for (int i = 1; i < 16; i++) if (np[i] != np[0]) { distinct = 1; break; }
    if (!distinct) palflat++;
    int identical = memcmp(np, shp, sizeof np) == 0;
    /* the 25 unused internal ids and the Egg legitimately share one palette */
    int unused = (ts >= 252 && ts <= 276) || ts == ROM_SPRITE_EGG;
    if (identical && !unused) shinysame++;
    if (identical && unused) unusedsame++;
  }
  chk(name, "all 440 normal + shiny palettes read", palbad == 0);
  chk(name, "no palette is a flat single colour", palflat == 0);
  chk(name, "every palette entry has bit 15 clear (real RGB15)", palbit15 == 0);
  chk(name, "shiny differs from normal for every real species", shinysame == 0);
  chk(name, "the 25 unused ids + the Egg share their shiny (expected)", unusedsame == 26);
  if (palbad || palflat || palbit15 || shinysame || unusedsame != 26)
    printf("  [%s] palbad=%d flat=%d bit15=%d shinysame=%d unusedsame=%d\n",
           name, palbad, palflat, palbit15, shinysame, unusedsame);

  /* 4) the forme axis */
  static uint8_t a[ROM_SPRITE_BUF_BYTES], b[ROM_SPRITE_BUF_BYTES];
  RomSpritePic ia, ib;
  chk(name, "Unown A (form 0) decodes", rom_sprite_pic(&rs, ROM_SPRITE_FRONT, 201, 0, a, sizeof a, &ia));
  chk(name, "Unown B (form 1) decodes", rom_sprite_pic(&rs, ROM_SPRITE_FRONT, 201, 1, b, sizeof b, &ib));
  chk(name, "Unown A != Unown B", memcmp(a, b, 2048) != 0);
  chk(name, "Unown '?' (form 27) decodes", rom_sprite_pic(&rs, ROM_SPRITE_FRONT, 201, 27, a, sizeof a, &ia));
  chk(name, "Unown form 28 rejected", !rom_sprite_pic(&rs, ROM_SPRITE_FRONT, 201, 28, a, sizeof a, &ia));
  chk(name, "the Egg (412) decodes", rom_sprite_pic(&rs, ROM_SPRITE_FRONT, 412, 0, a, sizeof a, &ia));
  chk(name, "species 440 rejected", !rom_sprite_pic(&rs, ROM_SPRITE_FRONT, 440, 0, a, sizeof a, &ia));
  chk(name, "a form on a species with no forme axis is rejected",
      !rom_sprite_pic(&rs, ROM_SPRITE_FRONT, 1, 1, a, sizeof a, &ia));
  const int is_rs = (rc.kind == ROM_RUBY || rc.kind == ROM_SAPPHIRE);
  chk(name, "a 2048-byte buffer fails rather than truncating a 4096 pic",
      !rom_sprite_pic(&rs, ROM_SPRITE_FRONT, ROM_SPRITE_CASTFORM, 0, a, 2048, &ia));
  if (!is_rs)
    chk(name, "a 2048-byte buffer fails rather than truncating Deoxys' 4096",
        !rom_sprite_pic(&rs, ROM_SPRITE_FRONT, ROM_SPRITE_DEOXYS, 0, a, 2048, &ia));

  /* Castform: four formes, four palettes, frame index == palette index */
  chk(name, "Castform is 4 formes", rom_sprite_pic(&rs, ROM_SPRITE_FRONT, ROM_SPRITE_CASTFORM, 0, a, sizeof a, &ia) &&
      ia.frames == 4 && ia.kind == ROM_SPRITE_FORME && ia.frame == 0 && ia.form_exact);
  for (int fm = 1; fm < 4; fm++) {
    char w[64]; sprintf(w, "Castform forme %d selects frame %d", fm, fm);
    chk(name, w, rom_sprite_pic(&rs, ROM_SPRITE_FRONT, ROM_SPRITE_CASTFORM, (uint8_t)fm, a, sizeof a, &ia) &&
        ia.frame == fm && ia.form_exact);
    uint16_t p0[16], pn[16];
    sprintf(w, "Castform forme %d has its OWN palette", fm);
    chk(name, w, rom_sprite_pal(&rs, ROM_SPRITE_CASTFORM, 0, 0, p0) &&
        rom_sprite_pal(&rs, ROM_SPRITE_CASTFORM, (uint8_t)fm, 0, pn) &&
        memcmp(p0, pn, sizeof p0) != 0);
  }
  chk(name, "Castform forme 4 rejected",
      !rom_sprite_pic(&rs, ROM_SPRITE_FRONT, ROM_SPRITE_CASTFORM, 4, a, sizeof a, &ia));

  /* Deoxys: frame 0 Normal in every cart, frame 1 this cart's own forme */
  int own = rom_sprite_deoxys_forme(&rs);
  int want = (rc.kind == ROM_FIRERED) ? 1 : (rc.kind == ROM_LEAFGREEN) ? 2
           : (rc.kind == ROM_EMERALD) ? 3 : 0;
  chk(name, "this cart's Deoxys forme is the right one", own == want);
  chk(name, "Deoxys Normal = frame 0, exact",
      rom_sprite_pic(&rs, ROM_SPRITE_FRONT, ROM_SPRITE_DEOXYS, 0, a, sizeof a, &ia) &&
      ia.frames == (is_rs ? 1 : 2) && ia.kind == ROM_SPRITE_FORME && ia.frame == 0 && ia.form_exact);
  if (!is_rs)   /* R/S carry Normal only: there is no own-forme frame to select */
    chk(name, "Deoxys own forme = frame 1, exact",
        rom_sprite_pic(&rs, ROM_SPRITE_FRONT, ROM_SPRITE_DEOXYS, (uint8_t)own, a, sizeof a, &ia) &&
        ia.frame == 1 && ia.form_exact);
  for (int fm = 1; fm <= 3; fm++) {
    if (fm == own) continue;
    char w[72]; sprintf(w, "Deoxys forme %d absent here -> Normal, form_exact 0", fm);
    chk(name, w, rom_sprite_pic(&rs, ROM_SPRITE_FRONT, ROM_SPRITE_DEOXYS, (uint8_t)fm, a, sizeof a, &ia) &&
        ia.frame == 0 && ia.form_exact == 0);
  }
  chk(name, "Deoxys forme 4 rejected",
      !rom_sprite_pic(&rs, ROM_SPRITE_FRONT, ROM_SPRITE_DEOXYS, 4, a, sizeof a, &ia));

  /* 5) the in-place RGB15 expansion, against the naive reference, every frame */
  {
    static uint16_t ref[ROM_SPRITE_PIXELS];
    int mism = 0, opaque_total = 0;
    const uint16_t probe[6] = { 1, 25, 201, ROM_SPRITE_EGG, ROM_SPRITE_DEOXYS, ROM_SPRITE_CASTFORM };
    for (int pi = 0; pi < 6; pi++) {
      for (int shiny = 0; shiny < 2; shiny++) {
        if (!rom_sprite_pic(&rs, ROM_SPRITE_FRONT, probe[pi], 0, a, sizeof a, &ia)) { mism++; continue; }
        for (int fr = 0; fr < ia.frames; fr++) {
          uint16_t pal[16];
          uint8_t palform = (probe[pi] == ROM_SPRITE_CASTFORM) ? (uint8_t)fr : 0;
          if (!rom_sprite_pal(&rs, probe[pi], palform, shiny, pal)) { mism++; continue; }
          memcpy(b, a, ia.bytes);                      /* keep the raw 4bpp */
          ref_expand(b + fr * 2048, pal, ref);
          rom_sprite_to_rgb15(b, ROM_SPRITE_BUF_BYTES, (uint8_t)fr, pal);  /* destroys b in place */
          if (memcmp(b, ref, sizeof ref) != 0) mism++;
          for (int i = 0; i < ROM_SPRITE_PIXELS; i++) if (ref[i] & 0x8000) opaque_total++;
        }
      }
    }
    chk(name, "in-place rom_sprite_to_rgb15 == out-of-place reference, every frame", mism == 0);
    /* It always writes 8192 B, so a caller who sized for a single 2048-byte frame
     * (which rom_sprite_pic's contract permits) must be REFUSED, not overrun. */
    { static uint8_t small[2048]; uint16_t p16[16] = {0};
      chk(name, "to_rgb15 refuses a 2048-byte buffer instead of overrunning it",
          rom_sprite_to_rgb15(small, sizeof small, 0, p16) == 0); }
    chk(name, "the expansion produced real opaque pixels", opaque_total > 5000);
  }

  /* 6a) a truncated dump: the tables are fine but the pixels are gone */
  {
    FileCtx tc; memset(&tc, 0, sizeof tc);
    tc.f = f; tc.limit = 0x00400000;                   /* 4 MiB of a 16 MiB cart */
    RomCtx trc;
    /* rom_open itself may refuse the truncated image; if it does, that is already
     * fail-closed. If it accepts, rom_sprite must still refuse to read past it. */
    if (rom_open(&trc, file_read, &tc, tc.limit)) {
      RomSprite trs;
      if (rom_sprite_open(&trs, &trc)) {
        int decoded = 0;
        for (uint16_t sp = 1; sp <= 40; sp++)
          if (rom_sprite_pic(&trs, ROM_SPRITE_FRONT, sp, 0, a, sizeof a, &ia)) decoded++;
        chk(name, "truncated dump: no sprite decodes off the end", decoded == 0);
      }
      chk(name, "truncated dump: no read past the stated size", tc.over == 0);
    } else {
      chk(name, "truncated dump: rejected at rom_open (fail closed)", 1);
    }
  }

  /* 6b) reads that SUCCEED holding valid-but-wrong bytes -- BACKLOG #103 step 4's
   * new verify-by-consumed-input-hash design, attacked at the granularity it
   * actually operates at: corrupt exactly ONE physical read call, by its
   * ordinal position, and prove decode+reread still catches it. (The old
   * mechanism this replaced redirected an address range and flipped on every
   * read matching `len == 4` at that address, modelling "every FULL DECODE
   * gets its own fresh header peek" -- true for the old double-full-decode
   * verify, no longer true now that the verify pass is a single raw re-read,
   * not a second decode. This mock keys on nothing but ordinal call count,
   * exactly what review-opus's attack list demands: "the mock must NOT key
   * on len == 4".) Traced once with a plain debug counter for this exact
   * call shape (Emerald, species 1 front, verify ON): table-entry read x2
   * (verified) = calls 1-2, mr_lz77_size's window-sizing peek = call 3,
   * decode_verified's own header read = call 4, the body window chunk =
   * call 5, the verify re-read = call 6. With verify OFF the table entry is
   * read once (not twice), so the body chunk lands on call 4 instead. */
  {
    uint8_t clean[ROM_SPRITE_BUF_BYTES]; RomSpritePic ic;
    int okc = rom_sprite_pic(&rs, ROM_SPRITE_FRONT, 1, 0, clean, sizeof clean, &ic);
    chk(name, "clean reference decode for the corruption cases below succeeds", okc);

    /* (a) corrupt the DECODE's own compressed-input read (call base+5).
     * A retry must recover the exact clean pixels. */
    long base = fc.calls;
    fc.hit_call = base + 5; fc.hit_pos = 1; fc.hit_call_xor = 0;  /* reset at the arm
      site (BACKLOG #165): an armed-but-never-fired xor from an earlier case must not
      leak into this one -- do not rely solely on the fire branch's own reset */
    uint8_t dirty_dec[ROM_SPRITE_BUF_BYTES]; RomSpritePic id;
    int ok_dec = rom_sprite_pic(&rs, ROM_SPRITE_FRONT, 1, 0, dirty_dec, sizeof dirty_dec, &id);
    chk(name, "a decode-phase read corruption still yields the clean pixels (a retry recovers)",
        okc && ok_dec && id.bytes == ic.bytes && memcmp(dirty_dec, clean, ic.bytes) == 0);
    chk(name, "a decode-phase read corruption cost more than one attempt (it was not free)",
        (fc.calls - base) > 6);

    /* Same corruption point, verification OFF: with no reread to catch it,
     * the corrupted decode must sail through UNFIXED -- proving it is
     * verification, not luck, doing the catching above. Verify OFF reads
     * the table entry once (not twice), so the body chunk is call base+4. */
    RomSprite nv = rs; rom_sprite_set_verify(&nv, 0);
    base = fc.calls;
    fc.hit_call = base + 4; fc.hit_pos = 1; fc.hit_call_xor = 0;
    uint8_t dirty_nv[ROM_SPRITE_BUF_BYTES]; RomSpritePic idn;
    int ok_nv = rom_sprite_pic(&nv, ROM_SPRITE_FRONT, 1, 0, dirty_nv, sizeof dirty_nv, &idn);
    chk(name, "with verification OFF the same corrupted decode sails through unfixed "
              "(so it is verification doing the catching)",
        okc && ok_nv && (idn.bytes != ic.bytes || memcmp(dirty_nv, clean, ic.bytes) != 0));

    /* (b) corrupt ONLY the verify RE-READ (call base+6); the decode itself is
     * clean, so its consumed-input hash is correct -- only the confirmation
     * read sees garbage. This must NOT be silently accepted on the first
     * attempt: prove a retry happened (more calls than one clean attempt
     * costs), and that the final, accepted pixels are still the clean ones. */
    base = fc.calls;
    fc.hit_call = base + 6; fc.hit_pos = 1; fc.hit_call_xor = 0;
    uint8_t dirty_rr[ROM_SPRITE_BUF_BYTES]; RomSpritePic ir;
    int ok_rr = rom_sprite_pic(&rs, ROM_SPRITE_FRONT, 1, 0, dirty_rr, sizeof dirty_rr, &ir);
    chk(name, "a re-read-phase corruption is not silently accepted (a retry happened)",
        (fc.calls - base) > 6);
    chk(name, "a re-read-phase corruption still yields the clean pixels after the retry",
        okc && ok_rr && ir.bytes == ic.bytes && memcmp(dirty_rr, clean, ic.bytes) == 0);

    /* (c) F1 review fix, re-verified for BACKLOG #144: corrupt ONLY the SEPARATE
     * mr_lz77_size() window-sizing peek (call base+3), leaving decode_verified's
     * own header read (call base+4, every retry attempt) clean. `win` is computed
     * from the peek's `size` BEFORE the loop; if the peek disagrees with the
     * header the decode itself reads, `win = dst + size` can land inside
     * dst[0,n) -- the window aliases live output, and mr_hash_span()'s re-read
     * would silently overwrite decoded pixels while the hash still matches (the
     * two refuters' reproduction: a 3,072 vs 4,096 disagreement -> 953 dirty
     * bytes, accepted, if nothing guarded against it).
     *
     * BACKLOG #144: the ORIGINAL guard (`if (win_bytes && n != size) return 0;`)
     * refused outright on this disagreement, spending the whole 3-attempt retry
     * budget on what a b103 re-verify sweep proved is almost always a TRANSIENT
     * bad peek read, not a bad blob (945,540 corpus-wide peek-corruption cases:
     * 0 accepted-dirty before AND after this fix -- the guard's job was never in
     * doubt -- but refused-with-a-missing-sprite dropped 351,498 -> 5,993 once
     * the guard falls back to the safe fixed window instead of quitting). The
     * fix (`{ win = 0; win_bytes = 0; continue; }`) retries with mr_lz77_x's own
     * fixed 64 B stack window, which is never derived from `size` and never
     * overlaps `dst` -- aliasing is impossible regardless of what the header
     * reads next -- so this case now RECOVERS the clean pixels instead of
     * refusing. The property that must hold either way: a disagreement is
     * NEVER silently accepted with wrong pixels -- whatever comes back is
     * either a refusal or byte-identical to the clean reference.
     *
     * XOR 0x1C on the peek's MIDDLE size byte, hit_pos 2 (species 1 front: true
     * size 4096 = 0x001000 -> peeked 0x000C00 = 3072, a peek BELOW the real size
     * this time, still <= cap and cap-peek >= 256 so a real window gets computed
     * and used): the window at dst+3072 overlaps the decode's own dst[3072,4096)
     * -- the aliasing hazard the guard exists to catch, from the other side of
     * the size comparison than the original 0xFF-on-the-low-byte case above. */
    base = fc.calls;
    fc.hit_call = base + 3; fc.hit_pos = 2; fc.hit_call_xor = 0x1C;
    uint8_t dirty_pk[ROM_SPRITE_BUF_BYTES]; RomSpritePic ip;
    int ok_pk = rom_sprite_pic(&rs, ROM_SPRITE_FRONT, 1, 0, dirty_pk, sizeof dirty_pk, &ip);
    chk(name, "F1/BACKLOG#144: a size-peek/header disagreement RECOVERS the clean "
              "pixels via the fixed-window retry, never refused and never dirty",
        ok_pk && ip.bytes == ic.bytes && memcmp(dirty_pk, clean, ic.bytes) == 0);
    chk(name, "the size-peek disagreement cost a retry (the fallback actually ran)",
        (fc.calls - base) > 6);

    /* (d) BACKLOG #165: corrupt decode_verified's OWN header read on attempt 0
     * (call base+4, hit_pos 0, default xor 0xFF: the LZ10 type byte 0x10 ->
     * 0xEF, so lz77_run's own `h[0] != 0x10` check bails after exactly that
     * one read -- attempt 0 spends only this single call). Before this lane's
     * fix, `if (!n) return 0;` on attempt 0 was terminal: a transient bad read
     * of the decode's OWN header (not the separate size-peek case (c) covers)
     * spent the whole 3-attempt budget on nothing and the sprite went missing.
     * The fix retries with the fixed fallback window: attempt 1 re-reads a
     * clean header (the hit already fired once) and decodes normally. */
    base = fc.calls;
    fc.hit_call = base + 4; fc.hit_pos = 0; fc.hit_call_xor = 0xFF;
    uint8_t dirty_h0[ROM_SPRITE_BUF_BYTES]; RomSpritePic ih0;
    int ok_h0 = rom_sprite_pic(&rs, ROM_SPRITE_FRONT, 1, 0, dirty_h0, sizeof dirty_h0, &ih0);
    chk(name, "BACKLOG#165: a bad read of decode_verified's OWN header on attempt 0 "
              "RECOVERS the clean pixels via the attempt-1 retry",
        ok_h0 && ih0.bytes == ic.bytes && memcmp(dirty_h0, clean, ic.bytes) == 0);
    chk(name, "the own-header corruption on attempt 0 cost a retry (the fallback ran)",
        (fc.calls - base) > 6);

    /* (e) the same corruption on BOTH attempt 0 and attempt 1's own header
     * reads (call base+4, then call base+5 -- attempt 1's header read is the
     * very next call, since a header-corrupted attempt bails after exactly
     * one read). Attempt 2 gets a clean header (both hits already fired) and
     * must still recover the exact clean pixels. */
    base = fc.calls;
    fc.hit_call = base + 4; fc.hit_pos = 0; fc.hit_call_xor = 0xFF;
    fc.hit_call_b = base + 5; fc.hit_pos_b = 0; fc.hit_call_xor_b = 0xFF;
    uint8_t dirty_h01[ROM_SPRITE_BUF_BYTES]; RomSpritePic ih01;
    int ok_h01 = rom_sprite_pic(&rs, ROM_SPRITE_FRONT, 1, 0, dirty_h01, sizeof dirty_h01, &ih01);
    chk(name, "BACKLOG#165: own-header corruption on attempts 0 AND 1 still RECOVERS "
              "the clean pixels on attempt 2",
        ok_h01 && ih01.bytes == ic.bytes && memcmp(dirty_h01, clean, ic.bytes) == 0);
    chk(name, "corrupting two attempts' own header reads cost two retries",
        (fc.calls - base) > 7);

    /* (f) all three attempts' own header reads corrupted (calls base+4,
     * base+5, base+6): the 3-attempt budget is genuinely exhausted, so this
     * MUST be refused -- never a silently accepted wrong picture. */
    base = fc.calls;
    fc.hit_call = base + 4; fc.hit_pos = 0; fc.hit_call_xor = 0xFF;
    fc.hit_call_b = base + 5; fc.hit_pos_b = 0; fc.hit_call_xor_b = 0xFF;
    fc.hit_call_c = base + 6; fc.hit_pos_c = 0; fc.hit_call_xor_c = 0xFF;
    uint8_t dirty_h012[ROM_SPRITE_BUF_BYTES]; RomSpritePic ih012;
    memset(dirty_h012, 0xAA, sizeof dirty_h012);   /* poison: prove a refusal never writes pixels */
    int ok_h012 = rom_sprite_pic(&rs, ROM_SPRITE_FRONT, 1, 0, dirty_h012, sizeof dirty_h012, &ih012);
    chk(name, "BACKLOG#165: own-header corruption on all three attempts is REFUSED, "
              "never dirty",
        ok_h012 == 0);
    chk(name, "a refused decode never wrote clean-looking pixels into the caller's buffer",
        memcmp(dirty_h012, clean, ic.bytes) != 0);

    chk(name, "no two corruption slots were armed on the same read call "
              "(a collision would silently weaken the case above)", fc.slot_collisions == 0);
    fc.hit_call = 0; fc.hit_call_b = 0; fc.hit_call_c = 0;
  }

  /* 6c) the same hazard one level up: the 8-byte TABLE ENTRY read itself. */
  {
    fc.flip_off = rs.front + 1 * 8;  fc.flip_alt = rs.front + 2 * 8;
    fc.flip_on = 0; fc.flip_armed = 1;
    chk(name, "an unstable sheet-table entry read is rejected",
        !rom_sprite_pic(&rs, ROM_SPRITE_FRONT, 1, 0, a, sizeof a, &ia));
    fc.flip_off = rs.npal + 1 * 8;   fc.flip_alt = rs.npal + 2 * 8;
    fc.flip_on = 0;
    uint16_t pl[16];
    chk(name, "an unstable palette-table entry read is rejected",
        !rom_sprite_pal(&rs, 1, 0, 0, pl));

    RomSprite nv2 = rs; rom_sprite_set_verify(&nv2, 0);
    fc.flip_off = rs.front + 1 * 8;  fc.flip_alt = rs.front + 2 * 8;
    fc.flip_on = 0;
    chk(name, "with verification OFF the unstable entry is trusted",
        rom_sprite_pic(&nv2, ROM_SPRITE_FRONT, 1, 0, a, sizeof a, &ia));
    fc.flip_armed = 0;
  }

  printf("  %s: ok (kind=%s rev=%u, %ld reads)\n", name, rom_kind_name(rc.kind), rc.version, fc.calls);
  fclose(f);
}

/* ---- the in-place expansion, on data designed to break it ------------------
 * rom_sprite_to_rgb15 expands over the buffer it is reading from. The one place
 * the write overtakes the read is the very last source byte — the sprite's
 * bottom-right two pixels — and MEASURED over Emerald's 440 fronts that byte is
 * transparent in every single one, so no retail sprite can catch the bug. This
 * runs the expansion on fully-opaque synthetic data, where every index (including
 * that last byte) is non-zero, for all four frame positions. No ROM needed. */
static void run_expand_selftest(void) {
  static uint8_t buf[ROM_SPRITE_BUF_BYTES], raw[ROM_SPRITE_BUF_BYTES];
  static uint16_t ref[ROM_SPRITE_PIXELS];
  uint16_t pal[16];
  for (int i = 0; i < 16; i++) pal[i] = (uint16_t)(0x0421 * (i + 1) + i);   /* bit15 clear */

  for (uint32_t i = 0; i < ROM_SPRITE_BUF_BYTES; i++) {
    uint8_t lo = (uint8_t)((i * 7u + 1u) % 15u + 1u);      /* 1..15, never index 0 */
    uint8_t hi = (uint8_t)((i * 11u + 5u) % 15u + 1u);
    raw[i] = (uint8_t)(lo | (hi << 4));
  }
  int mism = 0;
  for (int fr = 0; fr < ROM_SPRITE_MAX_FRAMES; fr++) {
    memcpy(buf, raw, sizeof buf);
    ref_expand(raw + fr * 2048, pal, ref);
    rom_sprite_to_rgb15(buf, ROM_SPRITE_BUF_BYTES, (uint8_t)fr, pal);
    if (memcmp(buf, ref, sizeof ref) != 0) mism++;
  }
  chk("expand", "fully-opaque synthetic frames survive the in-place aliasing", mism == 0);

  /* the exact touching case on its own: only the last source byte is opaque */
  memset(raw, 0, sizeof raw);
  raw[2047] = 0x21;                                        /* bottom-right 2 pixels */
  memcpy(buf, raw, sizeof buf);
  ref_expand(raw, pal, ref);
  rom_sprite_to_rgb15(buf, ROM_SPRITE_BUF_BYTES, 0, pal);
  chk("expand", "the last source byte (bottom-right pixels) survives being written over",
      memcmp(buf, ref, sizeof ref) == 0);
  chk("expand", "that case is a real signal, not a pair of zeroes",
      (ref[4094] & 0x8000) && (ref[4095] & 0x8000));

  /* frame index out of range must be a no-op, not a wild memmove */
  memcpy(buf, raw, sizeof buf);
  rom_sprite_to_rgb15(buf, sizeof buf, ROM_SPRITE_MAX_FRAMES, pal);
  chk("expand", "an out-of-range frame index is a no-op", memcmp(buf, raw, sizeof buf) == 0);
  rom_sprite_to_rgb15(0, ROM_SPRITE_BUF_BYTES, 0, pal);     /* must not crash */
  rom_sprite_to_rgb15(buf, sizeof buf, 0, 0);
  chk("expand", "NULL arguments are survivable", 1);
}

/* ---- #293: the Ruby/Sapphire PIN path, positive control + mutants ------------- */
static int rs_open_slice(uint8_t* img, uint32_t n, RomSprite* rs) {
  MemCtx mc = { img, n };
  RomCtx rc;
  memset(&rc, 0, sizeof rc);
  rc.read = mem_read; rc.ctx = &mc; rc.size = n;
  /* fill exactly the fields rom_sprite keys on; the mutants below need no rom_open() */
  memcpy(rc.code, img + 0xAC, 4); rc.version = img[0xBC];
  rc.kind = (memcmp(rc.code, "AXVE", 4) == 0) ? ROM_RUBY : ROM_SAPPHIRE;
  return rom_sprite_open(rs, &rc);
}

static void run_rs_pins(const char* path, const char* name, uint32_t front, uint32_t spal) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("SKIP %s pins (no %s)\n", name, path); return; }
  static uint8_t img[0x1000000];   /* the pointers reach 0x08DBxxxx: the whole 16 MiB */
  size_t n = fread(img, 1, sizeof img, f);
  fclose(f);
  if (n != sizeof img) { printf("SKIP %s pins (short read)\n", name); return; }
  RomSprite rs;
  /* positive control: without it every "rejected" below could pass for the wrong reason */
  chk(name, "pin path: the retail image opens (positive control)", rs_open_slice(img, (uint32_t)n, &rs) == 1);

  uint32_t fo = front - ROM_BASE, so = spal - ROM_BASE;
  uint8_t keep;
  keep = img[fo + 100 * 8 + 6]; img[fo + 100 * 8 + 6] ^= 1;
  chk(name, "pin path: front row 100 tag != index -> fails closed", rs_open_slice(img, (uint32_t)n, &rs) == 0);
  img[fo + 100 * 8 + 6] = keep;
  keep = img[so + 439 * 8 + 4]; img[so + 439 * 8 + 4] ^= 1;
  chk(name, "pin path: shiny row 439 tag != 500+index -> fails closed", rs_open_slice(img, (uint32_t)n, &rs) == 0);
  img[so + 439 * 8 + 4] = keep;
  keep = img[fo + 7 * 8 + 3]; img[fo + 7 * 8 + 3] = 0x00;     /* pointer leaves the image */
  chk(name, "pin path: a front pointer outside the image -> fails closed", rs_open_slice(img, (uint32_t)n, &rs) == 0);
  img[fo + 7 * 8 + 3] = keep;
  keep = img[0xBC]; img[0xBC] = (uint8_t)(keep ^ 1);          /* an unpinned revision */
  chk(name, "pin path: an unpinned revision byte -> fails closed", rs_open_slice(img, (uint32_t)n, &rs) == 0);
  img[0xBC] = keep;
  chk(name, "pin path: restored image opens again", rs_open_slice(img, (uint32_t)n, &rs) == 1);
}

/* ---- synthesised images: a GF header whose pointers are not usable ---------- */
static void run_synth(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("SKIP synth (no %s)\n", path); return; }
  static uint8_t img[0x400000];
  size_t n = fread(img, 1, sizeof img, f);
  fclose(f);
  if (n != sizeof img) { printf("SKIP synth (short read)\n"); return; }

  MemCtx mc = { img, (uint32_t)n };
  RomCtx rc;
  memset(&rc, 0, sizeof rc);
  rc.read = mem_read; rc.ctx = &mc; rc.size = mc.n; rc.kind = ROM_EMERALD;

  RomSprite rs;
  /* POSITIVE CONTROL: all four Emerald tables live below 3.2 MiB, so this 4 MiB
   * slice opens. Without this the "rejected" checks below could pass for the
   * wrong reason. */
  chk("synth", "the 4 MiB slice opens (positive control)", rom_sprite_open(&rs, &rc));

  uint8_t save[0x38]; memcpy(save, img + 0x100, sizeof save);
  /* Each of the four table pointers must be checked, not just the first, and the
   * check must require room for all 440 entries — not merely a pointer in range. */
  static const struct { int off; const char* what; } k_ptrs[4] = {
    { 0x28, "front" }, { 0x2C, "back" }, { 0x30, "normal palette" }, { 0x34, "shiny palette" }
  };
  for (int i = 0; i < 4; i++) {
    char w[80];
    uint32_t past = 0x08FFFFF0u;                       /* way past a 4 MiB image */
    memcpy(img + 0x100 + k_ptrs[i].off, &past, 4);
    sprintf(w, "%s table pointer past the end -> fail closed", k_ptrs[i].what);
    chk("synth", w, !rom_sprite_open(&rs, &rc));
    /* and one that IS in range but leaves no room for 440 entries */
    uint32_t tight = 0x08000000u + mc.n - 8;
    memcpy(img + 0x100 + k_ptrs[i].off, &tight, 4);
    sprintf(w, "%s table with room for 1 entry, not 440 -> fail closed", k_ptrs[i].what);
    chk("synth", w, !rom_sprite_open(&rs, &rc));
    memcpy(img + 0x100, save, sizeof save);
  }

  /* now break the header itself, three ways */
  img[0x100] = 0x99;                                   /* version out of range */
  chk("synth", "bad GF version rejected", !rom_sprite_open(&rs, &rc));
  memcpy(img + 0x100, save, sizeof save);
  img[0x108] = 'X';                                    /* gamename not "pokemon " */
  chk("synth", "bad GF game name rejected", !rom_sprite_open(&rs, &rc));
  memcpy(img + 0x100, save, sizeof save);
  memset(img + 0x100, 0, 0x38);                        /* all-zero header */
  chk("synth", "all-zero header rejected", !rom_sprite_open(&rs, &rc));
  memcpy(img + 0x100, save, sizeof save);

  /* a pure-garbage image with no header at all */
  for (uint32_t i = 0; i < 0x2000; i++) img[i] = (uint8_t)(i * 37u + 11u);
  chk("synth", "garbage image rejected", !rom_sprite_open(&rs, &rc));

  /* a NULL/short RomCtx must not crash or claim success */
  chk("synth", "NULL RomCtx rejected", !rom_sprite_open(&rs, 0));
  RomSprite z; memset(&z, 0, sizeof z);
  RomSpritePic pi; uint8_t buf[64];
  chk("synth", "pic on an unopened RomSprite rejected",
      !rom_sprite_pic(&z, ROM_SPRITE_FRONT, 1, 0, buf, sizeof buf, &pi));
  uint16_t pl[16];
  chk("synth", "pal on an unopened RomSprite rejected", !rom_sprite_pal(&z, 1, 0, 0, pl));
}

int main(int argc, char** argv) {
  const char* dir = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms";
  /* run_host_tests.py hands every argv[1]-reading test the .sav corpus — this test
   * wants the ROM DIRECTORY, so only accept an argument that is one. */
  if (argc > 1) { size_t l = strlen(argv[1]); if (l < 4 || strcmp(argv[1] + l - 4, ".sav") != 0) dir = argv[1]; }
  char p[512];
  run_expand_selftest();               /* needs no ROM */
  sprintf(p, "%s/Emerald.gba", dir);   run_rom(p, "Emerald",   1);
  sprintf(p, "%s/FireRed.gba", dir);   run_rom(p, "FireRed",   1);
  sprintf(p, "%s/LeafGreen.gba", dir); run_rom(p, "LeafGreen", 1);
  sprintf(p, "%s/Ruby.gba", dir);      run_rom(p, "Ruby",      1);
  sprintf(p, "%s/Sapphire.gba", dir);  run_rom(p, "Sapphire",  1);
  sprintf(p, "%s/Ruby.gba", dir);      run_rs_pins(p, "Ruby",     0x081E836Cu, 0x081EB38Cu);
  sprintf(p, "%s/Sapphire.gba", dir);  run_rs_pins(p, "Sapphire", 0x081E82FCu, 0x081EB31Cu);
  sprintf(p, "%s/Emerald.gba", dir);   run_synth(p);
  printf("rom_sprite test: %d checks, %d failure(s)\n", checks, fails);
  return fails ? 1 : 0;
}
