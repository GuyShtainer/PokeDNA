/* Clean-room GBC "LZ" decompressor -- see gb_lz.h for the format spec and
 * the hardening rationale. Pure C, no recursion, no tonc/FatFs/GBA headers. */
#include "gb_lz.h"

#define LZ_END 0xFFu

typedef struct {
  GbReadFn read;
  void*    ctx;
  uint32_t base;    /* src_off */
  uint32_t cap;      /* bytes considered, already clamped to GB_LZ_BANK_CAP */
  uint32_t cur;      /* input cursor, relative to base */
} In;

/* Bounded byte reader -- shared by command bytes, offset bytes AND payload
 * bytes (hardening rule 6: no separate, less-bounded path for payloads). */
static int rd_byte(In* in, uint8_t* out) {
  if (in->cur >= in->cap) return 0;
  if (!in->read(in->ctx, in->base + in->cur, out, 1)) return 0;
  in->cur++;
  return 1;
}

static uint8_t bitrev8(uint8_t b) {
  uint8_t r = 0;
  for (int i = 0; i < 8; i++) {
    r = (uint8_t)((r << 1) | (b & 1u));
    b = (uint8_t)(b >> 1);
  }
  return r;
}

uint32_t gb_lz_decode(GbReadFn read, void* ctx, uint32_t src_off, uint32_t src_cap,
                       uint8_t* out, uint32_t out_cap) {
  if (!read || !out || out_cap == 0) return 0;

  In in;
  in.read = read; in.ctx = ctx; in.base = src_off;
  in.cap = (src_cap > GB_LZ_BANK_CAP) ? GB_LZ_BANK_CAP : src_cap;
  in.cur = 0;

  uint32_t out_len = 0;

  /* Bounded outer loop: every branch consumes >= 1 input byte, and input is
   * capped at GB_LZ_BANK_CAP, so this can never iterate more than that. */
  for (uint32_t guard = 0; guard < GB_LZ_BANK_CAP; guard++) {
    uint8_t cmdbyte;
    if (!rd_byte(&in, &cmdbyte)) return 0;         /* missing LZ_END -> fail closed (rule 1) */
    if (cmdbyte == LZ_END) return out_len;         /* raw compare BEFORE masking (rule 5) */

    uint8_t cmd_id = (uint8_t)((cmdbyte >> 5) & 0x7u);
    uint32_t length;

    if (cmd_id == 7u) {
      uint8_t b1;
      if (!rd_byte(&in, &b1)) return 0;
      uint8_t real_cmd = (uint8_t)((cmdbyte >> 2) & 0x7u);
      length = (((uint32_t)(cmdbyte & 0x03u) << 8) | b1) + 1u;
      cmd_id = (real_cmd == 7u) ? 4u : real_cmd;   /* nested LONG -> REPEAT (rule 4) */
    } else {
      length = (uint32_t)(cmdbyte & 0x1Fu) + 1u;
    }

    int is_backref = (cmd_id & 0x4u) != 0u;

    if (!is_backref) {
      if (cmd_id == 0u) {                          /* LITERAL */
        for (uint32_t i = 0; i < length; i++) {
          uint8_t b;
          if (!rd_byte(&in, &b)) return 0;          /* payload via same bounded reader (rule 6) */
          if (out_len >= out_cap) return 0;         /* checked before EVERY byte (rule 2) */
          out[out_len++] = b;
        }
      } else if (cmd_id == 1u) {                    /* ITERATE */
        uint8_t b;
        if (!rd_byte(&in, &b)) return 0;
        for (uint32_t i = 0; i < length; i++) {
          if (out_len >= out_cap) return 0;
          out[out_len++] = b;
        }
      } else if (cmd_id == 2u) {                    /* ALTERNATE */
        uint8_t b0, b1;
        if (!rd_byte(&in, &b0)) return 0;
        if (!rd_byte(&in, &b1)) return 0;
        for (uint32_t i = 0; i < length; i++) {
          if (out_len >= out_cap) return 0;
          out[out_len++] = (i & 1u) ? b1 : b0;
        }
      } else {                                      /* cmd_id == 3: ZERO */
        for (uint32_t i = 0; i < length; i++) {
          if (out_len >= out_cap) return 0;
          out[out_len++] = 0u;
        }
      }
    } else {
      uint8_t ofs0;
      if (!rd_byte(&in, &ofs0)) return 0;
      uint32_t src;
      if (ofs0 & 0x80u) {
        uint32_t dist = (uint32_t)(ofs0 & 0x7Fu) + 1u;   /* "+1" bias is real (rule doc) */
        if (dist > out_len) return 0;
        src = out_len - dist;
      } else {
        uint8_t ofs1;
        if (!rd_byte(&in, &ofs1)) return 0;
        src = ((uint32_t)(ofs0 & 0x7Fu) << 8) | ofs1;    /* BIG-ENDIAN, absolute from blob start */
      }

      if (cmd_id == 4u) {                           /* REPEAT: forward, self-overlapping */
        for (uint32_t i = 0; i < length; i++) {
          if (src >= out_len) return 0;              /* bounds every byte (rule 3) */
          if (out_len >= out_cap) return 0;
          out[out_len++] = out[src++];
        }
      } else if (cmd_id == 5u) {                     /* FLIP: forward + bit-reverse */
        for (uint32_t i = 0; i < length; i++) {
          if (src >= out_len) return 0;
          if (out_len >= out_cap) return 0;
          out[out_len++] = bitrev8(out[src++]);
        }
      } else {                                       /* cmd_id == 6: REVERSE, backward walk */
        for (uint32_t i = 0; i < length; i++) {
          if (out_len >= out_cap) return 0;
          if (src >= out_len) return 0;               /* re-checked every byte: src can run off
                                                         * the front mid-command (rule 3) */
          out[out_len++] = out[src];
          if (src == 0u) src = 0xFFFFFFFFu;            /* force the next check to fail closed */
          else src--;
        }
      }
    }
  }
  return 0;   /* guard exhausted -- unreachable given the cap, but fail closed anyway */
}
